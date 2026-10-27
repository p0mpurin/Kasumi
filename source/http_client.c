#include "http_client.h"
#include "platform.h"

#include <curl/curl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "diagnostic.h"

#ifdef __vita__
#include <mbedtls/base64.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#endif

extern const unsigned char _binary_romfs_cacert_pem_start[];
extern const unsigned char _binary_romfs_cacert_pem_end[];

#ifdef __vita__
/* The Vita's curl is built on OpenSSL 1.1.1, which turned down the bundle
 * handed over whole (CURLOPT_CAINFO_BLOB: "Problem with the SSL CA cert"),
 * so no sign-in could start. The certificates are decoded here one by one,
 * once, and added to each connection's store. */
#define CA_MAX 256
static X509 *g_ca[CA_MAX];
static unsigned g_ca_count, g_ca_failed;

static void load_ca_bundle(void)
{
    const char *p = (const char *)_binary_romfs_cacert_pem_start;
    const char *const end = (const char *)_binary_romfs_cacert_pem_end;
    static const char begin_tag[] = "-----BEGIN CERTIFICATE-----", end_tag[] = "-----END CERTIFICATE-----";
    static unsigned char der[8192];
    unsigned failed = 0;
    while (g_ca_count < CA_MAX) {
        const char *begin = NULL;
        for (const char *q = p; q + sizeof(begin_tag) - 1 <= end; ++q)
            if (!memcmp(q, begin_tag, sizeof(begin_tag) - 1)) {
                begin = q + sizeof(begin_tag) - 1;
                break;
            }
        if (!begin) break;
        const char *stop = NULL;
        for (const char *q = begin; q + sizeof(end_tag) - 1 <= end; ++q)
            if (!memcmp(q, end_tag, sizeof(end_tag) - 1)) {
                stop = q;
                break;
            }
        if (!stop) break;
        p = stop + sizeof(end_tag) - 1;
        /* Base64 without the line breaks. */
        static char text[12288];
        size_t length = 0;
        for (const char *q = begin; q < stop && length < sizeof(text); ++q)
            if (*q != '\n' && *q != '\r' && *q != ' ') text[length++] = *q;
        size_t der_length = 0;
        if (mbedtls_base64_decode(der, sizeof(der), &der_length, (const unsigned char *)text, length) != 0) {
            ++failed;
            continue;
        }
        const unsigned char *cursor = der;
        X509 *cert = d2i_X509(NULL, &cursor, (long)der_length);
        if (cert) g_ca[g_ca_count++] = cert;
        else ++failed;
    }
    ERR_clear_error();
    g_ca_failed = failed;
}

static CURLcode add_ca_bundle(CURL *curl, void *ssl_ctx, void *context)
{
    (void)curl;
    (void)context;
    X509_STORE *store = SSL_CTX_get_cert_store((SSL_CTX *)ssl_ctx);
    unsigned added = 0;
    for (unsigned i = 0; i < g_ca_count; ++i)
        if (X509_STORE_add_cert(store, g_ca[i])) ++added;
    ERR_clear_error();
    /* Here, not at load: the log is not open yet when the bundle loads. */
    static bool logged;
    if (!logged) {
        logged = true;
        diagnostic_log("http", "CA bundle: %u certificates trusted, %u unreadable", added, g_ca_failed);
    }
    return added ? CURLE_OK : CURLE_SSL_CACERT_BADFILE;
}
#endif

void http_use_ca_bundle(void *handle)
{
    CURL *curl = handle;
#ifdef __vita__
    curl_easy_setopt(curl, CURLOPT_SSL_CTX_FUNCTION, add_ca_bundle);
    curl_easy_setopt(curl, CURLOPT_SSL_CTX_DATA, NULL);
#else
    struct curl_blob ca_bundle = {
        .data = (void *)_binary_romfs_cacert_pem_start,
        .len = (size_t)(_binary_romfs_cacert_pem_end - _binary_romfs_cacert_pem_start),
        .flags = CURL_BLOB_NOCOPY,
    };
    curl_easy_setopt(curl, CURLOPT_CAINFO_BLOB, &ca_bundle);
#endif
}
/* The network worker's requests. Keep a bounded connection/TLS cache between calls. */
static CURL *g_http;
static volatile bool g_cancel;
/* Box art has its own thread and its own connection (http_request_art):
 * covers no longer wait behind sign-ins and library loads. */
static CURL *g_art_http;
static volatile bool g_art_cancel;
static long g_next_timeout;
static HttpProgress g_next_progress;
static void *g_next_context;

void http_cancel(void) { g_cancel = true; }

/* All sockets share one IPC session with the soc service, so a call that
 * waits blocks every other thread's socket calls behind it. curl waiting in
 * poll() for a server's answer held the stream's socket reads for ~140 ms:
 * Xbox's keepalive every 30 s was a 150 ms video gap and a main-loop stall
 * each time (report 7XXNYS). During a stream the worker's requests are run
 * step by step instead, never waiting inside a socket call, and reuse the
 * DNS answers and connections of the requests made before the stream. */
static volatile bool g_gentle;
static CURLM *g_gentle_multi;
static CURLSH *g_share;

void http_set_gentle(bool on) { g_gentle = on; }

static CURLcode gentle_perform(CURL *curl)
{
    if (!g_gentle_multi) g_gentle_multi = curl_multi_init();
    if (!g_gentle_multi || curl_multi_add_handle(g_gentle_multi, curl) != CURLM_OK)
        return curl_easy_perform(curl);
    CURLcode result = CURLE_RECV_ERROR;
    int running = 1;
    while (running) {
        if (curl_multi_perform(g_gentle_multi, &running) != CURLM_OK) break;
        if (running) plat_sleep_ns(2000000LL);
    }
    CURLMsg *message;
    int left;
    while ((message = curl_multi_info_read(g_gentle_multi, &left)) != NULL)
        if (message->msg == CURLMSG_DONE && message->easy_handle == curl) result = message->data.result;
    curl_multi_remove_handle(g_gentle_multi, curl);
    return result;
}

void http_next_request(long timeout_seconds, HttpProgress progress, void *context)
{
    g_next_timeout = timeout_seconds;
    g_next_progress = progress;
    g_next_context = context;
}

/* Per request: whose cancel flag, and the progress callback if any. */
typedef struct {
    volatile bool *cancel;
    HttpProgress progress;
    void *context;
} TransferHook;

static bool body_contains(const char *data, size_t size, const char *needle)
{
    const size_t n = strlen(needle);
    if (!data || size < n) return false;
    for (size_t i = 0; i + n <= size; ++i)
        if (data[i] == needle[0] && !memcmp(data + i, needle, n)) return true;
    return false;
}

static int transfer_progress(void *userdata, curl_off_t dl_total, curl_off_t dl_now,
                             curl_off_t ul_total, curl_off_t ul_now)
{
    (void)ul_total; (void)ul_now;
    const TransferHook *hook = userdata;
    if (hook->progress && dl_now > 0)
        hook->progress((unsigned long long)dl_now, dl_total > 0 ? (unsigned long long)dl_total : 0,
                       hook->context);
    return *hook->cancel ? 1 : 0;
}

typedef struct {
    char *data;
    size_t size;
    size_t capacity;
    size_t limit;
} BodyBuffer;

static size_t write_body(char *data, size_t size, size_t count, void *userdata)
{
    BodyBuffer *buffer = userdata;
    if (size != 0 && count > SIZE_MAX / size) return 0;
    const size_t bytes = size * count;
    if (bytes > buffer->limit - buffer->size) return 0;
    const size_t needed = buffer->size + bytes + 1;
    if (needed > buffer->capacity) {
        size_t capacity = buffer->capacity ? buffer->capacity : 4096;
        while (capacity < needed && capacity < buffer->limit + 1) capacity *= 2;
        if (capacity > buffer->limit + 1) capacity = buffer->limit + 1;
        char *grown = realloc(buffer->data, capacity);
        if (!grown) return 0;
        buffer->data = grown;
        buffer->capacity = capacity;
    }
    memcpy(buffer->data + buffer->size, data, bytes);
    buffer->size += bytes;
    buffer->data[buffer->size] = '\0';
    return bytes;
}

bool http_global_init(void)
{
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) return false;
#ifdef __vita__
    load_ca_bundle();
#endif
    return true;
}

void http_art_cancel(void) { g_art_cancel = true; }

void http_global_exit(void)
{
    if (g_http) curl_easy_cleanup(g_http);
    g_http = NULL;
    if (g_art_http) curl_easy_cleanup(g_art_http);
    g_art_http = NULL;
    curl_global_cleanup();
}

void http_response_free(HttpResponse *response)
{
    if (!response) return;
    free(response->body);
    memset(response, 0, sizeof(*response));
}

static bool perform(CURL **handle, volatile bool *cancel, long timeout, HttpProgress progress, void *context,
                    const char *method, const char *url, const char *user_agent,
                    const char *const *headers, size_t header_count,
                    const char *body, size_t max_response, HttpResponse *response);

bool http_request(const char *method, const char *url, const char *user_agent,
                  const char *const *headers, size_t header_count,
                  const char *body, size_t max_response, HttpResponse *response)
{
    const long timeout = g_next_timeout;
    const HttpProgress progress = g_next_progress;
    void *const context = g_next_context;
    g_next_timeout = 0;
    g_next_progress = NULL;
    g_next_context = NULL;
    return perform(&g_http, &g_cancel, timeout, progress, context, method, url, user_agent, headers, header_count,
                   body, max_response, response);
}

bool http_request_art(const char *url, const char *user_agent, const char *const *headers, size_t header_count,
                      size_t max_response, HttpResponse *response)
{
    return perform(&g_art_http, &g_art_cancel, 20, NULL, NULL, "GET", url, user_agent, headers, header_count,
                   NULL, max_response, response);
}

static bool perform(CURL **handle, volatile bool *cancel, long timeout, HttpProgress progress, void *context,
                    const char *method, const char *url, const char *user_agent,
                    const char *const *headers, size_t header_count,
                    const char *body, size_t max_response, HttpResponse *response)
{
    memset(response, 0, sizeof(*response));
    if (!*handle) *handle = curl_easy_init();
    CURL *curl = *handle;
    if (!curl) {
        snprintf(response->error, sizeof(response->error), "curl init failed");
        return false;
    }
    curl_easy_reset(curl);

    struct curl_slist *header_list = NULL;
    for (size_t i = 0; i < header_count; ++i) {
        struct curl_slist *next = curl_slist_append(header_list, headers[i]);
        if (!next) {
            curl_slist_free_all(header_list);
            snprintf(response->error, sizeof(response->error), "Not enough memory for HTTP headers");
            return false;
        }
        header_list = next;
    }

    BodyBuffer buffer = {.limit = max_response};
    *cancel = false;
    TransferHook hook = { cancel, progress, context };
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, transfer_progress);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &hook);
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, user_agent);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout > 0 ? timeout : 25L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXCONNECTS, 2L);
    /* The worker's handle only (one thread, so no share locks needed). */
    if (handle == &g_http) {
        if (!g_share) {
            g_share = curl_share_init();
            if (g_share) {
                curl_share_setopt(g_share, CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS);
                curl_share_setopt(g_share, CURLSHOPT_SHARE, CURL_LOCK_DATA_CONNECT);
                curl_share_setopt(g_share, CURLSHOPT_SHARE, CURL_LOCK_DATA_SSL_SESSION);
            }
        }
        if (g_share) curl_easy_setopt(curl, CURLOPT_SHARE, g_share);
        /* A lookup blocks too: keep answers for the length of a session. */
        curl_easy_setopt(curl, CURLOPT_DNS_CACHE_TIMEOUT, 600L);
    }
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, (long)CURL_HTTP_VERSION_1_1);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    http_use_ca_bundle(curl);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_body);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buffer);
    if (header_list) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_list);
    if (strcmp(method, "POST") == 0) {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body ? body : "");
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)(body ? strlen(body) : 0));
    } else if (strcmp(method, "GET") != 0) {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);
        if (body) {
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(body));
        }
    }

    const CURLcode result = g_gentle && handle == &g_http ? gentle_perform(curl) : curl_easy_perform(curl);
    response->curl_code = (int)result;
    {
        curl_off_t value = 0;
        if (curl_easy_getinfo(curl, CURLINFO_NAMELOOKUP_TIME_T, &value) == CURLE_OK) response->dns_ms = (unsigned)(value / 1000);
        if (curl_easy_getinfo(curl, CURLINFO_CONNECT_TIME_T, &value) == CURLE_OK) response->connect_ms = (unsigned)(value / 1000);
        if (curl_easy_getinfo(curl, CURLINFO_APPCONNECT_TIME_T, &value) == CURLE_OK) response->tls_ms = (unsigned)(value / 1000);
        if (curl_easy_getinfo(curl, CURLINFO_TOTAL_TIME_T, &value) == CURLE_OK) response->total_ms = (unsigned)(value / 1000);
        if (curl_easy_getinfo(curl, CURLINFO_SIZE_UPLOAD_T, &value) == CURLE_OK) response->uploaded = (unsigned long long)value;
    }
    /* Only the host is logged: paths and queries can hold session IDs. */
    char host[64] = "";
    {
        const char *start = strstr(url, "://");
        start = start ? start + 3 : url;
        size_t length = strcspn(start, "/?:");
        if (length >= sizeof(host)) length = sizeof(host) - 1;
        memcpy(host, start, length);
        host[length] = '\0';
    }
    if (result == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response->status);
        response->body = buffer.data;
        response->size = buffer.size;
        /* A server falling over is NVIDIA's problem, but a new one we have
         * not seen means some call is malformed or a route moved. */
        /* Except NVIDIA's limited mode, a 500 that gfn_client already
         * waits out (55 of beta.34's http-5xx flags, each a report). */
        if (response->status >= 500 && !body_contains(buffer.data, buffer.size, "LIMITED_MODE"))
            diagnostic_flag("http-5xx", "%s %s -> %ld (%lu bytes)", method, host, response->status,
                            (unsigned long)buffer.size);
    } else {
        /* Certificate and TLS failures are ours to fix (an expired CA
         * bundle, a server changing its chain); timeouts and DNS are Wi-Fi. */
        if (result == CURLE_SSL_CONNECT_ERROR || result == CURLE_PEER_FAILED_VERIFICATION ||
            result == CURLE_SSL_CACERT_BADFILE || result == CURLE_SSL_CERTPROBLEM ||
            result == CURLE_SSL_CIPHER || result == CURLE_SSL_ISSUER_ERROR)
            diagnostic_flag("tls-error", "%s %s: %s", method, host, curl_easy_strerror(result));
        snprintf(response->error, sizeof(response->error), "%s",
                 result == CURLE_ABORTED_BY_CALLBACK ? "Cancelled" : curl_easy_strerror(result));
        /* How much arrived before it stopped (the connection check uses it
         * when a server ignores Range and the size cap cuts the body). */
        response->size = buffer.size;
        free(buffer.data);
    }
    curl_easy_reset(curl);
    curl_slist_free_all(header_list);
    return result == CURLE_OK;
}

char *http_url_encode(const char *value)
{
    CURL *curl = curl_easy_init();
    if (!curl) return NULL;
    char *escaped = curl_easy_escape(curl, value, 0);
    char *copy = escaped ? strdup(escaped) : NULL;
    curl_free(escaped);
    curl_easy_cleanup(curl);
    return copy;
}

bool http_clock_wrong(char *date, size_t size)
{
    /* 2026-09-01 and ten years on: no certificate checks out outside that. */
    const time_t now = time(NULL), earliest = 1788220800, latest = earliest + 10 * 365 * 24 * 3600;
    if (date && size) {
        const struct tm *t = gmtime(&now);
        if (!t || !strftime(date, size, "%Y-%m-%d", t)) snprintf(date, size, "?");
    }
    return now < earliest || now > latest;
}

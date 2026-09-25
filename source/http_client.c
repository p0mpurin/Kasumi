#include "http_client.h"

#include <curl/curl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern const unsigned char _binary_romfs_cacert_pem_start[];
extern const unsigned char _binary_romfs_cacert_pem_end[];
/* Main-thread requests only. Keep a bounded connection/TLS cache between calls. */
static CURL *g_http;
static volatile bool g_cancel;

void http_cancel(void) { g_cancel = true; }

static int transfer_progress(void *userdata, curl_off_t dl_total, curl_off_t dl_now,
                             curl_off_t ul_total, curl_off_t ul_now)
{
    (void)userdata; (void)dl_total; (void)dl_now; (void)ul_total; (void)ul_now;
    return g_cancel ? 1 : 0;
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
    return curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK;
}

void http_global_exit(void)
{
    if (g_http) curl_easy_cleanup(g_http);
    g_http = NULL;
    curl_global_cleanup();
}

void http_response_free(HttpResponse *response)
{
    if (!response) return;
    free(response->body);
    memset(response, 0, sizeof(*response));
}

bool http_request(const char *method, const char *url, const char *user_agent,
                  const char *const *headers, size_t header_count,
                  const char *body, size_t max_response, HttpResponse *response)
{
    memset(response, 0, sizeof(*response));
    if (!g_http) g_http = curl_easy_init();
    CURL *curl = g_http;
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
    g_cancel = false;
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, transfer_progress);
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, user_agent);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 25L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXCONNECTS, 2L);
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, (long)CURL_HTTP_VERSION_1_1);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    struct curl_blob ca_bundle = {
        .data = (void *)_binary_romfs_cacert_pem_start,
        .len = (size_t)(_binary_romfs_cacert_pem_end - _binary_romfs_cacert_pem_start),
        .flags = CURL_BLOB_NOCOPY,
    };
    curl_easy_setopt(curl, CURLOPT_CAINFO_BLOB, &ca_bundle);
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

    const CURLcode result = curl_easy_perform(curl);
    if (result == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response->status);
        response->body = buffer.data;
        response->size = buffer.size;
    } else {
        snprintf(response->error, sizeof(response->error), "%s",
                 result == CURLE_ABORTED_BY_CALLBACK ? "Cancelled" : curl_easy_strerror(result));
        /* How much arrived before it stopped (the connection check uses it
         * when a server ignores Range and the size cap cuts the body). */
        response->size = buffer.size;
        free(buffer.data);
    }
    /* Drop borrowed request pointers before their owners release them. */
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

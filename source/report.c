#include "report.h"

#include <3ds.h>
#include <dirent.h>
#include <jansson.h>
#include <mbedtls/base64.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <zlib.h>

#include "app_paths.h"
#include "diagnostic.h"
#include "file_worker.h"
#include "http_client.h"
#include "launch_stats.h"

/* Each log is capped (its end is kept): 6000 lines is well under this. */
#define LOG_CAP (768 * 1024)
#define DUMP_DIR "sdmc:/luma/dumps/arm11"
#define DUMP_MAX_AGE (3 * 24 * 3600)
#define DUMP_CAP (64 * 1024)
/* The installed CIA's title (APP_UNIQUE_ID 0x4B534). */
#define KASUMI_TITLE_ID 0x0004000004B53400ULL

static char g_code[16];
static char g_error[96];

const char *report_code(void) { return g_code; }
const char *report_error(void) { return g_error; }

static char *read_file(const char *path, size_t cap, size_t *length);

bool report_available(void) { return strstr(REPORT_BASE, "CHANGE-ME") == NULL; }

bool report_stats_pending(void)
{
    struct stat st;
    return (stat(REPORT_STATS_PENDING_PATH, &st) == 0 && st.st_size > 0) ||
           (stat(LAUNCH_PENDING_PATH, &st) == 0 && st.st_size > 2);
}

/* Launch records (launch_stats.h), as {"app":"Kasumi","launches":[...]}. */
static bool send_launches(void)
{
    size_t length = 0;
    file_worker_flush();
    char *list = read_file(LAUNCH_PENDING_PATH, 12 * 1024, &length);
    if (!list) return false;
    char *body = malloc(length + 40);
    if (!body) {
        free(list);
        return false;
    }
    snprintf(body, length + 40, "{\"app\":\"Kasumi\",\"launches\":%s}", list);
    free(list);
    static const char *const headers[] = { "Content-Type: application/json" };
    HttpResponse response;
    http_next_request(5, NULL, NULL);
    const bool sent = http_request("POST", LAUNCHES_URL, "Kasumi-3DS", headers, 1, body, 4096, &response);
    const long status = response.status;
    http_response_free(&response);
    free(body);
    /* A service without /launches yet answers 404: keep them (capped). */
    if (sent && (status == 200 || status == 400 || status == 413)) remove(LAUNCH_PENDING_PATH);
    diagnostic_log("REPORT", "launch records sent=%d http=%ld", sent, sent ? status : 0);
    return sent && status == 200;
}

bool report_send_stats(void)
{
    struct stat st;
    if (stat(LAUNCH_PENDING_PATH, &st) == 0 && st.st_size > 2) send_launches();
    size_t length = 0;
    file_worker_flush();
    char *summary = read_file(REPORT_STATS_PENDING_PATH, 4096, &length);
    if (!summary) return false;
    static const char *const headers[] = { "Content-Type: application/json" };
    HttpResponse response;
    http_next_request(5, NULL, NULL); /* ~1 KB: never worth a long wait */
    const bool sent = http_request("POST", STATS_URL, "Kasumi-3DS", headers, 1, summary, 4096, &response);
    const long status = response.status;
    http_response_free(&response);
    free(summary);
    /* Delivered, or refused as malformed: done. Anything else (no
     * connection, rate limit, a service without /stats yet) keeps it for a
     * later try; a newer session's summary replaces it anyway. */
    if (sent && (status == 200 || status == 400 || status == 413)) remove(REPORT_STATS_PENDING_PATH);
    diagnostic_log("REPORT", "session summary sent=%d http=%ld", sent, sent ? status : 0);
    return sent && status == 200;
}

/* The whole file, or its last `cap` bytes. NULL if missing. */
static char *read_file(const char *path, size_t cap, size_t *length)
{
    *length = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    if (size <= 0) {
        fclose(f);
        return NULL;
    }
    size_t want = (size_t)size;
    if (want > cap) {
        fseek(f, size - (long)cap, SEEK_SET);
        want = cap;
    } else {
        fseek(f, 0, SEEK_SET);
    }
    char *data = malloc(want + 1);
    if (data) {
        *length = fread(data, 1, want, f);
        data[*length] = '\0';
    }
    fclose(f);
    return data;
}

static bool private_address(int a, int b)
{
    return a == 0 || a == 10 || a == 127 || (a == 192 && b == 168) ||
           (a == 172 && b >= 16 && b <= 31) || (a == 169 && b == 254) || (a == 100 && b >= 64 && b <= 127);
}

static bool is_digit(char c) { return c >= '0' && c <= '9'; }

/* Public IPv4 addresses keep only their first two numbers ("151.62.x.x"):
 * enough to tell networks apart, not to identify anyone. Never longer than
 * the input. */
static char *scrub(const char *in, size_t length)
{
    char *out = malloc(length + 1);
    if (!out) return NULL;
    size_t o = 0, i = 0;
    while (i < length) {
        if (is_digit(in[i]) && (i == 0 || (!is_digit(in[i - 1]) && in[i - 1] != '.'))) {
            int parts[4];
            size_t j = i;
            int k = 0;
            for (; k < 4; ++k) {
                int value = 0, digits = 0;
                while (j < length && is_digit(in[j]) && digits < 4) value = value * 10 + (in[j++] - '0'), ++digits;
                if (!digits || digits > 3 || value > 255) break;
                parts[k] = value;
                if (k < 3) {
                    if (j >= length || in[j] != '.') break;
                    ++j;
                }
            }
            const bool ends = j >= length || (!is_digit(in[j]) && !(in[j] == '.' && j + 1 < length && is_digit(in[j + 1])));
            if (k == 4 && ends && !private_address(parts[0], parts[1])) {
                o += (size_t)sprintf(out + o, "%d.%d.x.x", parts[0], parts[1]);
                i = j;
                continue;
            }
        }
        out[o++] = in[i++];
    }
    out[o] = '\0';
    return out;
}

static json_t *scrubbed_file(const char *path, size_t cap)
{
    if (!cap) return json_string("");
    size_t length = 0;
    char *raw = read_file(path, cap, &length);
    if (!raw) return json_string("");
    char *clean = scrub(raw, length);
    free(raw);
    json_t *value = json_stringn(clean ? clean : "", clean ? strlen(clean) : 0);
    free(clean);
    return value ? value : json_string("");
}

static char *base64(const unsigned char *data, size_t length)
{
    size_t needed = 0;
    mbedtls_base64_encode(NULL, 0, &needed, data, length);
    char *out = malloc(needed + 1);
    if (!out) return NULL;
    size_t written = 0;
    if (mbedtls_base64_encode((unsigned char *)out, needed + 1, &written, data, length) != 0) {
        free(out);
        return NULL;
    }
    out[written] = '\0';
    return out;
}

/* The newest Luma ARM11 crash dump from the last few days, if any. */
/* Whether a Luma ARM11 dump is of Kasumi: its extra data holds the process
 * name (8 bytes) and title ID. Dumps of other apps are no use here (report
 * TYQJP7 carried a crash of a GTA port from the same console). */
static bool dump_is_kasumi(const unsigned char *data, size_t length)
{
    uint32_t header[10];
    if (length < sizeof(header)) return false;
    memcpy(header, data, sizeof(header));
    if (header[0] != 0xDEADC0DEu || header[1] != 0xDEADCAFEu) return false;
    if ((header[3] & 0xFFFF) != 11) return false; /* ARM11 */
    const size_t extra = 40 + (size_t)header[6] + header[7] + header[8];
    if ((size_t)header[9] < 16 || extra + 16 > length) return false;
    uint64_t title = 0;
    memcpy(&title, data + extra + 8, sizeof(title));
    return !memcmp(data + extra, APP_NAME, strlen(APP_NAME)) || title == KASUMI_TITLE_ID;
}

/* The newest Luma ARM11 crash dump of Kasumi from the last few days. */
static json_t *recent_dump(void)
{
    DIR *dir = opendir(DUMP_DIR);
    if (!dir) return NULL;
    char best[64] = "";
    time_t best_time = 0;
    const time_t now = time(NULL);
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        /* Luma names them crash_dump_00000033.dmp; skip anything else. */
        if (strncmp(entry->d_name, "crash_dump_", 11) || strlen(entry->d_name) >= sizeof(best)) continue;
        char path[160];
        snprintf(path, sizeof(path), "%s/%.63s", DUMP_DIR, entry->d_name);
        struct stat st;
        if (stat(path, &st) || st.st_size <= 0 || st.st_size > DUMP_CAP) continue;
        if (st.st_mtime && now - st.st_mtime > DUMP_MAX_AGE) continue;
        if (best[0] && (st.st_mtime < best_time || (st.st_mtime == best_time && strcmp(entry->d_name, best) < 0)))
            continue;
        size_t length = 0;
        char *data = read_file(path, DUMP_CAP, &length);
        const bool ours = data && dump_is_kasumi((const unsigned char *)data, length);
        free(data);
        if (!ours) continue;
        snprintf(best, sizeof(best), "%.63s", entry->d_name);
        best_time = st.st_mtime;
    }
    closedir(dir);
    if (!best[0]) return NULL;
    char path[160];
    snprintf(path, sizeof(path), "%s/%s", DUMP_DIR, best);
    size_t length = 0;
    char *data = read_file(path, DUMP_CAP, &length);
    if (!data) return NULL;
    char *encoded = base64((const unsigned char *)data, length);
    free(data);
    if (!encoded) return NULL;
    json_t *dump = json_pack("{s:s,s:s}", "name", best, "data", encoded);
    free(encoded);
    return dump;
}

/* gzip, so the service can unpack it with standard tools. */
static unsigned char *gzip(const char *data, size_t length, size_t *out_length)
{
    z_stream z;
    memset(&z, 0, sizeof(z));
    if (deflateInit2(&z, 6, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK) return NULL;
    const uLong bound = deflateBound(&z, (uLong)length);
    unsigned char *out = malloc(bound);
    if (!out) {
        deflateEnd(&z);
        return NULL;
    }
    z.next_in = (Bytef *)data;
    z.avail_in = (uInt)length;
    z.next_out = out;
    z.avail_out = (uInt)bound;
    const int result = deflate(&z, Z_FINISH);
    *out_length = z.total_out;
    deflateEnd(&z);
    if (result != Z_STREAM_END) {
        free(out);
        return NULL;
    }
    return out;
}

static bool fail(const char *message)
{
    snprintf(g_error, sizeof(g_error), "%s", message);
    diagnostic_log("REPORT", "not sent: %s", message);
    return false;
}

bool report_previous_run_unclean(void)
{
    size_t length = 0;
    char *head = read_file(DIAGNOSTIC_PREVIOUS_PATH, 1u << 30, &length);
    if (!head) return false;
    /* Only a run of this very version: older ones never wrote the marker. */
    const bool same_version = strstr(head, APP_NAME " " APP_VERSION " (build " APP_BUILD ")") == head;
    const char *tail = length > 4096 ? head + length - 4096 : head;
    const bool clean = strstr(tail, REPORT_CLEAN_EXIT) != NULL;
    /* A few lines only: it barely started (e.g. exited from a fatal screen). */
    unsigned lines = 0;
    for (size_t i = 0; i < length && lines < 8; ++i) lines += head[i] == '\n';
    free(head);
    return same_version && !clean && lines >= 8;
}

/* How much a report carries, tried in turn. A full report is a few hundred
 * KB packed; on weak Wi-Fi it could time out or run out of memory, and the
 * player saw "Sending" and then nothing (beta.28). A lighter report keeps
 * the end of this run's log, which matters most. */
typedef struct {
    size_t log, previous, older;
    bool dump;
    long timeout;
    const char *tag;
} ReportSize;

static const ReportSize REPORT_SIZES[] = {
    { LOG_CAP, LOG_CAP, LOG_CAP, true, 45, "" },
    { 256 * 1024, 128 * 1024, 0, false, 45, " (smaller)" },
    { 96 * 1024, 0, 0, false, 30, " (smallest)" },
};

/* The upload body, or NULL (with the reason in g_error). */
static char *report_body(const char *trigger, const ReportSize *size, size_t *raw_length, size_t *packed_length)
{
    char sent_at[32], why[96];
    const time_t now = time(NULL);
    strftime(sent_at, sizeof(sent_at), "%Y-%m-%dT%H:%M:%S", gmtime(&now));
    snprintf(why, sizeof(why), "%s%s", trigger, size->tag);
    json_t *root = json_pack("{s:s,s:s,s:s,s:s,s:s}", "app", APP_NAME, "version", APP_VERSION,
                             "build", APP_BUILD, "sent_at", sent_at, "trigger", why);
    if (!root) {
        fail("Out of memory");
        return NULL;
    }
    /* This run's flags (see diagnostic_flag): the service lists them first. */
    char flags[512];
    diagnostic_flags_summary(flags, sizeof(flags));
    json_object_set_new(root, "flags", json_string(flags));
    json_object_set_new(root, "log", scrubbed_file(DIAGNOSTIC_PATH, size->log));
    json_object_set_new(root, "log_older", scrubbed_file(DIAGNOSTIC_OLDER_PATH, size->older));
    json_object_set_new(root, "previous_log", scrubbed_file(DIAGNOSTIC_PREVIOUS_PATH, size->previous));
    json_t *settings = scrubbed_file(APP_DATA_DIR "/settings.json", LOG_CAP);
    json_error_t parse_error;
    json_t *parsed = json_loads(json_string_value(settings), 0, &parse_error);
    const char *install = json_is_object(parsed) ? json_string_value(json_object_get(parsed, "install_id")) : NULL;
    json_object_set_new(root, "install", json_string(install ? install : ""));
    json_decref(parsed);
    json_object_set_new(root, "settings", settings);
    if (size->dump) {
        json_t *dump = recent_dump();
        if (dump) json_object_set_new(root, "dump", dump);
    }
    char *report = json_dumps(root, JSON_COMPACT);
    json_decref(root);
    if (!report) {
        fail("Out of memory");
        return NULL;
    }
    *raw_length = strlen(report);
    unsigned char *packed = gzip(report, *raw_length, packed_length);
    free(report);
    if (!packed) {
        fail("Could not compress the report");
        return NULL;
    }
    char *payload = base64(packed, *packed_length);
    free(packed);
    if (!payload) {
        fail("Out of memory");
        return NULL;
    }
    json_t *upload = json_pack("{s:s,s:s}", "format", "kasumi-report-1", "payload", payload);
    free(payload);
    char *body = upload ? json_dumps(upload, JSON_COMPACT) : NULL;
    json_decref(upload);
    if (!body) fail("Out of memory");
    return body;
}

bool report_send(const char *trigger)
{
    g_code[0] = g_error[0] = 0;
    if (!trigger || !trigger[0]) trigger = "manual";
    if (!report_available()) return fail("Reports are not set up in this build");
    diagnostic_log("REPORT", "sending (%s)", trigger);
    diagnostic_checkpoint();

    static const char *const headers[] = { "Content-Type: application/json", "Accept: application/json" };
    const unsigned attempts = sizeof(REPORT_SIZES) / sizeof(REPORT_SIZES[0]);
    for (unsigned attempt = 0; attempt < attempts; ++attempt) {
        const ReportSize *size = &REPORT_SIZES[attempt];
        size_t raw_length = 0, packed_length = 0;
        char *body = report_body(trigger, size, &raw_length, &packed_length);
        if (!body) continue; /* out of memory at this size: a lighter one may fit */
        const size_t body_length = strlen(body);
        HttpResponse response;
        http_next_request(size->timeout, NULL, NULL);
        const bool sent = http_request("POST", REPORT_URL, "Kasumi-3DS", headers, 2, body, 16 * 1024, &response);
        free(body);
        /* Where it broke: no DNS, no connection, TLS, or a slow upload. */
        diagnostic_log("REPORT", "attempt %u%s: %lu bytes, %lu packed, %lu to send; http=%ld curl=%d "
                       "dns=%u connect=%u tls=%u total=%u ms, %llu sent%s%.80s",
                       attempt + 1, size->tag, (unsigned long)raw_length, (unsigned long)packed_length,
                       (unsigned long)body_length, response.status, response.curl_code, response.dns_ms,
                       response.connect_ms, response.tls_ms, response.total_ms, response.uploaded,
                       sent ? "" : "; ", sent ? "" : response.error);
        if (!sent) {
            const bool cancelled = !strcmp(response.error, "Cancelled");
            char message[96];
            snprintf(message, sizeof(message), "No connection: %.70s", response.error);
            http_response_free(&response);
            if (cancelled) return fail("Cancelled");
            fail(message);
            continue;
        }
        const long status = response.status;
        json_error_t error;
        json_t *reply = json_loadb(response.body ? response.body : "", response.size, 0, &error);
        http_response_free(&response);
        const char *code = json_is_object(reply) ? json_string_value(json_object_get(reply, "code")) : NULL;
        if (status == 200 && code && strlen(code) == 6) {
            snprintf(g_code, sizeof(g_code), "%.3s-%.3s", code, code + 3);
            json_decref(reply);
            diagnostic_log("REPORT", "sent code=%s bytes=%lu packed=%lu", g_code,
                           (unsigned long)raw_length, (unsigned long)packed_length);
            return true;
        }
        json_decref(reply);
        if (status == 429) return fail("Too many reports from this network: try again in an hour");
        char message[64];
        snprintf(message, sizeof(message), "The report service answered HTTP %ld", status);
        fail(message);
        /* Too large or a server hiccup: lighter may go through. Anything
         * else would be refused again. */
        if (status != 413 && status < 500) return false;
    }
    return false;
}

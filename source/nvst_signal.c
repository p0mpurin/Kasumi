#include "nvst_signal.h"
#include "diagnostic.h"
#include "stream_profile.h"
#include <3ds.h>
#include <curl/curl.h>
#include <jansson.h>
#include <mbedtls/base64.h>
#include <mbedtls/sha1.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SIGNAL_USER_AGENT "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36"
extern const unsigned char _binary_romfs_cacert_pem_start[];
extern const unsigned char _binary_romfs_cacert_pem_end[];
typedef struct {
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context drbg;
} SignalRandom;

/* Only fixed event labels and numeric counters enter this bounded log. */
static void record(NvstSignal *s, const char *event)
{
    snprintf(s->last_event, sizeof(s->last_event), "%s", event);
    if (s->diagnostic_lines++ >= 64) return;
    diagnostic_log("NVST", "event=%s elapsed=%llu frames=%u json=%u ack=%u hb=%u peer=%u offer=%lu ice=%u empty=%u close=%u",
        event, (unsigned long long)(osGetTime() - s->started_ms),
        s->parser.frames, s->messages, s->acknowledgements, s->heartbeats,
        s->peer_assigned ? 1 : 0, (unsigned long)s->offer_size, s->remote_ice_count,
        s->empty_polls, s->close_code);
}

/* Reconnects to the same session sign in with the same peer name, so the
 * server can hand the stream straight back instead of waiting out the old
 * connection (beta.9: a fresh name got no offer for 30 s). If that reused
 * name gets nowhere, the next try uses a new one. */
static char g_last_session[160], g_last_peer[32];
static bool g_reuse_failed;

static bool fail(NvstSignal *s, const char *message)
{
    if (s->reconnect && !s->offer_size && !g_reuse_failed && !strcmp(s->peer_name, g_last_peer))
        g_reuse_failed = true;
    s->state = NVST_SIGNAL_ERROR;
    snprintf(s->status, sizeof(s->status), "%s", message);
    record(s, "failure");
    return false;
}

static void transport_error(NvstSignal *s, const char *stage, CURLcode code)
{
    int e = errno;
    long os_error = 0;
    curl_easy_getinfo((CURL *)s->curl, CURLINFO_OS_ERRNO, &os_error);
    s->state = NVST_SIGNAL_ERROR;
    snprintf(s->status, sizeof(s->status),
        "%s curl=%d os=%ld errno=%d after %us; rx=%u offer=%lu. %.45s",
        stage, (int)code, os_error, e, (unsigned)((osGetTime()-s->started_ms)/1000),
        s->messages, (unsigned long)s->offer_size, curl_easy_strerror(code));
    record(s, "transport-error");
    diagnostic_log("NVST", "transport stage=%s curl=%d os=%ld errno=%d text=%s",
                   stage, (int)code, os_error, e, curl_easy_strerror(code));
}

/* errno is cleared immediately before each operation. Never reuse a stale
 * EAGAIN to hide an unrelated TLS error. This workaround remains specific
 * to the observed 3DS port behavior; resets and EOF are terminal. */
static bool would_block(CURLcode code)
{
    return code == CURLE_AGAIN ||
        ((code == CURLE_RECV_ERROR || code == CURLE_SEND_ERROR) &&
         (errno == EAGAIN || errno == EWOULDBLOCK));
}

static bool send_raw(NvstSignal *s, const void *data, size_t length)
{
    size_t offset = 0;
    uint64_t deadline = osGetTime() + 3000;
    while (offset < length) {
        size_t sent = 0;
        errno = 0; s->curl_error[0] = 0;
        CURLcode code = curl_easy_send((CURL *)s->curl,
            (const char *)data + offset, length - offset, &sent);
        if (would_block(code)) {
            if (osGetTime() >= deadline) return fail(s, "NVST send timeout (3s)");
            svcSleepThread(1000000); continue;
        }
        if (code != CURLE_OK) { transport_error(s, "Send", code); return false; }
        if (!sent) return fail(s, "NVST send stalled");
        offset += sent;
    }
    return true;
}

static bool random_bytes(NvstSignal *s, unsigned char *out, size_t size)
{
    return s->random && mbedtls_ctr_drbg_random(&((SignalRandom *)s->random)->drbg, out, size) == 0;
}

static bool send_frame(NvstSignal *s, unsigned opcode, const void *data, size_t size)
{
    unsigned char mask[4];
    if (size > WS_MESSAGE_LIMIT || !random_bytes(s, mask, 4))
        return fail(s, "NVST frame size/random failure");
    uint8_t *frame = malloc(size + 14);
    if (!frame) return fail(s, "NVST send allocation failed");
    size_t n = ws_encode_frame(frame, size + 14, opcode, data, size, mask);
    bool ok = n && send_raw(s, frame, n);
    free(frame);
    return ok;
}

static bool send_json(NvstSignal *s, json_t *value)
{
    if (!value) return fail(s, "NVST JSON allocation failed");
    char *text = json_dumps(value, JSON_COMPACT);
    if (!text) return fail(s, "NVST JSON encoding failed");
    bool ok = send_frame(s, 1, text, strlen(text));
    free(text);
    return ok;
}

static bool peer_info(NvstSignal *s)
{
    /* Vita uses role 1 here; Switch uses 0. Preserve Vita's complete
     * registration shape rather than guessing which field caused a reset. */
    char resolution[32];
    snprintf(resolution, sizeof(resolution), "%ux%u",
             stream_profile_width(), stream_profile_height());
    json_t *hello = json_pack("{s:i,s:{s:s,s:s,s:b,s:i,s:s,s:i,s:s,s:i}}",
        "ackid", (int)++s->ack_counter, "peer_info",
        "browser", "Chrome", "browserVersion", "131", "connected", 1,
        "id", (int)s->local_peer_id, "name", s->peer_name,
        "peerRole", 1, "resolution", resolution, "version", 2);
    bool ok = send_json(s, hello); json_decref(hello);
    s->last_peer_info_ms = osGetTime();
    if (ok) record(s, "peer-info-sent");
    return ok;
}

static bool heartbeat(NvstSignal *s)
{
    const char text[] = "{\"hb\":1}";
    s->last_heartbeat_ms = osGetTime();
    return send_frame(s, 1, text, sizeof(text)-1);
}

static bool incoming(void *context, unsigned opcode, const uint8_t *data, size_t size)
{
    NvstSignal *s = context;
    if (opcode == 9) return send_frame(s, 10, data, size);
    if (opcode == 10) return true;
    if (opcode == 8) {
        s->close_code = size >= 2 ? (uint16_t)((data[0] << 8) | data[1]) : 1005;
        /* Echo the close before relinquishing the transport. */
        send_frame(s, 8, data, size);
        s->state = NVST_SIGNAL_CLOSED;
        snprintf(s->status, sizeof(s->status), "NVST close %u; messages=%u offer=%lu bytes",
            s->close_code, s->messages, (unsigned long)s->offer_size);
        record(s, "websocket-close"); return true;
    }
    if (opcode != 1) return fail(s, "Unexpected binary NVST signaling");
    json_error_t error;
    json_t *root = json_loadb((const char *)data, size, JSON_REJECT_DUPLICATES, &error);
    if (!json_is_object(root)) { json_decref(root); return fail(s, "NVST invalid JSON object"); }
    ++s->messages;
    json_t *info = json_object_get(root, "peer_info");
    json_t *id = json_object_get(info, "id");
    const char *name = json_string_value(json_object_get(info, "name"));
    if (name && !strcmp(name, s->peer_name) && json_is_integer(id)) {
        s->local_peer_id = (uint32_t)json_integer_value(id);
        s->peer_assigned = true; record(s, "peer-assigned");
    }
    json_t *ackid = json_object_get(root, "ackid");
    if (json_is_integer(ackid) && !(json_is_integer(id) &&
        json_integer_value(id) == s->local_peer_id)) {
        json_t *ack = json_pack("{s:I}", "ack", json_integer_value(ackid));
        bool ok = send_json(s, ack); json_decref(ack);
        if (!ok) { json_decref(root); return false; }
    }
    if (json_is_integer(json_object_get(root, "ack"))) {
        ++s->acknowledgements; record(s, "ack-received");
    }
    if (json_object_get(root, "hb")) {
        ++s->heartbeats;
        if (!heartbeat(s)) { json_decref(root); return false; }
        if (s->heartbeats <= 2) record(s, "heartbeat-received");
    }
    if (json_object_get(root, "error")) {
        const char *why = json_string_value(json_object_get(root, "error"));
        fail(s, why && !strcmp(why, "peerRemoved") ?
            "NVIDIA removed signaling peer" : "NVIDIA signaling error (not transport)");
        json_decref(root); return false;
    }
    json_t *peer = json_object_get(root, "peer_msg");
    json_t *from = json_object_get(peer, "from");
    if (json_is_integer(from)) s->remote_peer_id = (uint32_t)json_integer_value(from);
    const char *text = json_string_value(json_object_get(peer, "msg"));
    if (text && !strcmp(text, "BYE")) {
        s->state = NVST_SIGNAL_CLOSED;
        snprintf(s->status, sizeof(s->status), "NVIDIA sent BYE; offer=%lu bytes", (unsigned long)s->offer_size);
        record(s, "peer-bye");
    } else if (text) {
        json_t *payload = json_loads(text, JSON_REJECT_DUPLICATES, &error);
        const char *type = json_string_value(json_object_get(payload, "type"));
        const char *sdp = json_string_value(json_object_get(payload, "sdp"));
        if (type && !strcmp(type, "offer") && sdp) {
            size_t n = strlen(sdp);
            char *copy = n <= WS_MESSAGE_LIMIT ? malloc(n + 1) : NULL;
            if (!copy) fail(s, "SDP exceeds limit/allocation failure");
            else {
                memcpy(copy, sdp, n+1); free(s->offer_sdp); s->offer_sdp = copy;
                s->offer_size = n; s->state = NVST_SIGNAL_OFFER;
                const char *nvst = json_string_value(json_object_get(payload, "nvstSdp"));
                if (nvst) {
                    const size_t nn = strlen(nvst);
                    char *nc = nn <= WS_MESSAGE_LIMIT ? malloc(nn + 1) : NULL;
                    if (nc) {
                        memcpy(nc, nvst, nn + 1);
                        free(s->offer_nvst_sdp); s->offer_nvst_sdp = nc;
                        s->offer_nvst_size = nn;
                    }
                }
                snprintf(s->status, sizeof(s->status), "NVST offer received (%lu bytes)", (unsigned long)n);
                record(s, "offer-received");
            }
        } else if (json_is_string(json_object_get(payload, "candidate"))) {
            /* Keep complete candidate objects for the future peer adapter. */
            size_t n = strlen(text);
            if (s->remote_ice_count >= 32 || n >= sizeof(s->remote_ice[0]))
                fail(s, "ICE candidate buffer limit");
            else {
                memcpy(s->remote_ice[s->remote_ice_count++], text, n + 1);
                record(s, "ice-received");
            }
        }
        json_decref(payload);
    }
    json_decref(root);
    return s->state != NVST_SIGNAL_ERROR;
}

static bool upgrade(NvstSignal *s, const char *url, const char *session_id)
{
    const char *authority = url + 6; /* start() validates wss:// */
    const char *path = strchr(authority, '/');
    if (!path) return fail(s, "NVST URL missing path");
    unsigned char raw[16], digest[20], key[25], accept[29];
    size_t n = 0;
    if (!random_bytes(s, raw, sizeof(raw)) ||
        mbedtls_base64_encode(key, sizeof(key), &n, raw, sizeof(raw)))
        return fail(s, "WebSocket key generation failed");
    char challenge[61];
    snprintf(challenge, sizeof(challenge), "%s258EAFA5-E914-47DA-95CA-C5AB0DC85B11", key);
    if (mbedtls_sha1_ret((unsigned char *)challenge, strlen(challenge), digest) ||
        mbedtls_base64_encode(accept, sizeof(accept), &n, digest, sizeof(digest)))
        return fail(s, "WebSocket challenge calculation failed");
    char protocol[200];
    snprintf(protocol, sizeof(protocol), "x-nv-sessionid.%s", session_id);
    static char request[2048], response[8192];
    int length = snprintf(request, sizeof(request),
        "GET %s HTTP/1.1\r\nHost: %.*s\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Protocol: %s\r\n"
        "Origin: https://play.geforcenow.com\r\nUser-Agent: %s\r\n\r\n",
        path, (int)(path-authority), authority, key, protocol, SIGNAL_USER_AGENT);
    if (length < 0 || (size_t)length >= sizeof(request)) return fail(s, "NVST upgrade request too large");
    if (!send_raw(s, request, (size_t)length)) return false;
    memset(request, 0, sizeof(request));
    size_t used = 0;
    uint64_t deadline = osGetTime() + 12000;
    while (osGetTime() < deadline && used < sizeof(response)-1) {
        size_t received = 0;
        errno = 0; s->curl_error[0] = 0;
        CURLcode code = curl_easy_recv((CURL *)s->curl, response+used, sizeof(response)-used-1, &received);
        if (would_block(code)) { ++s->empty_polls; svcSleepThread(10000000); continue; }
        if (code != CURLE_OK) { transport_error(s, "Upgrade recv", code); return false; }
        if (!received) return fail(s, "EOF during WebSocket upgrade");
        used += received; response[used] = 0;
        char *end = strstr(response, "\r\n\r\n");
        if (!end) continue;
        size_t h = (size_t)(end + 4 - response);
        if (!ws_validate_upgrade(response, h, (char *)accept, protocol)) {
            int http_status = 0;
            sscanf(response, "HTTP/1.1 %d", &http_status);
            s->upgrade_http = http_status;
            char reason[100];
            if (http_status == 404 || http_status == 410)
                snprintf(reason, sizeof(reason), "NVIDIA ended this session (HTTP %d). Press A to start the game again.", http_status);
            else
                snprintf(reason, sizeof(reason), "Invalid WebSocket upgrade HTTP=%d (challenge/headers)", http_status);
            return fail(s, reason);
        }
        s->state = NVST_SIGNAL_WAITING;
        record(s, "upgrade-verified");
        /* Parse bytes coalesced with the HTTP response immediately. */
        if (used > h && !ws_parser_feed(&s->parser, (uint8_t *)response+h, used-h, incoming, s)) {
            if (s->state != NVST_SIGNAL_ERROR) fail(s, s->parser.error);
            return false;
        }
        return nvst_signal_active(s);
    }
    return fail(s, "WebSocket upgrade timeout/header limit");
}

void nvst_signal_init(NvstSignal *s) { memset(s, 0, sizeof(*s)); }

bool nvst_signal_start(NvstSignal *s, const char *base_url, const char *session_id)
{
    nvst_signal_close(s);
    s->started_ms = osGetTime();
    diagnostic_log("NVST", "start signaling");
    if (!base_url || strncmp(base_url, "wss://", 6) || !session_id || !session_id[0])
        return fail(s, "Invalid secure signaling endpoint");
    if (strlen(session_id) >= 160 || strspn(session_id, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_.") != strlen(session_id))
        return fail(s, "Invalid session ID for signaling");
    SignalRandom *rng = calloc(1, sizeof(*rng));
    if (!rng) return fail(s, "NVST random allocation failed");
    s->random = rng;
    mbedtls_entropy_init(&rng->entropy); mbedtls_ctr_drbg_init(&rng->drbg);
    const unsigned char personalization[] = "opennow-3ds-websocket";
    if (mbedtls_ctr_drbg_seed(&rng->drbg, mbedtls_entropy_func, &rng->entropy,
                            personalization, sizeof(personalization)-1))
        return fail(s, "NVST secure random seed failed");
    uint32_t peer_random;
    if (!random_bytes(s, (unsigned char *)&peer_random, sizeof(peer_random))) return fail(s, "NVST random failed");
    s->reconnect = !strcmp(session_id, g_last_session);
    if (!s->reconnect) g_reuse_failed = false;
    if (s->reconnect && g_last_peer[0] && !g_reuse_failed) {
        snprintf(s->peer_name, sizeof(s->peer_name), "%s", g_last_peer);
    } else {
        snprintf(s->peer_name, sizeof(s->peer_name), "peer-%lu", (unsigned long)peer_random);
        g_reuse_failed = false;
    }
    snprintf(g_last_session, sizeof(g_last_session), "%s", session_id);
    snprintf(g_last_peer, sizeof(g_last_peer), "%s", s->peer_name);
    diagnostic_log("NVST", "%s peer=%s", s->reconnect ? "reconnect" : "connect", s->peer_name);
    char base[640], url[960], connection[1024];
    if (strlen(base_url) >= sizeof(base) || strpbrk(base_url, "\r\n")) return fail(s, "Invalid signaling URL length/characters");
    snprintf(base, sizeof(base), "%s", base_url);
    char *query = strchr(base, '?'); if (query) *query = 0;
    size_t len = strlen(base);
    while (len && base[len-1] == '/') base[--len] = 0;
    if (len >= 8 && !strcmp(base+len-8, "/sign_in")) base[len-8] = 0;
    snprintf(url, sizeof(url), "%s/sign_in?peer_id=%s&version=2&peer_role=1&pairing_id=%s", base, s->peer_name, session_id);
    snprintf(connection, sizeof(connection), "https://%s", url+6);
    CURL *curl = curl_easy_init();
    if (!curl) return fail(s, "NVST curl allocation failed");
    s->curl = curl;
    s->remote_peer_id = 1;
    struct curl_blob ca = {(void *)_binary_romfs_cacert_pem_start,
        (size_t)(_binary_romfs_cacert_pem_end-_binary_romfs_cacert_pem_start), CURL_BLOB_NOCOPY};
    curl_easy_setopt(curl, CURLOPT_URL, connection);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, s->curl_error);
    curl_easy_setopt(curl, CURLOPT_CONNECT_ONLY, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 12L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, (long)CURL_HTTP_VERSION_1_1);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl, CURLOPT_CAINFO_BLOB, &ca);
    errno = 0;
    CURLcode code = curl_easy_perform(curl);
    if (code != CURLE_OK) { transport_error(s, "TLS connect", code); return false; }
    record(s, "tls-connected");
    if (!upgrade(s, url, session_id)) return false;
    if (!peer_info(s) || !heartbeat(s)) return false;
    if (!s->offer_size) snprintf(s->status, sizeof(s->status), "NVST verified; waiting for SDP offer");
    return true;
}

void nvst_signal_tick(NvstSignal *s)
{
    if (!nvst_signal_active(s)) return;
    if (osGetTime()-s->last_heartbeat_ms >= 5000 && !heartbeat(s)) return;
    if (!s->offer_size && osGetTime()-s->last_peer_info_ms >= 2000 && !peer_info(s)) return;
    for (unsigned i = 0; i < 8; ++i) {
        uint8_t chunk[2048]; size_t n = 0;
        errno = 0; s->curl_error[0] = 0;
        CURLcode code = curl_easy_recv((CURL *)s->curl, chunk, sizeof(chunk), &n);
        if (would_block(code)) { ++s->empty_polls; break; }
        if (code != CURLE_OK) { transport_error(s, "Frame recv", code); return; }
        if (!n) { fail(s, "NVST transport EOF"); return; }
        if (!ws_parser_feed(&s->parser, chunk, n, incoming, s)) {
            if (s->state != NVST_SIGNAL_ERROR) fail(s, s->parser.error);
            return;
        }
        if (!nvst_signal_active(s)) return;
    }
    /* A reconnect normally gets its offer within 2 s; waiting 30 s for a
     * server that is not going to send one only delays the next try. */
    const u64 offer_wait = s->reconnect ? 12000 : 30000;
    if (!s->offer_size && osGetTime()-s->started_ms >= offer_wait)
        fail(s, s->reconnect ? "No SDP offer within 12s after reconnecting" : "No SDP offer within 30s; see NVST counters");
}

bool nvst_signal_send_answer(NvstSignal *s, const char *sdp, const char *nvst)
{
    if (!nvst_signal_active(s) || !sdp || !nvst) return false;
    json_t *payload = json_pack("{s:s,s:s,s:s}", "type", "answer", "sdp", sdp, "nvstSdp", nvst);
    char *text = payload ? json_dumps(payload, JSON_COMPACT) : NULL;
    json_decref(payload);
    if (!text) return fail(s, "NVST answer allocation failed");
    json_t *envelope = json_pack("{s:i,s:{s:i,s:i,s:s}}", "ackid", (int)++s->ack_counter,
        "peer_msg", "from", (int)s->local_peer_id, "to", (int)s->remote_peer_id, "msg", text);
    free(text);
    bool ok = send_json(s, envelope); json_decref(envelope);
    if (ok) record(s, "answer-sent");
    return ok;
}

bool nvst_signal_send_candidate(NvstSignal *s, const char *candidate_json)
{
    if (!nvst_signal_active(s) || !candidate_json) return false;
    json_t *envelope = json_pack("{s:i,s:{s:i,s:i,s:s}}", "ackid", (int)++s->ack_counter,
        "peer_msg", "from", (int)s->local_peer_id, "to", (int)s->remote_peer_id, "msg", candidate_json);
    bool ok = send_json(s, envelope); json_decref(envelope);
    if (ok) record(s, "candidate-sent");
    return ok;
}

void nvst_signal_close(NvstSignal *s)
{
    if (!s) return;
    if (s->curl) curl_easy_cleanup((CURL *)s->curl);
    if (s->random) {
        SignalRandom *rng = s->random;
        mbedtls_ctr_drbg_free(&rng->drbg); mbedtls_entropy_free(&rng->entropy); free(rng);
    }
    ws_parser_free(&s->parser); free(s->offer_sdp); free(s->offer_nvst_sdp);
    memset(s, 0, sizeof(*s));
}
bool nvst_signal_active(const NvstSignal *s)
{
    return s && s->curl && (s->state == NVST_SIGNAL_WAITING || s->state == NVST_SIGNAL_OFFER);
}

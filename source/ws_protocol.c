#include "ws_protocol.h"
#include <stdlib.h>
#include <string.h>

static bool reserve(uint8_t **data, size_t *capacity, size_t needed, size_t limit)
{
    if (needed > limit) return false;
    if (needed <= *capacity) return true;
    size_t n = *capacity ? *capacity : 2048;
    while (n < needed) n = n > limit / 2 ? limit : n * 2;
    uint8_t *grown = realloc(*data, n);
    if (!grown) return false;
    *data = grown;
    *capacity = n;
    return true;
}

void ws_parser_free(WsParser *p)
{
    free(p->wire); free(p->message); memset(p, 0, sizeof(*p));
}

static bool fail(WsParser *p, const char *error)
{
    p->error = error;
    return false;
}

static bool feed_chunk(WsParser *p, const uint8_t *data, size_t size,
                    WsMessageFn callback, void *context)
{
    if (p->error || p->closed) return false;
    if (size > WS_MESSAGE_LIMIT + 14 - p->wire_size ||
        !reserve(&p->wire, &p->wire_capacity, p->wire_size + size, WS_MESSAGE_LIMIT + 14))
        return fail(p, "WebSocket wire limit/allocation");
    if (size) memcpy(p->wire + p->wire_size, data, size);
    p->wire_size += size;
    while (p->wire_size >= 2) {
        const uint8_t *w = p->wire;
        bool final = (w[0] & 128) != 0;
        unsigned op = w[0] & 15;
        if ((w[0] & 0x70) || (w[1] & 128))
            return fail(p, "WebSocket RSV/masked server frame");
        if (op != 0 && op != 1 && op != 2 && op != 8 && op != 9 && op != 10)
            return fail(p, "WebSocket invalid opcode");
        uint64_t len = w[1] & 127;
        if (op >= 8 && (!final || len > 125))
            return fail(p, "WebSocket invalid control frame");
        size_t h = 2;
        if (len == 126) {
            if (p->wire_size < 4) return true;
            len = ((uint64_t)w[2] << 8) | w[3]; h = 4;
            if (len < 126) return fail(p, "WebSocket noncanonical length");
        } else if (len == 127) {
            if (p->wire_size < 10) return true;
            len = 0; h = 10;
            for (size_t i = 2; i < 10; ++i) len = (len << 8) | w[i];
            if (len < 65536) return fail(p, "WebSocket noncanonical length");
        }
        if (len > WS_MESSAGE_LIMIT) return fail(p, "WebSocket frame exceeds 128 KiB");
        size_t n = (size_t)len;
        if (p->wire_size < h + n) return true;
        ++p->frames;
        if (op >= 8) {
            if (op == 8 && n == 1) return fail(p, "WebSocket invalid close payload");
            if (!callback(context, op, w + h, n)) return fail(p, "WebSocket control callback failed");
            if (op == 8) { p->closed = true; return true; }
        } else {
            if ((op == 0 && !p->fragmented_opcode) || (op != 0 && p->fragmented_opcode))
                return fail(p, "WebSocket fragment order");
            if (op) p->fragmented_opcode = op;
            if (n > WS_MESSAGE_LIMIT - p->message_size ||
                !reserve(&p->message, &p->message_capacity, p->message_size + n + 1, WS_MESSAGE_LIMIT + 1))
                return fail(p, "WebSocket message exceeds 128 KiB/allocation");
            memcpy(p->message + p->message_size, w + h, n);
            p->message_size += n;
            p->message[p->message_size] = 0;
            if (final) {
                if (!callback(context, p->fragmented_opcode, p->message, p->message_size))
                    return fail(p, "WebSocket message callback failed");
                p->message_size = 0; p->fragmented_opcode = 0;
            }
        }
        memmove(p->wire, p->wire + h + n, p->wire_size - h - n);
        p->wire_size -= h + n;
    }
    return true;
}

bool ws_parser_feed(WsParser *p, const uint8_t *data, size_t size,
                    WsMessageFn callback, void *context)
{
    if (!size) return feed_chunk(p, data, 0, callback, context);
    while (size) {
        size_t space = WS_MESSAGE_LIMIT + 14 - p->wire_size;
        size_t n = size < space ? size : space;
        if (!n) return fail(p, "WebSocket parser stalled at wire limit");
        if (!feed_chunk(p, data, n, callback, context)) return false;
        if (p->closed) return true;
        data += n; size -= n;
    }
    return true;
}

size_t ws_encode_frame(uint8_t *out, size_t capacity, unsigned opcode,
                       const uint8_t *data, size_t size, const uint8_t mask[4])
{
    if (size > WS_MESSAGE_LIMIT || (opcode >= 8 && size > 125)) return 0;
    size_t h = size < 126 ? 2 : size < 65536 ? 4 : 10;
    if (capacity < h + 4 + size) return 0;
    out[0] = (uint8_t)(128 | opcode);
    if (h == 2) out[1] = (uint8_t)(128 | size);
    else if (h == 4) {
        out[1] = 254; out[2] = (uint8_t)(size >> 8); out[3] = (uint8_t)size;
    } else {
        out[1] = 255;
        for (unsigned i = 0; i < 8; ++i) out[2 + i] = (uint8_t)((uint64_t)size >> (56 - i * 8));
    }
    memcpy(out + h, mask, 4);
    for (size_t i = 0; i < size; ++i) out[h + 4 + i] = data[i] ^ mask[i & 3];
    return h + 4 + size;
}

static bool equals(const char *s, size_t n, const char *value, bool fold)
{
    if (n != strlen(value)) return false;
    for (size_t i = 0; i < n; ++i) {
        char a = s[i], b = value[i];
        if (fold) {
            if (a >= 'A' && a <= 'Z') a += 'a' - 'A';
            if (b >= 'A' && b <= 'Z') b += 'a' - 'A';
        }
        if (a != b) return false;
    }
    return true;
}
static bool token(const char *s, size_t n, const char *value)
{
    while (n) {
        size_t end = 0;
        while (end < n && s[end] != ',') ++end;
        size_t left = 0, right = end;
        while (left < right && (s[left] == ' ' || s[left] == '\t')) ++left;
        while (right > left && (s[right-1] == ' ' || s[right-1] == '\t')) --right;
        if (equals(s + left, right - left, value, true)) return true;
        if (end == n) return false;
        s += end + 1; n -= end + 1;
    }
    return false;
}

bool ws_validate_upgrade(const char *s, size_t size, const char *accept, const char *protocol)
{
    if (size < 16 || memcmp(s, "HTTP/1.1 101", 12) || (s[12] != ' ' && s[12] != '\r')) return false;
    bool upgrade = false, connection = false, accepted = false, selected = false;
    size_t start = 0;
    while (start + 1 < size && !(s[start] == '\r' && s[start+1] == '\n')) ++start;
    start += 2;
    while (start + 1 < size) {
        size_t end = start;
        while (end + 1 < size && !(s[end] == '\r' && s[end+1] == '\n')) ++end;
        if (end + 1 >= size) return false;
        if (end == start) return upgrade && connection && accepted;
        size_t colon = start;
        while (colon < end && s[colon] != ':') ++colon;
        if (colon == start || colon == end || s[start] == ' ' || s[start] == '\t') return false;
        size_t left = colon + 1, right = end;
        while (left < right && (s[left] == ' ' || s[left] == '\t')) ++left;
        while (right > left && (s[right-1] == ' ' || s[right-1] == '\t')) --right;
        if (equals(s+start, colon-start, "Upgrade", true)) upgrade |= token(s+left,right-left,"websocket");
        if (equals(s+start, colon-start, "Connection", true)) connection |= token(s+left,right-left,"Upgrade");
        if (equals(s+start, colon-start, "Sec-WebSocket-Accept", true)) {
            if (accepted || !equals(s+left,right-left,accept,false)) return false;
            accepted = true;
        }
        if (equals(s+start, colon-start, "Sec-WebSocket-Protocol", true)) {
            if (selected || !equals(s+left,right-left,protocol,false)) return false;
            selected = true;
        }
        if (equals(s+start, colon-start, "Sec-WebSocket-Extensions", true)) return false;
        start = end + 2;
    }
    return false;
}

#include "xcloud_net.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

static bool has_host_suffix(const char *host, size_t host_len, const char *suffix)
{
    const size_t suffix_len = strlen(suffix);
    return host_len > suffix_len && host[0] != '.' &&
           strncmp(host + host_len - suffix_len, suffix, suffix_len) == 0;
}

bool xcloud_valid_gs_base(const char *url)
{
    if (!url || strncmp(url, "https://", 8) != 0) return false;
    const char *host = url + 8;
    const size_t host_len = strcspn(host, "/:?#");
    if (host_len == 0) return false;
    /* Allow optional trailing slash only (no path, query, or fragment). */
    if (host[host_len] != '\0' && !(host[host_len] == '/' && host[host_len + 1] == '\0')) return false;
    return has_host_suffix(host, host_len, ".gssv-play-prod.xboxlive.com") ||
           has_host_suffix(host, host_len, ".gssv-play-prodxhome.xboxlive.com");
}

bool xcloud_is_home_store(const char *store)
{
    return store && strcmp(store, "Remote Play") == 0;
}

static bool ascii_ieq(const char *a, const char *b)
{
    if (!a || !b) return false;
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
        ++a;
        ++b;
    }
    return *a == '\0' && *b == '\0';
}

XboxPower xcloud_console_power(const char *power_state)
{
    if (!power_state || !power_state[0]) return XBC_POWER_UNKNOWN;
    if (ascii_ieq(power_state, "On")) return XBC_POWER_ON;
    if (ascii_ieq(power_state, "ConnectedStandby") || ascii_ieq(power_state, "Standby")) return XBC_POWER_STANDBY;
    if (ascii_ieq(power_state, "Off") || ascii_ieq(power_state, "SystemUpdate")) return XBC_POWER_OFF;
    return XBC_POWER_UNKNOWN;
}

void xcloud_session_subpath(char *out, size_t size, const char *session_id, const char *suffix)
{
    if (!out || !size) return;
    if (!session_id) session_id = "";
    while (*session_id == '/') ++session_id;
    if (suffix && suffix[0]) {
        while (*suffix == '/') ++suffix;
        snprintf(out, size, "/%s/%s", session_id, suffix);
    } else {
        snprintf(out, size, "/%s", session_id);
    }
}

static int hex_val(char ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

bool xcloud_parse_ipv6(const char *text, uint8_t out[16])
{
    if (!text || !text[0] || !out) return false;
    /* Strip optional %zone suffix (e.g. fe80::1%eth0). */
    char buf[64];
    const char *pct = strchr(text, '%');
    const size_t raw_len = pct ? (size_t)(pct - text) : strlen(text);
    if (raw_len == 0 || raw_len >= sizeof(buf)) return false;
    memcpy(buf, text, raw_len);
    buf[raw_len] = '\0';

    uint16_t words[8];
    memset(words, 0, sizeof(words));
    int count = 0;
    int compress_at = -1;
    const char *p = buf;

    if (p[0] == ':') {
        if (p[1] != ':') return false;
        compress_at = 0;
        p += 2;
        if (*p == '\0') {
            memset(out, 0, 16);
            return true;
        }
    }

    while (*p) {
        if (count >= 8) return false;
        uint32_t val = 0;
        int digits = 0;
        while (*p && *p != ':') {
            const int v = hex_val(*p);
            if (v < 0 || digits >= 4) return false;
            val = (val << 4) | (uint32_t)v;
            ++digits;
            ++p;
        }
        if (digits == 0) return false;
        words[count++] = (uint16_t)val;
        if (*p == '\0') break;
        ++p;
        if (*p == ':') {
            if (compress_at >= 0) return false;
            compress_at = count;
            ++p;
            if (*p == '\0') break;
            if (*p == ':') return false;
        } else if (*p == '\0') {
            return false;
        }
    }

    if (compress_at >= 0) {
        const int zeros = 8 - count;
        if (zeros <= 0) return false;
        const int tail = count - compress_at;
        for (int i = tail - 1; i >= 0; --i)
            words[compress_at + zeros + i] = words[compress_at + i];
        for (int i = 0; i < zeros; ++i)
            words[compress_at + i] = 0;
    } else if (count != 8) {
        return false;
    }

    for (int i = 0; i < 8; ++i) {
        out[i * 2] = (uint8_t)(words[i] >> 8);
        out[i * 2 + 1] = (uint8_t)(words[i] & 0xFF);
    }
    return true;
}

bool xcloud_teredo_ipv4(const char *ipv6_text, char *ip4_out, size_t ip4_size, uint16_t *port_out)
{
    uint8_t b[16];
    if (!ip4_out || !ip4_size || !xcloud_parse_ipv6(ipv6_text, b)) return false;
    /* RFC 4380 Teredo prefix: 2001:0000::/32. */
    if (b[0] != 0x20 || b[1] != 0x01 || b[2] != 0x00 || b[3] != 0x00) return false;
    const uint16_t port = (uint16_t)(((uint16_t)(b[10] ^ 0xFFu) << 8) | (uint16_t)(b[11] ^ 0xFFu));
    snprintf(ip4_out, ip4_size, "%u.%u.%u.%u",
             (unsigned)(b[12] ^ 0xFFu), (unsigned)(b[13] ^ 0xFFu),
             (unsigned)(b[14] ^ 0xFFu), (unsigned)(b[15] ^ 0xFFu));
    if (port_out) *port_out = port;
    return true;
}

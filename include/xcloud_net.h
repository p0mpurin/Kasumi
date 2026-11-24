#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Pure helpers for Xbox Cloud Gaming and Xbox Remote Play (xHome) URL,
 * session path, console power state, and Teredo IPv6->IPv4 ICE handling. */

typedef enum {
    XBC_POWER_UNKNOWN,
    XBC_POWER_ON,
    XBC_POWER_STANDBY,
    XBC_POWER_OFF
} XboxPower;

bool xcloud_valid_gs_base(const char *url);
bool xcloud_valid_stun_url(const char *url);
bool xcloud_is_private_ipv4(const char *ip);
bool xcloud_same_subnet24(const char *ip_a, const char *ip_b);
bool xcloud_is_home_store(const char *store);
XboxPower xcloud_console_power(const char *power_state);
void xcloud_session_subpath(char *out, size_t size, const char *session_id, const char *suffix);
bool xcloud_parse_ipv6(const char *text, uint8_t out[16]);
bool xcloud_teredo_ipv4(const char *ipv6_text, char *ip4_out, size_t ip4_size, uint16_t *port_out);

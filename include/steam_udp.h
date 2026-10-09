#pragma once

/* UDP sockets and a millisecond clock for the Steam Link code, on the 3DS
 * (libctru's BSD sockets) and on Windows (the PC test harness, Winsock).
 * Addresses are IPv4 in host byte order. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define STEAM_UDP_INVALID (-1)

uint64_t steam_now_ms(void);
/* A non-blocking UDP socket on an ephemeral port, broadcast allowed. */
int steam_udp_open(void);
void steam_udp_close(int sock);
bool steam_udp_send(int sock, uint32_t ip, uint16_t port, const void *data, size_t size);
/* One datagram: its length, 0 when none is waiting, -1 on error. */
int steam_udp_recv(int sock, void *data, size_t cap, uint32_t *ip, uint16_t *port);
/* Wait until the socket is readable or the timeout passes. */
bool steam_udp_wait(int sock, int timeout_ms);
/* The local network's broadcast address (255.255.255.255 if unknown). */
uint32_t steam_udp_broadcast(void);
uint32_t steam_ip_parse(const char *text);
void steam_ip_format(uint32_t ip, char *out, size_t size);

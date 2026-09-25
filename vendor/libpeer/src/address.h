#ifndef ADDRESS_H_
#define ADDRESS_H_

#include "config.h"
#if CONFIG_USE_LWIP
#include <lwip/sockets.h>
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#endif
#include <stdint.h>
#if defined(__3DS__)
/* libctru exposes IPv4 sockets only. These types allow disabled IPv6 branches
 * to compile; the 3DS adapter only creates AF_INET candidates/sockets. */
#define INET6_ADDRSTRLEN 46
struct in6_addr { uint8_t s6_addr[16]; };
struct sockaddr_in6 {
  sa_family_t sin6_family;
  uint16_t sin6_port;
  uint32_t sin6_flowinfo;
  struct in6_addr sin6_addr;
  uint32_t sin6_scope_id;
};
static const struct in6_addr in6addr_any = {{0}};
#endif

#define ADDRSTRLEN INET6_ADDRSTRLEN

typedef struct Address {
  uint8_t family;
  struct sockaddr_in sin;
  struct sockaddr_in6 sin6;
  uint16_t port;
} Address;

void addr_set_family(Address* addr, int family);

void addr_set_port(Address* addr, uint16_t port);

int addr_inet6_validate(const char* ipv6, size_t len, Address* addr);

int addr_inet_validate(const char* ipv4, size_t len, Address* addr);

int addr_to_string(const Address* addr, char* buf, size_t len);

int addr_from_string(const char* str, Address* addr);

int addr_equal(const Address* a, const Address* b);

#endif  // ADDRESS_H_

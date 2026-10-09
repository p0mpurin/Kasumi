#include "steam_udp.h"

#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

uint64_t steam_now_ms(void) { return GetTickCount64(); }

int steam_udp_open(void)
{
    static bool started;
    if (!started) {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
        started = true;
    }
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return STEAM_UDP_INVALID;
    BOOL yes = TRUE;
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, (const char *)&yes, sizeof(yes));
    int buffer = 1 << 20;
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, (const char *)&buffer, sizeof(buffer));
    u_long nonblocking = 1;
    ioctlsocket(s, FIONBIO, &nonblocking);
    struct sockaddr_in local;
    memset(&local, 0, sizeof(local));
    local.sin_family = AF_INET;
    bind(s, (struct sockaddr *)&local, sizeof(local));
    return (int)s;
}

void steam_udp_close(int sock)
{
    if (sock != STEAM_UDP_INVALID) closesocket((SOCKET)sock);
}

uint32_t steam_udp_broadcast(void) { return 0xFFFFFFFFu; }

#else
#include <3ds.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

uint64_t steam_now_ms(void) { return osGetTime(); }

int steam_udp_open(void)
{
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return STEAM_UDP_INVALID;
    int yes = 1;
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));
    /* Video comes in bursts of a frame at a time: room for a few frames. */
    int buffer = 256 * 1024;
    while (buffer >= 32 * 1024 && setsockopt(s, SOL_SOCKET, SO_RCVBUF, &buffer, sizeof(buffer)) != 0) buffer /= 2;
    fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
    struct sockaddr_in local;
    memset(&local, 0, sizeof(local));
    local.sin_family = AF_INET;
    bind(s, (struct sockaddr *)&local, sizeof(local));
    return s;
}

void steam_udp_close(int sock)
{
    if (sock >= 0) close(sock);
}

uint32_t steam_udp_broadcast(void)
{
    struct in_addr ip, mask, broadcast;
    if (R_SUCCEEDED(SOCU_GetIPInfo(&ip, &mask, &broadcast)) && broadcast.s_addr)
        return ntohl(broadcast.s_addr);
    return 0xFFFFFFFFu;
}
#endif

bool steam_udp_send(int sock, uint32_t ip, uint16_t port, const void *data, size_t size)
{
    struct sockaddr_in to;
    memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_port = htons(port);
    to.sin_addr.s_addr = htonl(ip);
    return sendto(sock, (const char *)data, (int)size, 0, (struct sockaddr *)&to, sizeof(to)) == (int)size;
}

int steam_udp_recv(int sock, void *data, size_t cap, uint32_t *ip, uint16_t *port)
{
    struct sockaddr_in from;
    socklen_t length = sizeof(from);
    const int n = (int)recvfrom(sock, (char *)data, (int)cap, 0, (struct sockaddr *)&from, &length);
    if (n < 0) {
#ifdef _WIN32
        const int error = WSAGetLastError();
        return error == WSAEWOULDBLOCK || error == WSAECONNRESET ? 0 : -1;
#else
        return errno == EAGAIN || errno == EWOULDBLOCK ? 0 : -1;
#endif
    }
    if (ip) *ip = ntohl(from.sin_addr.s_addr);
    if (port) *port = ntohs(from.sin_port);
    return n;
}

bool steam_udp_wait(int sock, int timeout_ms)
{
#ifdef _WIN32
    fd_set set;
    FD_ZERO(&set);
    FD_SET((SOCKET)sock, &set);
    struct timeval tv = { timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
    return select(0, &set, NULL, NULL, &tv) > 0;
#else
    struct pollfd p = { .fd = sock, .events = POLLIN, .revents = 0 };
    return poll(&p, 1, timeout_ms) > 0 && (p.revents & POLLIN);
#endif
}

uint32_t steam_ip_parse(const char *text)
{
    unsigned a, b, c, d;
    if (!text || sscanf(text, "%u.%u.%u.%u", &a, &b, &c, &d) != 4 || a > 255 || b > 255 || c > 255 || d > 255)
        return 0;
    return (a << 24) | (b << 16) | (c << 8) | d;
}

void steam_ip_format(uint32_t ip, char *out, size_t size)
{
    snprintf(out, size, "%u.%u.%u.%u", (unsigned)(ip >> 24), (unsigned)((ip >> 16) & 255),
             (unsigned)((ip >> 8) & 255), (unsigned)(ip & 255));
}

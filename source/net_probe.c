#include "net_probe.h"

#include <3ds.h>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define PROBE_MAGIC 0x4F4E3344u
#define PROBE_VERSION 1u
#define PROBE_HELLO 0u
#define PROBE_LOAD 1u
#define PROBE_ECHO 2u
#define PROBE_ECHO_REPLY 3u
#define PROBE_END 4u
#define PROBE_REPORT 5u
#define PROBE_MAX_PACKET 2048
#define PROBE_REPORT_WORDS 5

typedef struct __attribute__((packed)) {
    u32 magic;
    u16 version;
    u16 kind;
    u32 run_id;
    u32 sequence;
    u64 sender_ticks;
    u16 payload_length;
    u16 reserved;
} ProbeHeader;

typedef struct {
    int socket_fd;
    bool initialized;
    bool running;
    bool pinned;
    u32 run_id;
    u32 received_packets;
    u32 received_bytes;
    u32 lost_packets;
    u32 duplicate_packets;
    u32 reordered_packets;
    u32 last_sequence;
    bool have_sequence;
    struct sockaddr_in peer;
} ProbeState;

static ProbeState state = {.socket_fd = -1};

static u64 from_be64(u64 value)
{
    return __builtin_bswap64(value);
}

static u64 to_be64(u64 value)
{
    return __builtin_bswap64(value);
}

static bool same_peer(const struct sockaddr_in *a, const struct sockaddr_in *b)
{
    return a->sin_addr.s_addr == b->sin_addr.s_addr &&
           a->sin_port == b->sin_port;
}

static void reset_run(const ProbeHeader *header, const struct sockaddr_in *peer)
{
    const int fd = state.socket_fd;
    memset(&state, 0, sizeof(state));
    state.socket_fd = fd;
    state.initialized = true;
    state.running = true;
    state.pinned = true;
    state.run_id = header->run_id;
    state.peer = *peer;
}

static bool parse_header(const u8 *buffer, size_t length, ProbeHeader *header)
{
    ProbeHeader wire;
    if (length < sizeof(*header)) {
        return false;
    }
    memcpy(&wire, buffer, sizeof(wire));
    header->magic = ntohl(wire.magic);
    header->version = ntohs(wire.version);
    header->kind = ntohs(wire.kind);
    header->run_id = ntohl(wire.run_id);
    header->sequence = ntohl(wire.sequence);
    header->sender_ticks = from_be64(wire.sender_ticks);
    header->payload_length = ntohs(wire.payload_length);
    header->reserved = ntohs(wire.reserved);
    if (header->magic != PROBE_MAGIC || header->version != PROBE_VERSION ||
        header->payload_length != length - sizeof(*header) ||
        header->payload_length > PROBE_MAX_PACKET - sizeof(*header)) {
        return false;
    }
    return true;
}

static void send_packet(u16 kind, const ProbeHeader *request,
                        const void *payload, u16 payload_length)
{
    u8 buffer[PROBE_MAX_PACKET];
    ProbeHeader header = {
        .magic = htonl(PROBE_MAGIC),
        .version = htons(PROBE_VERSION),
        .kind = htons(kind),
        .run_id = htonl(request->run_id),
        .sequence = htonl(request->sequence),
        .sender_ticks = to_be64(request->sender_ticks),
        .payload_length = htons(payload_length),
        .reserved = 0,
    };
    if (payload_length > sizeof(buffer) - sizeof(header)) {
        return;
    }
    memcpy(buffer, &header, sizeof(header));
    if (payload_length != 0) {
        memcpy(buffer + sizeof(header), payload, payload_length);
    }
    (void)sendto(state.socket_fd, buffer, sizeof(header) + payload_length, 0,
                 (const struct sockaddr *)&state.peer, sizeof(state.peer));
}

bool probe_init(void)
{
    if (state.initialized) {
        return true;
    }
    state.socket_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (state.socket_fd < 0) {
        return false;
    }
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(PROBE_PORT);
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(state.socket_fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
        close(state.socket_fd);
        state.socket_fd = -1;
        return false;
    }
    int flags = fcntl(state.socket_fd, F_GETFL, 0);
    if (flags < 0 || fcntl(state.socket_fd, F_SETFL, flags | O_NONBLOCK) != 0) {
        close(state.socket_fd);
        state.socket_fd = -1;
        return false;
    }
    state.initialized = true;
    return true;
}

bool probe_start(void)
{
    if (!probe_init()) {
        return false;
    }
    state.running = true;
    state.pinned = false;
    state.have_sequence = false;
    return true;
}

void probe_stop(void)
{
    state.running = false;
    state.pinned = false;
}

bool probe_is_running(void)
{
    return state.initialized && state.running;
}

void probe_poll(void)
{
    if (!state.initialized || !state.running) {
        return;
    }
    u8 buffer[PROBE_MAX_PACKET];
    for (int i = 0; i < 8; i++) {
        struct sockaddr_in peer;
        socklen_t peer_length = sizeof(peer);
        const ssize_t received = recvfrom(state.socket_fd, buffer, sizeof(buffer),
                                          0, (struct sockaddr *)&peer,
                                          &peer_length);
        if (received < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            break;
        }
        ProbeHeader header;
        if (!parse_header(buffer, (size_t)received, &header)) {
            continue;
        }
        if (header.kind == PROBE_HELLO) {
            reset_run(&header, &peer);
            send_packet(PROBE_ECHO_REPLY, &header, NULL, 0);
            continue;
        }
        if (!state.pinned || header.run_id != state.run_id ||
            !same_peer(&peer, &state.peer)) {
            continue;
        }
        if (header.kind == PROBE_END) {
            u32 report[PROBE_REPORT_WORDS] = {
                htonl(state.received_packets),
                htonl(state.received_bytes),
                htonl(state.lost_packets),
                htonl(state.duplicate_packets),
                htonl(state.reordered_packets),
            };
            send_packet(PROBE_REPORT, &header, report, sizeof(report));
            state.running = false;
            continue;
        }
        state.received_packets++;
        state.received_bytes += (u32)received;
        if (state.have_sequence) {
            if (header.sequence > state.last_sequence + 1) {
                state.lost_packets += header.sequence - state.last_sequence - 1;
            } else if (header.sequence == state.last_sequence) {
                state.duplicate_packets++;
            } else if (header.sequence < state.last_sequence) {
                state.reordered_packets++;
            }
        }
        if (!state.have_sequence || header.sequence > state.last_sequence) {
            state.last_sequence = header.sequence;
            state.have_sequence = true;
        }
        if (header.kind == PROBE_ECHO) {
            send_packet(PROBE_ECHO_REPLY, &header, NULL, 0);
        }
    }
}

void probe_print_status(void)
{
    if (!state.initialized) {
        return;
    }
    printf("\x1b[20;1HProbe rx %lu pkts %lu KiB loss %lu dup %lu reorder %lu   ",
           (unsigned long)state.received_packets,
           (unsigned long)(state.received_bytes / 1024),
           (unsigned long)state.lost_packets,
           (unsigned long)state.duplicate_packets,
           (unsigned long)state.reordered_packets);
}

void probe_exit(void)
{
    if (state.socket_fd >= 0) {
        close(state.socket_fd);
    }
    memset(&state, 0, sizeof(state));
    state.socket_fd = -1;
}

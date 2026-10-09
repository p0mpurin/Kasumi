#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "ws_protocol.h"

typedef enum {
    NVST_SIGNAL_IDLE,
    NVST_SIGNAL_WAITING,
    NVST_SIGNAL_OFFER,
    NVST_SIGNAL_ERROR,
    NVST_SIGNAL_CLOSED
} NvstSignalState;

typedef struct NvstSignal {
    void *curl;
    void *random;
    NvstSignalState state;
    char peer_name[32];
    uint32_t local_peer_id;
    uint32_t remote_peer_id;
    uint32_t ack_counter;
    uint64_t last_heartbeat_ms;
    uint64_t started_ms, last_peer_info_ms;
    WsParser parser;
    unsigned messages, heartbeats, acknowledgements, empty_polls;
    unsigned diagnostic_lines;
    bool peer_assigned;
    uint16_t close_code;
    int upgrade_http; /* HTTP status of a refused WebSocket upgrade, else 0 */
    bool reconnect;   /* signalling again for the same session */
    char last_event[40];
    char *offer_sdp;
    size_t offer_size;
    char *offer_nvst_sdp;
    size_t offer_nvst_size;
    unsigned remote_ice_count;
    char remote_ice[32][1024];
    char status[160];
    char curl_error[256];
    /* Xbox Cloud Gaming (xcloud.h): no websocket. offer_sdp holds the
     * service's answer to our offer, remote_ice its candidates, and
     * xcloud_peer the peer that made the offer, until the transport takes
     * it over. */
    bool xcloud;
    void *xcloud_peer;
    /* Steam Link (steam_link.h): no signalling at all. The PC granted a
     * stream at this address with this session key; offer_sdp is only a
     * marker that it is ready. */
    bool steam;
    uint32_t steam_ip;
    uint16_t steam_port;
    uint8_t steam_key[32];
    uint8_t steam_key_size;
    uint64_t steam_id;
    unsigned steam_fps, steam_kbps;
} NvstSignal;

void nvst_signal_init(NvstSignal *signal);
bool nvst_signal_start(NvstSignal *signal, const char *base_url, const char *session_id);
void nvst_signal_tick(NvstSignal *signal);
void nvst_signal_close(NvstSignal *signal);
bool nvst_signal_active(const NvstSignal *signal);
bool nvst_signal_send_answer(NvstSignal *signal, const char *sdp, const char *nvst_sdp);
bool nvst_signal_send_candidate(NvstSignal *signal, const char *candidate_json);

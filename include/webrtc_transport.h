#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct NvstSignal;

typedef enum {
    WEBRTC_IDLE,
    WEBRTC_ANSWER_SENT,
    WEBRTC_CHECKING,
    WEBRTC_CONNECTED,
    WEBRTC_FAILED
} WebRtcState;

typedef struct {
    void *peer;
    WebRtcState state;
    unsigned applied_candidates;
    unsigned rejected_candidates;
    unsigned local_candidates;
    unsigned sent_local_candidates;
    int ice_pairs_total;
    int ice_pairs_frozen;
    int ice_pairs_inprogress;
    int ice_pairs_succeeded;
    int ice_pairs_failed;
    uint32_t gather_sent, gather_received;
    uint32_t checks_sent, udp_received, stun_received, valid_responses;
    uint32_t checks_attempted, send_failures;
    int last_send_errno;
    unsigned video_access_units;
    uint64_t video_bytes;
    uint64_t video_rate_bytes;
    uint64_t video_rate_started_at;
    uint64_t last_video_key_log_at;
    unsigned video_kbps;
    uint32_t video_sps_signature;
    unsigned video_source_width;
    unsigned video_source_height;
    unsigned video_source_refs;
    unsigned decoder_width;
    unsigned decoder_height;
    /* A size MVD refused and when: not retried on every packet. */
    unsigned decoder_failed_width, decoder_failed_height;
    uint64_t decoder_failed_at;
    /* Setups refused in a row: MVD itself has stopped working. */
    unsigned decoder_init_failures;
    /* Steam: a new picture size waiting to hold steady before MVD is rebuilt
     * for it (a window being resized streams a size every few frames). */
    unsigned pending_width, pending_height;
    uint64_t pending_since;
    unsigned decoder_rebuilds;
    /* Xbox: when a 1280x720 stream began (most games never switch to our
     * 800x480), and when the decoder was set up for it. */
    uint64_t xcloud_hd_since, decoder_started_at;
    unsigned keyframe_requests;
    unsigned audio_packets;
    unsigned audio_decoded;
    unsigned audio_dropped;
    unsigned audio_errors;
    unsigned data_messages;
    unsigned input_reports;
    unsigned mouse_moves;
    unsigned mouse_clicks;
    unsigned keyboard_keys;
    bool pointer_mode;
    bool keyboard_mode;
    /* The answer carried a mic section (voice chat can be sent). */
    bool mic_negotiated;
    bool data_open;
    bool input_ready;
    bool input_channel_requested;
    bool prefer_partial_input;
    bool partial_channel_requested;
    uint64_t partial_channel_at;
    uint16_t partial_sequence;
    uint64_t last_mouse_move_at;
    /* Set by the caller after poll(): drain the socket only when it is
     * readable, otherwise just service timers every 16 ms. */
    bool media_readable;
    unsigned video_au_dropped_seen;
    uint64_t last_media_poll_at;
    int input_protocol_version;
    uint64_t data_opened_at;
    uint64_t connected_at;
    uint64_t last_input_at;
    uint64_t last_input_state_log_at;
    uint64_t last_input_heartbeat_at;
    uint64_t answer_sent_at;
    uint64_t last_diagnostic_at;
    uint64_t last_keyframe_request_at;
    uint64_t last_decoded_frame_at;
    unsigned observed_decoded_frames;
    size_t last_video_size;
    int rtt_ms;
    int media_port;
    bool manual_candidate_added;
    bool remote_zero_rewritten;
    bool video_saw_sps;
    bool video_saw_pps;
    bool video_saw_idr;
    unsigned video_idr_units;
    unsigned keyframe_backoff_ms;
    uint32_t last_video_timestamp;
    uint64_t last_video_arrival_at;
    unsigned video_src_skipped, video_late_arrivals, video_max_gap_ms;
    /* The 5 s window behind each VIDEO rate line. */
    unsigned video_log_seconds, video_log_kbps_sum, video_log_kbps_min;
    uint64_t last_au_drop_at;
    bool input_state_logged;
    uint16_t last_input_buttons;
    uint8_t last_input_left_trigger;
    uint8_t last_input_right_trigger;
    int16_t last_input_left_x, last_input_left_y;
    int16_t last_input_right_x, last_input_right_y;
    int remote_candidate_port;
    char media_ip[64];
    char pending_local_candidates[5][256];
    char status[160];
    /* Media is received on the New 3DS's second app core (core 2). */
    bool media_threaded;
    /* Xbox Cloud Gaming stream (xcloud.h): we made the offer, and input
     * goes through xcloud_stream.c. */
    bool xcloud;
    /* Steam Link (steam_session.h): `peer` is the SteamSession, and the
     * stream runs on its own protocol instead of WebRTC. */
    bool steam;
    /* MVD may consume the first IDR while priming; request another once it
     * has produced output, including after a size rebuild. */
    bool steam_decoder_keyframe_pending;
    bool steam_capture_unavailable;
    uint64_t steam_capture_resumed_at;
    /* Reconnecting can't help: the PC ended the Steam stream itself, or the
     * video decoder stopped working. The status says why. */
    bool no_reconnect;
} WebRtcTransport;

/* While media runs on its own core, the peer connection and the video
 * decoder's frames are shared: hold this around presenting a frame. */
void webrtc_transport_lock(void);
void webrtc_transport_unlock(void);
/* The stream loop's wait: for packets on core 2, or on the media socket. */
void webrtc_transport_wait(WebRtcTransport *transport, int timeout_ms);
/* HOME Menu or sleep: the media core stops handing packets on, as the main
 * loop did by not running. */
void webrtc_transport_pause(bool paused);

void webrtc_transport_init(WebRtcTransport *transport);
bool webrtc_transport_start(WebRtcTransport *transport, struct NvstSignal *signal,
                            const char *media_ip, int media_port);
void webrtc_transport_tick(WebRtcTransport *transport, struct NvstSignal *signal);
void webrtc_transport_close(WebRtcTransport *transport);
bool webrtc_transport_gameplay_ready(const WebRtcTransport *transport);
/* The media UDP socket, for poll(); -1 when there is none. */
int webrtc_transport_socket(const WebRtcTransport *transport);
/* Video packets recovered by retransmission since the stream started. */
unsigned webrtc_transport_resent_packets(const WebRtcTransport *transport);
void webrtc_transport_set_pointer_mode(WebRtcTransport *transport, bool enabled);
/* Steam Link: close the game on the PC (Alt+F4) before the stream closes.
 * 1 sent, 0 no game in front (Big Picture or the desktop), -1 not sent. */
int webrtc_transport_steam_quit_game(WebRtcTransport *transport);
bool webrtc_transport_mouse_move(WebRtcTransport *transport, int16_t dx, int16_t dy);
bool webrtc_transport_mouse_button(WebRtcTransport *transport, bool pressed);
/* Voice chat: answer the offer's mic track at the next connect, and send
 * one 20 ms Opus frame on it (any thread; false when not connected). */
void webrtc_transport_set_mic(bool wanted);
bool webrtc_transport_send_mic(const uint8_t *opus, size_t size);
bool webrtc_transport_send_key(WebRtcTransport *transport, uint16_t keycode,
                               uint16_t scancode, uint16_t modifiers);
/* Xbox Cloud Gaming, on the network worker: a peer for the transport
 * passed to webrtc_transport_init and its offer (malloc'd, CRLF lines, our
 * ICE candidates included). webrtc_transport_start adopts the peer once the
 * signal holds the answer; until then discard it on failure. */
void *webrtc_transport_xcloud_offer(char **offer);
void webrtc_transport_discard_peer(void *peer);

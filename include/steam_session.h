#pragma once

/* A Steam Remote Play stream (the session after a granted stream request).
 *
 * One UDP socket to the PC. Every packet has a 13-byte header (type, retry
 * count, our and the host's connection ids, channel, fragment, packet id,
 * 16.16 timestamp) and a CRC-32C. Channel 0 connects and answers the host's
 * path-MTU pings, channel 1 carries control messages (reliable, ordered,
 * encrypted after authentication), channel 2 statistics, and the channels
 * named by StartAudioData / StartVideoData carry Opus and H.264, unreliable
 * and fragmented. Controller input is a virtual HID gamepad (RemoteHID).
 *
 * Not thread-safe: the caller serialises every call (Kasumi holds the
 * transport lock). Platform-neutral, so the PC test harness drives it too. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct SteamSession SteamSession;

typedef struct {
    uint32_t host_ip;
    uint16_t port;
    uint8_t key[32];
    size_t key_size;
    uint64_t steamid;
    unsigned width, height, fps, kbps;
} SteamSessionConfig;

typedef struct {
    void (*video_start)(void *user, unsigned width, unsigned height);
    /* One Annex-B access unit. */
    void (*video)(void *user, const uint8_t *data, size_t size, bool keyframe);
    /* One Opus packet (10 ms, 48 kHz stereo); `sequence` counts packets. */
    void (*audio)(void *user, const uint8_t *data, size_t size, uint16_t sequence);
    /* What the PC shows: EStreamActivity (2 game, 3 desktop), its game. */
    void (*activity)(void *user, int activity, uint64_t gameid, const char *name);
    void *user;
} SteamSessionCallbacks;

typedef enum {
    STEAM_SESSION_CONNECTING,
    STEAM_SESSION_HANDSHAKE,
    STEAM_SESSION_NEGOTIATING,
    STEAM_SESSION_STREAMING,
    STEAM_SESSION_CLOSED,   /* the PC ended it */
    STEAM_SESSION_FAILED
} SteamSessionState;

/* A gamepad in SDL's terms: axes LX, LY, RX, RY (y down), LT, RT
 * (0..32767), and SDL_GameControllerButton bits. */
enum {
    STEAM_PAD_A = 1 << 0, STEAM_PAD_B = 1 << 1, STEAM_PAD_X = 1 << 2, STEAM_PAD_Y = 1 << 3,
    STEAM_PAD_BACK = 1 << 4, STEAM_PAD_GUIDE = 1 << 5, STEAM_PAD_START = 1 << 6,
    STEAM_PAD_LEFT_STICK = 1 << 7, STEAM_PAD_RIGHT_STICK = 1 << 8,
    STEAM_PAD_LEFT_SHOULDER = 1 << 9, STEAM_PAD_RIGHT_SHOULDER = 1 << 10,
    STEAM_PAD_UP = 1 << 11, STEAM_PAD_DOWN = 1 << 12, STEAM_PAD_LEFT = 1 << 13, STEAM_PAD_RIGHT = 1 << 14
};
typedef struct {
    int16_t axes[6];
    uint16_t buttons;
} SteamPad;

typedef struct {
    unsigned video_frames, video_keyframes, video_lost, keyframe_requests;
    unsigned audio_packets, packets_in, packets_bad, resends, hid_reports;
    uint64_t video_bytes;
    unsigned video_width, video_height;
    int rtt_ms;
    bool input_ready;
    /* The host explicitly selected its Black Frame capture placeholder. */
    bool capture_unavailable;
} SteamSessionStats;

SteamSession *steam_session_open(const SteamSessionConfig *config, const SteamSessionCallbacks *callbacks);
int steam_session_socket(const SteamSession *s);
/* Reads and handles one datagram: 1 if one was handled, 0 if none waited. */
int steam_session_receive(SteamSession *s);
/* Timers: resends, keepalive, timeouts. Call at least every 16 ms. */
void steam_session_tick(SteamSession *s);
/* The controller's current state; sent when it changes. */
void steam_session_set_pad(SteamSession *s, const SteamPad *pad);
/* The decoder lost its place: ask for a keyframe. False if no video channel
 * exists yet, or another request was sent within the last 200 ms. */
bool steam_session_request_keyframe(SteamSession *s);
SteamSessionState steam_session_state(const SteamSession *s);
const char *steam_session_status(const SteamSession *s);
void steam_session_stats(const SteamSession *s, SteamSessionStats *out);
/* The PC's mouse and keyboard: relative motion, the left button, and keys
 * by USB HID usage (SDL scancode) with SDL modifier bits (1 = left shift). */
void steam_session_mouse_move(SteamSession *s, int dx, int dy);
bool steam_session_mouse_button(SteamSession *s, bool down);
bool steam_session_key(SteamSession *s, unsigned scancode, bool down, unsigned modifiers);
/* Asks the PC to quit the game (sent now; steam_session_close waits for
 * it to arrive). False if the stream isn't up. */
/* Closes the game in front on the PC (Alt+F4); false when none is. */
bool steam_session_stop_game(SteamSession *s);
bool steam_session_game_running(const SteamSession *s);
/* Leaves the stream (the game keeps running on the PC). */
void steam_session_close(SteamSession *s);

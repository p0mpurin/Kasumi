#pragma once

/* Steam Remote Play outside a stream: finding PCs running Steam on the
 * local network, pairing with one, and asking it to start a stream. All of
 * it is UDP on port 27036: an 8-byte magic, then a length-prefixed
 * CMsgRemoteClientBroadcastHeader and a length-prefixed message.
 *
 * Pairing is what the Steam Link app does: the console shows a 4-digit PIN,
 * the player types it into Steam on the PC ("Authorize Device"), and both
 * sides derive a secret for this PC from an X25519 exchange masked with
 * SHA256(PIN). Steam no longer completes a pairing from the key escrow
 * ticket alone (it answers Failed), so both are sent. The secret then
 * authenticates every stream request (proof challenge) and decrypts the
 * session key.
 *
 * Blocking calls are meant for the network worker. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define STEAM_PORT 27036

/* This console as a Steam Link device: a random id and escrow secret made
 * once and kept on the SD card. */
typedef struct {
    uint64_t device_id;
    uint8_t secret[32];
    char name[64];
} SteamIdentity;

typedef struct {
    char name[64];
    uint32_t ip;
    uint64_t client_id;
    uint64_t instance_id;
    uint64_t steamid;      /* the signed-in user, 0 if none */
    int universe;
    int ostype;
    bool games_running;
    bool screen_locked;
    bool remoteplay_active;
} SteamHost;

/* Ask the network (or one address, ip != 0) who runs Steam. Returns how
 * many answered within the timeout, at most `max`. */
int steam_discover(const SteamIdentity *id, uint32_t ip, SteamHost *hosts, int max, unsigned timeout_ms);

typedef enum {
    STEAM_PAIR_WAITING,    /* the PIN has not been typed yet */
    STEAM_PAIR_DONE,
    STEAM_PAIR_WRONG_PIN,  /* the exchange gave a different secret */
    STEAM_PAIR_REFUSED,    /* Steam answered: see `result` */
    STEAM_PAIR_TIMEOUT,
    STEAM_PAIR_ERROR
} SteamPairState;

typedef struct {
    int sock;
    SteamIdentity id;
    SteamHost host;
    uint8_t private_key[32];
    uint8_t pin_hash[32];
    uint8_t request[900];
    size_t request_size;
    uint64_t started_at, next_send_at;
    int result;            /* ERemoteDeviceAuthorizationResult of a refusal */
    uint8_t secret[32];    /* the host's secret, once DONE */
    uint64_t steamid;
} SteamPairing;

bool steam_pair_begin(SteamPairing *p, const SteamIdentity *id, const SteamHost *host, const char *pin);
/* Sends the request again every second and reads answers; non-blocking. */
SteamPairState steam_pair_poll(SteamPairing *p);
void steam_pair_cancel(SteamPairing *p);
const char *steam_pair_result_name(int result);

typedef struct {
    unsigned width, height, fps;
    uint64_t gameid;     /* 0: no game */
    int stream_interface; /* EStreamInterface: 2 Big Picture, 3 desktop */
} SteamStreamRequest;

typedef struct {
    int result;          /* ERemoteDeviceStreamingResult, -1 no answer */
    uint16_t port;
    uint8_t session_key[32];
    size_t session_key_size;
} SteamStreamGrant;

/* Asks the host to start streaming (Steam may first launch Big Picture or
 * the game). Blocks until Steam answers, up to `timeout_ms`; `cancel` may
 * be set from another thread. */
bool steam_request_stream(const SteamIdentity *id, const uint8_t host_secret[32], const SteamHost *host,
                          const SteamStreamRequest *request, SteamStreamGrant *grant, unsigned timeout_ms,
                          volatile bool *cancel);
const char *steam_stream_result_text(int result);

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "provider.h"

/* Build 69 capped the library at 64; accounts with Steam sync can own
 * hundreds of games (the diagnostic that reported this had 219). */
#define GFN_MAX_GAMES 256
#define GFN_MAX_VARIANTS 4

typedef enum {
    GFN_AUTH_LOGGED_OUT,
    GFN_AUTH_WAITING,
    GFN_AUTH_LOGGED_IN,
    GFN_AUTH_ERROR
} GfnAuthState;

typedef enum {
    GFN_SESSION_IDLE,
    GFN_SESSION_QUEUED,
    GFN_SESSION_SETUP,
    GFN_SESSION_READY,
    GFN_SESSION_ERROR
} GfnSessionState;

typedef struct {
    char title[96];
    char app_id[48];
    char store[24];
    /* Box art on img.nvidiagrid.net (resized by the server on request). */
    char image_url[192];
    /* Wide art (TV banner, key art or hero image), for shortcut banners. */
    char wide_url[192];
    /* Every store this game can launch from; app_id/store above are the
     * one the account selected. */
    struct {
        char id[16];
        char store[16];
    } variants[GFN_MAX_VARIANTS];
    unsigned variant_count;
    unsigned variant_selected;
} GfnGame;

/* A session in the way of a launch (SESSION_LIMIT_PER_DEVICE_EXCEEDED): the
 * refusal names it and the rig that controls it. */
typedef struct {
    char id[160];
    char host[128]; /* rig control server, "" when not named */
    char app_id[24];
    int status;
} GfnConflict;

typedef struct {
    GfnAuthState auth_state;
    /* The provider a sign-in in progress goes through; it becomes the
     * active provider (provider.h) when the sign-in completes. */
    GfnProvider login_provider;
    char status[160];
    char user_code[32];
    char verification_uri[256];
    /* Microsoft's are ~1050 characters (build 123 cut them at 1023, so
     * every approval check failed and the sign-in fell back). */
    char device_code[2048];
    int poll_interval;
    int64_t next_poll_at;
    int64_t challenge_expires_at;
    char access_token[8192];
    char refresh_token[8192];
    char id_token[8192];
    int64_t token_expires_at;
    char client_token[4096];
    int64_t client_token_expires_at;
    char user_id[256];
    GfnGame games[GFN_MAX_GAMES];
    size_t game_count;
    /* When the owned library shown was fetched (0 = never / not cached). */
    int64_t library_saved_at;
    /* Last connection check: Wi-Fi bars, round trip to NVIDIA, download. */
    unsigned conn_bars;
    unsigned conn_latency_ms;
    unsigned conn_kbps;
    int64_t conn_tested_at;
    bool conn_failed;
    /* A session found still running at start-up (after a crash or power
     * loss), and the game it is for. */
    bool resume_found;
    GfnGame resume_game;
    size_t catalog_total;
    char catalog_vpc[64];
    int64_t catalog_vpc_expires_at;
    GfnSessionState session_state;
    char session_id[160];
    char session_client_id[40];
    char session_device_id[40];
    char session_base_url[256];
    /* The zone server that controls the session now. NVIDIA moves a queued
     * session between zones (Frankfurt, London, Stockholm...) and names the
     * current one in every answer (sessionControlInfo); polls must follow it
     * or the queue freezes and is dropped after 120 s ("abandoned"). */
    char session_control_url[256];
    char signaling_url[512];
    char session_token[4096];
    char server_ip[128];
    char media_ip[128];
    int media_port;
    int session_status;
    int queue_position;
    int queue_session_position;
    int queue_seat_position;
    int queue_root_position;
    /* seatSetupInfo.seatSetupStep: NVIDIA reuses queuePosition for setup
     * stages, so a position is only trusted while this step is unchanged. */
    int seat_setup_step;
    int queue_step;
    int queue_best;
    /* NVIDIA's own number before the lock above (for the hidden-queue check). */
    int queue_reported;
    int64_t next_session_poll_at;
    /* Since when session polls have only got 502/503/504/429, and how many:
     * after a while the session counts as failed instead of retrying on. */
    int64_t poll_fail_since;
    unsigned poll_failures;
    /* A launch refused because another session holds the slot: the UI asks
     * whether to resume or end it (gfn_claim_conflict / gfn_end_conflict). */
    bool conflict_found;
    GfnConflict conflict;
    /* Nothing could be closed: NVIDIA releases the slot by itself within
     * minutes, so the UI retries the launch on a timer. */
    bool limit_wait;
    /* Set by the caller for one launch: a refusal waits (limit_wait)
     * instead of asking about the conflict again. */
    bool limit_quiet;
    /* The session in the way answers 404 to this client: only NVIDIA can
     * close it now (made by an older Kasumi or another GeForce NOW app). */
    bool limit_unclosable;
    /* The launch met NVIDIA's rate limit (429): the UI waits it out longer. */
    bool limit_rate;
    /* Why the last launch or session failed, short and fixed ("limit",
     * "abandoned", "entitlement"...), for the launch stats. */
    char fail_code[16];
    /* session.errorCode when NVIDIA ended the session ("1" = a clean end). */
    char end_error_code[24];
    /* Queue ads NVIDIA asked for (free accounts), and those answered. */
    bool ads_required;
    unsigned ads_answered;
    char ads_pending[4][64];
    unsigned ads_pending_count;
    /* Status 4/5: NVIDIA paused the session (the stream dropped); a RESUME
     * brings it back. While `resuming_until` runs, those states count as
     * setup instead of an error. */
    bool session_paused;
    int64_t resuming_until;
} GfnClient;

void gfn_client_init(GfnClient *client);
/* `provider_choice`: a provider code, or "" for NVIDIA's recommendation for
 * this country (Settings > Account > Provider). */
bool gfn_begin_login(GfnClient *client, const char *provider_choice);
void gfn_tick(GfnClient *client);
bool gfn_fetch_library(GfnClient *client);
/* The owned library as last fetched, from the SD card (instant, offline). */
bool gfn_library_load(GfnClient *client);
/* The account's library as last loaded (any thread): whether it is known
 * yet and how many games it has, and whether a game (any store) is in it. */
bool gfn_library_known(unsigned *games);
bool gfn_in_library(const GfnGame *game);
/* Measure Wi-Fi, latency and throughput to NVIDIA (a few seconds). */
bool gfn_connection_test(GfnClient *client);
/* Remember the running session on the SD card (cleared when it stops), so a
 * crash or power loss can resume it on the next start. */
void gfn_active_save(const GfnClient *client, const GfnGame *game);
bool gfn_active_exists(void);
/* An NVIDIA login is saved on the SD card (any service selected). */
bool gfn_login_saved(void);
/* Ask NVIDIA whether the remembered session still runs; if so the client
 * takes it over (queued, setting up or ready) and resume_found is set. */
bool gfn_resume_check(GfnClient *client);
bool gfn_search_catalog(GfnClient *client, const char *query);
bool gfn_start_session(GfnClient *client, const GfnGame *game);
void gfn_session_tick(GfnClient *client);
bool gfn_stop_session(GfnClient *client);
bool gfn_session_active(const GfnClient *client);
/* The conflicting session: take it over (RESUME on its rig, then poll it to
 * ready), or end it and launch `game` (limit_wait when it cannot be ended). */
bool gfn_claim_conflict(GfnClient *client);
bool gfn_end_conflict(GfnClient *client, const GfnGame *game);
/* After a dropped stream: ask NVIDIA about the session and RESUME it if it
 * was paused. True while it is still ours (signalling follows once ready). */
bool gfn_recover_session(GfnClient *client, const GfnGame *game);
bool gfn_has_session(const GfnClient *client);
const char *gfn_bearer_token(const GfnClient *client);
/* The cloud service changed (xcloud_select): forget the other service's
 * login and library in memory and load this one's from the SD card. Both
 * logins stay saved. */
void gfn_client_switch_service(GfnClient *client);
/* Forget tokens in memory and delete the saved login from the SD card. */
void gfn_sign_out(GfnClient *client);
/* Renews the NVIDIA login if it runs out within ten minutes. */
bool gfn_keep_login(GfnClient *client);

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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
    /* Every store this game can launch from; app_id/store above are the
     * one the account selected. */
    struct {
        char id[16];
        char store[16];
    } variants[GFN_MAX_VARIANTS];
    unsigned variant_count;
    unsigned variant_selected;
} GfnGame;

typedef struct {
    GfnAuthState auth_state;
    char status[160];
    char user_code[32];
    char verification_uri[256];
    char device_code[1024];
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
    int64_t next_session_poll_at;
} GfnClient;

void gfn_client_init(GfnClient *client);
bool gfn_begin_login(GfnClient *client);
void gfn_tick(GfnClient *client);
bool gfn_fetch_library(GfnClient *client);
/* The owned library as last fetched, from the SD card (instant, offline). */
bool gfn_library_load(GfnClient *client);
/* Measure Wi-Fi, latency and throughput to NVIDIA (a few seconds). */
bool gfn_connection_test(GfnClient *client);
/* Remember the running session on the SD card (cleared when it stops), so a
 * crash or power loss can resume it on the next start. */
void gfn_active_save(const GfnClient *client, const GfnGame *game);
bool gfn_active_exists(void);
/* Ask NVIDIA whether the remembered session still runs; if so the client
 * takes it over (queued, setting up or ready) and resume_found is set. */
bool gfn_resume_check(GfnClient *client);
bool gfn_search_catalog(GfnClient *client, const char *query);
bool gfn_start_session(GfnClient *client, const GfnGame *game);
void gfn_session_tick(GfnClient *client);
bool gfn_stop_session(GfnClient *client);
bool gfn_session_active(const GfnClient *client);
bool gfn_has_session(const GfnClient *client);
const char *gfn_bearer_token(const GfnClient *client);
/* Forget tokens in memory and delete the saved login from the SD card. */
void gfn_sign_out(GfnClient *client);

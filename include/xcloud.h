#pragma once

/* Xbox Cloud Gaming (xCloud), offered as one more provider next to NVIDIA
 * and the GeForce NOW partners (provider code "XBOX").
 *
 * Sign-in is Microsoft's device code (microsoft.com/link), then plain HTTPS:
 * Xbox user token -> XSTS for gssv.xboxlive.com -> a streaming token for the
 * region's session service. A session is created, polled until the Xbox is
 * ready, and negotiated over HTTPS (the console sends the WebRTC offer, the
 * service answers; ICE candidates go the same way). Media is ordinary WebRTC
 * (H.264 Baseline, Opus, SCTP data channels) on the same transport as GFN.
 *
 * Xbox is a second cloud service beside GeForce NOW, with its own login
 * (xcloud-login.json) and library cache: switching keeps both signed in.
 * While Xbox is selected its tokens sit in the GfnClient fields GFN uses
 * (access_token: streaming token, refresh_token: Microsoft refresh token,
 * id_token: Microsoft access token), so the screens work unchanged.
 *
 * Everything below xcloud_selected runs on the network worker.
 *
 * Protocol learned from a browser capture (2026-10-07) and the MIT-licensed
 * Greenlight / xbox-xcloud-player / xal-node projects by UnknownSKL. */

#include <stdbool.h>

#include "gfn_client.h"
#include "nvst_signal.h"

/* The service the library uses (settings: xbox_service); any thread. */
void xcloud_select(bool xbox);
bool xcloud_selected(void);

/* The saved Xbox login into the client (false: none). */
bool xcloud_load_login(GfnClient *client);
bool xcloud_begin_login(GfnClient *client);
/* Sign-in polling (worker idle loop). */
void xcloud_login_tick(GfnClient *client);
/* Renews the streaming token when it runs out within ten minutes. */
bool xcloud_keep_login(GfnClient *client);
/* The account's playable cloud games (entitled ones), names and box art. */
bool xcloud_fetch_library(GfnClient *client);
/* Filters the last fetched library by title (there is no search service). */
bool xcloud_search(GfnClient *client, const char *query);
bool xcloud_start_session(GfnClient *client, const GfnGame *game);
/* Polls the session to ready (and connects it), then keeps it alive. */
void xcloud_session_tick(GfnClient *client);
bool xcloud_stop_session(GfnClient *client);
/* After a dropped stream: still provisioned (so signalling can run again)? */
bool xcloud_recover_session(GfnClient *client);
/* The WebRTC offer/answer and ICE exchange for a ready session; on success
 * the signal holds the answer, the server's candidates and the peer. */
bool xcloud_signal_start(NvstSignal *signal, const GfnClient *client);
/* A Microsoft login is saved (any thread; reads the SD card). */
bool xcloud_login_saved(void);
/* Deletes the saved Xbox login and library. */
void xcloud_sign_out(void);

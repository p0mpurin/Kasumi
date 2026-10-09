#pragma once

/* Steam Link: stream from your own PC running Steam on the same Wi-Fi,
 * offered as a third service next to GeForce NOW and Xbox Cloud Gaming.
 *
 * "Signing in" pairs the console with one PC: Kasumi finds it on the
 * network and shows a PIN, which the player types into Steam's Authorize
 * Device window. The pairing (steam-link.json) holds this console's device
 * id and the PC's secret; nothing leaves the local network.
 *
 * The library is what can be started on the PC: Steam Big Picture, the
 * desktop, and the games played there through Kasumi before (Steam tells
 * the stream what is running). Starting one asks the PC to stream; the PC
 * launches the game itself.
 *
 * While Steam Link is selected its "tokens" are placeholders in GfnClient so
 * the screens work unchanged. Everything below steam_link_selected runs on
 * the network worker unless it says otherwise. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gfn_client.h"
#include "nvst_signal.h"

void steam_link_select(bool steam);
bool steam_link_selected(void);

bool steam_link_load_login(GfnClient *client);
bool steam_link_begin_login(GfnClient *client);
void steam_link_login_tick(GfnClient *client);
bool steam_link_fetch_library(GfnClient *client);
bool steam_link_search(GfnClient *client, const char *query);
bool steam_link_start_session(GfnClient *client, const GfnGame *game);
/* Any thread: stop asking the PC for a stream (B while it starts). */
void steam_link_cancel(void);
bool steam_link_stop_session(GfnClient *client);
/* After a dropped stream: ask the PC to stream again. */
bool steam_link_recover_session(GfnClient *client, const GfnGame *game);
/* Hands the granted stream to the transport (no signalling server). */
bool steam_link_signal_start(NvstSignal *signal, const GfnClient *client);
/* Forgets the PC in use; true (and the client filled in) when another
 * paired PC takes its place. */
bool steam_link_sign_out(GfnClient *client);
/* Streams from another paired PC (index in steam_link_pcs). */
bool steam_link_use_pc(GfnClient *client, unsigned index);
/* The paired PCs' names, the one in use first (any thread). */
unsigned steam_link_pcs(char names[][64], unsigned max);
/* The paired PC's name ("" if none), for the screens; any thread. */
const char *steam_link_host_name(void);
/* The paired PC's name from the SD card, whichever service is selected. */
bool steam_link_saved_pc(char *name, size_t size);

/* From the stream (any thread): the PC is showing a game. Remembered for
 * the library. */
void steam_link_note_activity(int activity, uint64_t gameid, const char *name);

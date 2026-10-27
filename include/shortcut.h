#pragma once

#include <stdbool.h>

#include "gfn_client.h"

/* HOME Menu shortcuts: a small app per game (shortcut/) that jumps into
 * Kasumi, which then launches that game. Kasumi draws each one's icon and
 * banner from the game's cover, installs it, and keeps which shortcut is
 * which game in APP_DATA_DIR/shortcuts.json.
 *
 * Making one takes three steps across threads:
 *   1. UI: shortcut_request(); the worker fetches sharper art (job);
 *   2. UI, each menu frame: shortcut_frame() draws it offscreen, then reads
 *      it back a frame later (once the GPU is done);
 *   3. worker: shortcut_work() builds the CIA and installs it. */

typedef enum {
    SHORTCUT_IDLE,
    SHORTCUT_FETCH,    /* art to download: submit NET_JOB_SHORTCUT */
    SHORTCUT_FETCHING, /* the worker is downloading it */
    SHORTCUT_DRAWING,  /* waiting for the cover, or for the GPU */
    SHORTCUT_READY,    /* art read back: submit NET_JOB_SHORTCUT */
    SHORTCUT_WORKING,  /* the worker is installing or removing it */
} ShortcutState;

/* Where making (or removing) one has got to, for the progress sheet. */
typedef enum {
    SHORTCUT_STEP_FETCH,
    SHORTCUT_STEP_DRAW,
    SHORTCUT_STEP_BUILD,
    SHORTCUT_STEP_INSTALL,
    SHORTCUT_STEP_DONE
} ShortcutStep;

ShortcutStep shortcut_step(void);
/* 0..1000 through the current step (the install's bytes written). */
unsigned shortcut_progress(void);
bool shortcut_removing(void);
/* The last shortcut's banner used the game's own wide art (false: the
 * library had no wide art for it yet, so Kasumi's own design). */
bool shortcut_used_wide_art(void);
/* UI thread, inside a frame: the icon and banner just made (false until
 * they are drawn). Kept until the next shortcut is started. */
bool shortcut_preview_banner(float x, float y, float scale);
bool shortcut_preview_icon(float x, float y, float scale);

/* Why shortcuts can't be made on this copy of Kasumi (shown as a notice),
 * or NULL when they can. */
const char *shortcut_unavailable(void);

/* Installed shortcut for this game (from shortcuts.json). */
bool shortcut_exists(const char *app_id);
/* UI thread: make (or remake) one for `game`, launching store `variant`. */
bool shortcut_request(const GfnGame *game, unsigned variant);
/* UI thread: queue removing this game's shortcut (then NET_JOB_SHORTCUT). */
bool shortcut_request_remove(const char *app_id);
ShortcutState shortcut_state(void);
/* UI thread: why drawing gave up, once (NULL when there is nothing new). */
const char *shortcut_take_failure(void);
/* UI thread, inside a menu frame (after ui_frame_begin). */
void shortcut_frame(void);
/* UI thread, after ui_frame_end: frees what shortcut_frame is done with. */
void shortcut_frame_end(void);
/* Worker thread (NET_JOB_SHORTCUT): install or remove. */
bool shortcut_work(void);
/* UI thread, after the job: the result for the player. */
const char *shortcut_result(void);
void shortcut_job_done(void);

/* Startup: Kasumi was opened by a shortcut. Fills the game to launch and its
 * store; false for a normal start. Call once. */
bool shortcut_take_launch(GfnGame *game, unsigned *variant);

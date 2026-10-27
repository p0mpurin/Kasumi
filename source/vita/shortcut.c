#include "shortcut.h"

/* HOME Menu shortcuts are a 3DS feature (a small installed app per game).
 * The Vita's equivalent, a LiveArea bubble per game, needs the system's
 * package installer, so for now there are none and the menus say so. */

ShortcutStep shortcut_step(void) { return SHORTCUT_STEP_DONE; }
unsigned shortcut_progress(void) { return 0; }
bool shortcut_removing(void) { return false; }
bool shortcut_used_wide_art(void) { return false; }

bool shortcut_preview_banner(float x, float y, float scale)
{
    (void)x; (void)y; (void)scale;
    return false;
}

bool shortcut_preview_icon(float x, float y, float scale)
{
    (void)x; (void)y; (void)scale;
    return false;
}

const char *shortcut_unavailable(void) { return "Shortcuts aren't available on the Vita yet"; }
bool shortcut_exists(const char *app_id) { (void)app_id; return false; }

bool shortcut_request(const GfnGame *game, unsigned variant)
{
    (void)game; (void)variant;
    return false;
}

bool shortcut_request_remove(const char *app_id) { (void)app_id; return false; }
ShortcutState shortcut_state(void) { return SHORTCUT_IDLE; }
const char *shortcut_take_failure(void) { return NULL; }
void shortcut_frame(void) {}
void shortcut_frame_end(void) {}
bool shortcut_work(void) { return false; }
const char *shortcut_result(void) { return ""; }
void shortcut_job_done(void) {}

bool shortcut_take_launch(GfnGame *game, unsigned *variant)
{
    (void)game; (void)variant;
    return false;
}

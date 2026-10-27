#include "render.h"
#include "platform.h"
#include "diagnostic.h"
#include "mvd_video.h"
#include "perf_stats.h"
#include "screenshot.h"
#include "ui.h"
#include "ui_vita.h"
#include "video_vita.h"
#include "webrtc_transport.h"

#include "app_paths.h"

#include <math.h>
#include <stdio.h>
#include <sys/stat.h>
#include <vita2d.h>

/* The Vita's one 960x544 screen. screens.c lays out every view for it and
 * reads touches in screen pixels; in a game the picture goes underneath
 * with the whole screen to itself. */

/* Where screens.c puts the game's keyboard (twice its 3DS size). */
#define KEYBOARD_X 160
#define KEYBOARD_Y 32
#define KEYBOARD_SCALE 2

static unsigned g_shown_frames;
static bool g_screenshot_requested;
static const char *g_notice;
static unsigned g_loop_max_ms, g_loop_slow;
/* What the last frame showed, for render_map_touch. */
static bool g_keyboard;

static void draw_video(void)
{
    bool fresh = false;
    unsigned w = 0, h = 0;
    vita2d_texture *frame = video_vita_take_frame(&fresh, &w, &h);
    if (!frame || !w || !h) return;
    if (fresh) {
        ++g_shown_frames;
        if (g_screenshot_requested) {
            g_screenshot_requested = false;
            const unsigned *pixels;
            unsigned pw, ph, stride;
            const bool ok = video_vita_frame_pixels(&pixels, &pw, &ph, &stride) &&
                            screenshot_capture_rgba(pixels, pw, ph, stride);
            g_notice = ok ? "Saving screenshot..." : "Screenshot failed";
        }
    }
    float cx, cy, cw, ch;
    mvd_video_view_crop(&cx, &cy, &cw, &ch);
    /* As large as fits, centred, shape kept; the zoom only narrows the
     * part of the frame drawn. */
    float scale = UI_SCREEN_WIDTH / (float)w;
    if (UI_SCREEN_HEIGHT / (float)h < scale) scale = UI_SCREEN_HEIGHT / (float)h;
    const float dw = (float)w * scale, dh = (float)h * scale;
    const float x = floorf((UI_SCREEN_WIDTH - dw) / 2.0f), y = floorf((UI_SCREEN_HEIGHT - dh) / 2.0f);
    vita2d_draw_texture_part_scale(frame, x, y, cx * (float)w, cy * (float)h, cw * (float)w, ch * (float)h,
                                   dw / (cw * (float)w), dh / (ch * (float)h));
}

/* The layout check: while APP_DATA_DIR/ui-shots.txt exists, each screen
 * the menus settle on is saved to APP_DATA_DIR/ui-shots (up to SHOTS_MAX a
 * run), for checking text and spacing against the real screen. */
#define SHOTS_MAX 80
#define SHOT_SETTLE_MS 1100
/* vita2d's display buffers. */
#define FB_STRIDE 1024

static int g_shots = -1;
static unsigned g_shot_key;
static u64 g_shot_key_at;
static bool g_shot_taken;

static unsigned screen_key(const App *app)
{
    unsigned key = (unsigned)app->view;
    const unsigned parts[] = {
        (unsigned)app->modal, app->stream_menu, app->controls_open, app->options_open, app->mapping_open,
        app->pc_sheet_open, (unsigned)(app->settings_section + 1), app->gallery_open, (unsigned)(app->guide_page + 1),
        app->whats_new_open, app->update_open, app->discord_open, gfn_has_session(app->client), app->keyboard_open,
        app->busy != NULL, (unsigned)app->hub_index, (unsigned)app->library_tab, app->search_text[0] != 0,
        (unsigned)(app->settings_section >= 0 ? app->setting_index : 0), (unsigned)app->options_index,
    };
    for (unsigned i = 0; i < sizeof(parts) / sizeof(parts[0]); ++i) key = key * 31u + parts[i];
    return key;
}

static void take_ui_shot(const App *app)
{
    if (g_shots < 0) {
        struct stat st;
        g_shots = stat(APP_DATA_DIR "/ui-shots.txt", &st) == 0 ? 0 : SHOTS_MAX;
    }
    if (g_shots >= SHOTS_MAX || app->view == VIEW_STREAM) return;
    const unsigned key = screen_key(app);
    if (key != g_shot_key) {
        g_shot_key = key;
        g_shot_key_at = plat_time_ms();
        g_shot_taken = false;
        return;
    }
    if (g_shot_taken || plat_time_ms() - g_shot_key_at < SHOT_SETTLE_MS) return;
    vita2d_wait_rendering_done();
    char name[48];
    snprintf(name, sizeof(name), "%02d-view%d-%08x.png", g_shots, (int)app->view, key);
    if (screenshot_capture_ui(vita2d_get_current_fb(), (unsigned)UI_SCREEN_WIDTH, (unsigned)UI_SCREEN_HEIGHT, FB_STRIDE,
                              name)) {
        g_shot_taken = true;
        ++g_shots;
    }
}

void render(const App *app, bool draw_ui)
{
    (void)draw_ui;
    const bool streaming = app->view == VIEW_STREAM;
    g_keyboard = streaming && app->keyboard_open;
    ui_frame_begin();
    if (streaming) {
        /* Frames are only looked at under the transport lock: the decoder
         * can be rebuilt from the media thread for a new stream size. */
        webrtc_transport_lock();
        draw_video();
        webrtc_transport_unlock();
    }
    screens_draw_top(app);
    ui_screen();
    ui_frame_finish();
    take_ui_shot(app);
    ui_frame_end();
}

PlatTouch render_map_touch(PlatTouch touch)
{
    if (!g_keyboard) return touch;
    /* The keyboard reads touches in its own 320x240. */
    PlatTouch out = touch;
    out.x = (s16)floorf((float)(touch.x - KEYBOARD_X) / KEYBOARD_SCALE);
    out.y = (s16)floorf((float)(touch.y - KEYBOARD_Y) / KEYBOARD_SCALE);
    return out;
}

unsigned render_shown_frames(void) { return g_shown_frames; }
void render_request_screenshot(void) { g_screenshot_requested = true; }

const char *render_take_notice(void)
{
    const char *notice = g_notice;
    g_notice = NULL;
    return notice;
}

void render_note_loop(unsigned loop_ms)
{
    if (loop_ms > g_loop_max_ms) g_loop_max_ms = loop_ms;
    if (loop_ms > 25) ++g_loop_slow;
}

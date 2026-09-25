#include <3ds.h>

#include <malloc.h>
#include <poll.h>
#include <stdarg.h>
#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"
#include "app_paths.h"
#include "audio_output.h"
#include "diagnostic.h"
#include "game_art.h"
#include "game_prefs.h"
#include "updater.h"
#include "play_history.h"
#include "screenshot.h"
#include "zoom_zones.h"
#include "gfn_client.h"
#include "gfn_input.h"
#include "http_client.h"
#include "mvd_video.h"
#include "net_worker.h"
#include "nvst_signal.h"
#include "remote_keyboard.h"
#include "settings.h"
#include "stream_profile.h"
#include "ui.h"
#include "webrtc_transport.h"

#define SOC_BUFFER_SIZE (0x100000)
#define SOC_BUFFER_ALIGNMENT (0x1000)
#define MENU_COMBO_HOLD_MS 800

static u32 *g_soc_buffer;
static bool g_soc_ready;
static bool g_ac_ready;
static bool g_ptm_ready;
/* Keep the bounded token/catalog state out of the small 3DSX main stack. */
GfnClient g_client;
NvstSignal g_signal;
static WebRtcTransport g_transport;
static App g_app;
static GfnGame g_current_game;
static SwkbdState g_search_keyboard;
static bool g_quit;
static const char *g_busy_message;
static bool g_leave_pending;
static bool g_screenshot_requested;
static void show_notice(const char *text);
/* Main-loop health while streaming: the longest iteration and how many took
 * over 25 ms (a stalled loop delays packets, input and presents alike). */
static unsigned g_loop_max_ms, g_loop_slow;
static char g_notice[96];
static u64 g_notice_until;

/* Pointer-mode state: A holds the mouse button, taps on the pad click. */
static bool g_mouse_a_held;
static u64 g_mouse_a_min_release_at;
static bool g_mouse_tap_held;
static u64 g_mouse_tap_release_at;
static bool g_touchpad_active;
static unsigned g_touchpad_start_x, g_touchpad_start_y;
static unsigned g_touchpad_last_x, g_touchpad_last_y;
static u64 g_touchpad_started_at;
static unsigned g_touchpad_travel;
static u64 g_combo_started_at;

/* ---- Services ------------------------------------------------------------ */

static void shutdown_services(void)
{
    webrtc_transport_close(&g_transport);
    nvst_signal_close(&g_signal);
    diagnostic_close();
    http_global_exit();
    if (g_soc_ready) socExit();
    free(g_soc_buffer);
    if (g_ptm_ready) ptmuExit();
    if (g_ac_ready) acExit();
}

static bool init_services(char *error, size_t error_size)
{
    Result result = acInit();
    if (R_SUCCEEDED(result)) g_ac_ready = true;
    else {
        snprintf(error, error_size, "ac:u init 0x%08lX", (unsigned long)result);
        return false;
    }
    g_ptm_ready = R_SUCCEEDED(ptmuInit());
    g_soc_buffer = memalign(SOC_BUFFER_ALIGNMENT, SOC_BUFFER_SIZE);
    if (!g_soc_buffer) {
        snprintf(error, error_size, "1 MiB SOC buffer allocation failed");
        return false;
    }
    result = socInit(g_soc_buffer, SOC_BUFFER_SIZE);
    if (R_FAILED(result)) {
        snprintf(error, error_size, "soc:u init 0x%08lX", (unsigned long)result);
        return false;
    }
    g_soc_ready = true;
    if (!http_global_init()) {
        snprintf(error, error_size, "libcurl global init failed");
        return false;
    }
    return true;
}

/* ---- Rendering ----------------------------------------------------------- */

static void present_video(void)
{
    static unsigned last_frame;
    const unsigned frame = mvd_video_decoded_frames();
    if (frame == last_frame) return;
    last_frame = frame;
    /* MVD frames are copied into the single-buffered top framebuffer by the
     * CPU; flush them to memory and re-present that buffer. */
    u16 width = 0, height = 0;
    u8 *framebuffer = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, &width, &height);
    if (framebuffer) GSPGPU_FlushDataCache(framebuffer, (u32)width * height * 2);
    gfxScreenSwapBuffers(GFX_TOP, false);
}

/* Frame pacing for wide video. The top LCD refreshes at ~60 Hz and the
 * stream is 30 fps, so a frame is shown for exactly two vblanks. Frames are
 * shown in decode order; up to two wait in reserve to absorb network jitter,
 * a late frame repeats the previous one, and beyond two queued the oldest is
 * skipped so latency cannot grow. Presentation is double-buffered, so each
 * frame appears whole at a vblank (no tearing). */
static void render_wide_video(bool draw_bottom)
{
    static u32 last_seen_vblank, last_present_vblank;
    static u64 present_ticks, present_max;
    static unsigned presented, repeated, skipped, drained, last_log_frames;
    static unsigned backlog_streak, depth_sum;
    /* Spare frames kept against late arrivals. Build 64 logged ~2 frames a
     * second arriving 50-140 ms after the previous one (Wi-Fi bunching),
     * which one spare (33 ms) cannot cover. Two repeats within 20 s raise
     * the reserve to two spares (+33 ms delay); a calm minute lowers it. */
    static unsigned reserve = 1;
    static u64 last_repeat_at;
    /* Surplus frames (the 30.00 vs 29.92 fps drift) are not dropped the
     * moment they are confirmed: the pacer waits up to ~3 s for a frame
     * whose encoded size says little moved, and drops that one instead. */
    static unsigned drop_wait, quiet_drops;
    static float avg_bytes;
    bool present = false;
    /* citro3d counts top-screen vblanks in its own GSP handler (only one
     * handler per event is allowed, so we must not register another). */
    const u32 vblank = C3D_FrameCounter(0);
    if (vblank != last_seen_vblank) {
        const u32 since = vblank - last_present_vblank;
        last_seen_vblank = vblank;
        const u64 now_ms = osGetTime();
        if (reserve > 1 && now_ms - last_repeat_at >= 60000) reserve = 1;
        unsigned ready = mvd_video_ready_frames();
        while (ready > reserve + 3) {
            mvd_video_skip_oldest_frame();
            --ready;
            ++skipped;
        }
        /* Strict two-vblank cadence: a frame is never shown for only one
         * refresh (that reads as a hitch too); overflow is trimmed above. */
        if (ready && since >= 2) {
            present = true;
        } else if (!ready && since == 2) {
            ++repeated;
            if (last_repeat_at && now_ms - last_repeat_at < 20000) reserve = 2;
            last_repeat_at = now_ms;
        }
        /* The spare frames must stay. NVIDIA sends 30.00 fps but the 3DS
         * LCD shows 29.92, so one surplus frame builds up every ~12 s and a
         * late frame leaves one behind too. Only a frame beyond the reserve
         * that has sat for a whole second is dropped (build 62 dropped the
         * reserve itself every ~4 s and was choppy). */
        if (present) {
            depth_sum += ready;
            const float bytes = (float)mvd_video_ready_frame_bytes(0);
            avg_bytes = avg_bytes > 0.0f ? avg_bytes * 0.95f + bytes * 0.05f : bytes;
            backlog_streak = ready >= reserve + 2 ? backlog_streak + 1 : 0;
            if (backlog_streak >= 30 || drop_wait) {
                /* Candidates exclude the frame about to be shown. */
                unsigned best = 0;
                size_t best_bytes = (size_t)-1;
                for (unsigned i = 1; i < ready; ++i) {
                    const size_t b = mvd_video_ready_frame_bytes(i);
                    if (b && b < best_bytes) { best_bytes = b; best = i; }
                }
                ++drop_wait;
                const bool quiet = best && (float)best_bytes <= avg_bytes * 0.7f;
                if (ready < reserve + 2) {
                    drop_wait = 0; /* the surplus went away on its own */
                } else if (best && (quiet || drop_wait >= 150)) {
                    mvd_video_skip_ready_frame(best);
                    if (quiet) ++quiet_drops;
                    ++drained;
                    drop_wait = 0;
                }
                backlog_streak = 0;
            }
        }
    }
    if (!present && !draw_bottom) return;
    const u64 start = svcGetSystemTick();
    const void *frame = present ? mvd_video_take_gpu_frame() : NULL;
    if (frame && g_screenshot_requested) {
        g_screenshot_requested = false;
        show_notice(screenshot_capture(frame) ? "Saving screenshot..." : "Screenshot failed");
    }
    if (frame) {
        ui_video_upload(frame);
        mvd_video_release_gpu_frame();
    }
    ui_frame_begin(false);
    if (frame) {
        ui_begin_top_video();
        ui_draw_video();
    }
    if (draw_bottom) {
        ui_begin_bottom();
        screens_draw_bottom(&g_app);
    }
    ui_frame_end();
    if (!frame) return;
    last_present_vblank = vblank;
    const u64 ticks = svcGetSystemTick() - start;
    present_ticks += ticks;
    if (ticks > present_max) present_max = ticks;
    if (++presented - last_log_frames >= 120) {
        const u64 per_us = SYSCLOCK_ARM11 / 1000000u;
        const unsigned frames = presented - last_log_frames;
        diagnostic_log("VIDEO", "paced present frames=%u avg/max=%llu/%llu us repeated=%u skipped=%u drained=%u quiet=%u depth=%u.%02u reserve=%u loopMax=%u slow=%u",
                       frames,
                       (unsigned long long)(present_ticks / frames / per_us),
                       (unsigned long long)(present_max / per_us), repeated, skipped, drained,
                       quiet_drops, depth_sum / frames, depth_sum * 100 / frames % 100, reserve,
                       g_loop_max_ms, g_loop_slow);
        g_loop_max_ms = g_loop_slow = 0;
        present_ticks = present_max = 0;
        repeated = skipped = drained = quiet_drops = depth_sum = 0;
        last_log_frames = presented;
    }
}

static void render(bool draw_bottom)
{
    const bool video = g_app.view == VIEW_STREAM;
    if (video && mvd_video_wide()) {
        render_wide_video(draw_bottom);
        return;
    }
    if (video && !draw_bottom) {
        present_video();
        return;
    }
    /* Menus sync to vblank; the stream never waits for it, so video and
     * input keep flowing while the lower screen redraws. */
    ui_frame_begin(!video);
    if (!video) {
        ui_begin_top();
        screens_draw_top(&g_app);
    }
    ui_begin_bottom();
    screens_draw_bottom(&g_app);
    ui_frame_end();
    if (video) present_video();
}

static void show_notice(const char *text)
{
    snprintf(g_notice, sizeof(g_notice), "%s", text);
    g_notice_until = osGetTime() + 4000;
}

/* Hand a blocking call to the network worker; the UI keeps animating. */
static bool submit_job(NetJobKind kind, const char *busy, const char *text, const GfnGame *game)
{
    if (!net_worker_submit(kind, text, game)) {
        show_notice("Still working on the last request");
        return false;
    }
    g_busy_message = busy;
    return true;
}

static void open_modal(AppModal modal, const char *jp, const char *title, const char *text)
{
    g_app.modal = modal;
    snprintf(g_app.modal_jp, sizeof(g_app.modal_jp), "%s", jp);
    snprintf(g_app.modal_title, sizeof(g_app.modal_title), "%s", title);
    snprintf(g_app.modal_text, sizeof(g_app.modal_text), "%s", text);
}

/* ---- View model ---------------------------------------------------------- */

static AppView derive_view(void)
{
    if (webrtc_transport_gameplay_ready(&g_transport) && mvd_video_active() &&
        mvd_video_decoded_frames() > g_app.stream_frame_base)
        return VIEW_STREAM;
    if (gfn_session_active(&g_client) || nvst_signal_active(&g_signal) || g_transport.peer)
        return VIEW_SESSION;
    if (g_client.auth_state == GFN_AUTH_WAITING) return VIEW_LOGIN;
    if (g_app.settings_open) return VIEW_SETTINGS;
    if (!gfn_has_session(&g_client)) return VIEW_WELCOME;
    if (g_app.details_open && g_app.selected < g_app.list_count) return VIEW_DETAILS;
    return VIEW_LIBRARY;
}

static void refresh_device_status(void)
{
    static u64 last;
    const u64 now = osGetTime();
    if (last && now - last < 1000) return;
    last = now;
    g_app.wifi_bars = osGetWifiStrength();
    u8 level = 5, charging = 0;
    if (g_ptm_ready) {
        PTMU_GetBatteryLevel(&level);
        PTMU_GetBatteryChargeState(&charging);
    }
    g_app.battery_level = level;
    g_app.charging = charging != 0;

    static unsigned last_frames;
    const unsigned frames = mvd_video_decoded_frames();
    g_app.fps = frames >= last_frames ? frames - last_frames : 0;
    last_frames = frames;

    static unsigned last_resent;
    const unsigned resent = webrtc_transport_resent_packets(&g_transport);
    g_app.resent_per_second = resent >= last_resent ? resent - last_resent : 0;
    last_resent = resent;
}

static void keep_selection_visible(void)
{
    if (g_app.selected >= g_app.list_count)
        g_app.selected = g_app.list_count ? g_app.list_count - 1 : 0;
    if (g_app.selected < g_app.list_top) g_app.list_top = g_app.selected;
    if (g_app.selected >= g_app.list_top + LIBRARY_ROWS)
        g_app.list_top = g_app.selected - LIBRARY_ROWS + 1;
}

/* ---- Actions ------------------------------------------------------------- */

static void begin_login(void)
{
    submit_job(NET_JOB_BEGIN_LOGIN, "Requesting a sign-in code from NVIDIA...", NULL, NULL);
}

static void load_library(void)
{
    g_app.search_text[0] = '\0';
    submit_job(NET_JOB_LOAD_LIBRARY, g_client.library_saved_at
               ? "Refreshing your library..." : "Loading your GeForce NOW library...", NULL, NULL);
}

/* Back from a search to the saved library: read from the SD card. */
static void show_saved_library(void)
{
    g_app.search_text[0] = '\0';
    submit_job(NET_JOB_LIBRARY_CACHED, NULL, NULL, NULL);
}

static void search_catalog(void)
{
    char text[sizeof(g_app.search_text)];
    snprintf(text, sizeof(text), "%s", g_app.search_text);
    swkbdInit(&g_search_keyboard, SWKBD_TYPE_QWERTY, 2, 64);
    swkbdSetHintText(&g_search_keyboard, "Search GeForce NOW games");
    swkbdSetButton(&g_search_keyboard, SWKBD_BUTTON_LEFT, "Cancel", false);
    swkbdSetButton(&g_search_keyboard, SWKBD_BUTTON_RIGHT, "Search", true);
    swkbdSetValidation(&g_search_keyboard, SWKBD_NOTEMPTY_NOTBLANK, 0, 0);
    swkbdSetFeatures(&g_search_keyboard, SWKBD_DARKEN_TOP_SCREEN | SWKBD_PREDICTIVE_INPUT);
    if (text[0]) swkbdSetInitialText(&g_search_keyboard, text);
    if (swkbdInputText(&g_search_keyboard, text, sizeof(text)) != SWKBD_BUTTON_RIGHT) return;
    snprintf(g_app.search_text, sizeof(g_app.search_text), "%s", text);
    submit_job(NET_JOB_SEARCH, "Searching the GeForce NOW catalog...", g_app.search_text, NULL);
}

static void launch_game(const GfnGame *game)
{
    if (game != &g_current_game) g_current_game = *game;
    snprintf(g_app.game_title, sizeof(g_app.game_title), "%s", g_current_game.title);
    snprintf(g_app.game_store, sizeof(g_app.game_store), "%s", g_current_game.store);
    g_app.genshin_session = strstr(g_current_game.title, "Genshin") != NULL;
    g_app.stream_started_at = 0;
    g_app.zone_index = -1;
    g_app.sound_muted = false;
    zoom_zones_select(g_current_game.app_id);
    g_app.free_tier_guess = false;
    g_app.reconnect_attempt = 0;
    g_app.controls_open = false;
    g_app.stream_frame_base = mvd_video_decoded_frames();
    settings_apply_picture(&g_app.settings);
    diagnostic_log("VIDEO", "launch profile=%s %ux%u@30 initial=%u min=%u max=%u dynamic=%u sharpen=%d",
                   stream_profile_name(), stream_profile_width(), stream_profile_height(),
                   stream_profile_initial_bitrate(), stream_profile_min_bitrate(),
                   stream_profile_max_bitrate(), stream_profile_dynamic_mode(),
                   stream_profile_sharpen());
    diagnostic_checkpoint();
    submit_job(NET_JOB_START_SESSION, "Creating your cloud session...", NULL, &g_current_game);
}

static void release_stream_input(void)
{
    if (g_mouse_a_held || g_mouse_tap_held) webrtc_transport_mouse_button(&g_transport, false);
    g_mouse_a_held = g_mouse_tap_held = g_touchpad_active = false;
    gfn_input_set_virtual_buttons(0);
    gfn_input_set_suppressed(false);
    g_app.stream_menu = false;
    g_app.keyboard_open = false;
}

static void close_media(void)
{
    release_stream_input();
    webrtc_transport_close(&g_transport);
    if (!net_worker_signal_starting()) nvst_signal_close(&g_signal);
}

static void leave_session(void)
{
    diagnostic_log("APP", "user left the session");
    close_media();
    /* If the worker is mid-request, cancel it and stop once it is free. */
    if (net_worker_busy()) {
        net_worker_cancel();
        g_leave_pending = true;
        return;
    }
    g_leave_pending = false;
    submit_job(NET_JOB_STOP_SESSION, "Closing the cloud session...", NULL, NULL);
}

static void retry_session(void)
{
    close_media();
    g_app.stream_frame_base = mvd_video_decoded_frames();
    if (g_client.session_state == GFN_SESSION_READY)
        submit_job(NET_JOB_START_SIGNAL, "Reconnecting to the cloud rig...", NULL, NULL);
    else if (g_current_game.app_id[0])
        submit_job(NET_JOB_RESTART_SESSION, "Restarting your cloud session...", NULL, &g_current_game);
}

static void save_settings(void)
{
    settings_apply_input(&g_app.settings);
    if (!settings_save(&g_app.settings))
        show_notice("Settings could not be saved to SD");
}

/* ---- Software update ------------------------------------------------------- */

static char g_whats_new_notes[1600];

static void start_update_check(bool quiet)
{
    if (gfn_session_active(&g_client)) return;
    submit_job(NET_JOB_UPDATE_CHECK, quiet ? NULL : "Checking for updates...",
               g_app.settings.update_beta ? "beta" : "stable", NULL);
}

static void open_updates(void)
{
    g_app.update_open = true;
    g_app.notes_scroll = 0;
    const UpdateInfo info = updater_info();
    if (info.state == UPDATE_IDLE || info.state == UPDATE_UP_TO_DATE || info.state == UPDATE_FAILED)
        start_update_check(true);
}

static void handle_updates(u32 down, u32 repeat, AppAction action)
{
    const UpdateInfo info = updater_info();
    const bool working = info.state == UPDATE_CHECKING || info.state == UPDATE_DOWNLOADING ||
                         info.state == UPDATE_VERIFYING || info.state == UPDATE_INSTALLING;
    if (repeat & KEY_UP) g_app.notes_scroll = g_app.notes_scroll > 0 ? g_app.notes_scroll - 1 : 0;
    if (repeat & KEY_DOWN) ++g_app.notes_scroll;
    /* Never leave mid-install: the page shows its progress to the end. */
    if (((down & KEY_B) || action == ACTION_UPDATE_CLOSE) && !working) {
        g_app.update_open = false;
        return;
    }
    if (((down & KEY_X) || action == ACTION_UPDATE_LATER) && info.state == UPDATE_AVAILABLE) {
        updater_dismiss();
        g_app.update_open = false;
        show_notice("OK - Kasumi will remind you when the next version is out");
        return;
    }
    if (!(down & KEY_A) && action != ACTION_UPDATE_PRIMARY) return;
    switch (info.state) {
    case UPDATE_AVAILABLE:
        if (gfn_session_active(&g_client)) {
            show_notice("Finish your game first - updates never run during a session");
            break;
        }
        diagnostic_log("UPDATE", "install %s requested", info.latest);
        submit_job(NET_JOB_UPDATE_INSTALL, NULL, NULL, NULL);
        break;
    case UPDATE_INSTALLED:
        /* The CIA relaunches into the new version; a .3dsx is reopened by hand. */
        updater_relaunch();
        g_quit = true;
        break;
    case UPDATE_CHECKING: case UPDATE_DOWNLOADING: case UPDATE_VERIFYING: case UPDATE_INSTALLING:
        break;
    default:
        start_update_check(false);
        break;
    }
}

static void handle_whats_new(u32 down, u32 repeat, AppAction action)
{
    if (repeat & KEY_UP) g_app.notes_scroll = g_app.notes_scroll > 0 ? g_app.notes_scroll - 1 : 0;
    if (repeat & KEY_DOWN) ++g_app.notes_scroll;
    if ((down & (KEY_A | KEY_B | KEY_START)) || action == ACTION_WHATS_NEW_CLOSE)
        g_app.whats_new_open = false;
}

/* Quiet daily check from the menus; never during a session. */
static void auto_update_check(void)
{
    static u64 started_at;
    if (!started_at) started_at = osGetTime();
    if (!g_app.settings.auto_update || osGetTime() - started_at < 4000) return;
    if (g_app.view != VIEW_LIBRARY && g_app.view != VIEW_WELCOME) return;
    if (net_worker_busy() || gfn_session_active(&g_client) || !updater_check_due()) return;
    if (updater_info().state != UPDATE_IDLE) return;
    start_update_check(true);
}

static void change_setting(int direction)
{
    const int index = screens_setting_at(g_app.setting_index);
    if (index == SETTING_ACCOUNT) {
        if (gfn_has_session(&g_client))
            open_modal(MODAL_SIGN_OUT, "サインアウト", "SIGN OUT?",
                       "The saved NVIDIA login will be deleted from the SD card.");
        return;
    }
    if (index == SETTING_CONNECTION) {
        submit_job(NET_JOB_CONNECTION_TEST, "Checking your connection to NVIDIA...", NULL, NULL);
        return;
    }
    if (index == SETTING_GUIDE) {
        g_app.guide_page = 0;
        return;
    }
    if (index == SETTING_UPDATES) {
        open_updates();
        return;
    }
    screens_setting_change(&g_app, index, direction);
    settings_apply_input(&g_app.settings);
    settings_apply_picture(&g_app.settings);
}

/* ---- Pointer mode -------------------------------------------------------- */

static void touchpad_begin(unsigned x, unsigned y)
{
    g_touchpad_active = true;
    g_touchpad_start_x = g_touchpad_last_x = x;
    g_touchpad_start_y = g_touchpad_last_y = y;
    g_touchpad_started_at = osGetTime();
    g_touchpad_travel = 0;
}

static void touchpad_move(unsigned x, unsigned y)
{
    if (!g_touchpad_active) return;
    const int dx = (int)x - (int)g_touchpad_last_x;
    const int dy = (int)y - (int)g_touchpad_last_y;
    g_touchpad_last_x = x;
    g_touchpad_last_y = y;
    g_touchpad_travel += (unsigned)abs(dx) + (unsigned)abs(dy);
    const int from_start = abs((int)x - (int)g_touchpad_start_x) +
                           abs((int)y - (int)g_touchpad_start_y);
    if (from_start <= 4 || (!dx && !dy)) return;
    const unsigned width = g_transport.video_source_width ? g_transport.video_source_width : 960;
    const unsigned height = g_transport.video_source_height ? g_transport.video_source_height : 544;
    webrtc_transport_mouse_move(&g_transport, (int16_t)(dx * (int)width / 320),
                                (int16_t)(dy * (int)height / 192));
}

static void touchpad_end(void)
{
    if (!g_touchpad_active) return;
    g_touchpad_active = false;
    if (g_touchpad_travel > 6 || osGetTime() - g_touchpad_started_at > 450 ||
        g_mouse_a_held || g_mouse_tap_held || !g_transport.pointer_mode ||
        !g_transport.input_ready)
        return;
    if (webrtc_transport_mouse_button(&g_transport, true)) {
        g_mouse_tap_held = true;
        g_mouse_tap_release_at = osGetTime() + 55;
        diagnostic_log("INPUT", "touchpad tap click down");
    }
}

static void update_pointer_click(u32 down, u32 held)
{
    if (g_mouse_tap_held && osGetTime() >= g_mouse_tap_release_at) {
        webrtc_transport_mouse_button(&g_transport, false);
        g_mouse_tap_held = false;
    }
    const bool can_click = g_app.view == VIEW_STREAM && g_transport.pointer_mode &&
                           !g_transport.keyboard_mode && !g_app.stream_menu &&
                           g_transport.input_ready;
    if (g_mouse_a_held && (!can_click || !(held & KEY_A)) &&
        (!can_click || osGetTime() >= g_mouse_a_min_release_at)) {
        webrtc_transport_mouse_button(&g_transport, false);
        g_mouse_a_held = false;
    }
    if (can_click && (down & KEY_A) && !g_mouse_a_held) {
        if (g_mouse_tap_held) {
            webrtc_transport_mouse_button(&g_transport, false);
            g_mouse_tap_held = false;
        }
        g_mouse_a_held = webrtc_transport_mouse_button(&g_transport, true);
        if (g_mouse_a_held) g_mouse_a_min_release_at = osGetTime() + 55;
    }
    if (!can_click) g_touchpad_active = false;
}

/* ---- Input per view ------------------------------------------------------ */

static void handle_modal(u32 down, AppAction action)
{
    const bool confirm = (down & KEY_A) || action == ACTION_CONFIRM || action == ACTION_RETRY;
    const bool dismiss = (down & KEY_B) || action == ACTION_DISMISS;
    if (!confirm && !dismiss) return;
    const AppModal modal = g_app.modal;
    g_app.modal = MODAL_NONE;
    if (dismiss) return;
    if (modal == MODAL_EXIT) {
        g_quit = true;
    } else if (modal == MODAL_SIGN_OUT) {
        submit_job(NET_JOB_SIGN_OUT, NULL, NULL, NULL);
        g_app.settings_open = false;
        g_app.search_text[0] = '\0';
        g_app.selected = g_app.list_top = 0;
    } else if (modal == MODAL_ERROR && g_current_game.app_id[0]) {
        launch_game(&g_current_game);
    }
}

static void handle_welcome(u32 down, AppAction action)
{
    if ((down & KEY_A) || action == ACTION_SIGN_IN) begin_login();
    else if ((down & KEY_SELECT) || action == ACTION_SETTINGS) g_app.settings_open = true;
    else if ((down & KEY_START) || action == ACTION_EXIT)
        open_modal(MODAL_EXIT, "終了", "EXIT KASUMI?", "Return to the HOME Menu.");
}

static void handle_login(u32 down, AppAction action)
{
    if ((down & KEY_Y) || action == ACTION_NEW_CODE) {
        begin_login();
    } else if ((down & KEY_B) || action == ACTION_CANCEL) {
        submit_job(NET_JOB_CANCEL_LOGIN, NULL, NULL, NULL);
    } else if (down & KEY_START) {
        open_modal(MODAL_EXIT, "終了", "EXIT KASUMI?", "Return to the HOME Menu.");
    }
}

/* Has this game (any of its store versions) been played on Kasumi? */
static bool game_history(const GfnGame *game, PlayHistory *out)
{
    if (play_history_get(game->app_id, out)) return true;
    for (unsigned v = 0; v < game->variant_count; ++v)
        if (play_history_get(game->variants[v].id, out)) return true;
    return false;
}

static int64_t g_recent_keys[GFN_MAX_GAMES];

static int compare_recent(const void *a, const void *b)
{
    const int64_t ka = g_recent_keys[*(const unsigned short *)a];
    const int64_t kb = g_recent_keys[*(const unsigned short *)b];
    return ka < kb ? 1 : ka > kb ? -1 : 0;
}

/* The visible library list for the current tab (search results: all). */
static void rebuild_list(void)
{
    const int tab = g_app.search_text[0] ? LIBRARY_TAB_ALL : g_app.library_tab;
    size_t n = 0;
    for (size_t i = 0; i < g_client.game_count; ++i) {
        const GfnGame *game = &g_client.games[i];
        if (tab == LIBRARY_TAB_FAVOURITES && !game_prefs_favourite(game->app_id)) continue;
        if (tab == LIBRARY_TAB_RECENT) {
            PlayHistory history;
            if (!game_history(game, &history)) continue;
            g_recent_keys[i] = history.last_played;
        }
        g_app.list_map[n++] = (unsigned short)i;
    }
    if (tab == LIBRARY_TAB_RECENT) qsort(g_app.list_map, n, sizeof(g_app.list_map[0]), compare_recent);
    g_app.list_count = n;
    keep_selection_visible();
}

static void change_tab(int direction)
{
    if (g_app.search_text[0]) return;
    g_app.library_tab = (g_app.library_tab + LIBRARY_TAB_COUNT + direction) % LIBRARY_TAB_COUNT;
    g_app.selected = g_app.list_top = 0;
    rebuild_list();
}

static void handle_library(u32 down, u32 repeat, AppAction action)
{
    const size_t count = g_app.list_count;
    if (down & KEY_L) change_tab(-1);
    if (down & KEY_R) change_tab(1);
    if (count) {
        if ((repeat & KEY_UP) || action == ACTION_PREV) {
            if (g_app.selected > 0) --g_app.selected;
        }
        if ((repeat & KEY_DOWN) || action == ACTION_NEXT) {
            if (g_app.selected + 1 < count) ++g_app.selected;
        }
        if (repeat & KEY_LEFT)
            g_app.selected = g_app.selected > LIBRARY_ROWS ? g_app.selected - LIBRARY_ROWS : 0;
        if (repeat & KEY_RIGHT)
            g_app.selected = g_app.selected + LIBRARY_ROWS < count ? g_app.selected + LIBRARY_ROWS
                                                                  : count - 1;
        keep_selection_visible();
    }
    if ((down & KEY_A) || action == ACTION_PLAY) {
        if (count && g_app.selected < count) {
            g_app.details_open = true;
            g_app.options_open = false;
            g_app.details_variant = app_game(&g_app, g_app.selected)->variant_selected;
        } else if (!g_client.game_count) {
            load_library();
        } else {
            g_app.library_tab = LIBRARY_TAB_ALL;
            rebuild_list();
        }
    } else if ((down & KEY_X) || action == ACTION_SEARCH) {
        search_catalog();
    } else if ((down & KEY_Y) || action == ACTION_LIBRARY) {
        /* From a search, Y returns to the library; otherwise it refreshes. */
        if (g_app.search_text[0]) show_saved_library();
        else load_library();
    } else if ((down & KEY_B) && g_app.search_text[0]) {
        show_saved_library();
    } else if ((down & KEY_SELECT) || action == ACTION_SETTINGS) {
        g_app.settings_open = true;
    } else if (down & KEY_START) {
        open_modal(MODAL_EXIT, "終了", "EXIT KASUMI?", "Return to the HOME Menu.");
    }
}

/* ---- First-run guide ------------------------------------------------------- */

static void finish_guide(void)
{
    g_app.guide_page = -1;
    if (!g_app.settings.guide_done) {
        g_app.settings.guide_done = true;
        save_settings();
    }
}

static void handle_guide(u32 down, AppAction action)
{
    if ((down & KEY_START) || action == ACTION_GUIDE_SKIP) {
        finish_guide();
    } else if ((down & (KEY_A | KEY_RIGHT)) || action == ACTION_GUIDE_NEXT) {
        if (++g_app.guide_page >= GUIDE_PAGES) finish_guide();
    } else if (((down & (KEY_B | KEY_LEFT)) || action == ACTION_GUIDE_BACK) && g_app.guide_page > 0) {
        --g_app.guide_page;
    }
}

/* The running game's options (its custom button map survives menu toggles). */
static GamePrefs g_session_prefs;

static void reapply_game_map(void)
{
    if (gfn_session_active(&g_client) && g_session_prefs.has_map) gfn_input_set_custom_map(g_session_prefs.map);
}

/* Settings for this game's session: the global ones with its options. */
static void apply_game_options(const GamePrefs *prefs)
{
    AppSettings session = g_app.settings;
    if (prefs->bitrate >= 0 && prefs->bitrate < STREAM_BITRATE_COUNT)
        session.bitrate_mode = (StreamBitrateMode)prefs->bitrate;
    if (prefs->gyro >= 0 && prefs->gyro < GFN_GYRO_MODE_COUNT) session.gyro_mode = (GfnGyroMode)prefs->gyro;
    if (prefs->layout >= 0 && prefs->layout < 2) session.button_layout = (GfnButtonLayout)prefs->layout;
    settings_apply_input(&session);
    settings_apply_picture(&session);
    g_session_prefs = *prefs;
    if (prefs->has_map) gfn_input_set_custom_map(prefs->map);
}

/* Launch the details page's game with the store chosen there. */
static void launch_details_game(void)
{
    const GfnGame *base = app_game(&g_app, g_app.selected);
    if (!base) return;
    GfnGame game = *base;
    const GamePrefs prefs = game_prefs_get(base->app_id);
    if (g_app.details_variant < game.variant_count) {
        snprintf(game.app_id, sizeof(game.app_id), "%s", game.variants[g_app.details_variant].id);
        snprintf(game.store, sizeof(game.store), "%s", game.variants[g_app.details_variant].store);
    }
    launch_game(&game);
    /* launch_game applied the global picture settings; layer this game's. */
    apply_game_options(&prefs);
    diagnostic_log("APP", "game options bitrate=%d gyro=%d layout=%d map=%d", prefs.bitrate, prefs.gyro,
                   prefs.layout, prefs.has_map);
}

/* ---- Button mapping editor ---------------------------------------------- */

static void open_mapping(const GfnGame *game, const GamePrefs *prefs)
{
    (void)game;
    const GfnButtonLayout layout = prefs->layout >= 0 ? (GfnButtonLayout)prefs->layout : g_app.settings.button_layout;
    gfn_input_default_map(layout, g_app.settings.swap_shoulders, g_app.mapping_default);
    memcpy(g_app.mapping, prefs->has_map ? prefs->map : g_app.mapping_default, sizeof(g_app.mapping));
    g_app.mapping_input = GFN_IN_A;
    g_app.mapping_open = true;
}

static void handle_mapping(u32 down, u32 repeat, AppAction action)
{
    const GfnGame *game = app_game(&g_app, g_app.selected);
    if (!game) { g_app.mapping_open = false; return; }
    /* Pressing a 3DS button picks it; the Circle Pad changes what it sends
     * (every button, D-Pad included, is itself remappable). */
    for (unsigned i = 0; i < GFN_INPUT_COUNT; ++i)
        if (down & gfn_input_key(i)) g_app.mapping_input = (int)i;
    unsigned char *out = &g_app.mapping[g_app.mapping_input];
    if ((repeat & KEY_CPAD_LEFT) || action == ACTION_MAP_PREV)
        *out = (unsigned char)((*out + GFN_OUTPUT_COUNT - 1) % GFN_OUTPUT_COUNT);
    if ((repeat & KEY_CPAD_RIGHT) || action == ACTION_MAP_NEXT)
        *out = (unsigned char)((*out + 1) % GFN_OUTPUT_COUNT);
    if (repeat & KEY_CPAD_UP) g_app.mapping_input = (g_app.mapping_input + GFN_INPUT_COUNT - 1) % GFN_INPUT_COUNT;
    if (repeat & KEY_CPAD_DOWN) g_app.mapping_input = (g_app.mapping_input + 1) % GFN_INPUT_COUNT;
    if (action == ACTION_MAP_RESET) memcpy(g_app.mapping, g_app.mapping_default, sizeof(g_app.mapping));
    if (action == ACTION_MAP_CANCEL) g_app.mapping_open = false;
    if (action == ACTION_MAP_DONE) {
        GamePrefs prefs = game_prefs_get(game->app_id);
        prefs.has_map = memcmp(g_app.mapping, g_app.mapping_default, sizeof(g_app.mapping)) != 0;
        memcpy(prefs.map, g_app.mapping, sizeof(prefs.map));
        game_prefs_set(game->app_id, &prefs);
        g_app.mapping_open = false;
        show_notice(prefs.has_map ? "Button mapping saved for this game" : "This game uses the normal layout");
    }
}

static void handle_options(u32 down, u32 repeat, AppAction action)
{
    if (g_app.mapping_open) {
        handle_mapping(down, repeat, action);
        return;
    }
    const GfnGame *game = app_game(&g_app, g_app.selected);
    if (!game) { g_app.options_open = false; return; }
    GamePrefs prefs = game_prefs_get(game->app_id);
    if ((down & KEY_B) || action == ACTION_OPTIONS_CLOSE) { g_app.options_open = false; return; }
    if ((action == ACTION_OPTION_PREV || action == ACTION_OPTION_NEXT) && screens_touched_option_row() >= 0)
        g_app.options_index = screens_touched_option_row();
    if (repeat & KEY_UP) g_app.options_index = (g_app.options_index + OPTION_COUNT - 1) % OPTION_COUNT;
    if (repeat & KEY_DOWN) g_app.options_index = (g_app.options_index + 1) % OPTION_COUNT;
    int step = 0;
    if ((repeat & KEY_LEFT) || action == ACTION_OPTION_PREV) step = -1;
    if ((repeat & KEY_RIGHT) || action == ACTION_OPTION_NEXT || (down & KEY_A)) step = 1;
    if (!step) return;
    /* Each option cycles "Default" (-1) then the setting's own values. */
    #define CYCLE(value, count) value = ((value) + 1 + step + (count) + 1) % ((count) + 1) - 1
    switch (g_app.options_index) {
    case OPTION_BITRATE: CYCLE(prefs.bitrate, STREAM_BITRATE_COUNT); break;
    case OPTION_GYRO: CYCLE(prefs.gyro, GFN_GYRO_MODE_COUNT); break;
    case OPTION_LAYOUT: CYCLE(prefs.layout, 2); break;
    case OPTION_MAPPING:
        if ((down & KEY_A) || action == ACTION_OPTION_NEXT || action == ACTION_OPTION_PREV) open_mapping(game, &prefs);
        return;
    case OPTION_CONNECTION:
        if ((down & KEY_A) || action == ACTION_OPTION_NEXT)
            submit_job(NET_JOB_CONNECTION_TEST, "Checking your connection to NVIDIA...", NULL, NULL);
        return;
    }
    #undef CYCLE
    game_prefs_set(game->app_id, &prefs);
}

static void handle_details(u32 down, u32 repeat, AppAction action)
{
    const GfnGame *game = app_game(&g_app, g_app.selected);
    if (!game) { g_app.details_open = false; return; }
    if (g_app.options_open || action == ACTION_OPTIONS_CLOSE) {
        handle_options(down, repeat, action);
        return;
    }
    const unsigned variants = game->variant_count;
    if ((down & KEY_A) || action == ACTION_DETAILS_PLAY) {
        launch_details_game();
    } else if ((down & KEY_B) || action == ACTION_BACK) {
        g_app.details_open = false;
    } else if ((down & KEY_Y) || action == ACTION_FAVOURITE) {
        GamePrefs prefs = game_prefs_get(game->app_id);
        prefs.favourite = !prefs.favourite;
        game_prefs_set(game->app_id, &prefs);
        show_notice(prefs.favourite ? "Added to favourites" : "Removed from favourites");
    } else if ((down & KEY_X) || action == ACTION_OPTIONS) {
        g_app.options_open = true;
        g_app.options_index = 0;
    } else if (variants > 1 && ((repeat & KEY_LEFT) || action == ACTION_VARIANT_PREV)) {
        g_app.details_variant = (g_app.details_variant + variants - 1) % variants;
    } else if (variants > 1 && ((repeat & KEY_RIGHT) || action == ACTION_VARIANT_NEXT)) {
        g_app.details_variant = (g_app.details_variant + 1) % variants;
    } else if ((repeat & KEY_UP) && g_app.selected > 0) {
        --g_app.selected;
        keep_selection_visible();
        g_app.details_variant = app_game(&g_app, g_app.selected)->variant_selected;
    } else if ((repeat & KEY_DOWN) && g_app.selected + 1 < g_app.list_count) {
        ++g_app.selected;
        keep_selection_visible();
        g_app.details_variant = app_game(&g_app, g_app.selected)->variant_selected;
    }
}

static void handle_settings(u32 down, u32 repeat, AppAction action)
{
    if (repeat & KEY_UP) g_app.setting_index = (g_app.setting_index + SETTING_COUNT - 1) % SETTING_COUNT;
    if (repeat & KEY_DOWN) g_app.setting_index = (g_app.setting_index + 1) % SETTING_COUNT;
    if ((repeat & KEY_LEFT) || action == ACTION_VALUE_PREV) change_setting(-1);
    if ((repeat & KEY_RIGHT) || (down & KEY_A) || action == ACTION_VALUE_NEXT) change_setting(1);
    if ((down & (KEY_B | KEY_SELECT)) || action == ACTION_BACK) {
        if (action == ACTION_BACK && screens_setting_at(g_app.setting_index) == SETTING_ACCOUNT &&
            gfn_has_session(&g_client)) {
            change_setting(1);
            return;
        }
        save_settings();
        g_app.settings_open = false;
    }
}

static void handle_session(u32 down, AppAction action)
{
    const bool failed = g_client.session_state == GFN_SESSION_ERROR ||
                        g_transport.state == WEBRTC_FAILED || g_signal.state == NVST_SIGNAL_ERROR;
    if ((down & KEY_B) || action == ACTION_CANCEL) leave_session();
    else if (failed && ((down & KEY_A) || action == ACTION_RETRY)) {
        g_app.reconnect_attempt = 0;
        retry_session();
    }
}


/* Stream menu "zone": save the current zoom, or clear the game's zones. */
static void menu_zone(void)
{
    if (mvd_video_zoomed()) {
        unsigned x = 50, y = 50;
        mvd_video_zoom_position(&x, &y);
        const unsigned n = zoom_zones_add(mvd_video_zoom_level(), x, y);
        char text[80];
        if (n) snprintf(text, sizeof(text), "Zoom zone %u saved - ZOOM now cycles your zones", n);
        else snprintf(text, sizeof(text), zoom_zones_count() >= ZOOM_ZONES_MAX
                      ? "All %d zones are used - clear them first" : "That view is already a zone",
                      ZOOM_ZONES_MAX);
        show_notice(text);
        if (n) g_app.zone_index = (int)n - 1;
    } else if (zoom_zones_count()) {
        zoom_zones_clear();
        g_app.zone_index = -1;
        show_notice("Zoom zones cleared for this game");
    } else {
        show_notice("Zoom in with ZOOM and drag to a spot, then save it here");
    }
}

static void handle_stream(u32 down, u32 held, AppAction action, bool touch_down,
                          touchPosition touch)
{
    /* START + SELECT held opens the stream menu without leaving the game. */
    if ((held & (KEY_START | KEY_SELECT)) == (KEY_START | KEY_SELECT)) {
        if (!g_combo_started_at) g_combo_started_at = osGetTime();
        else if (osGetTime() - g_combo_started_at >= MENU_COMBO_HOLD_MS && !g_app.stream_menu) {
            g_app.stream_menu = true;
            g_app.stream_menu_index = STREAM_MENU_RESUME;
        }
    } else {
        g_combo_started_at = 0;
    }

    if (g_transport.keyboard_mode) {
        bool close = remote_keyboard_buttons(&g_transport, down);
        if (touch_down) close |= remote_keyboard_touch(&g_transport, touch.px, touch.py);
        if (close) g_transport.keyboard_mode = false;
        return;
    }

    if (g_app.controls_open) {
        if ((down & (KEY_A | KEY_B | KEY_SELECT)) || action == ACTION_CONTROLS_CLOSE)
            g_app.controls_open = false;
        return;
    }

    if (g_app.stream_menu) {
        /* Two columns: up/down move a row, left/right a column. */
        if ((down & KEY_UP) && g_app.stream_menu_index >= 2) g_app.stream_menu_index -= 2;
        if ((down & KEY_DOWN) && g_app.stream_menu_index + 2 < STREAM_MENU_COUNT) g_app.stream_menu_index += 2;
        if ((down & KEY_LEFT) && (g_app.stream_menu_index & 1)) --g_app.stream_menu_index;
        if ((down & KEY_RIGHT) && !(g_app.stream_menu_index & 1)) ++g_app.stream_menu_index;
        if (down & KEY_A) action = (AppAction)(ACTION_MENU_RESUME + g_app.stream_menu_index);
        if (down & KEY_B) action = ACTION_MENU_RESUME;
        if (action == ACTION_MENU_RESUME) {
            g_app.stream_menu = false;
        } else if (action == ACTION_MENU_CONTROLS) {
            g_app.stream_menu = false;
            g_app.controls_open = true;
        } else if (action == ACTION_MENU_SCREENSHOT) {
            g_app.stream_menu = false;
            g_screenshot_requested = true;
        } else if (action == ACTION_MENU_ZONE) {
            menu_zone();
        } else if (action == ACTION_MENU_SOUND) {
            g_app.sound_muted = !g_app.sound_muted;
        } else if (action == ACTION_MENU_GYRO) {
            screens_setting_change(&g_app, SETTING_GYRO, 1);
            settings_apply_input(&g_app.settings);
            reapply_game_map();
            save_settings();
        } else if (action == ACTION_MENU_LAYOUT) {
            screens_setting_change(&g_app, SETTING_LAYOUT, 1);
            save_settings();

        } else if (action == ACTION_MENU_DISCONNECT) {
            leave_session();
        }
        return;
    }

    switch (action) {
    case ACTION_STREAM_KEYBOARD:
        g_transport.keyboard_mode = true;
        remote_keyboard_open();
        return;
    case ACTION_STREAM_POINTER:
        webrtc_transport_set_pointer_mode(&g_transport, !g_transport.pointer_mode);
        return;
    case ACTION_STREAM_ZOOM:
        /* With saved zones, ZOOM steps through them (then back to full). */
        if (zoom_zones_count()) {
            g_app.zone_index = g_app.zone_index + 1 >= (int)zoom_zones_count() ? -1 : g_app.zone_index + 1;
            const ZoomZone *z = g_app.zone_index >= 0 ? zoom_zones_get((unsigned)g_app.zone_index) : NULL;
            mvd_video_set_zoom(z ? z->level : 0, z ? z->x : 50, z ? z->y : 50);
        } else {
            mvd_video_toggle_zoom();
        }
        return;
    case ACTION_STREAM_MENU:
        g_app.stream_menu = true;
        g_app.stream_menu_index = STREAM_MENU_RESUME;
        return;
    default: break;
    }

    /* Centre panel: touchpad in pointer mode, otherwise the zoom map. */
    const UiRect panel = screens_stream_panel();
    if (held & KEY_TOUCH) {
        if (g_transport.pointer_mode) {
            if (touch_down && ui_hit(panel, touch.px, touch.py) && g_transport.input_ready)
                touchpad_begin(touch.px, touch.py);
            else touchpad_move(touch.px, touch.py);
        } else if (mvd_video_zoomed() && ui_hit(panel, touch.px, touch.py)) {
            const float frame_w = panel.w - 8, frame_h = frame_w * 9 / 16;
            float nx = (touch.px - panel.x - 4) / frame_w, ny = (touch.py - panel.y - 6) / frame_h;
            if (nx < 0) nx = 0;
            if (nx > 1) nx = 1;
            if (ny < 0) ny = 0;
            if (ny > 1) ny = 1;
            mvd_video_pan_to((unsigned)(nx * 1000), (unsigned)(ny * 1000));
        }
    }
}

static const char *current_status(void);

/* ---- Session tracking ------------------------------------------------------ */

/* Lid closed or HOME opened: the app is frozen and the stream times out.
 * The hook only records when; track_session() reconnects afterwards. */
static volatile u64 g_suspended_at, g_resumed_at;
static aptHookCookie g_apt_cookie;

static void apt_hook(APT_HookType hook, void *param)
{
    (void)param;
    if (hook == APTHOOK_ONSLEEP || hook == APTHOOK_ONSUSPEND) g_suspended_at = osGetTime();
    else if (hook == APTHOOK_ONWAKEUP || hook == APTHOOK_ONRESTORE) g_resumed_at = osGetTime();
}

static bool g_history_open;

static void finish_history(void)
{
    if (!g_history_open) return;
    g_history_open = false;
    const u64 played = g_app.stream_started_at ? osGetTime() - g_app.stream_started_at : 0;
    play_history_end(g_current_game.app_id, (uint32_t)(played / 1000));
}

/* ---- Queue estimate -------------------------------------------------------- */

/* NVIDIA's queue number is not linear in time (in testing it fell 63 -> 50
 * in 41 s, then held at 50 for 80 s), but the whole wait was close to
 * proportional to the starting place: 63 places took 124 s, 70 took 131 s,
 * about 1.9 s per place. So the estimate is (starting place x learned
 * seconds per place) minus the time already waited, and every finished
 * queue refines the learned rate. */
#define QUEUE_STATS_PATH APP_DATA_DIR "/queue.json"

static float g_seconds_per_place = 1.9f;

static void queue_stats_load(void)
{
    json_error_t error;
    json_t *root = json_load_file(QUEUE_STATS_PATH, 0, &error);
    json_t *rate = json_is_object(root) ? json_object_get(root, "seconds_per_place") : NULL;
    if (json_is_number(rate) && json_number_value(rate) > 0.2 && json_number_value(rate) < 60.0)
        g_seconds_per_place = (float)json_number_value(rate);
    json_decref(root);
}

static void track_queue(void)
{
    static u64 queued_at;
    static int start_place;
    const u64 now = osGetTime();
    const bool queued = g_client.session_state == GFN_SESSION_QUEUED;
    if (queued) {
        if (!queued_at) queued_at = now;
        if (!start_place && g_client.queue_position > 0) start_place = g_client.queue_position;
        if (!start_place) { g_app.queue_eta = -1; return; }
        const float expected = (float)start_place * g_seconds_per_place;
        const float left = expected - (float)(now - queued_at) / 1000.0f;
        g_app.queue_eta = left > 20.0f ? (int)left : 0;
        return;
    }
    /* The queue just ended with a rig: learn from how long it really took. */
    if (queued_at && start_place > 0 &&
        (g_client.session_state == GFN_SESSION_SETUP || g_client.session_state == GFN_SESSION_READY)) {
        const float seconds = (float)(now - queued_at) / 1000.0f;
        const float rate = seconds / (float)start_place;
        if (rate > 0.2f && rate < 60.0f) {
            g_seconds_per_place = g_seconds_per_place * 0.6f + rate * 0.4f;
            json_t *root = json_pack("{s:f}", "seconds_per_place", (double)g_seconds_per_place);
            if (root) json_dump_file(root, QUEUE_STATS_PATH, JSON_COMPACT);
            json_decref(root);
            diagnostic_log("QUEUE", "finished start=%d seconds=%.0f rate=%.2f s/place learned=%.2f",
                           start_place, (double)seconds, (double)rate, (double)g_seconds_per_place);
        }
    }
    queued_at = 0;
    start_place = 0;
    g_app.queue_eta = -1;
}

/* Timer, free-tier warnings and automatic reconnects for a running session. */
static void track_session(void)
{
    static u64 reconnect_at, playing_since;
    static bool warned_5, warned_1;
    const u64 now = osGetTime();

    track_queue();
    char shot[64];
    const int shot_result = screenshot_poll(shot, sizeof(shot));
    if (shot_result > 0) {
        char text[96];
        snprintf(text, sizeof(text), "Screenshot saved: %s", shot);
        show_notice(text);
    } else if (shot_result < 0) {
        show_notice("Screenshot could not be saved to the SD card");
    }
    audio_output_set_muted(g_app.sound_muted ||
                           (g_app.settings.mute_in_menus && (g_app.stream_menu || g_app.controls_open)));

    static bool was_active;
    if (!gfn_session_active(&g_client)) {
        finish_history();
        /* A game's own options only last for its session. */
        if (was_active) {
            was_active = false;
            settings_apply_input(&g_app.settings);
            settings_apply_picture(&g_app.settings);
        }
        warned_5 = warned_1 = false;
        playing_since = 0;
        g_resumed_at = 0;
        return;
    }
    /* Back from sleep or the HOME Menu after more than a few seconds: the
     * media has timed out, so reconnect straight away (once Wi-Fi is back)
     * instead of waiting for the connection to be declared dead. */
    if (g_resumed_at && g_app.stream_started_at) {
        const u64 away = g_suspended_at && g_resumed_at > g_suspended_at ? g_resumed_at - g_suspended_at : 0;
        if (away < 4000) {
            g_resumed_at = 0;
        } else if ((osGetWifiStrength() > 0 || now - g_resumed_at > 15000) && !net_worker_busy() &&
                   g_client.session_state == GFN_SESSION_READY) {
            diagnostic_log("APP", "resumed after %llu ms away; reconnecting", (unsigned long long)away);
            g_resumed_at = 0;
            g_app.reconnect_attempt = 1;
            reconnect_at = now;
            show_notice("Welcome back - reconnecting to your rig");
            retry_session();
            return;
        }
    }
    was_active = true;
    if (g_client.queue_position > 0) g_app.free_tier_guess = true;
    if (g_app.view == VIEW_STREAM) {
        if (!g_app.stream_started_at) {
            g_app.stream_started_at = now;
            diagnostic_log("APP", "stream started freeTierGuess=%d", g_app.free_tier_guess);
            play_history_begin(g_current_game.app_id, g_current_game.title);
            g_history_open = true;
        }
        if (!playing_since) playing_since = now;
        /* Ten clean seconds after a reconnect: the next drop starts afresh. */
        if (g_app.reconnect_attempt && now - playing_since >= 10000) g_app.reconnect_attempt = 0;
    } else {
        playing_since = 0;
    }
    if (g_app.free_tier_guess && g_app.stream_started_at) {
        const u64 elapsed = now - g_app.stream_started_at;
        if (!warned_5 && elapsed >= 55ull * 60 * 1000) {
            warned_5 = true;
            show_notice("About 5 minutes left: free sessions end after one hour");
        }
        if (!warned_1 && elapsed >= 59ull * 60 * 1000) {
            warned_1 = true;
            show_notice("About 1 minute left in this free session");
        }
    }

    /* A dropped connection mid-game: the rig is still ours, so reconnect the
     * media instead of sending the player back to the library. */
    const bool dropped = g_transport.state == WEBRTC_FAILED || g_signal.state == NVST_SIGNAL_ERROR;
    if (!dropped || !g_app.stream_started_at || g_leave_pending || net_worker_busy()) return;
    if (g_client.session_state != GFN_SESSION_READY || g_app.reconnect_attempt > 3) return;
    if (reconnect_at && now - reconnect_at < 2500) return;
    if (g_app.reconnect_attempt == 3) {
        /* Three tries failed: hand the choice back to the player. */
        if (now - reconnect_at >= 8000) g_app.reconnect_attempt = 4;
        return;
    }
    reconnect_at = now;
    ++g_app.reconnect_attempt;
    diagnostic_log("APP", "connection lost (%s); reconnect attempt %u",
                   g_transport.state == WEBRTC_FAILED ? g_transport.status : g_signal.status,
                   g_app.reconnect_attempt);
    show_notice("Connection lost - reconnecting");
    retry_session();
}

/* ---- Resolution probe (developer) ---------------------------------------- */

/* If APP_DATA_DIR/probe.txt exists, each "WxH" line launches a game (the
 * first library entry, or the first whose title contains "game=<text>"),
 * records the SPS resolution NVIDIA actually encodes, and ends the session.
 * "WxH sharp" asks for the old prefilter; each run then watches 45 s of
 * video and counts IDRs.
 * Results go to probe-results.txt, outside the capped diagnostic log. */
enum { PROBE_MAX = 16 };
enum { PROBE_OFF, PROBE_LIBRARY, PROBE_LAUNCH, PROBE_WAIT_SPS, PROBE_LEAVE, PROBE_DONE };
static struct {
    unsigned width[PROBE_MAX], height[PROBE_MAX];
    bool sharp[PROBE_MAX];
    unsigned count, index;
    unsigned idr_base, pli_base;
    u64 video_since;
    bool offer_dumped;
    int phase;
    u64 since;
    char game[48];
} g_probe;

static void probe_result(const char *format, ...)
{
    char line[192];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    diagnostic_log("PROBE", "%s", line);
    diagnostic_checkpoint();
    FILE *f = fopen(APP_DATA_DIR "/probe-results.txt", "a");
    if (!f) return;
    fprintf(f, "%s\n", line);
    fclose(f);
}

static void probe_load(void)
{
    FILE *f = fopen(APP_DATA_DIR "/probe.txt", "r");
    if (!f) return;
    char line[96];
    while (fgets(line, sizeof(line), f)) {
        unsigned w, h;
        if (!strncmp(line, "game=", 5)) {
            snprintf(g_probe.game, sizeof(g_probe.game), "%.47s", line + 5);
            g_probe.game[strcspn(g_probe.game, "\r\n")] = 0;
        } else if (g_probe.count < PROBE_MAX && sscanf(line, "%ux%u", &w, &h) == 2) {
            g_probe.width[g_probe.count] = w;
            g_probe.height[g_probe.count] = h;
            g_probe.sharp[g_probe.count] = strstr(line, "sharp") != NULL;
            ++g_probe.count;
        }
    }
    fclose(f);
    if (!g_probe.count) return;
    stream_profile_set_probing(true);
    g_probe.phase = PROBE_LIBRARY;
    g_probe.since = osGetTime();
    probe_result("probe start: %u resolutions, game filter \"%s\"", g_probe.count, g_probe.game);
}

static const GfnGame *probe_game(void)
{
    for (size_t i = 0; g_probe.game[0] && i < g_client.game_count; ++i)
        if (strstr(g_client.games[i].title, g_probe.game)) return &g_client.games[i];
    return g_client.game_count ? &g_client.games[0] : NULL;
}

/* The server's NVST offer lists the knobs it understands. Record attribute
 * names only (values can hold ICE credentials). */
static void probe_dump_offer(void)
{
    const char *p = g_signal.offer_nvst_sdp;
    FILE *f = p ? fopen(APP_DATA_DIR "/probe-results.txt", "a") : NULL;
    if (!f) return;
    fputs("server offer attributes:", f);
    while ((p = strstr(p, "a=")) != NULL) {
        p += 2;
        const size_t len = strcspn(p, ":\r\n");
        fprintf(f, " %.*s", (int)len, p);
        p += len;
    }
    fputc('\n', f);
    fclose(f);
}

static void probe_tick(void)
{
    if (g_probe.phase == PROBE_OFF || g_probe.phase == PROBE_DONE || net_worker_busy()) return;
    const u64 now = osGetTime();
    const unsigned w = g_probe.width[g_probe.index], h = g_probe.height[g_probe.index];
    switch (g_probe.phase) {
    case PROBE_LIBRARY:
        if (g_client.game_count) {
            g_probe.phase = PROBE_LAUNCH;
        } else if (now - g_probe.since >= 5000 && gfn_has_session(&g_client)) {
            g_probe.since = now;
            load_library();
        }
        break;
    case PROBE_LAUNCH:
        if (now - g_probe.since < 4000) break; /* let the last session wind down */
        if (g_probe.index >= g_probe.count) {
            g_probe.phase = PROBE_DONE;
            stream_profile_set_override(0, 0);
            settings_apply_picture(&g_app.settings);
            probe_result("probe finished");
            break;
        }
        stream_profile_set_override(w, h);
        stream_profile_set_sharpen(g_probe.sharp[g_probe.index]);
        g_app.modal = MODAL_NONE;
        probe_result("request %ux%u%s (%s)", w, h,
                     g_probe.sharp[g_probe.index] ? " sharp" : "", probe_game()->title);
        launch_game(probe_game());
        g_probe.phase = PROBE_WAIT_SPS;
        g_probe.since = now;
        break;
    case PROBE_WAIT_SPS:
        if (g_transport.video_source_width && !g_probe.video_since) {
            probe_result("request %ux%u -> NVIDIA sends %ux%u refs=%u after %llu s", w, h,
                         g_transport.video_source_width, g_transport.video_source_height,
                         g_transport.video_source_refs,
                         (unsigned long long)((now - g_probe.since) / 1000));
            if (!g_probe.offer_dumped) probe_dump_offer();
            g_probe.offer_dumped = true;
            g_probe.video_since = now;
            g_probe.idr_base = g_transport.video_idr_units;
            g_probe.pli_base = g_transport.keyframe_requests;
            break;
        } else if (g_probe.video_since) {
            if (now - g_probe.video_since < 45000) break;
            probe_result("  45 s: %u IDRs, %u keyframe requests, %u AUs, %u kbps",
                         g_transport.video_idr_units - g_probe.idr_base,
                         g_transport.keyframe_requests - g_probe.pli_base,
                         g_transport.video_access_units, g_transport.video_kbps);
        } else if (g_app.modal == MODAL_ERROR) {
            probe_result("request %ux%u -> launch failed: %s", w, h, g_client.status);
            g_app.modal = MODAL_NONE;
        } else if (now - g_probe.since >= 600000) {
            probe_result("request %ux%u -> no video after 10 min (%s)", w, h, current_status());
        } else {
            break;
        }
        g_probe.video_since = 0;
        leave_session();
        g_probe.phase = PROBE_LEAVE;
        g_probe.since = now;
        break;
    case PROBE_LEAVE:
        if (!gfn_session_active(&g_client) && !g_transport.peer && !g_leave_pending) {
            ++g_probe.index;
            g_probe.phase = PROBE_LAUNCH;
            g_probe.since = now;
        }
        break;
    }
}

/* ---- Main ------------------------------------------------------------------ */

static void wait_for_media(int timeout_ms)
{
    const int fd = webrtc_transport_socket(&g_transport);
    if (fd < 0) {
        svcSleepThread((s64)timeout_ms * 1000000LL);
        return;
    }
    struct pollfd media = { .fd = fd, .events = POLLIN, .revents = 0 };
    if (poll(&media, 1, timeout_ms) > 0 && (media.revents & POLLIN))
        g_transport.media_readable = true;
}

static void tick_network(void)
{
    /* Signaling starts on the worker (its TLS connect blocks); afterwards
     * the UI thread services it without blocking. */
    if (g_client.session_state == GFN_SESSION_READY && g_signal.state == NVST_SIGNAL_IDLE &&
        !net_worker_busy() && !g_leave_pending)
        submit_job(NET_JOB_START_SIGNAL, NULL, NULL, NULL);
    if (net_worker_signal_starting()) return;
    /* Signaling is only heartbeats once media flows; ~60 Hz is plenty and
     * keeps socket requests low while the stream loop runs fast. */
    static u64 last_signal_tick;
    if (!g_transport.peer || osGetTime() - last_signal_tick >= 16) {
        nvst_signal_tick(&g_signal);
        last_signal_tick = osGetTime();
    }
    /* When the SDP offer has arrived, drive the WebRTC answer and ICE/DTLS. */
    if (g_signal.state == NVST_SIGNAL_OFFER && !g_transport.peer &&
        g_transport.state != WEBRTC_FAILED && g_signal.offer_sdp) {
        if (webrtc_transport_start(&g_transport, &g_signal,
                                   g_client.media_ip[0] ? g_client.media_ip : g_client.server_ip,
                                   g_client.media_port)) {
            g_transport.prefer_partial_input = g_app.settings.fast_input;
            if (g_app.genshin_session && g_app.settings.auto_pointer)
                webrtc_transport_set_pointer_mode(&g_transport, true);
        }
    }
    webrtc_transport_tick(&g_transport, &g_signal);
}

/* The status strip shows whichever layer is currently doing the work. */
static const char *current_status(void)
{
    static char line[160];
    if (g_notice[0] && osGetTime() < g_notice_until) return g_notice;
    if (g_transport.peer) {
        snprintf(line, sizeof(line), "WebRTC: %.145s", g_transport.status);
        return line;
    }
    if (nvst_signal_active(&g_signal) || g_signal.state == NVST_SIGNAL_ERROR) return g_signal.status;
    return g_client.status;
}

/* React to a worker job that just finished. */
static void finish_jobs(void)
{
    static unsigned seen_serial;
    const NetJobResult result = net_worker_last_result();
    if (result.serial == seen_serial) return;
    seen_serial = result.serial;
    if (result.cancelled) {
        show_notice("Cancelled");
    } else if ((result.kind == NET_JOB_START_SESSION || result.kind == NET_JOB_RESTART_SESSION) &&
               !result.ok) {
        open_modal(MODAL_ERROR, "起動失敗", "LAUNCH FAILED", g_client.status);
    }
    if (result.kind == NET_JOB_LOAD_LIBRARY || result.kind == NET_JOB_SEARCH)
        g_app.selected = g_app.list_top = 0;
    if (result.kind == NET_JOB_UPDATE_CHECK) {
        const UpdateInfo info = updater_info();
        if (info.state == UPDATE_AVAILABLE && !updater_dismissed() && !g_app.update_open) {
            char text[96];
            snprintf(text, sizeof(text), "Kasumi %s is available - Settings > Updates", info.latest);
            show_notice(text);
        }
    }
    if (g_leave_pending && !net_worker_busy()) leave_session();
}

static void log_session(void)
{
    static u64 last;
    /* Once video plays, every 5 s: long sessions must not flood the log. */
    const u64 interval = g_app.view == VIEW_STREAM ? 5000 : 1000;
    if (!gfn_session_active(&g_client) || osGetTime() - last < interval) return;
    last = osGetTime();
    diagnostic_log("SESSION", "state=%d status=%d queue=%d nvst=%d rx=%u frames=%u ack=%u hb=%u webrtc=%d video=%u kbps=%u idr=%u pli=%u audio=%u decoded=%u drop=%u err=%u data=%u input=%u reports=%u mouse=%u clicks=%u keys=%u conceal=%u",
        g_client.session_state, g_client.session_status, g_client.queue_position,
        g_signal.state, g_signal.messages, g_signal.parser.frames,
        g_signal.acknowledgements, g_signal.heartbeats, g_transport.state,
        g_transport.video_access_units, g_transport.video_kbps,
        g_transport.video_saw_idr ? 1 : 0, g_transport.keyframe_requests,
        g_transport.audio_packets, g_transport.audio_decoded, g_transport.audio_dropped,
        g_transport.audio_errors, g_transport.data_messages, g_transport.input_ready ? 1 : 0,
        g_transport.input_reports, g_transport.mouse_moves, g_transport.mouse_clicks,
        g_transport.keyboard_keys, audio_output_concealed());
}

static void fatal_screen(const char *title, const char *message)
{
    g_app.status = message;
    open_modal(MODAL_EXIT, "エラー", title, message);
    g_app.view = VIEW_WELCOME;
    while (aptMainLoop() && !g_quit) {
        hidScanInput();
        if (hidKeysDown() & (KEY_A | KEY_B | KEY_START)) break;
        render(true);
    }
}

int main(int argc, char **argv)
{
    gfxInitDefault();
    gfxSetScreenFormat(GFX_TOP, GSP_RGB565_OES);
    /* One top framebuffer: MVD frames are written into it directly. */
    gfxSetDoubleBuffering(GFX_TOP, false);
    if (!ui_init()) {
        gfxExit();
        return 1;
    }
    g_app.client = &g_client;
    g_app.signal = &g_signal;
    g_app.transport = &g_transport;
    g_app.current_game = &g_current_game;
    g_app.zone_index = -1;
    game_art_init();
    settings_load(&g_app.settings);
    g_app.guide_page = g_app.settings.guide_done ? -1 : 0;
    settings_apply_input(&g_app.settings);
    settings_apply_picture(&g_app.settings);
    hidSetRepeatParameters(18, 5);

    bool is_new_3ds = false;
    const Result model_result = APT_CheckNew3DS(&is_new_3ds);
    if (R_FAILED(model_result) || !is_new_3ds) {
        fatal_screen("NEW 3DS REQUIRED",
                     "Kasumi needs a New 3DS, New 3DS XL or New 2DS XL for its video decoder.");
        ui_exit();
        gfxExit();
        return 1;
    }

    osSetSpeedupEnable(true);
    char init_error[128] = "";
    if (!init_services(init_error, sizeof(init_error))) {
        fatal_screen("STARTUP FAILED", init_error);
        shutdown_services();
        ui_exit();
        gfxExit();
        return 1;
    }

    app_paths_migrate();
    diagnostic_init();
    diagnostic_log("APP", "startup model=%s wifiBars=%u linearFreeKiB=%lu",
                   is_new_3ds ? "new3ds-family" : "old3ds-family",
                   osGetWifiStrength(), (unsigned long)(linearSpaceFree() / 1024));
    nvst_signal_init(&g_signal);
    webrtc_transport_init(&g_transport);
    gfn_client_init(&g_client);
    /* The saved library appears instantly; covers keep filling in behind. */
    if (gfn_has_session(&g_client) && gfn_library_load(&g_client))
        game_art_prefetch(g_client.games, (unsigned)g_client.game_count);
    if (!gfn_input_self_test()) {
        diagnostic_log("INPUT", "wire encoder self-test FAILED");
        show_notice("Input packet self-test failed");
    }
    play_history_load();
    game_prefs_load();
    queue_stats_load();
    g_app.queue_eta = -1;
    updater_init(argc > 0 && argv ? argv[0] : NULL);
    /* First start of a freshly installed version: show what changed, once. */
    if (updater_take_whats_new(g_app.whats_new_version, sizeof(g_app.whats_new_version),
                               g_whats_new_notes, sizeof(g_whats_new_notes))) {
        g_app.whats_new_open = true;
        g_app.whats_new_notes = g_whats_new_notes;
    }
    aptHook(&g_apt_cookie, apt_hook, NULL);
    if (!net_worker_start(&g_client, &g_signal)) {
        fatal_screen("STARTUP FAILED", "Could not start the network worker thread.");
        shutdown_services();
        ui_exit();
        gfxExit();
        return 1;
    }
    probe_load();
    bool sleep_allowed = true;

    bool was_touching = false;
    u64 last_bottom_draw = 0;
    u64 loop_started = osGetTime();
    while (aptMainLoop() && !g_quit) {
        {
            const u64 loop_now = osGetTime();
            const unsigned loop_ms = (unsigned)(loop_now - loop_started);
            loop_started = loop_now;
            if (g_app.view == VIEW_STREAM) {
                if (loop_ms > g_loop_max_ms) g_loop_max_ms = loop_ms;
                if (loop_ms > 25) ++g_loop_slow;
            }
        }
        net_worker_sync(&g_client);
        finish_jobs();
        probe_tick();
        g_app.busy = net_worker_busy() ? g_busy_message : NULL;
        hidScanInput();
        const u32 down = hidKeysDown();
        const u32 held = hidKeysHeld();
        const u32 repeat = hidKeysDownRepeat();
        touchPosition touch = {0, 0};
        if (held & KEY_TOUCH) hidTouchRead(&touch);
        const bool touch_down = (down & KEY_TOUCH) != 0;
        g_app.touching = (held & KEY_TOUCH) != 0;
        g_app.touch_x = touch.px;
        g_app.touch_y = touch.py;

        const AppView previous_view = g_app.view;
        g_app.view = derive_view();
        if (previous_view == VIEW_STREAM && g_app.view != VIEW_STREAM) release_stream_input();
        g_app.keyboard_open = g_transport.keyboard_mode;

        if (g_app.view == VIEW_LIBRARY || g_app.view == VIEW_DETAILS) rebuild_list();
        const AppAction action = touch_down ? screens_touch(&g_app, touch.px, touch.py) : ACTION_NONE;
        if (g_app.view != VIEW_STREAM) auto_update_check();
        if (g_app.whats_new_open && g_app.view != VIEW_STREAM) {
            handle_whats_new(down, repeat, action);
        } else if (g_app.guide_page >= 0 && g_app.view != VIEW_STREAM && !g_app.busy) {
            handle_guide(down, action);
        } else if (g_app.update_open && g_app.view != VIEW_STREAM && !g_app.busy) {
            handle_updates(down, repeat, action);
        } else if (g_app.busy) {
            /* The UI stays live during requests; B cancels what can be cancelled. */
            if (down & KEY_B) net_worker_cancel();
        } else if (g_app.modal != MODAL_NONE) {
            handle_modal(down, action);
        } else {
            switch (g_app.view) {
            case VIEW_WELCOME: handle_welcome(down, action); break;
            case VIEW_LOGIN: handle_login(down, action); break;
            case VIEW_LIBRARY: handle_library(down, repeat, action); break;
            case VIEW_SETTINGS: handle_settings(down, repeat, action); break;
            case VIEW_SESSION: handle_session(down, action); break;
            case VIEW_DETAILS: handle_details(down, repeat, action); break;
            case VIEW_STREAM: handle_stream(down, held, action, touch_down, touch); break;
            }
        }
        if (hidKeysUp() & KEY_TOUCH) touchpad_end();
        update_pointer_click(down, held);

        /* Game input: touch-held L3/R3/PS, and nothing while menus are up. */
        const bool streaming = g_app.view == VIEW_STREAM;
        gfn_input_set_virtual_buttons(streaming && g_app.touching
            ? screens_stream_held_buttons(&g_app, touch.px, touch.py) : 0);
        gfn_input_set_suppressed(!streaming || g_app.stream_menu || g_app.controls_open || g_app.modal != MODAL_NONE);

        tick_network();
        refresh_device_status();
        log_session();

        /* "Keep streaming" holds the console awake for the whole session;
         * "Pause & resume" lets the lid sleep it and reconnects on waking. */
        const bool want_sleep = !gfn_session_active(&g_client) || !g_app.settings.lid_keeps_playing;
        if (want_sleep != sleep_allowed) {
            aptSetSleepAllowed(want_sleep);
            sleep_allowed = want_sleep;
        }

        g_app.view = derive_view();
        g_app.keyboard_open = g_transport.keyboard_mode;
        g_app.status = current_status();
        g_app.toast = g_notice[0] && osGetTime() < g_notice_until ? g_notice : NULL;
        track_session();
        g_app.video_stalled = g_app.view == VIEW_STREAM && g_transport.last_decoded_frame_at &&
                              osGetTime() - g_transport.last_decoded_frame_at > 1500;
        /* While video owns the top screen, redraw the lower screen only when
         * something on it can have changed. */
        const u64 now = osGetTime();
        const bool draw_bottom = g_app.view != VIEW_STREAM || g_app.touching || was_touching ||
                                 down || now - last_bottom_draw >= 250 ||
                                 /* Animations on the lower screen run at ~30 fps
                                  * while streaming (build 69 redrew every loop). */
                                 (screens_bottom_animating() &&
                                  (g_app.view != VIEW_STREAM || now - last_bottom_draw >= 33));
        if (draw_bottom) last_bottom_draw = now;
        was_touching = g_app.touching;
        if (g_app.view != VIEW_STREAM) game_art_pump();
        render(draw_bottom);
        /* While streaming, don't wait for vblank: sleep inside poll() on the
         * media socket so a video frame is decoded the moment its packets
         * land. The 4 ms cap keeps input sampling fast. Build 49 spun on
         * nonblocking recv every 1 ms instead, and that request flood crashed
         * the system socket module when video started. */
        if (g_app.view == VIEW_STREAM) wait_for_media(4);
    }

    finish_history();
    aptUnhook(&g_apt_cookie);
    if (gfn_session_active(&g_client)) {
        close_media();
        net_worker_wait_idle(8000);
        if (net_worker_submit(NET_JOB_STOP_SESSION, NULL, NULL)) net_worker_wait_idle(8000);
    }
    aptSetSleepAllowed(true);
    net_worker_stop();
    game_art_exit();
    shutdown_services();
    ui_exit();
    gfxExit();
    return 0;
}

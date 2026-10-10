#include <3ds.h>

#include <malloc.h>
#include <math.h>
#include <poll.h>
#include <stdarg.h>
#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "app.h"
#include "app_paths.h"
#include "audio_output.h"
#include "diagnostic.h"
#include "game_art.h"
#include "game_prefs.h"
#include "menu_audio.h"
#include "sfx.h"
#include "shortcut.h"
#include "gallery.h"
#include "queue_eta.h"
#include "queue_alert.h"
#include "regions.h"
#include "report.h"
#include "perf_stats.h"
#include "updater.h"
#include "play_history.h"
#include "screenshot.h"
#include "zoom_zones.h"
#include "gfn_client.h"
#include "gfn_input.h"
#include "launch_stats.h"
#include "net_memory.h"
#include "http_client.h"
#include "mvd_video.h"
#include "net_worker.h"
#include "nvst_signal.h"
#include "file_worker.h"
#include "mic_capture.h"
#include "phone_keyboard.h"
#include "remote_keyboard.h"
#include "settings.h"
#include "stream_profile.h"
#include "ui.h"
#include "webrtc_transport.h"
#include "xcloud.h"
#include "steam_link.h"

#define SOC_BUFFER_SIZE (0x100000)
#define SOC_BUFFER_ALIGNMENT (0x1000)
#define MENU_COMBO_HOLD_MS 800

static u32 *g_soc_buffer;
static bool g_soc_ready;
static bool g_ac_ready;
static bool g_ptm_ready;
/* ndm:u: exclusive Wi-Fi while a game runs (see wifi_exclusive). */
static bool g_ndm_ready, g_ndm_exclusive;
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
/* When a launch waiting for a free session slot last went out. */
static u64 g_limit_last_try;
static bool g_screenshot_requested;
static void show_notice(const char *text);
static void finish_history(void);
static void log_session_end(const char *by);
static void apply_game_options(const GamePrefs *prefs);
static void launch_with_options(const GfnGame *base, unsigned variant);
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
    mic_capture_stop();
    file_worker_exit();
    phone_keyboard_stop();
    webrtc_transport_close(&g_transport);
    nvst_signal_close(&g_signal);
    diagnostic_log("APP", REPORT_CLEAN_EXIT);
    diagnostic_close();
    http_global_exit();
    if (g_soc_ready) socExit();
    free(g_soc_buffer);
    if (g_ptm_ready) ptmuExit();
    if (g_ndm_exclusive) {
        NDMU_UnlockState();
        NDMU_LeaveExclusiveState();
        g_ndm_exclusive = false;
    }
    if (g_ndm_ready) ndmuExit();
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
    g_ndm_ready = R_SUCCEEDED(ndmuInit());
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
        /* Weak / hotspot keeps one more spare frame (+33 ms) against the
         * longer, burstier gaps of mobile data. */
        const unsigned reserve_low = stream_profile_weak() ? 2 : 1;
        if (reserve < reserve_low) reserve = reserve_low;
        if (reserve > reserve_low && now_ms - last_repeat_at >= 60000) reserve = reserve_low;
        /* The decoder can be rebuilt from core 2 (a new stream size):
         * frames are only looked at under the transport lock. */
        webrtc_transport_lock();
        unsigned ready = mvd_video_ready_frames();
        /* The rate NVIDIA really sends, not the one asked for: a 60 fps test
         * that got 30 showed every frame twice as "repeated" and grew the
         * reserve (test report ZUFZFS). Decoded frames over ~2 s. */
        static unsigned rate_frames;
        static u64 rate_since;
        static bool sixty;
        const unsigned decoded_now = mvd_video_decoded_frames();
        if (!rate_since || decoded_now < rate_frames) {
            /* Until measured, trust the rate asked for (the first 2 s of a
             * 60 fps stream paced as 30 skipped 62 frames). */
            sixty = stream_profile_fps() >= 60;
            rate_since = now_ms;
            rate_frames = decoded_now;
        } else if (now_ms - rate_since >= 2000) {
            const unsigned fps = (unsigned)((decoded_now - rate_frames) * 1000ull / (now_ms - rate_since));
            const bool was = sixty;
            sixty = stream_profile_fps() >= 60 && fps >= 45;
            if (sixty != was) diagnostic_log("VIDEO", "pacing %s (%u fps arriving)", sixty ? "60" : "30", fps);
            rate_since = now_ms;
            rate_frames = decoded_now;
        }
        /* Decoded frames waiting to be shown are input lag: at 60 fps keep
         * one beyond the reserve (~33-50 ms in all), not three (report
         * EM7YGV felt the 30 fps cap as lag at 60). */
        while (ready > reserve + (sixty ? 1u : 3u)) {
            mvd_video_skip_oldest_frame();
            --ready;
            ++skipped;
            ++g_perf.skipped;
        }
        /* Strict cadence, two vblanks at 30 fps and one at 60: a frame is
         * never shown for a shorter time than the others (that reads as a
         * hitch too); overflow is trimmed above. */
        const u32 cadence = sixty ? 1 : 2;
        if (ready && since >= cadence) {
            present = true;
        } else if (!ready && since == cadence) {
            ++repeated;
            ++g_perf.repeated;
            if (last_repeat_at && now_ms - last_repeat_at < 20000) reserve = reserve_low + 1;
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
                /* The bar relaxes as the wait goes on (0.7 -> 1.0 of the
                 * average), so the drop still lands on a low-motion frame:
                 * at ~1.6 Mbps only 2 of 21 drops found a frame under 0.7
                 * and the rest were forced after 5 s (beta.25 test). */
                const float bar = 0.7f + 0.3f * (float)(drop_wait < 150 ? drop_wait : 150) / 150.0f;
                const bool quiet = best && (float)best_bytes <= avg_bytes * bar;
                if (ready < reserve + 2) {
                    drop_wait = 0; /* the surplus went away on its own */
                } else if (best && (quiet || drop_wait >= 150)) {
                    mvd_video_skip_ready_frame(best);
                    if (quiet) ++quiet_drops;
                    ++drained;
                    ++g_perf.drained;
                    drop_wait = 0;
                }
                backlog_streak = 0;
            }
        }
        webrtc_transport_unlock();
    }
    /* A lower-screen redraw the GPU was too busy for goes out on the next
     * pass: build 116 dropped it, and a menu move waited for the 250 ms
     * refresh (the stream menu felt laggy). */
    static bool bottom_pending;
    draw_bottom = draw_bottom || bottom_pending;
    if (!present && !draw_bottom) return;
    const u64 start = svcGetSystemTick();
    /* A video frame waits for the GPU: build 115 skipped the pass instead
     * and came back later, but a frame put off past the next refresh grew
     * the queue and was trimmed (15 of every 120 skipped, report GWU2AV).
     * A bottom-screen-only redraw can wait for a free GPU. */
    if (present) {
        ui_frame_begin(false);
    } else if (!ui_frame_try_begin()) {
        bottom_pending = true;
        return;
    }
    bottom_pending = false;
    /* The frame is guarded by mvd_video's own present lock, not the
     * transport's: the media thread keeps reading packets meanwhile. */
    const void *frame = present ? mvd_video_take_gpu_frame() : NULL;
    if (frame && g_screenshot_requested) {
        g_screenshot_requested = false;
        show_notice(screenshot_capture(frame) ? "Saving screenshot..." : "Screenshot failed");
    }
    if (frame) {
        ui_video_upload(frame);
        mvd_video_release_gpu_frame();
    }
    if (frame) {
        ui_begin_top_video();
        unsigned video_w, video_h;
        mvd_video_wide_size(&video_w, &video_h);
        ui_set_video_size(video_w, video_h);
        float crop_x, crop_y, crop_w, crop_h;
        mvd_video_view_crop(&crop_x, &crop_y, &crop_w, &crop_h);
        ui_set_video_crop(crop_x, crop_y, crop_w, crop_h);
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
    /* Every 600 frames (~20 s), sooner when frames were repeated or skipped. */
    const unsigned pending_frames = ++presented - last_log_frames;
    if (pending_frames >= 600 || (pending_frames >= 120 && (repeated > 6 || skipped > 3))) {
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
    if (video) ui_top_classic_video();
    if (video && !draw_bottom) {
        present_video();
        return;
    }
    /* Menus sync to vblank; the stream never waits for it, so video and
     * input keep flowing while the lower screen redraws. */
    ui_frame_begin(!video);
    if (!video) {
        /* A shortcut's icon and banner, drawn offscreen (menus only). */
        shortcut_frame();
        ui_begin_top();
        screens_draw_top(&g_app);
    }
    ui_begin_bottom();
    screens_draw_bottom(&g_app);
    ui_frame_end();
    shortcut_frame_end();
    if (video) present_video();
}

static void show_notice(const char *text)
{
    /* What the player was told belongs in the log: reports showed the
     * internals but not the words on screen. Repeats within 10 s skip it. */
    static u64 logged_at;
    const u64 now = osGetTime();
    if (strcmp(g_notice, text) || now - logged_at >= 10000) {
        diagnostic_log("UI", "notice: %.90s", text);
        logged_at = now;
    }
    /* The wind chime, in the menus only: never over a game's own sound. */
    if (g_app.view != VIEW_STREAM && strcmp(g_notice, text)) sfx_play(SFX_NOTICE);
    snprintf(g_notice, sizeof(g_notice), "%s", text);
    g_notice_until = now + 4000;
}

/* A job asked for while the worker ran background work (stats upload,
 * update check): it runs as soon as the worker is free. Beta.19 test: the
 * player had to press A two or three times after a game ("Still working on
 * the last request") while the session summary was being sent. */
static struct {
    bool set;
    NetJobKind kind;
    const char *busy;
    char text[80];
    GfnGame game;
    bool has_game;
} g_deferred;

/* An automatic report is uploading (auto_report_tick). */
static bool g_auto_inflight;

static bool background_job(NetJobKind kind)
{
    return kind == NET_JOB_SEND_STATS || kind == NET_JOB_UPDATE_CHECK || kind == NET_JOB_KEEP_LOGIN ||
           kind == NET_JOB_SHORTCUT;
}

/* Hand a blocking call to the network worker; the UI keeps animating. */
static bool submit_job(NetJobKind kind, const char *busy, const char *text, const GfnGame *game)
{
    if (!net_worker_submit(kind, text, game)) {
        const NetJobKind running = net_worker_current_job();
        /* An automatic report is background work too: a manual one waits
         * for it instead of being refused behind a toast Settings never
         * shows. */
        const bool running_background =
            background_job(running) || (running == NET_JOB_SEND_REPORT && g_auto_inflight);
        if (running_background && !background_job(kind) && !g_deferred.set) {
            g_deferred.set = true;
            g_deferred.kind = kind;
            g_deferred.busy = busy;
            snprintf(g_deferred.text, sizeof(g_deferred.text), "%s", text ? text : "");
            g_deferred.has_game = game != NULL;
            if (game) g_deferred.game = *game;
            /* The stats upload retries later; an update check is quick. */
            if (running == NET_JOB_SEND_STATS) net_worker_cancel();
            g_busy_message = busy;
            return true;
        }
        show_notice("Still working on the last request");
        return false;
    }
    g_busy_message = busy;
    return true;
}

static void run_deferred_job(void)
{
    if (!g_deferred.set || net_worker_busy()) return;
    g_deferred.set = false;
    if (net_worker_submit(g_deferred.kind, g_deferred.text, g_deferred.has_game ? &g_deferred.game : NULL))
        g_busy_message = g_deferred.busy;
}

/* When the open question appeared: a press in its first 300 ms is not an
 * answer to it (a test run exited 151 ms after "EXIT KASUMI?" opened). */
static u64 g_modal_opened_at;

static void open_modal(AppModal modal, const char *jp, const char *title, const char *text)
{
    g_modal_opened_at = osGetTime();
    if (text && text[0]) diagnostic_log("UI", "modal %d %s: %.200s", (int)modal, title, text);
    else diagnostic_log("UI", "modal %d %s", (int)modal, title);
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
    if (g_app.hub_open) return VIEW_HUB;
    /* A service not signed in shows its sign-in card in the library. */
    if (g_app.details_open && gfn_has_session(&g_client) && g_app.selected < g_app.list_count)
        return VIEW_DETAILS;
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
    if (g_app.view == VIEW_STREAM && g_perf.active)
        perf_sample(g_transport.rtt_ms, g_app.wifi_bars, g_transport.video_kbps, g_app.fps,
                    g_app.resent_per_second, mvd_video_frames_lost(), g_transport.keyframe_requests,
                    audio_output_concealed());
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

static unsigned current_service(void) { return (unsigned)app_service(&g_app); }

static void begin_login(void)
{
    static const char *const busy[SERVICE_COUNT] = {
        "Requesting a sign-in code from NVIDIA...", "Requesting a sign-in code from Microsoft...",
        "Looking for your PC on this Wi-Fi..."
    };
    submit_job(NET_JOB_BEGIN_LOGIN, busy[current_service()], g_app.settings.provider, NULL);
}

static void load_library(void)
{
    static const char *const busy[SERVICE_COUNT] = {
        "Loading your GeForce NOW library...", "Loading your Xbox cloud games...", "Loading what your PC can stream..."
    };
    g_app.search_text[0] = '\0';
    submit_job(NET_JOB_LOAD_LIBRARY, g_client.library_saved_at ? "Refreshing your library..."
               : busy[current_service()], NULL, NULL);
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
    static const char *const hint[SERVICE_COUNT] = {
        "Search GeForce NOW games", "Search your Xbox cloud games", "Search the games played on your PC"
    };
    swkbdSetHintText(&g_search_keyboard, hint[current_service()]);
    swkbdSetButton(&g_search_keyboard, SWKBD_BUTTON_LEFT, "Cancel", false);
    swkbdSetButton(&g_search_keyboard, SWKBD_BUTTON_RIGHT, "Search", true);
    swkbdSetValidation(&g_search_keyboard, SWKBD_NOTEMPTY_NOTBLANK, 0, 0);
    swkbdSetFeatures(&g_search_keyboard, SWKBD_DARKEN_TOP_SCREEN | SWKBD_PREDICTIVE_INPUT);
    if (text[0]) swkbdSetInitialText(&g_search_keyboard, text);
    if (swkbdInputText(&g_search_keyboard, text, sizeof(text)) != SWKBD_BUTTON_RIGHT) return;
    snprintf(g_app.search_text, sizeof(g_app.search_text), "%s", text);
    submit_job(NET_JOB_SEARCH, current_service() == SERVICE_GFN ? "Searching the GeForce NOW catalog..."
               : "Searching...", g_app.search_text, NULL);
}

/* Everything a new session of this game needs, before any request. */
static void prepare_game_session(const GfnGame *game)
{
    if (game != &g_current_game) g_current_game = *game;
    snprintf(g_app.game_title, sizeof(g_app.game_title), "%s", g_current_game.title);
    g_app.session_game = &g_current_game;
    snprintf(g_app.game_store, sizeof(g_app.game_store), "%s", g_current_game.store);
    g_app.genshin_session = strstr(g_current_game.title, "Genshin") != NULL;
    g_app.stream_started_at = 0;
    g_app.end_note[0] = '\0';
    g_app.zone_index = -1;
    g_app.sound_muted = false;
    zoom_zones_select(g_current_game.app_id);
    g_app.free_tier_guess = false;
    g_app.reconnect_attempt = 0;
    g_app.controls_open = false;
    g_app.stream_frame_base = mvd_video_decoded_frames();
    settings_apply_picture(&g_app.settings);
    g_app.auto_weak = false;
    g_app.recover_tried = false;
    g_app.setup_retries = 0;
    g_app.end_logged = false;
    if (!g_app.settings.net_weak && net_memory_choppy_here()) {
        stream_profile_set_weak(true);
        g_app.auto_weak = true;
        diagnostic_log("NET", "the last Standard session on this network was choppy: Weak / hotspot for this one");
        show_notice("Choppy here last time: using Weak / hotspot mode");
    }
    diagnostic_log("VIDEO", "launch profile=%s %ux%u@%u initial=%u min=%u max=%u dynamic=%u sharpen=%d",
                   stream_profile_name(), stream_profile_width(), stream_profile_height(), stream_profile_fps(),
                   stream_profile_initial_bitrate(), stream_profile_min_bitrate(),
                   stream_profile_max_bitrate(), stream_profile_dynamic_mode(),
                   stream_profile_sharpen());
    diagnostic_checkpoint();
}

/* Launch records only go out with "Share performance stats" on. */
static const char *launch_share_id(void)
{
    return g_app.settings.share_stats ? g_app.settings.install_id : NULL;
}

static void launch_game(const GfnGame *game)
{
    static u64 last_launch;
    const u64 now = osGetTime();
    /* Only NVIDIA refuses launches that come too fast. */
    if (current_service() == SERVICE_GFN && last_launch && now - last_launch < 8000) {
        show_notice("One moment - launching again too fast makes NVIDIA refuse for a minute");
        return;
    }
    last_launch = now;
    prepare_game_session(game);
    g_app.limit_wait_until = g_app.limit_retry_at = 0;
    g_app.limit_unclosable = g_app.limit_rate = g_app.limit_busy = false;
    launch_begin(false, stream_profile_weak(), g_app.auto_weak);
    static char busy[128];
    if (current_service() == SERVICE_STEAM)
        snprintf(busy, sizeof(busy), "Starting %.60s on %.40s...", game->title, steam_link_host_name());
    else if (current_service() == SERVICE_XBOX)
        snprintf(busy, sizeof(busy), "Asking Xbox Cloud Gaming for a console...");
    else
        snprintf(busy, sizeof(busy), "Creating your cloud session...");
    submit_job(NET_JOB_START_SESSION, busy, NULL, &g_current_game);
    if (g_app.settings.voice_cues) menu_audio_cue(MENU_CUE_ITTERASSHAI);
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
    log_session_end("user");
    g_perf.user_left = true;
    launch_end("cancel", launch_share_id());
    g_app.limit_wait_until = g_app.limit_retry_at = 0;
    close_media();
    /* Already ending it: a second Leave must not cancel that DELETE (beta.21
     * report H4R-NPH: "Session stop network: Cancelled", and the next launch
     * was refused per device). */
    if (net_worker_current_job() == NET_JOB_STOP_SESSION) {
        g_leave_pending = false;
        return;
    }
    /* If the worker is mid-request, cancel it and stop once it is free. */
    if (net_worker_busy()) {
        net_worker_cancel();
        g_leave_pending = true;
        return;
    }
    g_leave_pending = false;
    submit_job(NET_JOB_STOP_SESSION, "Closing the cloud session...", NULL, NULL);
}

static bool session_gone(void)
{
    return g_signal.state == NVST_SIGNAL_ERROR && (g_signal.upgrade_http == 404 || g_signal.upgrade_http == 410);
}

static void fps60_latency_guard(u64 now);

static void retry_session(void)
{
    close_media();
    g_app.stream_frame_base = mvd_video_decoded_frames();
    /* The rig may still be ours even when the signalling says otherwise:
     * ask CloudMatch, which RESUMEs a paused session (OpenNOW desktop's
     * recovery). A failed session starts over. */
    const bool dead = g_client.session_state == GFN_SESSION_ERROR || !g_client.session_id[0];
    if (!dead)
        submit_job(NET_JOB_RECOVER, "Reconnecting to the cloud rig...", NULL, &g_current_game);
    else if (g_current_game.app_id[0]) {
        /* A new session: its play time, summary and history start again.
         * Beta.27 kept the first session's start time, so a relaunched
         * game "ended after 600 s" when it had run for 20. */
        log_session_end(g_app.stream_started_at ? "restart" : "restart-before-stream");
        finish_history();
        if (g_perf.active) perf_end(g_app.settings.install_id);
        g_app.stream_started_at = 0;
        g_app.setup_retries = 0;
        g_app.recover_tried = false;
        g_app.end_logged = false;
        launch_begin(false, stream_profile_weak(), g_app.auto_weak);
        submit_job(NET_JOB_RESTART_SESSION, "Restarting your cloud session...", NULL, &g_current_game);
    }
}

static void apply_session_input(void);
static void open_mapping(const GamePrefs *prefs);

static void save_settings(void)
{
    /* Mid-game this keeps the game's own camera speeds and button map
     * (the stream menu's layout switch used to drop a game's map). */
    apply_session_input();
    settings_save_async(&g_app.settings);
    if (settings_save_failed()) show_notice("Settings could not be saved to SD");
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

static u64 g_shortcut_launch_at;

/* The Discord invite, once per console: in the library, when nothing else
 * is open (after What's new and the first-run guide). */
static void discord_invite_tick(void)
{
    if (g_app.settings.discord_seen || g_app.discord_open || g_app.view != VIEW_LIBRARY ||
        g_app.modal != MODAL_NONE || g_app.whats_new_open || g_app.guide_page >= 0 || g_app.update_open ||
        g_app.settings_open || g_app.details_open || g_app.busy || g_shortcut_launch_at)
        return;
    g_app.discord_open = true;
    diagnostic_log("UI", "discord invite shown");
}

static void handle_discord(u32 down, AppAction action)
{
    if ((down & (KEY_A | KEY_B | KEY_START)) || action == ACTION_DISCORD_CLOSE) {
        g_app.discord_open = false;
        g_app.settings.discord_seen = true;
        save_settings();
    }
}

/* A background SD write failed (full or locked card, a bad sector): say so,
 * at most once a minute. Settings, history and the one-time Discord invite
 * were lost silently before (a beta.35 tester saw the invite every launch). */
static void sd_write_watch(void)
{
    static unsigned seen;
    static u64 told_at;
    const unsigned failed = file_worker_failed_writes();
    if (failed == seen) return;
    seen = failed;
    const u64 now = osGetTime();
    if (told_at && now - told_at < 60000) return;
    told_at = now;
    show_notice("Couldn't save to the SD card: check it isn't full or locked");
}

/* Quiet daily check from the menus; never during a session. */
static void auto_update_check(void)
{
    static u64 started_at;
    if (!started_at) started_at = osGetTime();
    if (!g_app.settings.auto_update || osGetTime() - started_at < 4000) return;
    if (g_app.view != VIEW_LIBRARY && g_app.view != VIEW_HUB) return;
    if (net_worker_busy() || gfn_session_active(&g_client) || !updater_check_due()) return;
    if (updater_info().state != UPDATE_IDLE) return;
    start_update_check(true);
}

static const char *const SERVICE_NAMES[SERVICE_COUNT] = { "GeForce NOW", "Xbox Cloud Gaming", "Steam Link" };

/* Entering a service from the hub. Each service keeps its login and library on
 * the SD card, so switching only changes which one is shown. */
static bool switch_service(unsigned service)
{
    if (service >= SERVICE_COUNT) return false;
    if (service == current_service()) return true;
    if (gfn_session_active(&g_client) || g_transport.peer ||
        (net_worker_busy() && !background_job(net_worker_current_job()))) {
        show_notice("Finish what's running first, then switch");
        return false;
    }
    g_app.settings.xbox_service = service == SERVICE_XBOX;
    g_app.settings.steam_service = service == SERVICE_STEAM;
    save_settings();
    steam_link_select(g_app.settings.steam_service);
    xcloud_select(g_app.settings.xbox_service);
    g_app.search_text[0] = '\0';
    g_app.selected = g_app.list_top = 0;
    g_app.details_open = false;
    g_app.continue_index = -1;
    /* Quick (the SD card): no busy card, the tab changes at once. */
    submit_job(NET_JOB_SWITCH_SERVICE, NULL, NULL, NULL);
    diagnostic_log("APP", "cloud service %s", SERVICE_NAMES[service]);
    return true;
}

static void change_setting(int direction)
{
    const int index = screens_setting_at(g_app.setting_index);
    if (index == SETTING_ACCOUNT) {
        if (gfn_has_session(&g_client))
            open_modal(MODAL_SIGN_OUT, "サインアウト", "SIGN OUT?", steam_link_selected()
                       ? "This 3DS will forget your PC. Pair it again to stream from it."
                       : xcloud_selected()
                       ? "The saved Microsoft (Xbox) login will be deleted from the SD card. "
                         "Your NVIDIA login stays."
                       : "The saved NVIDIA login will be deleted from the SD card.");
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
    if (index == SETTING_COMMUNITY) return; /* the QR code on the bottom screen is the whole row */
    if (index == SETTING_SCREENSHOTS) {
        if (gallery_open()) g_app.gallery_open = true;
        else show_notice("No screenshots yet: in a game, open the stream menu and pick Screenshot");
        return;
    }
    if (index == SETTING_SHARE_STATS) {
        screens_setting_change(&g_app, index, direction);
        if (!g_app.settings.share_stats) {
            file_worker_remove(REPORT_STATS_PENDING_PATH);
            file_worker_remove(LAUNCH_PENDING_PATH);
            report_stats_mark_pending(false);
        }
        diagnostic_log("REPORT", "share stats %s", g_app.settings.share_stats ? "on" : "off");
        return;
    }
    if (index == SETTING_SHARE) {
        screens_setting_change(&g_app, index, direction);
        diagnostic_log("REPORT", "share diagnostics %s", g_app.settings.share_reports == SHARE_YES ? "on" : "off");
        return;
    }
    if (index == SETTING_REPORT) {
        if (!report_available()) {
            show_notice("Diagnostic reports are not available in this build");
            return;
        }
        open_modal(MODAL_SEND_REPORT, "報告", "SEND A REPORT?",
                   "Sends this run's and the last run's diagnostic log, your settings and any recent "
                   "crash dump to Kasumi's developer. No login or passwords; IP addresses shortened. Kept 30 days.");
        return;
    }
    if (index == SETTING_UPDATES) {
        open_updates();
        return;
    }
    if (index == SETTING_MAPPING) {
        open_mapping(NULL);
        return;
    }
    screens_setting_change(&g_app, index, direction);
    settings_apply_input(&g_app.settings);
    settings_apply_picture(&g_app.settings);
}

/* ---- Voice chat -------------------------------------------------------------- */

/* The mic runs while a game streams with Settings > Microphone on and the
 * session's answer included the mic track; it starts muted (MIC button),
 * and closing the lid mutes it. */
static void mic_tick(bool streaming)
{
    static bool failed_this_game;
    webrtc_transport_set_mic(g_app.settings.mic);
    const bool want = streaming && g_app.settings.mic && g_transport.mic_negotiated &&
                      g_transport.state == WEBRTC_CONNECTED;
    if (!streaming) failed_this_game = false;
    if (want && !mic_capture_running() && !failed_this_game) {
        if (!mic_capture_start()) {
            failed_this_game = true;
            show_notice("The microphone could not start");
        }
    } else if (!want && mic_capture_running()) {
        mic_capture_stop();
    }
    if (g_app.lid_paused && mic_capture_running() && !mic_capture_muted()) mic_capture_set_muted(true);
    g_app.mic_available = mic_capture_running();
}

/* ---- Touch L3 / R3 / PS ----------------------------------------------------- */

/* The touch screen reads two fingers as one point halfway between them, so
 * a finger on L3 and another on R3 shows up in the middle: the stats panel
 * (which is itself an L3 + R3 zone) or, lower down, the PS button. A jump
 * from L3 or R3 into the middle is the second finger arriving; both stay
 * held until the reading lands back on L3 or R3 (one finger lifted). */
static uint16_t stream_touch_buttons(bool touch_down, touchPosition touch)
{
    static uint16_t last;
    static int last_x;
    static bool both;
    const uint16_t sticks = GFN_PAD_LEFT_THUMB | GFN_PAD_RIGHT_THUMB;
    if (!g_app.touching) {
        last = 0;
        both = false;
        return 0;
    }
    const int x = touch.px;
    uint16_t buttons = screens_stream_held_buttons(&g_app, x, touch.py);
    if (!g_app.look_mode && !touch_down && !g_transport.pointer_mode && !mvd_video_zoomed()) {
        const bool one_stick = buttons == GFN_PAD_LEFT_THUMB || buttons == GFN_PAD_RIGHT_THUMB;
        const bool was_one_stick = last == GFN_PAD_LEFT_THUMB || last == GFN_PAD_RIGHT_THUMB;
        if (both && one_stick) both = false;
        else if (was_one_stick && abs(x - last_x) >= 60 && x >= 68 && x <= 252) both = true;
    }
    if (touch_down) both = false;
    last = buttons;
    last_x = x;
    return both ? sticks : buttons;
}

/* ---- Touch camera ---------------------------------------------------------- */

/* The running game's options (its custom button map survives menu toggles). */
static GamePrefs g_session_prefs;

/* Settings > Controls > Touch camera, or this game's own choice. */
static unsigned touch_camera_mode(void)
{
    const int own = g_session_prefs.touch_camera;
    return own >= 0 && own < 3 ? (unsigned)own : g_app.settings.touch_camera;
}

/* Stick: full push this far from the centre, like the C-Stick's rim
 * (Settings > Controls > Touch C-stick size). */
static float look_radius(void)
{
    static const float RADIUS[3] = { 24.0f, 34.0f, 46.0f };
    return RADIUS[g_app.settings.touch_stick_size < 3 ? g_app.settings.touch_stick_size : 1];
}
#define LOOK_RADIUS look_radius()
/* The resistive panel drops a light touch for a frame or two; a finger
 * that comes back within this long, near where it was, never left. */
#define LOOK_GRACE_MS 160
/* One sample this far from the last is panel noise unless the next agrees. */
#define LOOK_JUMP_PX 60

static struct {
    bool active;
    int last_x, last_y;
    /* Stick mode: the centre, dragged along when pushed past the rim. */
    float cx, cy;
    u64 began_at, last_move_at, lost_at;
    unsigned travel;
    float vx, vy;
    /* The push sent last (kept through a dropped touch). */
    float sx, sy;
    bool jump_pending;
    u64 tap_at;
    int tap_x, tap_y;
} g_look;

static void look_trail_push(int x, int y, u64 now)
{
    const unsigned i = g_app.look_trail_head++ % 10;
    g_app.look_trail_x[i] = x;
    g_app.look_trail_y[i] = y;
    g_app.look_trail_at[i] = now;
}

static void look_release(u64 now)
{
    if (!g_look.active) return;
    g_look.active = false;
    g_look.lost_at = 0;
    g_look.sx = g_look.sy = 0.0f;
    g_app.look_active = false;
    g_app.look_r3 = false;
    g_app.look_amount = 0.0f;
    g_app.look_released_at = now;
    g_app.look_release_x = g_look.last_x;
    g_app.look_release_y = g_look.last_y;
    /* A short touch that barely moved is a tap; two close together hold R3. */
    if (g_look.travel <= 8 && now - g_look.began_at < 250) {
        g_look.tap_at = now;
        g_look.tap_x = g_look.last_x;
        g_look.tap_y = g_look.last_y;
    }
}

static void look_begin(int x, int y, u64 now)
{
    g_look.active = true;
    g_look.last_x = x;
    g_look.last_y = y;
    g_look.cx = (float)x;
    g_look.cy = (float)y;
    g_look.began_at = g_look.last_move_at = now;
    g_look.lost_at = 0;
    g_look.travel = 0;
    g_look.vx = g_look.vy = g_look.sx = g_look.sy = 0.0f;
    g_look.jump_pending = false;
    g_app.look_active = true;
    g_app.look_anchor_x = x;
    g_app.look_anchor_y = y;
    const int tx = x - g_look.tap_x, ty = y - g_look.tap_y;
    g_app.look_r3 = g_look.tap_at && now - g_look.tap_at < 300 && tx * tx + ty * ty < 28 * 28;
    g_look.tap_at = 0;
    look_trail_push(x, y, now);
}

/* The pad as a right stick. Stick mode is a C-Stick under the finger: the
 * push is the distance from where it landed, held for as long as the
 * finger stays there, and it goes through the C-Stick's own deadzone and
 * Camera stick speed. Trackpad mode turns by the finger's speed instead. */
static void look_tick(bool touch_down, bool touching, touchPosition touch)
{
    const u64 now = osGetTime();
    const unsigned mode = g_app.look_mode;
    if (!mode || g_app.stream_menu || g_app.controls_open || g_app.modal != MODAL_NONE) {
        look_release(now);
        gfn_input_set_touch_stick(0.0f, 0.0f);
        gfn_input_set_touch_look(0.0f, 0.0f);
        return;
    }
    const int x = touch.px, y = touch.py;
    g_app.look_radius = LOOK_RADIUS;
    if (g_look.active && !touching) {
        /* Contact lost: hold the push for a moment before letting go. */
        if (!g_look.lost_at) g_look.lost_at = now;
        if (now - g_look.lost_at > LOOK_GRACE_MS) look_release(now);
    } else if (g_look.active && g_look.lost_at) {
        /* Back: the same finger if it is near, else a new touch. */
        const int jx = x - g_look.last_x, jy = y - g_look.last_y;
        g_look.lost_at = 0;
        if (jx * jx + jy * jy > LOOK_JUMP_PX * LOOK_JUMP_PX) look_release(now);
        else g_look.last_move_at = now;
    }
    if (!g_look.active && touching && touch_down && ui_hit(screens_look_pad(), x, y) &&
        !ui_hit(screens_look_r3(), x, y) && !ui_hit(screens_look_hide(), x, y) &&
        !ui_hit(screens_look_both(), x, y) && !(g_app.mic_available && ui_hit(screens_look_mic(), x, y)))
        look_begin(x, y, now);

    if (g_look.active && touching) {
        int dx = x - g_look.last_x, dy = y - g_look.last_y;
        if (dx * dx + dy * dy > LOOK_JUMP_PX * LOOK_JUMP_PX && !g_look.jump_pending) {
            /* A wild sample from a light press: wait for the next one. */
            g_look.jump_pending = true;
            dx = dy = 0;
        } else {
            g_look.jump_pending = false;
        }
        const int fx = g_look.last_x + dx, fy = g_look.last_y + dy;
        if (dx || dy) {
            g_look.travel += (unsigned)(abs(dx) + abs(dy));
            look_trail_push(fx, fy, now);
        }
        if (mode == 1) {
            float ox = (float)fx - g_look.cx, oy = (float)fy - g_look.cy;
            const float d = sqrtf(ox * ox + oy * oy);
            if (d > LOOK_RADIUS) {
                /* Past the rim the centre follows, so turning back is
                 * immediate, like letting a stick spring back. */
                g_look.cx += ox * (1.0f - LOOK_RADIUS / d);
                g_look.cy += oy * (1.0f - LOOK_RADIUS / d);
                ox = (float)fx - g_look.cx;
                oy = (float)fy - g_look.cy;
            }
            g_look.sx = ox / LOOK_RADIUS;
            g_look.sy = -oy / LOOK_RADIUS;
            g_app.look_anchor_x = (int)(g_look.cx + 0.5f);
            g_app.look_anchor_y = (int)(g_look.cy + 0.5f);
        } else {
            /* The panel reports less often than this loop runs, so speed is
             * measured between real moves. Full push at 260 px a second. */
            if (dx || dy) {
                u64 dt = now - g_look.last_move_at;
                if (dt < 8) dt = 8;
                if (dt > 100) dt = 100;
                g_look.vx += ((float)dx * 1000.0f / (float)dt / 260.0f - g_look.vx) * 0.6f;
                g_look.vy += (-(float)dy * 1000.0f / (float)dt / 260.0f - g_look.vy) * 0.6f;
                g_look.last_move_at = now;
            } else if (now - g_look.last_move_at > 40) {
                g_look.vx *= 0.6f;
                g_look.vy *= 0.6f;
            }
            g_look.sx = g_look.vx;
            g_look.sy = g_look.vy;
        }
        g_look.last_x = fx;
        g_look.last_y = fy;
    } else if (g_look.active && mode != 1) {
        /* Trackpad through a dropped touch: ease off. */
        g_look.sx *= 0.6f;
        g_look.sy *= 0.6f;
    }
    g_app.look_x = g_look.last_x;
    g_app.look_y = g_look.last_y;

    float sx = g_look.active ? g_look.sx : 0.0f, sy = g_look.active ? g_look.sy : 0.0f;
    const float magnitude = sqrtf(sx * sx + sy * sy);
    if (mode == 1) {
        g_app.look_amount = magnitude > 1.0f ? 1.0f : magnitude;
        gfn_input_set_touch_stick(sx, sy);
        gfn_input_set_touch_look(0.0f, 0.0f);
        return;
    }
    /* Trackpad: a small dead spot, then a floor over the game's own stick
     * deadzone so slow drags still turn. */
    if (magnitude < 0.04f) {
        sx = sy = 0.0f;
        g_app.look_amount = 0.0f;
    } else {
        const float amount = 0.12f + 0.88f * (magnitude > 1.0f ? 1.0f : magnitude);
        sx = sx / magnitude * amount;
        sy = sy / magnitude * amount;
        g_app.look_amount = amount;
    }
    gfn_input_set_touch_stick(0.0f, 0.0f);
    gfn_input_set_touch_look(sx, sy);
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
    if (osGetTime() - g_modal_opened_at < 300) return;
    if (g_app.modal == MODAL_STEAM_LEAVE) {
        /* A: disconnect (the game keeps running), X: quit it on the PC, B: back. */
        const bool disconnect = (down & KEY_A) || action == ACTION_CONFIRM;
        const bool quit = (down & KEY_X) || action == ACTION_DISMISS;
        if (disconnect || quit || (down & KEY_B)) g_app.modal = MODAL_NONE;
        if (quit) {
            const int sent = webrtc_transport_steam_quit_game(&g_transport);
            if (sent == 0) show_notice("No game was open on the PC; only the stream was closed");
            else if (sent < 0) show_notice("Couldn't reach the PC to close the game");
        }
        if (disconnect || quit) leave_session();
        return;
    }
    if (g_app.modal == MODAL_PROVIDER_PICK) {
        /* A: the partner, X: NVIDIA, B: back. The answer is kept. */
        GfnProvider partner;
        providers_partner_here(&partner, NULL);
        const bool pick_partner = (down & KEY_A) || action == ACTION_CONFIRM;
        const bool pick_nvidia = (down & KEY_X) || action == ACTION_DISMISS;
        if (pick_partner || pick_nvidia) {
            snprintf(g_app.settings.provider, sizeof(g_app.settings.provider), "%s",
                     pick_partner ? partner.code : PROVIDER_NVIDIA);
            save_settings();
            g_app.modal = MODAL_NONE;
            diagnostic_log("AUTH", "provider picked at sign-in: %s", g_app.settings.provider);
            begin_login();
        } else if (down & KEY_B) {
            g_app.modal = MODAL_NONE;
        }
        return;
    }
    const bool confirm = (down & KEY_A) || action == ACTION_CONFIRM || action == ACTION_RETRY;
    const bool dismiss = (down & KEY_B) || action == ACTION_DISMISS;
    if (!confirm && !dismiss) return;
    const AppModal modal = g_app.modal;
    g_app.modal = MODAL_NONE;
    if (modal == MODAL_SHARE_ASK) {
        g_app.settings.share_reports = confirm ? SHARE_YES : SHARE_NO;
        g_app.settings.share_stats = confirm;
        g_app.settings.share_consent = SHARE_CONSENT_VERSION;
        diagnostic_log("REPORT", "share diagnostics answered %s", confirm ? "yes" : "no");
        if (!confirm) {
            file_worker_remove(REPORT_STATS_PENDING_PATH);
            file_worker_remove(LAUNCH_PENDING_PATH);
            report_stats_mark_pending(false);
        }
        save_settings();
        show_notice(confirm ? "Thank you! Change it anytime in Settings > System"
                            : "Nothing will be sent. Change it anytime in Settings > System");
        return;
    }
    if (modal == MODAL_RESUME) {
        if (confirm) {
            /* The rig is already ours: set the game up and let signalling
             * start as for any ready session. */
            prepare_game_session(&g_client.resume_game);
            launch_begin(true, stream_profile_weak(), g_app.auto_weak);
            const GamePrefs none = game_prefs_none();
            apply_game_options(&none);
            const GamePrefs prefs = game_prefs_get(g_current_game.app_id);
            apply_game_options(&prefs);
            diagnostic_log("APP", "resuming %s", g_current_game.title);
        } else {
            diagnostic_log("APP", "ending the session left running");
            leave_session();
        }
        return;
    }
    if (modal == MODAL_CONFLICT) {
        if (confirm && g_app.conflict_same_game) {
            diagnostic_log("APP", "resuming the session already running");
            submit_job(NET_JOB_CLAIM_CONFLICT, "Taking over your running game...", NULL, &g_current_game);
        } else if (confirm) {
            diagnostic_log("APP", "ending the other session to start %s", g_current_game.title);
            submit_job(NET_JOB_END_CONFLICT, "Closing your other session...", NULL, &g_current_game);
        } else {
            launch_end("limit", launch_share_id());
        }
        return;
    }
    if (modal == MODAL_LIMIT_WAIT) {
        if (confirm && osGetTime() - g_limit_last_try < 20000) {
            /* Pressing Try now over and over drew NVIDIA's rate limit (429)
             * in beta.21 reports: one try per 20 s. */
            g_app.modal = MODAL_LIMIT_WAIT;
            show_notice("Just tried - wait a few seconds before trying again");
        } else if (confirm) {
            g_app.limit_retry_at = osGetTime();
        } else {
            diagnostic_log("APP", "stopped waiting for NVIDIA to free the slot");
            g_app.limit_wait_until = g_app.limit_retry_at = 0;
            launch_end(g_app.limit_busy ? "limited" : g_app.limit_rate ? "429" : "limit", launch_share_id());
        }
        return;
    }
    if (dismiss) return;
    if (modal == MODAL_SEND_REPORT) {
        submit_job(NET_JOB_SEND_REPORT, "Sending diagnostic report...", NULL, NULL);
        return;
    }
    if (modal == MODAL_SHORTCUT_REMOVE) {
        const GfnGame *game = app_game(&g_app, g_app.selected);
        if (game && shortcut_request_remove(game->app_id)) g_app.shortcut_sheet = SHORTCUT_SHEET_WORKING;
        return;
    }
    if (modal == MODAL_DELETE_SHOT) {
        if (!gallery_delete_current()) show_notice("The screenshot could not be deleted");
        else if (!gallery_count()) {
            gallery_close();
            g_app.gallery_open = false;
            show_notice("No screenshots left");
        }
        return;
    }
    if (modal == MODAL_EXIT) {
        g_quit = true;
    } else if (modal == MODAL_SIGN_OUT) {
        submit_job(NET_JOB_SIGN_OUT, NULL, NULL, NULL);
        g_app.pc_sheet_open = false;
        g_app.continue_index = -1;
        g_app.settings_open = false;
        g_app.search_text[0] = '\0';
        g_app.selected = g_app.list_top = 0;
    } else if (modal == MODAL_ERROR && g_current_game.app_id[0]) {
        launch_game(&g_current_game);
    }
}

/* Sign in. Where the internet address says a partner runs GeForce NOW and
 * no provider was chosen yet, ask first which one the account is from:
 * guessing went wrong both ways (beta.21: a Chilean NVIDIA account sent
 * through Digevo; beta.29: an Australian on NVIDIA, refused every game). */
static void sign_in(void)
{
    GfnProvider partner;
    bool only = false;
    if (current_service() == SERVICE_GFN && !g_app.settings.provider[0] &&
        providers_partner_here(&partner, &only)) {
        char country[4], text[192];
        providers_country(country, sizeof(country));
        snprintf(text, sizeof(text), only
                 ? "You're in %s, where GeForce NOW is run by %s. Pick NVIDIA only if you signed up on nvidia.com."
                 : "You're in %s, where GeForce NOW is also sold by %s. Pick the one you signed up with.",
                 providers_country_name(country), partner.name);
        open_modal(MODAL_PROVIDER_PICK, "選択", "WHERE'S YOUR ACCOUNT FROM?", text);
        return;
    }
    begin_login();
}

/* A service picked on the hub: show its library, and sign in when it has
 * no login yet (first run asks only for the service chosen). */
static void pick_service(unsigned service)
{
    if (!g_app.settings.hub_done) {
        g_app.settings.hub_done = true;
        save_settings();
    }
    if (service == current_service()) {
        g_app.hub_open = false;
        return;
    }
    if (!switch_service(service)) return;
    g_app.hub_open = false;
}

/* What each hub card says: the selected service from the client, the
 * others from their saved logins. */
static void refresh_service_status(void)
{
    char pc[64] = "";
    const bool paired = steam_link_saved_pc(pc, sizeof(pc));
    const bool ready[SERVICE_COUNT] = {
        current_service() == SERVICE_GFN ? gfn_has_session(&g_client) : gfn_login_saved(),
        current_service() == SERVICE_XBOX ? gfn_has_session(&g_client) : xcloud_login_saved(),
        paired
    };
    for (int i = 0; i < SERVICE_COUNT; ++i) g_app.service_ready[i] = ready[i];
    snprintf(g_app.service_status[SERVICE_GFN], sizeof(g_app.service_status[0]), "%s",
             ready[SERVICE_GFN] ? "Signed in" : "Not signed in");
    snprintf(g_app.service_status[SERVICE_XBOX], sizeof(g_app.service_status[0]), "%s",
             ready[SERVICE_XBOX] ? "Signed in" : "Not signed in");
    if (paired) snprintf(g_app.service_status[SERVICE_STEAM], sizeof(g_app.service_status[0]), "PC: %.40s", pc);
    else snprintf(g_app.service_status[SERVICE_STEAM], sizeof(g_app.service_status[0]), "No PC paired");
}

/* The current service's card follows its login as it loads or ends (the
 * others were read from the SD card when the hub opened). No SD reads. */
static void refresh_current_status(void)
{
    const unsigned s = current_service();
    const bool ready = gfn_has_session(&g_client);
    g_app.service_ready[s] = ready;
    if (s == SERVICE_STEAM && ready)
        snprintf(g_app.service_status[s], sizeof(g_app.service_status[0]), "PC: %.40s", steam_link_host_name());
    else if (s == SERVICE_STEAM)
        snprintf(g_app.service_status[s], sizeof(g_app.service_status[0]), "No PC paired");
    else
        snprintf(g_app.service_status[s], sizeof(g_app.service_status[0]), "%s",
                 ready ? "Signed in" : "Not signed in");
}

static void open_hub(void)
{
    refresh_service_status();
    g_app.hub_open = true;
    g_app.pc_sheet_open = false;
    g_app.hub_index = (int)current_service();
}

/* A on the hub: the card grows to fill the screen, then its service
 * opens (enter_tick). */
static int g_entering = -1;

static void enter_service(int service)
{
    if (service < 0 || service >= SERVICE_COUNT) return;
    g_app.hub_index = service;
    g_entering = service;
    screens_hub_zoom(service, true);
    sfx_play(SFX_NOTICE);
}

static void enter_tick(void)
{
    if (g_entering < 0 || screens_zoom_busy()) return;
    const int service = g_entering;
    g_entering = -1;
    pick_service((unsigned)service);
}

/* B in a service: back out to the hub, the card shrinking into place. */
static void go_home(void)
{
    open_hub();
    screens_hub_zoom((int)current_service(), false);
}

/* The focused card's last game, when it is the service in use and has one. */
static const GfnGame *hub_continue_game(void)
{
    if (g_app.hub_index != (int)current_service() || !gfn_has_session(&g_client)) return NULL;
    if (g_app.continue_index < 0 || (size_t)g_app.continue_index >= g_client.game_count) return NULL;
    return &g_client.games[g_app.continue_index];
}

static void handle_hub(u32 down, u32 repeat, AppAction action)
{
    if (g_entering >= 0) return;
    /* START (or the Continue row): straight back into the last game. */
    const GfnGame *last = hub_continue_game();
    if (last && ((down & KEY_START) || action == ACTION_CONTINUE)) {
        launch_with_options(last, last->variant_selected);
        return;
    }
    if ((repeat & (KEY_LEFT | KEY_UP | KEY_ZL | KEY_L)) || action == ACTION_PREV) {
        if (g_app.hub_index > 0) --g_app.hub_index;
    } else if ((repeat & (KEY_RIGHT | KEY_DOWN | KEY_ZR | KEY_R)) || action == ACTION_NEXT) {
        if (g_app.hub_index + 1 < SERVICE_COUNT) ++g_app.hub_index;
    }
    if (action == ACTION_SERVICE) {
        enter_service(screens_touched_service());
    } else if (down & KEY_A) {
        enter_service(g_app.hub_index);
    } else if ((down & KEY_SELECT) || action == ACTION_SETTINGS) {
        g_app.settings_open = true;
        g_app.settings_section = -1;
    } else if ((down & KEY_START) || action == ACTION_EXIT) {
        open_modal(MODAL_EXIT, "終了", "EXIT KASUMI?", "Return to the HOME Menu.");
    }
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
    /* "Continue": the library game played most recently on Kasumi. */
    static unsigned seen_prefs = ~0u;
    static size_t seen_count = (size_t)-1;
    static int64_t seen_saved = -1;
    if (seen_prefs != game_prefs_version() || seen_count != g_client.game_count ||
        seen_saved != g_client.library_saved_at || g_app.continue_index < 0) {
        seen_prefs = game_prefs_version();
        seen_count = g_client.game_count;
        seen_saved = g_client.library_saved_at;
        int64_t best = 0;
        g_app.continue_index = -1;
        for (size_t i = 0; i < g_client.game_count; ++i) {
            PlayHistory history;
            if (game_history(&g_client.games[i], &history) && history.last_played > best) {
                best = history.last_played;
                g_app.continue_index = (int)i;
            }
        }
    }
    keep_selection_visible();
}

static void change_tab(int direction)
{
    if (g_app.search_text[0]) return;
    g_app.library_tab = (g_app.library_tab + LIBRARY_TAB_COUNT + direction) % LIBRARY_TAB_COUNT;
    g_app.selected = g_app.list_top = 0;
    rebuild_list();
}

/* Steam Link's PCs sheet: stream from another paired PC, pair one more,
 * or forget the one in use. */
static void handle_pc_sheet(u32 down, u32 repeat, AppAction action)
{
    char names[4][64];
    const int count = (int)steam_link_pcs(names, 4);
    const int rows = count + 3; /* the PCs, PAIR, FORGET, CLOSE */
    if (repeat & KEY_UP) g_app.pc_index = (g_app.pc_index + rows - 1) % rows;
    if (repeat & KEY_DOWN) g_app.pc_index = (g_app.pc_index + 1) % rows;
    if ((repeat & KEY_LEFT) && g_app.pc_index == count + 1) g_app.pc_index = count;
    if ((repeat & KEY_RIGHT) && g_app.pc_index == count) g_app.pc_index = count + 1;
    if (g_app.pc_index >= rows) g_app.pc_index = 0;
    if ((down & KEY_B) || action == ACTION_PC_CLOSE) {
        g_app.pc_sheet_open = false;
        return;
    }
    int pick = -1;
    if (action == ACTION_PC_ROW) pick = g_app.pc_index = screens_touched_pc();
    else if (down & KEY_A) pick = g_app.pc_index;
    if (pick < 0) return;
    if (pick == count + 2) {
        g_app.pc_sheet_open = false;
    } else if (pick == 0 && count) {
        g_app.pc_sheet_open = false;
        load_library();
    } else if (pick < count) {
        /* The busy card keeps the pointer: a static text. */
        static char busy_text[96];
        char index[12];
        snprintf(index, sizeof(index), "%d", pick);
        snprintf(busy_text, sizeof(busy_text), "Switching to %.60s...", names[pick]);
        if (submit_job(NET_JOB_USE_PC, busy_text, index, NULL)) {
            g_app.pc_sheet_open = false;
            g_app.selected = g_app.list_top = 0;
            g_app.details_open = false;
            g_app.continue_index = -1;
            diagnostic_log("APP", "Steam PC %d of %d", pick + 1, count);
        }
    } else if (pick == count) {
        g_app.pc_sheet_open = false;
        begin_login();
    } else if (count) {
        char text[256];
        if (count > 1)
            snprintf(text, sizeof(text), "This 3DS forgets %.60s and streams from %.60s instead. Pair it again "
                     "to stream from it.", names[0], names[1]);
        else
            snprintf(text, sizeof(text), "This 3DS forgets %.60s. Pair it again to stream from it.", names[0]);
        open_modal(MODAL_SIGN_OUT, "削除", "FORGET THIS PC?", text);
    }
}

static void handle_library(u32 down, u32 repeat, AppAction action)
{
    const size_t count = g_app.list_count;
    if (action == ACTION_HUB) {
        go_home();
        return;
    }
    if (g_app.pc_sheet_open) {
        handle_pc_sheet(down, repeat, action);
        return;
    }
    if (!gfn_has_session(&g_client)) {
        /* This service's sign-in card. */
        if ((down & KEY_A) || action == ACTION_SIGN_IN) sign_in();
        else if (down & KEY_B) go_home();
        else if ((down & KEY_SELECT) || action == ACTION_SETTINGS) {
            g_app.settings_open = true;
            g_app.settings_section = -1;
        } else if ((down & KEY_START) || action == ACTION_EXIT) {
            open_modal(MODAL_EXIT, "終了", "EXIT KASUMI?", "Return to the HOME Menu.");
        }
        return;
    }
    if (down & KEY_L) change_tab(-1);
    if (down & KEY_R) change_tab(1);
    if (count) {
        if ((repeat & KEY_LEFT) || action == ACTION_PREV) {
            if (g_app.selected > 0) --g_app.selected;
        }
        if ((repeat & KEY_RIGHT) || action == ACTION_NEXT) {
            if (g_app.selected + 1 < count) ++g_app.selected;
        }
        if (repeat & KEY_UP)
            g_app.selected = g_app.selected > 5 ? g_app.selected - 5 : 0;
        if (repeat & KEY_DOWN)
            g_app.selected = g_app.selected + 5 < count ? g_app.selected + 5 : count - 1;
        keep_selection_visible();
    }
    if (((down & KEY_START) || action == ACTION_CONTINUE) && g_app.continue_index >= 0 &&
        !g_app.search_text[0]) {
        const GfnGame *game = &g_client.games[g_app.continue_index];
        launch_with_options(game, game->variant_selected);
        return;
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
        /* From a search, Y returns to the library; otherwise it refreshes
         * (Steam Link: it opens the PCs). */
        if (g_app.search_text[0]) show_saved_library();
        else if (current_service() == SERVICE_STEAM) {
            g_app.pc_sheet_open = true;
            g_app.pc_index = 0;
        } else load_library();
    } else if ((down & KEY_B) && g_app.search_text[0]) {
        show_saved_library();
    } else if (down & KEY_B) {
        go_home();
    } else if ((down & KEY_SELECT) || action == ACTION_SETTINGS) {
        g_app.settings_open = true;
        g_app.settings_section = -1;
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

/* A game's own map, or its own layout, replaces the map for every game. */
static void apply_game_map(const GamePrefs *prefs)
{
    if (prefs->has_map) gfn_input_set_custom_map(&prefs->map);
    else if (prefs->layout >= 0) gfn_input_set_custom_map(NULL);
}

/* Settings for this game's session: the global ones with its options. */
static void apply_game_options(const GamePrefs *prefs)
{
    AppSettings session = g_app.settings;
    if (prefs->bitrate >= 0 && prefs->bitrate < STREAM_BITRATE_COUNT)
        session.bitrate_mode = (StreamBitrateMode)prefs->bitrate;
    if (prefs->gyro >= 0 && prefs->gyro < GFN_GYRO_MODE_COUNT) session.gyro_mode = (GfnGyroMode)prefs->gyro;
    if (prefs->layout >= 0 && prefs->layout < 2) session.button_layout = (GfnButtonLayout)prefs->layout;
    if (prefs->camera_speed >= 0 && prefs->camera_speed < 4) session.camera_speed = (unsigned)prefs->camera_speed;
    if (prefs->camera_invert >= 0 && prefs->camera_invert < 3) session.camera_invert = (unsigned)prefs->camera_invert;
    if (prefs->gyro_speed >= 0 && prefs->gyro_speed < 3) session.gyro_speed = (unsigned)prefs->gyro_speed;
    settings_apply_input(&session);
    settings_apply_picture(&session);
    /* What this game really streams with (a beta.25 test logged Sharp, then
     * the game's own Adaptive option replaced it). */
    if (prefs->bitrate >= 0) {
        diagnostic_log("VIDEO", "game profile=%s initial=%u min=%u max=%u", stream_profile_name(),
                       stream_profile_initial_bitrate(), stream_profile_min_bitrate(),
                       stream_profile_max_bitrate());
        /* Beta.25 tests: a game's own Adaptive quietly replaced Settings. */
        if ((StreamBitrateMode)prefs->bitrate != g_app.settings.bitrate_mode)
            show_notice("This game uses its own Bitrate (game page, X > Options)");
    }
    g_session_prefs = *prefs;
    apply_game_map(prefs);
}

/* Settings changed mid-game (the stream menu's gyro switch): apply them
 * again, keeping this game's own camera and gyro speeds. */
static void apply_session_input(void)
{
    AppSettings session = g_app.settings;
    const GamePrefs *p = &g_session_prefs;
    if (gfn_session_active(&g_client)) {
        if (p->camera_speed >= 0 && p->camera_speed < 4) session.camera_speed = (unsigned)p->camera_speed;
        if (p->camera_invert >= 0 && p->camera_invert < 3) session.camera_invert = (unsigned)p->camera_invert;
        if (p->gyro_speed >= 0 && p->gyro_speed < 3) session.gyro_speed = (unsigned)p->gyro_speed;
        if (p->layout >= 0 && p->layout < 2) session.button_layout = (GfnButtonLayout)p->layout;
    }
    settings_apply_input(&session);
    if (gfn_session_active(&g_client)) apply_game_map(p);
}

/* Launch a library game from one of its stores, with its own options. */
static void launch_with_options(const GfnGame *base, unsigned variant)
{
    if (!base) return;
    GfnGame game = *base;
    const GamePrefs prefs = game_prefs_get(base->app_id);
    if (variant < game.variant_count) {
        snprintf(game.app_id, sizeof(game.app_id), "%s", game.variants[variant].id);
        snprintf(game.store, sizeof(game.store), "%s", game.variants[variant].store);
    }
    launch_game(&game);
    /* launch_game applied the global picture settings; layer this game's. */
    apply_game_options(&prefs);
    diagnostic_log("APP", "game options bitrate=%d gyro=%d gyroSpeed=%d camera=%d invert=%d layout=%d map=%d",
                   prefs.bitrate, prefs.gyro, prefs.gyro_speed, prefs.camera_speed, prefs.camera_invert,
                   prefs.layout, prefs.has_map);
}

/* ---- Button mapping editor ---------------------------------------------- */

/* The editor for one game (prefs) or, with NULL, for every game. */
static void open_mapping(const GamePrefs *prefs)
{
    const AppSettings *s = &g_app.settings;
    const GfnButtonLayout layout = prefs && prefs->layout >= 0 ? (GfnButtonLayout)prefs->layout : s->button_layout;
    gfn_input_default_map(layout, s->swap_shoulders, &g_app.mapping_default);
    /* A game without a layout of its own starts from the map for every
     * game, and RESET goes back to it. */
    if (prefs && prefs->layout < 0 && s->has_map) g_app.mapping_default = s->map;
    if (prefs) g_app.mapping = prefs->has_map ? prefs->map : g_app.mapping_default;
    else g_app.mapping = s->has_map ? s->map : g_app.mapping_default;
    g_app.mapping_global = prefs == NULL;
    g_app.mapping_input = GFN_IN_A;
    g_app.mapping_field = 0;
    g_app.mapping_open = true;
}

static void handle_mapping(u32 down, u32 repeat, AppAction action)
{
    const GfnGame *game = app_game(&g_app, g_app.selected);
    if (!g_app.mapping_global && !game) { g_app.mapping_open = false; return; }
    /* Pressing a 3DS button picks it (every button, D-Pad included, is
     * itself remappable). The Circle Pad changes the chosen row: up and
     * down the button, left and right the value; the C-Stick or a tap
     * picks the row. */
    for (unsigned i = 0; i < GFN_INPUT_COUNT; ++i)
        if (down & gfn_input_key(i)) g_app.mapping_input = (int)i;
    if (screens_touched_map_field() >= 0 &&
        (action == ACTION_MAP_FIELD || action == ACTION_MAP_PREV || action == ACTION_MAP_NEXT))
        g_app.mapping_field = screens_touched_map_field();
    if (repeat & KEY_CSTICK_UP) g_app.mapping_field = (g_app.mapping_field + 2) % 3;
    if (repeat & KEY_CSTICK_DOWN) g_app.mapping_field = (g_app.mapping_field + 1) % 3;
    int step = 0;
    if ((repeat & KEY_CPAD_LEFT) || action == ACTION_MAP_PREV) step = -1;
    if ((repeat & KEY_CPAD_RIGHT) || action == ACTION_MAP_NEXT) step = 1;
    const int input = g_app.mapping_input;
    if (step) {
        unsigned char *value = g_app.mapping_field == 0 ? &g_app.mapping.out[input]
                             : g_app.mapping_field == 1 ? &g_app.mapping.also[input] : &g_app.mapping.mode[input];
        const int count = g_app.mapping_field == 2 ? GFN_BIND_MODE_COUNT : GFN_OUTPUT_COUNT;
        *value = (unsigned char)((*value + count + step) % count);
    }
    if (repeat & KEY_CPAD_UP) g_app.mapping_input = (g_app.mapping_input + GFN_INPUT_COUNT - 1) % GFN_INPUT_COUNT;
    if (repeat & KEY_CPAD_DOWN) g_app.mapping_input = (g_app.mapping_input + 1) % GFN_INPUT_COUNT;
    if (action == ACTION_MAP_RESET) g_app.mapping = g_app.mapping_default;
    if (action == ACTION_MAP_CANCEL) g_app.mapping_open = false;
    if (action != ACTION_MAP_DONE) return;
    g_app.mapping_open = false;
    const bool custom = !gfn_button_map_equal(&g_app.mapping, &g_app.mapping_default);
    if (g_app.mapping_global) {
        g_app.settings.has_map = custom;
        g_app.settings.map = g_app.mapping;
        save_settings();
        show_notice(custom ? "Button mapping saved for every game" : "Every game uses the normal layout");
        return;
    }
    GamePrefs prefs = game_prefs_get(game->app_id);
    prefs.has_map = custom;
    prefs.map = g_app.mapping;
    game_prefs_set(game->app_id, &prefs);
    show_notice(custom ? "Button mapping saved for this game" : "This game uses the normal mapping");
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
    case OPTION_CAMERA_SPEED: CYCLE(prefs.camera_speed, 4); break;
    case OPTION_CAMERA_INVERT: CYCLE(prefs.camera_invert, 3); break;
    case OPTION_TOUCH_CAMERA: CYCLE(prefs.touch_camera, 3); break;
    case OPTION_GYRO_SPEED: CYCLE(prefs.gyro_speed, 3); break;
    case OPTION_MAPPING:
        if ((down & KEY_A) || action == ACTION_OPTION_NEXT || action == ACTION_OPTION_PREV) open_mapping(&prefs);
        return;
    case OPTION_CONNECTION:
        if ((down & KEY_A) || action == ACTION_OPTION_NEXT)
            submit_job(NET_JOB_CONNECTION_TEST, "Checking your connection to NVIDIA...", NULL, NULL);
        return;
    }
    #undef CYCLE
    game_prefs_set(game->app_id, &prefs);
}

/* Game page: put the game on the HOME Menu, or take it off. */
static void shortcut_action(const GfnGame *game)
{
    if (shortcut_state() != SHORTCUT_IDLE) {
        show_notice("Still making the last shortcut");
        return;
    }
    /* A shortcut names a game of the service it was made in; Steam Link's
     * entries only mean something on the paired PC. */
    if (steam_link_selected()) {
        show_notice("Shortcuts aren't available for Steam Link yet");
        return;
    }
    /* The shortcut jumps to the installed Kasumi title. */
    if (updater_is_3dsx()) {
        show_notice("Shortcuts need Kasumi installed as a CIA (with FBI), not the .3dsx");
        return;
    }
    if (shortcut_exists(game->app_id)) {
        open_modal(MODAL_SHORTCUT_REMOVE, "削除", "REMOVE THE SHORTCUT?",
                   "Takes this game's shortcut off the HOME Menu. You can add it again any time.");
        return;
    }
    g_app.shortcut_message[0] = '\0';
    if (shortcut_request(game, g_app.details_variant)) {
        g_app.shortcut_sheet = SHORTCUT_SHEET_WORKING;
    } else {
        snprintf(g_app.shortcut_message, sizeof(g_app.shortcut_message), "%s",
                 shortcut_result()[0] ? shortcut_result() : "Couldn't start making the shortcut.");
        g_app.shortcut_sheet = SHORTCUT_SHEET_FAILED;
    }
}

/* The sheet over the game page: nothing else on it can be pressed until
 * the shortcut is made, so nobody leaves halfway thinking it stalled. */
static void handle_shortcut_sheet(const GfnGame *game, u32 down, AppAction action)
{
    switch (g_app.shortcut_sheet) {
    case SHORTCUT_SHEET_WORKING:
        if (down & (KEY_B | KEY_A)) show_notice("Almost there: keep Kasumi open");
        return;
    case SHORTCUT_SHEET_FAILED:
        if ((down & KEY_A) || action == ACTION_RETRY) {
            g_app.shortcut_sheet = SHORTCUT_SHEET_NONE;
            shortcut_action(game);
        } else if ((down & KEY_B) || action == ACTION_DISMISS) {
            g_app.shortcut_sheet = SHORTCUT_SHEET_NONE;
        }
        return;
    default:
        if ((down & (KEY_A | KEY_B)) || action == ACTION_CONFIRM) g_app.shortcut_sheet = SHORTCUT_SHEET_NONE;
        return;
    }
}

static void handle_details(u32 down, u32 repeat, AppAction action)
{
    const GfnGame *game = app_game(&g_app, g_app.selected);
    if (!game) { g_app.details_open = false; return; }
    if (g_app.shortcut_sheet != SHORTCUT_SHEET_NONE) {
        handle_shortcut_sheet(game, down, action);
        return;
    }
    if (g_app.options_open || action == ACTION_OPTIONS_CLOSE) {
        handle_options(down, repeat, action);
        return;
    }
    const unsigned variants = game->variant_count;
    if ((down & KEY_A) || action == ACTION_DETAILS_PLAY) {
        launch_with_options(app_game(&g_app, g_app.selected), g_app.details_variant);
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
    } else if ((down & KEY_SELECT) || action == ACTION_SHORTCUT) {
        shortcut_action(game);
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

static void open_settings_section(int section)
{
    g_app.settings_section = section;
    g_app.settings_grid = section;
    g_app.setting_index = screens_section_first(section);
}

static void handle_gallery(u32 down, u32 repeat, AppAction action)
{
    if ((repeat & (KEY_L | KEY_LEFT)) || action == ACTION_GALLERY_PREV) gallery_step(-1);
    if ((repeat & (KEY_R | KEY_RIGHT)) || action == ACTION_GALLERY_NEXT) gallery_step(1);
    if (((down & KEY_X) || action == ACTION_GALLERY_DELETE) && gallery_count())
        open_modal(MODAL_DELETE_SHOT, "削除", "DELETE THIS SCREENSHOT?",
                   "It is removed from the SD card for good.");
    if ((down & KEY_B) || action == ACTION_GALLERY_CLOSE) {
        gallery_close();
        g_app.gallery_open = false;
    }
}

static void handle_settings(u32 down, u32 repeat, AppAction action)
{
    if (g_app.mapping_open) {
        handle_mapping(down, repeat, action);
        return;
    }
    if (g_app.gallery_open) {
        handle_gallery(down, repeat, action);
        return;
    }
    const int sections = screens_section_count();
    /* The grid of sections. */
    if (g_app.settings_section < 0) {
        int *grid = &g_app.settings_grid;
        if (repeat & KEY_LEFT) *grid = (*grid + sections - 1) % sections;
        if (repeat & KEY_RIGHT) *grid = (*grid + 1) % sections;
        if (repeat & (KEY_UP | KEY_DOWN)) *grid = (*grid + 3) % sections;
        if (action == ACTION_SETTINGS_SECTION && screens_touched_section() >= 0)
            open_settings_section(screens_touched_section());
        else if (down & KEY_A)
            open_settings_section(*grid);
        else if ((down & (KEY_B | KEY_SELECT)) || action == ACTION_BACK) {
            save_settings();
            g_app.settings_open = false;
        }
        return;
    }
    /* One section: up and down stay inside it, L and R change it. */
    const int section = g_app.settings_section;
    const int first = screens_section_first(section), size = screens_section_size(section);
    if (repeat & KEY_UP) g_app.setting_index = first + (g_app.setting_index - first + size - 1) % size;
    if (repeat & KEY_DOWN) g_app.setting_index = first + (g_app.setting_index - first + 1) % size;
    if (down & KEY_L) { open_settings_section((section + sections - 1) % sections); return; }
    if (down & KEY_R) { open_settings_section((section + 1) % sections); return; }
    if ((repeat & KEY_LEFT) || action == ACTION_VALUE_PREV) change_setting(-1);
    if ((repeat & KEY_RIGHT) || (down & KEY_A) || action == ACTION_VALUE_NEXT) change_setting(1);
    if ((down & KEY_X) && screens_setting_at(g_app.setting_index) == SETTING_MUSIC) menu_audio_next();
    if ((down & (KEY_B | KEY_SELECT)) || action == ACTION_BACK) {
        if (action == ACTION_BACK && screens_setting_at(g_app.setting_index) == SETTING_ACCOUNT &&
            gfn_has_session(&g_client)) {
            change_setting(1);
            return;
        }
        save_settings();
        /* B goes back to the sections; SELECT leaves Settings. */
        if (down & KEY_SELECT) g_app.settings_open = false;
        g_app.settings_section = -1;
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
            save_settings();
        } else if (action == ACTION_MENU_LAYOUT) {
            /* A button map decides every button, layout or not, and a
             * game's own layout outranks the global one. */
            if (g_session_prefs.has_map || g_session_prefs.layout >= 0) {
                show_notice("This game has its own buttons (game page > X > Options)");
            } else if (gfn_input_custom_map_active()) {
                show_notice("Your button mapping is in use (Settings > Controls)");
            } else {
                screens_setting_change(&g_app, SETTING_LAYOUT, 1);
                save_settings();
            }

        } else if (action == ACTION_MENU_DISCONNECT) {
            if (g_transport.steam) {
                /* The PC keeps the game unless asked: let the player choose. */
                char text[160];
                snprintf(text, sizeof(text), "Disconnect leaves the game running on %.40s. Quit closes it there.",
                         steam_link_host_name()[0] ? steam_link_host_name() : "your PC");
                g_app.stream_menu = false;
                open_modal(MODAL_STEAM_LEAVE, "切断", "LEAVE THE GAME?", text);
            } else {
                leave_session();
            }
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
        show_notice(g_transport.pointer_mode ? "Mouse & keyboard mode: drag to move the mouse, tap or A to click"
                                             : "Controller mode: buttons and sticks act as a gamepad");
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
    case ACTION_LOOK_TOGGLE:
        /* Kept for the next game too. */
        g_app.settings.touch_camera_shown = !g_app.settings.touch_camera_shown;
        settings_save_async(&g_app.settings);
        return;
    case ACTION_STREAM_MENU:
        g_app.stream_menu = true;
        g_app.stream_menu_index = STREAM_MENU_RESUME;
        return;
    case ACTION_MIC_TOGGLE:
        mic_capture_set_muted(!mic_capture_muted());
        show_notice(mic_capture_muted() ? "Mic muted" : "Mic on: your team can hear you");
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
static void queue_auto_report(const char *trigger);

/* ---- Session tracking ------------------------------------------------------ */

/* Lid closed or HOME opened: the app is frozen and the stream times out.
 * The hook only records when and why; track_session() logs it and
 * reconnects afterwards. */
static volatile u64 g_suspended_at, g_resumed_at;
static volatile bool g_suspend_was_sleep;
static aptHookCookie g_apt_cookie;

/* Where the last main-loop pass spent its time, so a ui-stall says which
 * part froze (beta.31: one console stopped for 12.8 s in menus, no job). */
/* PHASE_TRACK (build 122): end-of-game bookkeeping, timed apart from
 * drawing; beta.36's "render" stalls after games were mostly that. */
enum { PHASE_SYNC, PHASE_TICKS, PHASE_HANDLE, PHASE_NETWORK, PHASE_TRACK, PHASE_RENDER, PHASE_COUNT };
static unsigned g_phase_ms[PHASE_COUNT];
static u64 g_phase_at;

static void phase_end(int phase)
{
    const u64 now = osGetTime();
    g_phase_ms[phase] = now > g_phase_at ? (unsigned)(now - g_phase_at) : 0;
    g_phase_at = now;
}

/* Until when a long main-loop gap (the Rosalina menu, a stall) excuses the
 * video-freeze check: the whole app was paused, decoder included, so the gap
 * looked like a frozen picture. Beta.36 export: 20 s in Rosalina, then a
 * needless reconnect. */
static u64 g_loop_gap_grace_until;

/* Any HOME, sleep or applet event: a long main-loop gap around one is not
 * a stall (see watch_for_bugs). */
static volatile unsigned g_apt_events;
/* The buttons held at the last input scan (see rosalina_combo). */
static u32 g_last_held;

/* Luma's Rosalina menu (L + Down + Select) freezes the whole app while it
 * is open, with no HOME or sleep event, and players open it mid-game for
 * screenshots: beta.34 flagged a 57 s "stall" with exactly that combo in
 * the input log. Select with L or Down is close enough to tell. */
static bool rosalina_combo(u32 held)
{
    return (held & KEY_SELECT) && (held & (KEY_L | KEY_DDOWN));
}

static void apt_hook(APT_HookType hook, void *param)
{
    (void)param;
    ++g_apt_events;
    if (hook == APTHOOK_ONSLEEP || hook == APTHOOK_ONSUSPEND) {
        webrtc_transport_pause(true);
        g_suspended_at = osGetTime();
        g_suspend_was_sleep = hook == APTHOOK_ONSLEEP;
    } else if (hook == APTHOOK_ONWAKEUP || hook == APTHOOK_ONRESTORE) {
        webrtc_transport_pause(false);
        g_resumed_at = osGetTime();
    }
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
/* Only when neither the shared estimate nor this console's own queues say
 * anything (queue_eta.h). */
static const float g_seconds_per_place = 1.9f;

static void queue_stats_load(void)
{
    queue_eta_load();
}

/* Connected to an access point (ac:u), checked at most twice a second. */
static bool wifi_connected(void)
{
    static u64 checked_at;
    static bool connected = true;
    const u64 now = osGetTime();
    if (now - checked_at >= 500) {
        checked_at = now;
        u32 status = 0;
        connected = R_SUCCEEDED(ACU_GetWifiStatus(&status)) && status != 0;
    }
    return connected;
}

/* Once per game, after 90 s: a ping or loss that Standard mode on this
 * server can't carry (beta.23: 17 of 67 sessions averaged over 100 ms). */
static void connection_hint(u64 now)
{
    static u64 hinted_for;
    if (hinted_for == g_app.stream_started_at || !g_app.stream_started_at || g_perf.weak) return;
    const u64 played = now - g_app.stream_started_at;
    if (played < 90000 || !g_perf.ping_samples) return;
    const unsigned ping = (unsigned)(g_perf.ping_sum / g_perf.ping_samples);
    const unsigned lost_per_min = (unsigned)((u64)g_perf.lost * 60000u / played);
    if (ping < 120 && lost_per_min < 6) return;
    hinted_for = g_app.stream_started_at;
    diagnostic_log("APP", "connection hint shown ping=%u lostPerMin=%u", ping, lost_per_min);
    show_notice(ping >= 120 ? "High ping: try a closer Server, or Weak / hotspot (Settings > Network)"
                            : "Choppy? Try Weak / hotspot in Settings > Network");
}

/* "Pause": with the lid shut the stream stays connected but the sound and
 * the controls are held; opening it shows a short "Welcome back" card. */
static void track_lid_pause(bool paused)
{
    static u64 paused_at;
    if (paused == g_app.lid_paused) return;
    g_app.lid_paused = paused;
    const u64 now = osGetTime();
    if (paused) {
        paused_at = now;
        g_app.welcome_at = 0;
        diagnostic_log("APP", "lid closed: paused, connection kept");
        return;
    }
    const u64 away = paused_at ? now - paused_at : 0;
    const bool dropped = g_transport.state == WEBRTC_FAILED || g_signal.state == NVST_SIGNAL_ERROR;
    diagnostic_log("APP", "lid opened after %llu ms: %s", (unsigned long long)away,
                   dropped ? "connection dropped, reconnecting" : "resumed without reconnecting");
    if (away >= 1500 && !dropped) {
        g_app.welcome_at = now;
        g_app.welcome_away_s = (unsigned)(away / 1000);
    }
    paused_at = 0;
}

static void track_queue(void)
{
    static u64 queued_at;
    static int start_place;
    static bool alert_pending;
    static u64 lid_checked_at;
    const u64 now = osGetTime();
    /* Lid shut while waiting for a rig, or mid-game with sleep held off
     * ("Pause" and "Keep playing"): screens off to save battery. */
    if (now - lid_checked_at >= 400) {
        lid_checked_at = now;
        const bool closed = queue_alert_lid_closed();
        const bool session = gfn_session_active(&g_client);
        const bool waiting = session && !g_app.stream_started_at;
        const bool playing = session && g_app.stream_started_at && g_app.settings.lid_mode != LID_SLEEP;
        queue_alert_screens((waiting || playing) && closed);
        track_lid_pause(playing && closed && g_app.settings.lid_mode == LID_PAUSE);
    }
    /* After a real queue, the rig being ready is worth a chime and a light. */
    if (alert_pending && g_client.session_state == GFN_SESSION_READY) {
        alert_pending = false;
        queue_alert_start();
    }
    if (!gfn_session_active(&g_client)) alert_pending = false;
    const bool queued = g_client.session_state == GFN_SESSION_QUEUED;
    if (queued) {
        if (!queued_at) queued_at = now;
        if (!start_place && g_client.queue_position > 0) start_place = g_client.queue_position;
        /* Everyone's recent queues with this provider (base + per place),
         * else this console's own, else a rough guess from the place. */
        GfnProvider provider;
        provider_active(&provider);
        float expected = 0.0f;
        if (!queue_eta_expected(provider.code, start_place, &expected)) {
            if (!start_place) { g_app.queue_eta = -1; return; }
            expected = (float)start_place * g_seconds_per_place;
        }
        const float left = expected - (float)(now - queued_at) / 1000.0f;
        g_app.queue_eta = left > 20.0f ? (int)left : 0;
        return;
    }
    /* The queue just ended with a rig: learn from how long it really took. */
    if (queued_at && start_place > 0 &&
        (g_client.session_state == GFN_SESSION_SETUP || g_client.session_state == GFN_SESSION_READY)) {
        const float seconds = (float)(now - queued_at) / 1000.0f;
        alert_pending = seconds >= 20.0f;
        GfnProvider provider;
        provider_active(&provider);
        float guessed = 0.0f;
        const bool had = queue_eta_expected(provider.code, start_place, &guessed);
        queue_eta_learn(provider.code, start_place, seconds);
        diagnostic_log("QUEUE", "finished start=%d seconds=%.0f provider=%s estimate=%.0f", start_place,
                       (double)seconds, provider.code, had ? (double)guessed : -1.0);
    }
    queued_at = 0;
    start_place = 0;
    g_app.queue_eta = -1;
}

/* The 3DS keeps doing background Wi-Fi work (StreetPass, SpotPass and
 * notification checks) while an app streams, and each scan takes the radio
 * off the access point for a moment. Beta.19 logs: the stream stopped for
 * 200-280 ms at a time, frames were lost, and ~35 packets a minute had to be
 * re-sent on a fast, nearby connection. Moonlight-N3DS takes the Wi-Fi
 * exclusively (infrastructure only, background daemons stopped) for the
 * same reason. Kasumi does it only while a game session runs. */
static void wifi_exclusive(bool on)
{
    if (!g_ndm_ready || on == g_ndm_exclusive) return;
    Result result;
    if (on) {
        result = NDMU_EnterExclusiveState(NDM_EXCLUSIVE_STATE_INFRASTRUCTURE);
        if (R_SUCCEEDED(result)) result = NDMU_LockState();
        g_ndm_exclusive = R_SUCCEEDED(result);
        if (!g_ndm_exclusive) NDMU_LeaveExclusiveState();
    } else {
        NDMU_UnlockState();
        result = NDMU_LeaveExclusiveState();
        g_ndm_exclusive = false;
    }
    diagnostic_log("NET", "exclusive Wi-Fi %s rc=%08lX", on ? "on" : "off", (unsigned long)result);
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
        gallery_mark_dirty();
        sfx_play(SFX_SCREENSHOT);
        char text[96];
        snprintf(text, sizeof(text), "Screenshot saved: %s", shot);
        show_notice(text);
    } else if (shot_result < 0) {
        show_notice("Screenshot could not be saved to the SD card");
    }
    audio_output_set_muted(g_app.sound_muted || g_app.lid_paused ||
                           (g_app.settings.mute_in_menus && (g_app.stream_menu || g_app.controls_open)));

    static bool was_active;
    wifi_exclusive(gfn_session_active(&g_client));
    if (!gfn_session_active(&g_client)) {
        finish_history();
        if (g_perf.active) {
            perf_end(g_app.settings.install_id);
            /* A Weak session Kasumi chose by itself counts for the network: when it
             * went smoothly, the next session there tries Standard again. */
            /* Not a Sharp (test) session: a stress test that loses packets on
             * purpose put the next ordinary session into Weak (beta.25 test). */
            if (!stream_profile_test_mode())
                net_memory_note(g_perf.weak && !g_app.auto_weak, g_perf.seconds, g_perf.lost, g_perf.repeated);
            /* A clearly choppy session on Standard: point at Weak / hotspot
             * (beta.17 stats: one console lost ~5 frames a minute). */
            const unsigned minutes = g_perf.seconds / 60;
            if (minutes >= 2 && !g_app.settings.net_weak &&
                (g_perf.lost / minutes >= 3 || g_perf.repeated / minutes >= 15))
                show_notice("Choppy connection? Try Settings > Network > Connection type: Weak / hotspot");
        }
        /* A game's own options only last for its session. */
        if (was_active) {
            was_active = false;
            g_session_prefs = game_prefs_none();
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
        /* Only a pause during this stream counts: an older timestamp would
         * make a short HOME visit look like minutes away. */
        const bool paired = g_suspended_at >= g_app.stream_started_at && g_resumed_at > g_suspended_at;
        const u64 away = paired ? g_resumed_at - g_suspended_at : 0;
        if (away < 4000) {
            if (paired)
                diagnostic_log("APP", "system %s for %llu ms; stream kept", g_suspend_was_sleep ? "sleep" : "HOME/applet",
                               (unsigned long long)away);
            g_resumed_at = g_suspended_at = 0;
        } else if ((osGetWifiStrength() > 0 || now - g_resumed_at > 15000) && !net_worker_busy() &&
                   g_client.session_state == GFN_SESSION_READY) {
            diagnostic_log("APP", "resumed after %llu ms away (system %s, lid=%s); reconnecting",
                           (unsigned long long)away, g_suspend_was_sleep ? "sleep" : "HOME/applet",
                           queue_alert_lid_closed() ? "closed" : "open");
            g_resumed_at = g_suspended_at = 0;
            g_app.reconnect_attempt = 1;
            reconnect_at = now;
            show_notice("Welcome back - reconnecting to your rig");
            ++g_perf.reconnects;
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
            launch_end("ok", launch_share_id());
            char region[40];
            regions_last_used(region, sizeof(region));
            perf_begin(g_current_game.title, region, stream_profile_weak(), (unsigned)g_app.settings.bitrate_mode);
            play_history_begin(g_current_game.app_id, g_current_game.title);
            g_history_open = true;
        }
        if (!playing_since) playing_since = now;
        connection_hint(now);
        /* Ten clean seconds after a reconnect: the next drop starts afresh. */
        if (g_app.reconnect_attempt && now - playing_since >= 10000) g_app.reconnect_attempt = 0;
        if (g_app.recover_tried && now - playing_since >= 10000) g_app.recover_tried = false;
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
    /* Video frozen for 12 s with nothing else wrong (no lid, Wi-Fi up): the
     * keyframe requests (every 3 s) have not helped, so reconnect. */
    /* The video thread can stamp a frame after `now` was read: unsigned
     * now - last then wrapped to 2^64 and every such reconnect in beta.23
     * reports was bogus (one broke a working game). */
    fps60_latency_guard(now);
    const u64 last_frame = g_transport.last_decoded_frame_at;
    const bool frozen = g_app.view == VIEW_STREAM && g_transport.state == WEBRTC_CONNECTED &&
                        !g_transport.steam_capture_unavailable &&
                        (!g_transport.steam_capture_resumed_at ||
                         (now > g_transport.steam_capture_resumed_at && now - g_transport.steam_capture_resumed_at > 12000)) &&
                        last_frame && now > last_frame && now - last_frame > 12000 &&
                        !g_app.lid_paused && !g_resumed_at && now >= g_loop_gap_grace_until && wifi_connected();
    if (frozen && !net_worker_busy() && g_client.session_state == GFN_SESSION_READY && g_app.reconnect_attempt < 3) {
        diagnostic_flag("video-freeze", "no picture for %llu ms with the connection up (rtp=%u kbps=%u pli=%u); reconnecting",
                        (unsigned long long)(now - g_transport.last_decoded_frame_at),
                        g_transport.video_access_units, g_transport.video_kbps, g_transport.keyframe_requests);
        g_transport.last_decoded_frame_at = now;
        ++g_app.reconnect_attempt;
        reconnect_at = now;
        ++g_perf.reconnects;
        show_notice("Video froze - reconnecting");
        retry_session();
        return;
    }
    /* NVIDIA closing the signalling mid-game (websocket close 1000) took
     * the stream down 60-120 s later in beta.27 report SAFA4K, and nothing
     * reconnected. Reconnect while the rig is still ours. */
    static u64 signal_closed_at;
    if (g_signal.state == NVST_SIGNAL_CLOSED && g_app.stream_started_at) {
        if (!signal_closed_at) {
            signal_closed_at = now;
            diagnostic_log("APP", "signalling closed mid-stream (%.80s)", g_signal.status);
        }
    } else {
        signal_closed_at = 0;
    }
    const bool signal_lost = signal_closed_at && now - signal_closed_at >= 3000;
    const bool dropped = g_transport.state == WEBRTC_FAILED || g_signal.state == NVST_SIGNAL_ERROR || signal_lost;
    static u64 wifi_wait_since, wifi_back_at;
    if (!dropped) {
        g_app.waiting_wifi = false;
        wifi_wait_since = wifi_back_at = 0;
    }
    if (!dropped || !g_app.stream_started_at || g_leave_pending || net_worker_busy()) return;
    if (g_client.session_state != GFN_SESSION_READY || g_app.reconnect_attempt > 3) return;
    /* The PC ended the stream itself (stopped from Steam there), or the
     * decoder broke: reconnecting would fight the player or fail again. */
    if (g_transport.no_reconnect) {
        diagnostic_log("APP", "not reconnecting: %.100s", g_transport.status);
        log_session_end(g_transport.decoder_init_failures >= 3 ? "decoder" : "pc-ended");
        g_app.reconnect_attempt = 4;
        return;
    }
    /* 404/410 on the signalling upgrade: NVIDIA has closed the session, so
     * reconnecting cannot work. Say so; Retry starts the game again. */
    if (session_gone()) {
        /* It may only be paused: CloudMatch knows (and RESUMEs it). Once. */
        if (!g_app.recover_tried) {
            g_app.recover_tried = true;
            diagnostic_log("APP", "signalling says the session is gone (HTTP %d); asking CloudMatch", g_signal.upgrade_http);
            reconnect_at = now;
            ++g_perf.reconnects;
            show_notice("Connection lost - checking your session");
            retry_session();
            return;
        }
        diagnostic_log("APP", "session ended on NVIDIA's side (HTTP %d); not reconnecting", g_signal.upgrade_http);
        log_session_end("nvidia-gone");
        queue_auto_report("session-ended");
        perf_note_error("session-ended");
        g_app.reconnect_attempt = 4;
        return;
    }
    /* No Wi-Fi (lid shut, or out of range): retrying now only burns the
     * three attempts on "Couldn't resolve host name". Wait for the lid to
     * open and Wi-Fi to come back (up to 30 s), then let it settle. */
    if (g_app.lid_paused || !wifi_connected()) {
        if (!wifi_wait_since) {
            wifi_wait_since = now;
            diagnostic_log("APP", "connection lost with %s; waiting before reconnecting",
                           g_app.lid_paused ? "the lid closed" : "Wi-Fi off");
        }
        wifi_back_at = 0;
        if (g_app.lid_paused || now - wifi_wait_since < 30000) {
            g_app.waiting_wifi = true;
            return;
        }
    } else if (wifi_wait_since) {
        if (!wifi_back_at) {
            wifi_back_at = now;
            diagnostic_log("APP", "Wi-Fi back after %llu ms", (unsigned long long)(now - wifi_wait_since));
        }
        if (now - wifi_back_at < 1500) return;
        wifi_wait_since = wifi_back_at = 0;
        g_app.reconnect_attempt = 0;
    }
    g_app.waiting_wifi = false;
    if (reconnect_at && now - reconnect_at < 2500 && g_app.reconnect_attempt) return;
    if (g_app.reconnect_attempt == 3) {
        /* Three tries failed: hand the choice back to the player. */
        if (now - reconnect_at >= 8000) {
            g_app.reconnect_attempt = 4;
            log_session_end("reconnect-failed");
            queue_auto_report("reconnect-failed");
            perf_note_error("reconnect-failed");
        }
        return;
    }
    reconnect_at = now;
    ++g_app.reconnect_attempt;
    diagnostic_log("APP", "connection lost (%s); reconnect attempt %u",
                   g_transport.state == WEBRTC_FAILED ? g_transport.status : g_signal.status,
                   g_app.reconnect_attempt);
    show_notice("Connection lost - reconnecting");
    ++g_perf.reconnects;
    retry_session();
}

/* ---- Resolution probe (developer) ---------------------------------------- */

/* If APP_DATA_DIR/probe.txt exists, each "WxH" line launches a game (the
 * first library entry, or the first whose title contains "game=<text>"),
 * records the SPS resolution NVIDIA actually encodes, and ends the session.
 * "WxH sharp" asks for the old prefilter, "WxH@60" for 60 frames a second;
 * each run then watches 45 s of video: IDRs, the frame rate that arrived,
 * decode time and frames repeated or skipped (is 60 fps decodable?).
 * Results go to probe-results.txt, outside the capped diagnostic log. */
enum { PROBE_MAX = 16 };
enum { PROBE_OFF, PROBE_LIBRARY, PROBE_LAUNCH, PROBE_WAIT_SPS, PROBE_LEAVE, PROBE_DONE };
static struct {
    unsigned width[PROBE_MAX], height[PROBE_MAX];
    bool sharp[PROBE_MAX];
    bool fps60[PROBE_MAX];
    bool decode[PROBE_MAX];
    unsigned count, index;
    unsigned frames_base, repeated_base, skipped_base, decode_count_base, au_base, reconnect_base;
    unsigned long long decode_sum_base;
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
            g_probe.fps60[g_probe.count] = strstr(line, "@60") != NULL;
            g_probe.decode[g_probe.count] = g_probe.fps60[g_probe.count] || strstr(line, "decode") != NULL;
            ++g_probe.count;
        }
    }
    fclose(f);
    if (!g_probe.count) return;
    stream_profile_set_probing(true);
    g_probe.phase = PROBE_LIBRARY;
    g_probe.since = osGetTime();
    probe_result("probe start (build " APP_BUILD "): %u resolutions, game filter \"%s\"", g_probe.count, g_probe.game);
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
            stream_profile_set_probing(false);
            stream_profile_set_probe_decode(false);
            settings_apply_picture(&g_app.settings);
            probe_result("probe finished");
            break;
        }
        stream_profile_set_override(w, h);
        stream_profile_set_sharpen(g_probe.sharp[g_probe.index]);
        stream_profile_set_fps60(g_probe.fps60[g_probe.index]);
        stream_profile_set_probe_decode(g_probe.decode[g_probe.index]);
        g_app.modal = MODAL_NONE;
        probe_result("request %ux%u%s%s (%s)", w, h, g_probe.fps60[g_probe.index] ? "@60" : "",
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
            g_probe.frames_base = mvd_video_decoded_frames();
            g_probe.au_base = g_transport.video_access_units;
            g_probe.reconnect_base = g_perf.reconnects;
            g_probe.repeated_base = g_perf.repeated;
            g_probe.skipped_base = g_perf.skipped;
            unsigned decode_max;
            mvd_video_decode_totals(&g_probe.decode_sum_base, &g_probe.decode_count_base, &decode_max, true);
            break;
        } else if (g_probe.video_since) {
            if (now - g_probe.video_since < 45000) break;
            probe_result("  45 s: %u IDRs, %u keyframe requests, %u AUs, %u kbps",
                         g_transport.video_idr_units - g_probe.idr_base,
                         g_transport.keyframe_requests - g_probe.pli_base,
                         g_transport.video_access_units, g_transport.video_kbps);
            unsigned long long decode_sum;
            unsigned decode_count, decode_max;
            mvd_video_decode_totals(&decode_sum, &decode_count, &decode_max, false);
            const unsigned decoded = decode_count - g_probe.decode_count_base;
            probe_result("  45 s: %u fps sent, %u frames decoded, decode avg %llu us max %u us, repeated %u, "
                         "skipped %u, reconnects %u, now %ux%u",
                         (g_transport.video_access_units - g_probe.au_base) / 45, decoded,
                         decoded ? (decode_sum - g_probe.decode_sum_base) / decoded : 0ull, decode_max,
                         g_perf.repeated - g_probe.repeated_base, g_perf.skipped - g_probe.skipped_base,
                         g_perf.reconnects - g_probe.reconnect_base,
                         g_transport.video_source_width, g_transport.video_source_height);
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
    /* Packets on core 2 when it runs there, else the media socket. */
    webrtc_transport_wait(&g_transport, timeout_ms);
}

static void tick_network(void)
{
    /* Signaling starts on the worker (its TLS connect blocks); afterwards
     * the UI thread services it without blocking. */
    if (g_client.session_state == GFN_SESSION_READY && g_signal.state == NVST_SIGNAL_IDLE &&
        !net_worker_busy() && !g_leave_pending && g_app.modal != MODAL_RESUME)
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
            if (g_app.genshin_session && g_app.settings.auto_pointer) {
                webrtc_transport_set_pointer_mode(&g_transport, true);
                show_notice("Mouse & keyboard mode for the login. Tap MOUSE for controller mode");
            }
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
        snprintf(line, sizeof(line), "%s%.145s", g_transport.steam ? "" : "WebRTC: ", g_transport.status);
        return line;
    }
    if (nvst_signal_active(&g_signal) || g_signal.state == NVST_SIGNAL_ERROR) return g_signal.status;
    return g_client.status;
}

/* ---- Diagnostic reports ---------------------------------------------------- */

/* Automatic reports (Share diagnostics on), sent from the menus once nothing
 * else is happening, never during a game. Up to three a run: build 97 sent
 * one, so a second, different problem in the same run never reached us.
 * Reasons that pile up before a send go out together ("session-error+flag:x"),
 * and a send waits a minute after the last reason so a cascade is one report. */
#define AUTO_REPORTS_PER_RUN 3
static char g_auto_trigger[72];
static unsigned g_auto_sent;
static u64 g_auto_queued_at;

static void queue_auto_report(const char *trigger)
{
    if (g_auto_sent >= AUTO_REPORTS_PER_RUN || !report_available() || strstr(g_auto_trigger, trigger)) return;
    const size_t used = strlen(g_auto_trigger);
    if (used + strlen(trigger) + 2 > sizeof(g_auto_trigger)) return;
    snprintf(g_auto_trigger + used, sizeof(g_auto_trigger) - used, "%s%s", used ? "+" : "", trigger);
    g_auto_queued_at = osGetTime();
    diagnostic_log("REPORT", "automatic report queued (%s)", trigger);
}

static void auto_report_tick(void)
{
    if (!g_auto_trigger[0] || g_auto_sent >= AUTO_REPORTS_PER_RUN || g_auto_inflight ||
        g_app.settings.share_reports != SHARE_YES)
        return;
    if ((g_app.view != VIEW_LIBRARY && g_app.view != VIEW_HUB) || gfn_session_active(&g_client) ||
        net_worker_busy() || g_app.modal != MODAL_NONE || osGetTime() - g_auto_queued_at < 60000)
        return;
    if (submit_job(NET_JOB_SEND_REPORT, NULL, g_auto_trigger, NULL)) {
        ++g_auto_sent;
        g_auto_inflight = true;
        g_auto_trigger[0] = '\0';
    }
}

/* One line per session, written when it ends: who ended it and in what
 * state, so a report can be read from its [END] lines first. */
static void log_session_end(const char *by)
{
    if (g_app.end_logged) return;
    g_app.end_logged = true;
    const u64 now = osGetTime();
    const u64 last = g_transport.last_decoded_frame_at;
    diagnostic_log("END", "by=%s code=%s nvidia=%s played=%llus reconnects=%u frames=%u lastFrame=%lldms "
                   "dark=%d kbps=%u pli=%u errors=%u state=%d status=%d game=\"%.40s\" msg=\"%.100s\"",
                   by, g_client.fail_code[0] ? g_client.fail_code : "-",
                   g_client.end_error_code[0] ? g_client.end_error_code : "-",
                   (unsigned long long)(g_app.stream_started_at ? (now - g_app.stream_started_at) / 1000 : 0),
                   g_perf.reconnects, mvd_video_decoded_frames() - g_app.stream_frame_base,
                   last && now > last ? (long long)(now - last) : -1LL, mvd_video_picture_dark() ? 1 : 0,
                   g_transport.video_kbps, g_transport.keyframe_requests, mvd_video_errors(),
                   g_client.session_state, g_client.session_status, g_current_game.title,
                   g_app.end_note[0] ? g_app.end_note : g_client.status);
}

/* Watchdogs for states that should never last: each raises a flag (see
 * diagnostic_flag), and every new flag asks for an automatic report. */
static void watch_for_bugs(void)
{
    const u64 now = osGetTime();
    for (const char *code; (code = diagnostic_take_new_flag());) {
        char trigger[40];
        snprintf(trigger, sizeof(trigger), "flag:%.32s", code);
        queue_auto_report(trigger);
    }
    /* A busy message that never clears: a job hung on the worker. */
    static const char *busy_message;
    static u64 busy_since;
    static bool busy_flagged;
    if (g_app.busy != busy_message) {
        busy_message = g_app.busy;
        busy_since = now;
        busy_flagged = false;
    } else if (busy_message && !busy_flagged && now - busy_since > 90000 &&
               net_worker_current_job() != NET_JOB_UPDATE_INSTALL &&
               net_worker_current_job() != NET_JOB_CONNECTION_TEST) {
        busy_flagged = true;
        diagnostic_flag("busy-stuck", "\"%.60s\" on screen for %llus (job %d)", busy_message,
                        (unsigned long long)((now - busy_since) / 1000), (int)net_worker_current_job());
    }
    /* NVIDIA reports a place in line but the screen shows none (the queue
     * lock in gfn_client; beta.27: a queue counter stuck at 0). */
    static u64 hidden_since;
    if (gfn_session_active(&g_client) && g_client.queue_reported > 0 && g_client.queue_position <= 0 &&
        !g_app.stream_started_at) {
        if (!hidden_since) hidden_since = now;
        else if (now - hidden_since > 45000) {
            diagnostic_flag("queue-hidden", "NVIDIA says place %d, screen shows %d (step=%d queueStep=%d) for %llus",
                            g_client.queue_reported, g_client.queue_position, g_client.seat_setup_step,
                            g_client.queue_step, (unsigned long long)((now - hidden_since) / 1000));
            hidden_since = now;
        }
    } else {
        hidden_since = 0;
    }
    /* Rig setup (not the queue) taking minutes. */
    static u64 setup_since;
    if (gfn_session_active(&g_client) && g_client.session_state == GFN_SESSION_SETUP && !g_app.stream_started_at) {
        if (!setup_since) setup_since = now;
        else if (now - setup_since > 300000) {
            diagnostic_flag("setup-stuck", "rig setup for %llus (status %d, step %d): %.80s",
                            (unsigned long long)((now - setup_since) / 1000), g_client.session_status,
                            g_client.seat_setup_step, g_client.status);
            setup_since = now;
        }
    } else {
        setup_since = 0;
    }
    /* Signalling and media up, the stream never started. */
    static u64 connect_since;
    if (gfn_session_active(&g_client) && g_client.session_state == GFN_SESSION_READY && !g_app.stream_started_at &&
        g_transport.state == WEBRTC_CONNECTED && !g_transport.steam_capture_unavailable) {
        if (!connect_since) connect_since = now;
        else if (now - connect_since > 45000) {
            diagnostic_flag("no-first-frame", "connected %llus without a picture (video AU=%u decoded=%u errors=%u)",
                            (unsigned long long)((now - connect_since) / 1000), g_transport.video_access_units,
                            mvd_video_decoded_frames(), mvd_video_errors());
            connect_since = now;
        }
    } else {
        connect_since = 0;
    }
    /* Opus packets the decoder refused (not lost ones: those are Wi-Fi).
     * Without the DSP every packet "fails" (one console raised 1906 flags
     * in a run): that case is told to the player instead. */
    static unsigned audio_errors_flagged;
    if (!audio_system_ready()) {
        audio_errors_flagged = g_transport.audio_errors;
    } else if (g_transport.audio_errors >= audio_errors_flagged + 25) {
        audio_errors_flagged = g_transport.audio_errors;
        diagnostic_flag("audio-decode", "%u audio packets failed to decode (%u decoded)", g_transport.audio_errors,
                        g_transport.audio_decoded);
    } else if (g_transport.audio_errors < audio_errors_flagged) {
        audio_errors_flagged = g_transport.audio_errors;
    }
    /* Linear memory running out ends in a black screen or a crash. */
    static u64 memory_checked;
    static bool memory_flagged;
    if (!memory_flagged && now - memory_checked > 10000) {
        memory_checked = now;
        const u32 free_bytes = linearSpaceFree();
        if (free_bytes < 1536 * 1024) {
            memory_flagged = true;
            diagnostic_flag("low-memory", "linear free %lu KiB (view %d)", (unsigned long)(free_bytes / 1024),
                            (int)g_app.view);
        }
    }
}

/* No DSP firmware dump: no sound in menus or games. Beta.34 reports had a
 * console play a whole session silent with nothing on screen to say why.
 * Said once per run in the library, and again when a game starts. */
static void sound_hint_tick(void)
{
    static bool told_menu, told_game;
    if (!audio_system_firmware_missing()) return;
    if (!told_menu && g_app.view == VIEW_LIBRARY && g_app.modal == MODAL_NONE && g_app.guide_page < 0) {
        told_menu = true;
        show_notice("No sound: this 3DS's sound firmware is missing. Run DSP1 once, then restart Kasumi");
    }
    if (!told_game && g_app.view == VIEW_STREAM && g_app.stream_started_at) {
        told_game = true;
        show_notice("No sound on this 3DS: run DSP1 once (see Settings > Sound & Look)");
    }
}

/* A finished session's summary goes out from the menus when idle. */
static bool g_stats_inflight, g_stats_failed;

static void stats_tick(void)
{
    static u64 tried_at;
    if (!g_app.settings.share_stats || !report_available() || g_stats_inflight) return;
    if ((g_app.view != VIEW_LIBRARY && g_app.view != VIEW_HUB) || gfn_session_active(&g_client) ||
        net_worker_busy() || g_app.modal != MODAL_NONE)
        return;
    const u64 now = osGetTime();
    if (tried_at && now - tried_at < (g_stats_failed ? 600000u : 60000u)) return;
    if (!report_stats_pending()) return;
    tried_at = now;
    if (submit_job(NET_JOB_SEND_STATS, NULL, NULL, NULL)) g_stats_inflight = true;
}

/* Libraries saved before beta.30 have no wide art (shortcut banners), and
 * nobody presses Y to refresh: do it once, quietly, when the menus are idle. */
static bool g_library_upgrade;
static u64 g_shortcut_launch_at;

static void library_upgrade_tick(void)
{
    if (!g_library_upgrade) return;
    if (!gfn_has_session(&g_client) || g_client.game_count == 0) {
        g_library_upgrade = false;
        return;
    }
    if (g_app.view != VIEW_LIBRARY || net_worker_busy() || g_deferred.set || g_app.modal != MODAL_NONE ||
        g_app.search_text[0] || gfn_session_active(&g_client) || g_shortcut_launch_at || g_app.whats_new_open ||
        g_app.guide_page >= 0)
        return;
    g_library_upgrade = false;
    diagnostic_log("APP", "refreshing a library saved without wide art");
    load_library();
}

/* A shortcut's art is drawn (shortcut_frame); then the worker installs it. */
static void shortcut_tick(void)
{
    /* Drawing gave up (no video memory, no template): say why. It can fail
     * in the same loop it started, so ask, don't watch the state. */
    const char *failure = shortcut_take_failure();
    if (failure && g_app.shortcut_sheet == SHORTCUT_SHEET_WORKING) {
        snprintf(g_app.shortcut_message, sizeof(g_app.shortcut_message), "%s", failure);
        g_app.shortcut_sheet = SHORTCUT_SHEET_FAILED;
        sfx_play(SFX_ERROR);
    } else if (failure) {
        show_notice(failure);
    }
    const ShortcutState now = shortcut_state();
    if ((now == SHORTCUT_READY || now == SHORTCUT_FETCH) && !net_worker_busy() && !g_deferred.set)
        submit_job(NET_JOB_SHORTCUT, NULL, NULL, NULL);
}

/* Opened from a shortcut: launch its game as soon as Kasumi is ready (signed
 * in, the start-up checks done, nothing on screen asking first). */
static GfnGame g_shortcut_game;
static unsigned g_shortcut_variant;
static u64 g_shortcut_launch_at;

static void shortcut_launch_tick(void)
{
    if (!g_shortcut_launch_at) return;
    if (gfn_session_active(&g_client) || osGetTime() - g_shortcut_launch_at > 60000) {
        /* A game already running (resumed at start-up), or it never got ready. */
        diagnostic_log("SHORTCUT", "launch of %.60s dropped", g_shortcut_game.title);
        g_shortcut_launch_at = 0;
        return;
    }
    if (!gfn_has_session(&g_client)) {
        if (g_client.auth_state == GFN_AUTH_LOGGED_OUT || g_client.auth_state == GFN_AUTH_ERROR) {
            show_notice("Sign in to Kasumi first, then open the shortcut again");
            g_shortcut_launch_at = 0;
        }
        return;
    }
    if (net_worker_busy() || g_deferred.set || g_app.modal != MODAL_NONE || g_app.guide_page >= 0 ||
        g_app.update_open || (g_app.view != VIEW_LIBRARY && g_app.view != VIEW_DETAILS))
        return;
    g_shortcut_launch_at = 0;
    diagnostic_log("SHORTCUT", "launching %.60s", g_shortcut_game.title);
    launch_with_options(&g_shortcut_game, g_shortcut_variant);
}

/* The NVIDIA login is renewed ten minutes before it runs out, during a game
 * too. A long game used to outlast it and the renewal at its end could be
 * refused: "it logged me out" after quitting from the game's own menu. */
static void keep_login_tick(void)
{
    static u64 tried_at;
    if (!gfn_has_session(&g_client) || net_worker_busy() || g_leave_pending || g_deferred.set) return;
    const int64_t now_s = (int64_t)time(NULL);
    const bool login_due = g_client.token_expires_at && g_client.token_expires_at - now_s <= 540;
    const bool client_due = g_client.client_token[0] && g_client.client_token_expires_at - now_s <= 540;
    if (!login_due && !client_due) return;
    const u64 now = osGetTime();
    if (tried_at && now - tried_at < 120000) return;
    tried_at = now;
    diagnostic_log("AUTH", "renewing in the background (login %llds, client token %llds left)",
                   (long long)(g_client.token_expires_at - now_s),
                   (long long)(g_client.client_token_expires_at - now_s));
    submit_job(NET_JOB_KEEP_LOGIN, NULL, NULL, NULL);
}

/* Before the first frame, a failed connection (signalling refused, the
 * encrypted media link not answering) is retried by itself, twice. Beta.21
 * report 9T9HZJ: a fresh rig ignored DTLS for ~7 s, the reconnect then hit
 * "peer removed", and the player's own retry streamed at once. */
#define SETUP_RETRIES 2

static void setup_retry_tick(void)
{
    if (!gfn_session_active(&g_client) || g_app.stream_started_at || g_leave_pending || net_worker_busy() ||
        net_worker_signal_starting() || g_client.session_state != GFN_SESSION_READY)
        return;
    const bool signal_failed = g_signal.state == NVST_SIGNAL_ERROR;
    const bool media_failed = g_transport.state == WEBRTC_FAILED;
    if ((!signal_failed && !media_failed) || g_app.setup_retries >= SETUP_RETRIES) return;
    if (media_failed && g_transport.no_reconnect) {
        /* Stays on the error screen with the transport's reason. */
        g_app.setup_retries = SETUP_RETRIES;
        return;
    }
    ++g_app.setup_retries;
    diagnostic_log("APP", "connection failed before the stream (%.80s); automatic retry %u",
                   signal_failed ? g_signal.status : g_transport.status, g_app.setup_retries);
    show_notice("Connection failed - trying again");
    retry_session();
}

/* A session that fails at any stage (queue, rig setup, stream). */
static void watch_session_errors(void)
{
    static bool was_error;
    const bool error = gfn_session_active(&g_client) &&
                       (g_client.session_state == GFN_SESSION_ERROR ||
                        (g_signal.state == NVST_SIGNAL_ERROR && !g_app.stream_started_at &&
                         g_app.setup_retries >= SETUP_RETRIES));
    /* A free session ending around its hour: NVIDIA says "internal error",
     * but it is the free tier's limit (beta.23 report K9J4FF, 63 min). */
    if (error && !was_error && g_app.free_tier_guess && g_app.stream_started_at &&
        osGetTime() - g_app.stream_started_at >= 58ull * 60 * 1000) {
        diagnostic_log("APP", "free session ended after its hour (%.80s)", g_client.status);
        snprintf(g_app.end_note, sizeof(g_app.end_note),
                 "Your free hour is over. Press A to start the game again (you may queue again).");
        log_session_end("free-hour");
        perf_note_error("free-hour");
        was_error = error;
        return;
    }
    /* NVIDIA ending a stream that ran a while is usually the player quitting
     * from the game's own menu (beta.26 reports 9WFFRT, QYHU2H): a normal
     * end, not a failure to report. Short streams still report. */
    if (error && !was_error && g_app.stream_started_at && g_client.session_state == GFN_SESSION_ERROR &&
        !strcmp(g_client.fail_code, "ended") && osGetTime() - g_app.stream_started_at >= 2ull * 60 * 1000) {
        diagnostic_log("APP", "session ended after %llus of play",
                       (unsigned long long)((osGetTime() - g_app.stream_started_at) / 1000));
        log_session_end("nvidia");
        was_error = error;
        return;
    }
    if (error && !was_error) {
        diagnostic_log("APP", "session failed: %.120s", g_client.session_state == GFN_SESSION_ERROR ? g_client.status : g_signal.status);
        /* NVIDIA ending a short stream cleanly (errorCode 1) is mostly the
         * game itself exiting (beta.27 report GTXMTY: the picture went black
         * first). The END line keeps it; anything odd around it raises its
         * own flag. */
        const bool clean_end = !strcmp(g_client.fail_code, "ended") && !strcmp(g_client.end_error_code, "1");
        log_session_end(clean_end ? "nvidia" : g_client.session_state == GFN_SESSION_ERROR ? "error" : "signalling");
        if (!clean_end) queue_auto_report(g_app.stream_started_at ? "session-error" : "queue-or-setup-failed");
        perf_note_error("session-error");
        if (!g_app.stream_started_at)
            launch_end(g_client.session_state == GFN_SESSION_ERROR && g_client.fail_code[0] ? g_client.fail_code :
                       "setup", launch_share_id());
    }
    was_error = error;
}

/* Asked once per console, when the menus are quiet. */
static void share_prompt_tick(void)
{
    static bool asked;
    if (asked || g_app.settings.share_consent >= SHARE_CONSENT_VERSION || !report_available()) return;
    if ((g_app.view != VIEW_LIBRARY && g_app.view != VIEW_HUB) || g_app.whats_new_open ||
        g_app.guide_page >= 0 || g_app.update_open || g_app.modal != MODAL_NONE || g_app.busy)
        return;
    asked = true;
    open_modal(MODAL_SHARE_ASK, "協力", "HELP IMPROVE KASUMI?", "");
}

/* How long NVIDIA may take to free a slot by itself (beta.18: 12 s to
 * 8 min), and the pause before each try: 1:30, growing to 3:00. In beta.22
 * reports a try every 45 s drew NVIDIA's rate limit (429) after a few, and
 * the consoles that got in did so after a few quiet minutes. */
#define LIMIT_WAIT_MS (15u * 60u * 1000u)
static unsigned g_limit_tries;

static u64 limit_retry_delay(void)
{
    /* Limited mode: 45 s, then 15 s longer each time, at most 2 min. */
    if (g_app.limit_busy) {
        const u64 busy = 45000u + 15000u * (u64)g_limit_tries;
        return busy < 120000u ? busy : 120000u;
    }
    const u64 delay = 90000u + 30000u * (u64)g_limit_tries;
    if (g_app.limit_rate && delay < 120000u) return 120000u;
    return delay < 180000u ? delay : 180000u;
}

static void limit_wait_text(void)
{
    const u64 now = osGetTime();
    const unsigned left = g_app.limit_retry_at > now ? (unsigned)((g_app.limit_retry_at - now + 999) / 1000) : 0;
    if (g_app.limit_busy) {
        snprintf(g_app.modal_text, sizeof(g_app.modal_text),
                 "NVIDIA is limiting new sessions for a few minutes (limited mode). Kasumi tries again by "
                 "itself: next try in %u:%02u.", left / 60, left % 60);
        return;
    }
    if (g_app.limit_rate) {
        snprintf(g_app.modal_text, sizeof(g_app.modal_text),
                 "NVIDIA asked Kasumi to slow down after several launches. Trying again in %u:%02u by itself.",
                 left / 60, left % 60);
        return;
    }
    if (g_app.limit_unclosable) {
        snprintf(g_app.modal_text, sizeof(g_app.modal_text),
                 "A session from an older Kasumi (or another GeForce NOW app) is still open. Only NVIDIA can "
                 "close it now. Retrying in %u:%02u; B stops, try later.", left / 60, left % 60);
        return;
    }
    /* Beta.23: every one of 27 waits in a day's reports was cancelled; the
     * ones left alone got in. */
    snprintf(g_app.modal_text, sizeof(g_app.modal_text),
             "NVIDIA limits how often one console can start games (free accounts most). It usually "
             "clears in 2-5 min and Kasumi keeps trying: next try in %u:%02u.", left / 60, left % 60);
}

/* A launch failed: ask about a session in the way, wait for NVIDIA to free
 * the slot, or show the error. */
static void launch_failed(void)
{
    const u64 now = osGetTime();
    if (g_client.conflict_found) {
        char other[112] = "another game";
        for (size_t i = 0; i < g_client.game_count; ++i)
            if (!strcmp(g_client.games[i].app_id, g_client.conflict.app_id))
                snprintf(other, sizeof(other), "%.40s", g_client.games[i].title);
        g_app.conflict_same_game = g_client.conflict.app_id[0] &&
                                   !strcmp(g_client.conflict.app_id, g_current_game.app_id);
        char text[sizeof(g_app.modal_text)];
        if (g_app.conflict_same_game)
            snprintf(text, sizeof(text), "%.90s is already running on your account. Resume it here?",
                     g_current_game.title);
        else
            snprintf(text, sizeof(text), "%s is running on your account, maybe on another device. "
                     "End it and start %.40s?", other, g_current_game.title);
        open_modal(MODAL_CONFLICT, "使用中", g_app.conflict_same_game ? "RESUME YOUR GAME?" : "SESSION IN USE", text);
        return;
    }
    if (g_client.limit_wait) {
        if (g_app.limit_busy) {
            /* From limited mode to a slot wait: a fresh 15 minutes. */
            g_app.limit_busy = false;
            g_app.limit_wait_until = 0;
        }
        if (g_client.limit_unclosable) g_app.limit_unclosable = true;
        g_app.limit_rate = g_client.limit_rate;
        if (!g_app.limit_wait_until) {
            g_app.limit_wait_until = now + LIMIT_WAIT_MS;
            g_limit_tries = 0;
            diagnostic_log("APP", "%s", g_app.limit_rate ? "NVIDIA is rate limiting launches: waiting"
                                                         : "waiting for NVIDIA to free the session slot");
        }
        if (now < g_app.limit_wait_until) {
            g_app.limit_retry_at = now + limit_retry_delay();
            ++g_limit_tries;
            open_modal(MODAL_LIMIT_WAIT, "待機中", "ALMOST THERE", "");
            limit_wait_text();
            return;
        }
        g_app.limit_wait_until = g_app.limit_retry_at = 0;
        open_modal(MODAL_ERROR, "起動失敗", "LAUNCH FAILED", g_app.limit_rate
                   ? "NVIDIA kept refusing launches for 15 minutes. Wait a little, then try again."
                   : "NVIDIA didn't free your session slot in 15 minutes. Close GeForce NOW on your other "
                     "devices (or wait a little), then try again.");
        queue_auto_report("launch-failed");
        launch_end(g_app.limit_rate ? "429" : "limit", launch_share_id());
        return;
    }
    /* NVIDIA's limited mode passes within minutes: 54 launches in beta.31's
     * export ended on it, each a Retry press away. Wait and retry here. */
    if (!strcmp(g_client.fail_code, "limited")) {
        if (!g_app.limit_wait_until || !g_app.limit_busy) {
            g_app.limit_wait_until = now + LIMIT_WAIT_MS;
            g_limit_tries = 0;
            g_app.limit_busy = true;
            diagnostic_log("APP", "NVIDIA is in limited mode: waiting to retry");
        }
        if (now < g_app.limit_wait_until) {
            g_app.limit_retry_at = now + limit_retry_delay();
            ++g_limit_tries;
            open_modal(MODAL_LIMIT_WAIT, "混雑", "NVIDIA IS BUSY", "");
            limit_wait_text();
            return;
        }
        g_app.limit_wait_until = g_app.limit_retry_at = 0;
        g_app.limit_busy = false;
        open_modal(MODAL_ERROR, "起動失敗", "LAUNCH FAILED",
                   "NVIDIA stayed in limited mode for 15 minutes. Try again a little later.");
        launch_end("limited", launch_share_id());
        return;
    }
    /* Beta.23: one console asked for Japan from Australia 18 times. */
    if (!strcmp(g_client.fail_code, "region") && g_app.settings.server[0] &&
        strcmp(g_app.settings.server, REGION_CHOICE_NVIDIA)) {
        static char text[96];
        diagnostic_log("APP", "server %s refused for this account: switching to Auto", g_app.settings.server);
        snprintf(text, sizeof(text), "%.30s isn't open to your account - Server is now Auto", g_app.settings.server);
        g_app.settings.server[0] = '\0';
        regions_set_choice("");
        save_settings();
        show_notice(text);
        submit_job(NET_JOB_START_SESSION, "Trying again on Auto...", NULL, &g_current_game);
        return;
    }
    const char *failed_text = g_client.status;
    char entitlement[sizeof(g_app.modal_text)];
    unsigned library_games = 0;
    /* Entitlement was the most common refusal in beta.31's export (62):
     * accounts never set up at play.geforcenow.com, and games picked from
     * search that the account doesn't have. Say which. */
    if (!strcmp(g_client.fail_code, "entitlement") && provider_is_nvidia() && gfn_library_known(&library_games)) {
        if (!library_games) {
            snprintf(entitlement, sizeof(entitlement),
                     "Your GeForce NOW library is empty, so NVIDIA won't start games yet. On a phone or PC, "
                     "sign in at play.geforcenow.com, accept the terms and link Steam or Epic, then try again.");
            failed_text = entitlement;
        } else if (!gfn_in_library(&g_current_game)) {
            snprintf(entitlement, sizeof(entitlement),
                     "%.40s isn't in your GeForce NOW library. Add it at play.geforcenow.com (and own it on "
                     "its store), refresh your library here, then try again.", g_current_game.title);
            failed_text = entitlement;
        }
    }
    open_modal(MODAL_ERROR, "起動失敗", "LAUNCH FAILED", failed_text);
    log_session_end("launch-failed");
    /* NVIDIA's limited mode and an account without the game are not bugs
     * (beta.27 report QQEN7S: five entitlement refusals, five reports). An
     * answer we have no words for raises flag:unmapped-reason instead. */
    if (strcmp(g_client.fail_code, "limited") && strcmp(g_client.fail_code, "entitlement"))
        queue_auto_report("launch-failed");
    launch_end(g_client.fail_code[0] ? g_client.fail_code : "error", launch_share_id());
}

/* 60 fps without falling back. The decoder takes about 14 ms of the 16.7 a
 * frame has (beta.34 export), so it keeps up on average; heavy scenes and
 * Wi-Fi bunching still queue encoded frames, and every queued frame is
 * input lag. A frame can't be skipped (each needs the one before), so a
 * backlog that lasts is dropped to the next keyframe: a short hitch instead
 * of lag that grows. Beta.33-34 reconnected at 30 fps instead (about 20 s,
 * in 52 of 58 sessions). */
static void fps60_latency_guard(u64 now)
{
    static u64 backed_up_since, last_catchup, window_at;
    static unsigned window_catchups;
    static u64 hinted_for;
    if (stream_profile_fps() < 60 || g_app.view != VIEW_STREAM || !g_app.stream_started_at ||
        g_transport.state != WEBRTC_CONNECTED || now < g_app.stream_started_at + 3000) {
        backed_up_since = 0;
        return;
    }
    /* Six in the queue: five waiting behind the one being decoded, about
     * 85 ms. A Wi-Fi burst or a busy moment clears in well under a second.
     * Each catch-up is itself a hitch (the drop plus ~60 ms for the
     * keyframe), and one every 2 s in a scene heavier than the decoder
     * read as constant stutter (report GWU2AV): at most one per 6 s. */
    if (mvd_video_pending_units() >= 6) {
        if (!backed_up_since) backed_up_since = now;
    } else {
        backed_up_since = 0;
    }
    if (!backed_up_since || now - backed_up_since < 600 || now - last_catchup < 6000) return;
    const unsigned dropped = mvd_video_drop_backlog();
    backed_up_since = 0;
    if (!dropped) return;
    last_catchup = now;
    ++g_perf.catchups;
    diagnostic_log("VIDEO", "60 fps catch-up: dropped %u queued frames, waiting for a keyframe", dropped);
    /* Often (four in a minute): the game is heavier than the 3DS decodes
     * at 60. Said once per game; 30 fps stays the player's choice. */
    if (!window_at || now - window_at > 60000) {
        window_at = now;
        window_catchups = 0;
    }
    if (++window_catchups >= 4 && hinted_for != g_app.stream_started_at) {
        hinted_for = g_app.stream_started_at;
        show_notice("Heavy game for 60 fps on the 3DS: 30 fps will feel smoother (Settings > Picture)");
    }
}

/* The limit-wait countdown, and its retry. */
static void limit_wait_tick(void)
{
    if (!g_app.limit_retry_at) return;
    if (g_app.modal == MODAL_LIMIT_WAIT) limit_wait_text();
    if (osGetTime() < g_app.limit_retry_at || net_worker_busy() || gfn_session_active(&g_client)) return;
    if (g_app.modal != MODAL_LIMIT_WAIT && g_app.modal != MODAL_NONE) return;
    g_app.limit_retry_at = 0;
    g_app.modal = MODAL_NONE;
    diagnostic_log("APP", "trying the launch again");
    g_limit_last_try = osGetTime();
    submit_job(NET_JOB_START_SESSION, "Trying again...", "retry", &g_current_game);
}

/* React to a worker job that just finished. */
static void finish_jobs(void)
{
    static unsigned seen_serial;
    const NetJobResult result = net_worker_last_result();
    if (result.serial == seen_serial) return;
    seen_serial = result.serial;
    if (result.cancelled && !background_job(result.kind)) {
        show_notice("Cancelled");
    } else if ((result.kind == NET_JOB_START_SESSION || result.kind == NET_JOB_RESTART_SESSION ||
                result.kind == NET_JOB_END_CONFLICT) && !result.ok) {
        launch_failed();
    } else if (result.kind == NET_JOB_CLAIM_CONFLICT && !result.ok && g_client.limit_wait) {
        launch_failed();
    } else if (result.kind == NET_JOB_CLAIM_CONFLICT && !result.ok) {
        open_modal(MODAL_ERROR, "起動失敗", "COULDN'T RESUME", g_client.status);
        launch_end(g_client.fail_code[0] ? g_client.fail_code : "claim", launch_share_id());
    } else if (result.kind == NET_JOB_RECOVER && !result.ok && g_client.session_state == GFN_SESSION_ERROR) {
        /* watch_session_errors reports it (the session is in error now). */
        diagnostic_log("APP", "session ended on NVIDIA's side (%s); not reconnecting", g_client.fail_code);
        g_app.reconnect_attempt = 4;
    }
    if (result.kind == NET_JOB_LOAD_LIBRARY || result.kind == NET_JOB_SEARCH || result.kind == NET_JOB_USE_PC ||
        result.kind == NET_JOB_SIGN_OUT)
        g_app.selected = g_app.list_top = 0;
    /* Steam Link: pairing another PC found none, or the PC switched. */
    if (result.kind == NET_JOB_BEGIN_LOGIN && !result.ok && gfn_has_session(&g_client))
        show_notice(g_client.status);
    if ((result.kind == NET_JOB_USE_PC || result.kind == NET_JOB_SIGN_OUT) && gfn_has_session(&g_client) &&
        current_service() == SERVICE_STEAM) {
        char text[96];
        snprintf(text, sizeof(text), "Streaming from %.60s", steam_link_host_name());
        show_notice(text);
    }
    /* A service signed in before but with no saved library yet. */
    if (result.kind == NET_JOB_SWITCH_SERVICE && gfn_has_session(&g_client) && !g_client.game_count)
        load_library();
    if (result.kind == NET_JOB_RESUME_CHECK && result.ok && g_client.resume_found) {
        char text[192];
        snprintf(text, sizeof(text), "%.90s is still running on your rig. Jump back in, or end it?",
                 g_client.resume_game.title[0] ? g_client.resume_game.title : "Your game");
        g_current_game = g_client.resume_game;
        open_modal(MODAL_RESUME, "再開", "RESUME YOUR GAME?", text);
    }
    /* The first job only fetched the art; drawing comes next. */
    if (result.kind == NET_JOB_SHORTCUT && shortcut_state() != SHORTCUT_DRAWING &&
        shortcut_state() != SHORTCUT_FETCH) {
        if (result.ok) {
            g_app.shortcut_sheet = shortcut_removing() ? SHORTCUT_SHEET_REMOVED : SHORTCUT_SHEET_ADDED;
            sfx_play(SFX_QUEUE_READY);
        } else {
            snprintf(g_app.shortcut_message, sizeof(g_app.shortcut_message), "%s",
                     shortcut_result()[0] ? shortcut_result() : "The installer didn't finish.");
            g_app.shortcut_sheet = SHORTCUT_SHEET_FAILED;
            sfx_play(SFX_ERROR);
        }
        shortcut_job_done();
    }
    if (result.kind == NET_JOB_SEND_STATS) {
        g_stats_inflight = false;
        g_stats_failed = !result.ok;
    }
    if (result.kind == NET_JOB_SEND_REPORT && g_auto_inflight) {
        g_auto_inflight = false;
        if (result.ok) {
            char text[64];
            snprintf(text, sizeof(text), "Diagnostic report sent · %s", report_code());
            show_notice(text);
        }
    } else if (result.kind == NET_JOB_SEND_REPORT && !result.cancelled) {
        if (result.ok) {
            snprintf(g_app.report_code, sizeof(g_app.report_code), "%s", report_code());
            open_modal(MODAL_REPORT_SENT, "送信完了", "REPORT SENT",
                       "Share this code on GitHub or in the Kasumi Discord so the developer can find your report.");
        } else {
            /* A modal, not a toast: reports are sent from Settings, which
             * shows no toasts, so a failure looked like nothing happened. */
            char text[192];
            snprintf(text, sizeof(text), "%.110s. Check the Wi-Fi, then send it again.", report_error());
            open_modal(MODAL_SEND_REPORT, "送信失敗", "REPORT NOT SENT", text);
        }
    }
    if (result.kind == NET_JOB_UPDATE_CHECK) {
        const UpdateInfo info = updater_info();
        if (info.state == UPDATE_AVAILABLE && !updater_dismissed() && !g_app.update_open) {
            /* Show the update itself (notes, Install / Later) when nothing
             * else is on screen; a toast was easy to miss (beta.18). */
            const bool quiet = (g_app.view == VIEW_LIBRARY || g_app.view == VIEW_HUB) &&
                               g_app.modal == MODAL_NONE && !g_app.whats_new_open && g_app.guide_page < 0 &&
                               !gfn_session_active(&g_client) && !g_app.settings_open;
            if (quiet) {
                diagnostic_log("UPDATE", "showing %s", info.latest);
                open_updates();
            } else {
                char text[96];
                snprintf(text, sizeof(text), "Kasumi %s is available - Settings > Updates", info.latest);
                show_notice(text);
            }
        }
    }
    if (g_leave_pending && !net_worker_busy()) leave_session();
    run_deferred_job();
}

/* ---- Menu sound effects ----------------------------------------------------- */

/* What the player can see change. Compared from one loop to the next, so a
 * button, a tap and a reply from the worker all get the same sound without
 * every handler knowing about it. */
typedef struct {
    AppView view;
    AppModal modal;
    bool settings_open, details_open, options_open, mapping_open, update_open, whats_new_open, discord_open;
    bool stream_menu, controls_open, guide_open;
    int library_tab, setting_index, options_index, stream_menu_index, guide_page, mapping_input, mapping_field;
    int settings_section, settings_grid;
    size_t selected;
    unsigned details_variant;
    bool busy;
    AppSettings settings;
} UiState;

static void ui_state(UiState *s)
{
    memset(s, 0, sizeof(*s));
    s->view = g_app.view;
    s->modal = g_app.modal;
    s->settings_open = g_app.settings_open;
    s->details_open = g_app.details_open;
    s->options_open = g_app.options_open;
    s->mapping_open = g_app.mapping_open;
    s->update_open = g_app.update_open;
    s->whats_new_open = g_app.whats_new_open;
    s->discord_open = g_app.discord_open;
    s->stream_menu = g_app.stream_menu;
    s->controls_open = g_app.controls_open;
    s->guide_open = g_app.guide_page >= 0;
    s->library_tab = g_app.library_tab;
    s->setting_index = g_app.setting_index;
    s->settings_section = g_app.settings_section;
    s->settings_grid = g_app.settings_grid;
    s->options_index = g_app.options_index;
    s->stream_menu_index = g_app.stream_menu_index;
    s->guide_page = g_app.guide_page;
    s->mapping_input = g_app.mapping_input;
    s->mapping_field = g_app.mapping_field;
    s->selected = g_app.selected;
    s->details_variant = g_app.details_variant;
    s->busy = g_app.busy != NULL;
    s->settings = g_app.settings;
}

/* Overlays that open over the current view, as a bit set. */
static unsigned ui_overlays(const UiState *s)
{
    return (s->settings_open ? 1u : 0) | (s->details_open ? 2u : 0) | (s->options_open ? 4u : 0) |
           (s->mapping_open ? 8u : 0) | (s->update_open ? 16u : 0) | (s->whats_new_open ? 32u : 0) | (s->discord_open ? 512u : 0) |
           (s->stream_menu ? 64u : 0) | (s->controls_open ? 128u : 0) | (s->guide_open ? 256u : 0);
}

static void ui_sounds(u32 down, AppAction action)
{
    static UiState last;
    static bool have_last;
    UiState now;
    ui_state(&now);
    sfx_set_enabled(g_app.settings.sound_effects);
    if (!have_last) {
        last = now;
        have_last = true;
        return;
    }
    const UiState *was = &last;
    const bool back = (down & KEY_B) || action == ACTION_BACK || action == ACTION_CANCEL;
    /* In a game, only the stream menu and its sheets make sounds: the
     * buttons belong to the game. */
    const bool in_game = now.view == VIEW_STREAM && !now.stream_menu && !was->stream_menu &&
                         !now.controls_open && !was->controls_open && now.modal == MODAL_NONE &&
                         was->modal == MODAL_NONE;
    const unsigned opened = ui_overlays(&now) & ~ui_overlays(was);
    const unsigned closed = ui_overlays(was) & ~ui_overlays(&now);
    int sound = -1;
    if (in_game) {
        sound = -1;
    } else if (now.modal != was->modal) {
        if (now.modal == MODAL_ERROR) sound = SFX_ERROR;
        else if (now.modal != MODAL_NONE) sound = SFX_OPEN;
        else sound = back ? SFX_BACK : SFX_SELECT;
    } else if (opened) {
        sound = SFX_OPEN;
    } else if (closed) {
        sound = back ? SFX_BACK : SFX_CLOSE;
    } else if (now.settings_section != was->settings_section) {
        sound = now.settings_section >= 0 ? SFX_OPEN : SFX_BACK;
    } else if (now.library_tab != was->library_tab) {
        sound = SFX_TAB;
    } else if (memcmp(&now.settings, &was->settings, sizeof(now.settings))) {
        const bool lower = (down & KEY_LEFT) || action == ACTION_VALUE_PREV || action == ACTION_OPTION_PREV ||
                           action == ACTION_MAP_PREV || action == ACTION_VARIANT_PREV;
        sound = lower ? SFX_TOGGLE_OFF : SFX_TOGGLE_ON;
    } else if (now.selected != was->selected || now.setting_index != was->setting_index ||
               now.settings_grid != was->settings_grid ||
               now.options_index != was->options_index || now.stream_menu_index != was->stream_menu_index ||
               now.guide_page != was->guide_page || now.mapping_input != was->mapping_input ||
               now.mapping_field != was->mapping_field ||
               now.details_variant != was->details_variant) {
        sound = SFX_MOVE;
    } else if (now.view != was->view && now.view != VIEW_STREAM && was->view != VIEW_STREAM) {
        sound = back ? SFX_BACK : now.view == VIEW_LIBRARY && was->view == VIEW_SESSION ? -1 : SFX_SELECT;
    } else if (now.busy && !was->busy && (down & KEY_A)) {
        sound = SFX_SELECT;
    } else if ((down & (KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT)) && now.modal == MODAL_NONE && !now.busy &&
               (now.view == VIEW_LIBRARY || now.view == VIEW_SETTINGS || now.view == VIEW_DETAILS ||
                now.stream_menu)) {
        /* Pressed against the end of a list. */
        sound = SFX_BUMP;
    }
    if (sound >= 0) sfx_play((Sfx)sound);
    last = now;
}

static void log_session(void)
{
    static u64 last;
    /* Every second while the rig is being set up and the stream connects;
     * every 5 s otherwise (playing, queued, or an ended session waiting on
     * the player), so long sessions do not flood the log. */
    const bool connecting = g_client.session_state == GFN_SESSION_SETUP ||
                            (g_client.session_state == GFN_SESSION_READY && g_app.view != VIEW_STREAM);
    const u64 interval = connecting ? 1000 : 5000;
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
    g_app.view = VIEW_HUB;
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
    providers_load();
    regions_load();
    settings_load(&g_app.settings);
    /* Files the menus consult, read once before the background writer
     * starts: on a slow card a read waits behind any write. */
    report_stats_pending();
    net_memory_load();
    file_worker_init();
    if (!g_app.settings.install_id[0]) {
        /* Anonymous: random, made here, not linked to any account. */
        srand((unsigned)(svcGetSystemTick() ^ osGetTime()));
        snprintf(g_app.settings.install_id, sizeof(g_app.settings.install_id), "%04x%04x%04x",
                 rand() & 0xFFFF, rand() & 0xFFFF, rand() & 0xFFFF);
        settings_save(&g_app.settings);
    }
    g_app.guide_page = g_app.settings.guide_done ? -1 : 0;
    /* Kasumi opens on the hub: the services as cards (the guide first for a
     * new player). */
    open_hub();
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
    /* The last run of this version never reached a normal exit. */
    if (report_previous_run_unclean()) {
        diagnostic_log("APP", "previous run did not exit cleanly");
        queue_auto_report("unclean-exit");
    }
    nvst_signal_init(&g_signal);
    webrtc_transport_init(&g_transport);
    steam_link_select(g_app.settings.steam_service);
    xcloud_select(g_app.settings.xbox_service && !g_app.settings.steam_service);
    gfn_client_init(&g_client);
    /* The saved library appears instantly; covers keep filling in behind. */
    if (gfn_has_session(&g_client) && gfn_library_load(&g_client)) {
        game_art_prefetch(g_client.games, (unsigned)g_client.game_count);
        g_library_upgrade = true;
        for (size_t i = 0; i < g_client.game_count && g_library_upgrade; ++i)
            if (g_client.games[i].wide_url[0]) g_library_upgrade = false;
    }
    if (!gfn_input_self_test()) {
        diagnostic_log("INPUT", "wire encoder self-test FAILED");
        show_notice("Input packet self-test failed");
    }
    /* Audio driver now, while nothing else is running (see audio_output.c). */
    audio_system_init();
    menu_audio_set((MenuMusicMode)g_app.settings.music_mode, MENU_SCENE_MENUS);
    menu_audio_start();
    sfx_init();
    sfx_set_enabled(g_app.settings.sound_effects);
    if (g_app.settings.voice_cues) menu_audio_cue(MENU_CUE_OKAERI);
    play_history_load();
    game_prefs_load();
    queue_stats_load();
    g_app.continue_index = -1;
    g_app.queue_eta = -1;
    updater_init(argc > 0 && argv ? argv[0] : NULL);
    /* First start of a freshly installed version: show what changed, once. */
    if (updater_take_whats_new(g_app.whats_new_version, sizeof(g_app.whats_new_version),
                               g_whats_new_notes, sizeof(g_whats_new_notes))) {
        g_app.whats_new_open = true;
        g_app.whats_new_notes = g_whats_new_notes;
    }
    /* Opened from a game's HOME Menu shortcut: launch it once signed in. */
    if (shortcut_take_launch(&g_shortcut_game, &g_shortcut_variant)) {
        g_shortcut_launch_at = osGetTime();
        g_app.whats_new_open = false;
        g_app.hub_open = false;
        char text[128];
        snprintf(text, sizeof(text), "Starting %.90s...", g_shortcut_game.title);
        show_notice(text);
    }
    aptHook(&g_apt_cookie, apt_hook, NULL);
    queue_alert_stop();
    if (!net_worker_start(&g_client, &g_signal)) {
        fatal_screen("STARTUP FAILED", "Could not start the network worker thread.");
        shutdown_services();
        ui_exit();
        gfxExit();
        return 1;
    }
    probe_load();
    /* A crash or power loss may have left a game running on a rig. */
    if (gfn_has_session(&g_client) && gfn_active_exists())
        submit_job(NET_JOB_RESUME_CHECK, "Checking for a game that's still running...", NULL, NULL);
    bool sleep_allowed = true;

    bool was_touching = false;
    u64 last_bottom_draw = 0;
    u64 loop_started = osGetTime();
    while (aptMainLoop() && !g_quit) {
        {
            const u64 loop_now = osGetTime();
            const unsigned loop_ms = loop_now > loop_started ? (unsigned)(loop_now - loop_started) : 0;
            loop_started = loop_now;
            /* The UI froze with no HOME, sleep or applet in between. */
            static unsigned apt_seen;
            /* An hour or more is the clock being changed, not a stall (a beta.31
             * report showed 38 days). */
            const bool interrupted = apt_seen != g_apt_events || rosalina_combo(g_last_held);
            if (loop_ms > 2000) g_loop_gap_grace_until = loop_now + 12000;
            if (loop_ms > 2500 && loop_ms < 3600000 && apt_seen == g_apt_events && rosalina_combo(g_last_held)) {
                diagnostic_log("APP", "paused %u ms by the system menu (L+Down+Select)", loop_ms);
            } else if (loop_ms > 2500 && loop_ms < 3600000 && !interrupted) {
                unsigned measured = 0;
                for (int i = 0; i < PHASE_COUNT; ++i) measured += g_phase_ms[i];
                diagnostic_flag("ui-stall", "main loop blocked %u ms (view %d, job %d) sync=%u ticks=%u input=%u "
                                "net=%u track=%u render=%u other=%u", loop_ms, (int)g_app.view,
                                (int)net_worker_current_job(),
                                g_phase_ms[PHASE_SYNC], g_phase_ms[PHASE_TICKS], g_phase_ms[PHASE_HANDLE],
                                g_phase_ms[PHASE_NETWORK], g_phase_ms[PHASE_TRACK], g_phase_ms[PHASE_RENDER],
                                loop_ms > measured ? loop_ms - measured : 0);
            }
            apt_seen = g_apt_events;
            g_phase_at = loop_now;
            if (g_app.view == VIEW_STREAM && loop_ms < 5000 && !interrupted) {
                if (loop_ms > g_loop_max_ms) g_loop_max_ms = loop_ms;
                if (loop_ms > 25) {
                    ++g_loop_slow;
                    ++g_perf.slow_loops;
                }
                if (loop_ms > g_perf.loop_max_ms) g_perf.loop_max_ms = loop_ms;
            }
        }
        net_worker_sync(&g_client);
        finish_jobs();
        probe_tick();
        phase_end(PHASE_SYNC);
        g_app.busy = net_worker_busy() ? g_busy_message : NULL;
        hidScanInput();
        const u32 down = hidKeysDown();
        const u32 held = hidKeysHeld();
        g_last_held = held;
        const u32 repeat = hidKeysDownRepeat();
        touchPosition touch = {0, 0};
        if (held & KEY_TOUCH) hidTouchRead(&touch);
        const bool touch_down = (down & KEY_TOUCH) != 0;
        g_app.touching = (held & KEY_TOUCH) != 0;
        g_app.touch_x = touch.px;
        g_app.touch_y = touch.py;

        const AppView previous_view = g_app.view;
        enter_tick();
        g_app.service_loading = net_worker_current_job() == NET_JOB_SWITCH_SERVICE;
        /* Covers stop downloading while a game starts or runs. */
        game_art_pause(gfn_session_active(&g_client) || g_transport.peer != NULL);
        g_app.view = derive_view();
        if (g_app.view == VIEW_HUB) refresh_current_status();
        /* Steam Link pairing ended with a PC still paired ("Paired with
         * zen", or another PC that didn't pair): say how it went. */
        if (previous_view == VIEW_LOGIN && g_app.view != VIEW_LOGIN && gfn_has_session(&g_client) &&
            current_service() == SERVICE_STEAM)
            show_notice(g_client.status);
        if (previous_view == VIEW_STREAM && g_app.view != VIEW_STREAM) release_stream_input();
        g_app.keyboard_open = g_transport.keyboard_mode;

        if (g_app.view == VIEW_LIBRARY || g_app.view == VIEW_DETAILS || g_app.view == VIEW_HUB) rebuild_list();
        const AppAction action = touch_down ? screens_touch(&g_app, touch.px, touch.py) : ACTION_NONE;
        if (g_app.view != VIEW_STREAM) {
            auto_update_check();
            discord_invite_tick();
            share_prompt_tick();
            auto_report_tick();
            stats_tick();
        }
        keep_login_tick();
        sound_hint_tick();
        shortcut_tick();
        shortcut_launch_tick();
        library_upgrade_tick();
        phase_end(PHASE_TICKS);
        if (g_app.whats_new_open && g_app.view != VIEW_STREAM) {
            handle_whats_new(down, repeat, action);
        } else if (g_app.discord_open && g_app.view != VIEW_STREAM) {
            handle_discord(down, action);
        } else if (g_app.guide_page >= 0 && g_app.view != VIEW_STREAM && !g_app.busy) {
            handle_guide(down, action);
        } else if (g_app.update_open && g_app.view != VIEW_STREAM && !g_app.busy) {
            handle_updates(down, repeat, action);
        } else if (g_app.busy) {
            /* The UI stays live during requests; B cancels what can be
             * cancelled, but never the request that ends the session on
             * NVIDIA's side (beta.23: players pressing B while leaving got
             * "Session stop network: Cancelled", and the game stayed open). */
            if ((down & KEY_B) && net_worker_current_job() != NET_JOB_STOP_SESSION) net_worker_cancel();
        } else if (g_app.modal != MODAL_NONE) {
            handle_modal(down, action);
        } else {
            switch (g_app.view) {
            case VIEW_HUB: handle_hub(down, repeat, action); break;
            case VIEW_LOGIN: handle_login(down, action); break;
            case VIEW_LIBRARY: handle_library(down, repeat, action); break;
            case VIEW_SETTINGS: handle_settings(down, repeat, action); break;
            case VIEW_SESSION: handle_session(down, action); break;
            case VIEW_DETAILS: handle_details(down, repeat, action); break;
            case VIEW_STREAM: handle_stream(down, held, action, touch_down, touch); break;
            }
        }
        phase_end(PHASE_HANDLE);
        if (hidKeysUp() & KEY_TOUCH) touchpad_end();
        if (queue_alert_active() && (down || touch_down)) queue_alert_stop();
        update_pointer_click(down, held);
        ui_sounds(down, action);

        /* Game input: touch-held L3/R3/PS, and nothing while menus are up. */
        const bool streaming = g_app.view == VIEW_STREAM;
        /* The touch camera's layout, unless the lower screen is the mouse
         * pad, the zoom map or the keyboard. */
        const unsigned look = streaming && !g_transport.pointer_mode && !g_transport.keyboard_mode &&
                              !mvd_video_zoomed() ? touch_camera_mode() : 0;
        g_app.look_available = look != 0;
        g_app.look_mode = g_app.settings.touch_camera_shown ? look : 0;
        look_tick(touch_down, g_app.touching, touch);
        g_app.touch_buttons = streaming ? stream_touch_buttons(touch_down, touch) : 0;
        gfn_input_set_virtual_buttons(g_app.touch_buttons | (g_app.look_r3 ? GFN_PAD_RIGHT_THUMB : 0));
        mic_tick(streaming);
        /* The phone keyboard lives as long as the game. */
        if (phone_keyboard_running()) {
            if (streaming) phone_keyboard_tick(&g_transport);
            else phone_keyboard_stop();
        }
        gfn_input_set_suppressed(!streaming || g_app.stream_menu || g_app.controls_open || g_app.modal != MODAL_NONE ||
                                 g_app.lid_paused);

        tick_network();
        refresh_device_status();
        log_session();
        phase_end(PHASE_NETWORK);

        /* While queued or setting up, stay awake even with the lid shut so
         * the queue keeps moving and the alert can fire. Once playing, only
         * "Sleep" lets the lid sleep the console (and reconnects on waking);
         * "Pause" and "Keep playing" hold the connection with screens off. */
        const bool want_sleep = !gfn_session_active(&g_client) ||
                                (g_app.stream_started_at && g_app.settings.lid_mode == LID_SLEEP);
        if (want_sleep != sleep_allowed) {
            aptSetSleepAllowed(want_sleep);
            sleep_allowed = want_sleep;
        }

        g_app.view = derive_view();
        g_app.keyboard_open = g_transport.keyboard_mode;
        /* The game's sound can start before its picture: no menu music over it. */
        const bool game_audio = g_transport.state == WEBRTC_CONNECTED && g_transport.audio_decoded > 0;
        menu_audio_set((MenuMusicMode)g_app.settings.music_mode,
                       g_app.view == VIEW_STREAM || game_audio ? MENU_SCENE_GAME
                       : g_app.view == VIEW_SESSION ? MENU_SCENE_WAITING : MENU_SCENE_MENUS);
        g_app.status = current_status();
        g_app.toast = g_notice[0] && osGetTime() < g_notice_until ? g_notice : NULL;
        if (!g_app.toast && g_transport.state == WEBRTC_CONNECTED && g_transport.steam_capture_unavailable)
            g_app.toast = g_transport.status;
        track_session();
        setup_retry_tick();
        watch_session_errors();
        watch_for_bugs();
        launch_track(&g_client);
        limit_wait_tick();
        sd_write_watch();
        phase_end(PHASE_TRACK);
        {
            const u64 last_frame = g_transport.last_decoded_frame_at, at = osGetTime();
            g_app.video_stalled = g_app.view == VIEW_STREAM && last_frame && at > last_frame &&
                                  at - last_frame > 1500;
        }
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
        if (g_app.gallery_open) gallery_pump();
        render(draw_bottom);
        /* While streaming, don't wait for vblank: sleep inside poll() on the
         * media socket so a video frame is decoded the moment its packets
         * land. The 4 ms cap keeps input sampling fast. Build 49 spun on
         * nonblocking recv every 1 ms instead, and that request flood crashed
         * the system socket module when video started. */
        if (g_app.view == VIEW_STREAM) wait_for_media(4);
        phase_end(PHASE_RENDER);
    }

    finish_history();
    if (g_perf.active) perf_end(g_app.settings.install_id);
    aptUnhook(&g_apt_cookie);
    queue_alert_exit();
    /* The media core stops before the sound it feeds is closed. */
    webrtc_transport_close(&g_transport);
    audio_output_close();
    menu_audio_exit();
    sfx_exit();
    audio_system_exit();
    if (gfn_session_active(&g_client)) {
        close_media();
        net_worker_wait_idle(8000);
        if (net_worker_submit(NET_JOB_STOP_SESSION, NULL, NULL)) net_worker_wait_idle(8000);
    }
    /* A session summary still waiting (closing right after playing is
     * common): one quick try, else it goes out on the next start. */
    if (g_app.settings.share_stats && report_available() && report_stats_pending()) {
        net_worker_wait_idle(3000);
        if (net_worker_submit(NET_JOB_SEND_STATS, NULL, NULL)) net_worker_wait_idle(3000);
    }
    aptSetSleepAllowed(true);
    net_worker_stop();
    game_art_exit();
    shutdown_services();
    ui_exit();
    gfxExit();
    return 0;
}

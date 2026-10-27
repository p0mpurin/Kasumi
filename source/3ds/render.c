#include "render.h"
#include "platform.h"
#include "diagnostic.h"
#include "mvd_video.h"
#include "perf_stats.h"
#include "screenshot.h"
#include "shortcut.h"
#include "stream_profile.h"
#include "ui.h"
#include "ui_3ds.h"
#include "webrtc_transport.h"

#include <3ds.h>
#include <citro3d.h>

/* The 3DS's two screens: menus on both, or video on the top one paced to
 * its ~60 Hz refresh with the lower screen redrawn when asked. */

static bool g_screenshot_requested;
static const char *g_notice;
/* Main-loop health while streaming: the longest iteration and how many took
 * over 25 ms (a stalled loop delays packets, input and presents alike). */
static unsigned g_loop_max_ms, g_loop_slow;

/* Frames put on the screen. The FPS readout counted decoded frames, so a
 * picture held back by the pacer or a stalled loop still read "30" while it
 * looked like 5 (player report, 2026-10-10). */
static unsigned g_shown_frames;

static void present_video(void)
{
    static unsigned last_frame;
    const unsigned frame = mvd_video_decoded_frames();
    if (frame == last_frame) return;
    last_frame = frame;
    ++g_shown_frames;
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
static void render_wide_video(const App *app, bool draw_bottom)
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
    /* How long shown frames waited after decoding: input lag of our own. */
    static u64 wait_sum_us;
    static unsigned wait_max_us, hd_shifts;
    static float hd_wait_ms;
    static u64 last_hd_shift;
    bool present = false;
    /* citro3d counts top-screen vblanks in its own GSP handler (only one
     * handler per event is allowed, so we must not register another). */
    const u32 vblank = C3D_FrameCounter(0);
    if (vblank != last_seen_vblank) {
        const u32 since = vblank - last_present_vblank;
        last_seen_vblank = vblank;
        const u64 now_ms = plat_time_ms();
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
            sixty = stream_profile_fps() >= 60 && !mvd_video_hd();
            rate_since = now_ms;
            rate_frames = decoded_now;
        } else if (now_ms - rate_since >= 2000) {
            const unsigned fps = (unsigned)((decoded_now - rate_frames) * 1000ull / (now_ms - rate_since));
            const bool was = sixty;
            /* Xbox's 720p stays at a steady 30. Shrunk to 800x480 it decoded
             * at ~45 a second and the cadence flipped 30/60 every 2 s (reports
             * 62AEG4, FURPBY). At half size it decodes 60 in 14.5 ms while
             * shown at 30, but showing all 60 slowed MVD to ~18 ms a frame:
             * it fell behind and dropped to a keyframe every 6 s (build 147,
             * report 7XXNYS). */
            sixty = stream_profile_fps() >= 60 && fps >= 45 && !mvd_video_hd();
            if (sixty != was) diagnostic_log("VIDEO", "pacing %s (%u fps arriving)", sixty ? "60" : "30", fps);
            rate_since = now_ms;
            rate_frames = decoded_now;
        }
        /* Decoded frames waiting to be shown are input lag: at 60 fps keep
         * one beyond the reserve (~33-50 ms in all), not three (report
         * EM7YGV felt the 30 fps cap as lag at 60). Xbox 720p comes in at
         * 60 but mvd_video keeps every second frame, so it is paced like
         * any 30 fps stream. */
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
            const unsigned age = mvd_video_ready_frame_age_us(0);
            wait_sum_us += age;
            if (age > wait_max_us) wait_max_us = age;
            depth_sum += ready;
        }
        /* Xbox's 60 fps against the LCD's 59.83 Hz: the 30 kept frames come
         * a little faster than they are shown, so each waits longer than the
         * last until a whole spare frame (+33 ms) builds up, which the drain
         * below lets sit for up to 5 s. Instead, once frames wait more than
         * ~28 ms on average, the next 60 fps frame is kept rather than the
         * one after: 17 ms less wait, one 50 ms step in the game instead of
         * 33. Kept clear of repeats, which a wait near 0 would bring. */
        /* Only while 60 is halved: a 30 fps game (issue #12) is paced like
         * GeForce NOW's 30, surplus drain included. */
        const bool xbox_hd = !sixty && mvd_video_hd_halving();
        if (present && xbox_hd) {
            const float age_ms = (float)mvd_video_ready_frame_age_us(0) / 1000.0f;
            hd_wait_ms = hd_wait_ms > 0.0f ? hd_wait_ms * 0.9f + age_ms * 0.1f : age_ms;
            if ((hd_wait_ms > 28.0f || ready >= 2) && now_ms - last_hd_shift >= 500 &&
                now_ms - last_repeat_at >= 2000) {
                mvd_video_hd_skip_one();
                hd_wait_ms -= 16.7f;
                last_hd_shift = now_ms;
                ++hd_shifts;
            }
        }
        if (present && !xbox_hd) {
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
    const u64 start = plat_ticks();
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
        g_notice = screenshot_capture(frame) ? "Saving screenshot..." : "Screenshot failed";
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
        screens_draw_bottom(app);
    }
    ui_frame_end();
    if (!frame) return;
    ++g_shown_frames;
    last_present_vblank = vblank;
    const u64 ticks = plat_ticks() - start;
    present_ticks += ticks;
    if (ticks > present_max) present_max = ticks;
    /* Every 600 frames (~20 s), sooner when frames were repeated or skipped. */
    const unsigned pending_frames = ++presented - last_log_frames;
    if (pending_frames >= 600 || (pending_frames >= 120 && (repeated > 6 || skipped > 3))) {
        const u64 per_us = PLAT_TICKS_PER_SEC / 1000000u;
        const unsigned frames = presented - last_log_frames;
        diagnostic_log("VIDEO", "paced present frames=%u avg/max=%llu/%llu us repeated=%u skipped=%u drained=%u quiet=%u depth=%u.%02u reserve=%u loopMax=%u slow=%u wait=%llu/%u ms shifts=%u",
                       frames,
                       (unsigned long long)(present_ticks / frames / per_us),
                       (unsigned long long)(present_max / per_us), repeated, skipped, drained,
                       quiet_drops, depth_sum / frames, depth_sum * 100 / frames % 100, reserve,
                       g_loop_max_ms, g_loop_slow, (unsigned long long)(wait_sum_us / frames / 1000u),
                       wait_max_us / 1000u, hd_shifts);
        wait_sum_us = 0;
        wait_max_us = hd_shifts = 0;
        g_loop_max_ms = g_loop_slow = 0;
        present_ticks = present_max = 0;
        repeated = skipped = drained = quiet_drops = depth_sum = 0;
        last_log_frames = presented;
    }
}

void render(const App *app, bool draw_bottom)
{
    const bool video = app->view == VIEW_STREAM;
    if (video && mvd_video_wide()) {
        render_wide_video(app, draw_bottom);
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
        screens_draw_top(app);
    }
    ui_begin_bottom();
    screens_draw_bottom(app);
    ui_frame_end();
    shortcut_frame_end();
    if (video) present_video();
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

/* The lower screen is the touch screen. */
PlatTouch render_map_touch(PlatTouch touch) { return touch; }

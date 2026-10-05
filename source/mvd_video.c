#include "mvd_video.h"
#include "diagnostic.h"
#include "stream_profile.h"

#include <3ds.h>
#include <stdio.h>
#include <string.h>

enum { OUTPUT_WIDTH = 400, OUTPUT_HEIGHT = 240, INPUT_CAPACITY = 512 * 1024,
       WIDE_OUTPUT_WIDTH = 800, WIDE_OUTPUT_HEIGHT = 480,
       HD_OUTPUT_WIDTH = 1280, HD_OUTPUT_HEIGHT = 720,
       HD_OUTPUT_ALIGNED_HEIGHT = 720,
       MVD_HD_NO_PROGRESS = 0x17040 };

static bool g_active;
static unsigned char *g_input;
static u16 *g_output;
static MVDSTD_Config g_config;
static unsigned g_frames, g_errors;
static bool g_picture_dark;
static unsigned g_input_width, g_input_height;
static unsigned g_output_width, g_output_height, g_output_alloc_height;
static unsigned g_zoom_level;
static unsigned g_zoom_center_x, g_zoom_center_y;
static u64 g_last_zoom_pan_at;
static bool g_first_process_done;
static bool g_first_render_done;
static bool g_process_failed;
static bool g_first_sps_logged;
static unsigned g_sps_level_rewrites;
static unsigned g_hd_nal_logs;
static unsigned g_status_ok, g_status_paramset, g_status_ready, g_status_incomplete;
static u64 g_perf_process_sum, g_perf_render_sum, g_perf_copy_sum;
static u64 g_perf_process_max, g_perf_render_max, g_perf_copy_max;
static unsigned g_perf_samples;
static u64 g_decode_total_us;
static unsigned g_decode_total_count, g_decode_max_us, g_frames_lost;
static char g_status[128] = "MVD idle";
/* Wide mode: the top screen shows 800 columns. MVD scales the 960x540
 * source gently to 800x480 inside a 1024x512 buffer (the GPU texture size,
 * as Moonlight-N3DS does); the UI then turns it into a texture with one
 * display transfer and the GPU draws it at half height, where bilinear
 * filtering averages each pair of rows. Build 53 did that averaging and the
 * rotation on the CPU and spent ~19 ms per frame on it. */
static bool g_wide;
static unsigned g_output_stride;

/* Wide mode decodes on its own thread. Build 56 decoded on the UI thread:
 * during each ~11.5 ms MVD call plus the GPU present nobody read the socket,
 * NVIDIA's per-frame bursts overflowed it, and 19% of packets arrived
 * reordered/retransmitted (build 46: 0.5%), which is the stutter. Now the
 * UI thread only queues access units and keeps draining the network. */
/* Build 57 used 4 slots and overflowed whenever frames arrived bunched up
 * (60 drops in one session, each a corrupted "flash"). */
/* Wide mode queues access units for the decoder thread in one ring of
 * linear memory, each AU contiguous and 0x80-aligned for MVD. Beta.29
 * exports: ten fixed 192 KB slots (1.9 MB, ~4 KB AUs at 1 Mbps) overflowed
 * on the burst that follows a network stall (maxGap ~400 ms), and every
 * overflow froze the picture until a keyframe ("decoder-backlog"). The ring
 * holds 48 AUs (1.6 s) in about the same memory. */
enum { AU_ENTRIES = 48, AU_RING_SIZE = 2 * 1024 * 1024, AU_MAX = AU_RING_SIZE / 4, WIDE_OUTPUTS = 6 };
static Thread g_decoder;
static volatile bool g_decoder_quit;
static LightLock g_queue_lock;
static LightEvent g_queue_event;
static unsigned char *g_ring;
static size_t g_entry_offset[AU_ENTRIES], g_entry_size[AU_ENTRIES];
/* Where the next AU goes (submitting thread only, under g_queue_lock). */
static size_t g_ring_write;
static unsigned g_queue_head, g_queue_count;
static volatile bool g_resync_requested;
/* After a dropped access unit, later P-frames would decode against missing
 * references and flash garbage; skip them until the next IDR instead. */
static bool g_await_idr;
/* Wide output buffers flow FREE -> DECODING -> READY (FIFO) -> PRESENTING
 * -> FREE. The UI's frame pacer takes READY frames in order, one per two
 * vblanks; if the pacer falls behind, the decoder recycles the oldest. */
enum { OUT_FREE, OUT_DECODING, OUT_READY, OUT_PRESENTING };
static u16 *g_wide_outputs[WIDE_OUTPUTS];
static int g_output_state[WIDE_OUTPUTS];
static int g_ready_fifo[WIDE_OUTPUTS];
static size_t g_output_bytes[WIDE_OUTPUTS];
static unsigned g_ready_count;
static unsigned g_recycled_frames;
static LightLock g_frame_lock;
/* Zoom edits g_config on the UI thread while the decoder renders with it. */
static LightLock g_config_lock;
static unsigned g_level_min = 255, g_level_max;

static void decoder_main(void *arg);

static void record_performance(u64 process_ticks, u64 render_ticks, u64 copy_ticks)
{
    const unsigned process_us = (unsigned)(process_ticks / (SYSCLOCK_ARM11 / 1000000u));
    /* A decode that spans sleep or the HOME Menu is not decoder time
     * (beta.18 stats: a 417 s "slowest decode"). */
    if (process_us > 1000000u) return;
    g_decode_total_us += process_us;
    ++g_decode_total_count;
    if (process_us > g_decode_max_us) g_decode_max_us = process_us;
    g_perf_process_sum += process_ticks;
    g_perf_render_sum += render_ticks;
    g_perf_copy_sum += copy_ticks;
    if (process_ticks > g_perf_process_max) g_perf_process_max = process_ticks;
    if (render_ticks > g_perf_render_max) g_perf_render_max = render_ticks;
    if (copy_ticks > g_perf_copy_max) g_perf_copy_max = copy_ticks;
    if (++g_perf_samples < 600) return; /* ~20 s */

    const u64 ticks_per_us = SYSCLOCK_ARM11 / 1000000u;
    diagnostic_log("MVD", "perf frames=%u process avg/max=%llu/%llu us render avg/max=%llu/%llu us copy avg/max=%llu/%llu us",
                   g_perf_samples,
                   (unsigned long long)(g_perf_process_sum / g_perf_samples / ticks_per_us),
                   (unsigned long long)(g_perf_process_max / ticks_per_us),
                   (unsigned long long)(g_perf_render_sum / g_perf_samples / ticks_per_us),
                   (unsigned long long)(g_perf_render_max / ticks_per_us),
                   (unsigned long long)(g_perf_copy_sum / g_perf_samples / ticks_per_us),
                   (unsigned long long)(g_perf_copy_max / ticks_per_us));
    g_perf_process_sum = g_perf_render_sum = g_perf_copy_sum = 0;
    g_perf_process_max = g_perf_render_max = g_perf_copy_max = 0;
    g_perf_samples = 0;
}

bool mvd_video_init(unsigned input_width, unsigned input_height)
{
    mvd_video_close();
    g_frames = 0;
    g_errors = 0;
    g_picture_dark = false;
    g_input_width = input_width;
    g_input_height = input_height;
    /* At 720p, render into a full-size linear surface before scaling to the
     * top screen.  Moonlight's MVD path says the decoder cannot downsample;
     * Video_player_for_3DS also configures its output at source dimensions.
     * Keep the proven direct 400x240 path for 540p. */
    const bool hd = input_width > 960 || input_height > 544;
    g_wide = stream_profile_wide() && !hd;
    if (hd) {
        g_output_width = HD_OUTPUT_WIDTH;
        g_output_height = HD_OUTPUT_HEIGHT;
    } else if (g_wide) {
        g_output_width = WIDE_OUTPUT_WIDTH;
        g_output_height = WIDE_OUTPUT_HEIGHT;
        /* The decoder only shrinks: Borderlands 2 switched its stream to
         * 726x544 (4:3) and stretching that to 800 wide failed with "MVD
         * config failed D961710D". A narrower or smaller picture keeps its
         * shape at the largest size that fits; the GPU draws it centred. */
        if (input_width < WIDE_OUTPUT_WIDTH || input_height < WIDE_OUTPUT_HEIGHT) {
            float scale = (float)WIDE_OUTPUT_WIDTH / (float)input_width;
            const float by_height = (float)WIDE_OUTPUT_HEIGHT / (float)input_height;
            if (by_height < scale) scale = by_height;
            if (scale > 1.0f) scale = 1.0f;
            g_output_width = (unsigned)((float)input_width * scale) & ~15u;
            g_output_height = (unsigned)((float)input_height * scale) & ~15u;
        }
    } else {
        g_output_width = OUTPUT_WIDTH;
        g_output_height = OUTPUT_HEIGHT;
    }
    g_output_stride = g_wide ? MVD_VIDEO_TEX_WIDTH : g_output_width;
    g_output_alloc_height = hd ? HD_OUTPUT_ALIGNED_HEIGHT :
                            g_wide ? MVD_VIDEO_TEX_HEIGHT : g_output_height;
    g_zoom_level = 0;
    g_zoom_center_x = input_width / 2;
    g_zoom_center_y = input_height / 2;
    g_last_zoom_pan_at = 0;
    g_first_process_done = false;
    g_first_render_done = false;
    g_process_failed = false;
    g_first_sps_logged = false;
    g_sps_level_rewrites = 0;
    g_hd_nal_logs = 0;
    g_status_ok = g_status_paramset = g_status_ready = g_status_incomplete = 0;
    g_perf_process_sum = g_perf_render_sum = g_perf_copy_sum = 0;
    g_perf_process_max = g_perf_render_max = g_perf_copy_max = 0;
    g_perf_samples = 0;
    MVDSTD_CalculateWorkBufSizeConfig calc = {0};
    calc.level.enable = true;
    calc.level.flag = MVD_CALC_WITH_LEVEL_FLAG_ENABLE_CALC |
                      MVD_CALC_WITH_LEVEL_FLAG_ENABLE_EXTRA_OP |
                      MVD_CALC_WITH_LEVEL_FLAG_UNK;
    /* The 720p NVST answer requests at most four reference frames.  The
     * transport checks the SPS before initializing MVD, so Level 3.2 can
     * reserve enough slots without the unsafe 15-reference-frame buffer. */
    calc.level.level = input_width > 960 || input_height > 544 ?
                       MVD_H264_LEVEL_3_2 : MVD_H264_LEVEL_4_2;
    calc.width = input_width;
    calc.height = input_height;
    u32 work_size = 0;
    diagnostic_log("MVD", "preflight-enter %ux%u linearFreeKiB=%lu",
                   input_width, input_height,
                   (unsigned long)(linearSpaceFree() / 1024));
    diagnostic_checkpoint();
    /* Fail rather than wait if mvd:STD is not there: a blocking lookup of a
     * service whose module is not running never returns, and it froze the
     * whole console (beta.5 CIA). The policy is restored after init. */
    srvSetBlockingPolicy(true);
    Result rc = mvdstdCalculateBufferSize(&calc, &work_size);
    diagnostic_log("MVD", "preflight %ux%u work=%lu sizeResult=%08lX linearFreeKiB=%lu",
                   input_width, input_height, (unsigned long)work_size,
                   (unsigned long)rc, (unsigned long)(linearSpaceFree() / 1024));
    diagnostic_checkpoint();
    if (rc != 0) {
        srvSetBlockingPolicy(false);
        snprintf(g_status, sizeof(g_status), "MVD size failed %08lX", (unsigned long)rc);
        diagnostic_log("MVD", "%s", g_status);
        return false;
    }
    const u32 output_size = g_output_stride * g_output_alloc_height * sizeof(u16);
    const u32 reserve_size = 1024 * 1024;
    if ((u64)work_size + INPUT_CAPACITY + output_size + reserve_size > linearSpaceFree()) {
        srvSetBlockingPolicy(false);
        snprintf(g_status, sizeof(g_status), "MVD 720p needs %lu KiB linear memory",
                 (unsigned long)((work_size + INPUT_CAPACITY + output_size + reserve_size) / 1024));
        diagnostic_log("MVD", "%s", g_status);
        diagnostic_checkpoint();
        return false;
    }
    diagnostic_log("MVD", "init-enter %ux%u work=%lu", input_width, input_height,
                   (unsigned long)work_size);
    diagnostic_checkpoint();
    rc = mvdstdInit(MVDMODE_VIDEOPROCESSING, MVD_INPUT_H264,
                    MVD_OUTPUT_BGR565, work_size, NULL);
    srvSetBlockingPolicy(false);
    if (rc != 0) {
        snprintf(g_status, sizeof(g_status), "MVD init failed %08lX", (unsigned long)rc);
        diagnostic_log("MVD", "%s", g_status);
        diagnostic_checkpoint();
        mvdstdExit();
        return false;
    }
    diagnostic_log("MVD", "init-return %ux%u rc=%08lX", input_width, input_height,
                   (unsigned long)rc);
    diagnostic_checkpoint();
    g_active = true;
    g_input = linearMemAlign(INPUT_CAPACITY, 0x80);
    g_output = linearMemAlign(output_size, 0x80);
    bool buffers_ok = g_input && g_output;
    if (g_wide) {
        g_wide_outputs[0] = g_output;
        for (unsigned i = 1; i < WIDE_OUTPUTS; ++i) {
            g_wide_outputs[i] = linearMemAlign(output_size, 0x80);
            buffers_ok = buffers_ok && g_wide_outputs[i];
        }
        g_ring = linearMemAlign(AU_RING_SIZE, 0x80);
        buffers_ok = buffers_ok && g_ring;
    }
    if (!buffers_ok) {
        snprintf(g_status, sizeof(g_status), "MVD linear allocation failed");
        diagnostic_log("MVD", "%s", g_status);
        mvd_video_close();
        return false;
    }
    memset(g_output, 0, output_size);
    diagnostic_log("MVD", "config-generate-enter input=%ux%u output=%ux%u",
                   input_width, input_height, g_output_width, g_output_height);
    diagnostic_checkpoint();
    mvdstdGenerateDefaultConfig(&g_config, input_width, input_height,
                                g_output_width, g_output_height, NULL,
                                (u32 *)g_output, NULL);
    diagnostic_log("MVD", "config-generate-return");
    diagnostic_checkpoint();
    /* The override sets the buffer pitch: wide output lands in the top-left
     * 800x480 of a 1024x512 surface that can be tiled into a GPU texture. */
    g_config.flag_x104 = 1;
    g_config.output_width_override = g_output_stride;
    g_config.output_height_override = g_output_alloc_height;
    diagnostic_log("MVD", "set-config-enter");
    diagnostic_checkpoint();
    rc = MVDSTD_SetConfig(&g_config);
    diagnostic_log("MVD", "set-config-return rc=%08lX", (unsigned long)rc);
    diagnostic_checkpoint();
    if (rc != MVD_STATUS_OK && g_wide && g_output_height > 240) {
        /* An unusual stream size refused at full size: try half size, which
         * the GPU scales up (better a softer picture than none). */
        float scale = 400.0f / (float)input_width;
        const float by_height = 240.0f / (float)input_height;
        if (by_height < scale) scale = by_height;
        g_output_width = (unsigned)((float)input_width * scale) & ~15u;
        g_output_height = (unsigned)((float)input_height * scale) & ~15u;
        mvdstdGenerateDefaultConfig(&g_config, input_width, input_height, g_output_width, g_output_height,
                                    NULL, (u32 *)g_output, NULL);
        g_config.flag_x104 = 1;
        g_config.output_width_override = g_output_stride;
        g_config.output_height_override = g_output_alloc_height;
        rc = MVDSTD_SetConfig(&g_config);
        diagnostic_log("MVD", "set-config retry output=%ux%u rc=%08lX", g_output_width, g_output_height,
                       (unsigned long)rc);
    }
    /* MVDSTD commands use their own status range.  0x17000 is success,
     * despite being non-zero as a normal libctru Result value. */
    if (rc != MVD_STATUS_OK) {
        snprintf(g_status, sizeof(g_status), "MVD config failed %08lX", (unsigned long)rc);
        diagnostic_log("MVD", "%s", g_status);
        mvd_video_close();
        return false;
    }
    if (g_wide) {
        for (unsigned i = 1; i < WIDE_OUTPUTS; ++i)
            memset(g_wide_outputs[i], 0, output_size);
        LightLock_Init(&g_queue_lock);
        LightLock_Init(&g_frame_lock);
        LightLock_Init(&g_config_lock);
        LightEvent_Init(&g_queue_event, RESET_ONESHOT);
        g_queue_head = g_queue_count = 0;
        g_ring_write = 0;
        for (unsigned i = 0; i < WIDE_OUTPUTS; ++i) g_output_state[i] = OUT_FREE;
        g_ready_count = 0;
        g_recycled_frames = 0;
        g_resync_requested = false;
        g_await_idr = false;
        g_decoder_quit = false;
        s32 priority = 0x30;
        svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);
        /* Above the UI thread: it spends almost all its time blocked in the
         * MVD service, and should resume the moment a frame is done. */
        g_decoder = threadCreate(decoder_main, NULL, 64 * 1024, priority - 1, -2, false);
        if (!g_decoder) {
            snprintf(g_status, sizeof(g_status), "MVD decoder thread failed");
            diagnostic_log("MVD", "%s", g_status);
            mvd_video_close();
            return false;
        }
    }
    snprintf(g_status, sizeof(g_status), "MVD ready %ux%u -> %ux%u -> %ux240%s",
             input_width, input_height, g_output_width, g_output_height,
             g_wide ? 800 : 400, g_wide ? " (GPU)" : "");
    diagnostic_log("MVD", "%s config=%08lX", g_status, (unsigned long)rc);
    return true;
}

/* Wide zoom is a crop on the GPU (fractions of the picture), not in the
 * decoder: after an MVD crop was set and cleared, decoding stayed ~2.5 ms a
 * frame slower (15.5 -> 18 ms), past the 16.7 ms 60 fps has, and the frames
 * queued up as input lag (reports GWU2AV and the build 116 export). */
static float g_view_crop[4] = { 0.0f, 0.0f, 1.0f, 1.0f };

static bool set_wide_zoom(unsigned level, unsigned center_x, unsigned center_y)
{
    if (level == g_zoom_level && center_x == g_zoom_center_x && center_y == g_zoom_center_y)
        return false;
    const float size = level == 1 ? 1.0f / 1.2f : level == 2 ? 1.0f / 1.5f : level == 3 ? 0.5f : 1.0f;
    float x = g_input_width ? (float)center_x / (float)g_input_width - size / 2.0f : 0.0f;
    float y = g_input_height ? (float)center_y / (float)g_input_height - size / 2.0f : 0.0f;
    if (x < 0.0f) x = 0.0f;
    if (y < 0.0f) y = 0.0f;
    if (x > 1.0f - size) x = 1.0f - size;
    if (y > 1.0f - size) y = 1.0f - size;
    g_view_crop[0] = x;
    g_view_crop[1] = y;
    g_view_crop[2] = size;
    g_view_crop[3] = size;
    if (level != g_zoom_level)
        diagnostic_log("MVD", "zoom=%u (GPU) view=%u%%x%u%%+%u%%+%u%%", level,
                       (unsigned)(size * 100.0f), (unsigned)(size * 100.0f),
                       (unsigned)(x * 100.0f), (unsigned)(y * 100.0f));
    g_zoom_level = level;
    g_zoom_center_x = center_x;
    g_zoom_center_y = center_y;
    return true;
}

void mvd_video_view_crop(float *x, float *y, float *w, float *h)
{
    const bool on = g_wide && g_zoom_level;
    if (x) *x = on ? g_view_crop[0] : 0.0f;
    if (y) *y = on ? g_view_crop[1] : 0.0f;
    if (w) *w = on ? g_view_crop[2] : 1.0f;
    if (h) *h = on ? g_view_crop[3] : 1.0f;
}

static bool set_zoom(unsigned level, unsigned center_x, unsigned center_y)
{
    if (!g_active) return false;
    if (g_wide) return set_wide_zoom(level, center_x, center_y);

    MVDSTD_Config next = g_config;
    if (level) {
        /* MVD crops the source before rendering into the existing surface.
         * Keep crop dimensions and offsets aligned to H.264 macroblocks. */
        const unsigned base_width = level == 1 ? 800 : level == 2 ? 640 : 480;
        const unsigned base_height = level == 1 ? 480 : level == 2 ? 384 : 288;
        const unsigned target_width = ((base_width * g_input_width / 960 + 8) / 16) * 16;
        const unsigned target_height = ((base_height * g_input_height / 544 + 8) / 16) * 16;
        const unsigned crop_width = g_input_width >= target_width ? target_width : g_input_width;
        const unsigned crop_height = g_input_height >= target_height ? target_height : g_input_height;
        unsigned x = center_x > crop_width / 2 ? center_x - crop_width / 2 : 0;
        unsigned y = center_y > crop_height / 2 ? center_y - crop_height / 2 : 0;
        if (x > g_input_width - crop_width) x = g_input_width - crop_width;
        if (y > g_input_height - crop_height) y = g_input_height - crop_height;
        x &= ~15u;
        y &= ~15u;
        next.enable_cropping = 1;
        next.input_crop_x_pos = x;
        next.input_crop_y_pos = y;
        next.input_crop_width = crop_width;
        next.input_crop_height = crop_height;
    } else {
        next.enable_cropping = 0;
        next.input_crop_x_pos = 0;
        next.input_crop_y_pos = 0;
        next.input_crop_width = 0;
        next.input_crop_height = 0;
    }

    if (level == g_zoom_level &&
        next.input_crop_x_pos == g_config.input_crop_x_pos &&
        next.input_crop_y_pos == g_config.input_crop_y_pos)
        return false;
    Result rc = MVDSTD_SetConfig(&next);
    if (rc != MVD_STATUS_OK) {
        diagnostic_log("MVD", "zoom config failed enable=%u rc=%08lX",
                       level, (unsigned long)rc);
        return false;
    }
    g_config = next;
    g_zoom_level = level;
    g_zoom_center_x = center_x;
    g_zoom_center_y = center_y;
    snprintf(g_status, sizeof(g_status), "MVD decoded frame %u; zoom %u",
             g_frames, g_zoom_level);
    diagnostic_log("MVD", "zoom=%u crop=%ux%u+%u+%u -> 400x240",
                   g_zoom_level,
                   (unsigned)g_config.input_crop_width,
                   (unsigned)g_config.input_crop_height,
                   (unsigned)g_config.input_crop_x_pos,
                   (unsigned)g_config.input_crop_y_pos);
    return true;
}

bool mvd_video_set_zoom(unsigned level, unsigned x_percent, unsigned y_percent)
{
    if (!g_active || level > 3) return false;
    if (x_percent > 100) x_percent = 100;
    if (y_percent > 100) y_percent = 100;
    return set_zoom(level, x_percent * g_input_width / 100, y_percent * g_input_height / 100);
}

bool mvd_video_toggle_zoom(void)
{
    const unsigned next_level = (g_zoom_level + 1) % 4;
    return set_zoom(next_level, g_zoom_center_x, g_zoom_center_y);
}

bool mvd_video_pan_to_touch(unsigned touch_x, unsigned touch_y)
{
    if (!g_active || !g_zoom_level || touch_x >= 320 || touch_y < 128 || touch_y >= 192)
        return false;
    const u64 now = osGetTime();
    if (g_last_zoom_pan_at && now - g_last_zoom_pan_at < (g_wide ? 16u : 100u)) return false;
    const unsigned center_x = touch_x * g_input_width / 319;
    const unsigned center_y = (touch_y - 128) * g_input_height / 63;
    g_last_zoom_pan_at = now;
    return set_zoom(g_zoom_level, center_x, center_y);
}

bool mvd_video_pan_to(unsigned x_permille, unsigned y_permille)
{
    if (!g_active || !g_zoom_level) return false;
    const u64 now = osGetTime();
    if (g_last_zoom_pan_at && now - g_last_zoom_pan_at < (g_wide ? 16u : 100u)) return false;
    if (x_permille > 1000) x_permille = 1000;
    if (y_permille > 1000) y_permille = 1000;
    g_last_zoom_pan_at = now;
    return set_zoom(g_zoom_level, x_permille * g_input_width / 1000,
                    y_permille * g_input_height / 1000);
}

bool mvd_video_zoomed(void) { return g_zoom_level != 0; }
unsigned mvd_video_zoom_level(void) { return g_zoom_level; }
void mvd_video_zoom_position(unsigned *x, unsigned *y)
{
    if (x) *x = g_input_width ? g_zoom_center_x * 100 / g_input_width : 50;
    if (y) *y = g_input_height ? g_zoom_center_y * 100 / g_input_height : 50;
}

static void copy_to_top_framebuffer(void)
{
    u16 width = 0, height = 0;
    u16 *framebuffer = (u16 *)gfxGetFramebuffer(GFX_TOP, GFX_LEFT, &width, &height);
    if (!framebuffer || width != 240 || height != OUTPUT_WIDTH) return;
    /* 3DS framebuffers are physically rotated: x is the major dimension. */
    if (g_output_width == OUTPUT_WIDTH && g_output_height == OUTPUT_HEIGHT) {
        for (unsigned y = 0; y < OUTPUT_HEIGHT; ++y)
            for (unsigned x = 0; x < OUTPUT_WIDTH; ++x)
                framebuffer[x * OUTPUT_HEIGHT + (OUTPUT_HEIGHT - 1 - y)] =
                    g_output[y * OUTPUT_WIDTH + x];
    } else {
        /* Nearest-neighbour is cheap enough for the media callback and keeps
         * the extra 720p stage bounded to a single reusable linear buffer. */
        for (unsigned y = 0; y < OUTPUT_HEIGHT; ++y) {
            const unsigned src_y = y * g_output_height / OUTPUT_HEIGHT;
            const u16 *row = g_output + src_y * g_output_stride;
            for (unsigned x = 0; x < OUTPUT_WIDTH; ++x)
                framebuffer[x * OUTPUT_HEIGHT + (OUTPUT_HEIGHT - 1 - y)] =
                    row[x * g_output_width / OUTPUT_WIDTH];
        }
    }
}

bool mvd_video_wide(void) { return g_active && g_wide; }

void mvd_video_wide_size(unsigned *width, unsigned *height)
{
    const bool known = g_wide && g_output_width && g_output_height;
    *width = known ? g_output_width : WIDE_OUTPUT_WIDTH;
    *height = known ? g_output_height : WIDE_OUTPUT_HEIGHT;
}

unsigned mvd_video_ready_frames(void)
{
    if (!g_wide) return 0;
    LightLock_Lock(&g_frame_lock);
    const unsigned count = g_ready_count;
    LightLock_Unlock(&g_frame_lock);
    return count;
}

/* Pop the oldest ready frame (FIFO); caller must release it. */
/* Held from taking a frame for the GPU until it is released, so the
 * decoder can't be closed (a new stream size) under the copy. The UI used
 * to hold the transport lock for this instead, and the media thread waited
 * up to 11 ms for it while the copy waited on the GPU (report KA48GF). */
static LightLock g_present_lock = 1;

const void *mvd_video_take_gpu_frame(void)
{
    LightLock_Lock(&g_present_lock);
    if (!g_wide) {
        LightLock_Unlock(&g_present_lock);
        return NULL;
    }
    int index = -1;
    LightLock_Lock(&g_frame_lock);
    if (g_ready_count) {
        index = g_ready_fifo[0];
        memmove(g_ready_fifo, g_ready_fifo + 1, --g_ready_count * sizeof(int));
        g_output_state[index] = OUT_PRESENTING;
    }
    LightLock_Unlock(&g_frame_lock);
    if (index < 0) LightLock_Unlock(&g_present_lock);
    return index >= 0 ? g_wide_outputs[index] : NULL;
}

void mvd_video_release_gpu_frame(void)
{
    LightLock_Lock(&g_frame_lock);
    for (unsigned i = 0; i < WIDE_OUTPUTS; ++i)
        if (g_output_state[i] == OUT_PRESENTING) g_output_state[i] = OUT_FREE;
    LightLock_Unlock(&g_frame_lock);
    LightLock_Unlock(&g_present_lock);
}

/* Discard the oldest ready frame so latency cannot build up. */
void mvd_video_skip_oldest_frame(void)
{
    if (!g_wide) return;
    LightLock_Lock(&g_frame_lock);
    if (g_ready_count) {
        g_output_state[g_ready_fifo[0]] = OUT_FREE;
        memmove(g_ready_fifo, g_ready_fifo + 1, --g_ready_count * sizeof(int));
    }
    LightLock_Unlock(&g_frame_lock);
}

size_t mvd_video_ready_frame_bytes(unsigned index)
{
    if (!g_wide) return 0;
    LightLock_Lock(&g_frame_lock);
    const size_t bytes = index < g_ready_count ? g_output_bytes[g_ready_fifo[index]] : 0;
    LightLock_Unlock(&g_frame_lock);
    return bytes;
}

void mvd_video_skip_ready_frame(unsigned index)
{
    if (!g_wide) return;
    LightLock_Lock(&g_frame_lock);
    if (index < g_ready_count) {
        g_output_state[g_ready_fifo[index]] = OUT_FREE;
        memmove(g_ready_fifo + index, g_ready_fifo + index + 1,
                (g_ready_count - index - 1) * sizeof(int));
        --g_ready_count;
    }
    LightLock_Unlock(&g_frame_lock);
}

void mvd_video_resync(void)
{
    if (!g_await_idr) {
        diagnostic_log("MVD", "frame lost upstream; holding picture until IDR");
        ++g_frames_lost;
    }
    g_await_idr = true;
}

unsigned mvd_video_drop_backlog(void)
{
    if (!g_wide) return 0;
    LightLock_Lock(&g_queue_lock);
    /* The head may be in the decoder's hands: keep it. */
    const unsigned dropped = g_queue_count > 1 ? g_queue_count - 1 : 0;
    if (dropped) g_queue_count = 1;
    LightLock_Unlock(&g_queue_lock);
    if (!dropped) return 0;
    g_await_idr = true;
    g_resync_requested = true;
    return dropped;
}

bool mvd_video_take_resync_request(void)
{
    const bool requested = g_resync_requested;
    g_resync_requested = false;
    return requested;
}

static void prepare_720p_sps(unsigned char *data, size_t size)
{
    if (g_input_width != 1280 || g_input_height != 720) return;
    for (size_t i = 0; i + 7 < size; ++i) {
        size_t header = 0;
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1)
            header = i + 3;
        else if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 0 && data[i + 3] == 1)
            header = i + 4;
        if (!header || header + 3 >= size || (data[header] & 0x1f) != 7)
            continue;
        if (!g_first_sps_logged) {
            char hex[3 * 40 + 1];
            unsigned count = size - header < 40 ? (unsigned)(size - header) : 40;
            for (unsigned j = 0; j < count; ++j)
                snprintf(hex + j * 3, sizeof(hex) - j * 3, "%02X ", data[header + j]);
            hex[count * 3] = 0;
            diagnostic_log("MVD", "first SPS bytes=%s", hex);
            g_first_sps_logged = true;
        }
        if (data[header + 1] == 100 && data[header + 3] == 50) {
            data[header + 3] = 32;
            if (++g_sps_level_rewrites <= 3)
                diagnostic_log("MVD", "720p SPS level rewritten 50->32 after reference-frame guard");
        }
        i = header + 3;
    }
}

static size_t start_code_length(const unsigned char *data, size_t size, size_t at)
{
    if (at + 3 <= size && data[at] == 0 && data[at + 1] == 0 && data[at + 2] == 1)
        return 3;
    if (at + 4 <= size && data[at] == 0 && data[at + 1] == 0 &&
        data[at + 2] == 0 && data[at + 3] == 1)
        return 4;
    return 0;
}

static Result process_720p_nals(const unsigned char *data, size_t size, bool *has_vcl)
{
    Result last = MVD_STATUS_OK;
    *has_vcl = false;
    for (size_t at = 0; at + 4 < size;) {
        size_t prefix = start_code_length(data, size, at);
        if (!prefix) { ++at; continue; }
        size_t header = at + prefix;
        size_t end = header + 1;
        while (end + 3 <= size && !start_code_length(data, size, end)) ++end;
        if (end + 3 > size) end = size;
        unsigned type = data[header] & 0x1f;
        size_t nal_size = end - header;
        at = end;
        if (type != 1 && type != 5 && type != 7 && type != 8) continue;
        if (nal_size + 3 > INPUT_CAPACITY) return (Result)-1;
        g_input[0] = 0; g_input[1] = 0; g_input[2] = 1;
        memcpy(g_input + 3, data + header, nal_size);
        if (type == 7) prepare_720p_sps(g_input, nal_size + 3);
        GSPGPU_FlushDataCache(g_input, nal_size + 3);
        bool trace = g_hd_nal_logs++ < 12;
        if (trace) {
            diagnostic_log("MVD", "NAL enter type=%u bytes=%lu", type,
                           (unsigned long)(nal_size + 3));
            diagnostic_checkpoint();
        }
        MVDSTD_ProcessNALUnitOut result = {0};
        last = mvdstdProcessVideoFrame(g_input, nal_size + 3, 0, &result);
        if (trace) {
            diagnostic_log("MVD", "NAL return type=%u rc=%08lX remaining=%lu",
                           type, (unsigned long)last, (unsigned long)result.remaining_size);
            diagnostic_checkpoint();
        }
        if (!MVD_CHECKNALUPROC_SUCCESS(last)) return last;
        if ((type == 1 || type == 5) &&
            last == MVD_STATUS_INCOMPLETEPROCESSING &&
            result.remaining_size >= nal_size + 3) {
            diagnostic_log("MVD", "720p slice not consumed type=%u bytes=%lu; stop before next service call",
                           type, (unsigned long)(nal_size + 3));
            diagnostic_checkpoint();
            return MVD_HD_NO_PROGRESS;
        }
        if (type == 1 || type == 5) *has_vcl = true;
    }
    return last;
}

/* Decode one access unit already in linear memory (`input`) and render it
 * into `output`. For 720p, `annex_b` is split into NAL units instead. */
static bool decode_access_unit(const unsigned char *annex_b, unsigned char *input,
                               size_t size, u16 *output)
{
    const bool hd = g_input_width > 960 || g_input_height > 544;
    if (!g_first_process_done) {
        diagnostic_log("MVD", "first-process-enter bytes=%lu input=%ux%u",
                       (unsigned long)size, g_input_width, g_input_height);
        diagnostic_checkpoint();
    }
    const u64 process_start = svcGetSystemTick();
    bool hd_has_vcl = false;
    Result rc = hd ? process_720p_nals(annex_b, size, &hd_has_vcl) :
                     mvdstdProcessVideoFrame(input, size, 1, NULL);
    const u64 process_ticks = svcGetSystemTick() - process_start;
    if (!MVD_CHECKNALUPROC_SUCCESS(rc)) {
        ++g_errors;
        g_process_failed = true;
        if (rc == MVD_HD_NO_PROGRESS)
            snprintf(g_status, sizeof(g_status), "720p MVD did not consume IDR; use 540p");
        else
            snprintf(g_status, sizeof(g_status), "MVD process %08lX AU %lu", (unsigned long)rc, (unsigned long)size);
        diagnostic_flag("decoder-fatal", "%s", g_status);
        diagnostic_log("MVD", "decoder submissions stopped after fatal process error");
        diagnostic_checkpoint();
        return false;
    }
    if (!g_first_process_done) {
        diagnostic_log("MVD", "first-process-return rc=%08lX", (unsigned long)rc);
        diagnostic_checkpoint();
        g_first_process_done = true;
    }
    if (rc == MVD_STATUS_OK) ++g_status_ok;
    else if (rc == MVD_STATUS_PARAMSET) ++g_status_paramset;
    else if (rc == MVD_STATUS_FRAMEREADY) ++g_status_ready;
    else if (rc == MVD_STATUS_INCOMPLETEPROCESSING) ++g_status_incomplete;
    if (g_frames < 3 && (g_status_ok + g_status_paramset + g_status_ready + g_status_incomplete) <= 12)
        diagnostic_log("MVD", "process status=%08lX AU=%lu counts ok=%u ps=%u ready=%u inc=%u",
                       (unsigned long)rc, (unsigned long)size, g_status_ok,
                       g_status_paramset, g_status_ready, g_status_incomplete);

    /* 0x17003 is explicitly MVD_STATUS_FRAMEREADY and must be presented.
     * Moonlight's older two-status check misses this result on GFN's stream. */
    if (hd ? !hd_has_vcl :
        (rc != MVD_STATUS_PARAMSET && rc != MVD_STATUS_FRAMEREADY &&
         rc != MVD_STATUS_INCOMPLETEPROCESSING))
        return true;
    const u64 render_start = svcGetSystemTick();
    if (!g_first_render_done) {
        diagnostic_log("MVD", "first-render-enter input=%ux%u",
                       g_input_width, g_input_height);
        diagnostic_checkpoint();
    }
    if (g_wide) {
        MVDSTD_Config config;
        LightLock_Lock(&g_config_lock);
        config = g_config;
        LightLock_Unlock(&g_config_lock);
        config.physaddr_outdata0 = osConvertVirtToPhys(output);
        rc = mvdstdRenderVideoFrame(&config, true);
    } else {
        rc = mvdstdRenderVideoFrame(&g_config, true);
    }
    if (!g_first_render_done) {
        diagnostic_log("MVD", "first-render-return rc=%08lX", (unsigned long)rc);
        diagnostic_checkpoint();
        g_first_render_done = true;
    }
    const u64 render_ticks = svcGetSystemTick() - render_start;
    if (rc != MVD_STATUS_OK) {
        ++g_errors;
        snprintf(g_status, sizeof(g_status), "MVD render %08lX", (unsigned long)rc);
        diagnostic_flag("decoder-render", "%s", g_status);
        return false;
    }
    /* The CPU only reads the output for the periodic log and for the
     * classic copy; wide frames go straight to the GPU, so skip the 1 MiB
     * cache invalidate and sampling on the frames that aren't logged. */
    const bool log_frame = g_frames < 5 || (g_frames + 1) % 120 == 0;
    if (!g_wide || log_frame)
        GSPGPU_InvalidateDataCache(output, g_output_stride * g_output_alloc_height * sizeof(u16));
    unsigned nonzero = 0;
    uint32_t sample_hash = 2166136261u;
    for (unsigned i = 0; (!g_wide || log_frame) && i < g_output_stride * g_output_height; i += 193) {
        nonzero += output[i] != 0;
        sample_hash = (sample_hash ^ output[i]) * 16777619u;
        /* Green carries most luma: track the darkest and brightest samples to
         * see whether MVD outputs limited-range (16-235) or full-range video. */
        const unsigned green = ((output[i] >> 5) & 63u) * 255u / 63u;
        if (green < g_level_min) g_level_min = green;
        if (green > g_level_max) g_level_max = green;
    }
    const u64 copy_start = svcGetSystemTick();
    if (g_frames == 0) {
        diagnostic_log("MVD", "first-copy-enter");
        diagnostic_checkpoint();
    }
    /* Wide frames are drawn by the GPU; the caller publishes them. */
    if (!g_wide) copy_to_top_framebuffer();
    if (g_frames == 0) {
        diagnostic_log("MVD", "first-copy-return");
        diagnostic_checkpoint();
    }
    const u64 copy_ticks = svcGetSystemTick() - copy_start;
    record_performance(process_ticks, render_ticks, copy_ticks);
    ++g_frames;
    snprintf(g_status, sizeof(g_status), "MVD decoded frame %u", g_frames);
    /* Brightness is sampled every frame; checked every 120 (~4 s). A
     * picture going black then the session ending is how a game quitting
     * looks (beta.27 report GTXMTY), so that is logged when it happens. */
    if (g_frames <= 5 || g_frames % 120 == 0) {
        const bool dark = g_level_max < 24;
        if (g_frames <= 5 || g_frames % 600 == 0 || dark != g_picture_dark) {
            diagnostic_log("MVD", "%s status=%08lX sampleNonzero=%u hash=%08lx",
                           g_status, (unsigned long)rc, nonzero, (unsigned long)sample_hash);
            diagnostic_log("MVD", "levels green min=%u max=%u output=%ux%u wide=%u%s",
                           g_level_min, g_level_max, g_output_width, g_output_height,
                           g_wide ? 1 : 0, g_frames <= 5 ? "" : dark ? " picture went dark" :
                           dark != g_picture_dark ? " picture back" : "");
        }
        if (g_frames > 5) g_picture_dark = dark;
        g_level_min = 255;
        g_level_max = 0;
    }
    return true;
}

static void decoder_main(void *arg)
{
    (void)arg;
    while (!g_decoder_quit) {
        LightLock_Lock(&g_queue_lock);
        const unsigned count = g_queue_count;
        const unsigned slot = g_queue_head;
        LightLock_Unlock(&g_queue_lock);
        if (!count) {
            LightEvent_Wait(&g_queue_event);
            continue;
        }
        /* Take a free output; if the pacer is behind, recycle the oldest
         * ready frame rather than stalling decode (references must flow). */
        LightLock_Lock(&g_frame_lock);
        int target = -1;
        for (unsigned i = 0; i < WIDE_OUTPUTS && target < 0; ++i)
            if (g_output_state[i] == OUT_FREE) target = (int)i;
        if (target < 0 && g_ready_count) {
            target = g_ready_fifo[0];
            memmove(g_ready_fifo, g_ready_fifo + 1, --g_ready_count * sizeof(int));
            ++g_recycled_frames;
        }
        if (target >= 0) g_output_state[target] = OUT_DECODING;
        LightLock_Unlock(&g_frame_lock);
        if (target < 0) target = 0; /* unreachable: 6 outputs, 1 presenting */
        const unsigned rendered_before = g_frames;
        unsigned char *au = g_ring + g_entry_offset[slot];
        if (!g_process_failed)
            decode_access_unit(au, au, g_entry_size[slot], g_wide_outputs[target]);
        LightLock_Lock(&g_frame_lock);
        if (g_frames != rendered_before) {
            g_output_state[target] = OUT_READY;
            g_output_bytes[target] = g_entry_size[slot];
            g_ready_fifo[g_ready_count++] = target;
        } else {
            g_output_state[target] = OUT_FREE;
        }
        LightLock_Unlock(&g_frame_lock);
        LightLock_Lock(&g_queue_lock);
        g_queue_head = (g_queue_head + 1) % AU_ENTRIES;
        --g_queue_count;
        LightLock_Unlock(&g_queue_lock);
    }
}

static bool contains_idr(const unsigned char *data, size_t size)
{
    for (size_t i = 0; i + 3 < size; ++i)
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1 && (data[i + 3] & 0x1f) == 5)
            return true;
    return false;
}

bool mvd_video_submit(const unsigned char *annex_b, size_t size)
{
    if (!g_active || !annex_b || size == 0 || g_process_failed) return false;
    const size_t capacity = g_wide ? AU_MAX : INPUT_CAPACITY;
    if (size > capacity) {
        ++g_errors;
        snprintf(g_status, sizeof(g_status), "MVD AU too large: %lu", (unsigned long)size);
        diagnostic_flag("au-too-large", "%s (capacity %lu)", g_status, (unsigned long)capacity);
        return false;
    }
    if (g_await_idr) {
        if (!contains_idr(annex_b, size)) return true; /* hold the last clean picture */
        g_await_idr = false;
        diagnostic_log("MVD", "resynchronised on IDR after dropped frames");
    }
    if (!g_wide) {
        const bool hd = g_input_width > 960 || g_input_height > 544;
        if (!hd) {
            memcpy(g_input, annex_b, size);
            GSPGPU_FlushDataCache(g_input, size);
        }
        return decode_access_unit(annex_b, g_input, size, g_output);
    }
    /* Room in the ring: after the newest AU, else from the start, never
     * reaching the oldest one still queued (the decoder is reading it). The
     * write position never lands exactly on the oldest, so "write == oldest"
     * can't be mistaken for an empty ring. */
    const size_t need = (size + 0x7F) & ~(size_t)0x7F;
    size_t start = (size_t)-1;
    LightLock_Lock(&g_queue_lock);
    if (g_queue_count == 0) {
        start = 0;
    } else if (g_queue_count < AU_ENTRIES) {
        const size_t oldest = g_entry_offset[g_queue_head];
        if (g_ring_write >= oldest) {
            if (g_ring_write + need <= AU_RING_SIZE) start = g_ring_write;
            else if (need < oldest) start = 0;
        } else if (g_ring_write + need < oldest) {
            start = g_ring_write;
        }
    }
    if (start == (size_t)-1) {
        LightLock_Unlock(&g_queue_lock);
        /* Dropping breaks the reference chain: freeze until a keyframe. */
        ++g_errors;
        g_resync_requested = true;
        g_await_idr = true;
        /* The first keyframe waits ~0.5 s for the decoder to set itself up,
         * so the queue fills once at every stream start: normal, not a bug
         * (beta.28 report HPSV3B, a clean session). It can spill just after
         * the first frames too (beta.34: frames 7 and 13), so only a full
         * queue in the first two seconds' worth of frames is left out. */
        if (g_frames > 60)
            diagnostic_flag("decoder-backlog", "decoder queue full; waiting for IDR (frame %u)", g_frames);
        else
            diagnostic_log("MVD", "queue full while the decoder starts; waiting for IDR");
        return false;
    }
    const unsigned slot = (g_queue_head + g_queue_count) % AU_ENTRIES;
    g_entry_offset[slot] = start;
    g_entry_size[slot] = size;
    g_ring_write = start + need;
    LightLock_Unlock(&g_queue_lock);
    memcpy(g_ring + start, annex_b, size);
    GSPGPU_FlushDataCache(g_ring + start, size);
    LightLock_Lock(&g_queue_lock);
    ++g_queue_count;
    LightLock_Unlock(&g_queue_lock);
    LightEvent_Signal(&g_queue_event);
    return true;
}

void mvd_video_close(void)
{
    /* Not while a frame is on its way to the GPU. */
    LightLock_Lock(&g_present_lock);
    if (g_decoder) {
        g_decoder_quit = true;
        LightEvent_Signal(&g_queue_event);
        threadJoin(g_decoder, U64_MAX);
        threadFree(g_decoder);
        g_decoder = NULL;
    }
    if (g_ring) linearFree(g_ring);
    g_ring = NULL;
    for (unsigned i = 1; i < WIDE_OUTPUTS; ++i) {
        if (g_wide_outputs[i]) linearFree(g_wide_outputs[i]);
        g_wide_outputs[i] = NULL;
    }
    if (g_recycled_frames)
        diagnostic_log("MVD", "decoder recycled %u unpresented frames", g_recycled_frames);
    g_wide_outputs[0] = NULL;
    g_wide = false;
    g_output_stride = 0;
    if (g_active) mvdstdExit();
    if (g_input) linearFree(g_input);
    if (g_output) linearFree(g_output);
    g_active = false; g_input = NULL; g_output = NULL;
    /* The next session counts from zero. Build 65 kept the old count, and
     * main.c waited for more frames than the previous session had decoded
     * before showing video: a second launch looked like it never started. */
    g_frames = 0;
    g_recycled_frames = 0;
    g_input_width = g_input_height = 0;
    g_output_width = g_output_height = g_output_alloc_height = 0;
    g_zoom_level = 0;
    g_zoom_center_x = g_zoom_center_y = 0;
    g_last_zoom_pan_at = 0;
    memset(&g_config, 0, sizeof(g_config));
    LightLock_Unlock(&g_present_lock);
}

bool mvd_video_active(void) { return g_active; }
unsigned mvd_video_decoded_frames(void) { return g_frames; }
unsigned mvd_video_pending_units(void) { return g_queue_count; }
unsigned mvd_video_frames_lost(void) { return g_frames_lost; }

void mvd_video_decode_totals(unsigned long long *sum_us, unsigned *count, unsigned *max_us, bool reset_max)
{
    *sum_us = g_decode_total_us;
    *count = g_decode_total_count;
    *max_us = g_decode_max_us;
    if (reset_max) g_decode_max_us = 0;
}
unsigned mvd_video_errors(void) { return g_errors; }
bool mvd_video_picture_dark(void) { return g_picture_dark; }
const char *mvd_video_status(void) { return g_status; }

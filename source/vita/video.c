#include "mvd_video.h"
#include "video_vita.h"
#include "platform.h"
#include "diagnostic.h"

#include <psp2/kernel/sysmem.h>
#include <psp2/sysmodule.h>
#include <psp2/videodec.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The Vita's hardware H.264 decoder (sceAvcdec) behind mvd_video.h. Encoded
 * frames queue up from the media thread; a decoder thread turns each into
 * RGBA straight inside one of four GPU textures, which render.c draws. The
 * textures are made once, at 720p, and kept for the app's life, so freeing
 * one never races the GPU. */

#define TEX_WIDTH 1280
#define TEX_HEIGHT 720
#define FRAMES 4
#define QUEUE 8
#define REF_FRAMES 5

static vita2d_texture *g_tex[FRAMES];
static PlatLock g_frame_lock = PLAT_LOCK_INIT;
/* The newest finished frame, the one render.c took last, and the one
 * before it (the GPU may still be drawing that). */
static int g_ready = -1, g_shown = -1, g_prev_shown = -1;
static bool g_ready_fresh;
static unsigned g_pic_w = 960, g_pic_h = 544;
static unsigned g_visible_w, g_visible_h;

typedef struct {
    unsigned char *data;
    size_t size;
} Unit;
static Unit g_queue[QUEUE];
static unsigned g_queue_head, g_queue_count;
static PlatLock g_queue_lock = PLAT_LOCK_INIT;
static PlatEvent g_queue_event;
static bool g_event_ready;

static SceAvcdecCtrl g_decoder;
static SceUID g_work_block = -1;
static bool g_library_ready, g_decoder_ready, g_module_loaded;
static unsigned g_width, g_height;
static PlatThread g_thread;
static volatile bool g_quit, g_active, g_failed, g_skip_to_idr, g_resync_request;
static volatile unsigned g_decoded, g_errors, g_lost;
static unsigned long long g_decode_sum_us;
static unsigned g_decode_count, g_decode_max_us;
static volatile bool g_dark;
static char g_status[96] = "idle";

/* Zoom: 0 off, then 1.5x, 2x and 3x, centred at a point (percent). */
static unsigned g_zoom, g_zoom_x = 50, g_zoom_y = 50;
static const float ZOOMS[4] = { 1.0f, 1.5f, 2.0f, 3.0f };

static unsigned align16(unsigned v) { return (v + 15u) & ~15u; }

/* An IDR slice or an SPS: decoding can start (again) here. */
static bool has_keyframe(const unsigned char *data, size_t size)
{
    for (size_t i = 0; i + 3 < size; ++i) {
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
            const unsigned type = data[i + 3] & 0x1F;
            if (type == 5 || type == 7) return true;
            i += 2;
        }
    }
    return false;
}

static void free_queue(void)
{
    while (g_queue_count) {
        free(g_queue[g_queue_head].data);
        g_queue[g_queue_head].data = NULL;
        g_queue_head = (g_queue_head + 1) % QUEUE;
        --g_queue_count;
    }
}

static bool make_textures(void)
{
    for (int i = 0; i < FRAMES; ++i) {
        if (g_tex[i]) continue;
        g_tex[i] = vita2d_create_empty_texture_format(TEX_WIDTH, TEX_HEIGHT, SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR);
        if (!g_tex[i]) return false;
        vita2d_texture_set_filters(g_tex[i], SCE_GXM_TEXTURE_FILTER_LINEAR, SCE_GXM_TEXTURE_FILTER_LINEAR);
        memset(vita2d_texture_get_datap(g_tex[i]), 0, vita2d_texture_get_stride(g_tex[i]) * TEX_HEIGHT);
    }
    return true;
}

bool video_vita_prepare(void) { return make_textures(); }

static int pick_target(void)
{
    for (int i = 0; i < FRAMES; ++i)
        if (i != g_ready && i != g_shown && i != g_prev_shown) return i;
    return -1;
}

static void check_brightness(int index)
{
    const unsigned *pixels = vita2d_texture_get_datap(g_tex[index]);
    const unsigned stride = vita2d_texture_get_stride(g_tex[index]) / 4;
    unsigned sum = 0;
    for (unsigned y = 1; y <= 8; ++y)
        for (unsigned x = 1; x <= 8; ++x) {
            const unsigned p = pixels[(g_pic_h * y / 9) * stride + g_pic_w * x / 9];
            sum += (p & 0xFF) + ((p >> 8) & 0xFF) * 2 + ((p >> 16) & 0xFF);
        }
    g_dark = sum / 64 / 4 < 12;
}

static void decode_unit(Unit *unit)
{
    plat_lock(&g_frame_lock);
    const int target = pick_target();
    plat_unlock(&g_frame_lock);
    if (target < 0) return;
    SceAvcdecAu au;
    memset(&au, 0, sizeof(au));
    au.pts.upper = au.pts.lower = au.dts.upper = au.dts.lower = 0xFFFFFFFFu;
    au.es.pBuf = unit->data;
    au.es.size = (uint32_t)unit->size;
    SceAvcdecPicture picture;
    memset(&picture, 0, sizeof(picture));
    picture.size = sizeof(picture);
    picture.frame.pixelType = SCE_AVCDEC_PIXELFORMAT_RGBA8888;
    picture.frame.framePitch = vita2d_texture_get_stride(g_tex[target]) / 4;
    picture.frame.frameWidth = g_width;
    picture.frame.frameHeight = g_height;
    picture.frame.pPicture[0] = vita2d_texture_get_datap(g_tex[target]);
    SceAvcdecPicture *pictures[1] = { &picture };
    SceAvcdecArrayPicture array;
    memset(&array, 0, sizeof(array));
    array.numOfElm = 1;
    array.pPicture = pictures;
    const u64 start = plat_ticks();
    const int rc = sceAvcdecDecode(&g_decoder, &au, &array);
    const unsigned us = (unsigned)(plat_ticks() - start);
    if (rc < 0) {
        ++g_errors;
        if (g_errors <= 5 || g_errors % 100 == 0) diagnostic_log("VIDEO", "decode error 0x%08X (%u)", (unsigned)rc, g_errors);
        snprintf(g_status, sizeof(g_status), "decode error 0x%08X", (unsigned)rc);
        g_skip_to_idr = true;
        g_resync_request = true;
        return;
    }
    g_decode_sum_us += us;
    ++g_decode_count;
    if (us > g_decode_max_us) g_decode_max_us = us;
    if (!array.numOfOutput) return;
    const unsigned w = g_visible_w ? g_visible_w : g_width, h = g_visible_h ? g_visible_h : g_height;
    plat_lock(&g_frame_lock);
    g_ready = target;
    g_ready_fresh = true;
    g_pic_w = w <= TEX_WIDTH ? w : TEX_WIDTH;
    g_pic_h = h <= TEX_HEIGHT ? h : TEX_HEIGHT;
    plat_unlock(&g_frame_lock);
    if (++g_decoded % 60 == 1) check_brightness(target);
}

static void decoder_main(void *arg)
{
    (void)arg;
    while (!g_quit) {
        plat_event_wait_timeout(&g_queue_event, 100000000LL);
        for (;;) {
            plat_lock(&g_queue_lock);
            if (!g_queue_count || g_quit) {
                plat_unlock(&g_queue_lock);
                break;
            }
            Unit unit = g_queue[g_queue_head];
            g_queue[g_queue_head].data = NULL;
            g_queue_head = (g_queue_head + 1) % QUEUE;
            --g_queue_count;
            plat_unlock(&g_queue_lock);
            if (g_skip_to_idr && !has_keyframe(unit.data, unit.size)) {
                free(unit.data);
                continue;
            }
            g_skip_to_idr = false;
            decode_unit(&unit);
            free(unit.data);
        }
    }
}

static void close_decoder(void)
{
    if (g_decoder_ready) sceAvcdecDeleteDecoder(&g_decoder);
    g_decoder_ready = false;
    if (g_work_block >= 0) sceKernelFreeMemBlock(g_work_block);
    g_work_block = -1;
    if (g_library_ready) sceVideodecTermLibrary(SCE_VIDEODEC_TYPE_HW_AVCDEC);
    g_library_ready = false;
}

bool mvd_video_init(unsigned input_width, unsigned input_height)
{
    const unsigned w = align16(input_width ? input_width : 960), h = align16(input_height ? input_height : 544);
    if (g_active && w == g_width && h == g_height) return true;
    if (g_active) mvd_video_close();
    if (w > TEX_WIDTH || h > align16(TEX_HEIGHT)) {
        snprintf(g_status, sizeof(g_status), "%ux%u is too big for the decoder", w, h);
        g_failed = true;
        return false;
    }
    if (!g_event_ready) {
        plat_event_init(&g_queue_event, false);
        g_event_ready = true;
    }
    if (!g_module_loaded) g_module_loaded = sceSysmoduleLoadModule(SCE_SYSMODULE_AVCDEC) >= 0;
    if (!make_textures()) {
        snprintf(g_status, sizeof(g_status), "no memory for video frames");
        g_failed = true;
        return false;
    }
    SceVideodecQueryInitInfoHwAvcdec init;
    memset(&init, 0, sizeof(init));
    init.size = sizeof(init);
    init.horizontal = w;
    init.vertical = h;
    init.numOfRefFrames = REF_FRAMES;
    init.numOfStreams = 1;
    int rc = sceVideodecInitLibrary(SCE_VIDEODEC_TYPE_HW_AVCDEC, &init);
    if (rc < 0) {
        snprintf(g_status, sizeof(g_status), "decoder library 0x%08X", (unsigned)rc);
        diagnostic_log("VIDEO", "%s", g_status);
        g_failed = true;
        return false;
    }
    g_library_ready = true;
    SceAvcdecQueryDecoderInfo query = { w, h, REF_FRAMES };
    SceAvcdecDecoderInfo info;
    memset(&info, 0, sizeof(info));
    rc = sceAvcdecQueryDecoderMemSize(SCE_VIDEODEC_TYPE_HW_AVCDEC, &query, &info);
    /* The work memory must be physically contiguous, in 1 MiB steps. */
    const SceSize size = (info.frameMemSize + 0xFFFFFu) & ~0xFFFFFu;
    if (rc >= 0) g_work_block = sceKernelAllocMemBlock("kasumi_avcdec", SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_NC_RW,
                                                        size, NULL);
    if (rc < 0 || g_work_block < 0) {
        snprintf(g_status, sizeof(g_status), "decoder memory 0x%08X", (unsigned)(rc < 0 ? rc : g_work_block));
        diagnostic_log("VIDEO", "%s (%u bytes)", g_status, (unsigned)size);
        close_decoder();
        g_failed = true;
        return false;
    }
    void *base = NULL;
    sceKernelGetMemBlockBase(g_work_block, &base);
    memset(&g_decoder, 0, sizeof(g_decoder));
    g_decoder.frameBuf.pBuf = base;
    g_decoder.frameBuf.size = info.frameMemSize;
    rc = sceAvcdecCreateDecoder(SCE_VIDEODEC_TYPE_HW_AVCDEC, &g_decoder, &query);
    if (rc < 0) {
        snprintf(g_status, sizeof(g_status), "decoder create 0x%08X", (unsigned)rc);
        diagnostic_log("VIDEO", "%s", g_status);
        close_decoder();
        g_failed = true;
        return false;
    }
    g_decoder_ready = true;
    g_width = w;
    g_height = h;
    g_pic_w = input_width;
    g_pic_h = input_height;
    g_failed = false;
    g_skip_to_idr = true;
    g_resync_request = false;
    g_errors = 0;
    g_quit = false;
    g_thread = plat_thread_create(decoder_main, NULL, 64 * 1024, plat_thread_priority() - 1, PLAT_CORE_DEFAULT, false);
    if (!g_thread) {
        close_decoder();
        g_failed = true;
        return false;
    }
    g_active = true;
    snprintf(g_status, sizeof(g_status), "avcdec %ux%u", w, h);
    diagnostic_log("VIDEO", "decoder ready %ux%u (work %u KiB)", w, h, (unsigned)(info.frameMemSize / 1024));
    return true;
}

void mvd_video_close(void)
{
    if (g_thread) {
        g_quit = true;
        plat_event_signal(&g_queue_event);
        plat_thread_join(g_thread);
        g_thread = NULL;
    }
    plat_lock(&g_queue_lock);
    free_queue();
    plat_unlock(&g_queue_lock);
    close_decoder();
    plat_lock(&g_frame_lock);
    g_ready = -1;
    g_ready_fresh = false;
    plat_unlock(&g_frame_lock);
    g_active = false;
    g_visible_w = g_visible_h = 0;
    g_zoom = 0;
    snprintf(g_status, sizeof(g_status), "closed");
}

bool mvd_video_submit(const unsigned char *annex_b, size_t size)
{
    if (!g_active || !annex_b || !size) return false;
    unsigned char *copy = malloc(size);
    if (!copy) return false;
    memcpy(copy, annex_b, size);
    plat_lock(&g_queue_lock);
    if (g_queue_count == QUEUE) {
        /* The decoder fell behind: start again at a keyframe. */
        g_lost += g_queue_count;
        free_queue();
        if (!has_keyframe(copy, size)) {
            g_skip_to_idr = true;
            g_resync_request = true;
        }
    }
    g_queue[(g_queue_head + g_queue_count) % QUEUE] = (Unit){ copy, size };
    ++g_queue_count;
    plat_unlock(&g_queue_lock);
    plat_event_signal(&g_queue_event);
    return true;
}

vita2d_texture *video_vita_take_frame(bool *fresh, unsigned *width, unsigned *height)
{
    plat_lock(&g_frame_lock);
    *fresh = false;
    if (g_ready >= 0 && g_ready_fresh) {
        if (g_ready != g_shown) {
            g_prev_shown = g_shown;
            g_shown = g_ready;
        }
        g_ready_fresh = false;
        *fresh = true;
    }
    const int shown = g_shown;
    *width = g_pic_w;
    *height = g_pic_h;
    plat_unlock(&g_frame_lock);
    return shown >= 0 ? g_tex[shown] : NULL;
}

bool video_vita_frame_pixels(const unsigned **pixels, unsigned *width, unsigned *height, unsigned *stride)
{
    plat_lock(&g_frame_lock);
    const int shown = g_shown;
    *width = g_pic_w;
    *height = g_pic_h;
    plat_unlock(&g_frame_lock);
    if (shown < 0) return false;
    *pixels = vita2d_texture_get_datap(g_tex[shown]);
    *stride = vita2d_texture_get_stride(g_tex[shown]) / 4;
    return true;
}

/* The 3DS's decode modes and memory sizes don't apply here. */
void mvd_video_set_hd_mode(int mode) { (void)mode; }
void mvd_video_set_big_work(bool big) { (void)big; }
bool mvd_video_failed(void) { return g_failed; }
bool mvd_video_hd(void) { return g_width > 960; }
bool mvd_video_wide(void) { return true; }

void mvd_video_wide_size(unsigned *width, unsigned *height)
{
    *width = g_pic_w;
    *height = g_pic_h;
}

void mvd_video_set_visible(unsigned width, unsigned height)
{
    g_visible_w = width;
    g_visible_h = height;
}

/* render.c takes frames through video_vita_take_frame instead. */
const void *mvd_video_take_gpu_frame(void) { return NULL; }
void mvd_video_release_gpu_frame(void) {}
unsigned mvd_video_ready_frames(void) { return g_ready >= 0 && g_ready_fresh ? 1 : 0; }
void mvd_video_skip_oldest_frame(void) {}
size_t mvd_video_ready_frame_bytes(unsigned index) { (void)index; return 0; }
unsigned mvd_video_ready_frame_age_us(unsigned index) { (void)index; return 0; }
void mvd_video_hd_skip_one(void) {}
bool mvd_video_hd_halving(void) { return false; }
void mvd_video_skip_ready_frame(unsigned index) { (void)index; }

void mvd_video_resync(void)
{
    g_skip_to_idr = true;
    ++g_lost;
}

bool mvd_video_take_resync_request(void)
{
    const bool requested = g_resync_request;
    g_resync_request = false;
    return requested;
}

unsigned mvd_video_drop_backlog(void)
{
    plat_lock(&g_queue_lock);
    const unsigned dropped = g_queue_count;
    free_queue();
    plat_unlock(&g_queue_lock);
    g_skip_to_idr = true;
    g_resync_request = true;
    return dropped;
}

bool mvd_video_toggle_zoom(void)
{
    g_zoom = (g_zoom + 1) % 4;
    return g_zoom != 0;
}

bool mvd_video_set_zoom(unsigned level, unsigned x_percent, unsigned y_percent)
{
    g_zoom = level < 4 ? level : 0;
    g_zoom_x = x_percent <= 100 ? x_percent : 50;
    g_zoom_y = y_percent <= 100 ? y_percent : 50;
    return g_zoom != 0;
}

/* A point on the lower view (320x240 in screens.c's units). */
bool mvd_video_pan_to_touch(unsigned touch_x, unsigned touch_y)
{
    if (!g_zoom) return false;
    g_zoom_x = touch_x * 100 / 320;
    g_zoom_y = touch_y * 100 / 240;
    if (g_zoom_x > 100) g_zoom_x = 100;
    if (g_zoom_y > 100) g_zoom_y = 100;
    return true;
}

bool mvd_video_pan_to(unsigned x_permille, unsigned y_permille)
{
    if (!g_zoom) return false;
    g_zoom_x = (x_permille > 1000 ? 1000 : x_permille) / 10;
    g_zoom_y = (y_permille > 1000 ? 1000 : y_permille) / 10;
    return true;
}

bool mvd_video_zoomed(void) { return g_zoom != 0; }

void mvd_video_view_crop(float *x, float *y, float *w, float *h)
{
    const float size = 1.0f / ZOOMS[g_zoom];
    float cx = (float)g_zoom_x / 100.0f - size / 2.0f, cy = (float)g_zoom_y / 100.0f - size / 2.0f;
    cx = cx < 0.0f ? 0.0f : cx > 1.0f - size ? 1.0f - size : cx;
    cy = cy < 0.0f ? 0.0f : cy > 1.0f - size ? 1.0f - size : cy;
    *x = cx;
    *y = cy;
    *w = *h = size;
}

unsigned mvd_video_zoom_level(void) { return g_zoom; }

void mvd_video_zoom_position(unsigned *x, unsigned *y)
{
    *x = g_zoom_x;
    *y = g_zoom_y;
}

bool mvd_video_active(void) { return g_active; }
unsigned mvd_video_decoded_frames(void) { return g_decoded; }
unsigned mvd_video_pending_units(void) { return g_queue_count; }
unsigned mvd_video_frames_lost(void) { return g_lost; }

void mvd_video_decode_totals(unsigned long long *sum_us, unsigned *count, unsigned *max_us, bool reset_max)
{
    *sum_us = g_decode_sum_us;
    *count = g_decode_count;
    *max_us = g_decode_max_us;
    if (reset_max) g_decode_max_us = 0;
}

unsigned mvd_video_errors(void) { return g_errors; }
bool mvd_video_picture_dark(void) { return g_dark; }
const char *mvd_video_status(void) { return g_status; }

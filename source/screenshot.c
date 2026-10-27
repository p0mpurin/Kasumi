#include "screenshot.h"
#include "platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <zlib.h>

#include "app_paths.h"
#include "diagnostic.h"

/* zlib is already linked for curl and is much faster than stb's deflate. */
static unsigned char *zlib_compress(unsigned char *data, int length, int *out_length, int quality)
{
    uLongf size = compressBound((uLong)length);
    unsigned char *out = malloc(size);
    if (!out) return NULL;
    if (compress2(out, &size, data, (uLong)length, quality > 6 ? 6 : quality) != Z_OK) {
        free(out);
        return NULL;
    }
    *out_length = (int)size;
    return out;
}

#define STBIW_ZLIB_COMPRESS zlib_compress
#define STBI_WRITE_NO_STDIO
#define STB_IMAGE_WRITE_IMPLEMENTATION
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#include "../vendor/stb/stb_image_write.h"
#pragma GCC diagnostic pop

#define SHOT_DIR APP_DATA_DIR "/screenshots"
#define SRC_W 800
#define SRC_H 480
#define SRC_STRIDE 1024
/* The decoder squeezes 960x540 into 800x480; 800x450 restores 16:9. */
#define OUT_W 800
#define OUT_H 450

static u16 *g_frame;
/* Or a picture already in 8-bit RGB (screenshot_capture_rgba). */
static unsigned char *g_rgb;
static int g_rgb_w, g_rgb_h;
static PlatThread g_thread;
static volatile int g_result;
static volatile bool g_busy;
static char g_name[64];
/* A layout check (screenshot_capture_ui): its own folder and name, and no
 * "saved" notice. */
static const char *g_dir = SHOT_DIR;
static const char *g_ui_name;
static bool g_quiet;

static void write_chunk(void *context, void *data, int size)
{
    fwrite(data, 1, (size_t)size, (FILE *)context);
}

static void writer_main(void *arg)
{
    (void)arg;
    int result = -1;
    unsigned char *rgb = g_rgb ? g_rgb : malloc(OUT_W * OUT_H * 3);
    const int out_w = g_rgb ? g_rgb_w : OUT_W, out_h = g_rgb ? g_rgb_h : OUT_H;
    if (rgb && g_frame) {
        /* Vertical resample 480 -> 450 with linear interpolation. */
        for (int y = 0; y < OUT_H; ++y) {
            const float sy = (y + 0.5f) * SRC_H / OUT_H - 0.5f;
            int y0 = (int)sy;
            if (y0 < 0) y0 = 0;
            const int y1 = y0 + 1 < SRC_H ? y0 + 1 : SRC_H - 1;
            const float t = sy - (float)y0;
            for (int x = 0; x < OUT_W; ++x) {
                const u16 a = g_frame[y0 * SRC_W + x], b = g_frame[y1 * SRC_W + x];
                const float ar = (float)((a >> 11) & 31) * 255 / 31, br = (float)((b >> 11) & 31) * 255 / 31;
                const float ag = (float)((a >> 5) & 63) * 255 / 63, bg = (float)((b >> 5) & 63) * 255 / 63;
                const float ab = (float)(a & 31) * 255 / 31, bb = (float)(b & 31) * 255 / 31;
                unsigned char *o = rgb + (y * OUT_W + x) * 3;
                o[0] = (unsigned char)(ar + (br - ar) * t + 0.5f);
                o[1] = (unsigned char)(ag + (bg - ag) * t + 0.5f);
                o[2] = (unsigned char)(ab + (bb - ab) * t + 0.5f);
            }
        }
    }
    if (rgb) {
        char path[128];
        snprintf(path, sizeof(path), "%s/%s", g_dir, g_name);
        /* Here, not on the UI thread: on a slow card any SD call there
         * froze the game 3-4 s (beta.36 export). */
        mkdir(g_dir, 0777);
        FILE *f = fopen(path, "wb");
        if (f) {
            if (stbi_write_png_to_func(write_chunk, f, out_w, out_h, 3, rgb, out_w * 3)) result = 1;
            fclose(f);
            if (result != 1) remove(path);
        }
        free(rgb);
    }
    free(g_frame);
    g_frame = NULL;
    g_rgb = NULL;
    diagnostic_log("APP", "screenshot %s %s", g_name, result == 1 ? "saved" : "failed");
    g_result = g_quiet ? 0 : result;
    g_busy = false;
}

static bool start_writer(void);

static void finish_previous(void)
{
    if (g_thread) {
        plat_thread_join(g_thread);
        g_thread = NULL;
    }
}

bool screenshot_capture(const void *surface)
{
    if (!surface || g_busy) return false;
    finish_previous();
    g_frame = malloc(SRC_W * SRC_H * sizeof(u16));
    if (!g_frame) return false;
    const u16 *src = surface;
    for (int y = 0; y < SRC_H; ++y)
        memcpy(g_frame + y * SRC_W, src + y * SRC_STRIDE, SRC_W * sizeof(u16));
    return start_writer();
}

bool screenshot_capture_rgba(const u32 *pixels, unsigned width, unsigned height, unsigned stride)
{
    if (!pixels || !width || !height || g_busy) return false;
    finish_previous();
    g_rgb = malloc((size_t)width * height * 3);
    if (!g_rgb) return false;
    for (unsigned y = 0; y < height; ++y)
        for (unsigned x = 0; x < width; ++x) {
            const u32 p = pixels[y * stride + x];
            unsigned char *o = g_rgb + ((size_t)y * width + x) * 3;
            o[0] = (unsigned char)(p & 0xFF);
            o[1] = (unsigned char)((p >> 8) & 0xFF);
            o[2] = (unsigned char)((p >> 16) & 0xFF);
        }
    g_rgb_w = (int)width;
    g_rgb_h = (int)height;
    return start_writer();
}

bool screenshot_capture_ui(const u32 *pixels, unsigned width, unsigned height, unsigned stride, const char *name)
{
    if (g_busy) return false;
    finish_previous();
    g_ui_name = name;
    const bool started = screenshot_capture_rgba(pixels, width, height, stride);
    g_ui_name = NULL;
    return started;
}

static bool start_writer(void)
{
    g_quiet = g_ui_name != NULL;
    g_dir = g_quiet ? APP_DATA_DIR "/ui-shots" : SHOT_DIR;
    const time_t now = time(NULL);
    const struct tm *t = gmtime(&now);
    if (g_ui_name) snprintf(g_name, sizeof(g_name), "%s", g_ui_name);
    else if (t) strftime(g_name, sizeof(g_name), "kasumi_%Y%m%d_%H%M%S.png", t);
    else snprintf(g_name, sizeof(g_name), "kasumi_%llu.png", (unsigned long long)plat_time_ms());
    g_busy = true;
    g_result = 0;
    const int priority = plat_thread_priority();
    g_thread = plat_thread_create(writer_main, NULL, 16 * 1024,
                                  priority + 3 > PLAT_PRIO_LOWEST ? PLAT_PRIO_LOWEST : priority + 3,
                                  PLAT_CORE_DEFAULT, false);
    if (!g_thread) {
        free(g_frame);
        free(g_rgb);
        g_frame = NULL;
        g_rgb = NULL;
        g_busy = false;
        return false;
    }
    return true;
}

int screenshot_poll(char *name, unsigned size)
{
    const int result = g_result;
    if (!result || g_busy) return 0;
    g_result = 0;
    if (name && size) snprintf(name, size, "%s", g_name);
    return result;
}

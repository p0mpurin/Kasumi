#include "gallery.h"
#include "platform.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_paths.h"
#include "diagnostic.h"
#include "ui.h"

#define SHOT_DIR APP_DATA_DIR "/screenshots"
#define MAX_SHOTS 256
/* Pictures are shrunk to fit this (screenshots are 800x450: exactly half). */
#define PIC_W 400
#define PIC_H 225
#define TEX_W 512
#define TEX_H 256

/* stb_image, compiled in game_art.c. */
extern unsigned char *stbi_load_from_memory(const unsigned char *buffer, int len, int *x, int *y, int *comp,
                                            int req_comp);
extern void stbi_image_free(void *data);

static char g_names[MAX_SHOTS][40];
static unsigned g_count, g_index;
static unsigned g_saved_count;
static bool g_saved_dirty = true;

/* Decoding happens on a thread; the UI uploads the result. */
static PlatThread g_thread;
static PlatLock g_lock;
static PlatEvent g_wake;
static volatile bool g_quit, g_open;
static int g_want = -1;           /* index asked for (under g_lock) */
static int g_decoded = -1;        /* index in g_pixels, waiting for upload */
static int g_shown = -1;          /* index in the texture */
static u32 *g_pixels;             /* TEX_W x TEX_H 0xRRGGBBAA */
static unsigned g_pic_w, g_pic_h; /* the picture inside g_pixels */
/* Kept once made: the GPU may still be drawing it when the viewer closes. */
static UiPicture *g_picture;

static int compare_desc(const void *a, const void *b) { return strcmp((const char *)b, (const char *)a); }

static unsigned scan(void)
{
    unsigned n = 0;
    DIR *dir = opendir(SHOT_DIR);
    if (!dir) return 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) && n < MAX_SHOTS) {
        const size_t len = strlen(entry->d_name);
        if (len < 5 || len >= sizeof(g_names[0]) || strcasecmp(entry->d_name + len - 4, ".png")) continue;
        snprintf(g_names[n++], sizeof(g_names[0]), "%s", entry->d_name);
    }
    closedir(dir);
    /* kasumi_YYYYMMDD_HHMMSS.png: by name is by time. */
    qsort(g_names, n, sizeof(g_names[0]), compare_desc);
    return n;
}

unsigned gallery_saved_count(void)
{
    if (g_saved_dirty && !g_open) {
        g_saved_count = scan();
        g_count = g_saved_count;
        g_saved_dirty = false;
    }
    return g_open ? g_count : g_saved_count;
}

void gallery_mark_dirty(void) { g_saved_dirty = true; }

/* Fit the picture in PIC_W x PIC_H, each output pixel the mean of the
 * source pixels under it. */
static void shrink(const unsigned char *rgb, int w, int h)
{
    float scale = (float)PIC_W / w;
    if ((float)PIC_H / h < scale) scale = (float)PIC_H / h;
    if (scale > 1.0f) scale = 1.0f;
    const unsigned ow = (unsigned)(w * scale), oh = (unsigned)(h * scale);
    memset(g_pixels, 0, TEX_W * TEX_H * sizeof(u32));
    for (unsigned dy = 0; dy < oh; ++dy) {
        int sy0 = (int)(dy / scale), sy1 = (int)((dy + 1) / scale);
        if (sy1 <= sy0) sy1 = sy0 + 1;
        if (sy1 > h) sy1 = h;
        for (unsigned dx = 0; dx < ow; ++dx) {
            int sx0 = (int)(dx / scale), sx1 = (int)((dx + 1) / scale);
            if (sx1 <= sx0) sx1 = sx0 + 1;
            if (sx1 > w) sx1 = w;
            unsigned sum[3] = { 0, 0, 0 }, n = 0;
            for (int sy = sy0; sy < sy1; ++sy)
                for (int sx = sx0; sx < sx1; ++sx, ++n)
                    for (int c = 0; c < 3; ++c) sum[c] += rgb[((size_t)sy * w + sx) * 3 + c];
            if (!n) n = 1;
            g_pixels[dy * TEX_W + dx] = (sum[0] / n) << 24 | (sum[1] / n) << 16 | (sum[2] / n) << 8 | 0xFF;
        }
    }
    g_pic_w = ow;
    g_pic_h = oh;
}

static bool decode(int index)
{
    char path[96];
    snprintf(path, sizeof(path), "%s/%s", SHOT_DIR, g_names[index]);
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *data = size > 0 && size < 8 * 1024 * 1024 ? malloc((size_t)size) : NULL;
    const bool read = data && fread(data, 1, (size_t)size, f) == (size_t)size;
    fclose(f);
    int w = 0, h = 0, comp = 0;
    unsigned char *rgb = read ? stbi_load_from_memory(data, (int)size, &w, &h, &comp, 3) : NULL;
    free(data);
    if (!rgb || w <= 0 || h <= 0) {
        if (rgb) stbi_image_free(rgb);
        diagnostic_log("GALLERY", "could not read %.40s", g_names[index]);
        return false;
    }
    shrink(rgb, w, h);
    stbi_image_free(rgb);
    return true;
}

static void worker(void *arg)
{
    (void)arg;
    while (!g_quit) {
        plat_event_wait(&g_wake);
        if (g_quit) break;
        plat_lock(&g_lock);
        const int want = g_want;
        const bool busy = g_decoded >= 0; /* the UI hasn't taken the last one */
        plat_unlock(&g_lock);
        if (want < 0 || busy || want == g_shown) continue;
        const bool ok = decode(want);
        plat_lock(&g_lock);
        if (ok && g_want == want) g_decoded = want;
        plat_unlock(&g_lock);
        /* Asked for another meanwhile: go again. */
        if (g_want != want) plat_event_signal(&g_wake);
    }
}

static void request(int index)
{
    plat_lock(&g_lock);
    g_want = index;
    plat_unlock(&g_lock);
    plat_event_signal(&g_wake);
}

bool gallery_open(void)
{
    if (g_open) return true;
    g_count = scan();
    g_saved_count = g_count;
    g_saved_dirty = false;
    if (!g_count) return false;
    if (!g_pixels) g_pixels = malloc(TEX_W * TEX_H * sizeof(u32));
    if (!g_pixels) return false;
    plat_lock_init(&g_lock);
    plat_event_init(&g_wake, false);
    g_quit = false;
    g_want = g_decoded = g_shown = -1;
    g_index = 0;
    g_thread = plat_thread_create(worker, NULL, 32 * 1024, 0x3A, PLAT_CORE_ANY, false);
    if (!g_thread) return false;
    g_open = true;
    request(0);
    diagnostic_log("GALLERY", "opened with %u screenshots", g_count);
    return true;
}

void gallery_close(void)
{
    if (!g_open) return;
    g_quit = true;
    plat_event_signal(&g_wake);
    plat_thread_join(g_thread);
    g_thread = NULL;
    g_open = false;
    /* The texture stays until the next open (the GPU may still be drawing
     * it this frame); the pixels buffer is reused. */
    g_shown = -1;
}

unsigned gallery_count(void) { return g_count; }
unsigned gallery_index(void) { return g_index; }

void gallery_step(int direction)
{
    if (!g_open || g_count < 2) return;
    g_index = (g_index + g_count + (unsigned)direction) % g_count;
    request((int)g_index);
}

bool gallery_delete_current(void)
{
    if (!g_open || !g_count) return false;
    char path[96];
    snprintf(path, sizeof(path), "%s/%s", SHOT_DIR, g_names[g_index]);
    if (remove(path) != 0) return false;
    diagnostic_log("GALLERY", "deleted %.40s", g_names[g_index]);
    memmove(g_names[g_index], g_names[g_index + 1], (g_count - g_index - 1) * sizeof(g_names[0]));
    --g_count;
    g_saved_count = g_count;
    if (g_index >= g_count && g_index) --g_index;
    plat_lock(&g_lock);
    g_decoded = -1;
    plat_unlock(&g_lock);
    g_shown = -1;
    if (g_count) request((int)g_index);
    return true;
}

const char *gallery_caption(void)
{
    static char text[48];
    static const char *const months[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                          "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    if (!g_count) return "";
    unsigned y, mo, d, h, mi;
    if (sscanf(g_names[g_index], "kasumi_%4u%2u%2u_%2u%2u", &y, &mo, &d, &h, &mi) == 5 && mo >= 1 && mo <= 12)
        snprintf(text, sizeof(text), "%u %s %u, %02u:%02u", d, months[mo - 1], y, h, mi);
    else
        snprintf(text, sizeof(text), "%.40s", g_names[g_index]);
    return text;
}

void gallery_pump(void)
{
    if (!g_open) return;
    plat_lock(&g_lock);
    const int decoded = g_decoded;
    plat_unlock(&g_lock);
    if (decoded < 0) return;
    if (!g_picture) g_picture = ui_picture_create(PIC_W, PIC_H);
    if (g_picture && ui_picture_set(g_picture, g_pixels, g_pic_w, g_pic_h, TEX_W)) g_shown = decoded;
    plat_lock(&g_lock);
    g_decoded = -1;
    plat_unlock(&g_lock);
    /* The thread may be waiting to decode the next one. */
    plat_event_signal(&g_wake);
}

bool gallery_draw(float x, float y, float w, float h)
{
    if (!g_open || !g_picture || g_shown != (int)g_index || !g_pic_w || !g_pic_h) return false;
    float scale = w / g_pic_w;
    if (h / g_pic_h < scale) scale = h / g_pic_h;
    const float dw = g_pic_w * scale, dh = g_pic_h * scale;
    return ui_picture_draw(g_picture, x + (w - dw) / 2.0f, y + (h - dh) / 2.0f, scale, 1.0f);
}

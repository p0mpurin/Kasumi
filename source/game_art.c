#include "game_art.h"

#include <3ds.h>
#include <citro2d.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>

#include "app_paths.h"
#include "diagnostic.h"
#include "http_client.h"

#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STB_IMAGE_IMPLEMENTATION
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#include "../vendor/stb/stb_image.h"
#pragma GCC diagnostic pop

#define ART_DIR APP_DATA_DIR "/art"
#define TEX_SIZE 128
#define ART_SLOTS 16
#define ART_PIXELS (GAME_ART_WIDTH * GAME_ART_HEIGHT)
/* Stored as 24-bit RGB on the card, RGBA8 in memory (no 16-bit banding). */
#define ART_FILE_BYTES (ART_PIXELS * 3)
/* Bumped when older cached covers must not be trusted: v2 (beta.11) drops
 * covers that may have been saved from a cut-off download. */
#define ART_CACHE_VERSION 2
#define ART_VERSION_PATH ART_DIR "/version"
/* A cover that failed is tried again after this long. */
#define ART_RETRY_MS 8000

typedef enum { ART_FREE, ART_WANTED, ART_LOADING, ART_DECODED, ART_READY, ART_FAILED } ArtState;

typedef struct {
    ArtState state;
    char key[48];
    char url[192];
    /* Decoded by the worker (malloc), uploaded and freed by the UI thread. */
    u32 *pixels;
    C3D_Tex tex;
    Tex3DS_SubTexture subtex;
    u64 used_at;
    u64 failed_at;
} ArtSlot;

static ArtSlot g_slots[ART_SLOTS];
static LightLock g_lock = 1;

/* Background download list: covers go to the SD cache only. */
#define PREFETCH_MAX 256
static struct { char key[48]; char url[192]; } g_prefetch[PREFETCH_MAX];
static unsigned g_prefetch_count, g_prefetch_next;

/* Clear the SD cache once when ART_CACHE_VERSION changes. */
static void check_cache_version(void)
{
    FILE *f = fopen(ART_VERSION_PATH, "r");
    int version = 0;
    if (f) {
        if (fscanf(f, "%d", &version) != 1) version = 0;
        fclose(f);
    }
    if (version == ART_CACHE_VERSION) return;
    unsigned removed = 0;
    DIR *dir = opendir(ART_DIR);
    if (dir) {
        struct dirent *entry;
        char path[160];
        while ((entry = readdir(dir))) {
            const size_t n = strlen(entry->d_name);
            if (n < 5 || strcmp(entry->d_name + n - 4, ".rgb")) continue;
            snprintf(path, sizeof(path), "%s/%s", ART_DIR, entry->d_name);
            if (unlink(path) == 0) ++removed;
        }
        closedir(dir);
    }
    f = fopen(ART_VERSION_PATH, "w");
    if (f) {
        fprintf(f, "%d\n", ART_CACHE_VERSION);
        fclose(f);
    }
    diagnostic_log("ART", "cache v%d -> v%d: removed %u covers", version, ART_CACHE_VERSION, removed);
}

void game_art_init(void)
{
    LightLock_Init(&g_lock);
    memset(g_slots, 0, sizeof(g_slots));
    mkdir(ART_DIR, 0777);
    check_cache_version();
}

void game_art_exit(void)
{
    LightLock_Lock(&g_lock);
    for (int i = 0; i < ART_SLOTS; ++i) {
        if (g_slots[i].state == ART_READY) C3D_TexDelete(&g_slots[i].tex);
        free(g_slots[i].pixels);
        g_slots[i].pixels = NULL;
        g_slots[i].state = ART_FREE;
    }
    LightLock_Unlock(&g_lock);
}

static ArtSlot *find(const char *key)
{
    for (int i = 0; i < ART_SLOTS; ++i)
        if (g_slots[i].state != ART_FREE && !strcmp(g_slots[i].key, key)) return &g_slots[i];
    return NULL;
}

void game_art_want(const GfnGame *game)
{
    if (!game || !game->app_id[0] || !game->image_url[0]) return;
    LightLock_Lock(&g_lock);
    ArtSlot *slot = find(game->app_id);
    if (!slot) {
        /* Reuse a free slot, else the least recently shown finished one. */
        for (int i = 0; i < ART_SLOTS && !slot; ++i)
            if (g_slots[i].state == ART_FREE) slot = &g_slots[i];
        for (int i = 0; i < ART_SLOTS; ++i) {
            ArtSlot *s = &g_slots[i];
            if (slot && slot->state == ART_FREE) break;
            if ((s->state == ART_READY || s->state == ART_FAILED) &&
                (!slot || s->used_at < slot->used_at)) slot = s;
        }
        if (slot) {
            if (slot->state == ART_READY) C3D_TexDelete(&slot->tex);
            memset(slot, 0, sizeof(*slot));
            slot->state = ART_WANTED;
            snprintf(slot->key, sizeof(slot->key), "%s", game->app_id);
            snprintf(slot->url, sizeof(slot->url), "%s", game->image_url);
        }
    }
    if (slot) {
        slot->used_at = osGetTime();
        /* A failed cover (network hiccup) gets another go after a while. */
        if (slot->state == ART_FAILED && slot->used_at - slot->failed_at >= ART_RETRY_MS)
            slot->state = ART_WANTED;
    }
    LightLock_Unlock(&g_lock);
}

void game_art_pump(void)
{
    for (int i = 0; i < ART_SLOTS; ++i) {
        ArtSlot *slot = &g_slots[i];
        LightLock_Lock(&g_lock);
        u32 *pixels = slot->state == ART_DECODED ? slot->pixels : NULL;
        LightLock_Unlock(&g_lock);
        if (!pixels) continue;
        /* Tile it the same way as video frames: one display transfer from a
         * linear 128x128 surface; row 0 lands at the top (v = 1). */
        u32 *linear = linearAlloc(TEX_SIZE * TEX_SIZE * sizeof(u32));
        bool ok = linear && C3D_TexInit(&slot->tex, TEX_SIZE, TEX_SIZE, GPU_RGBA8);
        if (ok) {
            /* The transfer writes the texture behind the CPU's back. After a
             * stream this memory held video buffers the CPU wrote to; any of
             * those lines still dirty in the data cache would later be
             * written back over the cover (blocks of it missing). Flush
             * them out first. */
            GSPGPU_FlushDataCache(slot->tex.data, slot->tex.size);
            memset(linear, 0, TEX_SIZE * TEX_SIZE * sizeof(u32));
            for (int y = 0; y < GAME_ART_HEIGHT; ++y)
                memcpy(linear + y * TEX_SIZE, pixels + y * GAME_ART_WIDTH, GAME_ART_WIDTH * sizeof(u32));
            GSPGPU_FlushDataCache(linear, TEX_SIZE * TEX_SIZE * sizeof(u32));
            C3D_SyncDisplayTransfer((u32 *)linear, GX_BUFFER_DIM(TEX_SIZE, TEX_SIZE),
                                    (u32 *)slot->tex.data, GX_BUFFER_DIM(TEX_SIZE, TEX_SIZE),
                                    GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(1) |
                                    GX_TRANSFER_RAW_COPY(0) |
                                    GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) |
                                    GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGBA8) |
                                    GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO));
            C3D_TexSetFilter(&slot->tex, GPU_LINEAR, GPU_LINEAR);
            C3D_TexSetWrap(&slot->tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
            slot->subtex.width = GAME_ART_WIDTH;
            slot->subtex.height = GAME_ART_HEIGHT;
            slot->subtex.left = 0.0f;
            slot->subtex.top = 1.0f;
            slot->subtex.right = (float)GAME_ART_WIDTH / TEX_SIZE;
            slot->subtex.bottom = 1.0f - (float)GAME_ART_HEIGHT / TEX_SIZE;
        }
        if (linear) linearFree(linear);
        LightLock_Lock(&g_lock);
        free(slot->pixels);
        slot->pixels = NULL;
        slot->state = ok ? ART_READY : ART_FAILED;
        if (!ok) slot->failed_at = osGetTime();
        LightLock_Unlock(&g_lock);
    }
}

bool game_art_draw(const GfnGame *game, float x, float y, float scale, float alpha)
{
    if (!game || !game->app_id[0]) return false;
    ArtSlot *slot = find(game->app_id);
    if (!slot || slot->state != ART_READY) return false;
    slot->used_at = osGetTime();
    const C2D_Image image = { &slot->tex, &slot->subtex };
    C2D_ImageTint tint;
    C2D_AlphaImageTint(&tint, alpha);
    return C2D_DrawImageAt(image, floorf(x + 0.5f), floorf(y + 0.5f), 0.0f,
                           alpha < 1.0f ? &tint : NULL, scale, scale);
}

/* ---- Worker side --------------------------------------------------------- */

static void cache_path(char *out, size_t size, const char *key)
{
    char safe[48];
    size_t n = 0;
    for (const char *p = key; *p && n < sizeof(safe) - 1; ++p)
        safe[n++] = ((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'z') ||
                     (*p >= 'A' && *p <= 'Z') || *p == '-') ? *p : '_';
    safe[n] = '\0';
    snprintf(out, size, "%s/%s.rgb", ART_DIR, safe);
}

/* Texture byte order: 0xRRGGBBAA as a little-endian word. */
static inline u32 rgba(unsigned r, unsigned g, unsigned b)
{
    return (r << 24) | (g << 16) | (b << 8) | 0xFFu;
}

static void unpack_rgb(const unsigned char *rgb, u32 *out)
{
    for (int i = 0; i < ART_PIXELS; ++i, rgb += 3) out[i] = rgba(rgb[0], rgb[1], rgb[2]);
}

/* Area-average "cover" resize: fill 96x128, cropping the longer side.
 * Writes packed 24-bit RGB (the SD format). */
static void cover_resize(const unsigned char *rgb, int w, int h, unsigned char *out)
{
    const float scale_x = (float)GAME_ART_WIDTH / (float)w, scale_y = (float)GAME_ART_HEIGHT / (float)h;
    const float scale = scale_x > scale_y ? scale_x : scale_y;
    const float crop_w = GAME_ART_WIDTH / scale, crop_h = GAME_ART_HEIGHT / scale;
    const float x0 = ((float)w - crop_w) / 2.0f, y0 = ((float)h - crop_h) / 2.0f;
    for (int dy = 0; dy < GAME_ART_HEIGHT; ++dy) {
        int sy0 = (int)(y0 + dy / scale), sy1 = (int)(y0 + (dy + 1) / scale);
        if (sy1 <= sy0) sy1 = sy0 + 1;
        if (sy1 > h) sy1 = h;
        if (sy0 >= h) sy0 = h - 1;
        for (int dx = 0; dx < GAME_ART_WIDTH; ++dx) {
            int sx0 = (int)(x0 + dx / scale), sx1 = (int)(x0 + (dx + 1) / scale);
            if (sx1 <= sx0) sx1 = sx0 + 1;
            if (sx1 > w) sx1 = w;
            if (sx0 >= w) sx0 = w - 1;
            unsigned r = 0, g = 0, b = 0, count = 0;
            for (int sy = sy0; sy < sy1; ++sy) {
                const unsigned char *p = rgb + ((size_t)sy * w + sx0) * 3;
                for (int sx = sx0; sx < sx1; ++sx, p += 3) {
                    r += p[0]; g += p[1]; b += p[2]; ++count;
                }
            }
            if (!count) count = 1;
            unsigned char *o = out + (dy * GAME_ART_WIDTH + dx) * 3;
            o[0] = (unsigned char)(r / count);
            o[1] = (unsigned char)(g / count);
            o[2] = (unsigned char)(b / count);
        }
    }
}

static bool on_disk(const char *key)
{
    char path[128];
    cache_path(path, sizeof(path), key);
    struct stat st;
    return stat(path, &st) == 0 && st.st_size == ART_FILE_BYTES;
}

static unsigned char *load_from_disk(const char *key)
{
    char path[128];
    cache_path(path, sizeof(path), key);
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    unsigned char *rgb = malloc(ART_FILE_BYTES);
    const bool ok = rgb && fread(rgb, 1, ART_FILE_BYTES, f) == ART_FILE_BYTES;
    fclose(f);
    if (!ok) { free(rgb); return NULL; }
    return rgb;
}

static void save_to_disk(const char *key, const unsigned char *rgb)
{
    char path[128];
    cache_path(path, sizeof(path), key);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fwrite(rgb, 1, ART_FILE_BYTES, f);
    fclose(f);
}

/* The file ends where it should: JPEG end-of-image marker or PNG IEND. */
static bool image_complete(const unsigned char *data, size_t size)
{
    if (size >= 4 && data[0] == 0xFF && data[1] == 0xD8) {
        const size_t from = size > 32 ? size - 32 : 2;
        for (size_t i = size - 2; i + 1 > from; --i)
            if (data[i] == 0xFF && data[i + 1] == 0xD9) return true;
        return false;
    }
    if (size >= 12 && data[0] == 0x89 && data[1] == 'P')
        return memcmp(data + size - 8, "IEND", 4) == 0;
    return true;
}

static unsigned char *download(const char *url)
{
    /* img.nvidiagrid.net resizes on request; three times the display width
     * gives the area-average downscale plenty of detail to work with. */
    char sized[256];
    if (strstr(url, "img.nvidiagrid.net"))
        snprintf(sized, sizeof(sized), "%s;f=jpg;w=%d", url, GAME_ART_WIDTH * 3);
    else
        snprintf(sized, sizeof(sized), "%s", url);
    static const char *const headers[] = { "Accept: image/jpeg,image/png,*/*" };
    HttpResponse response;
    if (!http_request("GET", sized, "Kasumi-3DS", headers, 1, NULL, 1024 * 1024, &response)) {
        diagnostic_log("ART", "download failed: %s", response.error);
        return NULL;
    }
    unsigned char *pixels = NULL;
    if (response.status == 200 && response.body && response.size && !image_complete(
            (const unsigned char *)response.body, response.size)) {
        /* stb_image decodes a cut-off JPEG without complaint, grey where the
         * data stopped; such a cover must not reach the SD cache. */
        diagnostic_log("ART", "incomplete image bytes=%lu", (unsigned long)response.size);
    } else if (response.status == 200 && response.body && response.size) {
        int w = 0, h = 0, comp = 0;
        unsigned char *rgb = stbi_load_from_memory((const unsigned char *)response.body,
                                                   (int)response.size, &w, &h, &comp, 3);
        if (rgb && w > 0 && h > 0 && w <= 4096 && h <= 4096) {
            pixels = malloc(ART_FILE_BYTES);
            if (pixels) cover_resize(rgb, w, h, pixels);
        } else {
            diagnostic_log("ART", "decode failed bytes=%lu", (unsigned long)response.size);
        }
        stbi_image_free(rgb);
    } else {
        diagnostic_log("ART", "HTTP %ld", response.status);
    }
    http_response_free(&response);
    return pixels;
}

void game_art_prefetch(const GfnGame *games, unsigned count)
{
    LightLock_Lock(&g_lock);
    g_prefetch_count = g_prefetch_next = 0;
    for (unsigned i = 0; i < count && g_prefetch_count < PREFETCH_MAX; ++i) {
        if (!games[i].app_id[0] || !games[i].image_url[0]) continue;
        snprintf(g_prefetch[g_prefetch_count].key, sizeof(g_prefetch[0].key), "%s", games[i].app_id);
        snprintf(g_prefetch[g_prefetch_count].url, sizeof(g_prefetch[0].url), "%s", games[i].image_url);
        ++g_prefetch_count;
    }
    LightLock_Unlock(&g_lock);
}

/* One background download for the SD cache; false when there is nothing to do. */
static bool prefetch_one(void)
{
    char key[48], url[192];
    for (;;) {
        LightLock_Lock(&g_lock);
        const bool any = g_prefetch_next < g_prefetch_count;
        if (any) {
            memcpy(key, g_prefetch[g_prefetch_next].key, sizeof(key));
            memcpy(url, g_prefetch[g_prefetch_next].url, sizeof(url));
            ++g_prefetch_next;
        }
        LightLock_Unlock(&g_lock);
        if (!any) return false;
        if (on_disk(key)) continue;
        unsigned char *rgb = download(url);
        if (rgb) save_to_disk(key, rgb);
        free(rgb);
        return true;
    }
}

bool game_art_work(void)
{
    char key[48], url[192];
    ArtSlot *slot = NULL;
    LightLock_Lock(&g_lock);
    for (int i = 0; i < ART_SLOTS && !slot; ++i)
        if (g_slots[i].state == ART_WANTED) slot = &g_slots[i];
    if (slot) {
        slot->state = ART_LOADING;
        memcpy(key, slot->key, sizeof(key));
        memcpy(url, slot->url, sizeof(url));
    }
    LightLock_Unlock(&g_lock);
    /* On-screen art first; otherwise keep filling the SD cache. */
    if (!slot) return prefetch_one();

    unsigned char *rgb = load_from_disk(key);
    if (!rgb) {
        rgb = download(url);
        if (rgb) save_to_disk(key, rgb);
    }
    u32 *pixels = rgb ? malloc(ART_PIXELS * sizeof(u32)) : NULL;
    if (pixels) unpack_rgb(rgb, pixels);
    free(rgb);
    LightLock_Lock(&g_lock);
    if (slot->state == ART_LOADING && !strcmp(slot->key, key)) {
        slot->pixels = pixels;
        slot->state = pixels ? ART_DECODED : ART_FAILED;
        if (!pixels) slot->failed_at = osGetTime();
        pixels = NULL;
    }
    LightLock_Unlock(&g_lock);
    free(pixels);
    return true;
}

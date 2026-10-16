#include "shortcut.h"
#include "i18n.h"

#include <3ds.h>
#include <citro2d.h>
#include <citro3d.h>
#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_paths.h"
#include "diagnostic.h"
#include "game_art.h"
#include "http_client.h"
#include "shortcut_cia.h"
#include "ui.h"

#define SHORTCUTS_PATH APP_DATA_DIR "/shortcuts.json"
/* Title IDs 0x0004000004C01xx..04CFFFxx: one per game, near Kasumi's own
 * (0x4B534) and clear of the template's 0x4C000. */
#define UNIQUE_FIRST 0x4C001u
#define UNIQUE_COUNT 0xFFFu
/* How long to wait for the game's cover before drawing without it. */
#define ART_WAIT_MS 6000
/* Asked of img.nvidiagrid.net (it resizes on request): a few times the
 * icon's and banner's size, for a clean area-average downscale. */
#define ICON_SOURCE_WIDTH 192
#define WIDE_SOURCE_WIDTH 512

/* stb_image, compiled in game_art.c. */
extern unsigned char *stbi_load_from_memory(const unsigned char *buffer, int len, int *x, int *y, int *comp,
                                            int req_comp);
extern void stbi_image_free(void *data);

/* Shared with shortcut/source/main.c. */
typedef struct {
    char magic[8];
    u64 shortcut_id;
    u32 version;
} ShortcutArg;

static ShortcutState g_state;
static bool g_removing;
static GfnGame g_game;
static unsigned g_variant;
static char g_remove_app[48];
static u64 g_title_id;
static u64 g_requested_at;
static bool g_drawn;
static unsigned g_frames;
static volatile ShortcutStep g_step;
static volatile unsigned g_progress;
/* The finished art as linear 0xRRGGBBAA pixels, then as textures for the
 * sheet's preview (uploaded outside a frame, in shortcut_frame_end). */
static u32 *g_preview_banner_px, *g_preview_icon_px;
static bool g_preview_pending, g_preview_ready;
static C3D_Tex g_preview_banner_tex, g_preview_icon_tex;
static Tex3DS_SubTexture g_preview_banner_sub, g_preview_icon_sub;
/* Fetched for this shortcut by the worker (fetch_art): the cover at 48x48
 * and 24x24 (RGB), and the game's wide art at 256x128 (linear RGBA). */
static u8 g_icon_rgb[48 * 48 * 3], g_small_rgb[24 * 24 * 3];
static u32 *g_wide_px;
static bool g_have_icon, g_have_wide, g_wide_uploaded;
static C3D_Tex g_wide_tex;
static Tex3DS_SubTexture g_wide_sub;
static uint8_t g_smdh[SHORTCUT_SMDH_SIZE];
static uint16_t *g_banner;
static char g_result[128];
/* g_result is news for the player (a failure while drawing). */
static bool g_result_new;

/* Offscreen art: banner 256x128, icon 48x48 (in 64x64), and a probe that
 * tells which way the GPU wrote the rows and columns. */
static C3D_Tex g_banner_tex, g_icon_tex, g_probe_tex;
static C3D_RenderTarget *g_banner_target, *g_icon_target, *g_probe_target;

static u64 title_id_for(unsigned unique) { return 0x0004000000000000ULL | ((u64)unique << 8); }

/* ---- shortcuts.json -------------------------------------------------------- */

/* Read once, then kept: the game page asks every frame. The worker writes
 * it, the UI reads it, so both go through the lock. */
static json_t *g_map;
static LightLock g_lock;
static bool g_lock_ready;

static void lock(void)
{
    if (!g_lock_ready) {
        LightLock_Init(&g_lock);
        g_lock_ready = true;
    }
    LightLock_Lock(&g_lock);
}

static void unlock(void) { LightLock_Unlock(&g_lock); }

/* Under the lock. */
static json_t *map(void)
{
    if (g_map) return g_map;
    json_error_t error;
    g_map = json_load_file(SHORTCUTS_PATH, 0, &error);
    if (!json_is_object(g_map)) {
        json_decref(g_map);
        g_map = json_object();
    }
    return g_map;
}

static void save_map(void)
{
    json_dump_file(map(), SHORTCUTS_PATH, JSON_COMPACT);
}

static void id_key(u64 title_id, char out[17])
{
    snprintf(out, 17, "%016llX", (unsigned long long)title_id);
}

/* The title ID of this game's shortcut, or 0. */
static u64 find_shortcut(json_t *map, const char *app_id)
{
    const char *key;
    json_t *value;
    json_object_foreach(map, key, value) {
        const char *id = json_string_value(json_object_get(value, "app_id"));
        if (id && !strcmp(id, app_id)) return strtoull(key, NULL, 16);
    }
    return 0;
}

static json_t *game_json(const GfnGame *game, unsigned variant)
{
    json_t *variants = json_array();
    for (unsigned i = 0; i < game->variant_count && i < GFN_MAX_VARIANTS; ++i)
        json_array_append_new(variants, json_pack("{s:s,s:s}", "id", game->variants[i].id,
                                                  "store", game->variants[i].store));
    return json_pack("{s:s,s:s,s:s,s:s,s:i,s:o}", "title", game->title, "app_id", game->app_id,
                     "store", game->store, "image", game->image_url, "variant", (int)variant,
                     "variants", variants);
}

static void copy_string(char *out, size_t size, json_t *object, const char *key)
{
    const char *value = json_string_value(json_object_get(object, key));
    snprintf(out, size, "%s", value ? value : "");
}

static bool game_from_json(json_t *entry, GfnGame *game, unsigned *variant)
{
    memset(game, 0, sizeof(*game));
    copy_string(game->title, sizeof(game->title), entry, "title");
    copy_string(game->app_id, sizeof(game->app_id), entry, "app_id");
    copy_string(game->store, sizeof(game->store), entry, "store");
    copy_string(game->image_url, sizeof(game->image_url), entry, "image");
    json_t *variants = json_object_get(entry, "variants");
    size_t index;
    json_t *v;
    json_array_foreach(variants, index, v) {
        if (game->variant_count >= GFN_MAX_VARIANTS) break;
        copy_string(game->variants[game->variant_count].id, sizeof(game->variants[0].id), v, "id");
        copy_string(game->variants[game->variant_count].store, sizeof(game->variants[0].store), v, "store");
        ++game->variant_count;
    }
    json_t *chosen = json_object_get(entry, "variant");
    *variant = json_is_integer(chosen) ? (unsigned)json_integer_value(chosen) : 0;
    if (*variant >= game->variant_count) *variant = 0;
    game->variant_selected = *variant;
    return game->app_id[0] != '\0';
}

bool shortcut_exists(const char *app_id)
{
    if (!app_id || !app_id[0]) return false;
    lock();
    const bool found = find_shortcut(map(), app_id) != 0;
    unlock();
    return found;
}

/* A free ID for this game: the one it already has, else one picked from its
 * name (so it stays the same if the file is lost), stepping past others. */
static u64 pick_title_id(json_t *map, const char *app_id)
{
    const u64 existing = find_shortcut(map, app_id);
    if (existing) return existing;
    u32 hash = 2166136261u;
    for (const char *p = app_id; *p; ++p) hash = (hash ^ (u8)*p) * 16777619u;
    for (unsigned step = 0; step < UNIQUE_COUNT; ++step) {
        const u64 id = title_id_for(UNIQUE_FIRST + (hash + step) % UNIQUE_COUNT);
        char key[17];
        id_key(id, key);
        if (!json_object_get(map, key)) return id;
    }
    return 0;
}

/* ---- Requests (UI thread) ---------------------------------------------------- */

ShortcutState shortcut_state(void) { return g_state; }
ShortcutStep shortcut_step(void) { return g_step; }
unsigned shortcut_progress(void) { return g_progress; }
bool shortcut_removing(void) { return g_removing; }
static bool g_used_wide;
bool shortcut_used_wide_art(void) { return g_used_wide; }

static void release_preview(void)
{
    if (g_preview_ready) {
        C3D_TexDelete(&g_preview_banner_tex);
        C3D_TexDelete(&g_preview_icon_tex);
    }
    g_preview_ready = g_preview_pending = false;
}

const char *shortcut_take_failure(void)
{
    if (!g_result_new) return NULL;
    g_result_new = false;
    return g_result;
}

/* Drawing gave up: say why on screen and in reports. */
static void drawing_failed(const char *why)
{
    snprintf(g_result, sizeof(g_result), "%s", why);
    g_result_new = true;
    diagnostic_flag("shortcut-art", "%s", why);
    g_state = SHORTCUT_IDLE;
}
const char *shortcut_result(void) { return g_result; }

void shortcut_job_done(void)
{
    g_state = SHORTCUT_IDLE;
}

bool shortcut_request(const GfnGame *game, unsigned variant)
{
    if (g_state != SHORTCUT_IDLE || !game || !game->app_id[0]) return false;
    if (!g_banner) g_banner = malloc(SHORTCUT_BANNER_WIDTH * SHORTCUT_BANNER_HEIGHT * 2);
    if (!g_banner) return false;
    g_game = *game;
    g_variant = variant;
    g_removing = false;
    g_drawn = false;
    g_frames = 0;
    g_step = SHORTCUT_STEP_FETCH;
    g_progress = 0;
    g_have_icon = g_have_wide = g_wide_uploaded = false;
    release_preview();
    if (!g_wide_px) g_wide_px = linearAlloc(SHORTCUT_BANNER_WIDTH * SHORTCUT_BANNER_HEIGHT * 4);
    if (!g_preview_banner_px) g_preview_banner_px = linearAlloc(SHORTCUT_BANNER_WIDTH * SHORTCUT_BANNER_HEIGHT * 4);
    if (!g_preview_icon_px) g_preview_icon_px = linearAlloc(64 * 64 * 4);
    g_requested_at = osGetTime();
    g_result[0] = '\0';
    lock();
    g_title_id = pick_title_id(map(), game->app_id);
    unlock();
    if (!g_title_id) {
        snprintf(g_result, sizeof(g_result), i18n_text("No free shortcut slots left"));
        return false;
    }
    diagnostic_log("SHORTCUT", "making %016llX for %.60s", (unsigned long long)g_title_id, game->title);
    g_state = SHORTCUT_FETCH;
    return true;
}

bool shortcut_request_remove(const char *app_id)
{
    if (g_state != SHORTCUT_IDLE || !app_id || !app_id[0]) return false;
    lock();
    g_title_id = find_shortcut(map(), app_id);
    unlock();
    if (!g_title_id) return false;
    snprintf(g_remove_app, sizeof(g_remove_app), "%s", app_id);
    g_removing = true;
    g_step = SHORTCUT_STEP_INSTALL;
    g_progress = 0;
    g_result[0] = '\0';
    g_state = SHORTCUT_READY;
    return true;
}

/* ---- Drawing (UI thread) ------------------------------------------------------- */

/* citro3d renders only into VRAM ("Render targets must be in VRAM"; beta.30
 * test M6H8R5 asked for normal memory and quietly got nothing). The CPU
 * reads it back through Kasumi's read-only VRAM mapping (app.rsf). */
static bool make_target(C3D_Tex *tex, C3D_RenderTarget **target, u16 width, u16 height)
{
    if (*target) return true;
    if (!C3D_TexInitVRAM(tex, width, height, GPU_RGBA8)) return false;
    *target = C3D_RenderTargetCreateFromTex(tex, GPU_TEXFACE_2D, 0, -1);
    if (!*target) {
        C3D_TexDelete(tex);
        return false;
    }
    return true;
}

/* citro3d panics if a render target is deleted mid-frame (beta.30 test:
 * crash dump 34, svcBreak in C3D_RenderTargetDelete), so they go after
 * ui_frame_end (shortcut_frame_end). */
static bool g_free_pending;

static void free_targets(void)
{
    if (g_banner_target) C3D_RenderTargetDelete(g_banner_target);
    if (g_icon_target) C3D_RenderTargetDelete(g_icon_target);
    if (g_probe_target) C3D_RenderTargetDelete(g_probe_target);
    if (g_banner_target) C3D_TexDelete(&g_banner_tex);
    if (g_icon_target) C3D_TexDelete(&g_icon_tex);
    if (g_probe_target) C3D_TexDelete(&g_probe_tex);
    g_banner_target = g_icon_target = g_probe_target = NULL;
    if (g_wide_uploaded) C3D_TexDelete(&g_wide_tex);
    g_wide_uploaded = false;
}

/* The game's own wide art (usually with its logo) filling the banner, 1:1
 * so it stays sharp, and a small Kasumi mark in the corner. Without wide
 * art: Kasumi's look, black, the mist, the cover, the game's name. */
static bool draw_banner(void)
{
    C2D_TargetClear(g_banner_target, UI_BG);
    C2D_SceneBegin(g_banner_target);
    ui_offset(0.0f, 0.0f);
    g_used_wide = g_wide_uploaded;
    if (g_wide_uploaded) {
        const C2D_Image wide = { &g_wide_tex, &g_wide_sub };
        C2D_DrawImageAt(wide, 0.0f, 0.0f, 0.0f, NULL, 1.0f, 1.0f);
        ui_rect(186.0f, 106.0f, 64.0f, 17.0f, C2D_Color32(0x00, 0x00, 0x00, 0xB0));
        ui_rect(186.0f, 106.0f, 2.0f, 17.0f, UI_ACCENT);
        ui_image(UI_IMAGE_SEAL_16, 189.0f, 106.5f, 1.0f, 1.0f);
        ui_label(209.0f, 108.0f, 11.0f, UI_TEXT, UI_ALIGN_LEFT, "KASUMI");
        return true;
    }
    ui_image(UI_IMAGE_MIST, -8.0f, 52.0f, 0.68f, 0.9f);
    const float art_x = 14.0f, art_y = 12.0f, art_scale = 104.0f / GAME_ART_HEIGHT;
    const float art_w = GAME_ART_WIDTH * art_scale, art_h = GAME_ART_HEIGHT * art_scale;
    ui_rect(art_x - 1.0f, art_y - 1.0f, art_w + 2.0f, art_h + 2.0f, UI_LINE_STRONG);
    const bool art = game_art_draw(&g_game, art_x, art_y, art_scale, 1.0f);
    if (!art) {
        ui_rect(art_x, art_y, art_w, art_h, UI_RAISED);
        ui_text(art_x + art_w / 2.0f, art_y + art_h / 2.0f - 12.0f, 22.0f, UI_ACCENT, UI_ALIGN_CENTER, "霞");
    }
    const float text_x = art_x + art_w + 12.0f, text_w = 256.0f - text_x - 10.0f;
    ui_text_wrap(text_x, 18.0f, 15.0f, UI_TEXT, UI_ALIGN_LEFT, text_w, 3, 18.0f, g_game.title);
    ui_image(UI_IMAGE_SEAL_16, text_x, 98.0f, 1.0f, 1.0f);
    ui_label(text_x + 21.0f, 100.0f, 11.0f, UI_TEXT_DIM, UI_ALIGN_LEFT, "KASUMI");
    return art;
}

/* Only when the sharper cover couldn't be fetched: the library's small one,
 * cropped square. */
static void draw_icon(void)
{
    C2D_TargetClear(g_icon_target, UI_BG);
    C2D_SceneBegin(g_icon_target);
    ui_offset(0.0f, 0.0f);
    const float scale = 48.0f / GAME_ART_WIDTH;
    if (!game_art_draw(&g_game, 0.0f, (48.0f - GAME_ART_HEIGHT * scale) / 2.0f, scale, 1.0f)) {
        ui_rect(0.0f, 0.0f, 48.0f, 48.0f, UI_RAISED);
        ui_text(24.0f, 12.0f, 22.0f, UI_ACCENT, UI_ALIGN_CENTER, "霞");
    }
}

/* Red top-left, green top-right, blue bottom-left. */
static void draw_probe(void)
{
    C2D_TargetClear(g_probe_target, UI_BG);
    C2D_SceneBegin(g_probe_target);
    ui_offset(0.0f, 0.0f);
    ui_rect(0.0f, 0.0f, 8.0f, 8.0f, C2D_Color32(0xFF, 0x00, 0x00, 0xFF));
    ui_rect(56.0f, 0.0f, 8.0f, 8.0f, C2D_Color32(0x00, 0xFF, 0x00, 0xFF));
    ui_rect(0.0f, 56.0f, 8.0f, 8.0f, C2D_Color32(0x00, 0x00, 0xFF, 0xFF));
}

/* ---- Reading it back ------------------------------------------------------------ */

/* GPU textures and SMDH icons: 8x8 tiles, Z-order inside each. */
static u32 tiled_index(u32 x, u32 y, u32 width)
{
    u32 m = 0;
    for (int i = 0; i < 3; ++i) m |= ((x >> i) & 1u) << (2 * i) | ((y >> i) & 1u) << (2 * i + 1);
    return ((y >> 3) * (width >> 3) + (x >> 3)) * 64 + m;
}

typedef struct { bool flip_x, flip_y, abgr; } Orientation;

/* RGBA8 render targets should hold 0xRRGGBBAA words, as citro3d textures
 * do; the probe checks the other order too. */
static void read_pixel(const C3D_Tex *tex, Orientation o, u32 x, u32 y, u8 rgb[3])
{
    const u32 sx = o.flip_x ? tex->width - 1 - x : x, sy = o.flip_y ? tex->height - 1 - y : y;
    const u32 v = ((const u32 *)tex->data)[tiled_index(sx, sy, tex->width)];
    if (o.abgr) {
        rgb[0] = (u8)v;
        rgb[1] = (u8)(v >> 8);
        rgb[2] = (u8)(v >> 16);
    } else {
        rgb[0] = (u8)(v >> 24);
        rgb[1] = (u8)(v >> 16);
        rgb[2] = (u8)(v >> 8);
    }
}

static bool is_colour(const u8 rgb[3], int channel)
{
    for (int c = 0; c < 3; ++c)
        if ((c == channel) != (rgb[c] > 200) || (c != channel && rgb[c] > 60)) return false;
    return true;
}

/* Where the GPU put the probe's red corner says how rows and columns run. */
static bool find_orientation(Orientation *out)
{
    static const Orientation options[] = {
        { false, false, false }, { false, true, false }, { true, false, false }, { true, true, false },
        { false, false, true }, { false, true, true }, { true, false, true }, { true, true, true },
    };
    for (size_t i = 0; i < sizeof(options) / sizeof(options[0]); ++i) {
        u8 tl[3], tr[3], bl[3];
        read_pixel(&g_probe_tex, options[i], 4, 4, tl);
        read_pixel(&g_probe_tex, options[i], 59, 4, tr);
        read_pixel(&g_probe_tex, options[i], 4, 59, bl);
        if (is_colour(tl, 0) && is_colour(tr, 1) && is_colour(bl, 2)) {
            *out = options[i];
            return true;
        }
    }
    return false;
}

static void read_back(Orientation o)
{
    /* Banner: RGBA4444 with a 4x4 ordered dither, so the mist's soft
     * gradient doesn't band at 16 levels. */
    static const u8 bayer[4][4] = { { 0, 8, 2, 10 }, { 12, 4, 14, 6 }, { 3, 11, 1, 9 }, { 15, 7, 13, 5 } };
    for (u32 y = 0; y < SHORTCUT_BANNER_HEIGHT; ++y)
        for (u32 x = 0; x < SHORTCUT_BANNER_WIDTH; ++x) {
            u8 rgb[3];
            read_pixel(&g_banner_tex, o, x, y, rgb);
            if (g_preview_banner_px)
                g_preview_banner_px[y * SHORTCUT_BANNER_WIDTH + x] =
                    (u32)rgb[0] << 24 | (u32)rgb[1] << 16 | (u32)rgb[2] << 8 | 0xFF;
            const float threshold = (bayer[y & 3][x & 3] + 0.5f) / 16.0f;
            u16 q[3];
            for (int c = 0; c < 3; ++c) {
                int v = (int)(rgb[c] * 15.0f / 255.0f + threshold);
                q[c] = (u16)(v > 15 ? 15 : v);
            }
            g_banner[tiled_index(x, y, SHORTCUT_BANNER_WIDTH)] = (u16)(q[0] << 12 | q[1] << 8 | q[2] << 4 | 0xF);
        }
    /* Icons: RGB565, 48x48 and 24x24, each straight from the fetched
     * cover; else the GPU's drawing of the small one. */
    u8 *large = g_smdh + 0x24C0, *small = g_smdh + 0x2040;
    static u8 pixels[48][48][3];
    for (u32 y = 0; y < 48; ++y)
        for (u32 x = 0; x < 48; ++x) {
            if (g_have_icon) memcpy(pixels[y][x], g_icon_rgb + (y * 48 + x) * 3, 3);
            else read_pixel(&g_icon_tex, o, x, y, pixels[y][x]);
            const u8 *p = pixels[y][x];
            if (g_preview_icon_px) g_preview_icon_px[y * 64 + x] = (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | 0xFF;
            const u16 v = (u16)((p[0] >> 3) << 11 | (p[1] >> 2) << 5 | p[2] >> 3);
            const u32 i = tiled_index(x, y, 48);
            large[i * 2] = (u8)v;
            large[i * 2 + 1] = (u8)(v >> 8);
        }
    for (u32 y = 0; y < 24; ++y)
        for (u32 x = 0; x < 24; ++x) {
            unsigned sum[3] = { 0, 0, 0 };
            for (u32 dy = 0; dy < 2; ++dy)
                for (u32 dx = 0; dx < 2; ++dx)
                    for (int c = 0; c < 3; ++c) sum[c] += pixels[y * 2 + dy][x * 2 + dx][c];
            if (g_have_icon)
                for (int c = 0; c < 3; ++c) sum[c] = g_small_rgb[(y * 24 + x) * 3 + c] * 4u;
            const u16 v = (u16)(((sum[0] / 4) >> 3) << 11 | ((sum[1] / 4) >> 2) << 5 | (sum[2] / 4) >> 3);
            const u32 i = tiled_index(x, y, 24);
            small[i * 2] = (u8)v;
            small[i * 2 + 1] = (u8)(v >> 8);
        }
}

/* UTF-8 into a fixed UTF-16 field (zero-filled; the last unit stays 0). */
static void put_utf16(u8 *field, size_t units, const char *text)
{
    memset(field, 0, units * 2);
    size_t n = 0;
    const u8 *p = (const u8 *)text;
    while (*p && n + 1 < units) {
        u32 c = *p++;
        if (c >= 0xF0 && p[0] && p[1] && p[2]) { c = (c & 7) << 18 | (p[0] & 63u) << 12 | (p[1] & 63u) << 6 | (p[2] & 63u); p += 3; }
        else if (c >= 0xE0 && p[0] && p[1]) { c = (c & 15) << 12 | (p[0] & 63u) << 6 | (p[1] & 63u); p += 2; }
        else if (c >= 0xC0 && p[0]) { c = (c & 31) << 6 | (p[0] & 63u); p += 1; }
        if (c > 0xFFFF) c = '?';
        field[n * 2] = (u8)c;
        field[n * 2 + 1] = (u8)(c >> 8);
        ++n;
    }
}

static bool make_smdh(void)
{
    if (!shortcut_cia_template_smdh(g_smdh)) return false;
    char longer[128];
    snprintf(longer, sizeof(longer), "%s (Kasumi)", g_game.title);
    for (int language = 0; language < 16; ++language) {
        u8 *names = g_smdh + 8 + language * 0x200;
        put_utf16(names, 0x40, g_game.title);
        put_utf16(names + 0x80, 0x80, longer);
        put_utf16(names + 0x180, 0x40, "Kasumi shortcut");
    }
    return true;
}

/* Linear pixels into a texture, the way game_art.c does: one display
 * transfer, row 0 at the top (v = 1). Outside a frame. */
static bool upload(C3D_Tex *tex, Tex3DS_SubTexture *sub, u32 *pixels, u16 width, u16 height, u16 shown_w, u16 shown_h)
{
    if (!C3D_TexInit(tex, width, height, GPU_RGBA8)) return false;
    GSPGPU_FlushDataCache(tex->data, tex->size);
    GSPGPU_FlushDataCache(pixels, (u32)width * height * 4);
    C3D_SyncDisplayTransfer(pixels, GX_BUFFER_DIM(width, height), (u32 *)tex->data, GX_BUFFER_DIM(width, height),
                            GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(1) | GX_TRANSFER_RAW_COPY(0) |
                            GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) |
                            GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGBA8) |
                            GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO));
    C3D_TexSetFilter(tex, GPU_LINEAR, GPU_LINEAR);
    C3D_TexSetWrap(tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    sub->width = shown_w;
    sub->height = shown_h;
    sub->left = 0.0f;
    sub->top = 1.0f;
    sub->right = (float)shown_w / width;
    sub->bottom = 1.0f - (float)shown_h / height;
    return true;
}

void shortcut_frame_end(void)
{
    if (g_free_pending) {
        g_free_pending = false;
        free_targets();
    }
    if (g_state == SHORTCUT_DRAWING && g_have_wide && !g_wide_uploaded && g_wide_px) {
        g_wide_uploaded = upload(&g_wide_tex, &g_wide_sub, g_wide_px, SHORTCUT_BANNER_WIDTH,
                                 SHORTCUT_BANNER_HEIGHT, SHORTCUT_BANNER_WIDTH, SHORTCUT_BANNER_HEIGHT);
        if (!g_wide_uploaded) g_have_wide = false;
    }
    if (g_preview_pending) {
        g_preview_pending = false;
        const bool banner = upload(&g_preview_banner_tex, &g_preview_banner_sub, g_preview_banner_px,
                                   SHORTCUT_BANNER_WIDTH, SHORTCUT_BANNER_HEIGHT, SHORTCUT_BANNER_WIDTH,
                                   SHORTCUT_BANNER_HEIGHT);
        const bool icon = upload(&g_preview_icon_tex, &g_preview_icon_sub, g_preview_icon_px, 64, 64, 48, 48);
        if (banner && icon) {
            g_preview_ready = true;
        } else {
            if (banner) C3D_TexDelete(&g_preview_banner_tex);
            if (icon) C3D_TexDelete(&g_preview_icon_tex);
        }
    }
}

bool shortcut_preview_banner(float x, float y, float scale)
{
    if (!g_preview_ready) return false;
    const C2D_Image image = { &g_preview_banner_tex, &g_preview_banner_sub };
    return C2D_DrawImageAt(image, x, y, 0.0f, NULL, scale, scale);
}

bool shortcut_preview_icon(float x, float y, float scale)
{
    if (!g_preview_ready) return false;
    const C2D_Image image = { &g_preview_icon_tex, &g_preview_icon_sub };
    return C2D_DrawImageAt(image, x, y, 0.0f, NULL, scale, scale);
}

void shortcut_frame(void)
{
    if (g_state != SHORTCUT_DRAWING) return;
    if (g_drawn) {
        /* A frame later: C3D_FrameBegin waited for the GPU to finish it. */
        Orientation o;
        if (!find_orientation(&o)) {
            g_free_pending = true;
            drawing_failed("Couldn't draw the shortcut's picture (unknown pixel order)");
            return;
        }
        if (!make_smdh()) {
            g_free_pending = true;
            drawing_failed("The shortcut template is missing from this build");
            return;
        }
        read_back(o);
        g_free_pending = true;
        diagnostic_log("SHORTCUT", "art drawn (flipX=%d flipY=%d abgr=%d)", o.flip_x, o.flip_y, o.abgr);
        g_preview_pending = g_preview_banner_px && g_preview_icon_px;
        g_step = SHORTCUT_STEP_BUILD;
        g_state = SHORTCUT_READY;
        return;
    }
    /* The wide art goes up as a texture after this frame; draw then. */
    if (g_have_wide && !g_wide_uploaded) return;
    game_art_want(&g_game);
    if (!make_target(&g_banner_tex, &g_banner_target, SHORTCUT_BANNER_WIDTH, SHORTCUT_BANNER_HEIGHT) ||
        !make_target(&g_icon_tex, &g_icon_target, 64, 64) || !make_target(&g_probe_tex, &g_probe_target, 64, 64)) {
        g_free_pending = true;
        drawing_failed("Not enough video memory to draw the shortcut");
        return;
    }
    if (!g_frames++) diagnostic_log("SHORTCUT", "drawing in VRAM %p/%p", g_banner_tex.data, g_icon_tex.data);
    const bool art = draw_banner();
    draw_icon();
    draw_probe();
    /* Without the cover yet, draw again next frame until it arrives. */
    if (art || osGetTime() - g_requested_at >= ART_WAIT_MS) {
        if (!art) diagnostic_log("SHORTCUT", "cover not ready; drawing without it");
        g_drawn = true;
    }
}

/* ---- Installing (worker thread) ---------------------------------------------- */

static bool install(const u8 *data, size_t size, u64 title_id)
{
    if (R_FAILED(amInit())) {
        snprintf(g_result, sizeof(g_result), i18n_text("Could not open the system installer"));
        return false;
    }
    /* Remaking one: the old copy goes first. */
    u64 id = title_id;
    AM_TitleEntry entry;
    if (R_SUCCEEDED(AM_GetTitleInfo(MEDIATYPE_SD, 1, &id, &entry))) AM_DeleteTitle(MEDIATYPE_SD, title_id);
    Handle cia;
    Result rc = AM_StartCiaInstall(MEDIATYPE_SD, &cia);
    if (R_FAILED(rc)) {
        amExit();
        snprintf(g_result, sizeof(g_result), i18n_text("The installer refused to start (0x%08lX)"), (unsigned long)rc);
        return false;
    }
    const size_t chunk = 64 * 1024;
    for (size_t offset = 0; offset < size; offset += chunk) {
        const size_t n = size - offset < chunk ? size - offset : chunk;
        u32 written = 0;
        rc = FSFILE_Write(cia, &written, offset, data + offset, (u32)n, 0);
        if (R_FAILED(rc) || written != n) {
            AM_CancelCIAInstall(cia);
            amExit();
            snprintf(g_result, sizeof(g_result), i18n_text("Writing the shortcut failed (0x%08lX)"), (unsigned long)rc);
            return false;
        }
        g_progress = (unsigned)((offset + n) * 1000 / size);
    }
    rc = AM_FinishCiaInstall(cia);
    amExit();
    if (R_FAILED(rc)) {
        snprintf(g_result, sizeof(g_result), i18n_text("The shortcut could not be installed (0x%08lX)"), (unsigned long)rc);
        return false;
    }
    return true;
}

static bool remove_title(u64 title_id)
{
    if (R_FAILED(amInit())) {
        snprintf(g_result, sizeof(g_result), i18n_text("Could not open the system installer"));
        return false;
    }
    u64 id = title_id;
    AM_TitleEntry entry;
    Result rc = 0;
    /* Already deleted from System Settings: just forget it. */
    if (R_SUCCEEDED(AM_GetTitleInfo(MEDIATYPE_SD, 1, &id, &entry))) rc = AM_DeleteTitle(MEDIATYPE_SD, title_id);
    if (R_SUCCEEDED(rc)) AM_DeleteTicket(title_id);
    amExit();
    if (R_FAILED(rc)) {
        snprintf(g_result, sizeof(g_result), i18n_text("The shortcut could not be removed (0x%08lX)"), (unsigned long)rc);
        return false;
    }
    return true;
}

/* A cover-crop area average: fill ow x oh from the middle of the source,
 * every output pixel the mean of the source pixels under it. */
static void cover_resize(const u8 *src, int w, int h, int ow, int oh, u8 *out)
{
    const float scale_x = (float)ow / w, scale_y = (float)oh / h;
    const float scale = scale_x > scale_y ? scale_x : scale_y;
    const float x0 = (w - ow / scale) / 2.0f, y0 = (h - oh / scale) / 2.0f;
    for (int dy = 0; dy < oh; ++dy) {
        int sy0 = (int)(y0 + dy / scale), sy1 = (int)(y0 + (dy + 1) / scale);
        if (sy0 >= h) sy0 = h - 1;
        if (sy1 <= sy0) sy1 = sy0 + 1;
        if (sy1 > h) sy1 = h;
        for (int dx = 0; dx < ow; ++dx) {
            int sx0 = (int)(x0 + dx / scale), sx1 = (int)(x0 + (dx + 1) / scale);
            if (sx0 >= w) sx0 = w - 1;
            if (sx1 <= sx0) sx1 = sx0 + 1;
            if (sx1 > w) sx1 = w;
            unsigned sum[3] = { 0, 0, 0 }, count = 0;
            for (int sy = sy0; sy < sy1; ++sy)
                for (int sx = sx0; sx < sx1; ++sx, ++count)
                    for (int c = 0; c < 3; ++c) sum[c] += src[((size_t)sy * w + sx) * 3 + c];
            for (int c = 0; c < 3; ++c) out[((size_t)dy * ow + dx) * 3 + c] = (u8)(sum[c] / (count ? count : 1));
        }
    }
}

/* One image from the catalog at `width`, decoded to RGB (stbi_image_free). */
static u8 *fetch_image(const char *url, int width, int *w, int *h)
{
    if (!url || !url[0]) return NULL;
    char sized[256];
    if (strstr(url, "img.nvidiagrid.net")) snprintf(sized, sizeof(sized), "%s;f=jpg;w=%d", url, width);
    else snprintf(sized, sizeof(sized), "%s", url);
    static const char *const headers[] = { "Accept: image/jpeg,image/png,*/*" };
    HttpResponse response;
    if (!http_request("GET", sized, "Kasumi-3DS", headers, 1, NULL, 2 * 1024 * 1024, &response)) {
        diagnostic_log("SHORTCUT", "art download failed: %.80s", response.error);
        return NULL;
    }
    u8 *rgb = NULL;
    int comp = 0;
    if (response.status == 200 && response.body && response.size)
        rgb = stbi_load_from_memory((const unsigned char *)response.body, (int)response.size, w, h, &comp, 3);
    if (!rgb || *w <= 0 || *h <= 0)
        diagnostic_log("SHORTCUT", "art http=%ld bytes=%lu not decoded", response.status, (unsigned long)response.size);
    http_response_free(&response);
    return rgb;
}

/* Worker: a sharp cover for the icon and the wide art for the banner.
 * Either may be missing; the drawing step falls back to the library's. */
static void fetch_art(void)
{
    int w = 0, h = 0;
    u8 *cover = fetch_image(g_game.image_url, ICON_SOURCE_WIDTH, &w, &h);
    if (cover) {
        cover_resize(cover, w, h, 48, 48, g_icon_rgb);
        cover_resize(cover, w, h, 24, 24, g_small_rgb);
        stbi_image_free(cover);
        g_have_icon = true;
    }
    g_progress = 500;
    u8 *wide = g_wide_px ? fetch_image(g_game.wide_url, WIDE_SOURCE_WIDTH, &w, &h) : NULL;
    if (wide) {
        static u8 rgb[SHORTCUT_BANNER_WIDTH * SHORTCUT_BANNER_HEIGHT * 3];
        cover_resize(wide, w, h, SHORTCUT_BANNER_WIDTH, SHORTCUT_BANNER_HEIGHT, rgb);
        stbi_image_free(wide);
        for (int i = 0; i < SHORTCUT_BANNER_WIDTH * SHORTCUT_BANNER_HEIGHT; ++i)
            g_wide_px[i] = (u32)rgb[i * 3] << 24 | (u32)rgb[i * 3 + 1] << 16 | (u32)rgb[i * 3 + 2] << 8 | 0xFF;
        g_have_wide = true;
    }
    diagnostic_log("SHORTCUT", "art fetched: icon=%d wide=%d (%s)", g_have_icon, g_have_wide,
                   g_game.wide_url[0] ? "has wide art" : "no wide art in the library yet");
}

bool shortcut_work(void)
{
    if (g_state == SHORTCUT_FETCH) {
        g_state = SHORTCUT_FETCHING;
        fetch_art();
        g_requested_at = osGetTime();
        g_step = SHORTCUT_STEP_DRAW;
        g_progress = 0;
        g_state = SHORTCUT_DRAWING;
        return true;
    }
    if (g_state != SHORTCUT_READY) return false;
    g_state = SHORTCUT_WORKING;
    char key[17];
    id_key(g_title_id, key);
    if (g_removing) {
        const bool ok = remove_title(g_title_id);
        if (ok) g_step = SHORTCUT_STEP_DONE;
        if (ok) {
            lock();
            json_object_del(map(), key);
            save_map();
            unlock();
            snprintf(g_result, sizeof(g_result), i18n_text("Shortcut removed from the HOME Menu"));
        }
        diagnostic_log("SHORTCUT", "remove %s: %s", key, ok ? "done" : g_result);
        return ok;
    }
    char error[96] = "";
    size_t size = 0;
    g_step = SHORTCUT_STEP_BUILD;
    g_progress = 0;
    u8 *cia = shortcut_cia_build(g_title_id, g_smdh, g_banner, &size, error, sizeof(error));
    if (!cia) {
        diagnostic_flag("shortcut-build", "%s", error);
        snprintf(g_result, sizeof(g_result), i18n_text("Couldn't make the shortcut: %.70s"), error);
        return false;
    }
    g_step = SHORTCUT_STEP_INSTALL;
    const bool ok = install(cia, size, g_title_id);
    free(cia);
    if (ok) g_step = SHORTCUT_STEP_DONE;
    if (ok) {
        lock();
        json_object_set_new(map(), key, game_json(&g_game, g_variant));
        save_map();
        unlock();
        snprintf(g_result, sizeof(g_result), i18n_text("Shortcut added: find %.60s on the HOME Menu"), g_game.title);
    } else {
        diagnostic_flag("shortcut-install", "%s", g_result);
    }
    diagnostic_log("SHORTCUT", "install %s (%lu bytes): %s", key, (unsigned long)size, ok ? "done" : g_result);
    return ok;
}

/* ---- Being opened by one ------------------------------------------------------ */

bool shortcut_take_launch(GfnGame *game, unsigned *variant)
{
    u8 param[0x300];
    u8 hmac[0x20];
    u64 sender = 0;
    bool received = false;
    memset(param, 0, sizeof(param));
    if (R_FAILED(APT_ReceiveDeliverArg(param, sizeof(param), hmac, &sender, &received)) || !received) return false;
    ShortcutArg arg;
    memcpy(&arg, param, sizeof(arg));
    if (memcmp(arg.magic, "KSHORTC1", 8)) return false;
    char key[17];
    id_key(arg.shortcut_id, key);
    lock();
    json_t *entry = json_object_get(map(), key);
    const bool ok = json_is_object(entry) && game_from_json(entry, game, variant);
    unlock();
    diagnostic_log("SHORTCUT", "opened from %s: %s", key, ok ? game->title : "unknown shortcut");
    return ok;
}

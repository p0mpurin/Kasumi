#include "ui.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define FONT_PX 30.0f
/* Must match mvd_video.h: the decoder's wide output surface. */
#define MVD_TEX_WIDTH 1024
#define MVD_TEX_HEIGHT 512
#define VIDEO_WIDTH 800
#define VIDEO_HEIGHT 480
#define TEXT_GLYPHS 6144

u32 g_ui_accent = 0xFFA5BE7Eu; /* C2D_Color32 packs ABGR */
u32 g_ui_accent_deep = 0xFF262E14u;
u32 g_ui_accent_ink = 0xFF212A0Eu;
static UiTheme g_theme;

/* 青磁 celadon, 桜 cherry, 金 gold, 藍 indigo, 藤 wisteria. */
static const struct { const char *name; u8 r, g, b; } THEMES[UI_THEME_COUNT] = {
    { "Seiji", 0x7E, 0xBE, 0xA5 },
    { "Sakura", 0xE8, 0x9A, 0xB4 },
    { "Kin", 0xD8, 0xA5, 0x3F },
    { "Ai", 0x74, 0x9B, 0xD8 },
    { "Fuji", 0xA9, 0x8E, 0xD6 },
};

void ui_set_theme(UiTheme theme)
{
    if (theme >= UI_THEME_COUNT) theme = UI_THEME_SEIJI;
    g_theme = theme;
    const u8 r = THEMES[theme].r, g = THEMES[theme].g, b = THEMES[theme].b;
    g_ui_accent = C2D_Color32(r, g, b, 0xFF);
    g_ui_accent_deep = C2D_Color32(r * 18 / 100, g * 18 / 100, b * 18 / 100, 0xFF);
    g_ui_accent_ink = C2D_Color32(r * 12 / 100, g * 12 / 100, b * 12 / 100, 0xFF);
}

const char *ui_theme_name(UiTheme theme)
{
    return theme < UI_THEME_COUNT ? THEMES[theme].name : "";
}

u32 ui_theme_color(UiTheme theme)
{
    if (theme >= UI_THEME_COUNT) theme = UI_THEME_SEIJI;
    return C2D_Color32(THEMES[theme].r, THEMES[theme].g, THEMES[theme].b, 0xFF);
}

static C3D_RenderTarget *g_top;
static C3D_RenderTarget *g_top_wide;
static C3D_RenderTarget *g_bottom;
static bool g_top_wide_linked;
static C3D_Tex g_video_tex;
static Tex3DS_SubTexture g_video_subtex;
static bool g_video_ready;
static C2D_TextBuf g_text;
static u64 g_started_at;
static u64 g_last_frame_at;
static float g_dt;

static bool g_has_japanese;

#define GFX_SYMBOLS(name)     extern const unsigned char _binary_##name##_t3x_start[];     extern const unsigned char _binary_##name##_t3x_end[];
GFX_SYMBOLS(hero)
GFX_SYMBOLS(mist)
GFX_SYMBOLS(enso)
GFX_SYMBOLS(seal)
GFX_SYMBOLS(seal40)
GFX_SYMBOLS(seal16)
GFX_SYMBOLS(lantern)
#define GFX_ENTRY(name) { _binary_##name##_t3x_start, _binary_##name##_t3x_end }

static const struct { const unsigned char *start, *end; } GFX_DATA[UI_IMAGE_COUNT] = {
    GFX_ENTRY(hero), GFX_ENTRY(mist), GFX_ENTRY(enso), GFX_ENTRY(seal), GFX_ENTRY(seal40),
    GFX_ENTRY(seal16), GFX_ENTRY(lantern)
};
static C2D_SpriteSheet g_sheets[UI_IMAGE_COUNT];

/* The top screen stays RGB565 because MVD video is copied straight into its
 * framebuffer; the render target converts to that format on transfer. */
#define TRANSFER_FLAGS(out) \
    (GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) | \
     GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(out) | \
     GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))

bool ui_init(void)
{
    if (!C3D_Init(C3D_DEFAULT_CMDBUF_SIZE)) return false;
    if (!C2D_Init(8192)) { C3D_Fini(); return false; }
    C2D_Prepare();
    fontEnsureMapped();
    g_top = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH16);
    g_bottom = C3D_RenderTargetCreate(240, 320, GPU_RB_RGBA8, GPU_RB_DEPTH16);
    /* Linked to the top screen only while wide video is showing. */
    g_top_wide = C3D_RenderTargetCreate(240, 800, GPU_RB_RGBA8, GPU_RB_DEPTH16);
    if (!g_top || !g_bottom || !g_top_wide) return false;
    if (C3D_TexInit(&g_video_tex, MVD_TEX_WIDTH, MVD_TEX_HEIGHT, GPU_RGB565)) {
        C3D_TexSetFilter(&g_video_tex, GPU_LINEAR, GPU_LINEAR);
        C3D_TexSetWrap(&g_video_tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
        /* Transferred unflipped, frame row 0 lands at v=1 (checked with a
         * test pattern: red top-left stays top-left). */
        g_video_subtex.width = VIDEO_WIDTH;
        g_video_subtex.height = VIDEO_HEIGHT;
        g_video_subtex.left = 0.0f;
        g_video_subtex.top = 1.0f;
        g_video_subtex.right = (float)VIDEO_WIDTH / MVD_TEX_WIDTH;
        g_video_subtex.bottom = 1.0f - (float)VIDEO_HEIGHT / MVD_TEX_HEIGHT;
        g_video_ready = true;
    }
    C3D_RenderTargetSetOutput(g_top, GFX_TOP, GFX_LEFT,
                              TRANSFER_FLAGS(gfxGetScreenFormat(GFX_TOP)));
    C3D_RenderTargetSetOutput(g_bottom, GFX_BOTTOM, GFX_LEFT,
                              TRANSFER_FLAGS(gfxGetScreenFormat(GFX_BOTTOM)));
    for (int i = 0; i < UI_IMAGE_COUNT; ++i)
        g_sheets[i] = C2D_SpriteSheetLoadFromMem(GFX_DATA[i].start,
                                                 (size_t)(GFX_DATA[i].end - GFX_DATA[i].start));
    g_text = C2D_TextBufNew(TEXT_GLYPHS);
    g_started_at = osGetTime();
    /* Regional system fonts (and emulators without a dumped font) can lack
     * kana/kanji; the decorative Japanese labels are then left out. */
    const int missing = fontGetInfo(NULL)->alterCharIndex;
    g_has_japanese = fontGlyphIndexFromCodePoint(NULL, 0x971E) != missing &&
                     fontGlyphIndexFromCodePoint(NULL, 0x30E9) != missing;
    return g_text != NULL;
}

void ui_exit(void)
{
    if (g_text) C2D_TextBufDelete(g_text);
    for (int i = 0; i < UI_IMAGE_COUNT; ++i)
        if (g_sheets[i]) C2D_SpriteSheetFree(g_sheets[i]);
    if (g_video_ready) C3D_TexDelete(&g_video_tex);
    if (g_top_wide) C3D_RenderTargetDelete(g_top_wide);
    if (g_top) C3D_RenderTargetDelete(g_top);
    if (g_bottom) C3D_RenderTargetDelete(g_bottom);
    C2D_Fini();
    C3D_Fini();
}

void ui_frame_begin(bool sync_vblank)
{
    C3D_FrameBegin(sync_vblank ? C3D_FRAME_SYNCDRAW : 0);
    C2D_TextBufClear(g_text);
    const u64 now = osGetTime();
    g_dt = g_last_frame_at ? (float)(now - g_last_frame_at) / 1000.0f : 0.0f;
    if (g_dt > 0.1f) g_dt = 0.1f;
    g_last_frame_at = now;
}

float ui_dt(void) { return g_dt; }

float ui_approach(float current, float target, float rate)
{
    const float step = 1.0f - expf(-rate * g_dt);
    const float next = current + (target - current) * step;
    return fabsf(next - target) < 0.05f ? target : next;
}

float ui_progress(u64 start_ms, float ms)
{
    const float t = (float)(osGetTime() - start_ms) / ms;
    return t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
}

float ui_ease_out(float t)
{
    const float inv = 1.0f - t;
    return 1.0f - inv * inv * inv;
}

void ui_offset(float dx, float dy)
{


    C2D_ViewReset();
    if (dx != 0.0f || dy != 0.0f) C2D_ViewTranslate(dx, dy);
}

void ui_frame_end(void) { C3D_FrameEnd(0); }

/* Only one render target can feed a screen; swap which one is linked. */
static void link_top(bool wide)
{
    if (wide == g_top_wide_linked) return;
    C3D_RenderTargetSetOutput(wide ? g_top_wide : g_top, GFX_TOP, GFX_LEFT,
                              TRANSFER_FLAGS(gfxGetScreenFormat(GFX_TOP)));
    g_top_wide_linked = wide;
    gfxSetWide(wide);
    /* Wide video is double-buffered so a new frame only appears at a vblank
     * (no tearing). Classic video writes the visible buffer directly. */
    gfxSetDoubleBuffering(GFX_TOP, wide);
}

void ui_begin_top(void)
{
    link_top(false);
    C2D_TargetClear(g_top, UI_BG);
    C2D_SceneBegin(g_top);
    ui_offset(0.0f, 0.0f);
}

bool ui_video_upload(const void *surface)
{
    if (!g_video_ready || !surface) return false;
    C3D_SyncDisplayTransfer((u32 *)surface, GX_BUFFER_DIM(MVD_TEX_WIDTH, MVD_TEX_HEIGHT),
                            (u32 *)g_video_tex.data, GX_BUFFER_DIM(MVD_TEX_WIDTH, MVD_TEX_HEIGHT),
                            GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(1) |
                            GX_TRANSFER_RAW_COPY(0) |
                            GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGB565) |
                            GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB565) |
                            GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO));
    return true;
}

void ui_begin_top_video(void)
{
    link_top(true);
    C2D_TargetClear(g_top_wide, UI_BG);
    C2D_SceneBegin(g_top_wide);
    /* citro2d reuses its cached 400-wide top-screen projection for 800-wide
     * targets too (a framebuffer readback showed everything drawn at twice
     * the width), so halve x to address all 800 columns. */
    C2D_ViewReset();
    C2D_ViewScale(0.5f, 1.0f);
}

void ui_draw_video(void)
{
    if (!g_video_ready) return;
    const C2D_Image image = { &g_video_tex, &g_video_subtex };
    /* Full width, half height: 800x480 -> 800x240 with a 2:1 row average. */
    C2D_DrawImageAt(image, 0.0f, 0.0f, 0.0f, NULL, 1.0f, 0.5f);
}

void ui_begin_bottom(void)
{
    C2D_TargetClear(g_bottom, UI_BG);
    C2D_SceneBegin(g_bottom);
    ui_offset(0.0f, 0.0f);
}

u64 ui_ticks(void) { return osGetTime() - g_started_at; }

bool ui_image(UiImage image, float x, float y, float scale, float alpha)
{
    if (image >= UI_IMAGE_COUNT || !g_sheets[image]) return false;
    C2D_ImageTint tint;
    C2D_AlphaImageTint(&tint, alpha);
    /* The seal, ensō and lantern art is celadon with transparent cut-outs,
     * so other themes recolour it wholesale. */
    const bool recolour = g_theme != UI_THEME_SEIJI &&
        (image == UI_IMAGE_SEAL || image == UI_IMAGE_SEAL_40 || image == UI_IMAGE_SEAL_16 ||
         image == UI_IMAGE_ENSO || image == UI_IMAGE_LANTERN);
    if (recolour) C2D_PlainImageTint(&tint, ui_with_alpha(g_ui_accent, (u8)(alpha * 255.0f)), 1.0f);
    /* Snap to whole pixels: a half-pixel offset makes bilinear filtering
     * smear every edge (the status-bar seal was drawn at x.5). */
    return C2D_DrawImageAt(C2D_SpriteSheetGetImage(g_sheets[image], 0), floorf(x + 0.5f),
                           floorf(y + 0.5f), 0.0f, recolour || alpha < 1.0f ? &tint : NULL,
                           scale, scale);
}

bool ui_image_rotated(UiImage image, float cx, float cy, float scale, float angle, float alpha)
{
    if (image >= UI_IMAGE_COUNT || !g_sheets[image]) return false;
    C2D_ImageTint tint;
    C2D_AlphaImageTint(&tint, alpha);
    const bool recolour = g_theme != UI_THEME_SEIJI && image == UI_IMAGE_ENSO;
    if (recolour) C2D_PlainImageTint(&tint, ui_with_alpha(g_ui_accent, (u8)(alpha * 255.0f)), 1.0f);
    return C2D_DrawImageAtRotated(C2D_SpriteSheetGetImage(g_sheets[image], 0), cx, cy, 0.0f,
                                  angle, recolour || alpha < 1.0f ? &tint : NULL, scale, scale);
}

u32 ui_with_alpha(u32 color, u8 alpha) { return (color & 0x00FFFFFFu) | ((u32)alpha << 24); }

u32 ui_mix(u32 a, u32 b, float t)
{
    if (t <= 0.0f) return a;
    if (t >= 1.0f) return b;
    u32 out = 0;
    for (unsigned shift = 0; shift < 32; shift += 8) {
        const float ca = (float)((a >> shift) & 0xFF), cb = (float)((b >> shift) & 0xFF);
        out |= (u32)(ca + (cb - ca) * t + 0.5f) << shift;
    }
    return out;
}

void ui_rect(float x, float y, float w, float h, u32 color)
{
    if (w > 0 && h > 0) C2D_DrawRectSolid(x, y, 0.0f, w, h, color);
}

void ui_rect_r(UiRect r, u32 color) { ui_rect(r.x, r.y, r.w, r.h, color); }

void ui_outline(float x, float y, float w, float h, float t, u32 color)
{
    ui_rect(x, y, w, t, color);
    ui_rect(x, y + h - t, w, t, color);
    ui_rect(x, y + t, t, h - 2 * t, color);
    ui_rect(x + w - t, y + t, t, h - 2 * t, color);
}

void ui_rounded(float x, float y, float w, float h, float r, u32 color)
{
    if (r * 2 > h) r = h / 2;
    if (r * 2 > w) r = w / 2;
    ui_rect(x + r, y, w - 2 * r, h, color);
    ui_rect(x, y + r, r, h - 2 * r, color);
    ui_rect(x + w - r, y + r, r, h - 2 * r, color);
    C2D_DrawCircleSolid(x + r, y + r, 0.0f, r, color);
    C2D_DrawCircleSolid(x + w - r, y + r, 0.0f, r, color);
    C2D_DrawCircleSolid(x + r, y + h - r, 0.0f, r, color);
    C2D_DrawCircleSolid(x + w - r, y + h - r, 0.0f, r, color);
}

void ui_hline(float x, float y, float w, u32 color) { ui_rect(x, y, w, 1.0f, color); }
void ui_vline(float x, float y, float h, u32 color) { ui_rect(x, y, 1.0f, h, color); }
void ui_circle(float cx, float cy, float r, u32 color) { C2D_DrawCircleSolid(cx, cy, 0.0f, r, color); }

void ui_ring(float cx, float cy, float r, float thickness, u32 color, u32 inside)
{
    ui_circle(cx, cy, r, color);
    ui_circle(cx, cy, r - thickness, inside);
}

void ui_line(float x0, float y0, float x1, float y1, float thickness, u32 color)
{
    C2D_DrawLine(x0, y0, color, x1, y1, color, thickness, 0.0f);
}

void ui_triangle(float x0, float y0, float x1, float y1, float x2, float y2, u32 color)
{
    C2D_DrawTriangle(x0, y0, color, x1, y1, color, x2, y2, color, 0.0f);
}

/* ---- Text ---------------------------------------------------------------- */

static float glyph_advance(u32 code_point)
{
    const int index = fontGlyphIndexFromCodePoint(NULL, code_point);
    const charWidthInfo_s *info = fontGetCharWidthInfo(NULL, index);
    return info ? (float)info->charWidth : 0.0f;
}

static bool contains_japanese(const char *text);
static float japanese_size(const char *text, float size, float *dy);

float ui_text_width(const char *text, float size)
{
    if (!text) return 0.0f;
    float dy;
    size = japanese_size(text, size, &dy);
    float width = 0.0f;
    const uint8_t *p = (const uint8_t *)text;
    while (*p && *p != '\n') {
        u32 code = 0;
        const ssize_t units = decode_utf8(&code, p);
        if (units <= 0) break;
        width += glyph_advance(code);
        p += units;
    }
    return width * size / FONT_PX;
}

bool ui_has_japanese(void) { return g_has_japanese; }

/* Kana and CJK ideographs are encoded with UTF-8 lead bytes E3-E9. */
static bool contains_japanese(const char *text)
{
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p)
        if (*p >= 0xE3 && *p <= 0xE9) return true;
    return false;
}

/* The system font's glyphs are 30 px. Kanji shrunk to 11-13 px (a 0.37-0.43
 * scale) blur their strokes together; exactly half size samples cleanly.
 * Small Japanese text is therefore drawn at 15 px, and full-size text
 * snaps to 30 px. Returns the size to use and how far to move y so the
 * text stays centred where the caller placed it. */
static float japanese_size(const char *text, float size, float *dy)
{
    *dy = 0.0f;
    if (!contains_japanese(text)) return size;
    float clean = size;
    if (size >= 10.5f && size < 15.0f) clean = 15.0f;
    else if (size >= 24.0f && size < 30.0f) clean = 30.0f;
    *dy = (size - clean) / 2.0f;
    return clean;
}

float ui_text(float x, float y, float size, u32 color, UiAlign align, const char *text)
{
    if (!text || !text[0]) return 0.0f;
    if (!g_has_japanese && contains_japanese(text)) return 0.0f;
    float dy;
    size = japanese_size(text, size, &dy);
    y += dy;
    C2D_Text parsed;
    C2D_TextParse(&parsed, g_text, text);
    C2D_TextOptimize(&parsed);
    const float scale = size / FONT_PX;
    float width = 0.0f, height = 0.0f;
    C2D_TextGetDimensions(&parsed, scale, scale, &width, &height);
    if (align == UI_ALIGN_CENTER) x -= width / 2.0f;
    else if (align == UI_ALIGN_RIGHT) x -= width;
    C2D_DrawText(&parsed, C2D_WithColor, floorf(x + 0.5f), floorf(y + 0.5f), 0.0f,
                 scale, scale, color);
    return width;
}

float ui_textf(float x, float y, float size, u32 color, UiAlign align, const char *format, ...)
{
    char buffer[256];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    return ui_text(x, y, size, color, align, buffer);
}

static size_t utf8_back(const char *text, size_t length)
{
    while (length > 0 && (((unsigned char)text[length - 1]) & 0xC0) == 0x80) --length;
    return length > 0 ? length - 1 : 0;
}

void ui_text_fit(float x, float y, float size, u32 color, UiAlign align,
                 float max_width, const char *text)
{
    if (!text) return;
    if (ui_text_width(text, size) <= max_width) {
        ui_text(x, y, size, color, align, text);
        return;
    }
    char buffer[256];
    size_t length = strlen(text);
    if (length > sizeof(buffer) - 4) length = sizeof(buffer) - 4;
    const float ellipsis = ui_text_width("...", size);
    while (length > 0) {
        length = utf8_back(text, length);
        memcpy(buffer, text, length);
        buffer[length] = '\0';
        if (ui_text_width(buffer, size) + ellipsis <= max_width) break;
    }
    while (length > 0 && buffer[length - 1] == ' ') buffer[--length] = '\0';
    memcpy(buffer + length, "...", 4);
    ui_text(x, y, size, color, align, buffer);
}

int ui_text_wrap(float x, float y, float size, u32 color, UiAlign align,
                 float max_width, int max_lines, float line_height, const char *text)
{
    if (!text) return 0;
    int lines = 0;
    const char *cursor = text;
    char line[256];
    while (*cursor && lines < max_lines) {
        while (*cursor == ' ') ++cursor;
        if (!*cursor) break;
        size_t best = 0;
        /* Grow word by word until the next word would overflow. */
        for (;;) {
            size_t next = best;
            while (cursor[next] == ' ') ++next;
            if (!cursor[next] || cursor[next] == '\n') break;
            while (cursor[next] && cursor[next] != ' ' && cursor[next] != '\n') ++next;
            if (next >= sizeof(line)) break;
            memcpy(line, cursor, next);
            line[next] = '\0';
            if (ui_text_width(line, size) > max_width) break;
            best = next;
        }
        if (best == 0) {
            /* A single word wider than the line: hard-break it. */
            while (cursor[best] && cursor[best] != '\n' && best < sizeof(line) - 2) {
                memcpy(line, cursor, best + 1);
                line[best + 1] = '\0';
                if (ui_text_width(line, size) > max_width && best > 0) break;
                ++best;
            }
        }
        const bool last = lines == max_lines - 1 && cursor[best] && cursor[best] != '\n';
        memcpy(line, cursor, best);
        line[best] = '\0';
        if (last) ui_text_fit(x, y + lines * line_height, size, color, align, max_width, cursor);
        else ui_text(x, y + lines * line_height, size, color, align, line);
        cursor += best;
        if (*cursor == '\n') ++cursor;
        ++lines;
    }
    return lines;
}

void ui_label(float x, float y, float size, u32 color, UiAlign align, const char *text)
{
    /* Hand-spaced capitals: one code point at a time with a tracking gap. */
    const float tracking = size * 0.10f;
    char upper[128];
    size_t n = 0;
    for (const char *p = text; *p && n < sizeof(upper) - 1; ++p)
        upper[n++] = (*p >= 'a' && *p <= 'z') ? (char)(*p - 32) : *p;
    upper[n] = '\0';
    float width = 0.0f;
    char glyph[5];
    for (const char *p = upper; *p;) {
        size_t len = 1;
        while ((((unsigned char)p[len]) & 0xC0) == 0x80 && len < 4) ++len;
        memcpy(glyph, p, len);
        glyph[len] = '\0';
        p += len;
        width += ui_text_width(glyph, size) + (*p ? tracking : 0.0f);
    }
    if (align == UI_ALIGN_CENTER) x -= width / 2.0f;
    else if (align == UI_ALIGN_RIGHT) x -= width;
    for (const char *p = upper; *p;) {
        size_t len = 1;
        while ((((unsigned char)p[len]) & 0xC0) == 0x80 && len < 4) ++len;
        memcpy(glyph, p, len);
        glyph[len] = '\0';
        p += len;
        x += ui_text_width(glyph, size) + tracking;
        ui_text(x - ui_text_width(glyph, size) - tracking, y, size, color, UI_ALIGN_LEFT, glyph);
    }
}

/* ---- Components ---------------------------------------------------------- */

void ui_panel(UiRect r, u32 accent)
{
    ui_rect_r(r, UI_SURFACE);
    ui_outline(r.x, r.y, r.w, r.h, 1.0f, UI_LINE);
    ui_rect(r.x + r.w / 2.0f - 12.0f, r.y, 24.0f, 1.0f, accent);
}

float ui_pill(float x, float y, u32 color, UiAlign align, const char *text)
{
    const float w = ui_text_width(text, 11.0f) + 12.0f;
    if (align == UI_ALIGN_CENTER) x -= w / 2.0f;
    else if (align == UI_ALIGN_RIGHT) x -= w;
    ui_rounded(x, y, w, 16.0f, 3.0f, color);
    ui_rounded(x + 1.0f, y + 1.0f, w - 2.0f, 14.0f, 2.5f, UI_BG);
    ui_text(x + w / 2.0f, y + 1.5f, 11.0f, color, UI_ALIGN_CENTER, text);
    return w;
}

void ui_dots(float cx, float cy, unsigned count, unsigned active, u32 on, u32 off)
{
    const float gap = 9.0f;
    float x = cx - gap * (float)(count - 1) / 2.0f;
    for (unsigned i = 0; i < count; ++i, x += gap) {
        if (i == active) ui_rounded(x - 3.5f, cy - 2.0f, 7.0f, 4.0f, 2.0f, on);
        else ui_circle(x, cy, 1.6f, off);
    }
}

void ui_section(float x, float y, float w, const char *jp, const char *en)
{
    float cursor = x;
    if (g_has_japanese) {
        cursor += ui_text(cursor, y, 11.0f, UI_ACCENT, UI_ALIGN_LEFT, jp) + 6.0f;
        ui_label(cursor, y + 1.0f, 11.0f, UI_TEXT_FAINT, UI_ALIGN_LEFT, en);
    } else {
        ui_label(cursor, y + 1.0f, 11.0f, UI_ACCENT, UI_ALIGN_LEFT, en);
    }
    ui_hline(x, y + 15.0f, w, UI_LINE);
}

void ui_seal(float x, float y, float size)
{
    /* Hanko: celadon stamp with 霞 (kasumi, mist) cut out of it. */
    /* GPU downscaling without mipmaps aliases badly, so small seals use
     * copies pre-shrunk with a high-quality filter. */
    const UiImage art = size <= 20.0f ? UI_IMAGE_SEAL_16 : size <= 48.0f ? UI_IMAGE_SEAL_40 : UI_IMAGE_SEAL;
    const float native = art == UI_IMAGE_SEAL_16 ? 16.0f : art == UI_IMAGE_SEAL_40 ? 40.0f : 64.0f;
    if (ui_image(art, x, y, size / native, 1.0f)) return;
    ui_rounded(x, y, size, size, size * 0.16f, UI_ACCENT);
    ui_outline(x + size * 0.09f, y + size * 0.09f, size * 0.82f, size * 0.82f,
               size > 30 ? 2.0f : 1.0f, UI_BG);
    if (g_has_japanese)
        ui_text(x + size / 2.0f, y + size * 0.14f, size * 0.72f, UI_BG, UI_ALIGN_CENTER, "霞");
    else
        ui_ring(x + size / 2.0f, y + size / 2.0f, size * 0.26f, size > 30 ? 3.0f : 1.5f,
                UI_BG, UI_ACCENT);
}

void ui_enso(float cx, float cy, float r, u32 color)
{
    /* A brush circle that chases itself: a bright head and a fading tail. */
    const unsigned dots = 36;
    const float phase = (float)(ui_ticks() % 1400) / 1400.0f * 2.0f * (float)M_PI;
    /* The brush ring in the art has a radius of about 58 of its 128 px. */
    if (ui_image_rotated(UI_IMAGE_ENSO, cx, cy, r / 58.0f, phase, 1.0f)) return;
    for (unsigned i = 0; i < dots; ++i) {
        const float t = (float)i / (float)dots;
        const float angle = phase - t * 1.6f * (float)M_PI;
        const float thickness = 2.6f - t * 1.6f;
        const u8 alpha = (u8)(255.0f * (1.0f - t) * (1.0f - t));
        ui_circle(cx + cosf(angle) * r, cy + sinf(angle) * r, thickness,
                  ui_with_alpha(color, alpha));
    }
    /* Faint guide circle underneath the brush stroke. */
    for (unsigned i = 0; i < 72; ++i) {
        const float angle = (float)i / 72.0f * 2.0f * (float)M_PI;
        ui_circle(cx + cosf(angle) * r, cy + sinf(angle) * r, 0.8f, ui_with_alpha(color, 0x30));
    }
}

void ui_seigaiha(float x, float y, float w, float h, float radius, u32 line, u32 fill)
{
    /* 青海波: overlapping concentric half-circles, drawn back row to front. */
    const float row_step = radius * 0.5f;
    const int rows = (int)(h / row_step) + 2;
    for (int row = 0; row < rows; ++row) {
        const float cy = y + row * row_step + radius;
        const float offset = (row & 1) ? radius : 0.0f;
        for (float cx = x - radius + offset; cx < x + w + radius; cx += radius * 2.0f) {
            for (int ring = 0; ring < 4; ++ring) {
                const float rr = radius * (1.0f - ring * 0.24f);
                ui_circle(cx, cy, rr, line);
                ui_circle(cx, cy, rr - 1.2f, fill);
            }
        }
    }
}

void ui_wifi_icon(float x, float y, unsigned bars, u32 on, u32 off)
{
    for (unsigned i = 0; i < 3; ++i) {
        const float bar_h = 4.0f + i * 3.0f;
        ui_rect(x + i * 5.0f, y + 10.0f - bar_h, 3.0f, bar_h, i < bars ? on : off);
    }
}

void ui_battery_icon(float x, float y, unsigned level, bool charging)
{
    ui_outline(x, y, 20.0f, 10.0f, 1.0f, UI_TEXT_DIM);
    ui_rect(x + 20.0f, y + 3.0f, 2.0f, 4.0f, UI_TEXT_DIM);
    if (level > 5) level = 5;
    const u32 color = charging ? UI_ACCENT : level <= 1 ? UI_DANGER : UI_TEXT;
    ui_rect(x + 2.0f, y + 2.0f, 16.0f * (float)level / 5.0f, 6.0f, color);
}

float ui_button_chip(float x, float y, const char *button, u32 color)
{
    /* Face buttons are circles; shoulders and system buttons are pills. */
    const bool round = strlen(button) == 1 && strchr("ABXY", button[0]);
    if (round) {
        ui_ring(x + 7.5f, y + 7.5f, 7.5f, 1.2f, color, UI_BG);
        ui_text(x + 7.5f, y + 1.5f, 12.0f, color, UI_ALIGN_CENTER, button);
        return 15.0f;
    }
    const float width = ui_text_width(button, 10.0f) + 10.0f;
    ui_rounded(x, y + 0.5f, width, 14.0f, 7.0f, color);
    ui_rounded(x + 1.0f, y + 1.5f, width - 2.0f, 12.0f, 6.0f, UI_BG);
    ui_text(x + width / 2.0f, y + 2.0f, 10.0f, color, UI_ALIGN_CENTER, button);
    return width;
}

float ui_hint(float x, float y, const char *button, const char *label, bool measure_only)
{
    const bool round = strlen(button) == 1 && strchr("ABXY", button[0]);
    const float chip = round ? 15.0f : ui_text_width(button, 10.0f) + 10.0f;
    const float total = chip + 5.0f + ui_text_width(label, 12.0f);
    if (!measure_only) {
        ui_button_chip(x, y, button, UI_TEXT_DIM);
        ui_text(x + chip + 5.0f, y + 1.0f, 12.0f, UI_TEXT, UI_ALIGN_LEFT, label);
    }
    return total;
}

void ui_hint_row(float center_x, float y, const char *const *pairs)
{
    const float gap = 16.0f;
    float total = 0.0f;
    unsigned count = 0;
    for (const char *const *p = pairs; p[0] && p[1]; p += 2, ++count)
        total += ui_hint(0, 0, p[0], p[1], true);
    if (!count) return;
    total += gap * (float)(count - 1);
    float x = center_x - total / 2.0f;
    for (const char *const *p = pairs; p[0] && p[1]; p += 2)
        x += ui_hint(x, y, p[0], p[1], false) + gap;
}

void ui_button(UiRect r, const char *label, const char *jp, UiButtonStyle style, bool pressed)
{
    u32 fill = UI_SURFACE, border = UI_LINE_STRONG, text = UI_TEXT, sub = UI_TEXT_FAINT;
    if (style == UI_BUTTON_PRIMARY) {
        fill = UI_ACCENT; border = UI_ACCENT; text = UI_BG; sub = UI_ACCENT_INK;
    } else if (style == UI_BUTTON_DANGER) {
        fill = UI_DANGER_DEEP; border = UI_DANGER; text = UI_TEXT; sub = UI_DANGER;
    } else if (style == UI_BUTTON_ACTIVE) {
        fill = UI_RAISED; border = UI_ACCENT; text = UI_TEXT; sub = UI_ACCENT;
    }
    if (pressed) fill = ui_mix(fill, UI_TEXT, 0.18f);
    ui_rect_r(r, fill);
    ui_outline(r.x, r.y, r.w, r.h, 1.0f, border);
    /* Corner ticks echo shoji frames. */
    if (style != UI_BUTTON_PRIMARY) {
        ui_rect(r.x, r.y, 5.0f, 2.0f, border == UI_LINE_STRONG ? UI_TEXT_FAINT : border);
        ui_rect(r.x + r.w - 5.0f, r.y + r.h - 2.0f, 5.0f, 2.0f,
                border == UI_LINE_STRONG ? UI_TEXT_FAINT : border);
    }
    const float label_size = r.h >= 40.0f ? 14.0f : 12.0f;
    const float jp_size = r.h >= 40.0f ? 12.0f : 11.0f;
    if (jp && jp[0] && r.h >= 30.0f && (g_has_japanese || !contains_japanese(jp))) {
        const float block = label_size + 2.0f + jp_size;
        const float top = r.y + (r.h - block) / 2.0f;
        ui_text_fit(r.x + r.w / 2.0f, top, label_size, text, UI_ALIGN_CENTER, r.w - 6.0f, label);
        ui_text_fit(r.x + r.w / 2.0f, top + label_size + 2.0f, jp_size, sub,
                    UI_ALIGN_CENTER, r.w - 6.0f, jp);
    } else {
        ui_text_fit(r.x + r.w / 2.0f, r.y + (r.h - label_size) / 2.0f - 1.0f, label_size,
                    text, UI_ALIGN_CENTER, r.w - 6.0f, label);
    }
}

bool ui_hit(UiRect r, int x, int y)
{
    return (float)x >= r.x && (float)x < r.x + r.w && (float)y >= r.y && (float)y < r.y + r.h;
}

void ui_ps_triangle(float cx, float cy, float s, u32 color)
{
    const float h = s * 0.87f;
    ui_triangle(cx, cy - h * 0.62f, cx - s / 2, cy + h * 0.38f, cx + s / 2, cy + h * 0.38f, color);
    const float inner = s - 5.0f;
    const float ih = inner * 0.87f;
    ui_triangle(cx, cy - ih * 0.62f + 0.6f, cx - inner / 2, cy + ih * 0.38f + 0.6f,
                cx + inner / 2, cy + ih * 0.38f + 0.6f, UI_BG);
}

void ui_ps_circle(float cx, float cy, float s, u32 color)
{
    ui_ring(cx, cy, s / 2, 2.0f, color, UI_BG);
}

void ui_ps_cross(float cx, float cy, float s, u32 color)
{
    const float d = s * 0.38f;
    ui_line(cx - d, cy - d, cx + d, cy + d, 2.2f, color);
    ui_line(cx - d, cy + d, cx + d, cy - d, 2.2f, color);
}

void ui_ps_square(float cx, float cy, float s, u32 color)
{
    const float side = s * 0.8f;
    ui_outline(cx - side / 2, cy - side / 2, side, side, 2.0f, color);
}

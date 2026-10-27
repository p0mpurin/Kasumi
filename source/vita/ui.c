#include "ui.h"
#include "ui_internal.h"
#include "ui_vita.h"
#include "video_vita.h"
#include "diagnostic.h"
#include "i18n.h"
#include "platform.h"

#include <psp2/gxm.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <vita2d.h>

/* vita2d draws the primitives ui_common.c builds everything from. Art is the
 * same PNGs the 3DS converts to t3x, linked in as they are. */

/* ---- Regions ------------------------------------------------------------- */

static float g_ox, g_oy, g_scale = 1.0f;
static float g_dx, g_dy;
static bool g_on_bottom, g_backdrop_drawn;

#define TX(x) (g_ox + ((x) + g_dx) * g_scale)
#define TY(y) (g_oy + ((y) + g_dy) * g_scale)
#define TS(v) ((v) * g_scale)

void ui_region(float ox, float oy, float scale, float w, float h, bool lower)
{
    g_ox = ox;
    g_oy = oy;
    g_scale = scale;
    g_dx = g_dy = 0.0f;
    g_on_bottom = lower;
    g_backdrop_drawn = false;
    vita2d_enable_clipping();
    vita2d_set_clip_rectangle((int)ox, (int)oy, (int)(ox + w * scale + 0.5f), (int)(oy + h * scale + 0.5f));
}

void ui_screen(void)
{
    g_ox = g_oy = g_dx = g_dy = 0.0f;
    g_scale = 1.0f;
    g_on_bottom = false;
    vita2d_disable_clipping();
}

void ui_offset(float dx, float dy)
{
    g_dx = dx;
    g_dy = dy;
}

/* ---- Art ----------------------------------------------------------------- */

#define PNG(name) extern const unsigned char _binary_##name##_png_start[];
PNG(hero) PNG(mist) PNG(enso) PNG(seal) PNG(seal40) PNG(seal16) PNG(lantern) PNG(discord)
PNG(sec_controls) PNG(sec_picture) PNG(sec_sound) PNG(sec_network) PNG(sec_system) PNG(sec_account)
PNG(no_cover) PNG(grid) PNG(look_ring) PNG(look_dot) PNG(svc_gfn) PNG(svc_xbox) PNG(svc_steam)
PNG(icon_gfn) PNG(icon_xbox) PNG(icon_steam) PNG(cover_bigpicture) PNG(cover_desktop) PNG(hub_backdrop)
PNG(bg_seiji) PNG(bg_sakura) PNG(bg_kin) PNG(bg_ai) PNG(bg_fuji)
PNG(bg_beni) PNG(bg_matcha) PNG(bg_kaki) PNG(bg_sumi) PNG(bg_shiro)
PNG(glass_seiji) PNG(glass_sakura) PNG(glass_kin) PNG(glass_ai) PNG(glass_fuji)
PNG(glass_beni) PNG(glass_matcha) PNG(glass_kaki) PNG(glass_sumi) PNG(glass_shiro)

static const unsigned char *const ART[UI_IMAGE_COUNT] = {
    _binary_hero_png_start, _binary_mist_png_start, _binary_enso_png_start, _binary_seal_png_start,
    _binary_seal40_png_start, _binary_seal16_png_start, _binary_lantern_png_start, _binary_discord_png_start,
    _binary_sec_controls_png_start, _binary_sec_picture_png_start, _binary_sec_sound_png_start,
    _binary_sec_network_png_start, _binary_sec_system_png_start, _binary_sec_account_png_start,
    _binary_no_cover_png_start, _binary_grid_png_start, _binary_look_ring_png_start, _binary_look_dot_png_start,
    _binary_svc_gfn_png_start, _binary_svc_xbox_png_start, _binary_svc_steam_png_start,
    _binary_icon_gfn_png_start, _binary_icon_xbox_png_start, _binary_icon_steam_png_start,
    _binary_cover_bigpicture_png_start, _binary_cover_desktop_png_start, _binary_hub_backdrop_png_start,
};

static const unsigned char *const WALLPAPERS[UI_THEME_COUNT][2] = {
    { _binary_bg_seiji_png_start, _binary_glass_seiji_png_start },
    { _binary_bg_sakura_png_start, _binary_glass_sakura_png_start },
    { _binary_bg_kin_png_start, _binary_glass_kin_png_start },
    { _binary_bg_ai_png_start, _binary_glass_ai_png_start },
    { _binary_bg_fuji_png_start, _binary_glass_fuji_png_start },
    { _binary_bg_beni_png_start, _binary_glass_beni_png_start },
    { _binary_bg_matcha_png_start, _binary_glass_matcha_png_start },
    { _binary_bg_kaki_png_start, _binary_glass_kaki_png_start },
    { _binary_bg_sumi_png_start, _binary_glass_sumi_png_start },
    { _binary_bg_shiro_png_start, _binary_glass_shiro_png_start },
};

static vita2d_texture *g_art[UI_IMAGE_COUNT];
/* White copies of the celadon art, so other themes can recolour it: vita2d
 * tints by multiplying, citro2d by replacing. */
static vita2d_texture *g_mask[UI_IMAGE_COUNT];
static vita2d_texture *g_backdrop, *g_glass;
static int g_backdrop_theme = -1;

static bool recolourable(UiImage image)
{
    return image == UI_IMAGE_SEAL || image == UI_IMAGE_SEAL_40 || image == UI_IMAGE_SEAL_16 ||
           image == UI_IMAGE_ENSO || image == UI_IMAGE_LANTERN;
}

static vita2d_texture *white_copy(const vita2d_texture *source)
{
    const unsigned w = vita2d_texture_get_width(source), h = vita2d_texture_get_height(source);
    if (vita2d_texture_get_format(source) != SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR) return NULL;
    vita2d_texture *copy = vita2d_create_empty_texture(w, h);
    if (!copy) return NULL;
    const unsigned in_stride = vita2d_texture_get_stride(source) / 4, out_stride = vita2d_texture_get_stride(copy) / 4;
    const unsigned *in = vita2d_texture_get_datap(source);
    unsigned *out = vita2d_texture_get_datap(copy);
    for (unsigned y = 0; y < h; ++y)
        for (unsigned x = 0; x < w; ++x) out[y * out_stride + x] = in[y * in_stride + x] | 0x00FFFFFFu;
    vita2d_texture_set_filters(copy, SCE_GXM_TEXTURE_FILTER_LINEAR, SCE_GXM_TEXTURE_FILTER_LINEAR);
    return copy;
}

/* A replaced wallpaper is freed after the next frame, once the GPU is done. */
static vita2d_texture *g_retired[2];

void ui_load_wallpaper(UiTheme theme)
{
    if ((int)theme == g_backdrop_theme || theme >= UI_THEME_COUNT) return;
    for (int i = 0; i < 2; ++i)
        if (g_retired[i]) vita2d_free_texture(g_retired[i]);
    g_retired[0] = g_backdrop;
    g_retired[1] = g_glass;
    g_backdrop = vita2d_load_PNG_buffer(WALLPAPERS[theme][0]);
    g_glass = vita2d_load_PNG_buffer(WALLPAPERS[theme][1]);
    if (g_backdrop) vita2d_texture_set_filters(g_backdrop, SCE_GXM_TEXTURE_FILTER_LINEAR, SCE_GXM_TEXTURE_FILTER_LINEAR);
    if (g_glass) vita2d_texture_set_filters(g_glass, SCE_GXM_TEXTURE_FILTER_LINEAR, SCE_GXM_TEXTURE_FILTER_LINEAR);
    g_backdrop_theme = (int)theme;
}

bool ui_glass_ready(void) { return g_on_bottom && g_glass && g_backdrop_drawn; }

static u32 white(float alpha)
{
    const int a = (int)(alpha * 255.0f + 0.5f);
    return UI_RGBA(0xFF, 0xFF, 0xFF, a < 0 ? 0 : a > 255 ? 255 : a);
}

/* Part of a texture (in its pixels) drawn at (x, y) in view units, scaled. */
static void draw_part(const vita2d_texture *tex, float x, float y, float tx, float ty, float tw, float th,
                      float sx, float sy, u32 tint)
{
    vita2d_draw_texture_tint_part_scale(tex, TX(x), TY(y), tx, ty, tw, th, TS(sx), TS(sy), tint);
}

bool ui_backdrop(void)
{
    if (!g_backdrop) return false;
    draw_part(g_backdrop, 0.0f, 0.0f, 0.0f, 0.0f, vita2d_texture_get_width(g_backdrop),
              vita2d_texture_get_height(g_backdrop), 1.0f, 1.0f, white(1.0f));
    g_backdrop_drawn = true;
    return true;
}

/* The glass image is the wallpaper at half size, blurred: take the part
 * under r and draw it back at twice the size, so it lines up. */
bool ui_glass(UiRect r)
{
    if (!g_glass || !g_on_bottom || !g_backdrop_drawn || r.w < 2.0f || r.h < 2.0f) return false;
    draw_part(g_glass, r.x, r.y, r.x / 2.0f, r.y / 2.0f, r.w / 2.0f, r.h / 2.0f, 2.0f, 2.0f, white(1.0f));
    return true;
}

bool ui_wallpaper(UiRect r)
{
    if (!g_backdrop || r.w < 1.0f || r.h < 1.0f) return false;
    draw_part(g_backdrop, r.x, r.y, r.x, r.y, r.w, r.h, 1.0f, 1.0f, white(1.0f));
    return true;
}

/* The 3DS filters its video picture on the GPU; on the Vita the decoder's
 * own output is shown as it is. */
void ui_set_video_look(unsigned sharpen, unsigned color)
{
    (void)sharpen;
    (void)color;
}

static float tex_w(UiImage image) { return (float)vita2d_texture_get_width(g_art[image]); }
static float tex_h(UiImage image) { return (float)vita2d_texture_get_height(g_art[image]); }

bool ui_image(UiImage image, float x, float y, float scale, float alpha)
{
    if (image >= UI_IMAGE_COUNT || !g_art[image]) return false;
    /* The seal, ensō and lantern art is celadon with transparent cut-outs,
     * so other themes recolour it wholesale. */
    const bool recolour = ui_theme() != UI_THEME_SEIJI && recolourable(image) && g_mask[image];
    const vita2d_texture *tex = recolour ? g_mask[image] : g_art[image];
    const u32 tint = recolour ? ui_with_alpha(g_ui_accent, (u8)(alpha * 255.0f)) : white(alpha);
    draw_part(tex, floorf(x + 0.5f), floorf(y + 0.5f), 0.0f, 0.0f, tex_w(image), tex_h(image), scale, scale, tint);
    return true;
}

bool ui_image_rotated(UiImage image, float cx, float cy, float scale, float angle, float alpha)
{
    if (image >= UI_IMAGE_COUNT || !g_art[image]) return false;
    const bool recolour = ui_theme() != UI_THEME_SEIJI && image == UI_IMAGE_ENSO && g_mask[image];
    const vita2d_texture *tex = recolour ? g_mask[image] : g_art[image];
    const u32 tint = recolour ? ui_with_alpha(g_ui_accent, (u8)(alpha * 255.0f)) : white(alpha);
    vita2d_draw_texture_tint_scale_rotate(tex, TX(cx), TY(cy), TS(scale), TS(scale), angle, tint);
    return true;
}

/* A fade from top to bottom, drawn as bands (vita2d tints a whole draw in
 * one colour). flip draws the image upside down, a reflection. */
#define FADE_BANDS 12

static void draw_fade(const vita2d_texture *tex, float x, float y, float w, float h, float tx, float ty, float tw,
                      float th, float alpha_top, float alpha_bottom, bool flip)
{
    if (alpha_top >= 1.0f && alpha_bottom >= 1.0f && !flip) {
        draw_part(tex, x, y, tx, ty, tw, th, w / tw, h / th, white(1.0f));
        return;
    }
    for (int band = 0; band < FADE_BANDS; ++band) {
        const float t = ((float)band + 0.5f) / FADE_BANDS;
        const float alpha = alpha_top + (alpha_bottom - alpha_top) * t;
        if (alpha <= 0.004f) continue;
        const float dy = y + h * (float)band / FADE_BANDS, dh = h / FADE_BANDS;
        const float source = flip ? ty + th * (float)(FADE_BANDS - 1 - band) / FADE_BANDS : ty + th * (float)band / FADE_BANDS;
        const float sh = th / FADE_BANDS;
        if (flip) draw_part(tex, x, dy + dh, tx, source, tw, sh, w / tw, -dh / sh, white(alpha));
        else draw_part(tex, x, dy, tx, source, tw, sh, w / tw, dh / sh, white(alpha));
    }
}

bool ui_image_fade(UiImage image, float x, float y, float w, float h, float alpha_top, float alpha_bottom,
                   bool flip)
{
    if (image >= UI_IMAGE_COUNT || !g_art[image] || w < 1.0f || h < 1.0f) return false;
    draw_fade(g_art[image], x, y, w, h, 0.0f, 0.0f, tex_w(image), tex_h(image), alpha_top, alpha_bottom, flip);
    return true;
}

bool ui_image_part(UiImage image, float x, float y, float w, float h, float u0, float v0, float u1, float v1,
                   float alpha)
{
    if (image >= UI_IMAGE_COUNT || !g_art[image] || w < 1.0f || h < 1.0f) return false;
    const float tw = tex_w(image), th = tex_h(image);
    const float pw = tw * (u1 - u0), ph = th * (v1 - v0);
    if (pw <= 0.0f || ph <= 0.0f) return false;
    draw_part(g_art[image], x, y, tw * u0, th * v0, pw, ph, w / pw, h / ph, white(alpha));
    return true;
}

bool ui_icon(UiImage image, float cx, float cy, float size, u32 color)
{
    if (image >= UI_IMAGE_COUNT || !g_art[image]) return false;
    const float scale = size / tex_w(image);
    draw_part(g_art[image], cx - size / 2, cy - size / 2, 0.0f, 0.0f, tex_w(image), tex_h(image), scale, scale, color);
    return true;
}

bool ui_image_tint(UiImage image, float x, float y, float scale, u32 color)
{
    if (image >= UI_IMAGE_COUNT || !g_art[image]) return false;
    const vita2d_texture *tex = recolourable(image) && g_mask[image] ? g_mask[image] : g_art[image];
    draw_part(tex, floorf(x + 0.5f), floorf(y + 0.5f), 0.0f, 0.0f, tex_w(image), tex_h(image), scale, scale, color);
    return true;
}

/* A tiling texture anchored to the view, so it lines up wherever it is
 * drawn: the texture repeats, and its coordinates are the view's. */
void ui_texture(UiImage image, float x, float y, float w, float h, u32 color)
{
    if (image >= UI_IMAGE_COUNT || !g_art[image] || w < 1.0f || h < 1.0f) return;
    draw_part(g_art[image], floorf(x + 0.5f), floorf(y + 0.5f), x, y, w, h, 1.0f, 1.0f, color);
}

/* ---- Pictures ------------------------------------------------------------ */

struct UiPicture {
    vita2d_texture *tex;
    unsigned width, height;
};

UiPicture *ui_picture_create(unsigned width, unsigned height)
{
    UiPicture *picture = calloc(1, sizeof(*picture));
    if (!picture) return NULL;
    picture->tex = vita2d_create_empty_texture(width, height);
    if (!picture->tex) {
        free(picture);
        return NULL;
    }
    vita2d_texture_set_filters(picture->tex, SCE_GXM_TEXTURE_FILTER_LINEAR, SCE_GXM_TEXTURE_FILTER_LINEAR);
    return picture;
}

bool ui_picture_set(UiPicture *picture, const u32 *pixels, unsigned width, unsigned height, unsigned stride)
{
    if (width > vita2d_texture_get_width(picture->tex) || height > vita2d_texture_get_height(picture->tex)) return false;
    /* Outside a frame: the GPU may still be drawing the old pixels. */
    vita2d_wait_rendering_done();
    u32 *out = vita2d_texture_get_datap(picture->tex);
    const unsigned out_stride = vita2d_texture_get_stride(picture->tex) / 4;
    /* 0xRRGGBBAA words to the texture's 0xAABBGGRR. */
    for (unsigned y = 0; y < height; ++y)
        for (unsigned x = 0; x < width; ++x) out[y * out_stride + x] = __builtin_bswap32(pixels[y * stride + x]);
    picture->width = width;
    picture->height = height;
    return true;
}

void ui_picture_free(UiPicture *picture)
{
    if (!picture) return;
    vita2d_wait_rendering_done();
    vita2d_free_texture(picture->tex);
    free(picture);
}

bool ui_picture_draw(const UiPicture *picture, float x, float y, float scale, float alpha)
{
    if (!picture || !picture->width) return false;
    draw_part(picture->tex, x, y, 0.0f, 0.0f, picture->width, picture->height, scale, scale, white(alpha));
    return true;
}

bool ui_picture_draw_fade(const UiPicture *picture, float x, float y, float scale, float alpha_top,
                          float alpha_bottom, bool flip)
{
    if (!picture || !picture->width) return false;
    draw_fade(picture->tex, x, y, picture->width * scale, picture->height * scale, 0.0f, 0.0f, picture->width,
              picture->height, alpha_top, alpha_bottom, flip);
    return true;
}

/* ---- Shapes -------------------------------------------------------------- */

static vita2d_color_vertex *vertices(unsigned count)
{
    return vita2d_pool_memalign(count * sizeof(vita2d_color_vertex), sizeof(vita2d_color_vertex));
}

static void vertex(vita2d_color_vertex *v, float x, float y, u32 color)
{
    v->x = TX(x);
    v->y = TY(y);
    v->z = 0.5f;
    v->color = color;
}

void ui_rect(float x, float y, float w, float h, u32 color)
{
    if (w > 0 && h > 0) vita2d_draw_rectangle(TX(x), TY(y), TS(w), TS(h), color);
}

void ui_gradient_corners(float x, float y, float w, float h, u32 top_left, u32 top_right,
                         u32 bottom_left, u32 bottom_right)
{
    if (w <= 0 || h <= 0) return;
    vita2d_color_vertex *v = vertices(4);
    if (!v) return;
    vertex(&v[0], x, y, top_left);
    vertex(&v[1], x + w, y, top_right);
    vertex(&v[2], x, y + h, bottom_left);
    vertex(&v[3], x + w, y + h, bottom_right);
    vita2d_draw_array(SCE_GXM_PRIMITIVE_TRIANGLE_STRIP, v, 4);
}

void ui_gradient(float x, float y, float w, float h, u32 top, u32 bottom)
{
    ui_gradient_corners(x, y, w, h, top, top, bottom, bottom);
}

void ui_circle(float cx, float cy, float r, u32 color)
{
    if (r > 0) vita2d_draw_fill_circle(TX(cx), TY(cy), TS(r), color);
}

void ui_triangle(float x0, float y0, float x1, float y1, float x2, float y2, u32 color)
{
    vita2d_color_vertex *v = vertices(3);
    if (!v) return;
    vertex(&v[0], x0, y0, color);
    vertex(&v[1], x1, y1, color);
    vertex(&v[2], x2, y2, color);
    vita2d_draw_array(SCE_GXM_PRIMITIVE_TRIANGLES, v, 3);
}

void ui_line(float x0, float y0, float x1, float y1, float thickness, u32 color)
{
    const float dx = x1 - x0, dy = y1 - y0, length = sqrtf(dx * dx + dy * dy);
    if (length <= 0.0f) return;
    /* A quad thickness wide along the line. */
    const float nx = -dy / length * thickness / 2.0f, ny = dx / length * thickness / 2.0f;
    vita2d_color_vertex *v = vertices(4);
    if (!v) return;
    vertex(&v[0], x0 + nx, y0 + ny, color);
    vertex(&v[1], x1 + nx, y1 + ny, color);
    vertex(&v[2], x0 - nx, y0 - ny, color);
    vertex(&v[3], x1 - nx, y1 - ny, color);
    vita2d_draw_array(SCE_GXM_PRIMITIVE_TRIANGLE_STRIP, v, 4);
}

/* ---- Text ---------------------------------------------------------------- */

/* The system fonts: Latin for Latin letters, the Japanese font for the
 * rest (kana, kanji, symbols). Sizes are glyph heights in view pixels, like
 * the 3DS's; the font's own line height becomes that size. */
static vita2d_pgf *g_font;
static float g_font_line = 20.0f;
static bool g_has_japanese;
/* Where the baseline sits below the top of a line, as a share of it. */
#define FONT_ASCENT 0.80f

static int latin(unsigned int c) { return c < 0x0250 || (c >= 0x2000 && c < 0x2070); }

/* The font's glyphs are drawn at one size and stretched: near that size,
 * text is drawn at exactly it, where every glyph is sharp. Measures use the
 * same scale, so centred and right-aligned text still lines up. */
static float glyph_scale(float size)
{
    const float scale = TS(size) / g_font_line;
    return scale > 0.9f && scale < 1.1f ? 1.0f : scale;
}

float ui_text_width(const char *text, float size)
{
    if (!text || !g_font) return 0.0f;
    text = i18n_text(text);
    /* One line, like the 3DS measure. */
    char line[256];
    size_t n = 0;
    while (text[n] && text[n] != '\n' && n < sizeof(line) - 1) {
        line[n] = text[n];
        ++n;
    }
    line[n] = '\0';
    if (!n) return 0.0f;
    /* Measured at a large size and scaled down: whole-pixel rounding at
     * small sizes would make lines that fit come out "too wide". */
    const float measure = 4.0f;
    return (float)vita2d_pgf_text_width(g_font, measure, line) / measure * glyph_scale(size) / g_scale;
}

bool ui_has_japanese(void) { return g_has_japanese; }

float ui_text(float x, float y, float size, u32 color, UiAlign align, const char *text)
{
    if (!text || !text[0] || !g_font) return 0.0f;
    text = i18n_text(text);
    const float width = ui_text_width(text, size);
    if (align == UI_ALIGN_CENTER) x -= width / 2.0f;
    else if (align == UI_ALIGN_RIGHT) x -= width;
    const float scale = glyph_scale(size);
    vita2d_pgf_draw_text(g_font, (int)floorf(TX(x) + 0.5f), (int)floorf(TY(y) + TS(size) * FONT_ASCENT + 0.5f), color,
                         scale, text);
    return width;
}

/* ---- Frames -------------------------------------------------------------- */

static vita2d_texture *g_frames_retired[2];
/* render.c may end the drawing first, to look at the frame. */
static bool g_drawing;

void ui_frame_begin(void)
{
    vita2d_start_drawing();
    g_drawing = true;
    vita2d_set_clear_color(UI_BG);
    vita2d_clear_screen();
    ui_screen();
    ui_frame_tick();
    /* Once, after the log opens: the size the glyphs are drawn at. */
    static unsigned frames;
    if (++frames == 120)
        diagnostic_log("UI", "font line %.1f px, Japanese %s", (double)g_font_line, g_has_japanese ? "yes" : "no");
}

void ui_frame_finish(void)
{
    if (g_drawing) vita2d_end_drawing();
    g_drawing = false;
}

void ui_frame_end(void)
{
    ui_frame_finish();
    vita2d_swap_buffers();
    /* The last frame is on the screen: a replaced wallpaper can go. */
    for (int i = 0; i < 2; ++i) {
        if (g_frames_retired[i]) vita2d_free_texture(g_frames_retired[i]);
        g_frames_retired[i] = g_retired[i];
        g_retired[i] = NULL;
    }
}

bool ui_init(void)
{
    /* Kasumi draws hundreds of shapes a frame (cards, rings, wave patterns):
     * a roomier vertex pool than vita2d's default. */
    if (vita2d_init_advanced(4 * 1024 * 1024) < 0) return false;
    vita2d_set_vblank_wait(1);
    const vita2d_system_pgf_config fonts[] = {
        { SCE_FONT_LANGUAGE_LATIN, latin },
        { SCE_FONT_LANGUAGE_DEFAULT, NULL },
    };
    g_font = vita2d_load_system_pgf(2, fonts);
    if (!g_font) g_font = vita2d_load_default_pgf();
    if (g_font) {
        const int line = vita2d_pgf_text_height(g_font, 1.0f, "Ag");
        if (line > 0) g_font_line = (float)line;
        g_has_japanese = vita2d_pgf_text_width(g_font, 1.0f, "\xE9\x9C\x9E") > 0;
    }
    for (int i = 0; i < UI_IMAGE_COUNT; ++i) {
        g_art[i] = vita2d_load_PNG_buffer(ART[i]);
        if (!g_art[i]) continue;
        vita2d_texture_set_filters(g_art[i], SCE_GXM_TEXTURE_FILTER_LINEAR, SCE_GXM_TEXTURE_FILTER_LINEAR);
        if (recolourable((UiImage)i)) g_mask[i] = white_copy(g_art[i]);
    }
    /* The backdrop grid tiles, pixel for pixel. */
    if (g_art[UI_IMAGE_GRID]) {
        vita2d_texture_set_filters(g_art[UI_IMAGE_GRID], SCE_GXM_TEXTURE_FILTER_POINT, SCE_GXM_TEXTURE_FILTER_POINT);
        sceGxmTextureSetUAddrMode(&g_art[UI_IMAGE_GRID]->gxm_tex, SCE_GXM_TEXTURE_ADDR_REPEAT);
        sceGxmTextureSetVAddrMode(&g_art[UI_IMAGE_GRID]->gxm_tex, SCE_GXM_TEXTURE_ADDR_REPEAT);
    }
    /* The video frames too, here on the UI thread rather than when a stream
     * starts on the media thread. */
    video_vita_prepare();
    ui_common_init();
    return g_font != NULL;
}

void ui_exit(void)
{
    vita2d_wait_rendering_done();
    for (int i = 0; i < UI_IMAGE_COUNT; ++i) {
        if (g_art[i]) vita2d_free_texture(g_art[i]);
        if (g_mask[i]) vita2d_free_texture(g_mask[i]);
        g_art[i] = g_mask[i] = NULL;
    }
    for (int i = 0; i < 2; ++i) {
        if (g_retired[i]) vita2d_free_texture(g_retired[i]);
        if (g_frames_retired[i]) vita2d_free_texture(g_frames_retired[i]);
        g_retired[i] = g_frames_retired[i] = NULL;
    }
    if (g_backdrop) vita2d_free_texture(g_backdrop);
    if (g_glass) vita2d_free_texture(g_glass);
    g_backdrop = g_glass = NULL;
    if (g_font) vita2d_free_pgf(g_font);
    g_font = NULL;
    vita2d_fini();
}

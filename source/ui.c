#include "ui.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
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

/* Wallpapers (gfx/bg_*.png, gfx/glass_*.png; tools/themes/make_themes.py). */
#define THEME_GFX(name) \
    extern const unsigned char _binary_bg_##name##_t3x_start[], _binary_bg_##name##_t3x_end[]; \
    extern const unsigned char _binary_glass_##name##_t3x_start[], _binary_glass_##name##_t3x_end[];
THEME_GFX(seiji) THEME_GFX(sakura) THEME_GFX(kin) THEME_GFX(ai) THEME_GFX(fuji)
THEME_GFX(beni) THEME_GFX(matcha) THEME_GFX(kaki) THEME_GFX(sumi) THEME_GFX(shiro)
#define THEME_ART(name) _binary_bg_##name##_t3x_start, _binary_bg_##name##_t3x_end, \
    _binary_glass_##name##_t3x_start, _binary_glass_##name##_t3x_end

/* 青磁 celadon, 桜 cherry, 金 gold, 藍 indigo, 藤 wisteria, 紅 crimson,
 * 抹茶 matcha, 柿 persimmon, 墨 ink, 白 white. */
static const struct {
    const char *name;
    u8 r, g, b;
    const unsigned char *bg, *bg_end, *glass, *glass_end;
} THEMES[UI_THEME_COUNT] = {
    { "Seiji", 0x7E, 0xBE, 0xA5, THEME_ART(seiji) },
    { "Sakura", 0xE8, 0x9A, 0xB4, THEME_ART(sakura) },
    { "Kin", 0xD8, 0xA5, 0x3F, THEME_ART(kin) },
    { "Ai", 0x74, 0x9B, 0xD8, THEME_ART(ai) },
    { "Fuji", 0xA9, 0x8E, 0xD6, THEME_ART(fuji) },
    { "Beni", 0xE2, 0x72, 0x7C, THEME_ART(beni) },
    { "Matcha", 0x9C, 0xC2, 0x6E, THEME_ART(matcha) },
    { "Kaki", 0xEC, 0x93, 0x52, THEME_ART(kaki) },
    { "Sumi", 0xC8, 0xC4, 0xBC, THEME_ART(sumi) },
    { "Shiro", 0xEC, 0xE8, 0xDE, THEME_ART(shiro) },
};

/* The theme's wallpaper and its blurred copy. A replaced pair is freed at
 * the next frame, once the GPU has stopped drawing with it. */
static C2D_SpriteSheet g_backdrop, g_glass, g_retired[2];
static int g_backdrop_theme = -1;
/* Drawing into the lower screen, and the wallpaper is behind this frame:
 * glass only makes sense over it (in a game the lower screen stays black). */
static bool g_on_bottom, g_backdrop_drawn;

static void load_backdrop(UiTheme theme)
{
    if ((int)theme == g_backdrop_theme) return;
    if (g_retired[0]) C2D_SpriteSheetFree(g_retired[0]);
    if (g_retired[1]) C2D_SpriteSheetFree(g_retired[1]);
    g_retired[0] = g_backdrop;
    g_retired[1] = g_glass;
    g_backdrop = C2D_SpriteSheetLoadFromMem(THEMES[theme].bg, (size_t)(THEMES[theme].bg_end - THEMES[theme].bg));
    g_glass = C2D_SpriteSheetLoadFromMem(THEMES[theme].glass,
                                         (size_t)(THEMES[theme].glass_end - THEMES[theme].glass));
    g_backdrop_theme = (int)theme;
}

void ui_set_theme(UiTheme theme)
{
    if (theme >= UI_THEME_COUNT) theme = UI_THEME_AI;
    g_theme = theme;
    const u8 r = THEMES[theme].r, g = THEMES[theme].g, b = THEMES[theme].b;
    g_ui_accent = C2D_Color32(r, g, b, 0xFF);
    g_ui_accent_deep = C2D_Color32(r * 18 / 100, g * 18 / 100, b * 18 / 100, 0xFF);
    g_ui_accent_ink = C2D_Color32(r * 12 / 100, g * 12 / 100, b * 12 / 100, 0xFF);
    load_backdrop(theme);
}

bool ui_backdrop(void)
{
    if (!g_backdrop) return false;
    g_backdrop_drawn = C2D_DrawImageAt(C2D_SpriteSheetGetImage(g_backdrop, 0), 0.0f, 0.0f, 0.0f, NULL, 1.0f, 1.0f);
    return g_backdrop_drawn;
}

/* The glass image is the wallpaper at half size, blurred: take the part
 * under r and draw it back at twice the size, so it lines up. */
bool ui_glass(UiRect r)
{
    if (!g_glass || !g_on_bottom || !g_backdrop_drawn || r.w < 2.0f || r.h < 2.0f) return false;
    const C2D_Image full = C2D_SpriteSheetGetImage(g_glass, 0);
    const float tw = full.tex->width, th = full.tex->height;
    const float dv = full.subtex->bottom < full.subtex->top ? -1.0f : 1.0f;
    const float x = r.x / 2.0f, y = r.y / 2.0f, w = r.w / 2.0f, h = r.h / 2.0f;
    Tex3DS_SubTexture sub = *full.subtex;
    sub.width = (u16)(w + 0.5f) ? (u16)(w + 0.5f) : 1;
    sub.height = (u16)(h + 0.5f) ? (u16)(h + 0.5f) : 1;
    sub.left = full.subtex->left + x / tw;
    sub.right = full.subtex->left + (x + w) / tw;
    sub.top = full.subtex->top + dv * y / th;
    sub.bottom = full.subtex->top + dv * (y + h) / th;
    const C2D_Image part = { full.tex, &sub };
    return C2D_DrawImageAt(part, r.x, r.y, 0.0f, NULL, r.w / sub.width, r.h / sub.height);
}

bool ui_wallpaper(UiRect r)
{
    if (!g_backdrop || r.w < 1.0f || r.h < 1.0f) return false;
    const C2D_Image full = C2D_SpriteSheetGetImage(g_backdrop, 0);
    const float tw = full.tex->width, th = full.tex->height;
    const float dv = full.subtex->bottom < full.subtex->top ? -1.0f : 1.0f;
    Tex3DS_SubTexture sub = *full.subtex;
    sub.width = (u16)r.w;
    sub.height = (u16)r.h;
    sub.left = full.subtex->left + r.x / tw;
    sub.right = full.subtex->left + (r.x + r.w) / tw;
    sub.top = full.subtex->top + dv * r.y / th;
    sub.bottom = full.subtex->top + dv * (r.y + r.h) / th;
    const C2D_Image part = { full.tex, &sub };
    return C2D_DrawImageAt(part, r.x, r.y, 0.0f, NULL, 1.0f, 1.0f);
}

const char *ui_theme_name(UiTheme theme)
{
    return theme < UI_THEME_COUNT ? THEMES[theme].name : "";
}

u32 ui_theme_color(UiTheme theme)
{
    if (theme >= UI_THEME_COUNT) theme = UI_THEME_AI;
    return C2D_Color32(THEMES[theme].r, THEMES[theme].g, THEMES[theme].b, 0xFF);
}

static C3D_RenderTarget *g_top;
static C3D_RenderTarget *g_top_wide;
static C3D_RenderTarget *g_bottom;
static bool g_top_wide_linked;
static C3D_Tex g_video_tex;
static Tex3DS_SubTexture g_video_subtex;
static bool g_video_ready;
/* The picture inside the surface, and where it goes on the 800x480 view. */
static unsigned g_video_w = VIDEO_WIDTH, g_video_h = VIDEO_HEIGHT;
static float g_video_x, g_video_y, g_video_scale = 1.0f;
/* The part of the picture shown (zoom), as fractions of it. */
static float g_crop_x, g_crop_y, g_crop_w = 1.0f, g_crop_h = 1.0f;
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
GFX_SYMBOLS(discord)
GFX_SYMBOLS(sec_controls)
GFX_SYMBOLS(sec_picture)
GFX_SYMBOLS(sec_sound)
GFX_SYMBOLS(sec_network)
GFX_SYMBOLS(sec_system)
GFX_SYMBOLS(sec_account)
GFX_SYMBOLS(no_cover)
GFX_SYMBOLS(grid)
GFX_SYMBOLS(look_ring)
GFX_SYMBOLS(look_dot)
extern const unsigned char _binary_video_shbin_start[];
extern const unsigned char _binary_video_shbin_end[];
#define GFX_ENTRY(name) { _binary_##name##_t3x_start, _binary_##name##_t3x_end }

static const struct { const unsigned char *start, *end; } GFX_DATA[UI_IMAGE_COUNT] = {
    GFX_ENTRY(hero), GFX_ENTRY(mist), GFX_ENTRY(enso), GFX_ENTRY(seal), GFX_ENTRY(seal40),
    GFX_ENTRY(seal16), GFX_ENTRY(lantern), GFX_ENTRY(discord),
    GFX_ENTRY(sec_controls), GFX_ENTRY(sec_picture), GFX_ENTRY(sec_sound), GFX_ENTRY(sec_network),
    GFX_ENTRY(sec_system), GFX_ENTRY(sec_account), GFX_ENTRY(no_cover), GFX_ENTRY(grid),
    GFX_ENTRY(look_ring), GFX_ENTRY(look_dot)
};
static C2D_SpriteSheet g_sheets[UI_IMAGE_COUNT];

/* The picture filter (shaders/video.v.pica + six texture combiner stages). */
static u32 *g_look_code;
static DVLB_s *g_look_dvlb;
static shaderProgram_s g_look_program;
static int g_look_projection = -1;
static bool g_look_ready;
static float g_look_sharp, g_look_mix, g_look_tone;
static bool g_look_on;
/* citro3d cannot unbind units 1 and 2 (C3D_TexBind dereferences NULL
 * there: crash dump 35), so they get a tiny texture back after the draw. */
static C3D_Tex g_look_blank;
static bool g_look_blank_ready;

static void look_init(void)
{
    const size_t size = (size_t)(_binary_video_shbin_end - _binary_video_shbin_start);
    /* DVLB_ParseFile reads words and keeps pointers into the data: an
     * aligned copy that lives as long as the program. */
    g_look_code = malloc((size + 3) & ~(size_t)3);
    if (!g_look_code) return;
    memcpy(g_look_code, _binary_video_shbin_start, size);
    g_look_dvlb = DVLB_ParseFile(g_look_code, (u32)size);
    if (!g_look_dvlb) return;
    shaderProgramInit(&g_look_program);
    if (shaderProgramSetVsh(&g_look_program, &g_look_dvlb->DVLE[0]) < 0) return;
    g_look_projection = shaderInstanceGetUniformLocation(g_look_program.vertexShader, "projection");
    g_look_blank_ready = C3D_TexInit(&g_look_blank, 8, 8, GPU_RGB565);
    if (g_look_blank_ready) memset(g_look_blank.data, 0, g_look_blank.size);
    g_look_ready = g_look_projection >= 0 && g_look_blank_ready;
}

static void look_exit(void)
{
    if (g_look_dvlb) {
        shaderProgramFree(&g_look_program);
        DVLB_Free(g_look_dvlb);
    }
    if (g_look_blank_ready) C3D_TexDelete(&g_look_blank);
    g_look_blank_ready = false;
    free(g_look_code);
    g_look_dvlb = NULL;
    g_look_code = NULL;
    g_look_ready = false;
}

void ui_set_video_look(unsigned sharpen, unsigned color)
{
    /* Sharpening: k * (centre - blur of its six nearest neighbours). */
    static const float SHARP[4] = { 0.0f, 0.3f, 0.5f, 0.75f };
    /* Colour: saturation s (away from green, a stand-in for brightness)
     * and contrast q (away from mid-grey). */
    static const float SAT[3] = { 0.0f, 0.12f, 0.22f };
    static const float CONTRAST[3] = { 0.0f, 0.05f, 0.08f };
    const float k = SHARP[sharpen < 4 ? sharpen : 0];
    const float s = SAT[color < 3 ? color : 0], q = CONTRAST[color < 3 ? color : 0];
    /* Stage 4 mixes the colour term (weight t) with the sharpening term
     * (weight 1 - t), so both are pre-scaled to come out as k, s and q. */
    g_look_mix = q + 2.0f * s;
    g_look_tone = g_look_mix > 0.0f ? 1.0f - s / g_look_mix : 1.0f;
    g_look_sharp = g_look_mix < 1.0f ? k / (1.0f - g_look_mix) : 0.0f;
    if (g_look_sharp > 1.0f) g_look_sharp = 1.0f;
    g_look_on = k > 0.0f || g_look_mix > 0.0f;
}

static u32 look_alpha(float a, u32 rgb)
{
    const int v = (int)(a * 255.0f + 0.5f);
    return ((u32)(v < 0 ? 0 : v > 255 ? 255 : v) << 24) | rgb;
}

static void look_stage(int id, GPU_TEVSRC s1, GPU_TEVOP_RGB o1, GPU_TEVSRC s2, GPU_TEVOP_RGB o2,
                       GPU_TEVSRC s3, GPU_TEVOP_RGB o3, GPU_COMBINEFUNC func, u32 constant)
{
    C3D_TexEnv *env = C3D_GetTexEnv(id);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_RGB, s1, s2, s3);
    C3D_TexEnvOpRgb(env, o1, o2, o3);
    C3D_TexEnvFunc(env, C3D_RGB, func);
    /* RGB565 has no alpha: the frame's 1.0 keeps the output opaque. */
    C3D_TexEnvSrc(env, C3D_Alpha, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
    C3D_TexEnvFunc(env, C3D_Alpha, GPU_REPLACE);
    C3D_TexEnvColor(env, constant);
}

static void look_vertex(float x, float y, float u, float v)
{
    /* One output pixel is one texel across and two rows down. The blur
     * taps sit half a pixel diagonally either way, so bilinear filtering
     * averages the centre with its six nearest neighbours. */
    const float du = 0.5f / MVD_TEX_WIDTH, dv = 1.0f / MVD_TEX_HEIGHT;
    C3D_ImmSendAttrib(x, y, 0.0f, 1.0f);
    C3D_ImmSendAttrib(u, v, 0.0f, 0.0f);
    C3D_ImmSendAttrib(u + du, v - dv, 0.0f, 0.0f);
    C3D_ImmSendAttrib(u - du, v + dv, 0.0f, 0.0f);
}

/* Draws the wide frame through the filter. With c the pixel, b the blur
 * and G its green channel, every value kept around 0.5 so it can go
 * negative:
 *   0  b                              5  c + k(c - b) + s(c - G) + q(c - 0.5)
 *   1  0.5 + (c - b)
 *   2  0.5 + k'(c - b)          -> combiner buffer
 *   3  0.5 + (1-a)(c - G) + (2a-1)(c - 0.5)
 *   4  t * stage 3 + (1 - t) * buffer */
static void draw_video_look(void)
{
    C2D_Flush();
    C3D_BindProgram(&g_look_program);
    C3D_AttrInfo *attr = C3D_GetAttrInfo();
    AttrInfo_Init(attr);
    for (int i = 0; i < 4; ++i) AttrInfo_AddLoader(attr, i, GPU_FLOAT, 4);
    C3D_Mtx projection;
    Mtx_OrthoTilt(&projection, 0.0f, 800.0f, 240.0f, 0.0f, 1.0f, -1.0f, true);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, g_look_projection, &projection);
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
    for (int unit = 0; unit < 3; ++unit) C3D_TexBind(unit, &g_video_tex);

    look_stage(0, GPU_TEXTURE1, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEXTURE2, GPU_TEVOP_RGB_SRC_COLOR,
               GPU_CONSTANT, GPU_TEVOP_RGB_SRC_ALPHA, GPU_INTERPOLATE, look_alpha(0.5f, 0));
    look_stage(1, GPU_TEXTURE0, GPU_TEVOP_RGB_SRC_COLOR, GPU_PREVIOUS, GPU_TEVOP_RGB_ONE_MINUS_SRC_COLOR,
               GPU_PRIMARY_COLOR, GPU_TEVOP_RGB_SRC_COLOR, GPU_ADD_SIGNED, 0);
    look_stage(2, GPU_PREVIOUS, GPU_TEVOP_RGB_SRC_COLOR, GPU_CONSTANT, GPU_TEVOP_RGB_SRC_COLOR,
               GPU_CONSTANT, GPU_TEVOP_RGB_SRC_ALPHA, GPU_INTERPOLATE, look_alpha(g_look_sharp, 0x808080));
    look_stage(3, GPU_TEXTURE0, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEXTURE0, GPU_TEVOP_RGB_ONE_MINUS_SRC_G,
               GPU_CONSTANT, GPU_TEVOP_RGB_SRC_ALPHA, GPU_INTERPOLATE, look_alpha(g_look_tone, 0));
    look_stage(4, GPU_PREVIOUS, GPU_TEVOP_RGB_SRC_COLOR, GPU_PREVIOUS_BUFFER, GPU_TEVOP_RGB_SRC_COLOR,
               GPU_CONSTANT, GPU_TEVOP_RGB_SRC_ALPHA, GPU_INTERPOLATE, look_alpha(g_look_mix, 0));
    look_stage(5, GPU_TEXTURE0, GPU_TEVOP_RGB_SRC_COLOR, GPU_PREVIOUS, GPU_TEVOP_RGB_SRC_COLOR,
               GPU_PRIMARY_COLOR, GPU_TEVOP_RGB_SRC_COLOR, GPU_ADD_SIGNED, 0);
    C3D_TexEnvBufUpdate(C3D_RGB, BIT(2));

    const float u0 = g_crop_x * (float)g_video_w / MVD_TEX_WIDTH;
    const float u1 = (g_crop_x + g_crop_w) * (float)g_video_w / MVD_TEX_WIDTH;
    const float v0 = 1.0f - g_crop_y * (float)g_video_h / MVD_TEX_HEIGHT;
    const float v1 = 1.0f - (g_crop_y + g_crop_h) * (float)g_video_h / MVD_TEX_HEIGHT;
    const float x0 = g_video_x, x1 = g_video_x + (float)g_video_w * g_video_scale;
    const float y0 = g_video_y / 2.0f, y1 = (g_video_y + (float)g_video_h * g_video_scale) / 2.0f;
    C3D_ImmDrawBegin(GPU_TRIANGLE_STRIP);
    look_vertex(x0, y0, u0, v0);
    look_vertex(x0, y1, u0, v1);
    look_vertex(x1, y0, u1, v0);
    look_vertex(x1, y1, u1, v1);
    C3D_ImmDrawEnd();

    /* Hand the GPU back to citro2d as it expects it. */
    for (int i = 0; i < 6; ++i) C3D_TexEnvInit(C3D_GetTexEnv(i));
    C3D_TexEnvBufUpdate(C3D_Both, 0);
    C3D_TexBind(1, &g_look_blank);
    C3D_TexBind(2, &g_look_blank);
    C2D_Prepare();
}

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
        look_init();
    }
    C3D_RenderTargetSetOutput(g_top, GFX_TOP, GFX_LEFT,
                              TRANSFER_FLAGS(gfxGetScreenFormat(GFX_TOP)));
    C3D_RenderTargetSetOutput(g_bottom, GFX_BOTTOM, GFX_LEFT,
                              TRANSFER_FLAGS(gfxGetScreenFormat(GFX_BOTTOM)));
    for (int i = 0; i < UI_IMAGE_COUNT; ++i)
        g_sheets[i] = C2D_SpriteSheetLoadFromMem(GFX_DATA[i].start,
                                                 (size_t)(GFX_DATA[i].end - GFX_DATA[i].start));
    /* The backdrop tiles, pixel for pixel. */
    if (g_sheets[UI_IMAGE_GRID]) {
        C3D_Tex *tex = C2D_SpriteSheetGetImage(g_sheets[UI_IMAGE_GRID], 0).tex;
        C3D_TexSetWrap(tex, GPU_REPEAT, GPU_REPEAT);
        C3D_TexSetFilter(tex, GPU_NEAREST, GPU_NEAREST);
    }
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
    look_exit();
    if (g_video_ready) C3D_TexDelete(&g_video_tex);
    if (g_top_wide) C3D_RenderTargetDelete(g_top_wide);
    if (g_top) C3D_RenderTargetDelete(g_top);
    if (g_bottom) C3D_RenderTargetDelete(g_bottom);
    C2D_Fini();
    C3D_Fini();
}

static void frame_started(void);

void ui_frame_begin(bool sync_vblank)
{
    C3D_FrameBegin(sync_vblank ? C3D_FRAME_SYNCDRAW : 0);
    frame_started();
}

bool ui_frame_try_begin(void)
{
    if (!C3D_FrameBegin(C3D_FRAME_NONBLOCK)) return false;
    frame_started();
    return true;
}

static void frame_started(void)
{
    /* The GPU has finished the last frame: a replaced wallpaper can go. */
    for (int i = 0; i < 2; ++i)
        if (g_retired[i]) {
            C2D_SpriteSheetFree(g_retired[i]);
            g_retired[i] = NULL;
        }
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

/* Double-buffered, a new picture only appears at a vblank (no tearing):
 * menus and wide video. Classic video writes the visible buffer directly,
 * so it alone stays single. Beta.30 test: menus were single-buffered too,
 * and a scrolling list tore across the middle ("the text splits in half"). */
static bool g_top_double;

static void top_double_buffer(bool on)
{
    if (on == g_top_double) return;
    gfxSetDoubleBuffering(GFX_TOP, on);
    g_top_double = on;
}

/* Only one render target can feed a screen; swap which one is linked. */
static void link_top(bool wide)
{
    if (wide == g_top_wide_linked) return;
    C3D_RenderTargetSetOutput(wide ? g_top_wide : g_top, GFX_TOP, GFX_LEFT,
                              TRANSFER_FLAGS(gfxGetScreenFormat(GFX_TOP)));
    g_top_wide_linked = wide;
    gfxSetWide(wide);
    top_double_buffer(wide);
}

void ui_top_classic_video(void)
{
    link_top(false);
    top_double_buffer(false);
}

void ui_begin_top(void)
{
    g_on_bottom = false;
    link_top(false);
    top_double_buffer(true);
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
    g_on_bottom = false;
    link_top(true);
    /* Black: a picture narrower than the screen leaves bars. */
    C2D_TargetClear(g_top_wide, C2D_Color32(0x00, 0x00, 0x00, 0xFF));
    C2D_SceneBegin(g_top_wide);
    /* citro2d reuses its cached 400-wide top-screen projection for 800-wide
     * targets too (a framebuffer readback showed everything drawn at twice
     * the width), so halve x to address all 800 columns. */
    C2D_ViewReset();
    C2D_ViewScale(0.5f, 1.0f);
}

void ui_set_video_size(unsigned width, unsigned height)
{
    if (!width || !height || width > VIDEO_WIDTH || height > VIDEO_HEIGHT) width = VIDEO_WIDTH, height = VIDEO_HEIGHT;
    if (width == g_video_w && height == g_video_h) return;
    g_video_w = width;
    g_video_h = height;
    g_video_subtex.width = (u16)width;
    g_video_subtex.height = (u16)height;
    g_video_subtex.right = (float)width / MVD_TEX_WIDTH;
    g_video_subtex.bottom = 1.0f - (float)height / MVD_TEX_HEIGHT;
    /* As large as fits in 800x480, centred, shape kept. */
    float scale = (float)VIDEO_WIDTH / (float)width;
    const float by_height = (float)VIDEO_HEIGHT / (float)height;
    if (by_height < scale) scale = by_height;
    g_video_scale = scale;
    g_video_x = floorf(((float)VIDEO_WIDTH - (float)width * scale) / 2.0f);
    g_video_y = floorf(((float)VIDEO_HEIGHT - (float)height * scale) / 2.0f);
}

void ui_set_video_crop(float x, float y, float w, float h)
{
    if (!(w > 0.0f && h > 0.0f && w <= 1.0f && h <= 1.0f)) x = y = 0.0f, w = h = 1.0f;
    g_crop_x = x;
    g_crop_y = y;
    g_crop_w = w;
    g_crop_h = h;
}

void ui_draw_video(void)
{
    if (!g_video_ready) return;
    if (g_look_on && g_look_ready) {
        draw_video_look();
        return;
    }
    /* Same size on screen; the zoom only narrows the texture coordinates. */
    Tex3DS_SubTexture sub = g_video_subtex;
    sub.left = g_crop_x * (float)g_video_w / MVD_TEX_WIDTH;
    sub.right = (g_crop_x + g_crop_w) * (float)g_video_w / MVD_TEX_WIDTH;
    sub.top = 1.0f - g_crop_y * (float)g_video_h / MVD_TEX_HEIGHT;
    sub.bottom = 1.0f - (g_crop_y + g_crop_h) * (float)g_video_h / MVD_TEX_HEIGHT;
    const C2D_Image image = { &g_video_tex, &sub };
    /* Full width, half height: 800x480 -> 800x240 with a 2:1 row average. */
    C2D_DrawImageAt(image, g_video_x, g_video_y / 2.0f, 0.0f, NULL, g_video_scale, 0.5f * g_video_scale);
}

void ui_begin_bottom(void)
{
    g_on_bottom = true;
    g_backdrop_drawn = false;
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

void ui_texture(UiImage image, float x, float y, float w, float h, u32 color)
{
    if (image >= UI_IMAGE_COUNT || !g_sheets[image] || w < 1.0f || h < 1.0f) return;
    const C2D_Image full = C2D_SpriteSheetGetImage(g_sheets[image], 0);
    const float tw = full.tex->width, th = full.tex->height;
    /* t3x images run top (v = top) to bottom; keep that direction. */
    const float dv = full.subtex->bottom < full.subtex->top ? -1.0f : 1.0f;
    Tex3DS_SubTexture sub = *full.subtex;
    sub.width = (u16)w;
    sub.height = (u16)h;
    sub.left = x / tw;
    sub.right = (x + w) / tw;
    sub.top = full.subtex->top + dv * y / th;
    sub.bottom = full.subtex->top + dv * (y + h) / th;
    const C2D_Image part = { full.tex, &sub };
    C2D_ImageTint tint;
    C2D_PlainImageTint(&tint, color, 1.0f);
    C2D_DrawImageAt(part, floorf(x + 0.5f), floorf(y + 0.5f), 0.0f, &tint, 1.0f, 1.0f);
}

bool ui_image_tint(UiImage image, float x, float y, float scale, u32 color)
{
    if (image >= UI_IMAGE_COUNT || !g_sheets[image]) return false;
    C2D_ImageTint tint;
    C2D_PlainImageTint(&tint, color, 1.0f);
    return C2D_DrawImageAt(C2D_SpriteSheetGetImage(g_sheets[image], 0), floorf(x + 0.5f), floorf(y + 0.5f),
                           0.0f, &tint, scale, scale);
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
        /* Not UTF-8: citro2d draws a box there, so measure one and go on.
         * Stopping here made a long message "fit" on one line. */
        if (units <= 0) {
            width += glyph_advance('?');
            ++p;
            continue;
        }
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
    ui_surface(r, UI_LINE, UI_SURFACE);
    ui_rect(r.x + r.w / 2.0f - 12.0f, r.y, 24.0f, 2.0f, accent);
}

float ui_pill(float x, float y, u32 color, UiAlign align, const char *text)
{
    const float w = ui_text_width(text, 11.0f) + 12.0f;
    if (align == UI_ALIGN_CENTER) x -= w / 2.0f;
    else if (align == UI_ALIGN_RIGHT) x -= w;
    ui_rect(x, y, w, 16.0f, color);
    ui_rect(x + 1.0f, y + 1.0f, w - 2.0f, 14.0f, UI_BG);
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

/* Corner marks, like registration marks on a print: short L strokes
 * laid over the border at each corner. */
/* Strokes never overlap: with a see-through colour a pixel drawn twice
 * comes out brighter, and every corner showed a dot (beta.30 test). */
static void corner_marks(UiRect r, float len, u32 color)
{
    ui_rect(r.x, r.y, len, 1.0f, color);
    ui_rect(r.x, r.y + 1.0f, 1.0f, len - 1.0f, color);
    ui_rect(r.x + r.w - len, r.y, len, 1.0f, color);
    ui_rect(r.x + r.w - 1.0f, r.y + 1.0f, 1.0f, len - 1.0f, color);
    ui_rect(r.x, r.y + r.h - 1.0f, len, 1.0f, color);
    ui_rect(r.x, r.y + r.h - len, 1.0f, len - 1.0f, color);
    ui_rect(r.x + r.w - len, r.y + r.h - 1.0f, len, 1.0f, color);
    ui_rect(r.x + r.w - 1.0f, r.y + r.h - len, 1.0f, len - 1.0f, color);
}

/* A 1 px frame whose sides don't overlap at the corners (see above). */
static void frame(UiRect r, u32 color)
{
    ui_rect(r.x, r.y, r.w, 1.0f, color);
    ui_rect(r.x, r.y + r.h - 1.0f, r.w, 1.0f, color);
    ui_rect(r.x, r.y + 1.0f, 1.0f, r.h - 2.0f, color);
    ui_rect(r.x + r.w - 1.0f, r.y + 1.0f, 1.0f, r.h - 2.0f, color);
}

/* Square and defined: a 1 px border and brighter marks at the corners.
 * (Washi fibres inside read as scratches on the 3DS screen, beta.30 test.) */
void ui_surface(UiRect r, u32 border, u32 fill)
{
    const UiRect inner = { r.x + 1.0f, r.y + 1.0f, r.w - 2.0f, r.h - 2.0f };
    if (ui_glass(inner)) {
        /* Frosted glass over the wallpaper: a wash of the fill, a light
         * edge, and a brighter rim along the top. */
        ui_rect(inner.x, inner.y, inner.w, inner.h, ui_with_alpha(fill, 0x70));
        frame(r, C2D_Color32(0xFF, 0xFF, 0xFF, 0x38));
        ui_rect(inner.x, inner.y, inner.w, 1.0f, C2D_Color32(0xFF, 0xFF, 0xFF, 0x30));
        /* Over the frame, not on top of it twice: the marks are opaque. */
        corner_marks(r, 5.0f, C2D_Color32(0xB8, 0xB8, 0xB8, 0xFF));
        return;
    }
    ui_rect_r(r, border);
    ui_rect(inner.x, inner.y, inner.w, inner.h, fill);
    corner_marks(r, 5.0f, UI_TEXT_FAINT);
}

/* Every pressable thing: a square key on a hard 2 px lip. Pressed, the
 * face drops onto the lip. A 1 px bevel along its top, a faint sheen on
 * its upper half, and corner marks like the cards'. Returns the face,
 * where the content goes (it moves with the press). */
#define KEY_LIP 2.0f
UiRect ui_key(UiRect r, UiButtonStyle style, bool pressed)
{
    u32 face = UI_RAISED, border = UI_LINE_STRONG, lip = C2D_Color32(0x2A, 0x2A, 0x31, 0xFF);
    u32 tick = UI_TEXT_DIM;
    u8 bevel = 0x22, sheen = 0x0C;
    if (style == UI_BUTTON_PRIMARY) {
        face = UI_ACCENT; border = UI_ACCENT; lip = UI_ACCENT_DEEP; tick = UI_ACCENT_INK;
        bevel = 0x50; sheen = 0x22;
    } else if (style == UI_BUTTON_DANGER) {
        face = UI_DANGER_DEEP; border = UI_DANGER; lip = ui_mix(UI_DANGER, UI_BG, 0.55f); tick = UI_DANGER;
    } else if (style == UI_BUTTON_ACTIVE) {
        border = UI_ACCENT; lip = UI_ACCENT_DEEP; tick = UI_ACCENT;
    }
    if (pressed) face = ui_mix(face, UI_TEXT, 0.10f);
    const float sink = pressed ? KEY_LIP : 0.0f;
    const float face_h = r.h - KEY_LIP;
    const UiRect top = { r.x, r.y + sink, r.w, face_h };
    const UiRect inner = { top.x + 1.0f, top.y + 1.0f, top.w - 2.0f, top.h - 2.0f };
    if (g_on_bottom && g_glass && g_backdrop_drawn) {
        /* Liquid glass over the wallpaper: the blurred picture behind it,
         * tinted by the key's colour (solid for the main action), a light
         * rim, a sheen down from the top and a dark lip below. */
        /* The lip only shows below the face, so draw just that strip. */
        if (!pressed) ui_rect(r.x + 1.0f, r.y + face_h, r.w - 2.0f, KEY_LIP, C2D_Color32(0x00, 0x00, 0x00, 0xA0));
        ui_glass(inner);
        const u8 wash = style == UI_BUTTON_PRIMARY ? 0xE0 : style == UI_BUTTON_DANGER ? 0xB0 : 0x48;
        ui_rect(inner.x, inner.y, inner.w, inner.h, ui_with_alpha(style == UI_BUTTON_NORMAL ? UI_SURFACE : face, wash));
        if (style == UI_BUTTON_ACTIVE)
            ui_rect(inner.x, inner.y, inner.w, inner.h, ui_with_alpha(UI_ACCENT, 0x30));
        if (pressed) ui_rect(inner.x, inner.y, inner.w, inner.h, C2D_Color32(0xFF, 0xFF, 0xFF, 0x18));
        frame(top, style == UI_BUTTON_NORMAL ? C2D_Color32(0xFF, 0xFF, 0xFF, 0x48) : ui_with_alpha(border, 0xE0));
        C2D_DrawRectangle(inner.x, inner.y + 1.0f, 0.0f, inner.w, inner.h * 0.5f - 1.0f,
                          C2D_Color32(0xFF, 0xFF, 0xFF, 0x1C), C2D_Color32(0xFF, 0xFF, 0xFF, 0x1C),
                          C2D_Color32(0xFF, 0xFF, 0xFF, 0x00), C2D_Color32(0xFF, 0xFF, 0xFF, 0x00));
        ui_rect(inner.x, inner.y, inner.w, 1.0f, C2D_Color32(0xFF, 0xFF, 0xFF, 0x60));
        corner_marks(top, 5.0f, tick);
        return top;
    }
    ui_rect(r.x, r.y + KEY_LIP, r.w, face_h, lip);
    ui_rect_r(top, border);
    ui_rect(top.x + 1.0f, top.y + 1.0f, top.w - 2.0f, top.h - 2.0f, face);
    C2D_DrawRectangle(top.x + 1.0f, top.y + 1.0f, 0.0f, top.w - 2.0f, (top.h - 2.0f) * 0.5f,
                      C2D_Color32(0xFF, 0xFF, 0xFF, sheen), C2D_Color32(0xFF, 0xFF, 0xFF, sheen),
                      C2D_Color32(0xFF, 0xFF, 0xFF, 0x00), C2D_Color32(0xFF, 0xFF, 0xFF, 0x00));
    ui_rect(top.x + 1.0f, top.y + 1.0f, top.w - 2.0f, 1.0f, C2D_Color32(0xFF, 0xFF, 0xFF, bevel));
    corner_marks(top, 5.0f, tick);
    return top;
}

void ui_button(UiRect r, const char *label, const char *jp, UiButtonStyle style, bool pressed)
{
    u32 text = UI_TEXT, sub = UI_TEXT_FAINT;
    if (style == UI_BUTTON_PRIMARY) { text = UI_BG; sub = UI_ACCENT_INK; }
    else if (style == UI_BUTTON_DANGER) sub = UI_DANGER;
    else if (style == UI_BUTTON_ACTIVE) sub = UI_ACCENT;
    const UiRect f = ui_key(r, style, pressed);
    const float label_size = r.h >= 40.0f ? 14.0f : 12.0f;
    const float jp_size = r.h >= 40.0f ? 12.0f : 11.0f;
    if (jp && jp[0] && r.h >= 30.0f && (g_has_japanese || !contains_japanese(jp))) {
        const float block = label_size + 2.0f + jp_size;
        const float top = f.y + (f.h - block) / 2.0f;
        ui_text_fit(f.x + f.w / 2.0f, top, label_size, text, UI_ALIGN_CENTER, f.w - 8.0f, label);
        ui_text_fit(f.x + f.w / 2.0f, top + label_size + 2.0f, jp_size, sub,
                    UI_ALIGN_CENTER, f.w - 8.0f, jp);
    } else {
        ui_text_fit(f.x + f.w / 2.0f, f.y + (f.h - label_size) / 2.0f - 1.0f, label_size,
                    text, UI_ALIGN_CENTER, f.w - 8.0f, label);
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

void ui_xbox_face(float cx, float cy, float s, char letter, u32 color)
{
    ui_circle(cx, cy, s / 2 + 1.0f, color);
    const char text[2] = { letter, '\0' };
    const float size = s * 0.9f;
    ui_text(cx, cy - size / 2, size, UI_BG, UI_ALIGN_CENTER, text);
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

#include "ui.h"
#include "ui_3ds.h"
#include "ui_internal.h"
#include "platform.h"
#include "i18n.h"

#include <3ds.h>
#include <citro2d.h>

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

/* Wallpapers (gfx/bg_*.png, gfx/glass_*.png; tools/themes/make_themes.py). */
#define THEME_GFX(name) \
    extern const unsigned char _binary_bg_##name##_t3x_start[], _binary_bg_##name##_t3x_end[]; \
    extern const unsigned char _binary_glass_##name##_t3x_start[], _binary_glass_##name##_t3x_end[];
THEME_GFX(seiji) THEME_GFX(sakura) THEME_GFX(kin) THEME_GFX(ai) THEME_GFX(fuji)
THEME_GFX(beni) THEME_GFX(matcha) THEME_GFX(kaki) THEME_GFX(sumi) THEME_GFX(shiro)
#define THEME_ART(name) _binary_bg_##name##_t3x_start, _binary_bg_##name##_t3x_end, \
    _binary_glass_##name##_t3x_start, _binary_glass_##name##_t3x_end

/* Each theme's wallpaper and its blurred copy, in UiTheme order. */
static const struct {
    const unsigned char *bg, *bg_end, *glass, *glass_end;
} THEME_ART_DATA[UI_THEME_COUNT] = {
    { THEME_ART(seiji) }, { THEME_ART(sakura) }, { THEME_ART(kin) }, { THEME_ART(ai) }, { THEME_ART(fuji) },
    { THEME_ART(beni) }, { THEME_ART(matcha) }, { THEME_ART(kaki) }, { THEME_ART(sumi) }, { THEME_ART(shiro) },
};

/* The theme's wallpaper and its blurred copy. A replaced pair is freed at
 * the next frame, once the GPU has stopped drawing with it. */
static C2D_SpriteSheet g_backdrop, g_glass, g_retired[2];
static int g_backdrop_theme = -1;
/* Drawing into the lower screen, and the wallpaper is behind this frame:
 * glass only makes sense over it (in a game the lower screen stays black). */
static bool g_on_bottom, g_backdrop_drawn;

void ui_load_wallpaper(UiTheme theme)
{
    if ((int)theme == g_backdrop_theme) return;
    if (g_retired[0]) C2D_SpriteSheetFree(g_retired[0]);
    if (g_retired[1]) C2D_SpriteSheetFree(g_retired[1]);
    g_retired[0] = g_backdrop;
    g_retired[1] = g_glass;
    g_backdrop = C2D_SpriteSheetLoadFromMem(THEME_ART_DATA[theme].bg, (size_t)(THEME_ART_DATA[theme].bg_end - THEME_ART_DATA[theme].bg));
    g_glass = C2D_SpriteSheetLoadFromMem(THEME_ART_DATA[theme].glass,
                                         (size_t)(THEME_ART_DATA[theme].glass_end - THEME_ART_DATA[theme].glass));
    g_backdrop_theme = (int)theme;
}

bool ui_glass_ready(void) { return g_on_bottom && g_glass && g_backdrop_drawn; }

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
GFX_SYMBOLS(svc_gfn)
GFX_SYMBOLS(svc_xbox)
GFX_SYMBOLS(svc_steam)
GFX_SYMBOLS(icon_gfn)
GFX_SYMBOLS(icon_xbox)
GFX_SYMBOLS(icon_steam)
GFX_SYMBOLS(cover_bigpicture)
GFX_SYMBOLS(cover_desktop)
GFX_SYMBOLS(hub_backdrop)
extern const unsigned char _binary_video_shbin_start[];
extern const unsigned char _binary_video_shbin_end[];
#define GFX_ENTRY(name) { _binary_##name##_t3x_start, _binary_##name##_t3x_end }

static const struct { const unsigned char *start, *end; } GFX_DATA[UI_IMAGE_COUNT] = {
    GFX_ENTRY(hero), GFX_ENTRY(mist), GFX_ENTRY(enso), GFX_ENTRY(seal), GFX_ENTRY(seal40),
    GFX_ENTRY(seal16), GFX_ENTRY(lantern), GFX_ENTRY(discord),
    GFX_ENTRY(sec_controls), GFX_ENTRY(sec_picture), GFX_ENTRY(sec_sound), GFX_ENTRY(sec_network),
    GFX_ENTRY(sec_system), GFX_ENTRY(sec_account), GFX_ENTRY(no_cover), GFX_ENTRY(grid),
    GFX_ENTRY(look_ring), GFX_ENTRY(look_dot),
    GFX_ENTRY(svc_gfn), GFX_ENTRY(svc_xbox), GFX_ENTRY(svc_steam),
    GFX_ENTRY(icon_gfn), GFX_ENTRY(icon_xbox), GFX_ENTRY(icon_steam),
    GFX_ENTRY(cover_bigpicture), GFX_ENTRY(cover_desktop), GFX_ENTRY(hub_backdrop)
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

static bool ui_start(void)
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
    ui_common_init();
    /* Regional system fonts (and emulators without a dumped font) can lack
     * kana/kanji; the decorative Japanese labels are then left out. */
    const int missing = fontGetInfo(NULL)->alterCharIndex;
    g_has_japanese = fontGlyphIndexFromCodePoint(NULL, 0x971E) != missing &&
                     fontGlyphIndexFromCodePoint(NULL, 0x30E9) != missing;
    return g_text != NULL;
}

bool ui_init(void)
{
    gfxInitDefault();
    gfxSetScreenFormat(GFX_TOP, GSP_RGB565_OES);
    /* One top framebuffer: MVD frames are written into it directly. */
    gfxSetDoubleBuffering(GFX_TOP, false);
    if (ui_start()) return true;
    gfxExit();
    return false;
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
    gfxExit();
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
    ui_frame_tick();
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
    C2D_TargetClear(g_top_wide, UI_RGBA(0x00, 0x00, 0x00, 0xFF));
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

bool ui_image(UiImage image, float x, float y, float scale, float alpha)
{
    if (image >= UI_IMAGE_COUNT || !g_sheets[image]) return false;
    C2D_ImageTint tint;
    C2D_AlphaImageTint(&tint, alpha);
    /* The seal, ensō and lantern art is celadon with transparent cut-outs,
     * so other themes recolour it wholesale. */
    const bool recolour = ui_theme() != UI_THEME_SEIJI &&
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
    const bool recolour = ui_theme() != UI_THEME_SEIJI && image == UI_IMAGE_ENSO;
    if (recolour) C2D_PlainImageTint(&tint, ui_with_alpha(g_ui_accent, (u8)(alpha * 255.0f)), 1.0f);
    return C2D_DrawImageAtRotated(C2D_SpriteSheetGetImage(g_sheets[image], 0), cx, cy, 0.0f,
                                  angle, recolour || alpha < 1.0f ? &tint : NULL, scale, scale);
}

struct UiPicture {
    C3D_Tex tex;
    Tex3DS_SubTexture sub;
};

static u16 texture_side(unsigned size)
{
    u16 side = 8;
    while (side < size) side <<= 1;
    return side;
}

UiPicture *ui_picture_create(unsigned width, unsigned height)
{
    UiPicture *picture = calloc(1, sizeof(*picture));
    if (!picture) return NULL;
    if (!C3D_TexInit(&picture->tex, texture_side(width), texture_side(height), GPU_RGBA8)) {
        free(picture);
        return NULL;
    }
    C3D_TexSetFilter(&picture->tex, GPU_LINEAR, GPU_LINEAR);
    C3D_TexSetWrap(&picture->tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    return picture;
}

bool ui_picture_set(UiPicture *picture, const u32 *pixels, unsigned width, unsigned height, unsigned stride)
{
    const unsigned tw = picture->tex.width, th = picture->tex.height;
    if (width > tw || height > th) return false;
    /* Tiled the same way as video frames: one display transfer from a
     * linear surface; row 0 lands at the top (v = 1). Outside a frame, so
     * it waits for the GPU to finish the last one before overwriting a
     * texture it may have drawn. */
    u32 *linear = linearAlloc(tw * th * sizeof(u32));
    if (!linear) return false;
    /* The transfer writes the texture behind the CPU's back. After a stream
     * this memory held video buffers the CPU wrote to; any of those lines
     * still dirty in the data cache would later be written back over the
     * picture (blocks of it missing). Flush them out first. */
    GSPGPU_FlushDataCache(picture->tex.data, picture->tex.size);
    memset(linear, 0, tw * th * sizeof(u32));
    for (unsigned y = 0; y < height; ++y) memcpy(linear + y * tw, pixels + y * stride, width * sizeof(u32));
    GSPGPU_FlushDataCache(linear, tw * th * sizeof(u32));
    C3D_SyncDisplayTransfer(linear, GX_BUFFER_DIM(tw, th), (u32 *)picture->tex.data, GX_BUFFER_DIM(tw, th),
                            GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(1) | GX_TRANSFER_RAW_COPY(0) |
                            GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) |
                            GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGBA8) |
                            GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO));
    /* Nothing of the old contents may linger in the CPU's cache. */
    GSPGPU_InvalidateDataCache(picture->tex.data, picture->tex.size);
    linearFree(linear);
    picture->sub.width = (u16)width;
    picture->sub.height = (u16)height;
    picture->sub.left = 0.0f;
    picture->sub.top = 1.0f;
    picture->sub.right = (float)width / tw;
    picture->sub.bottom = 1.0f - (float)height / th;
    return true;
}

void ui_picture_free(UiPicture *picture)
{
    if (!picture) return;
    C3D_TexDelete(&picture->tex);
    free(picture);
}

bool ui_picture_draw(const UiPicture *picture, float x, float y, float scale, float alpha)
{
    if (!picture || !picture->sub.width) return false;
    const C2D_Image image = { (C3D_Tex *)&picture->tex, &picture->sub };
    C2D_ImageTint tint;
    C2D_AlphaImageTint(&tint, alpha);
    return C2D_DrawImageAt(image, x, y, 0.0f, alpha < 1.0f ? &tint : NULL, scale, scale);
}

bool ui_picture_draw_fade(const UiPicture *picture, float x, float y, float scale, float alpha_top,
                          float alpha_bottom, bool flip)
{
    if (!picture || !picture->sub.width) return false;
    Tex3DS_SubTexture sub = picture->sub;
    if (flip) {
        sub.top = picture->sub.bottom;
        sub.bottom = picture->sub.top;
    }
    const C2D_Image image = { (C3D_Tex *)&picture->tex, &sub };
    C2D_ImageTint tint;
    const u32 top = C2D_Color32f(1, 1, 1, alpha_top), bottom = C2D_Color32f(1, 1, 1, alpha_bottom);
    C2D_SetImageTint(&tint, C2D_TopLeft, top, 0.0f);
    C2D_SetImageTint(&tint, C2D_TopRight, top, 0.0f);
    C2D_SetImageTint(&tint, C2D_BotLeft, bottom, 0.0f);
    C2D_SetImageTint(&tint, C2D_BotRight, bottom, 0.0f);
    return C2D_DrawImageAt(image, x, y, 0.0f, &tint, scale, scale);
}

bool ui_image_fade(UiImage image, float x, float y, float w, float h, float alpha_top, float alpha_bottom,
                   bool flip)
{
    if (image >= UI_IMAGE_COUNT || !g_sheets[image] || w < 1.0f || h < 1.0f) return false;
    const C2D_Image full = C2D_SpriteSheetGetImage(g_sheets[image], 0);
    Tex3DS_SubTexture sub = *full.subtex;
    if (flip) {
        sub.top = full.subtex->bottom;
        sub.bottom = full.subtex->top;
    }
    const C2D_Image part = { full.tex, &sub };
    C2D_ImageTint tint;
    const u32 top = C2D_Color32f(1, 1, 1, alpha_top), bottom = C2D_Color32f(1, 1, 1, alpha_bottom);
    C2D_SetImageTint(&tint, C2D_TopLeft, top, 0.0f);
    C2D_SetImageTint(&tint, C2D_TopRight, top, 0.0f);
    C2D_SetImageTint(&tint, C2D_BotLeft, bottom, 0.0f);
    C2D_SetImageTint(&tint, C2D_BotRight, bottom, 0.0f);
    const bool plain = alpha_top >= 1.0f && alpha_bottom >= 1.0f;
    return C2D_DrawImageAt(part, x, y, 0.0f, plain ? NULL : &tint, w / sub.width, h / sub.height);
}

bool ui_image_part(UiImage image, float x, float y, float w, float h, float u0, float v0, float u1, float v1,
                   float alpha)
{
    if (image >= UI_IMAGE_COUNT || !g_sheets[image] || w < 1.0f || h < 1.0f) return false;
    const C2D_Image full = C2D_SpriteSheetGetImage(g_sheets[image], 0);
    const Tex3DS_SubTexture *f = full.subtex;
    Tex3DS_SubTexture sub = *f;
    sub.left = f->left + (f->right - f->left) * u0;
    sub.right = f->left + (f->right - f->left) * u1;
    sub.top = f->top + (f->bottom - f->top) * v0;
    sub.bottom = f->top + (f->bottom - f->top) * v1;
    sub.width = (u16)(f->width * (u1 - u0));
    sub.height = (u16)(f->height * (v1 - v0));
    const C2D_Image part = { full.tex, &sub };
    C2D_ImageTint tint;
    C2D_AlphaImageTint(&tint, alpha);
    return C2D_DrawImageAt(part, x, y, 0.0f, alpha < 1.0f ? &tint : NULL, w / sub.width, h / sub.height);
}

bool ui_icon(UiImage image, float cx, float cy, float size, u32 color)
{
    if (image >= UI_IMAGE_COUNT || !g_sheets[image]) return false;
    const C2D_Image full = C2D_SpriteSheetGetImage(g_sheets[image], 0);
    C2D_ImageTint tint;
    C2D_PlainImageTint(&tint, color, 1.0f);
    const float scale = size / full.subtex->width;
    return C2D_DrawImageAt(full, cx - size / 2, cy - size / 2, 0.0f, &tint, scale, scale);
}

void ui_gradient(float x, float y, float w, float h, u32 top, u32 bottom)
{
    if (w > 0 && h > 0) C2D_DrawRectangle(x, y, 0.0f, w, h, top, top, bottom, bottom);
}

void ui_gradient_corners(float x, float y, float w, float h, u32 top_left, u32 top_right,
                         u32 bottom_left, u32 bottom_right)
{
    if (w > 0 && h > 0) C2D_DrawRectangle(x, y, 0.0f, w, h, top_left, top_right, bottom_left, bottom_right);
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

void ui_rect(float x, float y, float w, float h, u32 color)
{
    if (w > 0 && h > 0) C2D_DrawRectSolid(x, y, 0.0f, w, h, color);
}

void ui_circle(float cx, float cy, float r, u32 color) { C2D_DrawCircleSolid(cx, cy, 0.0f, r, color); }

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

static float japanese_size(const char *text, float size, float *dy);

/* Every text drawn or measured here goes through i18n_text first, so the
 * screens pass English and get the chosen language (i18n.h). */
float ui_text_width(const char *text, float size)
{
    if (!text) return 0.0f;
    text = i18n_text(text);
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

/* The system font's glyphs are 30 px. Kanji shrunk to 11-13 px (a 0.37-0.43
 * scale) blur their strokes together; exactly half size samples cleanly.
 * Small Japanese text is therefore drawn at 15 px, and full-size text
 * snaps to 30 px. Returns the size to use and how far to move y so the
 * text stays centred where the caller placed it. */
static float japanese_size(const char *text, float size, float *dy)
{
    *dy = 0.0f;
    if (!ui_text_is_japanese(text)) return size;
    float clean = size;
    if (size >= 10.5f && size < 15.0f) clean = 15.0f;
    else if (size >= 24.0f && size < 30.0f) clean = 30.0f;
    *dy = (size - clean) / 2.0f;
    return clean;
}

float ui_text(float x, float y, float size, u32 color, UiAlign align, const char *text)
{
    if (!text || !text[0]) return 0.0f;
    text = i18n_text(text);
    if (!g_has_japanese && ui_text_is_japanese(text)) return 0.0f;
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

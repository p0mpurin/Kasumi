#pragma once

/* The drawing layer: OLED-black theme with a Japanese palette. Everything
 * here is stateless drawing; screens.c composes it. Each console draws it
 * with its own GPU library (source/3ds/ui.c: citro2d). */

#include <stdbool.h>

#include "platform.h"

#define UI_TOP_WIDTH 400.0f
#define UI_BOTTOM_WIDTH 320.0f
#define UI_HEIGHT 240.0f

/* Colours are 8-bit RGBA packed with red in the low byte (citro2d's and
 * vita2d's layout). */
#define UI_RGBA(r, g, b, a)     ((u32)(r) | ((u32)(g) << 8) | ((u32)(b) << 16) | ((u32)(a) << 24))

/* Palette. Pure black background for OLED-style contrast; warm washi paper
 * whites; seiji (celadon) as the single accent. Red (shu) is reserved for
 * danger and errors, kin (gold) for warnings; matcha, ai and sakura only
 * colour the PlayStation symbols. */
#define UI_BG          UI_RGBA(0x00, 0x00, 0x00, 0xFF)
#define UI_SURFACE     UI_RGBA(0x0C, 0x0C, 0x0F, 0xFF)
#define UI_RAISED      UI_RGBA(0x17, 0x17, 0x1C, 0xFF)
#define UI_LINE        UI_RGBA(0x24, 0x24, 0x2A, 0xFF)
#define UI_LINE_STRONG UI_RGBA(0x3C, 0x3C, 0x44, 0xFF)
#define UI_TEXT        UI_RGBA(0xEE, 0xEA, 0xE2, 0xFF)
/* Greys are brighter than they look on a monitor: the 3DS LCD has low
 * contrast, and dim text at small sizes simply disappears. */
#define UI_TEXT_DIM    UI_RGBA(0xC6, 0xC2, 0xBA, 0xFF)
#define UI_TEXT_FAINT  UI_RGBA(0x98, 0x95, 0x8E, 0xFF)
/* The accent follows the chosen theme (Settings > Appearance). */
extern u32 g_ui_accent, g_ui_accent_deep, g_ui_accent_ink;
#define UI_ACCENT      g_ui_accent
#define UI_ACCENT_DEEP g_ui_accent_deep
/* Text drawn on a filled accent surface. */
#define UI_ACCENT_INK  g_ui_accent_ink
#define UI_DANGER      UI_RGBA(0xE0, 0x48, 0x3C, 0xFF)
#define UI_DANGER_DEEP UI_RGBA(0x48, 0x14, 0x10, 0xFF)
#define UI_KIN         UI_RGBA(0xD8, 0xA5, 0x3F, 0xFF)
#define UI_MATCHA      UI_RGBA(0x8D, 0xB3, 0x6A, 0xFF)
#define UI_AI          UI_RGBA(0x6A, 0x8C, 0xC8, 0xFF)
#define UI_SAKURA      UI_RGBA(0xE8, 0x9A, 0xB4, 0xFF)
#define UI_SCRIM       UI_RGBA(0x00, 0x00, 0x00, 0xC8)

typedef enum { UI_ALIGN_LEFT, UI_ALIGN_CENTER, UI_ALIGN_RIGHT } UiAlign;

typedef struct { float x, y, w, h; } UiRect;

/* Art from gfx/, linked into the executable. Drawing falls back to the
 * procedural versions if a texture failed to load. */
typedef enum {
    UI_IMAGE_HERO,
    UI_IMAGE_MIST,
    UI_IMAGE_ENSO,
    UI_IMAGE_SEAL,
    UI_IMAGE_SEAL_40,
    UI_IMAGE_SEAL_16,
    UI_IMAGE_LANTERN,
    UI_IMAGE_DISCORD,
    /* Settings sections, in section order (white, tinted when drawn). */
    UI_IMAGE_SEC_CONTROLS,
    UI_IMAGE_SEC_PICTURE,
    UI_IMAGE_SEC_SOUND,
    UI_IMAGE_SEC_NETWORK,
    UI_IMAGE_SEC_SYSTEM,
    UI_IMAGE_SEC_ACCOUNT,
    /* A game without cover art (96x128). */
    UI_IMAGE_NO_COVER,
    /* The lower screen's backdrop tile: dots every 16 px, a cross every 32. */
    UI_IMAGE_GRID,
    /* Touch camera: the ensō under the finger and its ink trail (white). */
    UI_IMAGE_LOOK_RING,
    UI_IMAGE_LOOK_DOT,
    /* The game hub: each service's banner art (512x256), its emblem (white,
     * 64x64), and covers for Steam Link's own entries (96x128). */
    UI_IMAGE_SVC_GFN,
    UI_IMAGE_SVC_XBOX,
    UI_IMAGE_SVC_STEAM,
    UI_IMAGE_ICON_GFN,
    UI_IMAGE_ICON_XBOX,
    UI_IMAGE_ICON_STEAM,
    UI_IMAGE_COVER_BIGPICTURE,
    UI_IMAGE_COVER_DESKTOP,
    /* The hub's own painting, behind the cards (400x240). */
    UI_IMAGE_HUB_BACKDROP,
    UI_IMAGE_COUNT
} UiImage;

bool ui_init(void);
void ui_exit(void);

/* Each theme: an accent colour and a wallpaper for the lower screen. */
typedef enum { UI_THEME_SEIJI, UI_THEME_SAKURA, UI_THEME_KIN, UI_THEME_AI, UI_THEME_FUJI,
               UI_THEME_BENI, UI_THEME_MATCHA, UI_THEME_KAKI, UI_THEME_SUMI, UI_THEME_SHIRO,
               UI_THEME_COUNT } UiTheme;
void ui_set_theme(UiTheme theme);
/* Inside a service the accent is the service's colour; pop restores the
 * theme's. */
void ui_push_accent(u32 color);
void ui_pop_accent(void);
const char *ui_theme_name(UiTheme theme);
/* Accent colour of a theme, for swatches. */
u32 ui_theme_color(UiTheme theme);

/* The picture filter for wide video, done on the GPU while drawing:
 * sharpening 0 (off) ... 3 and colour 0 (natural) ... 2. */
void ui_set_video_look(unsigned sharpen, unsigned color);
/* Milliseconds since ui_init, for animation. */
u64 ui_ticks(void);
/* Seconds since the previous frame (clamped to 0.1 s), so animation speed
 * does not depend on how often a screen is redrawn. */
float ui_dt(void);
/* Frame-rate independent exponential approach towards target; rate is how
 * many e-foldings per second (12 settles in about a quarter second). */
float ui_approach(float current, float target, float rate);
/* 0..1 progress of an animation that started at start_ms and lasts ms. */
float ui_progress(u64 start_ms, float ms);
float ui_ease_out(float t);
/* Shift everything drawn next on the current screen (slide animations). */
void ui_offset(float dx, float dy);

bool ui_image(UiImage image, float x, float y, float scale, float alpha);
/* Stretched to w x h; alpha fades from top to bottom, and flip draws it
 * upside down (a reflection). */
bool ui_image_fit(UiImage image, float x, float y, float w, float h, float alpha);
bool ui_image_fade(UiImage image, float x, float y, float w, float h, float alpha_top, float alpha_bottom,
                   bool flip);
/* Part of an image (u0, v0 to u1, v1, fractions of it) stretched to w x h:
 * a slow pan across art inside a frame. */
bool ui_image_part(UiImage image, float x, float y, float w, float h, float u0, float v0, float u1, float v1,
                   float alpha);
/* Part of an image (u0, v0 to u1, v1, fractions of it) stretched to w x h:
 * a slow pan across art inside a frame. */
bool ui_image_part(UiImage image, float x, float y, float w, float h, float u0, float v0, float u1, float v1,
                   float alpha);
/* The emblem art tinted, centred on (cx, cy) at a size in pixels. */
bool ui_icon(UiImage image, float cx, float cy, float size, u32 color);
/* A filled rect with a vertical gradient (top colour to bottom colour). */
void ui_gradient(float x, float y, float w, float h, u32 top, u32 bottom);
/* A filled rect blending between a colour at each corner. */
void ui_gradient_corners(float x, float y, float w, float h, u32 top_left, u32 top_right,
                         u32 bottom_left, u32 bottom_right);
/* A tiling texture (UI_IMAGE_GRID) over a rect in one colour, anchored to
 * the screen so it lines up wherever it is drawn. */
void ui_texture(UiImage image, float x, float y, float w, float h, u32 color);
/* A white image drawn in one colour (its alpha kept). */
bool ui_image_tint(UiImage image, float x, float y, float scale, u32 color);
/* Rotated about its centre (cx, cy). */
bool ui_image_rotated(UiImage image, float cx, float cy, float scale, float angle, float alpha);

/* Pictures made while running (cover art, screenshots). Pixels are u32
 * 0xRRGGBBAA, rows top to bottom. Created, filled and freed on the UI
 * thread outside a frame. */
typedef struct UiPicture UiPicture;
/* Room for up to width x height pixels; NULL when out of GPU memory. */
UiPicture *ui_picture_create(unsigned width, unsigned height);
/* Shows width x height pixels (stride pixels a row) from now on; false if
 * they could not be copied in. */
bool ui_picture_set(UiPicture *picture, const u32 *pixels, unsigned width, unsigned height, unsigned stride);
void ui_picture_free(UiPicture *picture);
/* At (x, y), scale times its size; alpha 1 is opaque. */
bool ui_picture_draw(const UiPicture *picture, float x, float y, float scale, float alpha);
/* Fading from alpha_top to alpha_bottom, upside down if flip. */
bool ui_picture_draw_fade(const UiPicture *picture, float x, float y, float scale, float alpha_top,
                          float alpha_bottom, bool flip);

u32 ui_with_alpha(u32 color, u8 alpha);
u32 ui_mix(u32 a, u32 b, float t);

void ui_rect(float x, float y, float w, float h, u32 color);
void ui_rect_r(UiRect r, u32 color);
void ui_outline(float x, float y, float w, float h, float thickness, u32 color);
void ui_rounded(float x, float y, float w, float h, float radius, u32 color);
void ui_hline(float x, float y, float w, u32 color);
void ui_vline(float x, float y, float h, u32 color);
void ui_circle(float cx, float cy, float r, u32 color);
void ui_ring(float cx, float cy, float r, float thickness, u32 color, u32 inside);
void ui_line(float x0, float y0, float x1, float y1, float thickness, u32 color);
void ui_triangle(float x0, float y0, float x1, float y1, float x2, float y2, u32 color);

/* Text is sized in pixels of glyph height (the system font is 30 px). */
/* False when the system font cannot draw kana/kanji. */
bool ui_has_japanese(void);
float ui_text_width(const char *text, float size);
float ui_text(float x, float y, float size, u32 color, UiAlign align, const char *text);
float ui_textf(float x, float y, float size, u32 color, UiAlign align,
               const char *format, ...) __attribute__((format(printf, 6, 7)));
/* Single line clipped to max_width with an ellipsis. */
void ui_text_fit(float x, float y, float size, u32 color, UiAlign align,
                 float max_width, const char *text);
/* Greedy word wrap; returns lines drawn (at most max_lines). */
int ui_text_wrap(float x, float y, float size, u32 color, UiAlign align,
                 float max_width, int max_lines, float line_height, const char *text);
/* Letter-spaced uppercase label, the small "eyebrow" style. */
void ui_label(float x, float y, float size, u32 color, UiAlign align, const char *text);

/* Components. */
/* Surface card with a hairline border and a short accent tick on top. */
void ui_panel(UiRect r, u32 accent);
/* Outlined tag ("Steam", "ON"); returns its width. */
float ui_pill(float x, float y, u32 color, UiAlign align, const char *text);
/* Option indicator: count dots centred on cx, the active one lit. */
void ui_dots(float cx, float cy, unsigned count, unsigned active, u32 on, u32 off);
/* Section heading for grouped lists: small caps, kanji, hairline. */
void ui_section(float x, float y, float w, const char *jp, const char *en);
void ui_seal(float x, float y, float size);
void ui_enso(float cx, float cy, float r, u32 color);
void ui_seigaiha(float x, float y, float w, float h, float radius, u32 line, u32 fill);
void ui_wifi_icon(float x, float y, unsigned bars, u32 on, u32 off);
void ui_battery_icon(float x, float y, unsigned level, bool charging);
/* A face/shoulder button chip: "A", "B", "X", "Y", "L", "R", "START"... */
float ui_button_chip(float x, float y, const char *button, u32 color);
/* Chip plus label; returns total width. measure_only skips drawing. */
float ui_hint(float x, float y, const char *button, const char *label, bool measure_only);
/* Centered row of hints: pairs of button, label, NULL terminated. */
void ui_hint_row(float center_x, float y, const char *const *pairs);

typedef enum { UI_BUTTON_NORMAL, UI_BUTTON_PRIMARY, UI_BUTTON_DANGER, UI_BUTTON_ACTIVE } UiButtonStyle;
void ui_button(UiRect r, const char *label, const char *jp, UiButtonStyle style, bool pressed);
/* The pressable key under every button: square, on a hard lip that the
 * face sinks onto when pressed. Returns the face rect. */
UiRect ui_key(UiRect r, UiButtonStyle style, bool pressed);
/* A square box: 1 px border and corner marks (cards). On the lower screen
 * over a wallpaper it is frosted glass instead of a solid fill. */
void ui_surface(UiRect r, u32 border, u32 fill);
/* The theme's wallpaper over the whole lower screen; false if none. */
bool ui_backdrop(void);
/* Frosted glass: the blurred wallpaper behind r (lower screen only). */
bool ui_glass(UiRect r);
/* The theme's lower-screen wallpaper, only the part under r (in game,
 * where the full backdrop is not drawn). */
bool ui_wallpaper(UiRect r);
bool ui_hit(UiRect r, int x, int y);

/* PlayStation face symbols, drawn geometrically. */
void ui_ps_triangle(float cx, float cy, float size, u32 color);
void ui_ps_circle(float cx, float cy, float size, u32 color);
void ui_ps_cross(float cx, float cy, float size, u32 color);
void ui_ps_square(float cx, float cy, float size, u32 color);
/* Xbox face buttons: the letter on a disc of the button's colour. */
void ui_xbox_face(float cx, float cy, float size, char letter, u32 color);

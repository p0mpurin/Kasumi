#pragma once

/* Citro2D drawing layer: OLED-black theme with a Japanese palette.
 * Everything here is stateless drawing; screens.c composes it. */

#include <3ds.h>
#include <citro2d.h>
#include <stdbool.h>

#define UI_TOP_WIDTH 400.0f
#define UI_BOTTOM_WIDTH 320.0f
#define UI_HEIGHT 240.0f

/* Palette. Pure black background for OLED-style contrast; warm washi paper
 * whites; seiji (celadon) as the single accent. Red (shu) is reserved for
 * danger and errors, kin (gold) for warnings; matcha, ai and sakura only
 * colour the PlayStation symbols. */
#define UI_BG          C2D_Color32(0x00, 0x00, 0x00, 0xFF)
#define UI_SURFACE     C2D_Color32(0x0C, 0x0C, 0x0F, 0xFF)
#define UI_RAISED      C2D_Color32(0x17, 0x17, 0x1C, 0xFF)
#define UI_LINE        C2D_Color32(0x24, 0x24, 0x2A, 0xFF)
#define UI_LINE_STRONG C2D_Color32(0x3C, 0x3C, 0x44, 0xFF)
#define UI_TEXT        C2D_Color32(0xEE, 0xEA, 0xE2, 0xFF)
/* Greys are brighter than they look on a monitor: the 3DS LCD has low
 * contrast, and dim text at small sizes simply disappears. */
#define UI_TEXT_DIM    C2D_Color32(0xC6, 0xC2, 0xBA, 0xFF)
#define UI_TEXT_FAINT  C2D_Color32(0x98, 0x95, 0x8E, 0xFF)
/* The accent follows the chosen theme (Settings > Appearance). */
extern u32 g_ui_accent, g_ui_accent_deep, g_ui_accent_ink;
#define UI_ACCENT      g_ui_accent
#define UI_ACCENT_DEEP g_ui_accent_deep
/* Text drawn on a filled accent surface. */
#define UI_ACCENT_INK  g_ui_accent_ink
#define UI_DANGER      C2D_Color32(0xE0, 0x48, 0x3C, 0xFF)
#define UI_DANGER_DEEP C2D_Color32(0x48, 0x14, 0x10, 0xFF)
#define UI_KIN         C2D_Color32(0xD8, 0xA5, 0x3F, 0xFF)
#define UI_MATCHA      C2D_Color32(0x8D, 0xB3, 0x6A, 0xFF)
#define UI_AI          C2D_Color32(0x6A, 0x8C, 0xC8, 0xFF)
#define UI_SAKURA      C2D_Color32(0xE8, 0x9A, 0xB4, 0xFF)
#define UI_SCRIM       C2D_Color32(0x00, 0x00, 0x00, 0xC8)

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
    UI_IMAGE_COUNT
} UiImage;

bool ui_init(void);
void ui_exit(void);

typedef enum { UI_THEME_SEIJI, UI_THEME_SAKURA, UI_THEME_KIN, UI_THEME_AI, UI_THEME_FUJI,
               UI_THEME_COUNT } UiTheme;
void ui_set_theme(UiTheme theme);
const char *ui_theme_name(UiTheme theme);
/* Accent colour of a theme, for swatches. */
u32 ui_theme_color(UiTheme theme);

/* One frame: begin, draw into one or both targets, end. */
void ui_frame_begin(bool sync_vblank);
void ui_frame_end(void);
void ui_begin_top(void);
void ui_begin_bottom(void);

/* Wide video: the top screen in its 800-column mode, fed by the GPU. Upload a
 * 1024x512 linear RGB565 decoder surface (tiled into a texture by one display
 * transfer), then draw its 800x480 region at half height: bilinear sampling
 * lands exactly between each pair of rows and averages them. */
bool ui_video_upload(const void *surface);
void ui_begin_top_video(void);
void ui_draw_video(void);
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
/* Rotated about its centre (cx, cy). */
bool ui_image_rotated(UiImage image, float cx, float cy, float scale, float angle, float alpha);

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
bool ui_hit(UiRect r, int x, int y);

/* PlayStation face symbols, drawn geometrically. */
void ui_ps_triangle(float cx, float cy, float size, u32 color);
void ui_ps_circle(float cx, float cy, float size, u32 color);
void ui_ps_cross(float cx, float cy, float size, u32 color);
void ui_ps_square(float cx, float cy, float size, u32 color);

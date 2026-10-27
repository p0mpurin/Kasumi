#include "ui.h"
#include "ui_internal.h"
#include "platform.h"
#include "i18n.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* The console-independent half of ui.h: themes and accents, animation
 * timing, text layout and every component, all drawn through the
 * primitives each console's ui.c provides (ui_internal.h). */

u32 g_ui_accent = 0xFFA5BE7Eu; /* UI_RGBA packs ABGR */
u32 g_ui_accent_deep = 0xFF262E14u;
u32 g_ui_accent_ink = 0xFF212A0Eu;
static UiTheme g_theme;

UiTheme ui_theme(void) { return g_theme; }

/* 青磁 celadon, 桜 cherry, 金 gold, 藍 indigo, 藤 wisteria, 紅 crimson,
 * 抹茶 matcha, 柿 persimmon, 墨 ink, 白 white. */
static const struct {
    const char *name;
    u8 r, g, b;
} THEMES[UI_THEME_COUNT] = {
    { "Seiji", 0x7E, 0xBE, 0xA5 },
    { "Sakura", 0xE8, 0x9A, 0xB4 },
    { "Kin", 0xD8, 0xA5, 0x3F },
    { "Ai", 0x74, 0x9B, 0xD8 },
    { "Fuji", 0xA9, 0x8E, 0xD6 },
    { "Beni", 0xE2, 0x72, 0x7C },
    { "Matcha", 0x9C, 0xC2, 0x6E },
    { "Kaki", 0xEC, 0x93, 0x52 },
    { "Sumi", 0xC8, 0xC4, 0xBC },
    { "Shiro", 0xEC, 0xE8, 0xDE },
};

void ui_set_theme(UiTheme theme)
{
    if (theme >= UI_THEME_COUNT) theme = UI_THEME_AI;
    g_theme = theme;
    const u8 r = THEMES[theme].r, g = THEMES[theme].g, b = THEMES[theme].b;
    g_ui_accent = UI_RGBA(r, g, b, 0xFF);
    g_ui_accent_deep = UI_RGBA(r * 18 / 100, g * 18 / 100, b * 18 / 100, 0xFF);
    g_ui_accent_ink = UI_RGBA(r * 12 / 100, g * 12 / 100, b * 12 / 100, 0xFF);
    ui_load_wallpaper(theme);
}

static u32 g_saved_accent[3];
static bool g_accent_pushed;

void ui_push_accent(u32 color)
{
    if (!g_accent_pushed) {
        g_saved_accent[0] = g_ui_accent;
        g_saved_accent[1] = g_ui_accent_deep;
        g_saved_accent[2] = g_ui_accent_ink;
        g_accent_pushed = true;
    }
    const unsigned r = color & 0xFF, g = (color >> 8) & 0xFF, b = (color >> 16) & 0xFF;
    g_ui_accent = UI_RGBA(r, g, b, 0xFF);
    g_ui_accent_deep = UI_RGBA(r * 18 / 100, g * 18 / 100, b * 18 / 100, 0xFF);
    g_ui_accent_ink = UI_RGBA(r * 12 / 100, g * 12 / 100, b * 12 / 100, 0xFF);
}

void ui_pop_accent(void)
{
    if (!g_accent_pushed) return;
    g_ui_accent = g_saved_accent[0];
    g_ui_accent_deep = g_saved_accent[1];
    g_ui_accent_ink = g_saved_accent[2];
    g_accent_pushed = false;
}

const char *ui_theme_name(UiTheme theme)
{
    return theme < UI_THEME_COUNT ? THEMES[theme].name : "";
}

u32 ui_theme_color(UiTheme theme)
{
    if (theme >= UI_THEME_COUNT) theme = UI_THEME_AI;
    return UI_RGBA(THEMES[theme].r, THEMES[theme].g, THEMES[theme].b, 0xFF);
}

static u64 g_started_at;
static u64 g_last_frame_at;
static float g_dt;

void ui_common_init(void) { g_started_at = plat_time_ms(); }

void ui_frame_tick(void)
{
    const u64 now = plat_time_ms();
    g_dt = g_last_frame_at ? (float)(now - g_last_frame_at) / 1000.0f : 0.0f;
    if (g_dt > 0.1f) g_dt = 0.1f;
    g_last_frame_at = now;
}

u64 ui_ticks(void) { return plat_time_ms() - g_started_at; }

float ui_dt(void) { return g_dt; }

float ui_approach(float current, float target, float rate)
{
    const float step = 1.0f - expf(-rate * g_dt);
    const float next = current + (target - current) * step;
    return fabsf(next - target) < 0.05f ? target : next;
}

float ui_progress(u64 start_ms, float ms)
{
    const float t = (float)(plat_time_ms() - start_ms) / ms;
    return t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
}

float ui_ease_out(float t)
{
    const float inv = 1.0f - t;
    return 1.0f - inv * inv * inv;
}

bool ui_image_fit(UiImage image, float x, float y, float w, float h, float alpha)
{
    return ui_image_fade(image, x, y, w, h, alpha, alpha, false);
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
    ui_circle(x + r, y + r, r, color);
    ui_circle(x + w - r, y + r, r, color);
    ui_circle(x + r, y + h - r, r, color);
    ui_circle(x + w - r, y + h - r, r, color);
}

void ui_hline(float x, float y, float w, u32 color) { ui_rect(x, y, w, 1.0f, color); }
void ui_vline(float x, float y, float h, u32 color) { ui_rect(x, y, 1.0f, h, color); }

void ui_ring(float cx, float cy, float r, float thickness, u32 color, u32 inside)
{
    ui_circle(cx, cy, r, color);
    ui_circle(cx, cy, r - thickness, inside);
}

/* ---- Text ---------------------------------------------------------------- */

/* Kana and CJK ideographs are encoded with UTF-8 lead bytes E3-E9. */
bool ui_text_is_japanese(const char *text)
{
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p)
        if (*p >= 0xE3 && *p <= 0xE9) return true;
    return false;
}

float ui_textf(float x, float y, float size, u32 color, UiAlign align, const char *format, ...)
{
    char buffer[256];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), i18n_text(format), args);
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
    text = i18n_text(text);
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
    text = i18n_text(text);
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
    if (!text) return;
    text = i18n_text(text);
    const float tracking = size * 0.10f;
    char upper[128];
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)text; *p && n < sizeof(upper) - 1; ++p) {
        unsigned char c = *p;
        if (c >= 'a' && c <= 'z') c -= 32;
        /* Latin-1 small letters (C3 A0..BE, not the division sign) have
         * their capital 0x20 below: e acute -> E acute, for French, German... */
        else if (p > (const unsigned char *)text && p[-1] == 0xC3 && c >= 0xA0 && c <= 0xBE && c != 0xB7) c -= 0x20;
        upper[n++] = (char)c;
    }
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
    /* In Japanese the kanji heading says it all. */
    if (ui_has_japanese() && i18n_get() == LANG_JA && jp && jp[0]) {
        ui_text(cursor, y, 11.0f, UI_ACCENT, UI_ALIGN_LEFT, jp);
    } else if (ui_has_japanese()) {
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
    if (ui_has_japanese())
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

/* The system font has no arrow glyphs (they came out as boxes): arrow
 * chips are drawn. "◀ ▶" left and right, "▲ ▼" up and down. */
static int arrow_chip(const char *button)
{
    if (!strcmp(button, "◀ ▶") || !strcmp(button, "◀▶")) return 1;
    if (!strcmp(button, "▲ ▼") || !strcmp(button, "▲▼")) return 2;
    return 0;
}

static void draw_arrows(float cx, float cy, int kind, float s, u32 color)
{
    const float gap = s * 1.4f;
    if (kind == 1) {
        ui_triangle(cx - gap + s * 0.5f, cy - s, cx - gap + s * 0.5f, cy + s, cx - gap - s * 0.7f, cy, color);
        ui_triangle(cx + gap - s * 0.5f, cy - s, cx + gap - s * 0.5f, cy + s, cx + gap + s * 0.7f, cy, color);
    } else {
        ui_triangle(cx - gap - s, cy + s * 0.5f, cx - gap + s, cy + s * 0.5f, cx - gap, cy - s * 0.7f, color);
        ui_triangle(cx + gap - s, cy - s * 0.5f, cx + gap + s, cy - s * 0.5f, cx + gap, cy + s * 0.7f, color);
    }
}

static bool round_chip(const char *button)
{
    return strlen(button) == 1 && strchr("ABXY", button[0]);
}

/* The console's name for a shoulder button: the Vita's rear-touch triggers
 * are L2 / R2 where the 3DS has ZL / ZR. */
static const char *chip_label(const char *button)
{
#if PLAT_PS_BUTTONS
    if (!strcmp(button, "ZL")) return "L2";
    if (!strcmp(button, "ZR")) return "R2";
#endif
    return button;
}

/* A face button's mark: its letter, or its PlayStation shape by position
 * (A is the right button: Circle; B Cross, X Triangle, Y Square). */
static void face_mark(float cx, float top, float size, char letter, u32 color)
{
#if PLAT_PS_BUTTONS
    const float cy = top + size / 2.0f + 0.5f, s = size * 0.78f;
    if (letter == 'A') ui_ps_circle(cx, cy, s, color);
    else if (letter == 'B') ui_ps_cross(cx, cy, s, color);
    else if (letter == 'X') ui_ps_triangle(cx, cy + s * 0.05f, s, color);
    else ui_ps_square(cx, cy, s, color);
#else
    const char text[2] = { letter, '\0' };
    ui_text(cx, top, size, color, UI_ALIGN_CENTER, text);
#endif
}

float ui_button_chip(float x, float y, const char *button, u32 color)
{
    /* Face buttons are circles; shoulders and system buttons are pills. */
    if (round_chip(button)) {
        ui_ring(x + 7.5f, y + 7.5f, 7.5f, 1.2f, color, UI_BG);
        face_mark(x + 7.5f, y + 1.5f, 12.0f, button[0], color);
        return 15.0f;
    }
    button = chip_label(button);
    const int arrows = arrow_chip(button);
    const float width = arrows ? 28.0f : ui_text_width(button, 10.0f) + 10.0f;
    ui_rounded(x, y + 0.5f, width, 14.0f, 7.0f, color);
    ui_rounded(x + 1.0f, y + 1.5f, width - 2.0f, 12.0f, 6.0f, UI_BG);
    if (arrows) draw_arrows(x + width / 2.0f, y + 7.5f, arrows, 3.2f, color);
    else ui_text(x + width / 2.0f, y + 2.0f, 10.0f, color, UI_ALIGN_CENTER, button);
    return width;
}

/* Hint rows: the system font is sharp only at 30 px and exactly half that,
 * so a row is set at 15 px when it fits and at 12 px when it doesn't.
 * Big chips: a filled disc for face buttons, an outlined pill otherwise. */
static float hint_chip_width(const char *button, bool big)
{
    button = chip_label(button);
    if (!big) return round_chip(button) ? 15.0f : arrow_chip(button) ? 28.0f : ui_text_width(button, 10.0f) + 10.0f;
    if (round_chip(button)) return 17.0f;
    if (arrow_chip(button)) return 32.0f;
    return ui_text_width(button, 15.0f) + 12.0f;
}

static void hint_chip(float x, float y, const char *button, u32 color)
{
    const float h = 17.0f, w = hint_chip_width(button, true);
    if (round_chip(button)) {
        ui_circle(x + h / 2, y + h / 2, h / 2, color);
        face_mark(x + h / 2, y + 1.0f, 15.0f, button[0], UI_BG);
        return;
    }
    button = chip_label(button);
    ui_rounded(x, y, w, h, h / 2, color);
    ui_rounded(x + 1.0f, y + 1.0f, w - 2.0f, h - 2.0f, h / 2 - 1.0f, UI_BG);
    const int arrows = arrow_chip(button);
    if (arrows) draw_arrows(x + w / 2, y + h / 2, arrows, 3.8f, color);
    else ui_text(x + w / 2, y + 1.0f, 15.0f, color, UI_ALIGN_CENTER, button);
}

static float hint_width(const char *button, const char *label, bool big)
{
    return hint_chip_width(button, big) + (big ? 6.0f : 5.0f) + ui_text_width(label, big ? 15.0f : 12.0f);
}

float ui_hint(float x, float y, const char *button, const char *label, bool measure_only)
{
    const float total = hint_width(button, label, false);
    if (!measure_only) {
        ui_button_chip(x, y, button, UI_TEXT_DIM);
        ui_text(x + hint_chip_width(button, false) + 5.0f, y + 1.0f, 12.0f, UI_TEXT, UI_ALIGN_LEFT, label);
    }
    return total;
}

void ui_hint_row(float center_x, float y, const char *const *pairs)
{
    unsigned count = 0;
    float big_total = 0.0f;
    for (const char *const *p = pairs; p[0] && p[1]; p += 2, ++count) big_total += hint_width(p[0], p[1], true);
    if (!count) return;
    const float big_gap = 14.0f;
    big_total += big_gap * (float)(count - 1);
    /* The row is centred on its screen: the room is the screen less margins. */
    const bool big = big_total <= center_x * 2.0f - 24.0f;
    if (big) {
        float x = floorf(center_x - big_total / 2.0f);
        for (const char *const *p = pairs; p[0] && p[1]; p += 2) {
            hint_chip(x, y - 1.0f, p[0], UI_TEXT_DIM);
            const float chip = hint_chip_width(p[0], true);
            ui_text(x + chip + 6.0f, y - 1.0f, 15.0f, UI_TEXT, UI_ALIGN_LEFT, p[1]);
            x += hint_width(p[0], p[1], true) + big_gap;
        }
        return;
    }
    const float gap = 16.0f;
    float total = 0.0f;
    for (const char *const *p = pairs; p[0] && p[1]; p += 2) total += ui_hint(0, 0, p[0], p[1], true);
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
        frame(r, UI_RGBA(0xFF, 0xFF, 0xFF, 0x38));
        ui_rect(inner.x, inner.y, inner.w, 1.0f, UI_RGBA(0xFF, 0xFF, 0xFF, 0x30));
        /* Over the frame, not on top of it twice: the marks are opaque. */
        corner_marks(r, 5.0f, UI_RGBA(0xB8, 0xB8, 0xB8, 0xFF));
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
    u32 face = UI_RAISED, border = UI_LINE_STRONG, lip = UI_RGBA(0x2A, 0x2A, 0x31, 0xFF);
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
    if (ui_glass_ready()) {
        /* Liquid glass over the wallpaper: the blurred picture behind it,
         * tinted by the key's colour (solid for the main action), a light
         * rim, a sheen down from the top and a dark lip below. */
        /* The lip only shows below the face, so draw just that strip. */
        if (!pressed) ui_rect(r.x + 1.0f, r.y + face_h, r.w - 2.0f, KEY_LIP, UI_RGBA(0x00, 0x00, 0x00, 0xA0));
        ui_glass(inner);
        const u8 wash = style == UI_BUTTON_PRIMARY ? 0xE0 : style == UI_BUTTON_DANGER ? 0xB0 : 0x48;
        ui_rect(inner.x, inner.y, inner.w, inner.h, ui_with_alpha(style == UI_BUTTON_NORMAL ? UI_SURFACE : face, wash));
        if (style == UI_BUTTON_ACTIVE)
            ui_rect(inner.x, inner.y, inner.w, inner.h, ui_with_alpha(UI_ACCENT, 0x30));
        if (pressed) ui_rect(inner.x, inner.y, inner.w, inner.h, UI_RGBA(0xFF, 0xFF, 0xFF, 0x18));
        frame(top, style == UI_BUTTON_NORMAL ? UI_RGBA(0xFF, 0xFF, 0xFF, 0x48) : ui_with_alpha(border, 0xE0));
        ui_gradient(inner.x, inner.y + 1.0f, inner.w, inner.h * 0.5f - 1.0f, UI_RGBA(0xFF, 0xFF, 0xFF, 0x1C),
                    UI_RGBA(0xFF, 0xFF, 0xFF, 0x00));
        ui_rect(inner.x, inner.y, inner.w, 1.0f, UI_RGBA(0xFF, 0xFF, 0xFF, 0x60));
        corner_marks(top, 5.0f, tick);
        return top;
    }
    ui_rect(r.x, r.y + KEY_LIP, r.w, face_h, lip);
    ui_rect_r(top, border);
    ui_rect(top.x + 1.0f, top.y + 1.0f, top.w - 2.0f, top.h - 2.0f, face);
    ui_gradient(top.x + 1.0f, top.y + 1.0f, top.w - 2.0f, (top.h - 2.0f) * 0.5f,
                UI_RGBA(0xFF, 0xFF, 0xFF, sheen), UI_RGBA(0xFF, 0xFF, 0xFF, 0x00));
    ui_rect(top.x + 1.0f, top.y + 1.0f, top.w - 2.0f, 1.0f, UI_RGBA(0xFF, 0xFF, 0xFF, bevel));
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
    /* 15 px is half the font's size, the only small size that stays sharp:
     * used whenever the label fits (the kanji line too, when there's room). */
    const bool sharp = ui_text_width(label, 15.0f) <= f.w - 10.0f;
    const float label_size = sharp ? 15.0f : r.h >= 40.0f ? 14.0f : 12.0f;
    const float jp_size = sharp ? 15.0f : r.h >= 40.0f ? 12.0f : 11.0f;
    const bool room = f.h >= label_size + 2.0f + jp_size + 2.0f || !sharp;
    /* The kanji line repeats the label in Japanese: not when that is the language. */
    if (i18n_get() == LANG_JA && ui_has_japanese()) jp = NULL;
    if (jp && jp[0] && r.h >= 30.0f && room && (ui_has_japanese() || !ui_text_is_japanese(jp))) {
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

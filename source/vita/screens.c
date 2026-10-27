#include "app.h"
#include "screens_model.h"
#include "platform.h"
#include "app_paths.h"
#include "ui_vita.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "audio_output.h"
#include "game_art.h"
#include "game_prefs.h"
#include "gallery.h"
#include "http_client.h"
#include "i18n.h"
#include "menu_audio.h"
#include "mic_capture.h"
#include "mvd_video.h"
#include "play_history.h"
#include "provider.h"
#include "regions.h"
#include "remote_keyboard.h"
#include "report.h"
#include "steam_link.h"
#include "stream_profile.h"
#include "updater.h"
#include "zoom_zones.h"

/* The PS Vita's screens: one 960x544 touch screen, laid out for fingers.
 *
 * Everything is drawn on a 600x340 canvas scaled 1.6 times, so Kasumi's
 * text, chips and keys come out the same size in the hand as on a 3DS
 * (its pixels are 1.65 times coarser). Lists show whole (the library's
 * shelf, the settings rows) and every item can be tapped; buttons are at
 * least 34 canvas pixels (6 mm) tall. In a game the picture has the whole
 * screen and the menu slides over it. */

#define S 1.6f
#define CW 600.0f
#define CH 340.0f
/* The top bar and the hint bar under the content. */
#define TOP_H 30.0f
#define HINT_Y 314.0f
#define BTN_H 34.0f
/* The row of buttons at the bottom of most views. */
#define ACT_Y 272.0f

static void canvas(void) { ui_region(0.0f, 0.0f, S, CW, CH, false); }

/* Touches arrive in screen pixels; the canvas is 1.6 times smaller. */
static int to_canvas(int v) { return (int)floorf((float)v / S); }

static bool pressed(const App *app, UiRect r)
{
    return app->touching && ui_hit(r, to_canvas(app->touch_x), to_canvas(app->touch_y));
}

static float clamp01(float v) { return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v; }

static float ease_in_out(float t)
{
    return t < 0.5f ? 4.0f * t * t * t : 1.0f - powf(-2.0f * t + 2.0f, 3.0f) / 2.0f;
}

static float ease_out_back(float t)
{
    const float c1 = 1.70158f, c3 = c1 + 1.0f, u = t - 1.0f;
    return 1.0f + c3 * u * u * u + c1 * u * u;
}

static UiRect lerp_rect(UiRect a, UiRect b, float t)
{
    return (UiRect){ a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.w + (b.w - a.w) * t, a.h + (b.h - a.h) * t };
}

static float hash01(unsigned i, unsigned k)
{
    unsigned x = i * 374761393u + k * 668265263u;
    x = (x ^ (x >> 13)) * 1274126177u;
    return (float)((x ^ (x >> 16)) & 0xFFFFu) / 65536.0f;
}

/* Taps that pick from a list (read by main with the action). */
static int g_touched_service = -1, g_touched_section = -1, g_touched_pc = -1, g_touched_option_row = -1;
static int g_touched_map_field = -1, g_touched_index = -1;
int screens_touched_service(void) { return g_touched_service; }
int screens_touched_section(void) { return g_touched_section; }
int screens_touched_pc(void) { return g_touched_pc; }
int screens_touched_option_row(void) { return g_touched_option_row; }
int screens_touched_map_field(void) { return g_touched_map_field; }
int screens_touched_index(void) { return g_touched_index; }

/* ---- Animation ------------------------------------------------------------- */

/* Each view fades and rises in when it changes; overlays fade in over it. */
static struct {
    int view, overlay;
    u64 since, overlay_since;
} g_anim = { -1, -1, 0, 0 };

#define VIEW_FADE_MS 240.0f
#define OVERLAY_FADE_MS 170.0f

static float view_progress(int view, int overlay)
{
    const u64 now = plat_time_ms();
    if (g_anim.view != view) {
        g_anim.view = view;
        g_anim.since = now;
    }
    if (g_anim.overlay != overlay) {
        g_anim.overlay = overlay;
        g_anim.overlay_since = now;
    }
    return ui_ease_out(ui_progress(g_anim.since, VIEW_FADE_MS));
}

static float overlay_progress(void) { return ui_ease_out(ui_progress(g_anim.overlay_since, OVERLAY_FADE_MS)); }
static float since_view(void) { return (float)(plat_time_ms() - g_anim.since); }

/* The whole screen redraws every frame on the Vita. */
bool screens_bottom_animating(void) { return false; }

/* A card growing into its service (A on the hub) or shrinking back (B). */
#define ZOOM_MS 460.0f
static struct {
    int service;
    int dir;
    u64 at;
} g_zoom;

void screens_hub_zoom(int service, bool into)
{
    g_zoom.service = service;
    g_zoom.dir = into ? 1 : -1;
    g_zoom.at = plat_time_ms();
}

bool screens_zoom_busy(void) { return g_zoom.at && g_zoom.dir > 0 && ui_progress(g_zoom.at, ZOOM_MS) < 1.0f; }

static float zoom_amount(void)
{
    if (!g_zoom.at) return 0.0f;
    const float p = ui_progress(g_zoom.at, ZOOM_MS);
    float z;
    if (g_zoom.dir < 0) z = p >= 1.0f ? 0.0f : 1.0f - ease_in_out(p);
    else z = plat_time_ms() - g_zoom.at > (u64)ZOOM_MS + 1500 ? 0.0f : ease_in_out(p);
    if (z <= 0.0f && (g_zoom.dir < 0 || plat_time_ms() - g_zoom.at > (u64)ZOOM_MS + 1500)) g_zoom.at = 0;
    return z;
}

/* After growing into a service: 0 at the hand-over, 1 once settled. */
static float entered_amount(void)
{
    if (!g_zoom.at || g_zoom.dir < 0) return 1.0f;
    return clamp01(((float)(plat_time_ms() - g_zoom.at) - ZOOM_MS) / 560.0f);
}

/* ---- Backdrops --------------------------------------------------------------- */

#define PETAL_COLOR UI_RGBA(0xF0, 0xC4, 0xD2, 0xFF)

static void draw_petal(float x, float y, float size, float angle, float flip, u32 color)
{
    const float ux = cosf(angle) * size, uy = sinf(angle) * size;
    const float px = -uy * 0.45f * flip, py = ux * 0.45f * flip;
    ui_triangle(x - ux, y - uy, x + px, y + py, x + ux, y + uy, color);
    ui_triangle(x - ux, y - uy, x - px, y - py, x + ux, y + uy, color);
}

/* Sakura petals drifting down on the wind, turning as they fall. */
static void draw_petals(float strength)
{
    if (strength <= 0.01f) return;
    const float t = (float)ui_ticks() / 1000.0f;
    for (unsigned i = 0; i < 22; ++i) {
        const float span = CH + 30.0f;
        const float y = fmodf(t * (8.0f + 11.0f * hash01(i, 1)) + hash01(i, 2) * span, span) - 10.0f;
        if (y < TOP_H) continue;
        const float wander = sinf(t * (0.5f + hash01(i, 4)) + (float)i) * 16.0f;
        const float x = fmodf(hash01(i, 3) * (CW + 40.0f) + t * (5.0f + 7.0f * hash01(i, 7)) + wander + 600.0f,
                              CW + 40.0f) - 20.0f;
        const float spin = t * (0.6f + 1.4f * hash01(i, 6)) + (float)i;
        const float flip = 0.3f + 0.7f * fabsf(cosf(spin * 0.8f));
        const float fade = y < TOP_H + 22.0f ? (y - TOP_H) / 22.0f : 1.0f;
        const float a = strength * fade * (0.35f + 0.5f * hash01(i, 8));
        draw_petal(x, y, 2.2f + 2.0f * hash01(i, 5), spin, flip, ui_with_alpha(PETAL_COLOR, (u8)(255.0f * clamp01(a))));
    }
}

/* Art over the whole canvas, slowly drifting, filling it however shaped. */
static void draw_drift(UiImage image, float aspect, float alpha, float phase)
{
    if (alpha <= 0.01f) return;
    const float t = (float)ui_ticks() / 1000.0f + phase;
    const float zoom = 1.06f + 0.03f * sinf(t * 0.08f);
    float w = CW * zoom, h = w / aspect;
    if (h < CH * zoom) {
        h = CH * zoom;
        w = h * aspect;
    }
    ui_image_fit(image, CW / 2 - w / 2 + sinf(t * 0.06f) * 14.0f, CH / 2 - h / 2 + cosf(t * 0.05f) * 6.0f, w, h, alpha);
}

static float g_hub_pos = -1.0f;

static void draw_hub_backdrop(const App *app)
{
    if (g_hub_pos < 0.0f) g_hub_pos = (float)app->hub_index;
    g_hub_pos = ui_approach(g_hub_pos, (float)app->hub_index, 9.0f);
    const int a = (int)floorf(g_hub_pos);
    const float f = g_hub_pos - (float)a;
    draw_drift(UI_IMAGE_HUB_BACKDROP, 400.0f / 240.0f, 1.0f, 0.0f);
    draw_drift(SERVICE_ART[a], 2.0f, 0.26f, (float)a);
    if (f > 0.01f && a + 1 < SERVICE_COUNT) draw_drift(SERVICE_ART[a + 1], 2.0f, 0.26f * f, (float)(a + 1));
    ui_gradient(0, 0, CW, 150, ui_with_alpha(UI_BG, 0xB4), ui_with_alpha(UI_BG, 0x30));
    ui_gradient(0, 150, CW, CH - 150, ui_with_alpha(UI_BG, 0x30), ui_with_alpha(UI_BG, 0xEC));
    draw_petals(0.8f);
}

static void draw_service_backdrop(int s, float base)
{
    const float e = ui_ease_out(entered_amount());
    draw_drift(SERVICE_ART[s], 2.0f, base + (1.0f - base) * (1.0f - e), (float)s);
    ui_gradient(0, 0, CW, 110, ui_with_alpha(UI_BG, (u8)(0xC8 * e)), ui_with_alpha(UI_BG, (u8)(0x50 * e)));
    ui_gradient(0, 110, CW, CH - 110, ui_with_alpha(UI_BG, (u8)(0x50 * e)), ui_with_alpha(UI_BG, (u8)(0xEC * e)));
    draw_petals(0.4f * e);
}

/* The theme's wallpaper, calmed, behind settings and the guides. */
static void draw_wallpaper_backdrop(void)
{
    const float t = (float)ui_ticks() / 1000.0f;
    ui_region(0.0f, 0.0f, CW * S / UI_BOTTOM_WIDTH, UI_BOTTOM_WIDTH, CH * UI_BOTTOM_WIDTH / CW, true);
    ui_offset(sinf(t * 0.05f) * 4.0f, 0.0f);
    const bool wall = ui_backdrop();
    ui_offset(0.0f, 0.0f);
    canvas();
    if (wall) {
        ui_rect(0, 0, CW, CH, UI_RGBA(0x00, 0x00, 0x00, 0x90));
    } else {
        ui_gradient(0, 0, CW, 160, ui_with_alpha(UI_ACCENT, 0x18), ui_with_alpha(UI_ACCENT, 0x00));
        ui_texture(UI_IMAGE_GRID, 0, 0, CW, CH, UI_RGBA(0xFF, 0xFF, 0xFF, 0x30));
    }
}

static const GfnGame *g_shown_cover, *g_previous_cover;
static u64 g_cover_changed_at;

/* The focused game's cover, huge and soft, behind its shelf or page. */
static void draw_cover_backdrop(const GfnGame *game, float alpha)
{
    if (game != g_shown_cover) {
        g_previous_cover = g_shown_cover;
        g_shown_cover = game;
        g_cover_changed_at = plat_time_ms();
    }
    const float f = ui_ease_out(ui_progress(g_cover_changed_at, 400.0f));
    const float t = (float)ui_ticks() / 1000.0f;
    const float w = CW * 1.08f, scale = w / GAME_ART_WIDTH, h = GAME_ART_HEIGHT * scale;
    const float x = CW / 2 - w / 2 + sinf(t * 0.07f) * 10.0f, y = CH / 2 - h * 0.42f;
    const GfnGame *layers[2] = { f < 1.0f ? g_previous_cover : NULL, g_shown_cover };
    const float alphas[2] = { alpha * (1.0f - f), alpha * f };
    for (int i = 0; i < 2; ++i) {
        const GfnGame *g = layers[i];
        if (!g || alphas[i] <= 0.01f) continue;
        const UiImage special = special_cover(g);
        if (special != UI_IMAGE_COUNT) ui_image_fit(special, x, y, w, h, alphas[i]);
        else game_art_draw(g, x, y, scale, alphas[i]);
    }
}

/* ---- Common pieces -------------------------------------------------------------- */

static void draw_title(float cx, float y, const char *jp, const char *en)
{
    if (ui_has_japanese() && jp && jp[0]) {
        ui_text(cx, y - 2.0f, 13.0f, UI_ACCENT, UI_ALIGN_CENTER, jp);
        ui_label(cx, y + 14.0f, 12.0f, UI_TEXT, UI_ALIGN_CENTER, en);
    } else {
        ui_label(cx, y + 6.0f, 13.0f, UI_TEXT, UI_ALIGN_CENTER, en);
    }
}

/* A heading at the left: kanji, then the English in small caps. */
static void draw_heading(float x, float y, const char *jp, const char *en, u32 color)
{
    float cursor = x;
    if (ui_has_japanese() && jp && jp[0]) cursor += ui_text(cursor, y, 13.0f, color, UI_ALIGN_LEFT, jp) + 7.0f;
    ui_label(cursor, y + 1.0f, 12.0f, ui_has_japanese() && jp && jp[0] ? UI_TEXT_DIM : color, UI_ALIGN_LEFT, en);
}

static void draw_hints(const char *const *hints)
{
    ui_gradient(0, HINT_Y - 10.0f, CW, CH - HINT_Y + 10.0f, ui_with_alpha(UI_BG, 0x00), ui_with_alpha(UI_BG, 0xD8));
    ui_hint_row(CW / 2, HINT_Y + 8.0f, hints);
}

static void draw_card(UiRect r, float p)
{
    ui_rect_r(r, UI_SURFACE);
    ui_outline(r.x, r.y, r.w, r.h, 1.0f, UI_LINE_STRONG);
    ui_rect(r.x, r.y, r.w * p, 2, UI_ACCENT);
}

static void draw_arrow(UiRect r, int direction, bool enabled, bool is_pressed)
{
    UiRect f = r;
    if (enabled) f = ui_key(r, UI_BUTTON_NORMAL, is_pressed);
    else {
        ui_rect_r(r, UI_LINE);
        ui_rect(r.x + 1, r.y + 1, r.w - 2, r.h - 2, UI_BG);
    }
    const float cx = f.x + f.w / 2 + (is_pressed ? (float)direction : 0.0f), cy = f.y + f.h / 2;
    const u32 color = enabled ? UI_TEXT : UI_LINE_STRONG;
    if (direction < 0) ui_triangle(cx + 4, cy - 7, cx + 4, cy + 7, cx - 5, cy, color);
    else ui_triangle(cx - 4, cy - 7, cx - 4, cy + 7, cx + 5, cy, color);
}

/* A row of buttons, the same width, centred. */
static UiRect row_button(int i, int count, float y, float width, float gap)
{
    const float total = width * (float)count + gap * (float)(count - 1);
    return (UiRect){ floorf(CW / 2 - total / 2 + (width + gap) * (float)i), y, width, BTN_H };
}

static void draw_tag_at(float x, float y, const char *text, u32 color, bool filled, float *width)
{
    const float w = floorf(ui_text_width(text, 13) + 12.0f), h = 18.0f;
    if (filled) {
        ui_rect(x, y, w, h, color);
        ui_text(x + w / 2, y + 2, 13, UI_BG, UI_ALIGN_CENTER, text);
    } else {
        ui_outline(x, y, w, h, 1.0f, color);
        ui_text(x + w / 2, y + 2, 13, color, UI_ALIGN_CENTER, text);
    }
    if (width) *width = w;
}

/* ---- The top bar ----------------------------------------------------------------- */

/* The way back from where you are: tapped, it does what B does. */
static const UiRect TOP_BACK = { 6, 3, 104, 24 };

static AppAction back_action(const App *app)
{
    if (app->busy || app->modal != MODAL_NONE || app->guide_page >= 0 || app->whats_new_open || app->discord_open)
        return ACTION_NONE;
    if (app->update_open) return ACTION_UPDATE_CLOSE;
    switch (app->view) {
    case VIEW_LIBRARY: return app->pc_sheet_open ? ACTION_PC_CLOSE : ACTION_HUB;
    case VIEW_DETAILS:
        if (app->mapping_open) return ACTION_MAP_CANCEL;
        if (app->options_open) return ACTION_OPTIONS_CLOSE;
        return app->shortcut_sheet == SHORTCUT_SHEET_NONE ? ACTION_BACK : ACTION_NONE;
    case VIEW_SETTINGS:
        if (app->mapping_open) return ACTION_MAP_CANCEL;
        if (app->gallery_open) return ACTION_GALLERY_CLOSE;
        return ACTION_BACK;
    case VIEW_LOGIN: return ACTION_CANCEL;
    default: return ACTION_NONE;
    }
}

static const char *back_label(const App *app)
{
    if (app->view == VIEW_LIBRARY && !app->pc_sheet_open) return "HOME";
    if (app->view == VIEW_LOGIN) return "CANCEL";
    return "BACK";
}

static void draw_top_bar(const App *app)
{
    ui_gradient(0, 0, CW, TOP_H + 6, ui_with_alpha(UI_BG, 0xD8), ui_with_alpha(UI_BG, 0x60));
    const bool toast = app->toast != NULL;
    const AppAction back = back_action(app);
    if (back != ACTION_NONE) {
        if (pressed(app, TOP_BACK)) ui_rect_r(TOP_BACK, ui_with_alpha(UI_TEXT, 0x24));
        ui_triangle(16, 15, 23, 9, 23, 21, UI_TEXT);
        ui_label(30, 9, 12, UI_TEXT, UI_ALIGN_LEFT, back_label(app));
    } else {
        ui_seal(12, 6, 18);
        ui_text(36, 7, 14, UI_TEXT, UI_ALIGN_LEFT, APP_NAME);
    }
    /* The middle: a message, or where you are. */
    if (toast) {
        const float w = fminf(ui_text_width(app->toast, 13) + 28.0f, 340.0f);
        ui_rect(CW / 2 - w / 2, 4, w, 22, UI_ACCENT_DEEP);
        ui_outline(CW / 2 - w / 2, 4, w, 22, 1.0f, UI_ACCENT);
        ui_text_fit(CW / 2, 7, 13, UI_TEXT, UI_ALIGN_CENTER, w - 16, app->toast);
    } else if (app->view == VIEW_LIBRARY || app->view == VIEW_DETAILS || app->view == VIEW_SESSION) {
        const int s = app_service(app);
        const float w = ui_text_width(SERVICE_INFO[s].name, 14);
        ui_icon(SERVICE_ICON[s], CW / 2 - w / 2 - 12, 15, 16, service_light(s));
        ui_text(CW / 2 + 2, 7, 14, UI_TEXT, UI_ALIGN_CENTER, SERVICE_INFO[s].name);
    } else if (back != ACTION_NONE) {
        const float w = ui_text_width(APP_NAME, 14);
        ui_seal(CW / 2 - w / 2 - 14, 6, 18);
        ui_text(CW / 2 + 9, 7, 14, UI_TEXT, UI_ALIGN_CENTER, APP_NAME);
    }
    /* The right: clock, battery, Wi-Fi, and a waiting update. */
    char clock[8] = "--:--";
    const time_t now = time(NULL);
    const struct tm *local = localtime(&now);
    if (local) strftime(clock, sizeof(clock), "%H:%M", local);
    float x = CW - 12.0f;
    x -= ui_text(x, 8, 13, UI_TEXT, UI_ALIGN_RIGHT, clock) + 10.0f;
    ui_battery_icon(x - 22.0f, 10.0f, app->battery_level, app->charging);
    x -= 32.0f;
    ui_wifi_icon(x - 14.0f, 9.0f, app->wifi_bars, UI_TEXT, UI_LINE_STRONG);
    x -= 24.0f;
    if (updater_info().state == UPDATE_AVAILABLE && !updater_dismissed()) ui_pill(x, 7, UI_ACCENT, UI_ALIGN_RIGHT, "UPDATE");
    ui_hline(0, TOP_H, CW, ui_with_alpha(UI_LINE, 0xC0));
    ui_rect(CW / 2 - 18.0f, TOP_H, 36.0f, 1.0f, UI_ACCENT);
}

/* ---- Hub -------------------------------------------------------------------------- */

static void draw_service_card(const App *app, int s, UiRect r, float alpha, float focus, float detail)
{
    const u32 light = service_light(s);
    const float t = (float)ui_ticks() / 1000.0f;
    if (focus > 0.05f) {
        ui_rect(r.x - 5, r.y - 5, r.w + 10, r.h + 10, ui_with_alpha(light, (u8)(focus * alpha * 24.0f)));
        ui_rect(r.x - 2, r.y - 2, r.w + 4, r.h + 4, ui_with_alpha(light, (u8)(focus * alpha * 32.0f)));
    }
    ui_rect(r.x, r.y, r.w, r.h, ui_with_alpha(UI_BG, (u8)(255.0f * alpha)));
    const float pan = 0.08f;
    const float u = (0.5f + 0.5f * sinf(t * 0.13f + (float)s)) * pan;
    const float v = (0.5f + 0.5f * cosf(t * 0.11f + (float)s * 2.0f)) * pan;
    ui_image_part(SERVICE_ART[s], r.x, r.y, r.w, r.h, u, v, u + 1.0f - pan, v + 1.0f - pan, alpha);
    if (focus < 0.99f) ui_rect(r.x, r.y, r.w, r.h, ui_with_alpha(UI_BG, (u8)(alpha * (1.0f - focus) * 120.0f)));
    const float k = r.w / 280.0f;
    if (detail > 0.01f) {
        const float da = detail * alpha;
        const u8 a8 = (u8)(255.0f * da);
        ui_gradient(r.x, r.y + r.h * 0.4f, r.w, r.h * 0.6f, ui_with_alpha(UI_BG, 0), ui_with_alpha(UI_BG, (u8)(230.0f * da)));
        const float pad = 12.0f * k, icon = 26.0f * k, base = r.y + r.h - pad;
        ui_icon(SERVICE_ICON[s], r.x + pad + icon / 2, base - 18.0f * k, icon, ui_with_alpha(UI_TEXT, a8));
        const float tx = r.x + pad + icon + 9.0f * k, room = r.x + r.w - pad - tx;
        ui_text_fit(tx, base - 36.0f * k, 17.0f * k, ui_with_alpha(UI_TEXT, a8), UI_ALIGN_LEFT, room, SERVICE_INFO[s].name);
        const bool ready = app->service_ready[s];
        ui_circle(tx + 3.0f * k, base - 7.0f * k, 2.6f * k, ui_with_alpha(ready ? light : UI_TEXT_FAINT, a8));
        ui_text_fit(tx + 10.0f * k, base - 14.0f * k, 12.0f * k, ui_with_alpha(ready ? UI_TEXT : UI_TEXT_DIM, a8),
                    UI_ALIGN_LEFT, room - 10.0f * k, app->service_status[s]);
        if (ui_has_japanese()) {
            const float seal = 24.0f * k;
            ui_rect(r.x + pad, r.y + pad, seal, seal, ui_with_alpha(light, (u8)(230.0f * da)));
            ui_outline(r.x + pad + 2 * k, r.y + pad + 2 * k, seal - 4 * k, seal - 4 * k, 1.0f,
                       ui_with_alpha(UI_BG, (u8)(120.0f * da)));
            ui_text(r.x + pad + seal / 2, r.y + pad + 3.0f * k, 17.0f * k, ui_with_alpha(UI_BG, a8), UI_ALIGN_CENTER,
                    SERVICE_KANJI[s]);
        }
        if (s == SERVICE_XBOX && k > 0.9f) {
            const char *tag = app->xbox_paused ? "MAINTENANCE" : "BETA";
            const float bw = floorf(ui_text_width(tag, 13) + 12.0f), bh = 18.0f;
            const float bx = floorf(r.x + r.w - pad - bw), by = floorf(r.y + pad);
            ui_rect(bx, by, bw, bh, ui_with_alpha(app->xbox_paused ? UI_TEXT_DIM : UI_KIN, (u8)(235.0f * da)));
            ui_text(bx + bw / 2, by + 2, 13, ui_with_alpha(UI_BG, a8), UI_ALIGN_CENTER, tag);
        }
    }
    const bool lit = focus > 0.5f;
    ui_outline(r.x, r.y, r.w, r.h, 1.0f, ui_with_alpha(lit ? light : UI_TEXT, (u8)(alpha * (lit ? 210.0f : 50.0f))));
}

static u64 g_hub_shown_at;

/* The card rects on the hub's carousel (where they sit right now). */
static UiRect hub_card(int i, float pos, float *alpha, float *focus)
{
    const float d = (float)i - pos, ad = fminf(fabsf(d), 1.0f);
    const float w = 272.0f - 100.0f * ad;
    const float cx = CW / 2 + d * 216.0f, cy = 133.0f + 6.0f * ad;
    if (alpha) *alpha = 1.0f - 0.4f * ad;
    if (focus) *focus = 1.0f - ad;
    return (UiRect){ cx - w / 2, cy - w / 4, w, w / 2 };
}

static const UiRect HUB_INFO = { 64, 214, 472, 52 };

static UiRect hub_button(int i) { return row_button(i, 3, ACT_Y, 168, 14); }

static void draw_hub(const App *app)
{
    const u64 now = plat_time_ms();
    if (!g_hub_shown_at) g_hub_shown_at = now;
    const float since = (float)(now - g_hub_shown_at);
    const bool first = !app->settings.hub_done;
    const float delay = first ? 900.0f : 40.0f;
    const float z = zoom_amount();
    const int zs = g_zoom.at ? g_zoom.service : -1;
    const float chrome = clamp01((since - delay - 200.0f) / 320.0f) * (1.0f - z);
    if (first) {
        /* First run: the seal draws itself before the cards arrive. */
        const float in = ui_ease_out(clamp01(since / 520.0f)), out = clamp01((since - 800.0f) / 320.0f);
        const float a = in * (1.0f - out);
        if (a > 0.01f) {
            ui_image_rotated(UI_IMAGE_ENSO, CW / 2, 130, (0.7f + 0.3f * ease_out_back(clamp01(since / 620.0f))) * 0.9f,
                             (1.0f - in) * -2.4f, a);
            if (ui_has_japanese()) ui_text(CW / 2, 112, 34, ui_with_alpha(UI_TEXT, (u8)(255 * a)), UI_ALIGN_CENTER, "霞");
            ui_label(CW / 2, 196, 16, ui_with_alpha(UI_TEXT, (u8)(255 * a)), UI_ALIGN_CENTER, "KASUMI");
        }
    }
    if (chrome > 0.01f) {
        const u8 a = (u8)(255.0f * chrome);
        static const char *const jp = "何で遊ぶ？", *const en = "Choose where to play";
        const float jw = ui_has_japanese() ? ui_text_width(jp, 17) + 10.0f : 0.0f, ew = ui_text_width(en, 17);
        const float x0 = floorf(CW / 2 - (jw + ew) / 2.0f);
        if (ui_has_japanese()) ui_text(x0, 38, 17, ui_with_alpha(service_light(app->hub_index), a), UI_ALIGN_LEFT, jp);
        ui_text(x0 + jw, 38, 17, ui_with_alpha(UI_TEXT, a), UI_ALIGN_LEFT, en);
    }
    int order[SERVICE_COUNT];
    for (int i = 0; i < SERVICE_COUNT; ++i) order[i] = i;
    for (int i = 1; i < SERVICE_COUNT; ++i)
        for (int j = i; j > 0 && fabsf((float)order[j] - g_hub_pos) > fabsf((float)order[j - 1] - g_hub_pos); --j) {
            const int swap = order[j];
            order[j] = order[j - 1];
            order[j - 1] = swap;
        }
    for (int n = 0; n < SERVICE_COUNT; ++n) {
        const int i = order[n];
        float alpha, focus;
        UiRect r = hub_card(i, g_hub_pos, &alpha, &focus);
        const float in = ui_ease_out(clamp01((since - delay - 110.0f * fabsf((float)(i - app->hub_index))) / 480.0f));
        r.y += (1.0f - in) * 80.0f;
        alpha *= in;
        float detail = 1.0f;
        if (i == zs) {
            r = lerp_rect(r, (UiRect){ -60, 0, CW + 120, CH }, z);
            detail = 1.0f - z * 1.6f;
            focus = 1.0f;
            alpha += (1.0f - alpha) * z;
        } else if (zs >= 0) {
            r.x += ((float)i < g_hub_pos ? -1.0f : 1.0f) * z * 380.0f;
            alpha *= 1.0f - z;
        }
        if (pressed(app, r) && z <= 0.0f) ui_rect(r.x - 3, r.y - 3, r.w + 6, r.h + 6, ui_with_alpha(UI_TEXT, 0x30));
        if (alpha > 0.01f) draw_service_card(app, i, r, alpha, focus, detail < 0.0f ? 0.0f : detail);
        if (i == zs && g_zoom.dir < 0) ui_rect(r.x, r.y, r.w, r.h, ui_with_alpha(UI_BG, (u8)(z * 175.0f)));
    }
    if (zs >= 0 && g_zoom.dir > 0 && z > 0.8f) ui_rect(0, 0, CW, CH, ui_with_alpha(UI_TEXT, (u8)((z - 0.8f) / 0.2f * 48.0f)));
    if (chrome <= 0.6f) return;
    ui_offset(0.0f, (1.0f - chrome) * 20.0f);
    ui_dots(CW / 2, 206, SERVICE_COUNT, (unsigned)app->hub_index, service_light(app->hub_index), UI_LINE_STRONG);
    /* What the focused service is and how it stands. */
    const int s = app->hub_index;
    const u32 light = service_light(s);
    const GfnGame *last = hub_last_game(app);
    const UiRect info = HUB_INFO;
    ui_surface(info, ui_with_alpha(light, 0xA0), UI_SURFACE);
    ui_gradient_corners(info.x + 1, info.y + 1, info.w - 2, info.h - 2, ui_with_alpha(light, 0x40),
                        ui_with_alpha(light, 0x00), ui_with_alpha(light, 0x18), ui_with_alpha(light, 0x00));
    if (last) {
        const float cw = 34.0f, chh = GAME_ART_HEIGHT * cw / GAME_ART_WIDTH;
        const float gx = info.x + 10, gy = info.y + (info.h - chh) / 2;
        const UiImage special = special_cover(last);
        if (special != UI_IMAGE_COUNT) ui_image_fit(special, gx, gy, cw, chh, 1.0f);
        else {
            game_art_want(last);
            if (!game_art_draw(last, gx, gy, cw / GAME_ART_WIDTH, 1.0f)) ui_image_fit(UI_IMAGE_NO_COVER, gx, gy, cw, chh, 1.0f);
        }
        ui_outline(gx - 1, gy - 1, cw + 2, chh + 2, 1.0f, light);
        ui_text(gx + cw + 12, info.y + 9, 13, light, UI_ALIGN_LEFT, "Continue");
        ui_text_fit(gx + cw + 12, info.y + 27, 16, UI_TEXT, UI_ALIGN_LEFT, info.w - cw - 34, last->title);
    } else {
        const bool paused = s == SERVICE_XBOX && app->xbox_paused;
        ui_text_fit(info.x + 14, info.y + 8, 14, UI_TEXT, UI_ALIGN_LEFT, info.w - 28, SERVICE_INFO[s].name);
        ui_text_wrap(info.x + 14, info.y + 24, 12, UI_TEXT_DIM, UI_ALIGN_LEFT, info.w - 28, 2, 13,
                     paused ? "Xbox Cloud Gaming is paused for now. Back when it streams again." : SERVICE_INFO[s].about);
    }
    const char *main_label = last ? "CONTINUE" : app->service_ready[s] ? "ENTER" : "SET UP";
    ui_button(hub_button(0), "SETTINGS", "設定", UI_BUTTON_NORMAL, pressed(app, hub_button(0)));
    ui_button(hub_button(1), main_label, last ? "続ける" : "入る", UI_BUTTON_PRIMARY, pressed(app, hub_button(1)));
    ui_button(hub_button(2), "EXIT", "終了", UI_BUTTON_NORMAL, pressed(app, hub_button(2)));
    ui_offset(0.0f, 0.0f);
    static const char *const hints[] = { "◀ ▶", "Choose", "A", "Enter", "SELECT", "Settings", "START", "Exit", NULL };
    static const char *const continue_hints[] = { "◀ ▶", "Choose", "A", "Enter", "START", "Continue", NULL };
    draw_hints(last ? continue_hints : hints);
}

static AppAction touch_hub(const App *app, int x, int y)
{
    if (ui_hit(hub_button(0), x, y)) return ACTION_SETTINGS;
    if (ui_hit(hub_button(2), x, y)) return ACTION_EXIT;
    if (ui_hit(hub_button(1), x, y) || ui_hit(HUB_INFO, x, y)) {
        if (hub_last_game(app)) return ACTION_CONTINUE;
        g_touched_service = app->hub_index;
        return ACTION_SERVICE;
    }
    for (int i = 0; i < SERVICE_COUNT; ++i)
        if (ui_hit(hub_card(i, g_hub_pos < 0 ? (float)app->hub_index : g_hub_pos, NULL, NULL), x, y)) {
            g_touched_service = i;
            return ACTION_SERVICE;
        }
    return ACTION_NONE;
}

/* ---- A service: sign in ----------------------------------------------------------- */

static const UiRect SIGNIN_MAIN = { 320, 176, 252, 44 };
static const UiRect SIGNIN_HUB = { 320, 228, 122, BTN_H };
static const UiRect SIGNIN_SETTINGS = { 450, 228, 122, BTN_H };

static void draw_signin(const App *app)
{
    const int s = app_service(app);
    const u32 light = service_light(s);
    const float in = ui_ease_out(entered_amount());
    const float t = (float)ui_ticks() / 1000.0f;
    const UiRect card = { 28, 50.0f + sinf(t * 1.3f) * 2.0f, 270, 135 };
    draw_service_card(app, s, card, in, 1.0f, 1.0f);
    ui_text_wrap(28, 198, 13, UI_TEXT, UI_ALIGN_LEFT, 270, 4, 16, SERVICE_INFO[s].about);

    const GfnClient *client = app->client;
    const bool failed = client->auth_state == GFN_AUTH_ERROR && client->status[0];
    float y = 54;
    draw_heading(320, y, SERVICE_INFO[s].sign_in_jp, s == SERVICE_STEAM ? "PAIR WITH YOUR PC" : "SIGN IN", light);
    y += 24;
    char date[16], text[200];
    if (s != SERVICE_STEAM && http_clock_wrong(date, sizeof(date))) {
        snprintf(text, sizeof(text), i18n_text("Your Vita's clock says %s. Set the date and time in Settings first, "
                                               "or the sign-in fails."), date);
        ui_text_wrap(320, y, 12, UI_KIN, UI_ALIGN_LEFT, 252, 4, 15, text);
    } else if (failed) {
        ui_rect(318, y - 2, 256, 50, ui_with_alpha(UI_KIN, 0x24));
        ui_rect(318, y - 2, 2, 50, UI_KIN);
        ui_text_wrap(326, y + 2, 12, UI_KIN, UI_ALIGN_LEFT, 244, 3, 15, client->status);
    } else {
        ui_text_wrap(320, y, 12, UI_TEXT_DIM, UI_ALIGN_LEFT, 252, 3, 15,
                     s == SERVICE_STEAM ? "Turn the PC on and start Steam. Kasumi finds it on your Wi-Fi and shows a "
                                          "code to type into Steam there."
                                        : "Sign-in happens on your phone or computer: no password is typed on the Vita.");
        snprintf(text, sizeof(text), i18n_text("You need: %s"), SERVICE_INFO[s].needs);
        ui_text_wrap(320, y + 52, 12, light, UI_ALIGN_LEFT, 252, 2, 15, text);
    }
    const float pulse = 0.5f + 0.5f * sinf(t * 3.0f);
    ui_rect(SIGNIN_MAIN.x - 3, SIGNIN_MAIN.y - 3, SIGNIN_MAIN.w + 6, SIGNIN_MAIN.h + 6,
            ui_with_alpha(light, (u8)(30.0f + 50.0f * pulse)));
    ui_button(SIGNIN_MAIN, failed ? "TRY AGAIN" : SERVICE_INFO[s].sign_in, SERVICE_INFO[s].sign_in_jp, UI_BUTTON_PRIMARY,
              pressed(app, SIGNIN_MAIN));
    ui_button(SIGNIN_HUB, "HOME", "ホーム", UI_BUTTON_NORMAL, pressed(app, SIGNIN_HUB));
    ui_button(SIGNIN_SETTINGS, "SETTINGS", "設定", UI_BUTTON_NORMAL, pressed(app, SIGNIN_SETTINGS));
    static const char *const hints[] = { "A", "Set up", "B", "Home", "SELECT", "Settings", NULL };
    static const char *const steam_hints[] = { "A", "Set up", "X", "PC address", "B", "Home", NULL };
    draw_hints(s == SERVICE_STEAM ? steam_hints : hints);
}

/* ---- Sign-in code ---------------------------------------------------------------- */

static UiRect login_button(int i) { return row_button(i, 2, ACT_Y, 180, 16); }

static void draw_login(const App *app)
{
    const GfnClient *client = app->client;
    const bool steam = login_is_steam(client), xbox = login_is_xbox(client);
    const int s = login_service(client);
    const u32 light = service_light(s);
    const float since = since_view();
    ui_icon(SERVICE_ICON[s], CW / 2, 50, 22, light);
    ui_label(CW / 2, 64, 13, UI_TEXT, UI_ALIGN_CENTER, steam ? "PAIR WITH YOUR PC" : xbox ? "SIGN IN WITH XBOX" : "SIGN IN WITH NVIDIA");
    ui_text(CW / 2, 84, 13, UI_TEXT_DIM, UI_ALIGN_CENTER, steam ? "Steam asks for a code on" : "On your phone or computer, open");
    ui_text_fit(CW / 2, 102, 20, light, UI_ALIGN_CENTER, CW - 40, client->verification_uri);

    const size_t length = strlen(client->user_code);
    const float cell = 40.0f, gap = 8.0f, cell_h = 54.0f, top = 138.0f;
    float total = 0;
    for (size_t i = 0; i < length; ++i) total += (client->user_code[i] == '-' ? 14.0f : cell) + (i + 1 < length ? gap : 0);
    float x = CW / 2 - total / 2;
    const float sweep = fmodf(since / 1000.0f * 0.8f, 1.6f) * (total + 80.0f) - 40.0f + (CW / 2 - total / 2);
    for (size_t i = 0; i < length; ++i) {
        char glyph[2] = { client->user_code[i], 0 };
        if (glyph[0] == '-') {
            ui_rect(x + 3, top + cell_h / 2 - 1, 8, 2, UI_TEXT_FAINT);
            x += 14.0f + gap;
            continue;
        }
        const float drop = ease_out_back(clamp01((since - 80.0f * (float)i) / 380.0f));
        const float y = top - (1.0f - drop) * 26.0f;
        const u8 a8 = (u8)(255.0f * clamp01(drop));
        ui_rect(x, y, cell, cell_h, ui_with_alpha(UI_SURFACE, a8));
        const float near = clamp01(1.0f - fabsf(x + cell / 2 - sweep) / 44.0f);
        if (near > 0.0f) ui_rect(x, y, cell, cell_h, ui_with_alpha(light, (u8)(70.0f * near * clamp01(drop))));
        ui_outline(x, y, cell, cell_h, 1.0f, ui_with_alpha(ui_mix(UI_LINE_STRONG, light, near), a8));
        ui_rect(x, y + cell_h - 2, cell, 2, ui_with_alpha(light, a8));
        ui_text(x + cell / 2, y + 11, 30, ui_with_alpha(UI_TEXT, a8), UI_ALIGN_CENTER, glyph);
        x += cell + gap;
    }
    const char *waiting = steam ? "Type it in the Authorize Device window" : "Waiting for approval";
    const float w = ui_text_width(waiting, 13);
    ui_enso(CW / 2 - w / 2 - 14, 214, 7, light);
    ui_text(CW / 2 - w / 2 + 2, 207, 13, UI_TEXT_DIM, UI_ALIGN_LEFT, waiting);
    const long remaining = (long)(client->challenge_expires_at - (int64_t)time(NULL));
    if (remaining > 0)
        ui_textf(CW / 2, 228, 12, remaining < 60 ? UI_KIN : UI_TEXT_FAINT, UI_ALIGN_CENTER, "Code expires in %ld:%02ld",
                 remaining / 60, remaining % 60);
    ui_text_fit(CW / 2, 248, 11, UI_TEXT_FAINT, UI_ALIGN_CENTER, CW - 60,
                steam ? "The pairing stays on this Vita; the stream never leaves your Wi-Fi. Kasumi is unofficial, not made by Valve."
                : xbox ? "Your login stays on this Vita only. Kasumi is unofficial, not made by Microsoft."
                       : "Your login stays on this Vita only. Kasumi is unofficial, not made by NVIDIA.");
    ui_button(login_button(0), "NEW CODE", "再発行", UI_BUTTON_NORMAL, pressed(app, login_button(0)));
    ui_button(login_button(1), "CANCEL", "取消", UI_BUTTON_NORMAL, pressed(app, login_button(1)));
    static const char *const hints[] = { "Y", "New code", "B", "Cancel", NULL };
    draw_hints(hints);
}

/* ---- Library ------------------------------------------------------------------------- */

static void draw_cover(const GfnGame *game, float x, float y, float w, float alpha)
{
    const float scale = w / GAME_ART_WIDTH, h = GAME_ART_HEIGHT * scale;
    const UiImage special = special_cover(game);
    if (special != UI_IMAGE_COUNT && ui_image_fit(special, x, y, w, h, alpha)) return;
    game_art_want(game);
    if (game_art_draw(game, x, y, scale, alpha)) return;
    if (ui_image_fit(UI_IMAGE_NO_COVER, x, y, w, h, alpha)) {
        if (h > 70)
            ui_text_wrap(x + w / 2, y + h * 0.66f, 10, ui_with_alpha(UI_TEXT, (u8)(255.0f * alpha)), UI_ALIGN_CENTER,
                         w - 8, 3, 12, game->title);
    } else {
        ui_rect(x, y, w, h, ui_with_alpha(UI_SURFACE, (u8)(255.0f * alpha)));
    }
}

static void draw_cover_reflection(const GfnGame *game, float x, float y, float w, float alpha)
{
    const float scale = w / GAME_ART_WIDTH, h = GAME_ART_HEIGHT * scale;
    const UiImage special = special_cover(game);
    if (special != UI_IMAGE_COUNT) {
        ui_image_fade(special, x, y, w, h * 0.45f, alpha, 0.0f, true);
        return;
    }
    if (!game_art_draw_fade(game, x, y, scale, alpha, 0.0f, true))
        ui_image_fade(UI_IMAGE_NO_COVER, x, y, w, h, alpha, 0.0f, true);
}

/* The shelf: the focused cover big in the middle, its neighbours smaller
 * and fading out, gliding as the focus moves. */
#define SHELF_BOTTOM 232.0f
#define SHELF_W 116.0f
static float g_shelf_pos = -1.0f;

static UiRect shelf_cover(int i, float *alpha)
{
    const float d = (float)i - g_shelf_pos, ad = fabsf(d);
    const float scale = ad < 1.0f ? 1.0f - 0.32f * ad : fmaxf(0.46f, 0.68f - 0.07f * (ad - 1.0f));
    const float w = SHELF_W * scale, h = GAME_ART_HEIGHT * w / GAME_ART_WIDTH;
    const float off = ad <= 1.0f ? ad * 110.0f : 110.0f + (ad - 1.0f) * 76.0f;
    const float cx = CW / 2 + (d < 0.0f ? -off : off);
    if (alpha) *alpha = ad <= 1.0f ? 1.0f - 0.3f * ad : fmaxf(0.0f, 0.7f - 0.16f * (ad - 1.0f));
    return (UiRect){ cx - w / 2, SHELF_BOTTOM - h, w, h };
}

static UiRect tab_rect(int i) { return (UiRect){ 12.0f + (float)i * 100.0f, 34, 96, 22 }; }
static UiRect library_button(int i) { return row_button(i, 4, ACT_Y, 132, 10); }
static const UiRect LIB_CONTINUE = { 376, 34, 212, 22 };

static void draw_library(const App *app)
{
    const int s = app_service(app);
    const u32 light = service_light(s);
    const bool searching = app->search_text[0] != '\0';
    const size_t count = app->list_count;
    const GfnGame *focused = app_game(app, app->selected);
    const float enter_t = entered_amount();
    if (app->service_loading || !focused) draw_service_backdrop(s, 0.4f);
    else {
        draw_service_backdrop(s, 0.24f);
        draw_cover_backdrop(focused, 0.16f * ui_ease_out(enter_t));
        ui_gradient(0, 0, CW, CH, ui_with_alpha(UI_BG, 0x40), ui_with_alpha(UI_BG, 0x90));
    }

    /* Tabs (or the search), and the game to go back to. */
    if (searching) {
        char query[96];
        snprintf(query, sizeof(query), i18n_text("SEARCH  \"%s\""), app->search_text);
        ui_label(16, 38, 12, light, UI_ALIGN_LEFT, query);
    } else if (app->client->game_count) {
        static const char *const names[LIBRARY_TAB_COUNT] = { "ALL", "FAVOURITES", "RECENT" };
        for (int i = 0; i < LIBRARY_TAB_COUNT; ++i) {
            const UiRect r = tab_rect(i);
            const bool on = i == app->library_tab;
            if (pressed(app, r)) ui_rect_r(r, ui_with_alpha(UI_TEXT, 0x20));
            ui_label(r.x + r.w / 2, r.y + 4, 12, on ? UI_TEXT : UI_TEXT_FAINT, UI_ALIGN_CENTER, names[i]);
            if (on) ui_rect(r.x + 10, r.y + r.h - 2, r.w - 20, 2, light);
        }
        ui_button_chip(tab_rect(LIBRARY_TAB_COUNT).x + 4, 37, "L", UI_TEXT_FAINT);
        ui_button_chip(tab_rect(LIBRARY_TAB_COUNT).x + 24, 37, "R", UI_TEXT_FAINT);
    }
    if (app->continue_index >= 0 && !searching && (size_t)app->continue_index < app->client->game_count) {
        const GfnGame *last = &app->client->games[app->continue_index];
        const UiRect c = LIB_CONTINUE;
        ui_rect_r(c, ui_with_alpha(pressed(app, c) ? light : UI_BG, pressed(app, c) ? 0x50 : 0xA0));
        ui_outline(c.x, c.y, c.w, c.h, 1.0f, ui_with_alpha(light, 0xB0));
        ui_triangle(c.x + 9, c.y + 6, c.x + 9, c.y + 16, c.x + 17, c.y + 11, light);
        ui_text_fit(c.x + 24, c.y + 4, 12, UI_TEXT, UI_ALIGN_LEFT, c.w - 80, last->title);
        ui_button_chip(c.x + c.w - 52, c.y + 3.5f, "START", UI_TEXT_DIM);
    }

    if (app->service_loading) {
        const float pulse = 0.5f + 0.5f * sinf((float)ui_ticks() / 250.0f);
        for (int k = -2; k <= 2; ++k) {
            const float sc = k ? 0.66f : 1.0f, w = SHELF_W * sc, h = GAME_ART_HEIGHT * w / GAME_ART_WIDTH;
            const float cx = CW / 2 + (float)k * 110.0f;
            ui_rect(cx - w / 2, SHELF_BOTTOM - h, w, h, ui_with_alpha(light, (u8)(18.0f + 22.0f * pulse)));
        }
        ui_enso(CW / 2, 150, 18, light);
    } else if (!count) {
        const char *title = "No games here yet";
        const char *hint = searching ? "Try a different search." : "Press Y to load your library, or X to search.";
        GfnProvider partner;
        bool only = false;
        static char wrong[160];
        if (!searching && !app->client->game_count && provider_is_nvidia() && providers_partner_here(&partner, &only) && only) {
            char country[4];
            providers_country(country, sizeof(country));
            snprintf(wrong, sizeof(wrong), i18n_text("GeForce NOW in %s is %s's: sign out in Settings > Account and pick %s."),
                     providers_country_name(country), partner.name, partner.name);
            title = "Signed in with NVIDIA";
            hint = wrong;
        } else if (!searching && app->client->game_count && app->library_tab == LIBRARY_TAB_FAVOURITES) {
            title = "No favourites yet";
            hint = "Open a game and tap FAVOURITE to add it here.";
        } else if (!searching && app->client->game_count && app->library_tab == LIBRARY_TAB_RECENT) {
            title = "Nothing played on Kasumi yet";
            hint = "Games you play show up here, newest first.";
        }
        if (!ui_image(UI_IMAGE_LANTERN, CW / 2 - 36, 74, 0.75f, 1.0f)) ui_ring(CW / 2, 118, 28, 1.5f, UI_LINE_STRONG, UI_BG);
        ui_text(CW / 2, 164, 16, UI_TEXT, UI_ALIGN_CENTER, title);
        ui_text_wrap(CW / 2, 186, 13, UI_TEXT_DIM, UI_ALIGN_CENTER, 440, 3, 16, hint);
    } else {
        if (g_shelf_pos < 0.0f || g_shelf_pos > (float)count) g_shelf_pos = (float)app->selected;
        g_shelf_pos = ui_approach(g_shelf_pos, (float)app->selected, 13.0f);
        for (int k = 0; k <= 6; ++k) {
            const long after = (long)app->selected + k, before = (long)app->selected - k;
            if (after < (long)count) game_art_want(app_game(app, (size_t)after));
            if (k && before >= 0) game_art_want(app_game(app, (size_t)before));
        }
        int items[14];
        int n = 0;
        const int lo = (int)floorf(g_shelf_pos) - 5, hi = (int)ceilf(g_shelf_pos) + 5;
        for (int i = lo < 0 ? 0 : lo; i <= hi && i < (int)count && n < 14; ++i) items[n++] = i;
        for (int i = 1; i < n; ++i)
            for (int j = i; j > 0 && fabsf((float)items[j] - g_shelf_pos) > fabsf((float)items[j - 1] - g_shelf_pos); --j) {
                const int swap = items[j];
                items[j] = items[j - 1];
                items[j - 1] = swap;
            }
        for (int k = 0; k < n; ++k) {
            const int i = items[k];
            const GfnGame *game = app_game(app, (size_t)i);
            if (!game) continue;
            float alpha;
            UiRect r = shelf_cover(i, &alpha);
            const float d = (float)i - g_shelf_pos;
            const float appear = ui_ease_out(clamp01((enter_t - 0.06f * (d + 4.0f)) / 0.5f));
            r.x += (1.0f - appear) * 90.0f;
            alpha *= appear;
            if (alpha <= 0.01f) continue;
            const bool on = fabsf(d) < 0.5f;
            if (on) {
                ui_rect(r.x - 6, r.y - 6, r.w + 12, r.h + 12, ui_with_alpha(light, (u8)(alpha * 22.0f)));
                ui_rect(r.x - 3, r.y - 3, r.w + 6, r.h + 6, ui_with_alpha(light, (u8)(alpha * 32.0f)));
                draw_cover_reflection(game, r.x, SHELF_BOTTOM + 4, r.w, 0.30f * alpha);
            }
            draw_cover(game, r.x, r.y, r.w, alpha);
            if (pressed(app, r)) ui_rect_r(r, ui_with_alpha(UI_TEXT, 0x30));
            if (on) {
                ui_outline(r.x - 1, r.y - 1, r.w + 2, r.h + 2, 2.0f, ui_with_alpha(light, (u8)(255.0f * alpha)));
                if (game_prefs_favourite(game->app_id)) {
                    ui_circle(r.x + r.w - 10, r.y + 10, 8, ui_with_alpha(UI_BG, 0xC0));
                    ui_text(r.x + r.w - 10, r.y + 3, 12, UI_KIN, UI_ALIGN_CENTER, "★");
                }
            } else {
                ui_rect(r.x, r.y, r.w, r.h, ui_with_alpha(UI_BG, (u8)(alpha * 70.0f)));
            }
        }
        /* The focused game, its count, store and how it streams. */
        char counter[64], age[32] = "";
        library_age(app->client, age, sizeof(age));
        snprintf(counter, sizeof(counter), i18n_text("%lu / %lu%s%s"), (unsigned long)(app->selected + 1),
                 (unsigned long)count, age[0] ? "  ·  " : "", age);
        ui_gradient(0, 236, CW, 34, ui_with_alpha(UI_BG, 0x00), ui_with_alpha(UI_BG, 0xC0));
        ui_text_fit(CW / 2, 238, 17, UI_TEXT, UI_ALIGN_CENTER, CW - 40, focused->title);
        unsigned library_games = 0;
        const bool missing = searching && gfn_library_known(&library_games) && library_games && !gfn_in_library(focused);
        const bool paid = s == SERVICE_GFN && focused->needs_paid;
        char line[200];
        snprintf(line, sizeof(line), "%s  ·  %s  ·  %s", missing ? i18n_text("NOT IN LIBRARY")
                 : paid ? i18n_text("PAID MEMBERSHIP ONLY") : store_label(focused->store), stream_line(app), counter);
        ui_text_fit(CW / 2, 258, 12, missing || paid ? UI_KIN : UI_TEXT_DIM, UI_ALIGN_CENTER, CW - 40, line);
    }
    /* Buttons: the game, the library, search, settings. */
    const bool has_game = focused != NULL;
    const bool pcs = !searching && s == SERVICE_STEAM;
    ui_button(library_button(0), has_game ? "OPEN GAME" : app->client->game_count ? "SHOW ALL" : "LOAD LIBRARY",
              has_game ? "詳細" : "ライブラリ", has_game ? UI_BUTTON_PRIMARY : UI_BUTTON_NORMAL, pressed(app, library_button(0)));
    ui_button(library_button(1), searching ? "LIBRARY" : pcs ? "PCS" : "REFRESH", searching ? "ライブラリ" : pcs ? "パソコン" : "更新",
              UI_BUTTON_NORMAL, pressed(app, library_button(1)));
    ui_button(library_button(2), "SEARCH", "検索", searching ? UI_BUTTON_ACTIVE : UI_BUTTON_NORMAL, pressed(app, library_button(2)));
    ui_button(library_button(3), "SETTINGS", "設定", UI_BUTTON_NORMAL, pressed(app, library_button(3)));
    static const char *const hints[] = { "◀ ▶", "Browse", "L R", "Tab", "A", "Open", "X", "Search", "Y",
                                         "Refresh", "B", "Home", NULL };
    static const char *const search_hints[] = { "◀ ▶", "Browse", "A", "Open", "X", "Search", "B", "Library", NULL };
    draw_hints(searching ? search_hints : hints);
}

static UiRect pc_row(int i) { return (UiRect){ 150, 70.0f + (float)i * 40.0f, 300, 34 }; }
static const UiRect PC_PAIR = { 150, 232, 146, BTN_H };
static const UiRect PC_FORGET = { 304, 232, 146, BTN_H };
static const UiRect PC_CLOSE = { 150, 272, 300, BTN_H };

static void draw_pc_sheet(const App *app)
{
    char names[4][64];
    const int count = (int)steam_link_pcs(names, 4);
    ui_rect(0, TOP_H, CW, CH - TOP_H, ui_with_alpha(UI_BG, 0xE8));
    draw_card((UiRect){ 136, 40, 328, 272 }, 1.0f);
    draw_title(CW / 2, 50, "パソコン", "YOUR PCS");
    for (int i = 0; i < count; ++i)
        ui_button(pc_row(i), names[i], i == 0 ? "IN USE - TAP TO RELOAD ITS GAMES" : "STREAM FROM THIS PC",
                  i == app->pc_index ? UI_BUTTON_PRIMARY : i == 0 ? UI_BUTTON_ACTIVE : UI_BUTTON_NORMAL, pressed(app, pc_row(i)));
    if (count < 4) ui_text(CW / 2, pc_row(count).y + 10, 12, UI_TEXT_FAINT, UI_ALIGN_CENTER, "Up to four PCs; pair another one below.");
    ui_button(PC_PAIR, "PAIR ANOTHER PC", "追加", app->pc_index == count ? UI_BUTTON_PRIMARY : UI_BUTTON_NORMAL, pressed(app, PC_PAIR));
    ui_button(PC_FORGET, "FORGET THIS PC", "削除", app->pc_index == count + 1 ? UI_BUTTON_PRIMARY : UI_BUTTON_DANGER,
              pressed(app, PC_FORGET));
    ui_button(PC_CLOSE, "CLOSE", "閉じる", app->pc_index == count + 2 ? UI_BUTTON_PRIMARY : UI_BUTTON_NORMAL, pressed(app, PC_CLOSE));
    static const char *const hints[] = { "▲ ▼", "Choose", "A", "OK", "X", "PC address", "B", "Close", NULL };
    draw_hints(hints);
}

static AppAction touch_library(const App *app, int x, int y)
{
    if (app->pc_sheet_open) {
        char names[4][64];
        const int count = (int)steam_link_pcs(names, 4);
        g_touched_pc = -1;
        for (int i = 0; i < count; ++i)
            if (ui_hit(pc_row(i), x, y)) g_touched_pc = i;
        if (ui_hit(PC_PAIR, x, y)) g_touched_pc = count;
        if (ui_hit(PC_FORGET, x, y)) g_touched_pc = count + 1;
        if (ui_hit(PC_CLOSE, x, y)) return ACTION_PC_CLOSE;
        return g_touched_pc >= 0 ? ACTION_PC_ROW : ACTION_NONE;
    }
    if (!gfn_has_session(app->client)) {
        if (ui_hit(SIGNIN_MAIN, x, y)) return ACTION_SIGN_IN;
        if (ui_hit(SIGNIN_HUB, x, y)) return ACTION_HUB;
        if (ui_hit(SIGNIN_SETTINGS, x, y)) return ACTION_SETTINGS;
        return ACTION_NONE;
    }
    if (ui_hit(library_button(0), x, y)) return ACTION_PLAY;
    if (ui_hit(library_button(1), x, y)) return ACTION_LIBRARY;
    if (ui_hit(library_button(2), x, y)) return ACTION_SEARCH;
    if (ui_hit(library_button(3), x, y)) return ACTION_SETTINGS;
    if (app->continue_index >= 0 && !app->search_text[0] && ui_hit(LIB_CONTINUE, x, y)) return ACTION_CONTINUE;
    if (!app->search_text[0])
        for (int i = 0; i < LIBRARY_TAB_COUNT; ++i)
            if (ui_hit(tab_rect(i), x, y)) {
                g_touched_index = i;
                return ACTION_TAB;
            }
    /* A cover: the focused one opens, another is chosen. Nearest first. */
    const size_t count = app->list_count;
    int best = -1;
    float best_d = 1e9f;
    for (int i = (int)app->selected - 6; i <= (int)app->selected + 6; ++i) {
        if (i < 0 || (size_t)i >= count) continue;
        const UiRect r = shelf_cover(i, NULL);
        if (!ui_hit(r, x, y)) continue;
        const float d = fabsf((float)i - g_shelf_pos);
        if (d < best_d) {
            best_d = d;
            best = i;
        }
    }
    if (best < 0) return ACTION_NONE;
    if ((size_t)best == app->selected) return ACTION_PLAY;
    g_touched_index = best;
    return ACTION_SELECT_GAME;
}

/* ---- Game page ------------------------------------------------------------------------- */

static UiRect details_button(int i) { return i == 0 ? (UiRect){ 206, ACT_Y, 150, BTN_H }
                                                     : (UiRect){ 364 + (float)(i - 1) * 76, ACT_Y, 70, BTN_H }; }
/* The store arrows sit around the tags, so where depends on the tags. */
static UiRect g_store_prev, g_store_next;

static void draw_details(const App *app)
{
    const GfnGame *game = app_game(app, app->selected);
    if (!game) return;
    const int s = app_service(app);
    const u32 light = service_light(s);
    const float in = ui_ease_out(ui_progress(g_anim.since, 420.0f));
    draw_service_backdrop(s, 0.16f);
    draw_cover_backdrop(game, 0.30f * in);
    ui_gradient(0, 0, CW, CH, ui_with_alpha(UI_BG, 0x50), ui_with_alpha(UI_BG, 0xB0));
    /* The cover with its halo and reflection. */
    const float cw = 150.0f, chh = GAME_ART_HEIGHT * cw / GAME_ART_WIDTH;
    const float cx = 30.0f - (1.0f - in) * 20.0f, cy = 42.0f;
    ui_rect(cx - 6, cy - 6, cw + 12, chh + 12, ui_with_alpha(light, (u8)(22.0f * in)));
    ui_rect(cx - 3, cy - 3, cw + 6, chh + 6, ui_with_alpha(light, (u8)(32.0f * in)));
    draw_cover_reflection(game, cx, cy + chh + 4, cw, 0.24f * in);
    draw_cover(game, cx, cy, cw, in);
    ui_outline(cx - 1, cy - 1, cw + 2, chh + 2, 2.0f, ui_with_alpha(light, (u8)(255.0f * in)));

    const float x = 206.0f + (1.0f - in) * 12.0f, w = CW - 16 - x;
    const int lines = ui_text_wrap(x, 44, 22, UI_TEXT, UI_ALIGN_LEFT, w, 2, 26, game->title);
    float y = floorf(44.0f + (float)lines * 26.0f + 8.0f);
    /* Store versions as tags (the chosen one filled), arrows to switch. */
    const bool choice = s == SERVICE_GFN && game->variant_count > 1;
    float px = x + (choice ? 38.0f : 0.0f);
    if (choice) {
        g_store_prev = (UiRect){ x - 4, y - 8, 38, 34 };
        draw_arrow((UiRect){ x, y - 4, 30, 26 }, -1, true, pressed(app, g_store_prev));
        for (unsigned i = 0; i < game->variant_count; ++i) {
            const bool on = i == app->details_variant;
            float tw;
            draw_tag_at(px, y, store_label(game->variants[i].store), on ? light : UI_TEXT_FAINT, on, &tw);
            px += tw + 6;
        }
        g_store_next = (UiRect){ px - 2, y - 8, 38, 34 };
        draw_arrow((UiRect){ px + 2, y - 4, 30, 26 }, 1, true, pressed(app, g_store_next));
        px += 40;
    } else {
        float tw;
        draw_tag_at(px, y, s == SERVICE_GFN ? details_store(app, game) : SERVICE_INFO[s].name, light, true, &tw);
        px += tw + 6;
    }
    if (game_prefs_favourite(game->app_id)) draw_tag_at(px, y, "★", UI_KIN, true, NULL);
    y += 32;
    PlayHistory history = {0};
    const bool played = play_history_get(game->app_id, &history) ||
                        (app->details_variant < game->variant_count &&
                         play_history_get(game->variants[app->details_variant].id, &history));
    char line[96], value[48];
    if (played && history.seconds) {
        format_played(value, sizeof(value), history.seconds);
        snprintf(line, sizeof(line), i18n_text("Played %s on Kasumi"), value);
    } else {
        snprintf(line, sizeof(line), "%s", played ? i18n_text("Played briefly on Kasumi") : i18n_text("Not played on Kasumi yet"));
    }
    ui_text_fit(x, y, 16, UI_TEXT, UI_ALIGN_LEFT, w, line);
    y += 22;
    if (played) {
        char when[24] = "";
        if (history.last_played) {
            const time_t at = (time_t)history.last_played;
            const struct tm *t = gmtime(&at);
            if (t) strftime(when, sizeof(when), "%d %b", t);
        }
        snprintf(line, sizeof(line), i18n_text(history.sessions == 1 ? "%lu session%s%s" : "%lu sessions%s%s"),
                 (unsigned long)history.sessions, when[0] ? i18n_text("  ·  last ") : "", when);
        ui_text_fit(x, y, 14, UI_TEXT_DIM, UI_ALIGN_LEFT, w, line);
        y += 20;
    }
    const GamePrefs prefs = game_prefs_get(game->app_id);
    ui_text_fit(x, y, 14, UI_TEXT_DIM, UI_ALIGN_LEFT, w, game_prefs_custom(&prefs) ? "Its own options (tap OPTIONS to change)"
                                                                                   : "Your usual settings");
    y += 22;
    ui_text_fit(x, y, 12, UI_TEXT_FAINT, UI_ALIGN_LEFT, w, stream_line(app));
    const bool favourite = game_prefs_favourite(game->app_id);
    ui_button(details_button(0), "PLAY", "遊ぶ", UI_BUTTON_PRIMARY, pressed(app, details_button(0)));
    ui_button(details_button(1), favourite ? "SAVED" : "FAV", "お気に入り", favourite ? UI_BUTTON_ACTIVE : UI_BUTTON_NORMAL,
              pressed(app, details_button(1)));
    ui_button(details_button(2), "OPTIONS", "設定", UI_BUTTON_NORMAL, pressed(app, details_button(2)));
    static const char *const hints[] = { "A", "Play", "Y", "Favourite", "X", "Options", "B", "Back", NULL };
    if (!app->options_open && !app->mapping_open) draw_hints(hints);
}

/* The per-game options sheet, over the right of the page. */
#define OPT_X 196.0f
#define OPT_ROW_Y 70.0f
#define OPT_ROW_H 23.0f
static const UiRect OPT_CLOSE = { 494, 38, 94, 28 };

static UiRect option_row(int i) { return (UiRect){ OPT_X + 6, OPT_ROW_Y + (float)i * OPT_ROW_H, CW - OPT_X - 18, OPT_ROW_H - 3 }; }

static void draw_options_sheet(const App *app, float p)
{
    const GfnGame *game = app_game(app, app->selected);
    if (!game) return;
    const GamePrefs prefs = game_prefs_get(game->app_id);
    ui_offset((1.0f - p) * 30.0f, 0.0f);
    ui_rect(OPT_X, TOP_H, CW - OPT_X, CH - TOP_H, ui_with_alpha(UI_BG, (u8)(0xF4 * p)));
    ui_vline(OPT_X, TOP_H, CH - TOP_H, UI_ACCENT);
    draw_heading(OPT_X + 12, 40, "設定", "GAME OPTIONS", UI_ACCENT);
    ui_text_fit(OPT_X + 12, 54, 12, UI_TEXT, UI_ALIGN_LEFT, 280, game->title);
    ui_button(OPT_CLOSE, "DONE", "完了", UI_BUTTON_NORMAL, pressed(app, OPT_CLOSE));
    static const char *const labels[OPTION_COUNT] = {
        "Bitrate", "Camera stick speed", "Invert camera", "Touch camera", "Gyro aim", "Gyro speed", "Button layout",
        "Button mapping", "Connection" };
    static const char *const help[OPTION_COUNT] = {
        "Picture detail for this game. Default follows Settings.",
        "How fast the right stick turns the camera in this game.",
        "Reverse the right stick in this game. Gyro is not inverted.",
        "Turn the camera by touching the screen, in this game.",
        "Aim by turning the Vita, in this game only.",
        "How fast turning the Vita moves the camera here.",
        "Circle sends the pad's right button (position), or the bottom one (letters).",
        "Move any Vita button to any controller button. Tap to edit.",
        "",
    };
    const float value_x = OPT_X + 186, value_w = CW - 18 - value_x, value_cx = value_x + value_w / 2;
    char buffer[64];
    for (int i = 0; i < OPTION_COUNT; ++i) {
        const UiRect r = option_row(i);
        const float cy = r.y + r.h / 2;
        const bool focus = i == app->options_index;
        const bool action_row = i == OPTION_MAPPING || i == OPTION_CONNECTION;
        if (focus || pressed(app, r)) {
            ui_rect_r(r, UI_RAISED);
            ui_rect(r.x, r.y, 2, r.h, UI_ACCENT);
        }
        const bool custom = option_custom(&prefs, i);
        if (custom) ui_circle(r.x + 10, cy, 2.5f, UI_ACCENT);
        ui_text_fit(r.x + 18, r.y + 3, 13, focus ? UI_TEXT : UI_TEXT_DIM, UI_ALIGN_LEFT, value_x - r.x - 22, labels[i]);
        const char *value = option_value(app, &prefs, i, buffer, sizeof(buffer));
        const u32 value_color = custom ? UI_ACCENT : focus ? UI_TEXT : UI_TEXT_FAINT;
        if (action_row) {
            ui_text_fit(value_x + value_w - 6, r.y + 3, 13, value_color, UI_ALIGN_RIGHT, value_w - 10, value);
        } else {
            ui_text_fit(value_cx, r.y + 3, 13, value_color, UI_ALIGN_CENTER, value_w - 30, value);
            ui_triangle(value_x + 9, cy - 5, value_x + 9, cy + 5, value_x + 2, cy, focus ? UI_ACCENT : UI_LINE_STRONG);
            ui_triangle(value_x + value_w - 9, cy - 5, value_x + value_w - 9, cy + 5, value_x + value_w - 2, cy,
                        focus ? UI_ACCENT : UI_LINE_STRONG);
        }
    }
    const int index = app->options_index >= 0 && app->options_index < OPTION_COUNT ? app->options_index : 0;
    ui_hline(OPT_X + 12, 280, CW - OPT_X - 24, UI_LINE);
    ui_text_wrap(OPT_X + 12, 286, 12, UI_TEXT_DIM, UI_ALIGN_LEFT, CW - OPT_X - 24, 2, 14,
                 index == OPTION_CONNECTION ? connection_advice(app->client) : help[index]);
    ui_offset(0.0f, 0.0f);
}

/* The mapping editor: every input and what it sends on the left, the
 * chosen one's card on the right. */
static UiRect map_row(int field) { return (UiRect){ 330, 82.0f + (float)field * 56.0f, 254, 50 }; }
static UiRect map_prev(int field) { const UiRect r = map_row(field); return (UiRect){ r.x, r.y, 46, r.h }; }
static UiRect map_next(int field) { const UiRect r = map_row(field); return (UiRect){ r.x + r.w - 46, r.y, 46, r.h }; }
static UiRect map_button(int i) { return (UiRect){ 330 + (float)i * 86, ACT_Y, 80, BTN_H }; }

static void draw_face_slot(int slot, bool xbox, float cx, float cy, float size)
{
    if (xbox) {
        static const char letters[4] = { 'Y', 'B', 'A', 'X' };
        const u32 colors[4] = { UI_KIN, UI_DANGER, UI_MATCHA, UI_AI };
        ui_xbox_face(cx, cy, size, letters[slot & 3], colors[slot & 3]);
        return;
    }
    switch (slot) {
    case 0: ui_ps_triangle(cx, cy, size, UI_MATCHA); break;
    case 1: ui_ps_circle(cx, cy, size, UI_DANGER); break;
    case 2: ui_ps_cross(cx, cy, size, UI_AI); break;
    default: ui_ps_square(cx, cy, size, UI_SAKURA); break;
    }
}

static void draw_output(float cx, float y, float size, unsigned output, u32 color)
{
    const bool xbox = gfn_input_xbox_names();
    const bool face = output >= GFN_OUT_CROSS && output <= GFN_OUT_TRIANGLE;
    char name[32];
    snprintf(name, sizeof(name), xbox && face ? "%s button" : "%s", gfn_output_name(output));
    const float w = ui_text_width(name, size);
    float x = cx - (w + (face ? size + 6 : 0)) / 2;
    if (face) {
        static const int slots[4] = { 2, 1, 3, 0 };
        const float sz = size * 0.8f;
        draw_face_slot(slots[output - GFN_OUT_CROSS], xbox, x + sz / 2, y + size * 0.55f, sz);
        x += size + 6;
    }
    ui_text(x, y, size, color, UI_ALIGN_LEFT, name);
}

static void draw_mapping(const App *app)
{
    const GfnGame *game = app_game(app, app->selected);
    ui_rect(0, TOP_H, CW, CH - TOP_H, ui_with_alpha(UI_BG, 0xF4));
    draw_heading(16, 40, "ボタン設定", "BUTTON MAPPING", UI_ACCENT);
    ui_text_fit(16, 56, 12, UI_TEXT_DIM, UI_ALIGN_LEFT, 296,
                app->mapping_global ? "Every game (a game's own mapping goes first)" : game ? game->title : "");
    for (int i = 0; i < GFN_INPUT_COUNT; ++i) {
        const float x = i < 7 ? 16 : 168, y = 78 + (float)(i % 7) * 26.0f;
        const bool on = i == app->mapping_input;
        if (on) {
            ui_rect(x - 4, y - 3, 148, 23, UI_RAISED);
            ui_rect(x - 4, y - 3, 2, 23, UI_ACCENT);
        }
        ui_button_chip(x, y, gfn_input_name((unsigned)i), on ? UI_TEXT : UI_TEXT_DIM);
        char text[48];
        binding_text(text, sizeof(text), &app->mapping, i);
        const unsigned mode = app->mapping.mode[i];
        ui_text_fit(x + 52, y, 12, binding_changed(app, i) ? UI_ACCENT : on ? UI_TEXT : UI_TEXT_DIM, UI_ALIGN_LEFT,
                    mode ? 54 : 88, text);
        if (mode) ui_label(x + 142, y + 2, 9, UI_ACCENT, UI_ALIGN_RIGHT, mode == GFN_BIND_TURBO ? "TURBO" : "TOGGLE");
    }
    /* The chosen input's card. */
    const unsigned input = (unsigned)app->mapping_input;
    ui_label(457, 40, 12, UI_TEXT_FAINT, UI_ALIGN_CENTER, "PRESS A BUTTON TO PICK IT");
    const char *name = gfn_input_name(input);
    const float chip_w = strlen(name) == 1 ? 15.0f : ui_text_width(name, 10) + 10;
    ui_button_chip(457 - chip_w / 2, 58, name, UI_TEXT);
    static const char *const captions[3] = { "SENDS", "AND AT THE SAME TIME", "HOW" };
    const GfnButtonMap *m = &app->mapping, *d = &app->mapping_default;
    for (int f = 0; f < 3; ++f) {
        const UiRect r = map_row(f);
        const bool focus = f == app->mapping_field;
        ui_rect_r(r, focus ? UI_RAISED : UI_SURFACE);
        ui_outline(r.x, r.y, r.w, r.h, 1.0f, focus ? UI_ACCENT : UI_LINE);
        draw_arrow(map_prev(f), -1, true, pressed(app, map_prev(f)));
        draw_arrow(map_next(f), 1, true, pressed(app, map_next(f)));
        ui_label(r.x + r.w / 2, r.y + 5, 10, focus ? UI_ACCENT : UI_TEXT_FAINT, UI_ALIGN_CENTER, captions[f]);
        const float vy = r.y + 22, vx = r.x + r.w / 2;
        if (f == 0) draw_output(vx, vy, 15, m->out[input], m->out[input] != d->out[input] ? UI_ACCENT : UI_TEXT);
        else if (f == 1) {
            if (m->also[input] == GFN_OUT_NONE) ui_text(vx, vy, 15, UI_TEXT_FAINT, UI_ALIGN_CENTER, "Nothing more");
            else draw_output(vx, vy, 15, m->also[input], UI_ACCENT);
        } else {
            static const char *const modes[GFN_BIND_MODE_COUNT] = { "Normal", "Turbo: 10 a second", "Toggle: press once" };
            ui_text_fit(vx, vy, 15, m->mode[input] ? UI_ACCENT : UI_TEXT, UI_ALIGN_CENTER, r.w - 100,
                        modes[m->mode[input] < GFN_BIND_MODE_COUNT ? m->mode[input] : 0]);
        }
    }
    ui_button(map_button(0), "RESET", "初期化", UI_BUTTON_NORMAL, pressed(app, map_button(0)));
    ui_button(map_button(1), "CANCEL", "取消", UI_BUTTON_NORMAL, pressed(app, map_button(1)));
    ui_button(map_button(2), "SAVE", "保存", UI_BUTTON_PRIMARY, pressed(app, map_button(2)));
    static const char *const hints[] = { "ANY BUTTON", "Pick", "◀ ▶", "Change", "▲ ▼", "Row", NULL };
    draw_hints(hints);
}

static AppAction touch_mapping(int x, int y)
{
    g_touched_map_field = -1;
    for (int f = 0; f < 3; ++f) {
        if (!ui_hit(map_row(f), x, y)) continue;
        g_touched_map_field = f;
        if (ui_hit(map_prev(f), x, y)) return ACTION_MAP_PREV;
        if (ui_hit(map_next(f), x, y)) return ACTION_MAP_NEXT;
        return ACTION_MAP_FIELD;
    }
    if (ui_hit(map_button(0), x, y)) return ACTION_MAP_RESET;
    if (ui_hit(map_button(1), x, y)) return ACTION_MAP_CANCEL;
    if (ui_hit(map_button(2), x, y)) return ACTION_MAP_DONE;
    return ACTION_NONE;
}

static AppAction touch_details(const App *app, int x, int y)
{
    if (app->mapping_open) return touch_mapping(x, y);
    if (app->options_open) {
        if (ui_hit(OPT_CLOSE, x, y)) return ACTION_OPTIONS_CLOSE;
        for (int i = 0; i < OPTION_COUNT; ++i) {
            if (!ui_hit(option_row(i), x, y)) continue;
            g_touched_option_row = i;
            const float value_x = OPT_X + 186, mid = value_x + (CW - 18 - value_x) / 2;
            return (float)x < mid && i != OPTION_CONNECTION && i != OPTION_MAPPING ? ACTION_OPTION_PREV : ACTION_OPTION_NEXT;
        }
        return x < (int)OPT_X ? ACTION_OPTIONS_CLOSE : ACTION_NONE;
    }
    const GfnGame *game = app_game(app, app->selected);
    if (ui_hit(details_button(0), x, y)) return ACTION_DETAILS_PLAY;
    if (ui_hit(details_button(1), x, y)) return ACTION_FAVOURITE;
    if (ui_hit(details_button(2), x, y)) return ACTION_OPTIONS;
    if (game && app_service(app) == SERVICE_GFN && game->variant_count > 1) {
        if (ui_hit(g_store_prev, x, y)) return ACTION_VARIANT_PREV;
        if (ui_hit(g_store_next, x, y)) return ACTION_VARIANT_NEXT;
    }
    return ACTION_NONE;
}

/* ---- Settings ----------------------------------------------------------------------------- */

static UiRect section_tile(int i)
{
    return (UiRect){ 16.0f + (float)(i % 3) * 192.0f, 72.0f + (float)(i / 3) * 99.0f, 184, 92 };
}
static const UiRect SET_HOME_BACK = { 220, ACT_Y, 160, BTN_H };

/* What each section holds, as the Vita has it (no screen mode, sharpness,
 * colour or lid; the rear touch buttons instead). */
static const char *section_summary(int section)
{
    static const char *const SUMMARIES[] = {
        "Buttons, sticks, rear touch, camera, gyro",
        "Frame rate, bitrate, filter, stats",
        "Volume, music, voice, sounds, theme",
        "Connection check, type, server",
        "Language, guide, screenshots, reports",
        "Updates, provider, sign out",
    };
    return section >= 0 && section < 6 ? SUMMARIES[section] : "";
}

static void draw_settings_home(const App *app)
{
    draw_title(CW / 2, 38, "設定", "SETTINGS");
    const int count = screens_section_count();
    for (int i = 0; i < count && i < 6; ++i) {
        const UiRect r = section_tile(i);
        const bool on = i == app->settings_grid;
        const UiRect f = ui_key(r, on ? UI_BUTTON_ACTIVE : UI_BUTTON_NORMAL, pressed(app, r));
        if (!ui_image_tint((UiImage)(UI_IMAGE_SEC_CONTROLS + i), f.x + 12, f.y + 12, 0.8f, on ? UI_ACCENT : UI_TEXT_DIM))
            ui_text(f.x + 28, f.y + 14, 22, on ? UI_ACCENT : UI_TEXT_DIM, UI_ALIGN_CENTER, SECTION_INFO[i].kanji);
        const SettingEntry *h = section_header(i);
        ui_label(f.x + 58, f.y + 16, 13, on ? UI_TEXT : UI_TEXT_DIM, UI_ALIGN_LEFT, h ? h->en : "");
        ui_text_wrap(f.x + 12, f.y + 44, 12, on ? UI_TEXT_DIM : UI_TEXT_FAINT, UI_ALIGN_LEFT, f.w - 24, 3, 13,
                     section_summary(i));
    }
    ui_button(SET_HOME_BACK, "BACK", "戻る", UI_BUTTON_NORMAL, pressed(app, SET_HOME_BACK));
    static const char *const hints[] = { "◀ ▶ ▲ ▼", "Choose", "A", "Open", "B", "Save & back", NULL };
    draw_hints(hints);
}

/* Inside a section: its tabs across the top, the rows on the left, the
 * chosen row's card on the right. */
#define SET_LIST_X 12.0f
#define SET_LIST_W 300.0f
#define SET_LIST_Y 64.0f
#define SET_LIST_BOTTOM 306.0f
#define SET_ROW_H 26.0f
#define SET_CARD_X 324.0f

static UiRect section_tab(int i)
{
    const float w = (CW - 24.0f) / 6.0f;
    return (UiRect){ 12.0f + (float)i * w, 34, w, 24 };
}

static float g_set_scroll;
static int g_set_section = -1;

static UiRect setting_row(const App *app, int position)
{
    const int first = screens_section_first(app->settings_section);
    return (UiRect){ SET_LIST_X, SET_LIST_Y + (float)(position - first) * SET_ROW_H - g_set_scroll, SET_LIST_W, SET_ROW_H - 3 };
}

static const UiRect SET_PREV = { SET_CARD_X + 4, 210, 46, 40 };
static const UiRect SET_NEXT = { CW - 16 - 46, 210, 46, 40 };
static const UiRect SET_BACK = { SET_CARD_X + 54, ACT_Y, CW - 12 - SET_CARD_X - 108, BTN_H };

static void draw_settings_preview(const App *app, int setting, float cx)
{
    if (setting == SETTING_THEME) {
        for (int i = 0; i < UI_THEME_COUNT; ++i) {
            const float sx = cx + (i - (UI_THEME_COUNT - 1) / 2.0f) * 25.0f;
            if ((unsigned)i == app->settings.theme) ui_ring(sx, 189, 11, 1.5f, UI_TEXT, UI_BG);
            ui_circle(sx, 189, 7.5f, ui_theme_color((UiTheme)i));
        }
    } else if (setting == SETTING_BITRATE) {
        const float x0 = cx - 100, x1 = cx + 100, y = 184;
        ui_hline(x0, y, x1 - x0, UI_LINE_STRONG);
        ui_label(x0, y + 7, 10, UI_TEXT_FAINT, UI_ALIGN_LEFT, "SMOOTH");
        ui_label(x1, y + 7, 10, UI_TEXT_FAINT, UI_ALIGN_RIGHT, "SHARP");
        static const float stops[STREAM_BITRATE_COUNT] = { 0.4f, 0.0f, 0.25f, 0.6f, 1.0f };
        for (int i = 1; i < STREAM_BITRATE_COUNT; ++i) ui_circle(x0 + (x1 - x0) * stops[i], y, 2.0f, UI_LINE_STRONG);
        const float at = x0 + (x1 - x0) * stops[app->settings.bitrate_mode];
        ui_circle(at, y, 6.0f, UI_ACCENT);
        ui_circle(at, y, 2.5f, UI_BG);
    } else if (setting == SETTING_EXTRA_BUTTONS) {
        /* The Vita's outline, with the four zones where they are. */
        /* Front buttons are filled on the screen's edges; rear ones are
         * outlines on the body's corners, behind it. */
        const float w = 124, h = 42, x = cx - w / 2, y = 168;
        ui_rounded(x, y, w, h, 14, UI_LINE_STRONG);
        ui_rounded(x + 1, y + 1, w - 2, h - 2, 13, UI_SURFACE);
        const UiRect screen = { x + 28, y + 5, w - 56, h - 10 };
        ui_rect_r(screen, UI_BG);
        const unsigned mode = app->settings.extra_buttons;
        if (mode != EXTRA_BUTTONS_OFF) {
            const bool front = mode == EXTRA_BUTTONS_FRONT;
            static const char *const names[4] = { "L2", "L3", "R2", "R3" };
            for (int i = 0; i < 4; ++i) {
                const bool right = i >= 2, lower = i % 2;
                const float bw = front ? 16.0f : 22.0f, bh = front ? 14.0f : 17.0f;
                const float bx = front ? (right ? screen.x + screen.w - bw : screen.x) : (right ? x + w - 3 - bw : x + 3);
                const float by = front ? screen.y + 1 + (lower ? 16.0f : 0.0f) : (lower ? y + h - 3 - bh : y + 3);
                if (front) ui_rect(bx, by, bw, bh, ui_with_alpha(UI_ACCENT, 0xB0));
                else ui_outline(bx, by, bw, bh, 1.0f, UI_ACCENT);
                ui_text(bx + bw / 2, by + (bh - 9) / 2, 9, front ? UI_BG : UI_ACCENT, UI_ALIGN_CENTER, names[i]);
            }
        }
    } else if (setting == SETTING_COMMUNITY) {
        ui_image(UI_IMAGE_DISCORD, cx - 42, 170, 0.8f, 1.0f);
    }
}

static void draw_settings(const App *app)
{
    const int section = app->settings_section, sections = screens_section_count();
    for (int i = 0; i < sections && i < 6; ++i) {
        const UiRect r = section_tab(i);
        const SettingEntry *h = section_header(i);
        const bool on = i == section;
        if (pressed(app, r)) ui_rect_r(r, ui_with_alpha(UI_TEXT, 0x20));
        ui_text_fit(r.x + r.w / 2, r.y + 5, 11, on ? UI_TEXT : UI_TEXT_FAINT, UI_ALIGN_CENTER, r.w - 6, h ? h->en : "");
        if (on) ui_rect(r.x + 8, r.y + r.h - 2, r.w - 16, 2, UI_ACCENT);
    }
    ui_hline(12, 58, CW - 24, UI_LINE);
    /* The rows, the chosen one kept in view. */
    const int first = screens_section_first(section), size = screens_section_size(section);
    const float view_h = SET_LIST_BOTTOM - SET_LIST_Y, content = (float)size * SET_ROW_H;
    const float selected_y = (float)(app->setting_index - first) * SET_ROW_H;
    float target = selected_y - view_h / 2 + SET_ROW_H / 2;
    if (target > content - view_h) target = content - view_h;
    if (target < 0) target = 0;
    g_set_scroll = section != g_set_section ? target : ui_approach(g_set_scroll, target, 16.0f);
    g_set_section = section;
    ui_region(0.0f, SET_LIST_Y * S, S, CW, SET_LIST_BOTTOM - SET_LIST_Y, false);
    ui_offset(0.0f, -SET_LIST_Y);
    for (int pos = first; pos < first + size; ++pos) {
        const UiRect r = setting_row(app, pos);
        if (r.y + r.h < SET_LIST_Y - 2 || r.y > SET_LIST_BOTTOM) continue;
        const int setting = screens_setting_at(pos);
        const bool on = pos == app->setting_index;
        if (on || pressed(app, r)) {
            ui_rect_r(r, UI_RAISED);
            ui_rect(r.x, r.y, 2, r.h, UI_ACCENT);
        }
        const bool account = setting == SETTING_ACCOUNT;
        ui_text_fit(r.x + 10, r.y + 4, 13, on ? UI_TEXT : UI_TEXT_DIM, UI_ALIGN_LEFT, 150, SETTING_LABELS[setting]);
        ui_text_fit(r.x + r.w - 8, r.y + 4, 13, account ? UI_DANGER : on ? UI_ACCENT : UI_TEXT_FAINT, UI_ALIGN_RIGHT,
                    r.w - 168, setting_value(app, setting));
    }
    canvas();
    if (content > view_h) {
        const float rail = view_h, thumb = rail * view_h / content;
        ui_vline(SET_LIST_X + SET_LIST_W + 4, SET_LIST_Y, rail, UI_LINE);
        ui_rect(SET_LIST_X + SET_LIST_W + 3, SET_LIST_Y + (rail - thumb) * g_set_scroll / (content - view_h), 3, thumb, UI_ACCENT);
    }
    /* The chosen row's card. */
    const int setting = screens_setting_at(app->setting_index);
    const UiRect card = { SET_CARD_X, 64, CW - 12 - SET_CARD_X, 202 };
    ui_surface(card, UI_LINE, ui_with_alpha(UI_SURFACE, 0xE0));
    const float cx = card.x + card.w / 2;
    if (ui_has_japanese()) ui_text(cx, 72, 13, UI_ACCENT, UI_ALIGN_CENTER, SETTING_JP[setting]);
    ui_label(cx, ui_has_japanese() ? 88 : 76, 12, UI_TEXT, UI_ALIGN_CENTER, SETTING_LABELS[setting]);
    ui_text_wrap(card.x + 12, 108, 12, UI_TEXT_DIM, UI_ALIGN_LEFT, card.w - 24, 4, 14, setting_description(app, setting));
    draw_settings_preview(app, setting, cx);
    const bool account = setting == SETTING_ACCOUNT, community = setting == SETTING_COMMUNITY;
    if (!community) {
        ui_text_fit(cx, 220, 17, account ? UI_DANGER : UI_TEXT, UI_ALIGN_CENTER, card.w - 120, setting_value(app, setting));
        unsigned count = 0;
        const unsigned option = setting_option(app, setting, &count);
        if (count > 1 && count <= 12) ui_dots(cx, 254, count, option, UI_ACCENT, UI_LINE_STRONG);
        if (!account) {
            draw_arrow(SET_PREV, -1, true, pressed(app, SET_PREV));
            draw_arrow(SET_NEXT, 1, true, pressed(app, SET_NEXT));
        }
    }
    const bool sign_out = account && gfn_has_session(app->client);
    ui_button(SET_BACK, sign_out ? "SIGN OUT" : "BACK", sign_out ? "サインアウト" : "戻る",
              sign_out ? UI_BUTTON_DANGER : UI_BUTTON_NORMAL, pressed(app, SET_BACK));
    static const char *const hints[] = { "▲ ▼", "Choose", "◀ ▶", "Change", "L R", "Section", "B", "Sections", NULL };
    draw_hints(hints);
}

/* The screenshot viewer. */
static const UiRect GAL_FRAME = { 90, 38, 420, 236 };
static const UiRect GAL_PREV = { 30, 136, 46, 46 };
static const UiRect GAL_NEXT = { 524, 136, 46, 46 };
static const UiRect GAL_DELETE = { 150, ACT_Y + 6, 140, 30 };
static const UiRect GAL_BACK = { 310, ACT_Y + 6, 140, 30 };

static void draw_gallery(const App *app)
{
    ui_rect(0, TOP_H, CW, CH - TOP_H, ui_with_alpha(UI_BG, 0xF0));
    const UiRect f = GAL_FRAME;
    ui_rect(f.x - 1, f.y - 1, f.w + 2, f.h + 2, UI_LINE_STRONG);
    ui_rect_r(f, UI_BG);
    /* Pictures are kept at 400x225: drawn in a region they fill. */
    ui_region(f.x * S, f.y * S, f.w * S / 400.0f, 400, 225, false);
    if (!gallery_draw(0, 0, 400, 225)) {
        canvas();
        ui_text(CW / 2, f.y + f.h / 2 - 8, 13, UI_TEXT_FAINT, UI_ALIGN_CENTER, "Loading...");
    }
    canvas();
    const unsigned count = gallery_count();
    const bool more = count > 1;
    draw_arrow(GAL_PREV, -1, more, pressed(app, GAL_PREV));
    draw_arrow(GAL_NEXT, 1, more, pressed(app, GAL_NEXT));
    ui_textf(CW / 2, f.y + f.h + 2, 12, UI_TEXT_DIM, UI_ALIGN_CENTER, "%u / %u  ·  %s", count ? gallery_index() + 1 : 0,
             count, gallery_caption());
    ui_button(GAL_DELETE, "DELETE", "削除", UI_BUTTON_DANGER, pressed(app, GAL_DELETE));
    ui_button(GAL_BACK, "BACK", "戻る", UI_BUTTON_NORMAL, pressed(app, GAL_BACK));
}

static AppAction touch_settings(const App *app, int x, int y)
{
    if (app->mapping_open) return touch_mapping(x, y);
    if (app->gallery_open) {
        if (ui_hit(GAL_PREV, x, y)) return ACTION_GALLERY_PREV;
        if (ui_hit(GAL_NEXT, x, y)) return ACTION_GALLERY_NEXT;
        if (ui_hit(GAL_DELETE, x, y)) return ACTION_GALLERY_DELETE;
        if (ui_hit(GAL_BACK, x, y)) return ACTION_GALLERY_CLOSE;
        return ACTION_NONE;
    }
    if (app->settings_section < 0) {
        for (int i = 0; i < screens_section_count() && i < 6; ++i)
            if (ui_hit(section_tile(i), x, y)) {
                g_touched_section = i;
                return ACTION_SETTINGS_SECTION;
            }
        return ui_hit(SET_HOME_BACK, x, y) ? ACTION_BACK : ACTION_NONE;
    }
    for (int i = 0; i < screens_section_count() && i < 6; ++i)
        if (ui_hit(section_tab(i), x, y)) {
            if (i == app->settings_section) return ACTION_NONE;
            g_touched_section = i;
            return ACTION_SETTINGS_SECTION;
        }
    if (ui_hit(SET_BACK, x, y)) return ACTION_BACK;
    const int setting = screens_setting_at(app->setting_index);
    if (setting != SETTING_ACCOUNT && setting != SETTING_COMMUNITY) {
        if (ui_hit(SET_PREV, x, y)) return ACTION_VALUE_PREV;
        if (ui_hit(SET_NEXT, x, y)) return ACTION_VALUE_NEXT;
    }
    if (y >= (int)SET_LIST_Y && y < (int)SET_LIST_BOTTOM) {
        const int first = screens_section_first(app->settings_section), size = screens_section_size(app->settings_section);
        for (int pos = first; pos < first + size; ++pos) {
            if (!ui_hit(setting_row(app, pos), x, y)) continue;
            /* The chosen row again: change it, as A does. */
            if (pos == app->setting_index) return screens_setting_at(pos) == SETTING_ACCOUNT ? ACTION_NONE : ACTION_VALUE_NEXT;
            g_touched_index = pos;
            return ACTION_SELECT_SETTING;
        }
    }
    return ACTION_NONE;
}

/* ---- Session ------------------------------------------------------------------------------ */

static UiRect session_button(int i, int count) { return row_button(i, count, ACT_Y, 170, 16); }

static void draw_session(const App *app)
{
    static float progress_x;
    const GfnClient *client = app->client;
    const bool failed = screens_session_failed(app);
    const int stage = session_stage(app);
    const bool reconnecting = (app->reconnect_attempt > 0 && app->reconnect_attempt <= 3) || app->waiting_wifi;
    const int s = app_service(app);
    const u32 light = service_light(s);
    const char *service_name = SERVICE_INFO[s].name;
    draw_service_backdrop(s, 0.2f);
    const bool stopped = failed && !reconnecting && (app->reconnect_attempt == 0 || app->reconnect_attempt > 3);
    if (stopped) {
        const bool ended = client->session_state == GFN_SESSION_ERROR && !strcmp(client->fail_code, "ended");
        if (ended) {
            ui_enso(CW / 2, 92, 38, UI_ACCENT);
            ui_text(CW / 2, 76, 30, UI_TEXT, UI_ALIGN_CENTER, "終");
            draw_title(CW / 2, 140, "終了", "SESSION ENDED");
            ui_text_fit(CW / 2, 174, 15, UI_TEXT_DIM, UI_ALIGN_CENTER, CW - 80, app->game_title[0] ? app->game_title : service_name);
            ui_text_wrap(CW / 2, 196, 12, UI_TEXT_FAINT, UI_ALIGN_CENTER, CW - 120, 3, 15,
                         app->end_note[0] ? app->end_note : client->status);
        } else {
            ui_ring(CW / 2, 92, 36, 2.0f, UI_DANGER, UI_BG);
            ui_text(CW / 2, 72, 34, UI_DANGER, UI_ALIGN_CENTER, "!");
            draw_title(CW / 2, 140, "エラー", "SESSION PROBLEM");
            const char *detail = client->session_state == GFN_SESSION_ERROR ? client->status
                               : app->transport->state == WEBRTC_FAILED ? app->transport->status : app->signal->status;
            ui_text_wrap(CW / 2, 174, 12, UI_TEXT_DIM, UI_ALIGN_CENTER, CW - 120, 4, 15, detail);
        }
        ui_button(session_button(0, 2), ended ? "PLAY AGAIN" : "RETRY", "再試行", UI_BUTTON_PRIMARY, pressed(app, session_button(0, 2)));
        ui_button(session_button(1, 2), "LEAVE", "退出", UI_BUTTON_DANGER, pressed(app, session_button(1, 2)));
        static const char *const hints[] = { "A", "Retry", "B", "Leave", NULL };
        draw_hints(hints);
        return;
    }
    static const char *const kanji[] = { "待", "準", "接", "始" };
    static const char *const stages[SERVICE_COUNT][4] = {
        { "In the queue", "Preparing your rig", "Connecting", "Starting the stream" },
        { "Waiting for an Xbox", "Preparing the Xbox", "Connecting", "Starting the stream" },
        { "Waiting for the PC", "Starting on your PC", "Connecting to your PC", "Starting the stream" },
    };
    const GfnGame *game = app->session_game;
    const float since = since_view();
    const float in = ease_in_out(clamp01(since / 700.0f));
    draw_cover_backdrop(game, 0.34f * clamp01(since / 500.0f));
    ui_gradient(0, 0, CW, CH, ui_with_alpha(UI_BG, 0x40), ui_with_alpha(UI_BG, 0xA0));
    const float cw = 170.0f - 50.0f * in, chh = GAME_ART_HEIGHT * cw / GAME_ART_WIDTH;
    const float ccx = CW / 2 - 190.0f * in, ccy = 150.0f - 10.0f * in;
    for (int k = 0; k < 2; ++k) {
        const float rt = clamp01((since - 150.0f - (float)k * 260.0f) / 1100.0f);
        if (rt > 0.0f && rt < 1.0f) {
            const float r = 60.0f + rt * 200.0f;
            for (int seg = 0; seg < 48; ++seg) {
                const float a0 = (float)seg / 48.0f * 2.0f * (float)M_PI, a1 = (float)(seg + 1) / 48.0f * 2.0f * (float)M_PI;
                ui_line(ccx + cosf(a0) * r, ccy + sinf(a0) * r, ccx + cosf(a1) * r, ccy + sinf(a1) * r, 1.0f,
                        ui_with_alpha(UI_TEXT, (u8)(100.0f * (1.0f - rt))));
            }
        }
    }
    if (game) {
        const float gx = ccx - cw / 2, gy = ccy - chh / 2;
        ui_rect(gx - 6, gy - 6, cw + 12, chh + 12, ui_with_alpha(light, 24));
        draw_cover(game, gx, gy, cw, 1.0f);
        ui_outline(gx - 1, gy - 1, cw + 2, chh + 2, 2.0f, reconnecting ? UI_KIN : light);
    }
    const float words = ui_ease_out(clamp01((since - 450.0f) / 400.0f));
    const u8 wa = (u8)(255.0f * words);
    const float x = 210.0f + (1.0f - words) * 12.0f, w = CW - 20 - x;
    const char *seal = reconnecting ? "再" : kanji[stage];
    const char *what = reconnecting ? "Reconnecting" : stages[s][stage];
    if (ui_has_japanese()) {
        ui_rect(x, 44, 26, 26, ui_with_alpha(reconnecting ? UI_KIN : light, wa));
        ui_text(x + 13, 48, 17, ui_with_alpha(UI_BG, wa), UI_ALIGN_CENTER, seal);
    }
    ui_text_fit(x + (ui_has_japanese() ? 34.0f : 0.0f), 47, 18, ui_with_alpha(UI_TEXT, wa), UI_ALIGN_LEFT, w - 34, what);
    float y = 82;
    if (!reconnecting && stage == 0 && client->queue_position > 0) {
        ui_textf(x, y, 40, ui_with_alpha(light, wa), UI_ALIGN_LEFT, "#%d", client->queue_position);
        y += 50;
    } else {
        if (words > 0.3f) ui_enso(x + 18, y + 20, 16, reconnecting ? UI_KIN : light);
        y += 48;
    }
    const int title_lines = ui_text_wrap(x, y, 17, ui_with_alpha(UI_TEXT, wa), UI_ALIGN_LEFT, w, 2, 20,
                                         app->game_title[0] ? app->game_title : service_name);
    y += (float)title_lines * 20.0f + 6.0f;
    char detail[160] = "";
    if (reconnecting) {
        if (app->waiting_wifi) snprintf(detail, sizeof(detail), "%s", i18n_text("Waiting for Wi-Fi to come back; your game keeps running."));
        else snprintf(detail, sizeof(detail), i18n_text("The connection dropped. Attempt %u of 3; your game keeps running."),
                      app->reconnect_attempt);
    } else if (stage == 0) {
        if (app->queue_eta > 90) snprintf(detail, sizeof(detail), i18n_text("About %d min to go"), (app->queue_eta + 30) / 60);
        else if (app->queue_eta > 0) snprintf(detail, sizeof(detail), i18n_text("About a minute to go"));
        else if (app->queue_eta == 0) snprintf(detail, sizeof(detail), i18n_text("Your rig should be ready any moment"));
        else snprintf(detail, sizeof(detail), "%s", s == SERVICE_XBOX ? i18n_text("Waiting for a free Xbox")
                      : client->queue_position > 0 ? i18n_text("Your place in NVIDIA's queue") : i18n_text("Waiting for a free rig"));
    } else if (stage >= 2) {
        snprintf(detail, sizeof(detail), "%s", app->transport->peer ? app->transport->status : app->signal->status);
    }
    if (detail[0]) y += 15.0f * (float)ui_text_wrap(x, y, 13, ui_with_alpha(UI_TEXT_DIM, wa), UI_ALIGN_LEFT, w, 2, 15, detail);
    ui_wifi_icon(x, y + 12, app->wifi_bars, UI_ACCENT, UI_LINE_STRONG);
    ui_textf(x + 20, y + 9, 12, app->wifi_bars < 2 ? UI_KIN : UI_TEXT_DIM, UI_ALIGN_LEFT,
             app->wifi_bars < 2 ? "Wi-Fi %u/3: move closer to the router" : "Wi-Fi %u/3", app->wifi_bars);
    /* Four steps: queue, rig, signal, stream. */
    static const char *const steps[] = { "QUEUE", "RIG", "SIGNAL", "STREAM" };
    const float step_x0 = 150, step_dx = 100, step_y = 244;
    progress_x = ui_approach(progress_x, step_dx * (float)stage, 8.0f);
    ui_hline(step_x0, step_y, step_dx * 3, UI_LINE);
    ui_hline(step_x0, step_y, progress_x, UI_ACCENT);
    const float pulse = 0.5f + 0.5f * sinf((float)ui_ticks() / 180.0f);
    for (int i = 0; i < 4; ++i) {
        const float sx = step_x0 + step_dx * (float)i;
        if (i < stage) ui_circle(sx, step_y, 4, UI_ACCENT);
        else if (i == stage) {
            ui_circle(sx, step_y, 6.0f + pulse, ui_with_alpha(UI_ACCENT, 0x50));
            ui_ring(sx, step_y, 5.0f, 1.5f, UI_ACCENT, UI_BG);
        } else ui_ring(sx, step_y, 4, 1.0f, UI_LINE_STRONG, UI_BG);
        ui_label(sx, step_y + 9, 11, i <= stage ? UI_TEXT_DIM : UI_TEXT_FAINT, UI_ALIGN_CENTER, steps[i]);
    }
    ui_button(session_button(0, 1), "LEAVE", "退出", UI_BUTTON_DANGER, pressed(app, session_button(0, 1)));
    static const char *const hints[] = { "B", "Leave", NULL };
    draw_hints(hints);
}

static AppAction touch_session(const App *app, int x, int y)
{
    const bool stopped = screens_session_failed(app) && !app->waiting_wifi &&
                         (app->reconnect_attempt == 0 || app->reconnect_attempt > 3);
    if (stopped) {
        if (ui_hit(session_button(0, 2), x, y)) return ACTION_RETRY;
        if (ui_hit(session_button(1, 2), x, y)) return ACTION_CANCEL;
    } else if (ui_hit(session_button(0, 1), x, y)) {
        return ACTION_CANCEL;
    }
    return ACTION_NONE;
}

/* ---- Guides, updates, notes ---------------------------------------------------------------- */

static void draw_notes(UiRect box, const char *text, int scroll)
{
    ui_rect_r(box, ui_with_alpha(UI_SURFACE, 0xE8));
    ui_outline(box.x, box.y, box.w, box.h, 1.0f, UI_LINE);
    const float line_h = 16.0f, pad = 10.0f, width = box.w - pad * 2 - 6;
    const int visible = (int)((box.h - pad * 2) / line_h);
    int total = 0;
    char line[256];
    for (int pass = 0; pass < 2; ++pass) {
        int max_scroll = total - visible;
        if (max_scroll < 0) max_scroll = 0;
        const int first = scroll > max_scroll ? max_scroll : scroll;
        int index = 0;
        const char *p = text && text[0] ? text : "No release notes.";
        while (*p) {
            const char *end = strchr(p, '\n');
            const size_t para = end ? (size_t)(end - p) : strlen(p);
            size_t start = 0;
            do {
                size_t best = 0, cursor = start;
                while (cursor < para) {
                    size_t next = cursor;
                    while (next < para && p[next] == ' ') ++next;
                    while (next < para && p[next] != ' ') ++next;
                    const size_t len = next - start < sizeof(line) - 1 ? next - start : sizeof(line) - 1;
                    memcpy(line, p + start, len);
                    line[len] = '\0';
                    if (ui_text_width(line, 13) > width && best) break;
                    best = next - start;
                    cursor = next;
                }
                if (!best) best = para - start;
                if (pass == 1 && index >= first && index < first + visible) {
                    const size_t len = best < sizeof(line) - 1 ? best : sizeof(line) - 1;
                    memcpy(line, p + start, len);
                    line[len] = '\0';
                    const bool bullet = line[0] == '-' && line[1] == ' ';
                    const float y = box.y + pad + (float)(index - first) * line_h;
                    if (bullet) {
                        ui_circle(box.x + pad + 3, y + 8, 2, UI_ACCENT);
                        ui_text(box.x + pad + 10, y, 13, UI_TEXT, UI_ALIGN_LEFT, line + 2);
                    } else {
                        ui_text(box.x + pad, y, 13, UI_TEXT_DIM, UI_ALIGN_LEFT, line);
                    }
                }
                ++index;
                start += best;
                while (start < para && p[start] == ' ') ++start;
            } while (start < para);
            if (!end) break;
            p = end + 1;
        }
        total = index;
        if (pass == 1 && total > visible) {
            const float rail = box.h - 8, thumb = rail * (float)visible / (float)total;
            ui_vline(box.x + box.w - 5, box.y + 4, rail, UI_LINE);
            ui_rect(box.x + box.w - 6, box.y + 4 + (rail - thumb) * (float)first / (float)(total - visible), 3, thumb, UI_ACCENT);
        }
    }
}

static UiRect guide_button(int i) { return row_button(i, 3, ACT_Y, 150, 14); }

/* The first-run guide's controls page, as the Vita has them. */
static const char *guide_body(int page)
{
    if (page == 2)
        return "The Vita plays like a PlayStation pad. L2 / R2 and L3 / R3 are on the rear touchpad's corners (or the "
               "screen's edges: Settings > Controls). PS is in the stream menu.";
    return GUIDE[page].body;
}

static void draw_guide(const App *app)
{
    const int page = app->guide_page < 0 ? 0 : app->guide_page;
    const GuidePage *g = &GUIDE[page];
    ui_enso(150, 150, 60, UI_ACCENT);
    ui_text(150, 120, 54, UI_TEXT, UI_ALIGN_CENTER, g->kanji);
    ui_textf(150, 232, 13, UI_ACCENT, UI_ALIGN_CENTER, "%d / %d", page + 1, GUIDE_PAGES);
    draw_heading(270, 52, g->jp, g->en, UI_ACCENT);
    ui_text_wrap(270, 78, 15, UI_TEXT, UI_ALIGN_LEFT, 310, 6, 19, guide_body(page));
    const UiRect tip = { 266, 200, 318, 60 };
    ui_panel(tip, UI_ACCENT);
    ui_label(tip.x + 12, tip.y + 9, 11, UI_TEXT_FAINT, UI_ALIGN_LEFT, "TIP");
    ui_text_wrap(tip.x + 12, tip.y + 24, 12, UI_TEXT, UI_ALIGN_LEFT, tip.w - 24, 2, 15,
                 page == 2 ? "Hold START + SELECT, or tap the handle at the top of the screen, for the stream menu."
                           : g->tip);
    ui_dots(150, 258, GUIDE_PAGES, (unsigned)page, UI_ACCENT, UI_LINE_STRONG);
    const bool last = page == GUIDE_PAGES - 1;
    ui_button(guide_button(0), "BACK", "戻る", UI_BUTTON_NORMAL, pressed(app, guide_button(0)));
    ui_button(guide_button(1), "SKIP", "省略", UI_BUTTON_NORMAL, pressed(app, guide_button(1)));
    ui_button(guide_button(2), last ? "START" : "NEXT", last ? "開始" : "次へ", UI_BUTTON_PRIMARY, pressed(app, guide_button(2)));
    static const char *const hints[] = { "A", "Next", "B", "Back", "START", "Skip", NULL };
    draw_hints(hints);
}

static const UiRect CONTINUE_BUTTON = { 220, ACT_Y, 160, BTN_H };

static void draw_whats_new(const App *app)
{
    draw_title(CW / 2, 38, "新機能", "WHAT'S NEW");
    char heading[64];
    snprintf(heading, sizeof(heading), i18n_text("Kasumi %s"), app->whats_new_version);
    ui_text(CW / 2, 66, 17, UI_TEXT, UI_ALIGN_CENTER, heading);
    draw_notes((UiRect){ 60, 90, 480, 172 }, app->whats_new_notes, app->notes_scroll);
    ui_button(CONTINUE_BUTTON, "CONTINUE", "続ける", UI_BUTTON_PRIMARY, pressed(app, CONTINUE_BUTTON));
    static const char *const hints[] = { "▲ ▼", "Scroll", "A", "Continue", NULL };
    draw_hints(hints);
}

static void draw_discord(const App *app)
{
    draw_heading(60, 60, "仲間", "JOIN THE KASUMI DISCORD", UI_ACCENT);
    ui_text_wrap(60, 88, 16, UI_TEXT, UI_ALIGN_LEFT, 300, 5, 20,
                 "New versions first, help from other players, and a say in what Kasumi gets next.");
    ui_text(60, 196, 16, UI_ACCENT, UI_ALIGN_LEFT, "discord.gg/K9Jy3t7YHE");
    ui_text(60, 220, 12, UI_TEXT_DIM, UI_ALIGN_LEFT, "Also in Settings > System > Kasumi Discord");
    ui_label(470, 60, 11, UI_TEXT_FAINT, UI_ALIGN_CENTER, "SCAN WITH YOUR PHONE");
    ui_image(UI_IMAGE_DISCORD, 400, 80, 1.3f, 1.0f);
    ui_button(CONTINUE_BUTTON, "CONTINUE", "続ける", UI_BUTTON_PRIMARY, pressed(app, CONTINUE_BUTTON));
    static const char *const hints[] = { "A", "Continue", NULL };
    draw_hints(hints);
}

static UiRect update_button(int i, int count) { return row_button(i, count, ACT_Y, 160, 14); }

static bool update_working(const UpdateInfo *info)
{
    return info->state == UPDATE_DOWNLOADING || info->state == UPDATE_VERIFYING || info->state == UPDATE_INSTALLING ||
           info->state == UPDATE_CHECKING;
}

static void draw_update(const App *app)
{
    static float bar;
    const UpdateInfo info = updater_info();
    draw_title(CW / 2, 38, "更新", "SOFTWARE UPDATE");
    ui_text(CW / 2, 66, 15, info.state == UPDATE_FAILED ? UI_DANGER : UI_TEXT, UI_ALIGN_CENTER, update_phase_title(&info));
    const bool has_new = info.latest[0] && info.state != UPDATE_UP_TO_DATE && info.state != UPDATE_IDLE &&
                         info.state != UPDATE_CHECKING;
    if (has_new) {
        const UiRect left = { 60, 90, 150, 40 }, right = { 60, 140, 150, 40 };
        ui_rect_r(left, UI_SURFACE);
        ui_outline(left.x, left.y, left.w, left.h, 1.0f, UI_LINE);
        ui_label(left.x + left.w / 2, left.y + 5, 10, UI_TEXT_FAINT, UI_ALIGN_CENTER, "INSTALLED");
        ui_text_fit(left.x + left.w / 2, left.y + 19, 14, UI_TEXT_DIM, UI_ALIGN_CENTER, left.w - 8, APP_VERSION);
        ui_rect_r(right, UI_SURFACE);
        ui_outline(right.x, right.y, right.w, right.h, 1.0f, UI_ACCENT);
        ui_label(right.x + right.w / 2, right.y + 5, 10, UI_ACCENT, UI_ALIGN_CENTER, info.prerelease ? "NEW BETA" : "NEW");
        ui_text_fit(right.x + right.w / 2, right.y + 19, 14, UI_TEXT, UI_ALIGN_CENTER, right.w - 8, info.latest);
        draw_notes((UiRect){ 226, 90, 314, 132 }, info.notes, app->notes_scroll);
    } else if (info.state == UPDATE_CHECKING || info.state == UPDATE_IDLE) {
        ui_enso(CW / 2, 140, 28, UI_ACCENT);
    } else {
        const bool ok = info.state == UPDATE_UP_TO_DATE;
        ui_ring(CW / 2, 134, 30, 2.0f, ok ? UI_ACCENT : UI_DANGER, UI_BG);
        if (ok) {
            ui_line(CW / 2 - 13, 134, CW / 2 - 3, 144, 3.0f, UI_ACCENT);
            ui_line(CW / 2 - 3, 144, CW / 2 + 15, 123, 3.0f, UI_ACCENT);
        } else {
            ui_text(CW / 2, 116, 30, UI_DANGER, UI_ALIGN_CENTER, "!");
        }
    }
    /* Three steps on one bar. */
    static const char *const steps[] = { "DOWNLOAD", "VERIFY", "INSTALL" };
    int step = -1;
    if (info.state == UPDATE_DOWNLOADING) step = 0;
    else if (info.state == UPDATE_VERIFYING) step = 1;
    else if (info.state == UPDATE_INSTALLING) step = 2;
    else if (info.state == UPDATE_INSTALLED) step = 3;
    const float overall = step < 0 ? 0.0f : step >= 3 ? 1.0f : ((float)step + info.progress / 1000.0f) / 3.0f;
    bar = ui_approach(bar, overall, 10.0f);
    const float bx = 100, bw = 400, by = 232;
    ui_rect(bx, by, bw, 4, UI_RAISED);
    if (bar > 0.005f) ui_rect(bx, by, bw * bar, 4, info.state == UPDATE_FAILED ? UI_DANGER : UI_ACCENT);
    for (int i = 0; i < 3; ++i)
        ui_label(bx + bw * ((float)i + 0.5f) / 3.0f, by + 9, 10, step >= i ? UI_TEXT : UI_TEXT_FAINT, UI_ALIGN_CENTER, steps[i]);
    char detail[200] = "";
    if (info.state == UPDATE_DOWNLOADING && info.size_bytes)
        snprintf(detail, sizeof(detail), i18n_text("%.1f of %.1f MB"), info.progress / 1000.0f * info.size_bytes / 1048576.0f,
                 info.size_bytes / 1048576.0f);
    else if (info.state == UPDATE_AVAILABLE && info.size_bytes)
        snprintf(detail, sizeof(detail), "%s  ·  %.1f MB  ·  %s", info.latest, info.size_bytes / 1048576.0f, updater_package_label());
    else if (info.state == UPDATE_FAILED) snprintf(detail, sizeof(detail), "%s", info.error);
    else if (info.state == UPDATE_UP_TO_DATE) snprintf(detail, sizeof(detail), i18n_text("Version %s"), APP_VERSION);
    ui_text_wrap(CW / 2, 256, 12, UI_TEXT_DIM, UI_ALIGN_CENTER, CW - 80, 1, 14, detail);
    if (update_working(&info)) return;
    if (info.state == UPDATE_AVAILABLE) {
        char label[48];
        snprintf(label, sizeof(label), i18n_text("INSTALL %s"), info.latest);
        ui_button(update_button(0, 3), label, "インストール", UI_BUTTON_PRIMARY, pressed(app, update_button(0, 3)));
        ui_button(update_button(1, 3), "LATER", "後で", UI_BUTTON_NORMAL, pressed(app, update_button(1, 3)));
        ui_button(update_button(2, 3), "CLOSE", "閉じる", UI_BUTTON_NORMAL, pressed(app, update_button(2, 3)));
    } else {
        ui_button(update_button(0, 2), info.state == UPDATE_FAILED ? "TRY AGAIN" : "CHECK AGAIN", "確認", UI_BUTTON_PRIMARY,
                  pressed(app, update_button(0, 2)));
        ui_button(update_button(1, 2), "CLOSE", "閉じる", UI_BUTTON_NORMAL, pressed(app, update_button(1, 2)));
    }
    static const char *const hints_new[] = { "A", "Install", "X", "Later", "B", "Close", NULL };
    static const char *const hints[] = { "A", "Check again", "B", "Close", NULL };
    draw_hints(info.state == UPDATE_AVAILABLE ? hints_new : hints);
}

static AppAction touch_update(int x, int y)
{
    const UpdateInfo info = updater_info();
    if (update_working(&info)) return ACTION_NONE;
    if (info.state == UPDATE_AVAILABLE) {
        if (ui_hit(update_button(0, 3), x, y)) return ACTION_UPDATE_PRIMARY;
        if (ui_hit(update_button(1, 3), x, y)) return ACTION_UPDATE_LATER;
        if (ui_hit(update_button(2, 3), x, y)) return ACTION_UPDATE_CLOSE;
        return ACTION_NONE;
    }
    if (ui_hit(update_button(0, 2), x, y)) return ACTION_UPDATE_PRIMARY;
    if (ui_hit(update_button(1, 2), x, y)) return ACTION_UPDATE_CLOSE;
    return ACTION_NONE;
}

/* ---- Overlays: questions, work in progress, a sign-in done ------------------------------------ */

static const UiRect MODAL_CARD = { 110, 62, 380, 220 };
static UiRect modal_button(int i, int count)
{
    const float w = count == 1 ? 160.0f : 160.0f, gap = 12.0f, total = w * (float)count + gap * (float)(count - 1);
    return (UiRect){ floorf(CW / 2 - total / 2 + (w + gap) * (float)i), MODAL_CARD.y + MODAL_CARD.h - BTN_H - 14, w, BTN_H };
}

typedef struct {
    const char *label, *jp;
    UiButtonStyle style;
} ModalButton;

/* The question's two answers (or one), as the 3DS's lower screen has them. */
static int modal_buttons(const App *app, ModalButton out[2])
{
    switch (app->modal) {
    case MODAL_REPORT_SENT:
        out[0] = (ModalButton){ "OK", "了解", UI_BUTTON_PRIMARY };
        return 1;
    case MODAL_PROVIDER_PICK: {
        static GfnProvider partner;
        providers_partner_here(&partner, NULL);
        out[0] = (ModalButton){ partner.name, "推奨", UI_BUTTON_PRIMARY };
        out[1] = (ModalButton){ "NVIDIA", "エヌビディア", UI_BUTTON_NORMAL };
        return 2;
    }
    case MODAL_PAID_ONLY:
        out[0] = (ModalButton){ "PLAY ANYWAY", "開始", UI_BUTTON_PRIMARY };
        out[1] = (ModalButton){ "BACK", "戻る", UI_BUTTON_NORMAL };
        return 2;
    case MODAL_STEAM_LEAVE:
        out[0] = (ModalButton){ "DISCONNECT", "切断", UI_BUTTON_PRIMARY };
        out[1] = (ModalButton){ "QUIT GAME", "終了", UI_BUTTON_DANGER };
        return 2;
    case MODAL_CONFLICT:
    case MODAL_LIMIT_WAIT: {
        const bool wait = app->modal == MODAL_LIMIT_WAIT, same = app->conflict_same_game;
        out[0] = (ModalButton){ wait ? "TRY NOW" : same ? "RESUME" : "END IT", wait ? "再試行" : same ? "再開" : "終了",
                                wait || same ? UI_BUTTON_PRIMARY : UI_BUTTON_DANGER };
        out[1] = (ModalButton){ wait ? "STOP" : "BACK", wait ? "中止" : "戻る", UI_BUTTON_NORMAL };
        return 2;
    }
    default: {
        const bool error = app->modal == MODAL_ERROR, resume = app->modal == MODAL_RESUME;
        const bool send = app->modal == MODAL_SEND_REPORT, share = app->modal == MODAL_SHARE_ASK;
        out[0] = (ModalButton){ resume ? "RESUME" : error ? "RETRY" : send ? "SEND" : share ? "SHARE" : "YES",
                                resume ? "再開" : error ? "再試行" : send ? "送信" : share ? "協力" : "はい",
                                app->modal == MODAL_EXIT || app->modal == MODAL_SIGN_OUT ? UI_BUTTON_DANGER : UI_BUTTON_PRIMARY };
        out[1] = (ModalButton){ resume ? "END GAME" : error ? "BACK" : send ? "CANCEL" : share ? "NO THANKS" : "NO",
                                resume ? "終了" : error ? "戻る" : send ? "取消" : share ? "不要" : "いいえ",
                                resume ? UI_BUTTON_DANGER : UI_BUTTON_NORMAL };
        return 2;
    }
    }
}

static void draw_modal(const App *app, float p)
{
    ui_rect(0, 0, CW, CH, ui_with_alpha(UI_BG, (u8)(0xC8 * p)));
    ui_offset(0.0f, (1.0f - p) * 12.0f);
    const UiRect card = MODAL_CARD;
    draw_card(card, p);
    if (app->modal == MODAL_SHARE_ASK) {
        ui_enso(CW / 2, card.y + 30, 18, UI_ACCENT);
        ui_text(CW / 2, card.y + 20, 17, UI_ACCENT, UI_ALIGN_CENTER, "協");
        draw_title(CW / 2, card.y + 54, "協力のお願い", "HELP IMPROVE KASUMI?");
        static const char *const points[3] = {
            "Problem reports: the log, after a crash or failed stream",
            "Performance stats: ping and smoothness after each session",
            "Never your login · change it in Settings > System",
        };
        for (int i = 0; i < 3; ++i) {
            const float y = card.y + 92 + (float)i * 18.0f;
            ui_rounded(card.x + 26, y + 5, 5, 5, 2.5f, UI_ACCENT);
            ui_text_fit(card.x + 38, y, 12, UI_TEXT, UI_ALIGN_LEFT, card.w - 56, points[i]);
        }
    } else {
        draw_title(CW / 2, card.y + 14, app->modal_jp, app->modal_title);
        if (app->modal == MODAL_REPORT_SENT) {
            ui_text(CW / 2, card.y + 56, 32, UI_ACCENT, UI_ALIGN_CENTER, app->report_code);
            ui_text_wrap(CW / 2, card.y + 100, 12, UI_TEXT_DIM, UI_ALIGN_CENTER, card.w - 40, 3, 15, app->modal_text);
        } else {
            ui_text_wrap(CW / 2, card.y + 52, 13, UI_TEXT_DIM, UI_ALIGN_CENTER, card.w - 40, 7, 16, app->modal_text);
        }
    }
    ModalButton buttons[2];
    const int count = modal_buttons(app, buttons);
    for (int i = 0; i < count; ++i) {
        const UiRect r = modal_button(i, count);
        ui_button(r, buttons[i].label, buttons[i].jp, buttons[i].style, pressed(app, r));
    }
    ui_offset(0.0f, 0.0f);
}

static AppAction touch_modal(const App *app, int x, int y)
{
    ModalButton buttons[2];
    const int count = modal_buttons(app, buttons);
    if (ui_hit(modal_button(0, count), x, y)) return app->modal == MODAL_ERROR ? ACTION_RETRY : ACTION_CONFIRM;
    if (count > 1 && ui_hit(modal_button(1, count), x, y)) return ACTION_DISMISS;
    return ACTION_NONE;
}

static void draw_busy(const App *app)
{
    ui_rect(0, 0, CW, CH, UI_SCRIM);
    const UiRect card = { 170, 110, 260, 120 };
    draw_card(card, 1.0f);
    ui_enso(CW / 2, card.y + 38, 18, UI_ACCENT);
    ui_text_wrap(CW / 2, card.y + 68, 13, UI_TEXT, UI_ALIGN_CENTER, card.w - 30, 2, 16, app->busy);
    static const char *const hints[] = { "B", "Cancel", NULL };
    ui_hint_row(CW / 2, card.y + card.h + 10, hints);
}

/* Signed in (or paired): the ensō brushed, petals, 完. */
static u64 g_celebrate_at;
static int g_celebrate_service;
static char g_celebrate_text[96];

static void draw_celebration(void)
{
    if (!g_celebrate_at) return;
    const float t = (float)(plat_time_ms() - g_celebrate_at);
    if (t > 2100.0f) {
        g_celebrate_at = 0;
        return;
    }
    const u32 light = service_light(g_celebrate_service);
    const float in = ui_ease_out(clamp01(t / 260.0f)), out = clamp01((t - 1700.0f) / 400.0f);
    const float a = in * (1.0f - out);
    ui_rect(0, TOP_H, CW, CH - TOP_H, ui_with_alpha(UI_BG, (u8)(228.0f * a)));
    const float cx = CW / 2, cy = 140;
    const float stroke = ui_ease_out(clamp01((t - 60.0f) / 560.0f));
    const int n = (int)(stroke * 64.0f);
    for (int i = 1; i <= n; ++i) {
        const float f0 = (float)(i - 1) / 64.0f, f1 = (float)i / 64.0f;
        const float a0 = -2.2f + f0 * 2.0f * (float)M_PI * 0.93f, a1 = -2.2f + f1 * 2.0f * (float)M_PI * 0.93f;
        ui_line(cx + cosf(a0) * 46, cy + sinf(a0) * 46, cx + cosf(a1) * 46, cy + sinf(a1) * 46, 9.0f - 7.0f * f1,
                ui_with_alpha(light, (u8)(255.0f * (1.0f - out))));
    }
    for (unsigned i = 0; i < 16; ++i) {
        const float pt = clamp01((t - 420.0f) / (900.0f + 400.0f * hash01(i, 8)));
        if (pt <= 0.0f || pt >= 1.0f) continue;
        const float angle = (float)i / 16.0f * 2.0f * (float)M_PI + hash01(i, 9) * 0.4f;
        const float d = 52.0f + ui_ease_out(pt) * (70.0f + 90.0f * hash01(i, 7));
        draw_petal(cx + cosf(angle) * d, cy + sinf(angle) * d + pt * 20.0f, 3.6f, angle + pt * 4.0f,
                   0.4f + 0.6f * fabsf(cosf(pt * 6.0f + (float)i)),
                   ui_with_alpha(PETAL_COLOR, (u8)(220.0f * (1.0f - pt) * (1.0f - out))));
    }
    const float stamp = clamp01((t - 520.0f) / 300.0f);
    if (stamp > 0.0f) {
        const float pop = ease_out_back(stamp);
        const u8 sa = (u8)(255.0f * stamp * (1.0f - out));
        if (ui_has_japanese()) ui_text(cx, cy - 19.0f * pop, 38.0f * pop, ui_with_alpha(UI_TEXT, sa), UI_ALIGN_CENTER, "完");
        else ui_text(cx, cy - 13.0f * pop, 26.0f * pop, ui_with_alpha(UI_TEXT, sa), UI_ALIGN_CENTER, "OK");
    }
    const float text_in = ui_ease_out(clamp01((t - 620.0f) / 380.0f)) * (1.0f - out);
    const u8 ta = (u8)(255.0f * text_in);
    ui_label(cx, 204 + (1.0f - text_in) * 8.0f, 16, ui_with_alpha(UI_TEXT, ta), UI_ALIGN_CENTER,
             g_celebrate_service == SERVICE_STEAM ? "PAIRED" : "SIGNED IN");
    ui_text_fit(cx, 228 + (1.0f - text_in) * 8.0f, 14, ui_with_alpha(light, ta), UI_ALIGN_CENTER, CW - 80, g_celebrate_text);
}

static void celebrate(const App *app)
{
    const int s = app_service(app);
    g_celebrate_at = plat_time_ms();
    g_celebrate_service = s;
    if (s == SERVICE_STEAM)
        snprintf(g_celebrate_text, sizeof(g_celebrate_text), i18n_text("Streaming from %.60s"),
                 steam_link_host_name()[0] ? steam_link_host_name() : i18n_text("your PC"));
    else
        snprintf(g_celebrate_text, sizeof(g_celebrate_text), "%s is ready. Your games are on their way.", SERVICE_INFO[s].name);
}

/* ---- In a game ------------------------------------------------------------------------------ */

/* The handle at the top: a tap opens the stream menu. */
static const UiRect STREAM_HANDLE = { 260, 0, 80, 18 };
/* The stream menu: the eight items, then the quick toggles. */
static const UiRect MENU_PANEL = { 40, 22, 520, 300 };

static UiRect menu_item(int index)
{
    const float w = (MENU_PANEL.w - 32 - 3 * 10) / 4;
    return (UiRect){ MENU_PANEL.x + 16 + (float)(index % 4) * (w + 10), MENU_PANEL.y + 70 + (float)(index / 4) * 52.0f, w, 44 };
}

enum { QUICK_KEYS, QUICK_MOUSE, QUICK_ZOOM, QUICK_LOOK, QUICK_MIC, QUICK_PS, QUICK_COUNT };

static UiRect quick_item(int index)
{
    const float w = (MENU_PANEL.w - 32 - 5 * 8) / 6;
    return (UiRect){ MENU_PANEL.x + 16 + (float)index * (w + 8), MENU_PANEL.y + 210, w, 40 };
}

static void draw_stream_stats(const App *app, float x, float y, u32 color)
{
    const WebRtcTransport *t = app->transport;
    char values[4][24];
    snprintf(values[0], sizeof(values[0]), i18n_text("%u FPS"), app->fps);
    snprintf(values[1], sizeof(values[1]), i18n_text("%.1f MBPS"), t->video_kbps / 1000.0f);
    snprintf(values[2], sizeof(values[2]), i18n_text("%d MS"), t->rtt_ms);
    snprintf(values[3], sizeof(values[3]), i18n_text("%u RESENT"), app->resent_per_second);
    const bool warn[4] = { app->fps > 0 && app->fps < 24, false, t->rtt_ms > 80, app->resent_per_second > 2 };
    for (int i = 0; i < 4; ++i) {
        x += ui_text_width(values[i], 11) + 4.0f;
        ui_label(x - ui_text_width(values[i], 11) - 4.0f, y, 11, warn[i] ? UI_KIN : color, UI_ALIGN_LEFT, values[i]);
        x += 10.0f;
    }
}

static void draw_stream_menu(const App *app, float p)
{
    const WebRtcTransport *t = app->transport;
    ui_rect(0, 0, CW, CH, ui_with_alpha(UI_BG, (u8)(0xB0 * p)));
    ui_offset(0.0f, (1.0f - p) * 14.0f);
    draw_card(MENU_PANEL, p);
    /* The game, its time, the link. */
    const u64 elapsed = app->stream_started_at ? plat_time_ms() - app->stream_started_at : 0;
    const bool ending = app->free_tier_guess && elapsed >= 55ull * 60 * 1000;
    char timer[24];
    format_elapsed(timer, sizeof(timer), ending ? (elapsed >= 60ull * 60 * 1000 ? 0 : 60ull * 60 * 1000 - elapsed) : elapsed);
    draw_heading(MENU_PANEL.x + 16, MENU_PANEL.y + 12, "一時停止", "STREAM MENU", UI_ACCENT);
    ui_text(MENU_PANEL.x + MENU_PANEL.w - 16, MENU_PANEL.y + 12, 13, ending ? UI_KIN : UI_TEXT_DIM, UI_ALIGN_RIGHT, timer);
    ui_text_fit(MENU_PANEL.x + 16, MENU_PANEL.y + 32, 15, UI_TEXT, UI_ALIGN_LEFT, MENU_PANEL.w - 32, app->game_title);
    draw_stream_stats(app, MENU_PANEL.x + 16, MENU_PANEL.y + 52, UI_TEXT_DIM);
    const char *layout = app->settings.button_layout == GFN_LAYOUT_POSITION ? "LAYOUT: POS" : "LAYOUT: ABC";
    const char *gyro = app->settings.gyro_mode == GFN_GYRO_ALWAYS ? "GYRO: ON"
                     : app->settings.gyro_mode == GFN_GYRO_WHILE_AIMING ? "GYRO: AIM" : "GYRO: OFF";
    const char *zone = mvd_video_zoomed() ? "SAVE ZONE" : zoom_zones_count() ? "CLEAR ZONES" : "SAVE ZONE";
    const char *labels[STREAM_MENU_COUNT] = { "RESUME", "SCREENSHOT", "CONTROLS", zone, gyro,
                                              app->sound_muted ? "SOUND: OFF" : "SOUND: ON", layout, "DISCONNECT" };
    static const char *const jp[STREAM_MENU_COUNT] = { "再開", "撮影", "操作", "ズーム", "ジャイロ", "音声", "ボタン配置", "切断" };
    for (int i = 0; i < STREAM_MENU_COUNT; ++i) {
        const UiRect r = menu_item(i);
        const bool focus = i == app->stream_menu_index;
        const UiButtonStyle style = i == STREAM_MENU_DISCONNECT ? UI_BUTTON_DANGER : i == STREAM_MENU_RESUME
                                    ? UI_BUTTON_PRIMARY : focus ? UI_BUTTON_ACTIVE : UI_BUTTON_NORMAL;
        ui_button(r, labels[i], jp[i], style, pressed(app, r));
        if (focus) ui_outline(r.x - 3, r.y - 3, r.w + 6, r.h + 6, 1.5f, UI_ACCENT);
    }
    /* Quick toggles: tapping one closes the menu and does it. */
    ui_label(MENU_PANEL.x + 16, MENU_PANEL.y + 192, 11, UI_TEXT_FAINT, UI_ALIGN_LEFT, "IN THE GAME");
    const unsigned level = mvd_video_zoom_level();
    char zoom[16];
    if (app->zone_index >= 0 && level) snprintf(zoom, sizeof(zoom), i18n_text("ZONE %d"), app->zone_index + 1);
    else snprintf(zoom, sizeof(zoom), "%s", level == 3 ? "3.0x" : level == 2 ? "2.0x" : level == 1 ? "1.5x" : "拡大");
    const bool xbox = app->settings.xbox_names;
    const char *quick[QUICK_COUNT] = { "KEYS", "MOUSE", "ZOOM", "TOUCH CAM", app->mic_available ? (mic_capture_muted() ? "MIC OFF" : "MIC ON") : "MIC",
                                       xbox ? "XBOX" : "PS" };
    const char *quick_jp[QUICK_COUNT] = { "キー", "マウス", zoom, "視点", "マイク", "ホーム" };
    const bool active[QUICK_COUNT] = { false, t->pointer_mode, level != 0, app->look_mode != 0,
                                       app->mic_available && !mic_capture_muted(), (app->touch_buttons & GFN_PAD_GUIDE) != 0 };
    const bool enabled[QUICK_COUNT] = { true, true, true, app->look_available, app->mic_available, true };
    for (int i = 0; i < QUICK_COUNT; ++i) {
        const UiRect r = quick_item(i);
        if (!enabled[i]) {
            ui_outline(r.x, r.y, r.w, r.h - 2, 1.0f, UI_LINE);
            ui_label(r.x + r.w / 2, r.y + 13, 10, UI_LINE_STRONG, UI_ALIGN_CENTER, quick[i]);
            continue;
        }
        ui_button(r, quick[i], quick_jp[i], active[i] ? UI_BUTTON_ACTIVE : UI_BUTTON_NORMAL, pressed(app, r));
    }
    ui_text_fit(CW / 2, MENU_PANEL.y + MENU_PANEL.h - 22, 11, UI_TEXT_FAINT, UI_ALIGN_CENTER, MENU_PANEL.w - 32,
                "Hold the PS / Xbox button above for the game's own menu. B or a tap outside resumes.");
    ui_offset(0.0f, 0.0f);
}

static void controls_row(float y, const char *input, const char *action)
{
    ui_button_chip(80, y, input, UI_TEXT_DIM);
    ui_text_fit(220, y + 1, 13, UI_TEXT, UI_ALIGN_LEFT, MENU_PANEL.x + MENU_PANEL.w - 16 - 220, action);
}

static void draw_controls_sheet(const App *app, float p)
{
    ui_rect(0, 0, CW, CH, ui_with_alpha(UI_BG, (u8)(0xE8 * p)));
    ui_offset(0.0f, (1.0f - p) * 14.0f);
    draw_card(MENU_PANEL, p);
    draw_title(CW / 2, MENU_PANEL.y + 12, "操作", "CONTROLS");
    const AppSettings *s = &app->settings;
    const bool xbox = s->xbox_names;
    float y = MENU_PANEL.y + 52;
    const float dy = 22;
    controls_row(y, "LEFT STICK", "Left stick"); y += dy;
    controls_row(y, "RIGHT STICK", s->gyro_mode != GFN_GYRO_OFF ? "Right stick + gyro" : "Right stick"); y += dy;
    controls_row(y, s->swap_shoulders ? "ZL ZR" : "L R", xbox ? "LB / RB" : "L1 / R1"); y += dy;
    const char *extra = s->extra_buttons == EXTRA_BUTTONS_FRONT ? "screen edges, upper buttons"
                      : s->extra_buttons == EXTRA_BUTTONS_OFF ? "off (Settings > Controls)" : "rear touchpad, top corners";
    char line[96];
    snprintf(line, sizeof(line), "%s: %s", xbox ? "LT / RT triggers" : "L2 / R2 triggers", i18n_text(extra));
    controls_row(y, s->swap_shoulders ? "L R" : "ZL ZR", line); y += dy;
    snprintf(line, sizeof(line), "%s: %s", xbox ? "LS / RS click" : "L3 / R3",
             i18n_text(s->extra_buttons == EXTRA_BUTTONS_FRONT ? "screen edges, lower buttons"
                       : s->extra_buttons == EXTRA_BUTTONS_OFF ? "off" : "rear touchpad, bottom corners"));
    controls_row(y, "L3 R3", line); y += dy;
    controls_row(y, "START", xbox ? "Menu" : "Options"); y += dy;
    controls_row(y, "SELECT", xbox ? "View" : "Share / View"); y += dy;
    controls_row(y, "START+SELECT", "Hold for the stream menu (or tap the top handle)"); y += dy;
    controls_row(y, "TOUCH", app->transport->pointer_mode ? "Drag moves the mouse, tap clicks"
                             : app->look_mode ? "Touch camera: drag to look" : mvd_video_zoomed() ? "Drag moves the zoomed view"
                                              : "Nothing (turn on Touch camera or MOUSE)");
    static const char *const hints[] = { "B", "Close", NULL };
    ui_hint_row(CW / 2, MENU_PANEL.y + MENU_PANEL.h - 24, hints);
    ui_offset(0.0f, 0.0f);
}

/* The touch camera's ink: a ring under the finger, its trail. */
static void look_image(UiImage image, float native, float cx, float cy, float size, u32 color)
{
    const float scale = size / native;
    ui_image_tint(image, cx - native / 2 * scale, cy - native / 2 * scale, scale, color);
}

static void draw_look(const App *app)
{
    /* In screen pixels, where main.c keeps the finger. */
    ui_screen();
    const u64 now = ui_ticks();
    for (unsigned i = 0; i < 10; ++i) {
        const u64 at = app->look_trail_at[i];
        if (!at || now - at > 320) continue;
        const float k = 1.0f - (float)(now - at) / 320.0f;
        look_image(UI_IMAGE_LOOK_DOT, 32, (float)app->look_trail_x[i], (float)app->look_trail_y[i], 10.0f + 12.0f * k,
                   ui_with_alpha(UI_ACCENT, (u8)(0x90 * k)));
    }
    if (app->look_active) {
        const float fx = (float)app->look_x, fy = (float)app->look_y;
        if (app->look_mode == 1) {
            const float ax = (float)app->look_anchor_x, ay = (float)app->look_anchor_y;
            float dx = fx - ax, dy = fy - ay;
            const float limit = app->look_radius > 0.0f ? app->look_radius : 58.0f;
            const float d = sqrtf(dx * dx + dy * dy);
            if (d > limit) { dx *= limit / d; dy *= limit / d; }
            look_image(UI_IMAGE_LOOK_RING, 96, ax, ay, limit * 2.0f + 20.0f, ui_with_alpha(UI_TEXT, 0xA0));
            look_image(UI_IMAGE_LOOK_DOT, 32, ax + dx, ay + dy, 36, UI_ACCENT);
        } else {
            look_image(UI_IMAGE_LOOK_RING, 96, fx, fy, 80 + 30 * app->look_amount, ui_with_alpha(UI_TEXT, 0xA0));
            look_image(UI_IMAGE_LOOK_DOT, 32, fx, fy, 20, UI_ACCENT);
        }
    }
    canvas();
}

/* The front-screen L2 / R2 / L3 / R3 (Settings > Controls). */
static void draw_extra_buttons(const App *app)
{
    PlatExtraButton b;
    ui_screen();
    const bool xbox = app->settings.xbox_names;
    for (unsigned i = 0; plat_input_extra_button(i, &b); ++i) {
        const char *name = b.key == PLAT_KEY_ZL ? (xbox ? "LT" : "L2") : b.key == PLAT_KEY_ZR ? (xbox ? "RT" : "R2")
                         : b.key == PLAT_KEY_L3 ? (xbox ? "LS" : "L3") : (xbox ? "RS" : "R3");
        const float x = (float)b.x + 10, y = (float)b.y + 10, w = (float)b.w - 20, h = (float)b.h - 20;
        ui_rounded(x, y, w, h, 18, ui_with_alpha(b.held ? UI_ACCENT : UI_BG, b.held ? 0xC0 : 0x58));
        ui_outline(x + 6, y, w - 12, 1, 1.0f, ui_with_alpha(UI_TEXT, 0x40));
        ui_text(x + w / 2, y + h / 2 - 14, 28, ui_with_alpha(b.held ? UI_BG : UI_TEXT, b.held ? 0xFF : 0xB0), UI_ALIGN_CENTER, name);
    }
    canvas();
}

static const UiRect STREAM_MIC = { 540, 4, 52, 18 };

static void draw_stream(const App *app)
{
    const WebRtcTransport *t = app->transport;
    if (app->keyboard_open) {
        /* The game's keyboard, twice its 3DS size, over a dim picture. */
        ui_rect(0, 0, CW, CH, UI_RGBA(0x00, 0x00, 0x00, 0xA0));
        ui_region(160.0f, 32.0f, 2.0f, UI_BOTTOM_WIDTH, UI_HEIGHT, true);
        /* render_map_touch already gave the touch in the keyboard's units. */
        remote_keyboard_draw(t, app->touching, app->touch_x, app->touch_y);
        canvas();
        return;
    }
    const float p = overlay_progress();
    if (app->controls_open) {
        draw_controls_sheet(app, p);
        return;
    }
    if (app->stream_menu) {
        draw_stream_menu(app, p);
        return;
    }
    if (app->look_mode) draw_look(app);
    draw_extra_buttons(app);
    /* What is going on, small, at the top. */
    const u64 elapsed = app->stream_started_at ? plat_time_ms() - app->stream_started_at : 0;
    if (app->video_stalled) {
        ui_rect(150, 0, 300, 22, UI_KIN);
        ui_text(CW / 2, 4, 12, UI_BG, UI_ALIGN_CENTER, "Connection unstable - waiting for video");
    } else if (app->toast) {
        const float w = fminf(ui_text_width(app->toast, 13) + 28.0f, 420.0f);
        ui_rect(CW / 2 - w / 2, 0, w, 22, ui_with_alpha(UI_ACCENT_DEEP, 0xE0));
        ui_text_fit(CW / 2, 4, 13, UI_TEXT, UI_ALIGN_CENTER, w - 16, app->toast);
    } else if (app->stream_started_at && elapsed < 8000) {
        const float a = elapsed > 6500 ? 1.0f - (float)(elapsed - 6500) / 1500.0f : 1.0f;
        ui_rect(130, 0, 340, 22, ui_with_alpha(UI_BG, (u8)(0xB0 * a)));
        ui_text(CW / 2, 4, 12, ui_with_alpha(UI_TEXT, (u8)(255 * a)), UI_ALIGN_CENTER,
                "Hold START + SELECT, or tap the handle, for the menu");
    } else {
        const UiRect h = STREAM_HANDLE;
        const bool down = pressed(app, h);
        ui_rounded(h.x + 20, h.y + 3, h.w - 40, 5, 2.5f, ui_with_alpha(UI_TEXT, down ? 0xE0 : 0x50));
    }
    if (t->pointer_mode) {
        ui_rect(6, 4, 52, 16, ui_with_alpha(UI_ACCENT, 0xD0));
        ui_label(32, 6, 11, UI_BG, UI_ALIGN_CENTER, "MOUSE");
    }
    if (app->settings.show_stats) {
        ui_rect(6, CH - 20, 300, 18, ui_with_alpha(UI_BG, 0x90));
        draw_stream_stats(app, 12, CH - 17, UI_TEXT_DIM);
    }
    if (app->mic_available) {
        const bool live = !mic_capture_muted();
        const UiRect r = STREAM_MIC;
        ui_rect_r(r, live ? ui_with_alpha(UI_ACCENT, 0xE0) : ui_with_alpha(UI_BG, 0x90));
        ui_label(r.x + r.w / 2, r.y + 4, 10, live ? UI_BG : UI_TEXT_DIM, UI_ALIGN_CENTER, live ? "MIC ON" : "MIC OFF");
        if (live) ui_rect(r.x + 3, r.y + r.h - 3, (r.w - 6) * fminf(mic_capture_level() * 3.0f, 1.0f), 2, UI_BG);
    }
    if (mvd_video_zoomed()) {
        /* Where the zoomed view sits in the picture. */
        const UiRect map = { CW - 96, CH - 62, 88, 50 };
        ui_rect_r(map, ui_with_alpha(UI_BG, 0x90));
        ui_outline(map.x, map.y, map.w, map.h, 1.0f, UI_LINE_STRONG);
        float cx, cy, cw, chh;
        mvd_video_view_crop(&cx, &cy, &cw, &chh);
        ui_rect(map.x + map.w * cx, map.y + map.h * cy, map.w * cw, map.h * chh, ui_with_alpha(UI_ACCENT, 0x40));
        ui_outline(map.x + map.w * cx, map.y + map.h * cy, map.w * cw, map.h * chh, 1.0f, UI_ACCENT);
    }
    if (app->welcome_at && plat_time_ms() - app->welcome_at < 3200) {
        const float a = ui_ease_out(clamp01((float)(plat_time_ms() - app->welcome_at) / 250.0f));
        const UiRect card = { 200, 120, 200, 90 };
        ui_rect_r(card, ui_with_alpha(UI_BG, (u8)(0xE8 * a)));
        ui_outline(card.x, card.y, card.w, card.h, 1.0f, ui_with_alpha(UI_ACCENT, (u8)(255 * a)));
        ui_text(CW / 2, card.y + 18, 17, ui_with_alpha(UI_ACCENT, (u8)(255 * a)), UI_ALIGN_CENTER, "おかえり");
        ui_label(CW / 2, card.y + 42, 12, ui_with_alpha(UI_TEXT, (u8)(255 * a)), UI_ALIGN_CENTER, "WELCOME BACK");
    }
}

static AppAction touch_stream(const App *app, int x, int y)
{
    if (app->controls_open) return ACTION_CONTROLS_CLOSE;
    if (app->stream_menu) {
        for (int i = 0; i < STREAM_MENU_COUNT; ++i)
            if (ui_hit(menu_item(i), x, y)) return (AppAction)(ACTION_MENU_RESUME + i);
        static const AppAction quick[QUICK_COUNT] = { ACTION_STREAM_KEYBOARD, ACTION_STREAM_POINTER, ACTION_STREAM_ZOOM,
                                                      ACTION_LOOK_TOGGLE, ACTION_MIC_TOGGLE, ACTION_NONE };
        for (int i = 0; i < QUICK_COUNT; ++i) {
            if (!ui_hit(quick_item(i), x, y)) continue;
            if ((i == QUICK_LOOK && !app->look_available) || (i == QUICK_MIC && !app->mic_available)) return ACTION_NONE;
            return quick[i];
        }
        if (!ui_hit(MENU_PANEL, x, y)) return ACTION_MENU_RESUME;
        return ACTION_NONE;
    }
    if (ui_hit(STREAM_HANDLE, x, y)) return ACTION_STREAM_MENU;
    if (app->mic_available && ui_hit(STREAM_MIC, x, y)) return ACTION_MIC_TOGGLE;
    return ACTION_NONE;
}

/* main.c's in-game touch: L3 / R3 / PS held by the touch (the PS button
 * sits in the stream menu here; L3 / R3 come from input.c). */
uint16_t screens_stream_held_buttons(const App *app, int x, int y)
{
    if (app->view != VIEW_STREAM || !app->stream_menu || app->keyboard_open) return 0;
    return ui_hit(quick_item(QUICK_PS), to_canvas(x), to_canvas(y)) ? GFN_PAD_GUIDE : 0;
}

/* In screen pixels, as main.c reads touches. The touchpad, the zoom pan and
 * the touch camera use the whole picture below the top handle. */
static UiRect screen_rect(UiRect r) { return (UiRect){ r.x * S, r.y * S, r.w * S, r.h * S }; }
UiRect screens_stream_panel(void) { return (UiRect){ 0, STREAM_HANDLE.h * S, UI_SCREEN_WIDTH, UI_SCREEN_HEIGHT - STREAM_HANDLE.h * S }; }
UiRect screens_look_pad(void) { return screens_stream_panel(); }
/* No R3 / HIDE / L3+R3 chips on the picture: double-tap is R3, the menu
 * hides the camera. */
UiRect screens_look_r3(void) { return (UiRect){ -100, -100, 0, 0 }; }
UiRect screens_look_hide(void) { return (UiRect){ -100, -100, 0, 0 }; }
UiRect screens_look_both(void) { return (UiRect){ -100, -100, 0, 0 }; }
UiRect screens_look_mic(void) { return screen_rect(STREAM_MIC); }

/* ---- Drawing and touch, by view ------------------------------------------------------------------ */

static void push_service_accent(const App *app)
{
    const bool menus = app->guide_page < 0 && !app->whats_new_open && !app->update_open;
    if (!menus) return;
    if (app->view == VIEW_HUB) ui_push_accent(service_light(app->hub_index));
    else if (app->view == VIEW_LIBRARY || app->view == VIEW_DETAILS || app->view == VIEW_SESSION)
        ui_push_accent(service_light(app_service(app)));
    else if (app->view == VIEW_LOGIN) ui_push_accent(service_light(login_service(app->client)));
}

static void draw_menus(const App *app)
{
    const bool guide = app->guide_page >= 0;
    const bool special = guide || app->whats_new_open || app->update_open || app->discord_open;
    const AppView v = app->view;
    /* Behind everything. */
    if (special || v == VIEW_SETTINGS) draw_wallpaper_backdrop();
    else if (v == VIEW_HUB) draw_hub_backdrop(app);
    else if (v == VIEW_LOGIN) draw_service_backdrop(login_service(app->client), 0.3f);
    else if (v == VIEW_LIBRARY && !gfn_has_session(app->client)) draw_service_backdrop(app_service(app), 0.45f);
    /* The view, rising as it fades in. */
    const int overlay = app->modal != MODAL_NONE ? 10 + (int)app->modal : app->options_open ? 3 : app->mapping_open ? 4
                      : app->pc_sheet_open ? 5 : 0;
    const float p = view_progress((int)v, overlay);
    const bool handover = g_zoom.at != 0;
    ui_offset(0.0f, handover ? 0.0f : (1.0f - p) * 10.0f);
    if (app->whats_new_open) draw_whats_new(app);
    else if (app->discord_open) draw_discord(app);
    else if (guide) draw_guide(app);
    else if (app->update_open) draw_update(app);
    else switch (v) {
    case VIEW_HUB: draw_hub(app); break;
    case VIEW_LOGIN: draw_login(app); break;
    case VIEW_LIBRARY:
        if (!gfn_has_session(app->client)) draw_signin(app);
        else {
            draw_library(app);
            if (app->pc_sheet_open) draw_pc_sheet(app);
        }
        break;
    case VIEW_SETTINGS:
        if (app->mapping_open) draw_mapping(app);
        else if (app->gallery_open) draw_gallery(app);
        else if (app->settings_section < 0) draw_settings_home(app);
        else draw_settings(app);
        break;
    case VIEW_SESSION: draw_session(app); break;
    case VIEW_DETAILS:
        draw_details(app);
        if (app->mapping_open) draw_mapping(app);
        else if (app->options_open) draw_options_sheet(app, overlay_progress());
        break;
    case VIEW_STREAM: break;
    }
    ui_offset(0.0f, 0.0f);
    if (!handover && p < 1.0f) ui_rect(0, TOP_H, CW, CH - TOP_H, ui_with_alpha(UI_BG, (u8)(255.0f * (1.0f - p))));
    draw_top_bar(app);
    draw_celebration();
}

void screens_draw_top(const App *app)
{
    static int previous_view = -1;
    canvas();
    push_service_accent(app);
    if (previous_view == VIEW_LOGIN && app->view == VIEW_LIBRARY && gfn_has_session(app->client) &&
        (status_from(app->client->status, "Signed in; session %s") || status_from(app->client->status, "Paired with %.60s")))
        celebrate(app);
    previous_view = (int)app->view;
    if (app->view == VIEW_STREAM) {
        view_progress((int)app->view, app->controls_open ? 2 : app->stream_menu ? 1 : 0);
        draw_stream(app);
    } else {
        draw_menus(app);
    }
    if (app->modal != MODAL_NONE) draw_modal(app, overlay_progress());
    if (app->busy) draw_busy(app);
    ui_pop_accent();
    ui_screen();
}

/* Everything is drawn by screens_draw_top on the Vita's one screen. */
void screens_draw_bottom(const App *app) { (void)app; }

AppAction screens_touch(const App *app, int sx, int sy)
{
    const int x = to_canvas(sx), y = to_canvas(sy);
    g_touched_option_row = g_touched_index = -1;
    if (app->busy || (app->view == VIEW_STREAM && app->keyboard_open)) return ACTION_NONE;
    if (app->modal != MODAL_NONE) return touch_modal(app, x, y);
    const bool menus = app->view != VIEW_STREAM;
    if (menus && ui_hit(TOP_BACK, x, y) && back_action(app) != ACTION_NONE) return back_action(app);
    if (menus && app->whats_new_open) return ui_hit(CONTINUE_BUTTON, x, y) ? ACTION_WHATS_NEW_CLOSE : ACTION_NONE;
    if (menus && app->discord_open) return ui_hit(CONTINUE_BUTTON, x, y) ? ACTION_DISCORD_CLOSE : ACTION_NONE;
    if (menus && app->guide_page >= 0) {
        if (ui_hit(guide_button(0), x, y)) return ACTION_GUIDE_BACK;
        if (ui_hit(guide_button(1), x, y)) return ACTION_GUIDE_SKIP;
        if (ui_hit(guide_button(2), x, y)) return ACTION_GUIDE_NEXT;
        return ACTION_NONE;
    }
    if (menus && app->update_open) return touch_update(x, y);
    switch (app->view) {
    case VIEW_HUB: return touch_hub(app, x, y);
    case VIEW_LOGIN:
        if (ui_hit(login_button(0), x, y)) return ACTION_NEW_CODE;
        if (ui_hit(login_button(1), x, y)) return ACTION_CANCEL;
        return ACTION_NONE;
    case VIEW_LIBRARY: return touch_library(app, x, y);
    case VIEW_SETTINGS: return touch_settings(app, x, y);
    case VIEW_SESSION: return touch_session(app, x, y);
    case VIEW_DETAILS: return touch_details(app, x, y);
    case VIEW_STREAM: return touch_stream(app, x, y);
    }
    return ACTION_NONE;
}

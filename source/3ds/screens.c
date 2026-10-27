#include "app.h"
#include "screens_model.h"
#include "platform.h"
#include "app_paths.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "audio_output.h"
#include "game_art.h"
#include "i18n.h"
#include "mic_capture.h"
#include "game_prefs.h"
#include "shortcut.h"
#include "gallery.h"
#include "http_client.h"
#include "menu_audio.h"
#include "updater.h"
#include "mvd_video.h"
#include "play_history.h"
#include "zoom_zones.h"
#include "remote_keyboard.h"
#include "stream_profile.h"
#include "regions.h"
#include "report.h"
#include "steam_link.h"

/* ---- Shared geometry (drawing and hit-testing use the same rects) -------- */

/* Library, lower screen: a roomy layout, and a compact one that makes space
 * for the "Continue" bar when a game was played before. */
typedef struct { UiRect prev, next, card, cont, play, library, search, settings; } LibraryLayout;
static const LibraryLayout LIB_ROOMY = {
    { 8, 34, 30, 84 }, { 282, 34, 30, 84 }, { 44, 34, 232, 84 }, { 0, 0, 0, 0 },
    { 16, 128, 288, 46 }, { 16, 184, 90, 48 }, { 115, 184, 90, 48 }, { 214, 184, 90, 48 },
};
static const LibraryLayout LIB_COMPACT = {
    { 8, 30, 30, 72 }, { 282, 30, 30, 72 }, { 44, 30, 232, 72 }, { 16, 108, 288, 30 },
    { 16, 144, 288, 40 }, { 16, 190, 90, 44 }, { 115, 190, 90, 44 }, { 214, 190, 90, 44 },
};

static const LibraryLayout *library_layout(const App *app)
{
    return app->continue_index >= 0 && !app->search_text[0] ? &LIB_COMPACT : &LIB_ROOMY;
}

/* A service not signed in: its sign-in card, the hub, Settings. */
static const UiRect SIGNIN_MAIN = { 40, 100, 240, 50 };
static const UiRect SIGNIN_HUB = { 40, 160, 116, 42 };
static const UiRect SIGNIN_SETTINGS = { 164, 160, 116, 42 };

/* The hub, lower screen: the focused service, a tile for each, Settings
 * and Exit. */
static const UiRect HOME_PANEL = { 16, 32, 288, 96 };
static UiRect home_tile(int i) { return (UiRect){ 16.0f + (float)i * 98.0f, 134.0f, 92.0f, 54.0f }; }
static const UiRect HUB_SETTINGS = { 16, 196, 140, 40 };
static const UiRect HUB_EXIT = { 164, 196, 140, 40 };
/* Inside a service, lower screen: back to the hub. */
static const UiRect HOME_BACK = { 0, 0, 96, 26 };

static int g_touched_service = -1;
int screens_touched_service(void) { return g_touched_service; }
/* Steam Link's PCs sheet: up to four PCs, then pair / forget, then close. */
static UiRect pc_row(int i) { return (UiRect){ 16, 30.0f + (float)i * 34.0f, 288, 30 }; }
static const UiRect PC_PAIR = { 16, 168, 140, 30 };
static const UiRect PC_FORGET = { 164, 168, 140, 30 };
static const UiRect PC_CLOSE = { 16, 204, 288, 30 };
static int g_touched_pc = -1;
int screens_touched_pc(void) { return g_touched_pc; }

/* Login and session: two mirrored buttons, or one centred. */
static const UiRect PAIR_LEFT = { 16, 186, 140, 44 };
static const UiRect PAIR_RIGHT = { 164, 186, 140, 44 };
static const UiRect SINGLE = { 60, 186, 200, 44 };

/* Settings. */
static const UiRect SET_PREV = { 16, 188, 60, 44 };
static const UiRect SET_BACK = { 84, 188, 152, 44 };
static const UiRect SET_NEXT = { 244, 188, 60, 44 };
/* Settings grid: three by two section tiles. */
static UiRect set_tile(int i)
{
    return (UiRect){ 16.0f + (float)(i % 3) * 98.0f, 34.0f + (float)(i / 3) * 76.0f, 92.0f, 70.0f };
}
static int g_touched_section = -1;
int screens_touched_section(void) { return g_touched_section; }

/* Stream. */
static const UiRect STR_L3 = { 6, 30, 56, 156 };
static const UiRect STR_R3 = { 258, 30, 56, 156 };
static const UiRect STR_PANEL = { 68, 30, 184, 112 };
static const UiRect STR_GUIDE = { 132, 146, 56, 42 };
#define STR_BUTTON_Y 194.0f
#define STR_BUTTON_H 40.0f

/* Modal and stream menu. */
static const UiRect MODAL_LEFT = { 40, 132, 116, 46 };
static const UiRect MODAL_RIGHT = { 164, 132, 116, 46 };
/* A modal with one button: centred where the pair sits. */
static const UiRect MODAL_ONLY = { 102, 132, 116, 46 };
static const UiRect MENU_PANEL = { 12, 6, 296, 228 };

/* Game details, lower screen. */
static const UiRect DET_PLAY = { 16, 34, 288, 58 };
static const UiRect DET_STORE_PREV = { 16, 106, 44, 44 };
static const UiRect DET_STORE_NEXT = { 260, 106, 44, 44 };
static const UiRect DET_FAV = { 16, 188, 92, 44 };
static const UiRect DET_OPTIONS = { 114, 188, 92, 44 };
static const UiRect DET_SHORTCUT = { 16, 158, 288, 24 };
static const UiRect DET_BACK = { 212, 188, 92, 44 };
static const UiRect OPT_CLOSE = { 246, 3, 64, 30 };
#define OPT_ROW_Y 39.0f
#define OPT_ROW_H 19.5f

/* Button mapping editor, lower screen. */
static const UiRect MAP_RESET = { 16, 196, 92, 38 };
static const UiRect MAP_CANCEL = { 114, 196, 92, 38 };
static const UiRect MAP_DONE = { 212, 196, 92, 38 };

/* Guide, lower screen. */
static const UiRect GUIDE_BACK = { 16, 188, 92, 44 };
static const UiRect GUIDE_SKIP = { 114, 188, 92, 44 };
static const UiRect GUIDE_NEXT = { 212, 188, 92, 44 };

/* Software update and what's new, lower screen. */
static const UiRect UPD_PRIMARY = { 16, 148, 288, 44 };
static const UiRect UPD_LATER = { 16, 200, 140, 34 };
static const UiRect UPD_CLOSE = { 164, 200, 140, 34 };
static const UiRect NEW_CONTINUE = { 60, 184, 200, 44 };

/* Which options row a tap landed on (read by main with the action). */
static int g_touched_option_row = -1;
int screens_touched_option_row(void) { return g_touched_option_row; }
/* The 3DS lists are walked with the buttons (the lower screen shows one
 * item at a time). */
int screens_touched_index(void) { return -1; }

/* Top screen list viewport shared by the library and settings. */
#define LIST_TOP 60.0f
#define LIST_BOTTOM 214.0f

static UiRect stream_button(int index)
{
    const float margin = 6.0f, gap = 6.0f;
    const float w = (UI_BOTTOM_WIDTH - 2 * margin - 3 * gap) / 4.0f;
    return (UiRect){ margin + index * (w + gap), STR_BUTTON_Y, w, STR_BUTTON_H };
}

static UiRect menu_item(int index)
{
    const float w = (MENU_PANEL.w - 32 - 8) / 2;
    return (UiRect){ MENU_PANEL.x + 16 + (index % 2) * (w + 8),
                     MENU_PANEL.y + 42 + (index / 2) * 45.0f, w, 38 };
}

UiRect screens_stream_panel(void) { return STR_PANEL; }

static bool pressed(const App *app, UiRect r)
{
    return app->touching && ui_hit(r, app->touch_x, app->touch_y);
}

/* ---- Animation state ------------------------------------------------------ */

/* Each screen fades and rises in when its view changes; the modal and the
 * stream menu fade in over it. Timestamps are per screen because the lower
 * screen is redrawn less often while streaming. */
typedef struct {
    int view;
    u64 since;
    int overlay;
    u64 overlay_since;
} ScreenAnim;

static ScreenAnim g_top_anim = { -1, 0, 0, 0 }, g_bottom_anim = { -1, 0, 0, 0 };
static bool g_bottom_busy_animating;

#define VIEW_FADE_MS 220.0f
#define OVERLAY_FADE_MS 160.0f

static float view_progress(ScreenAnim *anim, int view, int overlay)
{
    const u64 now = plat_time_ms();
    if (anim->view != view) {
        anim->view = view;
        anim->since = now;
    }
    if (anim->overlay != overlay) {
        anim->overlay = overlay;
        anim->overlay_since = now;
    }
    return ui_ease_out(ui_progress(anim->since, VIEW_FADE_MS));
}

static float overlay_progress(const ScreenAnim *anim)
{
    return ui_ease_out(ui_progress(anim->overlay_since, OVERLAY_FADE_MS));
}

bool screens_bottom_animating(void) { return g_bottom_busy_animating; }

/* A black veil over everything below the status bar, lifted as p -> 1. */
static void fade_in_veil(float width, float top, float p)
{
    if (p >= 1.0f) return;
    ui_rect(0, top, width, UI_HEIGHT - top, ui_with_alpha(UI_BG, (u8)(255.0f * (1.0f - p))));
}

/* ---- Common chrome ------------------------------------------------------- */

static void draw_status_bar(const App *app, float width)
{
    ui_wifi_icon(12.0f, 7.0f, app->wifi_bars, UI_TEXT, UI_LINE_STRONG);
    ui_battery_icon(36.0f, 7.0f, app->battery_level, app->charging);

    char clock[8] = "--:--";
    const time_t now = time(NULL);
    const struct tm *local = gmtime(&now);
    if (local) strftime(clock, sizeof(clock), "%H:%M", local);
    const float clock_w = ui_text(width - 12.0f, 5.0f, 12.0f, UI_TEXT, UI_ALIGN_RIGHT, clock);
    /* A quiet badge while a newer version waits (until dismissed). */
    if (app->view != VIEW_STREAM && updater_info().state == UPDATE_AVAILABLE && !updater_dismissed())
        ui_pill(width - 20.0f - clock_w, 4.0f, UI_ACCENT, UI_ALIGN_RIGHT, "UPDATE");

    /* Seal and wordmark centred as one group. */
    const float name_w = ui_text_width(APP_NAME, 12.0f);
    const float group_x = width / 2 - (16.0f + 6.0f + name_w) / 2;
    ui_seal(group_x, 4.0f, 16.0f);
    ui_text(group_x + 22.0f, 5.0f, 12.0f, UI_TEXT, UI_ALIGN_LEFT, APP_NAME);

    ui_hline(0, 25.0f, width, UI_LINE);
    ui_rect(width / 2 - 14.0f, 25.0f, 28.0f, 1.0f, UI_ACCENT);
}

static void draw_title(float center, float y, const char *jp, const char *en)
{
    if (ui_has_japanese()) {
        ui_text(center, y - 2.0f, 12.0f, UI_ACCENT, UI_ALIGN_CENTER, jp);
        ui_label(center, y + 12.0f, 11.0f, UI_TEXT, UI_ALIGN_CENTER, en);
    } else {
        ui_label(center, y + 6.0f, 12.0f, UI_TEXT, UI_ALIGN_CENTER, en);
    }
}

static void draw_footer(float width, const char *const *hints)
{
    ui_hline(16.0f, 218.0f, width - 32.0f, UI_LINE);
    ui_hint_row(width / 2, 223.0f, hints);
}

static void draw_status_strip(const App *app, const char *text)
{
    const bool signed_in = gfn_has_session(app->client);
    const bool toast = app->toast != NULL;
    if (toast) ui_rect(0, 0, UI_BOTTOM_WIDTH, 23, UI_ACCENT_DEEP);
    ui_circle(14.0f, 11.0f, 3.0f, signed_in ? UI_ACCENT : UI_TEXT_FAINT);
    ui_text_fit(UI_BOTTOM_WIDTH / 2, 5.0f, 11.0f, toast ? UI_TEXT : UI_TEXT_DIM, UI_ALIGN_CENTER,
                260.0f, toast ? app->toast : text);
    ui_circle(UI_BOTTOM_WIDTH - 14.0f, 11.0f, 3.0f, signed_in ? UI_ACCENT : UI_TEXT_FAINT);
    ui_hline(0, 23.0f, UI_BOTTOM_WIDTH, toast ? UI_ACCENT : UI_LINE);
}

static void draw_arrow(UiRect r, int direction, bool enabled, bool is_pressed)
{
    /* Same key as the buttons; a disabled one is a flat outline. */
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

/* Floating overlays share one card style: scrim, surface, accent rule. */
static void draw_card(UiRect r, float p)
{
    ui_rect_r(r, UI_SURFACE);
    ui_outline(r.x, r.y, r.w, r.h, 1.0f, UI_LINE_STRONG);
    ui_rect(r.x, r.y, r.w * p, 2, UI_ACCENT);
}

/* ---- Game hub: art and motion --------------------------------------------- */


static float clamp01(float v) { return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v; }

static float ease_in_out(float t)
{
    return t < 0.5f ? 4.0f * t * t * t : 1.0f - powf(-2.0f * t + 2.0f, 3.0f) / 2.0f;
}

/* Overshoots a little before settling: things that pop. */
static float ease_out_back(float t)
{
    const float c1 = 1.70158f, c3 = c1 + 1.0f, u = t - 1.0f;
    return 1.0f + c3 * u * u * u + c1 * u * u;
}

static UiRect lerp_rect(UiRect a, UiRect b, float t)
{
    return (UiRect){ a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.w + (b.w - a.w) * t, a.h + (b.h - a.h) * t };
}

/* A card growing into its service (A on the hub) or shrinking back (B). */
#define ZOOM_MS 460.0f
static struct {
    int service;
    int dir; /* 1: into the service, -1: back to the hub */
    u64 at;  /* 0: none */
} g_zoom;

void screens_hub_zoom(int service, bool into)
{
    g_zoom.service = service;
    g_zoom.dir = into ? 1 : -1;
    g_zoom.at = plat_time_ms();
}

bool screens_zoom_busy(void)
{
    return g_zoom.at && g_zoom.dir > 0 && ui_progress(g_zoom.at, ZOOM_MS) < 1.0f;
}

/* How far the card has grown: 0 a card, 1 the whole screen. */
static float zoom_peek(void)
{
    if (!g_zoom.at) return 0.0f;
    const float p = ui_progress(g_zoom.at, ZOOM_MS);
    if (g_zoom.dir < 0) return p >= 1.0f ? 0.0f : 1.0f - ease_in_out(p);
    /* Held at full until the service's screen takes over (or it gives up). */
    if (plat_time_ms() - g_zoom.at > (u64)ZOOM_MS + 1500) return 0.0f;
    return ease_in_out(p);
}

static float zoom_amount(void)
{
    const float z = zoom_peek();
    if (z <= 0.0f && g_zoom.at && (g_zoom.dir < 0 || plat_time_ms() - g_zoom.at > (u64)ZOOM_MS + 1500))
        g_zoom.at = 0;
    return z;
}

/* After growing into a service: 0 at the hand-over, 1 once settled. */
static float entered_amount(void)
{
    if (!g_zoom.at || g_zoom.dir < 0) return 1.0f;
    return clamp01(((float)(plat_time_ms() - g_zoom.at) - ZOOM_MS) / 560.0f);
}

/* The view changes under a zoom: no fade from black. */
static bool zoom_handover(void)
{
    if (!g_zoom.at) return false;
    if (g_zoom.dir < 0) return ui_progress(g_zoom.at, ZOOM_MS) < 1.0f;
    return entered_amount() < 1.0f;
}

static float hash01(unsigned i, unsigned k)
{
    unsigned x = i * 374761393u + k * 668265263u;
    x = (x ^ (x >> 13)) * 1274126177u;
    return (float)((x ^ (x >> 16)) & 0xFFFFu) / 65536.0f;
}

/* One petal: a narrow leaf shape along angle, turned edge-on by flip. */
static void draw_petal(float x, float y, float size, float angle, float flip, u32 color)
{
    const float ux = cosf(angle) * size, uy = sinf(angle) * size;
    const float px = -uy * 0.45f * flip, py = ux * 0.45f * flip;
    ui_triangle(x - ux, y - uy, x + px, y + py, x + ux, y + uy, color);
    ui_triangle(x - ux, y - uy, x - px, y - py, x + ux, y + uy, color);
}

/* Sakura petals drifting down on the wind, turning as they fall. */
#define PETAL_COLOR UI_RGBA(0xF0, 0xC4, 0xD2, 0xFF)

static void draw_petals(float width, float height, u32 color, float strength)
{
    if (strength <= 0.01f) return;
    const float t = (float)ui_ticks() / 1000.0f;
    for (unsigned i = 0; i < 16; ++i) {
        const float span = height + 30.0f;
        const float y = fmodf(t * (8.0f + 11.0f * hash01(i, 1)) + hash01(i, 2) * span, span) - 10.0f;
        if (y < 30.0f) continue;
        const float wander = sinf(t * (0.5f + hash01(i, 4)) + (float)i) * 16.0f;
        const float x = fmodf(hash01(i, 3) * (width + 40.0f) + t * (5.0f + 7.0f * hash01(i, 7)) + wander + 400.0f,
                              width + 40.0f) - 20.0f;
        const float spin = t * (0.6f + 1.4f * hash01(i, 6)) + (float)i;
        const float flip = 0.3f + 0.7f * fabsf(cosf(spin * 0.8f));
        const float fade = y < 52.0f ? (y - 30.0f) / 22.0f : 1.0f;
        const float a = strength * fade * (0.35f + 0.5f * hash01(i, 8));
        draw_petal(x, y, 2.2f + 2.0f * hash01(i, 5), spin, flip, ui_with_alpha(color, (u8)(255.0f * clamp01(a))));
    }
}

/* A service's art over the whole top screen, drifting slowly. */
static void draw_ambient(int service, float alpha)
{
    if (alpha <= 0.01f || service < 0 || service >= SERVICE_COUNT) return;
    const float t = (float)ui_ticks() / 1000.0f;
    const float zoom = 1.04f + 0.03f * sinf(t * 0.11f);
    const float w = 480.0f * zoom, h = 240.0f * zoom;
    const float dx = sinf(t * 0.07f) * 16.0f, dy = cosf(t * 0.09f) * 5.0f;
    ui_image_fit(SERVICE_ART[service], 200.0f - w / 2 + dx, 120.0f - h / 2 + dy, w, h, alpha);
}

/* An arc as short segments, from angle a0 over sweep (radians). */
static void draw_arc(float cx, float cy, float r, float thickness, float a0, float sweep, u32 color)
{
    const int n = (int)(fabsf(sweep) * r / 5.0f) + 4;
    float px = cx + cosf(a0) * r, py = cy + sinf(a0) * r;
    for (int i = 1; i <= n; ++i) {
        const float a = a0 + sweep * (float)i / (float)n;
        const float x = cx + cosf(a) * r, y = cy + sinf(a) * r;
        ui_line(px, py, x, y, thickness, color);
        px = x;
        py = y;
    }
}

/* A brush stroke round a circle: thick where it starts, thinning out. */
static void draw_brush_arc(float cx, float cy, float r, float a0, float sweep, float thick, float thin, u32 color)
{
    const int n = (int)(fabsf(sweep) * r / 4.0f) + 4;
    float px = cx + cosf(a0) * r, py = cy + sinf(a0) * r;
    for (int i = 1; i <= n; ++i) {
        const float f = (float)i / (float)n;
        const float a = a0 + sweep * f;
        /* The brush wobbles a little, as a hand would. */
        const float rr = r + sinf(f * 9.0f) * 0.8f;
        const float x = cx + cosf(a) * rr, y = cy + sinf(a) * rr;
        ui_line(px, py, x, y, thick + (thin - thick) * f, color);
        px = x;
        py = y;
    }
}


/* A service as a banner card: its art drifting inside, name and state on
 * a scrim, a glow and running lights when focused. focus 0..1; detail
 * fades the text out while the card grows into its service. */
static void draw_service_card(const App *app, int s, UiRect r, float alpha, float focus, float detail)
{
    const u32 light = service_light(s);
    const float t = (float)ui_ticks() / 1000.0f;
    if (focus > 0.05f) {
        ui_rect(r.x - 4, r.y - 4, r.w + 8, r.h + 8, ui_with_alpha(light, (u8)(focus * alpha * 26.0f)));
        ui_rect(r.x - 2, r.y - 2, r.w + 4, r.h + 4, ui_with_alpha(light, (u8)(focus * alpha * 30.0f)));
    }
    ui_rect(r.x, r.y, r.w, r.h, ui_with_alpha(UI_BG, (u8)(255.0f * alpha)));
    /* The scene drifts slowly inside the frame. */
    const float pan = 0.08f;
    const float u = (0.5f + 0.5f * sinf(t * 0.13f + (float)s)) * pan;
    const float v = (0.5f + 0.5f * cosf(t * 0.11f + (float)s * 2.0f)) * pan;
    ui_image_part(SERVICE_ART[s], r.x, r.y, r.w, r.h, u, v, u + 1.0f - pan, v + 1.0f - pan, alpha);
    if (focus < 0.99f) ui_rect(r.x, r.y, r.w, r.h, ui_with_alpha(UI_BG, (u8)(alpha * (1.0f - focus) * 120.0f)));
    const float k = r.w / 248.0f;
    if (detail > 0.01f) {
        const float da = detail * alpha;
        const u8 a8 = (u8)(255.0f * da);
        ui_gradient(r.x, r.y + r.h * 0.36f, r.w, r.h * 0.64f, ui_with_alpha(UI_BG, 0),
                    ui_with_alpha(UI_BG, (u8)(230.0f * da)));
        const float pad = 10.0f * k, icon = 24.0f * k, base = r.y + r.h - pad;
        ui_icon(SERVICE_ICON[s], r.x + pad + icon / 2, base - 17.0f * k, icon, ui_with_alpha(UI_TEXT, a8));
        const float tx = r.x + pad + icon + 8.0f * k, room = r.x + r.w - pad - tx;
        ui_text_fit(tx, base - 34.0f * k, 15.0f * k, ui_with_alpha(UI_TEXT, a8), UI_ALIGN_LEFT, room,
                    SERVICE_INFO[s].name);
        const bool ready = app->service_ready[s];
        ui_circle(tx + 3.0f * k, base - 7.0f * k, 2.5f * k, ui_with_alpha(ready ? light : UI_TEXT_FAINT, a8));
        const GfnGame *last = s == app->hub_index ? hub_last_game(app) : NULL;
        char status[128];
        if (last) snprintf(status, sizeof(status), i18n_text("Continue: %s"), last->title);
        ui_text_fit(tx + 10.0f * k, base - 14.0f * k, 11.0f * k, ui_with_alpha(ready ? UI_TEXT : UI_TEXT_DIM, a8),
                    UI_ALIGN_LEFT, room - 10.0f * k, last ? status : app->service_status[s]);
        /* The seal: the service's kanji, stamped in its colour. */
        if (ui_has_japanese()) {
            const float seal = 22.0f * k;
            ui_rect(r.x + pad, r.y + pad, seal, seal, ui_with_alpha(light, (u8)(230.0f * da)));
            ui_outline(r.x + pad + 2 * k, r.y + pad + 2 * k, seal - 4 * k, seal - 4 * k, 1.0f,
                       ui_with_alpha(UI_BG, (u8)(120.0f * da)));
            ui_text(r.x + pad + seal / 2, r.y + pad + 3.0f * k, 15.0f * k, ui_with_alpha(UI_BG, a8), UI_ALIGN_CENTER,
                    SERVICE_KANJI[s]);
        }
        /* A gold tag at 15 px (sharp), on the full-size card only: scaled
         * down on the side cards it would only blur. */
        if (s == SERVICE_XBOX && k > 0.94f) {
            const char *tag = app->xbox_paused ? "MAINTENANCE" : "BETA";
            const float bw = floorf(ui_text_width(tag, 15) + 12.0f), bh = 19.0f;
            const float bx = floorf(r.x + r.w - pad - bw), by = floorf(r.y + pad);
            ui_rect(bx, by, bw, bh, ui_with_alpha(app->xbox_paused ? UI_TEXT_DIM : UI_KIN, (u8)(235.0f * da)));
            ui_text(bx + bw / 2, by + 2, 15, ui_with_alpha(UI_BG, a8), UI_ALIGN_CENTER, tag);
        }
    }
    const bool lit = focus > 0.5f;
    ui_outline(r.x, r.y, r.w, r.h, 1.0f, ui_with_alpha(lit ? light : UI_TEXT, (u8)(alpha * (lit ? 200.0f : 50.0f))));
    if (lit && detail > 0.3f) {
        /* Corner ticks, as on the rest of Kasumi's frames. */
        const float c = 10.0f * k, w2 = 2.0f;
        const u32 tick = ui_with_alpha(UI_TEXT, (u8)(alpha * detail * 230.0f));
        ui_rect(r.x - 1, r.y - 1, c, w2, tick);
        ui_rect(r.x - 1, r.y - 1, w2, c, tick);
        ui_rect(r.x + r.w + 1 - c, r.y - 1, c, w2, tick);
        ui_rect(r.x + r.w - 1, r.y - 1, w2, c, tick);
        ui_rect(r.x - 1, r.y + r.h - 1, c, w2, tick);
        ui_rect(r.x - 1, r.y + r.h + 1 - c, w2, c, tick);
        ui_rect(r.x + r.w + 1 - c, r.y + r.h - 1, c, w2, tick);
        ui_rect(r.x + r.w - 1, r.y + r.h + 1 - c, w2, c, tick);
    }
}

/* First run: the seal draws itself before the cards arrive. */
static void draw_logo_intro(float since)
{
    const float in = ui_ease_out(clamp01(since / 520.0f));
    const float out = clamp01((since - 840.0f) / 320.0f);
    const float a = in * (1.0f - out);
    if (a <= 0.01f) return;
    const float cy = 104.0f - out * 26.0f;
    for (int k = 0; k < 3; ++k) {
        const float rt = clamp01((since - 120.0f - (float)k * 170.0f) / 760.0f);
        if (rt > 0.0f && rt < 1.0f)
            draw_arc(200, cy, 34.0f + rt * 110.0f, 2.0f - rt, 0.0f, 2.0f * (float)M_PI,
                     ui_with_alpha(UI_ACCENT, (u8)(160.0f * (1.0f - rt) * (1.0f - out))));
    }
    const float scale = (0.7f + 0.3f * ease_out_back(clamp01(since / 620.0f))) * 36.0f / 58.0f;
    ui_image_rotated(UI_IMAGE_ENSO, 200, cy, scale, (1.0f - in) * -2.4f, a);
    const u8 a8 = (u8)(255.0f * a);
    if (ui_has_japanese()) ui_text(200, cy - 16, 30, ui_with_alpha(UI_TEXT, a8), UI_ALIGN_CENTER, "霞");
    ui_label(200, cy + 48, 14, ui_with_alpha(UI_TEXT, a8), UI_ALIGN_CENTER, "KASUMI");
    ui_label(200, cy + 68, 10, ui_with_alpha(UI_TEXT_FAINT, a8), UI_ALIGN_CENTER, "YOUR GAMES, ON YOUR 3DS");
}

/* The hub's backdrop: the focused card's art, cross-fading as it moves. */
static float g_hub_pos = -1.0f;

static void draw_hub_backdrop(const App *app)
{
    if (g_hub_pos < 0.0f) g_hub_pos = (float)app->hub_index;
    g_hub_pos = ui_approach(g_hub_pos, (float)app->hub_index, 9.0f);
    const int a = (int)floorf(g_hub_pos);
    const float f = g_hub_pos - (float)a;
    /* The hub's own painting, drifting slowly; the focused service's scene
     * shows faintly through it. */
    const float t = (float)ui_ticks() / 1000.0f;
    const float zoom = 1.06f + 0.03f * sinf(t * 0.08f);
    const float w = UI_TOP_WIDTH * zoom, h = UI_HEIGHT * zoom;
    const bool painted = ui_image_fit(UI_IMAGE_HUB_BACKDROP, 200.0f - w / 2 + sinf(t * 0.06f) * 10.0f,
                                      120.0f - h / 2, w, h, 1.0f);
    const float tint = painted ? 0.28f : 0.5f;
    draw_ambient(a, tint);
    if (f > 0.01f && a + 1 < SERVICE_COUNT) draw_ambient(a + 1, tint * f);
    ui_gradient(0, 0, UI_TOP_WIDTH, 110, ui_with_alpha(UI_BG, 0xB4), ui_with_alpha(UI_BG, 0x38));
    ui_gradient(0, 110, UI_TOP_WIDTH, 130, ui_with_alpha(UI_BG, 0x38), ui_with_alpha(UI_BG, 0xE6));
    draw_petals(UI_TOP_WIDTH, UI_HEIGHT, PETAL_COLOR, 0.8f);
}

/* Inside a service: its art behind everything, bright at the hand-over
 * from the hub and then dimmed to base. */
static void draw_service_backdrop(int s, float base)
{
    const float e = ui_ease_out(entered_amount());
    draw_ambient(s, base + (1.0f - base) * (1.0f - e));
    ui_gradient(0, 0, UI_TOP_WIDTH, 80, ui_with_alpha(UI_BG, (u8)(0xC0 * e)), ui_with_alpha(UI_BG, (u8)(0x50 * e)));
    ui_gradient(0, 80, UI_TOP_WIDTH, 160, ui_with_alpha(UI_BG, (u8)(0x50 * e)), ui_with_alpha(UI_BG, (u8)(0xEC * e)));
    draw_petals(UI_TOP_WIDTH, UI_HEIGHT, PETAL_COLOR, 0.45f * e);
}

/* Signed in (or paired): rings, a check that draws itself, a burst. */
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
    ui_rect(0, 26, UI_TOP_WIDTH, UI_HEIGHT - 26, ui_with_alpha(UI_BG, (u8)(228.0f * a)));
    const float cx = 200, cy = 102;
    /* Ripples, as on still water. */
    for (int k = 0; k < 2; ++k) {
        const float rt = clamp01((t - 500.0f - (float)k * 220.0f) / 1000.0f);
        if (rt > 0.0f && rt < 1.0f)
            draw_arc(cx, cy, 40.0f + rt * 120.0f, 1.0f, 0.0f, 2.0f * (float)M_PI,
                     ui_with_alpha(UI_TEXT, (u8)(110.0f * (1.0f - rt) * (1.0f - out))));
    }
    /* The ensō, brushed in one stroke. */
    const float stroke = ui_ease_out(clamp01((t - 60.0f) / 560.0f));
    if (stroke > 0.0f)
        draw_brush_arc(cx, cy, 34.0f, -2.2f, stroke * 2.0f * (float)M_PI * 0.93f, 7.5f, 1.4f,
                       ui_with_alpha(light, (u8)(255.0f * (1.0f - out))));
    /* Petals scatter out from it. */
    for (unsigned i = 0; i < 14; ++i) {
        const float pt = clamp01((t - 420.0f) / (900.0f + 400.0f * hash01(i, 8)));
        if (pt <= 0.0f || pt >= 1.0f) continue;
        const float angle = (float)i / 14.0f * 2.0f * (float)M_PI + hash01(i, 9) * 0.4f;
        const float d = 40.0f + ui_ease_out(pt) * (50.0f + 70.0f * hash01(i, 7));
        draw_petal(cx + cosf(angle) * d, cy + sinf(angle) * d + pt * 18.0f, 3.2f, angle + pt * 4.0f,
                   0.4f + 0.6f * fabsf(cosf(pt * 6.0f + (float)i)),
                   ui_with_alpha(PETAL_COLOR, (u8)(220.0f * (1.0f - pt) * (1.0f - out))));
    }
    /* 完 (done), stamped in the middle. */
    const float stamp = clamp01((t - 520.0f) / 300.0f);
    if (stamp > 0.0f) {
        const float pop = ease_out_back(stamp);
        const u8 sa = (u8)(255.0f * stamp * (1.0f - out));
        if (ui_has_japanese())
            ui_text(cx, cy - 15.0f * pop, 30.0f * pop, ui_with_alpha(UI_TEXT, sa), UI_ALIGN_CENTER, "完");
        else
            ui_text(cx, cy - 10.0f * pop, 20.0f * pop, ui_with_alpha(UI_TEXT, sa), UI_ALIGN_CENTER, "OK");
    }
    const float text_in = ui_ease_out(clamp01((t - 620.0f) / 380.0f)) * (1.0f - out);
    const u8 ta = (u8)(255.0f * text_in);
    ui_label(200, 150 + (1.0f - text_in) * 8.0f, 14, ui_with_alpha(UI_TEXT, ta), UI_ALIGN_CENTER,
             g_celebrate_service == SERVICE_STEAM ? "PAIRED" : "SIGNED IN");
    ui_text_fit(200, 172 + (1.0f - text_in) * 8.0f, 12, ui_with_alpha(light, ta), UI_ALIGN_CENTER, 360,
                g_celebrate_text);
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
        snprintf(g_celebrate_text, sizeof(g_celebrate_text), "%s is ready. Your games are on their way.",
                 SERVICE_INFO[s].name);
}

/* The lower screen inside a service: back to the hub, and whose it is. */
static void draw_service_header(const App *app)
{
    if (app->toast) {
        draw_status_strip(app, app->status);
        return;
    }
    const int s = app_service(app);
    const u32 light = service_light(s);
    ui_gradient(0, 0, UI_BOTTOM_WIDTH, 26, ui_with_alpha(light, 0x58), ui_with_alpha(light, 0x14));
    if (pressed(app, HOME_BACK)) ui_rect_r(HOME_BACK, ui_with_alpha(UI_TEXT, 0x24));
    ui_triangle(18, 13, 24, 7, 24, 19, UI_TEXT);
    ui_label(30, 7, 11, UI_TEXT, UI_ALIGN_LEFT, "HOME");
    const float name_w = ui_text_width(SERVICE_INFO[s].name, 12);
    ui_text(306, 6, 12, UI_TEXT, UI_ALIGN_RIGHT, SERVICE_INFO[s].name);
    ui_icon(SERVICE_ICON[s], 306 - name_w - 12, 13, 14, UI_TEXT);
    ui_hline(0, 26, UI_BOTTOM_WIDTH, ui_with_alpha(light, 0xA0));
}


/* The hub and a service's screens take its colour as the accent. */
static void push_service_accent(const App *app)
{
    const bool menus = app->guide_page < 0 && !app->whats_new_open && !app->update_open;
    if (!menus) return;
    if (app->view == VIEW_HUB) ui_push_accent(service_light(app->hub_index));
    else if (app->view == VIEW_LIBRARY || app->view == VIEW_DETAILS || app->view == VIEW_SESSION)
        ui_push_accent(service_light(app_service(app)));
    else if (app->view == VIEW_LOGIN) ui_push_accent(service_light(login_service(app->client)));
}

/* The lower screen in the hub and inside a service: the painting carries
 * on below the top screen's (its lower part, dimmed) instead of the theme
 * wallpaper. False where the wallpaper stays (settings, guides, game). */
static bool draw_painted_bottom(const App *app)
{
    if (app->guide_page >= 0 || app->whats_new_open || app->update_open || app->discord_open) return false;
    const AppView v = app->view;
    if (v != VIEW_HUB && v != VIEW_LIBRARY && v != VIEW_DETAILS && v != VIEW_LOGIN && v != VIEW_SESSION)
        return false;
    const float t = (float)ui_ticks() / 1000.0f;
    const float drift = sinf(t * 0.06f) * 0.03f;
    bool drawn;
    if (v == VIEW_HUB) {
        /* 400x240: the lower middle, 4:3. */
        drawn = ui_image_part(UI_IMAGE_HUB_BACKDROP, 0, 0, UI_BOTTOM_WIDTH, UI_HEIGHT, 0.24f + drift, 0.35f,
                              0.76f + drift, 1.0f, 1.0f);
    } else {
        const int s = v == VIEW_LOGIN ? login_service(app->client) : app_service(app);
        /* 512x256: the lower half, 4:3. */
        drawn = ui_image_part(SERVICE_ART[s], 0, 0, UI_BOTTOM_WIDTH, UI_HEIGHT, 0.33f + drift, 0.5f, 0.67f + drift,
                              1.0f, 1.0f);
    }
    if (!drawn) return false;
    ui_gradient(0, 0, UI_BOTTOM_WIDTH, UI_HEIGHT, ui_with_alpha(UI_BG, 0xA8), ui_with_alpha(UI_BG, 0xD0));
    draw_petals(UI_BOTTOM_WIDTH, UI_HEIGHT, PETAL_COLOR, 0.3f);
    return true;
}

/* A game's cover over the whole top screen, soft (scaled far up) and dim:
 * the backdrop of its page and of its start. */
static void draw_cover_backdrop(const GfnGame *game, float alpha)
{
    if (!game || alpha <= 0.01f) return;
    const float t = (float)ui_ticks() / 1000.0f;
    const float w = 420.0f, scale = w / GAME_ART_WIDTH, h = GAME_ART_HEIGHT * scale;
    const float x = 200.0f - w / 2 + sinf(t * 0.07f) * 8.0f, y = 120.0f - h * 0.42f + cosf(t * 0.05f) * 6.0f;
    const UiImage special = special_cover(game);
    if (special != UI_IMAGE_COUNT) ui_image_fit(special, x, y, w, h, alpha);
    else game_art_draw(game, x, y, scale, alpha);
    ui_gradient(0, 26, UI_TOP_WIDTH, 107, ui_with_alpha(UI_BG, 0x90), ui_with_alpha(UI_BG, 0x50));
    ui_gradient(0, 133, UI_TOP_WIDTH, 107, ui_with_alpha(UI_BG, 0x50), ui_with_alpha(UI_BG, 0xE8));
}

/* Behind the shelf: the focused game's art, soft, fading from the last
 * one to the next as the selection moves. */
static void draw_focus_backdrop(const App *app)
{
    static const GfnGame *shown, *previous;
    static u64 changed_at;
    const GfnGame *game = app_game(app, app->selected);
    if (game != shown) {
        previous = shown;
        shown = game;
        changed_at = plat_time_ms();
    }
    const float settle = ui_ease_out(entered_amount());
    const float f = ui_ease_out(ui_progress(changed_at, 380.0f));
    const float t = (float)ui_ticks() / 1000.0f;
    const float w = 420.0f, scale = w / GAME_ART_WIDTH, h = GAME_ART_HEIGHT * scale;
    const float x = 200.0f - w / 2 + sinf(t * 0.07f) * 8.0f, y = 120.0f - h * 0.42f;
    const GfnGame *layers[2] = { f < 1.0f ? previous : NULL, shown };
    const float alphas[2] = { 0.19f * (1.0f - f), 0.19f * f };
    for (int i = 0; i < 2; ++i) {
        const GfnGame *g = layers[i];
        if (!g || alphas[i] * settle <= 0.01f) continue;
        const UiImage special = special_cover(g);
        if (special != UI_IMAGE_COUNT) ui_image_fit(special, x, y, w, h, alphas[i] * settle);
        else game_art_draw(g, x, y, scale, alphas[i] * settle);
    }
    ui_gradient(0, 26, UI_TOP_WIDTH, 60, ui_with_alpha(UI_BG, 0x70), ui_with_alpha(UI_BG, 0x00));
}

/* A tag at 15 px: filled for the one that counts, outlined otherwise. */
static float draw_tag(float x, float y, const char *text, u32 color, bool filled)
{
    const float w = floorf(ui_text_width(text, 15) + 12.0f), h = 19.0f;
    if (filled) {
        ui_rect(x, y, w, h, color);
        ui_text(x + w / 2, y + 2, 15, UI_BG, UI_ALIGN_CENTER, text);
    } else {
        ui_outline(x, y, w, h, 1.0f, color);
        ui_text(x + w / 2, y + 2, 15, color, UI_ALIGN_CENTER, text);
    }
    return w;
}

static void draw_busy_top(const App *app)
{
    ui_rect(0, 26, UI_TOP_WIDTH, 214, UI_SCRIM);
    const UiRect card = { 80, 80, 240, 96 };
    draw_card(card, 1.0f);
    ui_enso(200, 114, 16, UI_ACCENT);
    ui_text_wrap(200, 140, 12, UI_TEXT, UI_ALIGN_CENTER, 220, 2, 15, app->busy);
}

/* ---- Top screens --------------------------------------------------------- */

/* Mist that sways slowly sideways; oversized so its faded edges stay off-screen. */
static void draw_mist(float y, float alpha)
{
    const float sway = sinf((float)ui_ticks() / 9000.0f * 2.0f * (float)M_PI) * 18.0f;
    ui_image(UI_IMAGE_MIST, -26.0f + sway, y, 1.13f, alpha);
}

/* The game hub: each service a banner card on a carousel, the focused one
 * big in the middle. A grows it to fill the screen and goes in. */
static u64 g_hub_shown_at;

static void draw_hub_top(const App *app)
{
    const u64 now = plat_time_ms();
    if (!g_hub_shown_at) g_hub_shown_at = now;
    const float since = (float)(now - g_hub_shown_at);
    const bool first = !app->settings.hub_done;
    const float delay = first ? 1000.0f : 40.0f;
    if (first) draw_logo_intro(since);
    const float z = zoom_amount();
    const int zs = g_zoom.at ? g_zoom.service : -1;
    const float chrome = clamp01((since - delay - 220.0f) / 320.0f) * (1.0f - z);
    if (chrome > 0.01f) {
        /* One line at 15 px (half the font's size: the only small size that
         * stays sharp), on a soft band so it reads over any painting. */
        const u8 a = (u8)(255.0f * chrome);
        ui_gradient(0, 26, UI_TOP_WIDTH, 36, ui_with_alpha(UI_BG, (u8)(150.0f * chrome)), ui_with_alpha(UI_BG, 0));
        static const char *const jp = "何で遊ぶ？", *const en = "Choose where to play";
        const float jw = ui_has_japanese() ? ui_text_width(jp, 15) + 10.0f : 0.0f, ew = ui_text_width(en, 15);
        const float x0 = floorf(200.0f - (jw + ew) / 2.0f);
        if (ui_has_japanese()) ui_text(x0, 33, 15, ui_with_alpha(service_light(app->hub_index), a), UI_ALIGN_LEFT, jp);
        ui_text(x0 + jw, 33, 15, ui_with_alpha(UI_TEXT, a), UI_ALIGN_LEFT, en);
    }
    /* Far cards first, so the focused one sits on top. */
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
        const float d = (float)i - g_hub_pos, ad = fminf(fabsf(d), 1.0f);
        const float w = 248.0f - 80.0f * ad;
        const float cx = 200.0f + d * 200.0f;
        float cy = 126.0f + 4.0f * ad;
        float alpha = 1.0f - 0.4f * ad;
        /* Arriving: the cards rise in, the focused one first. */
        const float in = ui_ease_out(clamp01((since - delay - 110.0f * fabsf((float)(i - app->hub_index))) / 480.0f));
        cy += (1.0f - in) * 70.0f;
        alpha *= in;
        UiRect r = { cx - w / 2, cy - w / 4, w, w / 2 };
        float focus = 1.0f - ad, detail = 1.0f;
        if (i == zs) {
            const UiRect full = { -40, 0, 480, 240 };
            r = lerp_rect(r, full, z);
            detail = 1.0f - z * 1.6f;
            focus = 1.0f;
            alpha += (1.0f - alpha) * z;
        } else if (zs >= 0) {
            r.x += (d < 0.0f ? -1.0f : 1.0f) * z * 260.0f;
            alpha *= 1.0f - z;
        }
        if (alpha > 0.01f) draw_service_card(app, i, r, alpha, focus, detail < 0.0f ? 0.0f : detail);
        /* Coming back out: the card starts as dim as the service's backdrop
         * and brightens as it shrinks. */
        if (i == zs && g_zoom.dir < 0) ui_rect(r.x, r.y, r.w, r.h, ui_with_alpha(UI_BG, (u8)(z * 175.0f)));
    }
    /* A flash as the card fills the screen. */
    if (zs >= 0 && g_zoom.dir > 0 && z > 0.8f)
        ui_rect(0, 0, UI_TOP_WIDTH, UI_HEIGHT, ui_with_alpha(UI_TEXT, (u8)((z - 0.8f) / 0.2f * 48.0f)));
    if (chrome > 0.6f) {
        ui_dots(200, 199, SERVICE_COUNT, (unsigned)app->hub_index, service_light(app->hub_index), UI_LINE_STRONG);
        /* Settings and Exit are buttons on the lower screen too; START
         * continues the last game when the focused service has one. */
        static const char *const hints[] = { "◀ ▶", "Choose", "A", "Enter", "START", "Exit", NULL };
        static const char *const continue_hints[] = { "◀ ▶", "Choose", "A", "Enter", "START", "Continue", NULL };
        ui_gradient(0, 208, UI_TOP_WIDTH, 32, ui_with_alpha(UI_BG, 0), ui_with_alpha(UI_BG, 0xD0));
        draw_footer(UI_TOP_WIDTH, hub_last_game(app) ? continue_hints : hints);
    }
}

/* A service that isn't set up: its card, what it is, and its sign-in. */
static void draw_signin_top(const App *app)
{
    const int s = app_service(app);
    const u32 light = service_light(s);
    const float in = ui_ease_out(entered_amount());
    const float t = (float)ui_ticks() / 1000.0f;
    /* The card settles in from the zoom, gently bobbing. */
    const UiRect card = { 82, 34.0f + sinf(t * 1.3f) * 2.0f, 236, 118 };
    draw_service_card(app, s, card, in, 1.0f, 1.0f);
    const u8 a8 = (u8)(255.0f * in);
    ui_text_wrap(200, 160, 12, ui_with_alpha(UI_TEXT, a8), UI_ALIGN_CENTER, 360, 2, 15, SERVICE_INFO[s].about);
    /* A sign-in that failed says why ("No PC with Steam found", "Your
     * Microsoft sign-in has expired") instead of what is needed. */
    const GfnClient *client = app->client;
    const bool failed = client->auth_state == GFN_AUTH_ERROR && client->status[0];
    if (failed) {
        ui_rect(30, 190, 340, 28, ui_with_alpha(UI_KIN, 0x28));
        ui_rect(30, 190, 2, 28, UI_KIN);
        ui_text_wrap(200, 191, 11, UI_KIN, UI_ALIGN_CENTER, 330, 2, 13, client->status);
    } else {
        char needs[96];
        snprintf(needs, sizeof(needs), i18n_text("You need: %s"), SERVICE_INFO[s].needs);
        ui_text_wrap(200, 194, 11, ui_with_alpha(light, a8), UI_ALIGN_CENTER, 360, 2, 13, needs);
    }
    static const char *const hints[] = { "A", "Set up", "B", "Home", "SELECT", "Settings", NULL };
    static const char *const retry_hints[] = { "A", "Try again", "B", "Home", "SELECT", "Settings", NULL };
    /* Steam Link: X types the PC's address where the search can't find it. */
    static const char *const steam_hints[] = { "A", "Set up", "X", "PC address", "B", "Home", NULL };
    static const char *const steam_retry_hints[] = { "A", "Try again", "X", "PC address", "B", "Home", NULL };
    const bool steam = s == SERVICE_STEAM;
    if (!failed) draw_footer(UI_TOP_WIDTH, steam ? steam_hints : hints);
    else ui_hint_row(200, 223, steam ? steam_retry_hints : retry_hints);
}

static void draw_login_top(const App *app)
{
    const GfnClient *client = app->client;
    const bool steam = login_is_steam(client);
    const int s = login_service(client);
    const u32 light = service_light(s);
    const float since = (float)(plat_time_ms() - g_top_anim.since);
    ui_icon(SERVICE_ICON[s], 200, 40, 20, light);
    ui_label(200, 54, 11, UI_TEXT, UI_ALIGN_CENTER, steam ? "PAIR WITH YOUR PC"
             : login_is_xbox(client) ? "SIGN IN WITH XBOX" : "SIGN IN WITH NVIDIA");
    ui_text(200, 70, 11, UI_TEXT_DIM, UI_ALIGN_CENTER,
            steam ? "Steam asks for a code on" : "On your phone or computer, open");
    ui_text_fit(200, 84, 15, light, UI_ALIGN_CENTER, 368, client->verification_uri);

    /* One cell per character of the code: they drop in one by one, then a
     * light sweeps across them while Kasumi waits. */
    const size_t length = strlen(client->user_code);
    const float cell = 28.0f, gap = 6.0f;
    float total = 0;
    for (size_t i = 0; i < length; ++i)
        total += (client->user_code[i] == '-' ? 10.0f : cell) + (i + 1 < length ? gap : 0);
    float x = 200 - total / 2;
    const float sweep = fmodf(since / 1000.0f * 0.8f, 1.6f) * (total + 80.0f) - 40.0f + (200 - total / 2);
    for (size_t i = 0; i < length; ++i) {
        char glyph[2] = { client->user_code[i], 0 };
        if (glyph[0] == '-') {
            ui_rect(x + 2, 128, 6, 2, UI_TEXT_FAINT);
            x += 10.0f + gap;
            continue;
        }
        const float drop = ease_out_back(clamp01((since - 80.0f * (float)i) / 380.0f));
        const float y = 106.0f - (1.0f - drop) * 24.0f;
        const u8 a8 = (u8)(255.0f * clamp01(drop));
        ui_rect(x, y, cell, 40, ui_with_alpha(UI_SURFACE, a8));
        const float near = clamp01(1.0f - fabsf(x + cell / 2 - sweep) / 34.0f);
        if (near > 0.0f) ui_rect(x, y, cell, 40, ui_with_alpha(light, (u8)(70.0f * near * clamp01(drop))));
        ui_outline(x, y, cell, 40, 1.0f, ui_with_alpha(ui_mix(UI_LINE_STRONG, light, near), a8));
        ui_rect(x, y + 38, cell, 2, ui_with_alpha(light, a8));
        ui_text(x + cell / 2, y + 7, 22, ui_with_alpha(UI_TEXT, a8), UI_ALIGN_CENTER, glyph);
        x += cell + gap;
    }

    const char *waiting = steam ? "Type it in the Authorize Device window" : "Waiting for approval";
    const float w = ui_text_width(waiting, 11);
    ui_enso(200 - w / 2 - 12, 172, 6, light);
    ui_text(200 - w / 2 + 2, 166, 11, UI_TEXT_DIM, UI_ALIGN_LEFT, waiting);
    const long remaining = (long)(client->challenge_expires_at - (int64_t)time(NULL));
    if (remaining > 0)
        ui_textf(200, 188, 11, remaining < 60 ? UI_KIN : UI_TEXT_FAINT, UI_ALIGN_CENTER,
                 "Code expires in %ld:%02ld", remaining / 60, remaining % 60);
    static const char *const hints[] = { "Y", "New code", "B", "Cancel", NULL };
    draw_footer(UI_TOP_WIDTH, hints);
}

/* Smoothly gliding selection bar shared by the library and settings. */
static void draw_selection(float y, float h, float alpha)
{
    ui_rect(14, y, 372, h, ui_with_alpha(UI_RAISED, (u8)(255 * alpha)));
    ui_rect(14, y, 2, h, ui_with_alpha(UI_ACCENT, (u8)(255 * alpha)));
}

/* The list leaves room on the right for the selected game's box art. */
#define LIB_LIST_W 266.0f
#define LIB_ART_X 290.0f
#define LIB_ART_Y 64.0f

/* A placeholder card: the title's first letter on the seigaiha texture. */
static void draw_art_placeholder(const GfnGame *game, float x, float y, float w, float h)
{
    /* An ensō over mist (gfx/no_cover.png); the name on it when it's big
     * enough to read. */
    if (ui_image(UI_IMAGE_NO_COVER, x, y, w / GAME_ART_WIDTH, 1.0f)) {
        if (h > 80 && game)
            ui_text_wrap(x + w / 2, y + h * 0.74f, 11, UI_TEXT, UI_ALIGN_CENTER, w - 10, 2, 13, game->title);
        return;
    }
    ui_rect(x, y, w, h, UI_SURFACE);
    ui_outline(x, y, w, h, 1.0f, UI_LINE);
    char initial[2] = { game && game->title[0] ? game->title[0] : '?', 0 };
    if (initial[0] >= 'a' && initial[0] <= 'z') initial[0] = (char)(initial[0] - 32);
    ui_text(x + w / 2, y + h / 2 - (h > 80 ? 18 : 10), h > 80 ? 32 : 18, UI_LINE_STRONG,
            UI_ALIGN_CENTER, initial);
}

/* Draws a game's art (or its placeholder) fitted to w x h at (x, y). */
static void draw_game_art(const GfnGame *game, float x, float y, float w, float alpha)
{
    const float scale = w / GAME_ART_WIDTH, h = GAME_ART_HEIGHT * scale;
    const UiImage special = special_cover(game);
    if (special != UI_IMAGE_COUNT) {
        ui_image_fit(special, x, y, w, h, alpha);
    } else {
        game_art_want(game);
        if (!game_art_draw(game, x, y, scale, alpha)) draw_art_placeholder(game, x, y, w, h);
    }
    ui_outline(x - 1, y - 1, w + 2, h + 2, 1.0f, UI_LINE_STRONG);
}

/* A cover on the shelf: the game's art, Steam Link's own art, or the
 * placeholder with the title on it. */
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

/* The cover upside down under the shelf, fading out. */
static void draw_cover_reflection(const GfnGame *game, float x, float y, float w, float alpha)
{
    const float scale = w / GAME_ART_WIDTH, h = GAME_ART_HEIGHT * scale;
    const UiImage special = special_cover(game);
    if (special != UI_IMAGE_COUNT) {
        ui_image_fade(special, x, y, w, h, alpha, 0.0f, true);
        return;
    }
    if (!game_art_draw_fade(game, x, y, scale, alpha, 0.0f, true))
        ui_image_fade(UI_IMAGE_NO_COVER, x, y, w, h, alpha, 0.0f, true);
}

/* ALL / FAV / RECENT at the top right, switched with L and R. */
static void draw_library_tabs(const App *app, u32 light)
{
    static const char *const names[LIBRARY_TAB_COUNT] = { "ALL", "FAV", "RECENT" };
    ui_button_chip(371, 28, "R", UI_TEXT_FAINT);
    float x = 364;
    for (int i = LIBRARY_TAB_COUNT - 1; i >= 0; --i) {
        const float w = ui_text_width(names[i], 11) + 4;
        x -= w;
        const bool on = i == app->library_tab;
        ui_label(x, 30, 11, on ? UI_TEXT : UI_TEXT_FAINT, UI_ALIGN_LEFT, names[i]);
        if (on) ui_rect(x, 42, w, 2, light);
        x -= 12;
    }
    ui_button_chip(x - 7, 28, "L", UI_TEXT_FAINT);
}

/* Where the shelf is (a game index, fractional while it glides). */
static float g_shelf_pos = -1.0f;

static void draw_library_top(const App *app, bool entering)
{
    const int s = app_service(app);
    const u32 light = service_light(s);
    const bool searching = app->search_text[0] != '\0';
    const size_t count = app->list_count;
    const float enter_t = entered_amount();
    const float t = (float)ui_ticks() / 1000.0f;

    /* Header: the service, the tabs or the search, the game in focus. */
    ui_icon(SERVICE_ICON[s], 23, 36, 15, light);
    ui_label(35, 30, 11, light, UI_ALIGN_LEFT, SERVICE_TABS[s]);
    if (searching) {
        char query[96];
        snprintf(query, sizeof(query), i18n_text("SEARCH  \"%s\""), app->search_text);
        ui_text_fit(386, 30, 11, UI_TEXT_DIM, UI_ALIGN_RIGHT, 200, query);
    } else if (app->client->game_count) {
        draw_library_tabs(app, light);
    }
    const GfnGame *focused = app_game(app, app->selected);
    if (focused && !app->service_loading) {
        char counter[64], age[32] = "";
        library_age(app->client, age, sizeof(age));
        snprintf(counter, sizeof(counter), i18n_text("%lu / %lu%s%s"), (unsigned long)(app->selected + 1), (unsigned long)count,
                 age[0] ? "  ·  " : "", age);
        const float counter_w = ui_text_width(counter, 10);
        ui_text(386, 50, 10, UI_TEXT_FAINT, UI_ALIGN_RIGHT, counter);
        ui_text_fit(16, 46, 15, UI_TEXT, UI_ALIGN_LEFT, 360 - counter_w, focused->title);
    }

    if (app->service_loading) {
        /* Switching service: the shelf's outline, breathing. */
        const float pulse = 0.5f + 0.5f * sinf(t * 4.0f);
        for (int k = -2; k <= 2; ++k) {
            const float sc = k ? 0.66f : 1.0f, w = 96.0f * sc, h = 128.0f * sc;
            const float cx = 200.0f + (float)k * 88.0f;
            ui_rect(cx - w / 2, 192 - h, w, h, ui_with_alpha(light, (u8)(18.0f + 22.0f * pulse)));
        }
        ui_enso(200, 128, 14, light);
    } else if (!count) {
        const char *title = "No games here yet";
        const char *hint = searching ? "Try a different search." : "Press Y to load your library, or X to search.";
        GfnProvider partner;
        bool only = false;
        static char wrong[160];
        if (!searching && !app->client->game_count && provider_is_nvidia() && providers_partner_here(&partner, &only) &&
            only) {
            char country[4];
            providers_country(country, sizeof(country));
            snprintf(wrong, sizeof(wrong), i18n_text("GeForce NOW in %s is %s's: sign out in Settings > Account and pick %s."),
                     providers_country_name(country), partner.name, partner.name);
            title = "Signed in with NVIDIA";
            hint = wrong;
        } else if (!searching && app->client->game_count && app->library_tab == LIBRARY_TAB_FAVOURITES) {
            title = "No favourites yet";
            hint = "Open a game and press Y to add it here.";
        } else if (!searching && app->client->game_count && app->library_tab == LIBRARY_TAB_RECENT) {
            title = "Nothing played on Kasumi yet";
            hint = "Games you play show up here, newest first.";
        }
        if (!ui_image(UI_IMAGE_LANTERN, 164, 70, 0.75f, 1.0f)) {
            ui_ring(200, 114, 26, 1.5f, UI_LINE_STRONG, UI_BG);
            ui_text(200, 104, 18, UI_TEXT_FAINT, UI_ALIGN_CENTER, "空");
        }
        ui_text(200, 150, 13, UI_TEXT, UI_ALIGN_CENTER, title);
        ui_text_wrap(200, 168, 11, UI_TEXT_DIM, UI_ALIGN_CENTER, 340, 2, 13, hint);
    } else {
        if (g_shelf_pos < 0.0f || entering || g_shelf_pos > (float)count) g_shelf_pos = (float)app->selected;
        g_shelf_pos = ui_approach(g_shelf_pos, (float)app->selected, 13.0f);
        /* Ask for the covers in the order they matter: the focused one,
         * then its neighbours outward. They load (and fade in) that way. */
        for (int k = 0; k <= 5; ++k) {
            const long after = (long)app->selected + k, before = (long)app->selected - k;
            if (after < (long)count) game_art_want(app_game(app, (size_t)after));
            if (k && before >= 0) game_art_want(app_game(app, (size_t)before));
        }
        /* The covers near the focus, far ones first. */
        int items[12];
        int n = 0;
        const int lo = (int)floorf(g_shelf_pos) - 4, hi = (int)ceilf(g_shelf_pos) + 4;
        for (int i = lo < 0 ? 0 : lo; i <= hi && i < (int)count && n < 12; ++i) items[n++] = i;
        for (int i = 1; i < n; ++i)
            for (int j = i; j > 0 && fabsf((float)items[j] - g_shelf_pos) > fabsf((float)items[j - 1] - g_shelf_pos);
                 --j) {
                const int swap = items[j];
                items[j] = items[j - 1];
                items[j - 1] = swap;
            }
        const float bottom = 192.0f;
        for (int k = 0; k < n; ++k) {
            const int i = items[k];
            const GfnGame *game = app_game(app, (size_t)i);
            if (!game) continue;
            const float d = (float)i - g_shelf_pos, ad = fabsf(d);
            const float scale = ad < 1.0f ? 1.0f - 0.32f * ad : fmaxf(0.5f, 0.68f - 0.06f * (ad - 1.0f));
            const float w = GAME_ART_WIDTH * scale, h = GAME_ART_HEIGHT * scale;
            const float off = ad <= 1.0f ? ad * 88.0f : 88.0f + (ad - 1.0f) * 66.0f;
            float cx = 200.0f + (d < 0.0f ? -off : off);
            float alpha = ad <= 1.0f ? 1.0f - 0.3f * ad : fmaxf(0.0f, 0.7f - 0.22f * (ad - 1.0f));
            /* From the hub: the covers slide in from the right, one by one. */
            const float appear = ui_ease_out(clamp01((enter_t - 0.06f * (d + 4.0f)) / 0.5f));
            cx += (1.0f - appear) * 70.0f;
            alpha *= appear;
            if (alpha <= 0.01f) continue;
            const float x = cx - w / 2, y = bottom - h;
            const bool on = ad < 0.5f;
            if (on) {
                ui_rect(x - 5, y - 5, w + 10, h + 10, ui_with_alpha(light, (u8)(alpha * 22.0f)));
                ui_rect(x - 3, y - 3, w + 6, h + 6, ui_with_alpha(light, (u8)(alpha * 30.0f)));
                draw_cover_reflection(game, x, bottom + 3, w, 0.30f * alpha);
            }
            draw_cover(game, x, y, w, alpha);
            if (on) {
                ui_outline(x - 1, y - 1, w + 2, h + 2, 2.0f, ui_with_alpha(light, (u8)(255.0f * alpha)));
                if (game_prefs_favourite(game->app_id)) {
                    ui_circle(x + w - 9, y + 9, 7, ui_with_alpha(UI_BG, 0xC0));
                    ui_text(x + w - 9, y + 2, 11, UI_KIN, UI_ALIGN_CENTER, "★");
                }
            } else {
                ui_rect(x, y, w, h, ui_with_alpha(UI_BG, (u8)(alpha * 70.0f)));
                ui_outline(x - 1, y - 1, w + 2, h + 2, 1.0f, ui_with_alpha(UI_TEXT, (u8)(alpha * 40.0f)));
            }
        }
    }

    ui_gradient(0, 196, UI_TOP_WIDTH, 44, ui_with_alpha(UI_BG, 0x00), ui_with_alpha(UI_BG, 0xF0));
    if (focused && !app->service_loading) {
        /* In search, a game the account doesn't have says so instead of
         * its store (they were a common "can't play this game"). */
        unsigned library_games = 0;
        const bool missing = searching && gfn_library_known(&library_games) && library_games && !gfn_in_library(focused);
        char line[160];
        /* Paid-only on a free account: NVIDIA refuses it after the queue. */
        const bool paid = app_service(app) == SERVICE_GFN && focused->needs_paid;
        snprintf(line, sizeof(line), "%s  ·  %s", missing ? i18n_text("NOT IN LIBRARY") : paid ? i18n_text("PAID MEMBERSHIP ONLY")
                 : store_label(focused->store), stream_line(app));
        ui_text_fit(200, 200, 11, missing || paid ? UI_KIN : UI_TEXT_DIM, UI_ALIGN_CENTER, 370, line);
    }
    /* L and R sit beside the tabs, so the row stays short enough for 15 px. */
    static const char *const hints[] = { "◀ ▶", "Browse", "A", "Open", "X", "Search", "B", "Home", NULL };
    static const char *const pc_hints[] = { "A", "Choose", "X", "PC address", "B", "Close", NULL };
    static const char *const search_hints[] = { "◀ ▶", "Browse", "A", "Open", "X", "Search", "B", "Library",
                                                NULL };
    draw_footer(UI_TOP_WIDTH, app->pc_sheet_open ? pc_hints : searching ? search_hints : hints);
}

static void draw_session_top(const App *app)
{
    static float progress_x;
    const GfnClient *client = app->client;
    const bool failed = screens_session_failed(app);
    const int stage = session_stage(app);
    const bool reconnecting = (app->reconnect_attempt > 0 && app->reconnect_attempt <= 3) || app->waiting_wifi;
    static const char *const kanji[] = { "待", "準", "接", "始" };
    static const char *const stages[SERVICE_COUNT][4] = {
        { "In the queue", "Preparing your rig", "Connecting", "Starting the stream" },
        { "Waiting for an Xbox", "Preparing the Xbox", "Connecting", "Starting the stream" },
        { "Waiting for the PC", "Starting on your PC", "Connecting to your PC", "Starting the stream" },
    };
    const char *const *en = stages[app_service(app)];
    const char *service_name = SERVICE_INFO[app_service(app)].name;

    if (failed && !reconnecting && client->session_state == GFN_SESSION_ERROR &&
        !strcmp(client->fail_code, "ended")) {
        /* A normal end (often the player quitting in-game), not an error. */
        ui_enso(200, 84, 34, UI_ACCENT);
        ui_text(200, 70, 26, UI_TEXT, UI_ALIGN_CENTER, "終");
        draw_title(200, 126, "終了", "SESSION ENDED");
        ui_text_fit(200, 156, 13, UI_TEXT_DIM, UI_ALIGN_CENTER, 360,
                    app->game_title[0] ? app->game_title : service_name);
        ui_text_wrap(200, 172, 11, UI_TEXT_FAINT, UI_ALIGN_CENTER, 340, 2, 14,
                     app->end_note[0] ? app->end_note : client->status);
        static const char *const ended_hints[] = { "A", "Play again", "B", "Leave", NULL };
        draw_footer(UI_TOP_WIDTH, ended_hints);
        return;
    }
    if (failed && !reconnecting) {
        ui_ring(200, 86, 34, 2.0f, UI_DANGER, UI_BG);
        ui_text(200, 68, 32, UI_DANGER, UI_ALIGN_CENTER, "!");
        draw_title(200, 132, "エラー", "SESSION PROBLEM");
        const char *detail = client->session_state == GFN_SESSION_ERROR ? client->status :
                             app->transport->state == WEBRTC_FAILED ? app->transport->status :
                             app->signal->status;
        ui_text_wrap(200, 162, 11, UI_TEXT_DIM, UI_ALIGN_CENTER, 340, 3, 14, detail);
        static const char *const hints[] = { "A", "Retry", "B", "Leave", NULL };
        draw_footer(UI_TOP_WIDTH, hints);
        return;
    }

    /* The cover starts big in the middle and settles at the left, ripples
     * spreading from it, over its own soft backdrop. */
    const GfnGame *game = app->session_game;
    const u32 light = service_light(app_service(app));
    const float since = (float)(plat_time_ms() - g_top_anim.since);
    const float in = ease_in_out(clamp01(since / 700.0f));
    draw_cover_backdrop(game, 0.34f * clamp01(since / 500.0f));
    const float cw = 132.0f - 36.0f * in;
    const float ch = GAME_ART_HEIGHT * cw / GAME_ART_WIDTH;
    const float ccx = 200.0f - 116.0f * in, ccy = 120.0f - 12.0f * in;
    for (int k = 0; k < 2; ++k) {
        const float rt = clamp01((since - 150.0f - (float)k * 260.0f) / 1100.0f);
        if (rt > 0.0f && rt < 1.0f)
            draw_arc(ccx, ccy, 50.0f + rt * 150.0f, 1.0f, 0.0f, 2.0f * (float)M_PI,
                     ui_with_alpha(UI_TEXT, (u8)(110.0f * (1.0f - rt))));
    }
    if (game) {
        const float gx = ccx - cw / 2, gy = ccy - ch / 2;
        ui_rect(gx - 5, gy - 5, cw + 10, ch + 10, ui_with_alpha(light, 22));
        ui_rect(gx - 3, gy - 3, cw + 6, ch + 6, ui_with_alpha(light, 30));
        draw_cover(game, gx, gy, cw, 1.0f);
        ui_outline(gx - 1, gy - 1, cw + 2, ch + 2, 2.0f, reconnecting ? UI_KIN : light);
    }
    /* The words arrive once the cover has settled. */
    const float words = ui_ease_out(clamp01((since - 450.0f) / 400.0f));
    const u8 wa = (u8)(255.0f * words);
    const float x = 156.0f + (1.0f - words) * 10.0f, w = 230.0f;
    const char *seal = reconnecting ? "再" : kanji[stage];
    const char *what = reconnecting ? "Reconnecting" : en[stage];
    /* The stage: its kanji on a seal, then its name. */
    if (ui_has_japanese()) {
        ui_rect(x, 40, 22, 22, ui_with_alpha(reconnecting ? UI_KIN : light, wa));
        ui_text(x + 11, 43, 15, ui_with_alpha(UI_BG, wa), UI_ALIGN_CENTER, seal);
    }
    ui_text_fit(x + (ui_has_japanese() ? 30.0f : 0.0f), 43, 15, ui_with_alpha(UI_TEXT, wa), UI_ALIGN_LEFT, w - 30,
                what);
    float y = 72;
    if (!reconnecting && stage == 0 && client->queue_position > 0) {
        /* The place in the queue at 30 px, the font's own size: crisp. */
        ui_textf(x, y, 30, ui_with_alpha(light, wa), UI_ALIGN_LEFT, "#%d", client->queue_position);
        y += 38;
    } else {
        if (words > 0.3f) ui_enso(x + 14, y + 16, 13, reconnecting ? UI_KIN : light);
        y += 38;
    }
    const int title_lines = ui_text_wrap(x, y, 15, ui_with_alpha(UI_TEXT, wa), UI_ALIGN_LEFT, w, 2, 18,
                                         app->game_title[0] ? app->game_title : service_name);
    y += (float)title_lines * 18.0f + 6.0f;
    char detail[160] = "";
    if (reconnecting) {
        if (app->waiting_wifi)
            snprintf(detail, sizeof(detail), "%s", app->lid_paused
                     ? i18n_text("Paused with the lid closed; reconnecting when you open it.")
                     : i18n_text("Waiting for Wi-Fi to come back; your game keeps running."));
        else
            snprintf(detail, sizeof(detail), i18n_text("The connection dropped. Attempt %u of 3; your game keeps running."),
                     app->reconnect_attempt);
    } else if (stage == 0) {
        if (app->queue_eta > 90) snprintf(detail, sizeof(detail), i18n_text("About %d min to go"), (app->queue_eta + 30) / 60);
        else if (app->queue_eta > 0) snprintf(detail, sizeof(detail), i18n_text("About a minute to go"));
        else if (app->queue_eta == 0) snprintf(detail, sizeof(detail), i18n_text("Your rig should be ready any moment"));
        else snprintf(detail, sizeof(detail), "%s", app_service(app) == SERVICE_XBOX ? i18n_text("Waiting for a free Xbox")
                      : client->queue_position > 0 ? i18n_text("Your place in NVIDIA's queue") : i18n_text("Waiting for a free rig"));
    } else if (stage >= 2) {
        snprintf(detail, sizeof(detail), "%s", app->transport->peer ? app->transport->status : app->signal->status);
    }
    if (detail[0])
        ui_text_wrap(x, y, 12, ui_with_alpha(UI_TEXT_DIM, wa), UI_ALIGN_LEFT, w, 2, 15, detail);

    /* Four-step progress: queue, rig, signal, stream. The filled line glides. */
    static const char *const steps[] = { "QUEUE", "RIG", "SIGNAL", "STREAM" };
    const float step_x0 = 95, step_dx = 70, step_y = 194;
    progress_x = ui_approach(progress_x, step_dx * (float)stage, 8.0f);
    ui_hline(step_x0, step_y, step_dx * 3, UI_LINE);
    ui_hline(step_x0, step_y, progress_x, UI_ACCENT);
    const float pulse = 0.5f + 0.5f * sinf((float)ui_ticks() / 180.0f);
    for (int i = 0; i < 4; ++i) {
        const float x = step_x0 + step_dx * i;
        if (i < stage) ui_circle(x, step_y, 4, UI_ACCENT);
        else if (i == stage) {
            ui_circle(x, step_y, 5.5f + pulse, ui_with_alpha(UI_ACCENT, 0x50));
            ui_ring(x, step_y, 4.5f, 1.5f, UI_ACCENT, UI_BG);
        } else ui_ring(x, step_y, 4, 1.0f, UI_LINE_STRONG, UI_BG);
        ui_label(x, step_y + 8, 11, i <= stage ? UI_TEXT_DIM : UI_TEXT_FAINT, UI_ALIGN_CENTER, steps[i]);
    }
    static const char *const hints[] = { "B", "Leave", NULL };
    ui_hint_row(200, 223, hints);
}

/* ---- Screenshot viewer ---------------------------------------------------- */

static const UiRect GAL_DELETE = { 16, 132, 140, 44 };
static const UiRect GAL_BACK = { 164, 132, 140, 44 };

static void draw_gallery_top(const App *app)
{
    (void)app;
    const UiRect frame = { 34, 30, 332, 187 };
    ui_rect(frame.x - 1, frame.y - 1, frame.w + 2, frame.h + 2, UI_LINE_STRONG);
    ui_rect(frame.x, frame.y, frame.w, frame.h, UI_BG);
    if (!gallery_draw(frame.x, frame.y, frame.w, frame.h))
        ui_text(200, frame.y + frame.h / 2 - 8, 12, UI_TEXT_FAINT, UI_ALIGN_CENTER, "Loading...");
    static const char *const hints[] = { "L R", "Browse", "X", "Delete", "B", "Back", NULL };
    draw_footer(UI_TOP_WIDTH, hints);
}

static void draw_gallery_bottom(const App *app)
{
    ui_text(160, 2, 12, UI_ACCENT, UI_ALIGN_CENTER, "写真");
    ui_label(160, 16, 11, UI_TEXT, UI_ALIGN_CENTER, "SCREENSHOTS");
    ui_hline(0, 29, UI_BOTTOM_WIDTH, UI_LINE);
    const unsigned count = gallery_count();
    ui_textf(160, 50, 22, UI_TEXT, UI_ALIGN_CENTER, "%u / %u", count ? gallery_index() + 1 : 0, count);
    ui_text(160, 84, 12, UI_TEXT_DIM, UI_ALIGN_CENTER, gallery_caption());
    ui_button(GAL_DELETE, "DELETE", "削除", UI_BUTTON_DANGER, pressed(app, GAL_DELETE));
    ui_button(GAL_BACK, "BACK", "戻る", UI_BUTTON_NORMAL, pressed(app, GAL_BACK));
    const bool more = count > 1;
    draw_arrow(SET_PREV, -1, more, pressed(app, SET_PREV));
    draw_arrow(SET_NEXT, 1, more, pressed(app, SET_NEXT));
    ui_label(160, 204, 11, UI_TEXT_FAINT, UI_ALIGN_CENTER, "L  /  R");
}

/* Settings home: the six sections, matching the grid below. */
static void draw_settings_home_top(const App *app)
{
    static float bar_y;
    draw_title(200, 31, "設定", "SETTINGS");
    ui_hline(16, 57, 368, UI_LINE);
    const int count = screens_section_count();
    const float row_h = 25.0f;
    const float target = LIST_TOP + app->settings_grid * row_h;
    bar_y = bar_y == 0.0f ? target : ui_approach(bar_y, target, 24.0f);
    draw_selection(bar_y, row_h - 1, 1.0f);
    for (int i = 0; i < count; ++i) {
        const SettingEntry *h = section_header(i);
        const float y = LIST_TOP + i * row_h;
        const bool on = i == app->settings_grid;
        if (!ui_image_tint((UiImage)(UI_IMAGE_SEC_CONTROLS + i), 20, y + 2, 0.5f, on ? UI_ACCENT : UI_TEXT_FAINT))
            ui_text(30, y + 4, 13, on ? UI_ACCENT : UI_TEXT_FAINT, UI_ALIGN_CENTER, SECTION_INFO[i].kanji);
        ui_label(46, y + 6, 11, on ? UI_TEXT : UI_TEXT_DIM, UI_ALIGN_LEFT, h ? h->en : "");
        ui_text_fit(378, y + 5, 11, on ? UI_TEXT_DIM : UI_TEXT_FAINT, UI_ALIGN_RIGHT, 200,
                    SECTION_INFO[i].summary);
    }
    static const char *const hints[] = { "A", "Open", "B", "Save & back", NULL };
    draw_footer(UI_TOP_WIDTH, hints);
}

static void draw_settings_top(const App *app, bool entering)
{
    static float scroll, bar_y;
    static int last_section = -1;
    if (app->gallery_open) {
        draw_gallery_top(app);
        return;
    }
    if (app->settings_section < 0) {
        last_section = -1;
        draw_settings_home_top(app);
        return;
    }
    /* A new section starts at its top, without gliding over from the last. */
    if (app->settings_section != last_section) entering = true;
    last_section = app->settings_section;
    const int section = app->settings_section, sections = screens_section_count();
    const SettingEntry *header = section_header(section);
    draw_title(200, 31, header ? header->jp : "設定", header ? header->en : "SETTINGS");
    /* L and R step through the sections. */
    const SettingEntry *prev = section_header((section + sections - 1) % sections);
    const SettingEntry *next = section_header((section + 1) % sections);
    const float chip_w = ui_button_chip(16, 34, "L", UI_TEXT_DIM);
    ui_label(16 + chip_w + 5, 36, 11, UI_TEXT_FAINT, UI_ALIGN_LEFT, prev ? prev->en : "");
    const float next_w = ui_text_width(next ? next->en : "", 11) + 6;
    ui_label(384 - 22 - next_w, 36, 11, UI_TEXT_FAINT, UI_ALIGN_LEFT, next ? next->en : "");
    ui_button_chip(384 - 16, 34, "R", UI_TEXT_DIM);
    ui_hline(16, 57, 368, UI_LINE);

    /* Lay the grouped list out once per frame. */
    enum { HEADER_H = 22, ROW_H = 20 };
    float ys[SETTING_ENTRY_COUNT];
    float content = 0.0f, selected_y = 0.0f;
    const int selected = screens_setting_at(app->setting_index);
    /* Only this section's rows; its name is the title. */
    int in_section = -1;
    for (int i = 0; i < SETTING_ENTRY_COUNT; ++i) {
        if (SETTING_ENTRIES[i].setting < 0) ++in_section;
        ys[i] = content;
        if (SETTING_ENTRIES[i].setting < 0 || in_section != section) {
            ys[i] = -1000.0f;
            continue;
        }
        if (SETTING_ENTRIES[i].setting == selected) selected_y = content;
        content += ROW_H;
    }
    const float view_h = LIST_BOTTOM - LIST_TOP;
    float target = selected_y - view_h / 2.0f + ROW_H / 2.0f;
    if (target > content - view_h) target = content - view_h;
    if (target < 0.0f) target = 0.0f;
    scroll = entering ? target : ui_approach(scroll, target, 16.0f);
    const float bar_target = LIST_TOP + selected_y - scroll;
    bar_y = entering || bar_y == 0.0f ? bar_target : ui_approach(bar_y, bar_target, 24.0f);

    /* Rows fade out at the viewport edges instead of being cut off. */
    #define EDGE_ALPHA(top, h) \
        (fminf(1.0f, fmaxf(0.0f, ((top) - LIST_TOP + 8.0f) / 8.0f)) * \
         fminf(1.0f, fmaxf(0.0f, (LIST_BOTTOM - ((top) + (h)) + 8.0f) / 8.0f)))

    const float bar_alpha = EDGE_ALPHA(bar_y, ROW_H - 1);
    if (bar_alpha > 0.0f) draw_selection(bar_y, ROW_H - 1, bar_alpha);

    for (int i = 0; i < SETTING_ENTRY_COUNT; ++i) {
        const SettingEntry *e = &SETTING_ENTRIES[i];
        if (ys[i] < -999.0f) continue;
        const float y = LIST_TOP + ys[i] - scroll;
        const float h = e->setting < 0 ? HEADER_H : ROW_H;
        const float a = EDGE_ALPHA(y, h);
        if (a <= 0.02f) continue;
        const u8 alpha = (u8)(255.0f * a);
        if (e->setting < 0) {
            if (ui_has_japanese()) {
                const float w = ui_text(18, y + 5, 11, ui_with_alpha(UI_ACCENT, alpha), UI_ALIGN_LEFT, e->jp);
                ui_label(18 + w + 6, y + 6, 11, ui_with_alpha(UI_TEXT_FAINT, alpha), UI_ALIGN_LEFT, e->en);
            } else {
                ui_label(18, y + 6, 11, ui_with_alpha(UI_ACCENT, alpha), UI_ALIGN_LEFT, e->en);
            }
            continue;
        }
        const int s = e->setting;
        const bool is_selected = s == selected;
        const bool account = s == SETTING_ACCOUNT;
        const float label_w = ui_text(28, y + 3, 12,
                                      ui_with_alpha(is_selected ? UI_TEXT : UI_TEXT_DIM, alpha),
                                      UI_ALIGN_LEFT, SETTING_LABELS[s]);
        ui_text(28 + label_w + 7, y + 4, 11, ui_with_alpha(UI_TEXT_FAINT, alpha), UI_ALIGN_LEFT,
                SETTING_JP[s]);
        const u32 value_color = account ? UI_DANGER : is_selected ? UI_TEXT : UI_TEXT_DIM;
        const float value_right = is_selected && !account ? 364.0f : 378.0f;
        const float value_w = ui_text(value_right, y + 3, 12, ui_with_alpha(value_color, alpha),
                                      UI_ALIGN_RIGHT, setting_value(app, s));
        if (is_selected && !account) {
            const float cy = y + 9.5f;
            const float lx = value_right - value_w - 10;
            ui_triangle(lx + 3, cy - 4, lx + 3, cy + 4, lx - 2, cy, ui_with_alpha(UI_ACCENT, alpha));
            ui_triangle(371, cy - 4, 371, cy + 4, 376, cy, ui_with_alpha(UI_ACCENT, alpha));
        }
    }
    #undef EDGE_ALPHA

    /* Scroll rail. */
    if (content > view_h) {
        const float rail_h = view_h - 2;
        const float thumb_h = rail_h * view_h / content;
        const float thumb_y = LIST_TOP + (rail_h - thumb_h) * scroll / (content - view_h);
        ui_vline(392, LIST_TOP, rail_h, UI_LINE);
        ui_rect(391, thumb_y, 3, thumb_h, UI_ACCENT);
    }
    static const char *const hints[] = { "A", "Change", "L R", "Section", "B", "Sections", NULL };
    draw_footer(UI_TOP_WIDTH, hints);
}

/* "Help improve Kasumi?": asked once, after an update or the first start. */
static void draw_share_ask_top(float p)
{
    ui_rect(0, 26, UI_TOP_WIDTH, 214, ui_with_alpha(UI_BG, (u8)(0xE0 * p)));
    ui_offset(0.0f, (1.0f - p) * 10.0f);
    const UiRect panel = { 36, 36, 328, 176 };
    draw_card(panel, p);
    ui_enso(200, 66, 20, ui_with_alpha(UI_ACCENT, (u8)(0xFF * p)));
    ui_text(200, 55, 18, UI_ACCENT, UI_ALIGN_CENTER, "協");
    draw_title(200, 92, "協力のお願い", "HELP IMPROVE KASUMI?");
    ui_text_wrap(200, 118, 11, UI_TEXT_DIM, UI_ALIGN_CENTER, 300, 2, 14,
                 "Share diagnostics with the developer to find bugs and make streaming smoother.");
    static const char *const points[3] = {
        "Problem reports: the log, after a crash or failed stream",
        "Performance stats: ping and smoothness after each session",
        "Never your login · change it in Settings > System",
    };
    for (int i = 0; i < 3; ++i) {
        const float y = 150 + i * 15.0f;
        ui_rounded(76, y + 5, 4, 4, 2.0f, UI_ACCENT);
        ui_text(86, y, 11, UI_TEXT, UI_ALIGN_LEFT, points[i]);
    }
    static const char *const hints[] = { "A", "Share", "B", "No thanks", NULL };
    ui_hint_row(200, 200, hints);
    ui_offset(0.0f, 0.0f);
}

static void draw_modal_top(const App *app, float p)
{
    if (app->modal == MODAL_SHARE_ASK) {
        draw_share_ask_top(p);
        return;
    }
    ui_rect(0, 26, UI_TOP_WIDTH, 214, ui_with_alpha(UI_BG, (u8)(0xC8 * p)));
    ui_offset(0.0f, (1.0f - p) * 10.0f);
    const UiRect panel = { 60, 56, 280, 128 };
    draw_card(panel, p);
    draw_title(200, 66, app->modal_jp, app->modal_title);
    if (app->modal == MODAL_REPORT_SENT) {
        /* The code is what the player writes down: make it big. */
        ui_text(200, 92, 26, UI_ACCENT, UI_ALIGN_CENTER, app->report_code);
        ui_text_wrap(200, 126, 11, UI_TEXT_DIM, UI_ALIGN_CENTER, 250, 2, 14, app->modal_text);
    } else {
        ui_text_wrap(200, 98, 11, UI_TEXT_DIM, UI_ALIGN_CENTER, 250, 4, 14, app->modal_text);
    }
    static const char *const confirm[] = { "A", "Confirm", "B", "Cancel", NULL };
    static const char *const error[] = { "A", "Retry", "B", "Back", NULL };
    static const char *const send[] = { "A", "Send", "B", "Cancel", NULL };
    static const char *const done[] = { "A", "OK", NULL };
    static const char *const resume_other[] = { "A", "Resume", "B", "Back", NULL };
    static const char *const end_other[] = { "A", "End it", "B", "Back", NULL };
    static const char *const wait[] = { "A", "Try now", "B", "Stop", NULL };
    static const char *pick[] = { "A", "", "X", "NVIDIA", "B", "Back", NULL };
    static GfnProvider partner;
    if (app->modal == MODAL_PROVIDER_PICK) {
        providers_partner_here(&partner, NULL);
        pick[1] = partner.name;
    }
    ui_hint_row(200, 162, app->modal == MODAL_ERROR ? error : app->modal == MODAL_SEND_REPORT ? send :
                          app->modal == MODAL_REPORT_SENT ? done :
                          app->modal == MODAL_CONFLICT ? (app->conflict_same_game ? resume_other : end_other) :
                          app->modal == MODAL_LIMIT_WAIT ? wait :
                          app->modal == MODAL_PROVIDER_PICK ? pick : confirm);
    ui_offset(0.0f, 0.0f);
}

/* ---- Game details ---------------------------------------------------------- */

/* ---- HOME Menu shortcut sheet ----------------------------------------------- */

/* A dot running round an ensō: "working, don't leave". */
static void draw_working_ring(float cx, float cy, float r)
{
    ui_enso(cx, cy, r, ui_with_alpha(UI_ACCENT, 0x60));
    const float a = (float)ui_ticks() * 0.005f;
    ui_circle(cx + r * cosf(a), cy + r * sinf(a), 3.5f, UI_ACCENT);
}

static const char *shortcut_step_text(void)
{
    static char text[64];
    if (shortcut_removing()) return "Taking it off the HOME Menu...";
    switch (shortcut_step()) {
    case SHORTCUT_STEP_FETCH: return "Getting the game's art...";
    case SHORTCUT_STEP_DRAW: return "Drawing the icon and banner...";
    case SHORTCUT_STEP_BUILD: return "Building the shortcut...";
    case SHORTCUT_STEP_INSTALL:
        snprintf(text, sizeof(text), i18n_text("Installing on the HOME Menu  %u%%"), shortcut_progress() / 10);
        return text;
    case SHORTCUT_STEP_DONE: return "Done";
    }
    return "";
}

/* Top screen: the real banner and icon as they will look, the steps, then
 * where to find it. */
static void draw_shortcut_top(const App *app, const GfnGame *game)
{
    static float bar;
    const int sheet = app->shortcut_sheet;
    const bool removing = shortcut_removing() && (sheet == SHORTCUT_SHEET_WORKING || sheet == SHORTCUT_SHEET_REMOVED);
    /* Below the status bar (wifi, battery, clock: rows 0-25). */
    ui_text(16, 31, 12, UI_ACCENT, UI_ALIGN_LEFT, "近道");
    const float tag_w = ui_text_width("近道", 12);
    ui_label(16 + tag_w + 6, 33, 11, UI_TEXT_FAINT, UI_ALIGN_LEFT, "HOME MENU SHORTCUT");
    ui_text_fit(384, 32, 11, UI_TEXT_DIM, UI_ALIGN_RIGHT, 170, game->title);
    /* The banner as the HOME Menu will show it (at 80 %), the icon beside it. */
    const float scale = 0.8f, bw = 256 * scale, bh = 128 * scale;
    const float bx = 132, by = 52;
    ui_rect(bx - 1, by - 1, bw + 2, bh + 2, UI_LINE_STRONG);
    if (!removing && shortcut_preview_banner(bx, by, scale)) {
        ui_rect(55, by + bh / 2 - 25, 50, 50, UI_LINE_STRONG);
        shortcut_preview_icon(56, by + bh / 2 - 24, 1.0f);
    } else {
        ui_rect(bx, by, bw, bh, UI_SURFACE);
        if (sheet == SHORTCUT_SHEET_WORKING) draw_working_ring(bx + bw / 2, by + bh / 2 - 8, 16);
        if (sheet == SHORTCUT_SHEET_WORKING)
            ui_text(bx + bw / 2, by + bh / 2 + 16, 11, UI_TEXT_FAINT, UI_ALIGN_CENTER,
                    removing ? "Removing" : "Drawing...");
    }
    const float y = 172;
    if (sheet == SHORTCUT_SHEET_WORKING) {
        /* Three steps on one bar, as the software update shows them. */
        static const char *const steps[] = { "ART", "BUILD", "INSTALL" };
        const ShortcutStep now = shortcut_step();
        const int step = removing || now == SHORTCUT_STEP_INSTALL ? 2 : now == SHORTCUT_STEP_BUILD ? 1
                         : now == SHORTCUT_STEP_DONE ? 3 : 0;
        const float overall = step >= 3 ? 1.0f : (step + (step == 2 ? shortcut_progress() / 1000.0f : 0.5f)) / 3.0f;
        bar = ui_approach(bar, overall, 10.0f);
        const float x0 = 70, w = 260;
        ui_rect(x0, y, w, 4, UI_RAISED);
        if (bar > 0.005f) ui_rect(x0, y, w * bar, 4, UI_ACCENT);
        for (int i = 0; i < 3 && !removing; ++i)
            ui_label(x0 + w * (i + 0.5f) / 3.0f, y + 12, 11, step >= i ? UI_TEXT : UI_TEXT_FAINT, UI_ALIGN_CENTER,
                     steps[i]);
        ui_text(200, y + 32, 11, UI_TEXT_DIM, UI_ALIGN_CENTER,
                "Keep Kasumi open: this takes a few seconds.");
        return;
    }
    bar = 0.0f;
    if (sheet == SHORTCUT_SHEET_ADDED) {
        ui_text(200, y - 4, 14, UI_ACCENT, UI_ALIGN_CENTER, "Added to your HOME Menu");
        char text[200];
        if (shortcut_used_wide_art() || game->wide_url[0])
            snprintf(text, sizeof(text), i18n_text("Press HOME: it is at the end of your icons, wrapped as a present until "
                     "you open it once. It starts Kasumi and launches %.60s."), game->title);
        else
            snprintf(text, sizeof(text), i18n_text("Press HOME to find it. Tip: refresh your library (Y) and make it again "
                     "to get the game's own banner art."));
        ui_text_wrap(200, y + 14, 11, UI_TEXT_DIM, UI_ALIGN_CENTER, 360, 2, 14, text);
    } else if (sheet == SHORTCUT_SHEET_REMOVED) {
        ui_text(200, y - 4, 14, UI_TEXT, UI_ALIGN_CENTER, "Removed from your HOME Menu");
        ui_text(200, y + 14, 11, UI_TEXT_DIM, UI_ALIGN_CENTER, "Add it again any time from this page.");
    } else {
        ui_text(200, y - 4, 14, UI_DANGER, UI_ALIGN_CENTER, "Couldn't make the shortcut");
        ui_text_wrap(200, y + 14, 11, UI_TEXT_DIM, UI_ALIGN_CENTER, 360, 2, 14, app->shortcut_message);
    }
    static const char *const ok_hints[] = { "A", "OK", NULL };
    static const char *const failed_hints[] = { "A", "Try again", "B", "Close", NULL };
    draw_footer(UI_TOP_WIDTH, sheet == SHORTCUT_SHEET_FAILED ? failed_hints : ok_hints);
}

/* Lower screen: covers the game page so nothing else can be pressed. */
static void draw_shortcut_bottom(const App *app)
{
    const int sheet = app->shortcut_sheet;
    ui_rect(0, 0, UI_BOTTOM_WIDTH, UI_HEIGHT, UI_BG);
    if (sheet == SHORTCUT_SHEET_WORKING) {
        draw_working_ring(160, 70, 26);
        ui_text(160, 116, 14, UI_TEXT, UI_ALIGN_CENTER, shortcut_step_text());
        ui_text_wrap(160, 142, 11, UI_TEXT_DIM, UI_ALIGN_CENTER, 280, 2, 14,
                     "Please keep Kasumi open until it's done. Closing it now can leave a broken icon behind.");
        return;
    }
    const bool failed = sheet == SHORTCUT_SHEET_FAILED;
    ui_enso(160, 70, 26, failed ? UI_DANGER : UI_ACCENT);
    ui_text(160, 58, 22, failed ? UI_DANGER : UI_ACCENT, UI_ALIGN_CENTER, failed ? "!" : "完");
    ui_label(160, 110, 11, UI_TEXT, UI_ALIGN_CENTER,
             failed ? "NOT ADDED" : sheet == SHORTCUT_SHEET_REMOVED ? "REMOVED" : "ADDED TO THE HOME MENU");
    if (failed) {
        ui_text_wrap(160, 130, 11, UI_TEXT_DIM, UI_ALIGN_CENTER, 280, 3, 14, app->shortcut_message);
        ui_button(PAIR_LEFT, "TRY AGAIN", "再試行", UI_BUTTON_PRIMARY, pressed(app, PAIR_LEFT));
        ui_button(PAIR_RIGHT, "CLOSE", "閉じる", UI_BUTTON_NORMAL, pressed(app, PAIR_RIGHT));
    } else {
        if (sheet == SHORTCUT_SHEET_ADDED)
            ui_text_wrap(160, 130, 11, UI_TEXT_DIM, UI_ALIGN_CENTER, 280, 2, 14,
                         "Press HOME to see it. Opening it starts this game for you.");
        ui_button(SINGLE, "OK", "了解", UI_BUTTON_PRIMARY, pressed(app, SINGLE));
    }
}

static void draw_details_top(const App *app)
{
    const GfnGame *game = app_game(app, app->selected);
    if (!game) return;
    if (app->shortcut_sheet != SHORTCUT_SHEET_NONE) {
        draw_shortcut_top(app, game);
        return;
    }
    /* A hero: the cover large and soft behind everything, the cover itself
     * with a halo and its reflection, then the facts in plain sentences at
     * 15 px (the eyebrow labels at 11 px were hard to read). */
    const u32 light = service_light(app_service(app));
    const float in = ui_ease_out(ui_progress(g_top_anim.since, 420.0f));
    draw_cover_backdrop(game, 0.32f * in);
    const float cw = 108.0f, ch = GAME_ART_HEIGHT * cw / GAME_ART_WIDTH;
    const float cx = 24.0f - (1.0f - in) * 18.0f, cy = 36.0f;
    ui_rect(cx - 5, cy - 5, cw + 10, ch + 10, ui_with_alpha(light, (u8)(22.0f * in)));
    ui_rect(cx - 3, cy - 3, cw + 6, ch + 6, ui_with_alpha(light, (u8)(30.0f * in)));
    draw_cover_reflection(game, cx, cy + ch + 3, cw, 0.26f * in);
    draw_cover(game, cx, cy, cw, in);
    ui_outline(cx - 1, cy - 1, cw + 2, ch + 2, 2.0f, ui_with_alpha(light, (u8)(255.0f * in)));
    ui_gradient(0, 200, UI_TOP_WIDTH, 40, ui_with_alpha(UI_BG, 0), ui_with_alpha(UI_BG, 0xF0));

    const float x = 150.0f + (1.0f - in) * 12.0f, w = 236.0f;
    const int lines = ui_text_wrap(x, 37, 15, UI_TEXT, UI_ALIGN_LEFT, w, 2, 18, game->title);
    float y = floorf(37.0f + (float)lines * 18.0f + 6.0f);
    /* Store versions as tags, the chosen one filled. */
    float px = x;
    if (game->variant_count > 1) {
        for (unsigned i = 0; i < game->variant_count; ++i) {
            const bool on = i == app->details_variant;
            px += draw_tag(px, y, store_label(game->variants[i].store), on ? light : UI_TEXT_FAINT, on) + 6;
        }
    } else {
        px += draw_tag(px, y, app_service(app) == SERVICE_GFN ? details_store(app, game)
                                                             : SERVICE_INFO[app_service(app)].name, light, true) + 6;
    }
    if (game_prefs_favourite(game->app_id) && px < x + w - 40) draw_tag(px, y, "★", UI_KIN, true);
    y += 30;

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
    ui_text_fit(x, y, 15, UI_TEXT, UI_ALIGN_LEFT, w, line);
    y += 20;
    if (played) {
        char when[24] = "";
        if (history.last_played) {
            const time_t at = (time_t)history.last_played;
            const struct tm *t = gmtime(&at);
            if (t) strftime(when, sizeof(when), "%d %b", t);
        }
        /* One message per plural form: other languages don't add an s. */
        snprintf(line, sizeof(line), i18n_text(history.sessions == 1 ? "%lu session%s%s" : "%lu sessions%s%s"),
                 (unsigned long)history.sessions, when[0] ? i18n_text("  ·  last ") : "", when);
        ui_text_fit(x, y, 15, UI_TEXT_DIM, UI_ALIGN_LEFT, w, line);
        y += 20;
    }
    const GamePrefs prefs = game_prefs_get(game->app_id);
    ui_text_fit(x, y, 15, UI_TEXT_DIM, UI_ALIGN_LEFT, w,
                game_prefs_custom(&prefs) ? "Its own options (X to change)" : "Your usual settings");
    y += 24;
    ui_text_fit(x, y, 12, UI_TEXT_FAINT, UI_ALIGN_LEFT, w, stream_line(app));

    /* SELECT (shortcut) is a button on the lower screen: the row stays 15 px. */
    static const char *const hints[] = { "A", "Play", "Y", "Favourite", "X", "Options", "B", "Back", NULL };
    draw_footer(UI_TOP_WIDTH, hints);
}

static void draw_options_sheet(const App *app, float p)
{
    const GfnGame *game = app_game(app, app->selected);
    if (!game) return;
    const GamePrefs prefs = game_prefs_get(game->app_id);
    ui_rect(0, 0, UI_BOTTOM_WIDTH, UI_HEIGHT, ui_with_alpha(UI_BG, (u8)(0xF0 * p)));
    ui_offset(0.0f, (1.0f - p) * 10.0f);
    /* Whose options these are: the game's own name, not just "this game". */
    ui_text(16, 3, 12, UI_ACCENT, UI_ALIGN_LEFT, "設定");
    ui_label(42, 5, 11, UI_TEXT_FAINT, UI_ALIGN_LEFT, "GAME OPTIONS");
    ui_text_fit(16, 18, 12, UI_TEXT, UI_ALIGN_LEFT, 222, game->title);
    ui_button(OPT_CLOSE, "DONE", "完了", UI_BUTTON_NORMAL, pressed(app, OPT_CLOSE));
    ui_hline(16, 36, 288, UI_LINE);
    static const char *const labels[OPTION_COUNT] = {
        "Bitrate", "Camera stick speed", "Invert camera", "Touch camera", "Gyro aim", "Gyro speed", "Button layout",
        "Button mapping", "Connection" };
    static const char *const help[OPTION_COUNT] = {
        "Picture detail for this game. Default follows Settings.",
        "How fast the C-Stick turns the camera in this game.",
        "Reverse the C-Stick in this game. Gyro is not inverted.",
        "Turn the camera on the lower screen in this game.",
        "Aim by turning the console, in this game only.",
        "How fast turning the console moves the camera here.",
        "3DS A sends the pad's bottom button, or the one printed A.",
        "Move any 3DS button to any controller button. A to edit.",
        "",
    };
    /* Values sit in one column: steppers for the choices, an A chip for the
     * two rows that open something. A lit dot marks what this game changes;
     * everything else follows Settings. */
    const float value_x = 152, value_w = 152, value_cx = value_x + value_w / 2;
    char buffer[64];
    for (int i = 0; i < OPTION_COUNT; ++i) {
        const float y = OPT_ROW_Y + i * OPT_ROW_H, h = OPT_ROW_H - 3, cy = y + h / 2;
        const bool focus = i == app->options_index;
        const bool action_row = i == OPTION_MAPPING || i == OPTION_CONNECTION;
        if (focus) {
            ui_rect(12, y, 296, h, UI_RAISED);
            ui_rect(12, y, 2, h, UI_ACCENT);
        }
        const bool custom = option_custom(&prefs, i);
        if (custom) ui_circle(22, cy, 2.5f, UI_ACCENT);
        ui_text_fit(30, y + 3, 12, focus ? UI_TEXT : UI_TEXT_DIM, UI_ALIGN_LEFT, value_x - 34, labels[i]);
        const char *value = option_value(app, &prefs, i, buffer, sizeof(buffer));
        const u32 value_color = custom ? UI_ACCENT : focus ? UI_TEXT : UI_TEXT_FAINT;
        if (action_row) {
            ui_text_fit(value_x + value_w - 22, y + 3, 12, value_color, UI_ALIGN_RIGHT, value_w - 26, value);
            if (focus) ui_button_chip(value_x + value_w - 17, cy - 7.5f, "A", UI_ACCENT);
        } else {
            ui_text_fit(value_cx, y + 3, 12, value_color, UI_ALIGN_CENTER, value_w - 28, value);
            if (focus) {
                ui_triangle(value_x + 8, cy - 4.5f, value_x + 8, cy + 4.5f, value_x + 2, cy, UI_ACCENT);
                ui_triangle(value_x + value_w - 8, cy - 4.5f, value_x + value_w - 8, cy + 4.5f,
                            value_x + value_w - 2, cy, UI_ACCENT);
            }
        }
    }
    const int index = app->options_index >= 0 && app->options_index < OPTION_COUNT ? app->options_index : 0;
    ui_hline(16, 216, 288, UI_LINE);
    ui_text_wrap(160, 219, 11, UI_TEXT_DIM, UI_ALIGN_CENTER, 296, 2, 11,
                 index == OPTION_CONNECTION ? connection_advice(app->client) : help[index]);
    ui_offset(0.0f, 0.0f);
}

static void draw_face_slot(int slot, bool xbox, float cx, float cy, float size);

/* An output's name, with the symbol for face buttons. */
static float draw_output(float cx, float y, float size, unsigned output, u32 color)
{
    const bool xbox = gfn_input_xbox_names();
    const bool face = output >= GFN_OUT_CROSS && output <= GFN_OUT_TRIANGLE;
    char name[32];
    snprintf(name, sizeof(name), xbox && face ? "%s button" : "%s", gfn_output_name(output));
    const float w = ui_text_width(name, size);
    float x = cx - (w + (face ? size + 6 : 0)) / 2;
    if (face) {
        /* Cross, Circle, Square, Triangle sit bottom, right, left, top. */
        static const int slots[4] = { 2, 1, 3, 0 };
        const float s = size * 0.8f, sy = y + size * 0.55f;
        draw_face_slot(slots[output - GFN_OUT_CROSS], xbox, x + s / 2, sy, s);
        x += size + 6;
    }
    ui_text(x, y, size, color, UI_ALIGN_LEFT, name);
    return w;
}

static void draw_mapping_top(const App *app)
{
    const GfnGame *game = app_game(app, app->selected);
    draw_title(200, 31, "ボタン設定", "BUTTON MAPPING");
    if (app->mapping_global)
        ui_text_fit(200, 60, 12, UI_TEXT_DIM, UI_ALIGN_CENTER, 340, "Every game (a game's own mapping goes first)");
    else if (game)
        ui_text_fit(200, 60, 12, UI_TEXT_DIM, UI_ALIGN_CENTER, 340, game->title);
    /* Two columns of seven: 3DS button -> what it sends. */
    for (int i = 0; i < GFN_INPUT_COUNT; ++i) {
        const float x = i < 7 ? 22 : 206, y = 80 + (i % 7) * 19.0f;
        const bool on = i == app->mapping_input;
        const bool changed = binding_changed(app, i);
        if (on) {
            ui_rect(x - 6, y - 2, 178, 18, UI_RAISED);
            ui_rect(x - 6, y - 2, 2, 18, UI_ACCENT);
        }
        ui_button_chip(x, y, gfn_input_name((unsigned)i), on ? UI_TEXT : UI_TEXT_DIM);
        char text[48];
        binding_text(text, sizeof(text), &app->mapping, i);
        const unsigned mode = app->mapping.mode[i];
        ui_text_fit(x + 56, y, 12, changed ? UI_ACCENT : on ? UI_TEXT : UI_TEXT_DIM, UI_ALIGN_LEFT,
                    mode ? 70 : 112, text);
        if (mode)
            ui_label(x + 170, y + 2, 9, UI_ACCENT, UI_ALIGN_RIGHT, mode == GFN_BIND_TURBO ? "TURBO" : "TOGGLE");
    }
    static const char *const hints[] = { "ANY BUTTON", "Pick", "CIRCLE PAD", "Change", "C-STICK", "Row", NULL };
    draw_footer(UI_TOP_WIDTH, hints);
}

/* The mapping card's rows: what the button sends, a second output sent
 * with it, and how it is held. */
static UiRect map_row(int field) { return (UiRect){ 16, 40.0f + field * 47.0f, 288, 42 }; }
static UiRect map_prev(int field) { const UiRect r = map_row(field); return (UiRect){ r.x, r.y, 44, r.h }; }
static UiRect map_next(int field) { const UiRect r = map_row(field); return (UiRect){ r.x + r.w - 44, r.y, 44, r.h }; }
static int g_touched_map_field = -1;
int screens_touched_map_field(void) { return g_touched_map_field; }

static void draw_mapping_bottom(const App *app)
{
    ui_rect(0, 0, UI_BOTTOM_WIDTH, UI_HEIGHT, UI_BG);
    const unsigned input = (unsigned)app->mapping_input;
    const char *name = gfn_input_name(input);
    const bool round = strlen(name) == 1;
    const float chip_w = round ? 15.0f : ui_text_width(name, 10) + 10;
    ui_label(160, 4, 11, UI_TEXT_FAINT, UI_ALIGN_CENTER, "PRESS ANY 3DS BUTTON TO PICK IT");
    ui_button_chip(160 - chip_w / 2, 19, name, UI_TEXT);

    static const char *const captions[3] = { "SENDS", "AND AT THE SAME TIME", "HOW" };
    const GfnButtonMap *m = &app->mapping, *d = &app->mapping_default;
    for (int f = 0; f < 3; ++f) {
        const UiRect r = map_row(f);
        const bool focus = f == app->mapping_field;
        ui_rect_r(r, focus ? UI_RAISED : UI_SURFACE);
        ui_outline(r.x, r.y, r.w, r.h, 1.0f, focus ? UI_ACCENT : UI_LINE);
        draw_arrow(map_prev(f), -1, true, pressed(app, map_prev(f)));
        draw_arrow(map_next(f), 1, true, pressed(app, map_next(f)));
        ui_label(160, r.y + 4, 10, focus ? UI_ACCENT : UI_TEXT_FAINT, UI_ALIGN_CENTER, captions[f]);
        const float vy = r.y + 19;
        if (f == 0) {
            draw_output(160, vy, 14, m->out[input], m->out[input] != d->out[input] ? UI_ACCENT : UI_TEXT);
        } else if (f == 1) {
            if (m->also[input] == GFN_OUT_NONE) ui_text(160, vy, 14, UI_TEXT_FAINT, UI_ALIGN_CENTER, "Nothing more");
            else draw_output(160, vy, 14, m->also[input], UI_ACCENT);
        } else {
            static const char *const modes[GFN_BIND_MODE_COUNT] = {
                "Normal", "Turbo: 10 presses a second", "Toggle: press once to hold"
            };
            ui_text(160, vy, 14, m->mode[input] ? UI_ACCENT : UI_TEXT, UI_ALIGN_CENTER,
                    modes[m->mode[input] < GFN_BIND_MODE_COUNT ? m->mode[input] : 0]);
        }
    }
    ui_text(160, 180, 11, UI_TEXT_DIM, UI_ALIGN_CENTER, "Circle Pad: button and value  ·  C-Stick: row");
    ui_button(MAP_RESET, "RESET", "初期化", UI_BUTTON_NORMAL, pressed(app, MAP_RESET));
    ui_button(MAP_CANCEL, "CANCEL", "取消", UI_BUTTON_NORMAL, pressed(app, MAP_CANCEL));
    ui_button(MAP_DONE, "SAVE", "保存", UI_BUTTON_PRIMARY, pressed(app, MAP_DONE));
}

static void draw_details_bottom(const App *app, float overlay_p)
{
    const GfnGame *game = app_game(app, app->selected);
    if (!game) return;
    draw_status_strip(app, app->status);
    ui_button(DET_PLAY, "PLAY", "遊ぶ", UI_BUTTON_PRIMARY, pressed(app, DET_PLAY));
    const bool gfn = app_service(app) == SERVICE_GFN;
    const bool choice = gfn && game->variant_count > 1;
    draw_arrow(DET_STORE_PREV, -1, choice, pressed(app, DET_STORE_PREV));
    draw_arrow(DET_STORE_NEXT, 1, choice, pressed(app, DET_STORE_NEXT));
    /* Only GeForce NOW has store versions; the others say where it runs. */
    const char *where = gfn ? details_store(app, game)
                      : app_service(app) == SERVICE_STEAM
                      ? (steam_link_host_name()[0] ? steam_link_host_name() : "Your PC") : "Xbox Cloud Gaming";
    ui_label(160, 110, 11, UI_TEXT_FAINT, UI_ALIGN_CENTER,
             choice ? "LAUNCH FROM  (LEFT / RIGHT)" : gfn ? "LAUNCH FROM" : "PLAYS ON");
    ui_text_fit(160, 126, 15, UI_TEXT, UI_ALIGN_CENTER, 180, where);
    if (choice) ui_dots(160, 152, game->variant_count, app->details_variant, UI_ACCENT, UI_LINE_STRONG);
    const bool favourite = game_prefs_favourite(game->app_id);
    ui_button(DET_FAV, favourite ? "SAVED" : "FAVOURITE", "お気に入り",
              favourite ? UI_BUTTON_ACTIVE : UI_BUTTON_NORMAL, pressed(app, DET_FAV));
    ui_button(DET_OPTIONS, "OPTIONS", "設定", UI_BUTTON_NORMAL, pressed(app, DET_OPTIONS));
    /* HOME Menu shortcut: add, or (once there) remove. */
    const ShortcutState shortcut = shortcut_state();
    const bool on_home = shortcut_exists(game->app_id);
    ui_button(DET_SHORTCUT,
              shortcut == SHORTCUT_IDLE ? (on_home ? "ON THE HOME MENU  ·  REMOVE" : "+ ADD TO HOME MENU")
              : shortcut == SHORTCUT_WORKING ? "INSTALLING THE SHORTCUT..." : "DRAWING THE SHORTCUT...",
              NULL, on_home && shortcut == SHORTCUT_IDLE ? UI_BUTTON_ACTIVE : UI_BUTTON_NORMAL,
              pressed(app, DET_SHORTCUT));
    ui_button(DET_BACK, "BACK", "戻る", UI_BUTTON_NORMAL, pressed(app, DET_BACK));
    if (app->shortcut_sheet != SHORTCUT_SHEET_NONE) {
        draw_shortcut_bottom(app);
    } else if (app->mapping_open) {
        draw_mapping_bottom(app);
    } else if (app->options_open) {
        draw_options_sheet(app, overlay_p);
    }
}

/* ---- Software update --------------------------------------------------------- */

/* Word-wrapped, scrollable notes inside a panel; lines fade at the edges. */
static void draw_notes(UiRect box, const char *text, int scroll)
{
    ui_rect_r(box, UI_SURFACE);
    ui_outline(box.x, box.y, box.w, box.h, 1.0f, UI_LINE);
    const float line_h = 15.0f, pad = 8.0f, width = box.w - pad * 2;
    const int visible = (int)((box.h - pad * 2) / line_h);
    /* First pass counts lines so the scroll can be clamped. */
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
                /* Grow the line word by word. */
                size_t best = 0, cursor = start;
                while (cursor < para) {
                    size_t next = cursor;
                    while (next < para && p[next] == ' ') ++next;
                    while (next < para && p[next] != ' ') ++next;
                    const size_t len = next - start < sizeof(line) - 1 ? next - start : sizeof(line) - 1;
                    memcpy(line, p + start, len);
                    line[len] = '\0';
                    if (ui_text_width(line, 12) > width && best) break;
                    best = next - start;
                    cursor = next;
                }
                if (!best) best = para - start;
                if (pass == 1 && index >= first && index < first + visible) {
                    const size_t len = best < sizeof(line) - 1 ? best : sizeof(line) - 1;
                    memcpy(line, p + start, len);
                    line[len] = '\0';
                    const bool bullet = line[0] == '-' && line[1] == ' ';
                    const float y = box.y + pad + (index - first) * line_h;
                    if (bullet) {
                        ui_circle(box.x + pad + 3, y + 7, 2, UI_ACCENT);
                        ui_text(box.x + pad + 10, y, 12, UI_TEXT, UI_ALIGN_LEFT, line + 2);
                    } else {
                        ui_text(box.x + pad, y, 12, UI_TEXT_DIM, UI_ALIGN_LEFT, line);
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
            /* Scroll hint on the right edge. */
            const float rail = box.h - 8, thumb = rail * visible / total;
            ui_vline(box.x + box.w - 5, box.y + 4, rail, UI_LINE);
            ui_rect(box.x + box.w - 6, box.y + 4 + (rail - thumb) * first / (float)(total - visible), 3, thumb,
                    UI_ACCENT);
        }
    }
}

static void draw_update_top(const App *app)
{
    const UpdateInfo info = updater_info();
    draw_title(200, 31, "更新", "SOFTWARE UPDATE");
    const bool has_new = info.latest[0] && info.state != UPDATE_UP_TO_DATE && info.state != UPDATE_IDLE &&
                         info.state != UPDATE_CHECKING;
    if (has_new) {
        /* Installed -> available, side by side. */
        const UiRect left = { 40, 60, 140, 40 }, right = { 220, 60, 140, 40 };
        ui_rect_r(left, UI_SURFACE);
        ui_outline(left.x, left.y, left.w, left.h, 1.0f, UI_LINE);
        ui_label(left.x + left.w / 2, left.y + 5, 11, UI_TEXT_FAINT, UI_ALIGN_CENTER, "INSTALLED");
        ui_text_fit(left.x + left.w / 2, left.y + 20, 13, UI_TEXT_DIM, UI_ALIGN_CENTER, left.w - 8, APP_VERSION);
        ui_rect_r(right, UI_SURFACE);
        ui_outline(right.x, right.y, right.w, right.h, 1.0f, UI_ACCENT);
        ui_label(right.x + right.w / 2, right.y + 5, 11, UI_ACCENT, UI_ALIGN_CENTER,
                 info.prerelease ? "NEW BETA" : "NEW");
        ui_text_fit(right.x + right.w / 2, right.y + 20, 13, UI_TEXT, UI_ALIGN_CENTER, right.w - 8, info.latest);
        ui_triangle(194, 74, 194, 86, 204, 80, UI_ACCENT);
        char heading[64];
        snprintf(heading, sizeof(heading), i18n_text("WHAT'S NEW  ·  %s"), info.published[0] ? info.published : info.latest);
        ui_label(40, 106, 11, UI_TEXT_FAINT, UI_ALIGN_LEFT, heading);
        const UiRect notes = { 36, 120, 328, 94 };
        draw_notes(notes, info.notes, app->notes_scroll);
    } else if (info.state == UPDATE_CHECKING || info.state == UPDATE_IDLE) {
        ui_enso(200, 118, 24, UI_ACCENT);
        ui_text(200, 156, 13, UI_TEXT, UI_ALIGN_CENTER, "Checking GitHub for a newer Kasumi...");
    } else {
        const bool ok = info.state == UPDATE_UP_TO_DATE;
        ui_ring(200, 110, 26, 2.0f, ok ? UI_ACCENT : UI_DANGER, UI_BG);
        if (ok) {
            ui_line(188, 110, 197, 119, 3.0f, UI_ACCENT);
            ui_line(197, 119, 213, 101, 3.0f, UI_ACCENT);
        } else {
            ui_text(200, 94, 26, UI_DANGER, UI_ALIGN_CENTER, "!");
        }
        ui_text(200, 146, 14, UI_TEXT, UI_ALIGN_CENTER, ok ? "Kasumi is up to date" : "Could not check for updates");
        char detail[160];
        if (ok && info.checked_at) {
            const long age = (long)((int64_t)time(NULL) - info.checked_at);
            if (age < 120) snprintf(detail, sizeof(detail), i18n_text("Version %s  ·  checked just now"), APP_VERSION);
            else snprintf(detail, sizeof(detail), i18n_text("Version %s  ·  checked %ld min ago"), APP_VERSION, age / 60);
        } else {
            snprintf(detail, sizeof(detail), "%s", ok ? APP_VERSION : info.error);
        }
        ui_text_wrap(200, 166, 11, UI_TEXT_DIM, UI_ALIGN_CENTER, 330, 2, 14, detail);
    }
    static const char *const hints_new[] = { "A", "Install", "X", "Later", "B", "Close", NULL };
    static const char *const hints[] = { "A", "Check again", "B", "Close", NULL };
    static const char *const hints_busy[] = { NULL };
    const bool working = info.state == UPDATE_DOWNLOADING || info.state == UPDATE_VERIFYING ||
                         info.state == UPDATE_INSTALLING || info.state == UPDATE_CHECKING;
    draw_footer(UI_TOP_WIDTH, working ? hints_busy : info.state == UPDATE_AVAILABLE ? hints_new : hints);
}

static void draw_update_bottom(const App *app)
{
    static float bar;
    const UpdateInfo info = updater_info();
    ui_label(160, 8, 11, UI_TEXT_FAINT, UI_ALIGN_CENTER, "SOFTWARE UPDATE");
    ui_text(160, 26, 14, info.state == UPDATE_FAILED ? UI_DANGER : UI_TEXT, UI_ALIGN_CENTER,
            update_phase_title(&info));

    /* Three steps with one continuous bar underneath. */
    static const char *const steps[] = { "DOWNLOAD", "VERIFY", "INSTALL" };
    int step = -1;
    if (info.state == UPDATE_DOWNLOADING) step = 0;
    else if (info.state == UPDATE_VERIFYING) step = 1;
    else if (info.state == UPDATE_INSTALLING) step = 2;
    else if (info.state == UPDATE_INSTALLED) step = 3;
    const float overall = step < 0 ? 0.0f : step >= 3 ? 1.0f : (step + info.progress / 1000.0f) / 3.0f;
    bar = ui_approach(bar, overall, 10.0f);
    const float bx = 30, bw = 260, by = 76;
    ui_rect(bx, by, bw, 4, UI_RAISED);
    if (bar > 0.005f) ui_rect(bx, by, bw * bar, 4, info.state == UPDATE_FAILED ? UI_DANGER : UI_ACCENT);
    for (int i = 0; i < 3; ++i) {
        const float x = bx + bw * (i + 0.5f) / 3.0f;
        const bool done = step > i, now = step == i;
        ui_label(x, by + 14, 11, done || now ? UI_TEXT : UI_TEXT_FAINT, UI_ALIGN_CENTER, steps[i]);
    }
    char detail[160] = "";
    if (info.state == UPDATE_DOWNLOADING && info.size_bytes)
        snprintf(detail, sizeof(detail), i18n_text("%.1f of %.1f MB"), info.progress / 1000.0f * info.size_bytes / 1048576.0f,
                 info.size_bytes / 1048576.0f);
    else if (info.state == UPDATE_AVAILABLE && info.size_bytes)
        snprintf(detail, sizeof(detail), "%s  ·  %.1f MB  ·  %s", info.latest, info.size_bytes / 1048576.0f,
                 updater_package_label());
    else if (info.state == UPDATE_INSTALLED)
        snprintf(detail, sizeof(detail), updater_can_relaunch() ? i18n_text("Restart to use %s.") : i18n_text("Close Kasumi and open it again to use %s."),
                 info.latest);
    else if (info.state == UPDATE_FAILED)
        snprintf(detail, sizeof(detail), "%s", info.error);
    else if (info.state == UPDATE_INSTALLING)
        snprintf(detail, sizeof(detail), i18n_text("Your login, library and settings stay as they are."));
    ui_text_wrap(160, 112, 11, UI_TEXT_DIM, UI_ALIGN_CENTER, 290, 2, 14, detail);

    const bool working = info.state == UPDATE_DOWNLOADING || info.state == UPDATE_VERIFYING ||
                         info.state == UPDATE_INSTALLING || info.state == UPDATE_CHECKING;
    char label[48];
    const char *jp = "確認";
    if (info.state == UPDATE_AVAILABLE) { snprintf(label, sizeof(label), i18n_text("INSTALL %s"), info.latest); jp = "インストール"; }
    else if (info.state == UPDATE_INSTALLED) {
        snprintf(label, sizeof(label), "%s", updater_can_relaunch() ? i18n_text("RESTART KASUMI") : i18n_text("EXIT KASUMI"));
        jp = "再起動";
    } else if (info.state == UPDATE_FAILED) { snprintf(label, sizeof(label), i18n_text("TRY AGAIN")); jp = "再試行"; }
    else if (working) { snprintf(label, sizeof(label), i18n_text("PLEASE WAIT")); jp = "処理中"; }
    else snprintf(label, sizeof(label), i18n_text("CHECK AGAIN"));
    ui_button(UPD_PRIMARY, label, jp, working ? UI_BUTTON_NORMAL : UI_BUTTON_PRIMARY, pressed(app, UPD_PRIMARY));
    if (!working) {
        if (info.state == UPDATE_AVAILABLE)
            ui_button(UPD_LATER, "LATER", "後で", UI_BUTTON_NORMAL, pressed(app, UPD_LATER));
        ui_button(info.state == UPDATE_AVAILABLE ? UPD_CLOSE : (UiRect){ 90, 200, 140, 34 }, "CLOSE", "閉じる",
                  UI_BUTTON_NORMAL, pressed(app, UPD_CLOSE));
    }
}

static void draw_whats_new_top(const App *app)
{
    draw_title(200, 31, "新機能", "WHAT'S NEW");
    char heading[64];
    snprintf(heading, sizeof(heading), i18n_text("Kasumi %s"), app->whats_new_version);
    ui_text(200, 62, 15, UI_TEXT, UI_ALIGN_CENTER, heading);
    const UiRect notes = { 30, 86, 340, 126 };
    draw_notes(notes, app->whats_new_notes, app->notes_scroll);
    static const char *const hints[] = { "A", "Continue", NULL };
    draw_footer(UI_TOP_WIDTH, hints);
}

static void draw_whats_new_bottom(const App *app)
{
    const UiRect card = { 30, 40, 260, 120 };
    ui_panel(card, UI_ACCENT);
    ui_seal(144, card.y + 16, 32);
    ui_text(160, card.y + 58, 14, UI_TEXT, UI_ALIGN_CENTER, "Kasumi was updated");
    ui_text(160, card.y + 80, 12, UI_ACCENT, UI_ALIGN_CENTER, app->whats_new_version);
    ui_button(NEW_CONTINUE, "CONTINUE", "続ける", UI_BUTTON_PRIMARY, pressed(app, NEW_CONTINUE));
}

/* ---- Discord invite (once) ----------------------------------------------------- */

static void draw_discord_top(void)
{
    draw_title(200, 31, "仲間", "JOIN THE KASUMI DISCORD");
    ui_text_wrap(200, 70, 13, UI_TEXT, UI_ALIGN_CENTER, 330, 4, 18,
                 "New versions first, help from other players, and a say in what Kasumi gets next.");
    ui_text(200, 150, 14, UI_ACCENT, UI_ALIGN_CENTER, "discord.gg/K9Jy3t7YHE");
    ui_text(200, 172, 11, UI_TEXT_DIM, UI_ALIGN_CENTER, "Also in Settings > System > Kasumi Discord");
    static const char *const hints[] = { "A", "Continue", NULL };
    draw_footer(UI_TOP_WIDTH, hints);
}

static void draw_discord_bottom(const App *app)
{
    ui_label(160, 10, 11, UI_TEXT_FAINT, UI_ALIGN_CENTER, "SCAN WITH YOUR PHONE");
    /* The QR art carries its own white margin, so it scans on every theme. */
    ui_image(UI_IMAGE_DISCORD, 107, 40, 1.0f, 1.0f);
    ui_button(NEW_CONTINUE, "CONTINUE", "続ける", UI_BUTTON_PRIMARY, pressed(app, NEW_CONTINUE));
}

/* ---- First-run guide ------------------------------------------------------- */

static void draw_guide_top(const App *app)
{
    const int page = app->guide_page < 0 ? 0 : app->guide_page;
    const GuidePage *g = &GUIDE[page];
    ui_enso(200, 78, 30, UI_ACCENT);
    ui_text(200, 63, 30, UI_TEXT, UI_ALIGN_CENTER, g->kanji);
    draw_title(200, 118, g->jp, g->en);
    ui_text_wrap(200, 146, 12, UI_TEXT, UI_ALIGN_CENTER, 340, 4, 15, g->body);
    ui_dots(200, 212, GUIDE_PAGES, (unsigned)page, UI_ACCENT, UI_LINE_STRONG);
    static const char *const hints[] = { "A", "Next", "B", "Back", "START", "Skip", NULL };
    ui_hint_row(200, 223, hints);
}

static void draw_guide_bottom(const App *app)
{
    const int page = app->guide_page < 0 ? 0 : app->guide_page;
    ui_label(160, 8, 11, UI_TEXT_FAINT, UI_ALIGN_CENTER, "GETTING STARTED");
    ui_textf(160, 24, 12, UI_ACCENT, UI_ALIGN_CENTER, "%d / %d", page + 1, GUIDE_PAGES);
    const UiRect card = { 16, 50, 288, 110 };
    ui_panel(card, UI_ACCENT);
    ui_label(160, card.y + 14, 11, UI_TEXT_FAINT, UI_ALIGN_CENTER, "TIP");
    ui_text_wrap(160, card.y + 34, 13, UI_TEXT, UI_ALIGN_CENTER, card.w - 28, 4, 17, GUIDE[page].tip);
    ui_button(GUIDE_BACK, "BACK", "戻る", UI_BUTTON_NORMAL, pressed(app, GUIDE_BACK));
    ui_button(GUIDE_SKIP, "SKIP", "省略", UI_BUTTON_NORMAL, pressed(app, GUIDE_SKIP));
    const bool last = page == GUIDE_PAGES - 1;
    ui_button(GUIDE_NEXT, last ? "START" : "NEXT", last ? "開始" : "次へ", UI_BUTTON_PRIMARY,
              pressed(app, GUIDE_NEXT));
}

void screens_draw_top(const App *app)
{
    static int previous_view = -1;
    push_service_accent(app);
    const float p = view_progress(&g_top_anim, (int)app->view, (int)app->modal);
    const bool entering = p < 1.0f;
    /* Signed in or paired just now: celebrate it. */
    if (previous_view == VIEW_LOGIN && app->view == VIEW_LIBRARY && gfn_has_session(app->client) &&
        (status_from(app->client->status, "Signed in; session %s") ||
         status_from(app->client->status, "Paired with %.60s")))
        celebrate(app);
    previous_view = (int)app->view;
    /* Behind everything: the hub's art, or the service's. */
    const bool guide_up = app->guide_page >= 0 && app->view != VIEW_STREAM;
    if (guide_up) draw_mist(150, 0.35f);
    else if (app->view == VIEW_HUB) draw_hub_backdrop(app);
    else if (app->view == VIEW_LIBRARY) {
        draw_service_backdrop(app_service(app), gfn_has_session(app->client) ? 0.32f : 0.5f);
        if (gfn_has_session(app->client) && !app->service_loading) draw_focus_backdrop(app);
    }
    else if (app->view == VIEW_DETAILS) draw_service_backdrop(app_service(app), 0.18f);
    else if (app->view == VIEW_SESSION) draw_service_backdrop(app_service(app), 0.3f);
    else if (app->view == VIEW_LOGIN) draw_service_backdrop(login_service(app->client), 0.3f);
    if (!guide_up && (app->view == VIEW_HUB || app->view == VIEW_LIBRARY || app->view == VIEW_LOGIN))
        ui_gradient(0, 0, UI_TOP_WIDTH, 26, ui_with_alpha(UI_BG, 0xA0), ui_with_alpha(UI_BG, 0x60));
    draw_status_bar(app, UI_TOP_WIDTH);
    /* The view's content rises 8 px as it fades in (not under a zoom: the
     * card itself is the transition). */
    const bool handover = zoom_handover();
    ui_offset(0.0f, handover ? 0.0f : (1.0f - p) * 8.0f);
    const bool menus = app->view != VIEW_STREAM;
    const bool guide = app->guide_page >= 0 && menus;
    if (app->whats_new_open && menus) draw_whats_new_top(app);
    else if (app->discord_open && menus) draw_discord_top();
    else if (guide) draw_guide_top(app);
    else if (app->update_open && menus) draw_update_top(app);
    else switch (app->view) {
    case VIEW_HUB: draw_hub_top(app); break;
    case VIEW_LOGIN: draw_login_top(app); break;
    case VIEW_LIBRARY:
        if (gfn_has_session(app->client)) draw_library_top(app, entering);
        else draw_signin_top(app);
        break;
    case VIEW_SETTINGS:
        if (app->mapping_open) draw_mapping_top(app);
        else draw_settings_top(app, entering);
        break;
    case VIEW_SESSION: draw_session_top(app); break;
    case VIEW_DETAILS:
        if (app->mapping_open) draw_mapping_top(app);
        else draw_details_top(app);
        break;
    case VIEW_STREAM: break;
    }
    ui_offset(0.0f, 0.0f);
    if (!handover) fade_in_veil(UI_TOP_WIDTH, 26.0f, p);
    draw_celebration();
    if (app->modal != MODAL_NONE) draw_modal_top(app, overlay_progress(&g_top_anim));
    if (app->busy) draw_busy_top(app);
    ui_pop_accent();
}

/* ---- Bottom screens ------------------------------------------------------ */

static void draw_hub_bottom(const App *app)
{
    static int shown = -1;
    static u64 changed_at;
    static int dir;
    const int s = app->hub_index;
    if (shown != s) {
        dir = shown < 0 ? 0 : s > shown ? 1 : -1;
        shown = s;
        changed_at = plat_time_ms();
    }
    const u32 light = service_light(s);
    const float z = zoom_peek();
    const float since = g_hub_shown_at ? (float)(plat_time_ms() - g_hub_shown_at) : 0.0f;
    const float delay = app->settings.hub_done ? 40.0f : 1000.0f;
    ui_offset(0.0f, z * z * 70.0f);
    draw_status_strip(app, app->settings.hub_done ? "Kasumi game hub" : "Welcome! Where are your games?");

    /* The focused service: what it is and how it stands; tap to enter. */
    const UiRect p = HOME_PANEL;
    const float panel_in = ui_ease_out(clamp01((since - delay) / 420.0f));
    const float py = (1.0f - panel_in) * 40.0f;
    const UiRect pr = { p.x, p.y + py, p.w, p.h };
    ui_surface(pr, light, UI_SURFACE);
    ui_gradient_corners(pr.x + 1, pr.y + 1, pr.w - 2, pr.h - 2, ui_with_alpha(light, 0x50),
                        ui_with_alpha(light, 0x00), ui_with_alpha(light, 0x28), ui_with_alpha(light, 0x00));
    if (pressed(app, p)) ui_rect_r(pr, ui_with_alpha(UI_TEXT, 0x1C));
    const float e = ui_ease_out(ui_progress(changed_at, 280.0f));
    const float ox = (1.0f - e) * 28.0f * (float)dir;
    const u8 a = (u8)(255.0f * e * panel_in);
    const GfnGame *last = hub_last_game(app);
    if (last) {
        /* The service in use has a game to go back to: the whole panel is
         * that game, cover large, START (or a tap) to continue. Its service
         * is on the card above; the tiles below still enter it. */
        const float cw = 54.0f, ch = GAME_ART_HEIGHT * cw / GAME_ART_WIDTH;
        const float gx = floorf(pr.x + 12 + ox), gy = floorf(pr.y + (pr.h - ch) / 2);
        ui_rect(gx - 3, gy - 3, cw + 6, ch + 6, ui_with_alpha(light, (u8)(40.0f * e)));
        draw_cover(last, gx, gy, cw, e * panel_in);
        ui_outline(gx - 1, gy - 1, cw + 2, ch + 2, 1.0f, ui_with_alpha(light, a));
        const float x = floorf(gx + cw + 14), w = pr.x + pr.w - 12 - x;
        /* "Continue" and a START pill, both at 15 px. */
        ui_text(x, pr.y + 12, 15, ui_with_alpha(light, a), UI_ALIGN_LEFT, "Continue");
        const float pill_w = floorf(ui_text_width("START", 15) + 12.0f);
        const float pill_x = floorf(pr.x + pr.w - 12 - pill_w);
        ui_rounded(pill_x, pr.y + 11, pill_w, 19, 9.5f, ui_with_alpha(light, a));
        ui_text(pill_x + pill_w / 2, pr.y + 12, 15, ui_with_alpha(UI_BG, a), UI_ALIGN_CENTER, "START");
        const int lines = ui_text_wrap(x, pr.y + 36, 15, ui_with_alpha(UI_TEXT, a), UI_ALIGN_LEFT, w, 2, 18,
                                       last->title);
        PlayHistory history = {0};
        if (play_history_get(last->app_id, &history) && history.seconds) {
            char played[48], line[80];
            format_played(played, sizeof(played), history.seconds);
            snprintf(line, sizeof(line), "%s on Kasumi", played);
            ui_text_fit(x, pr.y + 40 + (float)lines * 18.0f, 12, ui_with_alpha(UI_TEXT_DIM, a), UI_ALIGN_LEFT, w,
                        line);
        }
    } else {
        ui_circle(pr.x + 30 + ox, pr.y + 30, 21, ui_with_alpha(light, (u8)(0x55 * e)));
        ui_icon(SERVICE_ICON[s], pr.x + 30 + ox, pr.y + 30, 26, ui_with_alpha(UI_TEXT, a));
        ui_text(floorf(pr.x + 60 + ox), pr.y + 12, 15, ui_with_alpha(UI_TEXT, a), UI_ALIGN_LEFT,
                SERVICE_INFO[s].name);
        const bool ready = app->service_ready[s];
        ui_circle(pr.x + 64 + ox, pr.y + 40, 2.5f, ui_with_alpha(ready ? light : UI_TEXT_FAINT, a));
        ui_text_fit(pr.x + 71 + ox, pr.y + 33, 11, ui_with_alpha(ready ? UI_TEXT : UI_TEXT_DIM, a), UI_ALIGN_LEFT,
                    150, app->service_status[s]);
        const bool paused = s == SERVICE_XBOX && app->xbox_paused;
        ui_text_wrap(pr.x + 12 + ox, pr.y + 58, 11, ui_with_alpha(UI_TEXT_DIM, a), UI_ALIGN_LEFT, pr.w - 24, 2, 13,
                     paused ? "Most Xbox games stream at a size the 3DS can't show yet. Back when they do."
                            : SERVICE_INFO[s].about);
        /* "A Enter" at 15 px, the size the font stays sharp at. */
        const char *verb = paused ? NULL : ready ? "Enter" : "Set up";
        if (verb) {
            const float verb_w = ui_text_width(verb, 15);
            const float vx = floorf(pr.x + pr.w - 12 - verb_w);
            ui_text(vx, pr.y + 12, 15, ui_with_alpha(light, (u8)(255.0f * panel_in)), UI_ALIGN_LEFT, verb);
            ui_circle(vx - 12, pr.y + 20, 8.5f, ui_with_alpha(light, (u8)(255.0f * panel_in)));
            ui_text(vx - 12, pr.y + 12, 15, UI_BG, UI_ALIGN_CENTER, "A");
        }
    }

    /* Every service as a tile: tap one to go straight in. */
    for (int i = 0; i < SERVICE_COUNT; ++i) {
        const float tin = ui_ease_out(clamp01((since - delay - 90.0f - 80.0f * (float)i) / 420.0f));
        const UiRect base = home_tile(i);
        const UiRect r = { base.x, base.y + (1.0f - tin) * 50.0f, base.w, base.h };
        const bool on = i == s;
        const u32 li = service_light(i);
        ui_surface(r, on ? li : UI_LINE, UI_SURFACE);
        if (on) ui_gradient(r.x + 1, r.y + 1, r.w - 2, r.h - 2, ui_with_alpha(li, 0x58), ui_with_alpha(li, 0x10));
        if (pressed(app, base)) ui_rect_r(r, ui_with_alpha(UI_TEXT, 0x24));
        ui_icon(SERVICE_ICON[i], r.x + r.w / 2, r.y + 19, 22, on ? UI_TEXT : UI_TEXT_FAINT);
        static const char *const short_names[SERVICE_COUNT] = { "GeForce NOW", "Xbox Cloud", "Steam Link" };
        ui_text_fit(r.x + r.w / 2, r.y + 33, 11, on ? UI_TEXT : UI_TEXT_DIM, UI_ALIGN_CENTER, r.w - 6,
                    short_names[i]);
        if (app->service_ready[i]) ui_circle(r.x + r.w - 8, r.y + 8, 2.5f, li);
        if (on) ui_rect(r.x + 18, r.y + r.h - 3, r.w - 36, 2, li);
    }
    ui_button(HUB_SETTINGS, "SETTINGS", "設定", UI_BUTTON_NORMAL, pressed(app, HUB_SETTINGS));
    ui_button(HUB_EXIT, "EXIT", "終了", UI_BUTTON_NORMAL, pressed(app, HUB_EXIT));
    ui_offset(0.0f, 0.0f);
    if (z > 0.0f) ui_rect(0, 0, UI_BOTTOM_WIDTH, UI_HEIGHT, ui_with_alpha(UI_BG, (u8)(z * 210.0f)));
}

static void draw_signin_bottom(const App *app)
{
    const int s = app_service(app);
    draw_service_header(app);
    char date[16];
    if (s != SERVICE_STEAM && http_clock_wrong(date, sizeof(date))) {
        /* Sign-in would fail with no code shown: say why first. */
        char text[160];
        snprintf(text, sizeof(text), i18n_text("Your 3DS clock says %s. Set the date and time in System Settings "
                 "first, or the sign-in fails."), date);
        ui_text_wrap(160, 40, 11, UI_KIN, UI_ALIGN_CENTER, 292, 3, 14, text);
    } else if (s == SERVICE_STEAM) {
        ui_text(160, 40, 11, UI_TEXT_DIM, UI_ALIGN_CENTER, "Turn the PC on and start Steam. Kasumi finds");
        ui_text(160, 55, 11, UI_TEXT_DIM, UI_ALIGN_CENTER, "it and shows a code to type into Steam there.");
        ui_text(160, 74, 11, UI_TEXT_FAINT, UI_ALIGN_CENTER, "The stream stays on your Wi-Fi.");
    } else {
        ui_text(160, 40, 11, UI_TEXT_DIM, UI_ALIGN_CENTER, "Sign-in happens on your phone or computer:");
        ui_text(160, 55, 11, UI_TEXT_DIM, UI_ALIGN_CENTER, "no password is typed on this console.");
    }
    /* The main button breathes in the service's light. */
    const float pulse = 0.5f + 0.5f * sinf((float)ui_ticks() / 1000.0f * 3.0f);
    const UiRect m = SIGNIN_MAIN;
    ui_rect(m.x - 3, m.y - 3, m.w + 6, m.h + 6, ui_with_alpha(service_light(s), (u8)(30.0f + 50.0f * pulse)));
    ui_button(SIGNIN_MAIN, SERVICE_INFO[s].sign_in, SERVICE_INFO[s].sign_in_jp, UI_BUTTON_PRIMARY,
              pressed(app, SIGNIN_MAIN));
    ui_button(SIGNIN_HUB, "HOME", "ホーム", UI_BUTTON_NORMAL, pressed(app, SIGNIN_HUB));
    ui_button(SIGNIN_SETTINGS, "SETTINGS", "設定", UI_BUTTON_NORMAL, pressed(app, SIGNIN_SETTINGS));
}

static void draw_login_bottom(const App *app)
{
    draw_status_strip(app, app->status);
    const bool xbox = login_is_xbox(app->client), steam = login_is_steam(app->client);
    const char *const steps[] = {
        steam ? "Go to the PC named above." : "Open the address shown above.",
        steam ? "Steam shows Authorize Device." : xbox ? "Sign in to your Microsoft account."
                                                : "Sign in to your NVIDIA account.",
        "Enter the code. Kasumi continues",
    };
    for (int i = 0; i < 3; ++i) {
        const float y = 40.0f + i * 34.0f;
        ui_ring(30, y + 8, 9, 1.2f, UI_ACCENT, UI_BG);
        ui_textf(30, y + 2, 11, UI_ACCENT, UI_ALIGN_CENTER, "%d", i + 1);
        ui_text(48, y + 1, 12, UI_TEXT, UI_ALIGN_LEFT, steps[i]);
        if (i < 2) ui_vline(30, y + 18, 16, UI_LINE);
    }
    ui_text(48, 123, 12, UI_TEXT, UI_ALIGN_LEFT, "on its own.");
    /* Where the login lives, and who made this. */
    ui_hline(16, 146, 288, UI_LINE);
    ui_text_wrap(160, 152, 11, UI_TEXT_DIM, UI_ALIGN_CENTER, 292, 2, 14,
                 steam ? "The pairing stays on this console's SD card; the stream never leaves your Wi-Fi. "
                         "Kasumi is unofficial, not made by Valve."
                 : xbox ? "Your login stays on this console's SD card only. Kasumi is unofficial, not made by "
                        "Microsoft." : "Your login stays on this console's SD card only. Kasumi is unofficial, "
                        "not made by NVIDIA.");
    ui_button(PAIR_LEFT, "NEW CODE", "再発行", UI_BUTTON_NORMAL, pressed(app, PAIR_LEFT));
    ui_button(PAIR_RIGHT, "CANCEL", "取消", UI_BUTTON_NORMAL, pressed(app, PAIR_RIGHT));
}

/* Steam Link: the paired PCs (the one in use first), pairing another,
 * forgetting the one in use. */
static void draw_pc_sheet_bottom(const App *app)
{
    char names[4][64];
    const int count = (int)steam_link_pcs(names, 4);
    draw_status_strip(app, "Your PCs: pick the one to stream from");
    for (int i = 0; i < count; ++i)
        ui_button(pc_row(i), names[i], i == 0 ? "IN USE - A RELOADS ITS GAMES" : "STREAM FROM THIS PC",
                  i == app->pc_index ? UI_BUTTON_PRIMARY : i == 0 ? UI_BUTTON_ACTIVE : UI_BUTTON_NORMAL,
                  pressed(app, pc_row(i)));
    if (count < 4)
        ui_text(160, pc_row(count).y + 8, 11, UI_TEXT_FAINT, UI_ALIGN_CENTER,
                "Up to four PCs; pair another one below.");
    ui_button(PC_PAIR, "PAIR ANOTHER PC", "追加", app->pc_index == count ? UI_BUTTON_PRIMARY : UI_BUTTON_NORMAL,
              pressed(app, PC_PAIR));
    ui_button(PC_FORGET, "FORGET THIS PC", "削除", app->pc_index == count + 1 ? UI_BUTTON_PRIMARY : UI_BUTTON_DANGER,
              pressed(app, PC_FORGET));
    ui_button(PC_CLOSE, "CLOSE", "閉じる", app->pc_index == count + 2 ? UI_BUTTON_PRIMARY : UI_BUTTON_NORMAL,
              pressed(app, PC_CLOSE));
}

static void draw_library_bottom(const App *app)
{
    if (app->pc_sheet_open) {
        draw_pc_sheet_bottom(app);
        return;
    }
    const GfnClient *client = app->client;
    draw_service_header(app);
    const LibraryLayout *l = library_layout(app);
    const bool compact = l == &LIB_COMPACT;
    const bool has_game = app_game(app, app->selected) != NULL;
    draw_arrow(l->prev, -1, has_game && app->selected > 0, pressed(app, l->prev));
    draw_arrow(l->next, 1, has_game && app->selected + 1 < app->list_count, pressed(app, l->next));

    const UiRect card = l->card;
    ui_surface(card, UI_LINE, UI_SURFACE);
    if (has_game) {
        const GfnGame *game = app_game(app, app->selected);
        /* The cover at the card's full height on the left, a wash of the
         * theme colour fading out behind it, and the text beside it. */
        const float pad = 6.0f, thumb_h = card.h - pad * 2, thumb = thumb_h * GAME_ART_WIDTH / GAME_ART_HEIGHT;
        ui_gradient_corners(card.x + 1, card.y + 1, card.w * 0.6f, card.h - 2,
                            ui_with_alpha(UI_ACCENT, 0x28), ui_with_alpha(UI_ACCENT, 0x00),
                            ui_with_alpha(UI_ACCENT, 0x28), ui_with_alpha(UI_ACCENT, 0x00));
        draw_game_art(game, card.x + pad, card.y + pad, thumb, 1.0f);
        const float text_x = card.x + pad + thumb + 10, text_w = card.x + card.w - 10 - text_x;
        const int lines = ui_text_wrap(text_x, card.y + (compact ? 8 : 11), 15, UI_TEXT, UI_ALIGN_LEFT,
                                       text_w, 2, 18, game->title);
        const float meta_y = card.y + (compact ? 14 : 17) + lines * 18.0f;
        const float pill_w = ui_pill(text_x, meta_y - 2, UI_ACCENT, UI_ALIGN_LEFT, store_label(game->store));
        if (game_prefs_favourite(game->app_id))
            ui_text(text_x + pill_w + 6, meta_y - 2, 12, UI_KIN, UI_ALIGN_LEFT, "★");
        if (!compact)
            ui_text_fit(text_x, meta_y + 18, 11, UI_TEXT_FAINT, UI_ALIGN_LEFT, text_w, stream_line(app));
    } else {
        ui_text(160, card.y + 22, 13, UI_TEXT_DIM, UI_ALIGN_CENTER, "No game selected");
        ui_text(160, card.y + 44, 11, UI_TEXT_FAINT, UI_ALIGN_CENTER, "Load your library or search");
    }

    /* Open the game's page; an empty tab offers the full list instead. */
    /* Continue: one tap (or START) back into the last game played. */
    if (compact) {
        const GfnGame *last = &client->games[app->continue_index];
        const UiRect c = ui_key(l->cont, UI_BUTTON_ACTIVE, pressed(app, l->cont));
        const float my = c.y + c.h / 2;
        ui_triangle(c.x + 12, my - 6, c.x + 12, my + 6, c.x + 21, my, UI_ACCENT);
        ui_label(c.x + 28, my - 6, 11, UI_ACCENT, UI_ALIGN_LEFT, "CONTINUE");
        const float label_w = ui_text_width("CONTINUE", 11) + 9.0f, gap = 8;
        ui_text_fit(c.x + 28 + label_w + gap, my - 8, 13, UI_TEXT, UI_ALIGN_LEFT,
                    c.w - 28 - label_w - gap - 54, last->title);
        ui_button_chip(c.x + c.w - 50, my - 7.5f, "START", UI_TEXT_DIM);
    }
    const char *main_label = has_game ? "OPEN GAME" : client->game_count ? "SHOW ALL GAMES" : "LOAD LIBRARY";
    const char *main_jp = has_game ? "詳細" : client->game_count ? "全て" : "ライブラリ";
    ui_button(l->play, main_label, main_jp, has_game ? UI_BUTTON_PRIMARY : UI_BUTTON_NORMAL,
              pressed(app, l->play));
    /* In the library this button refreshes it; after a search it goes back. */
    const bool searching = app->search_text[0] != '\0';
    /* Steam Link: its library is the PC's, so this button picks the PC. */
    const bool pcs = !searching && app_service(app) == SERVICE_STEAM;
    ui_button(l->library, searching ? "LIBRARY" : pcs ? "PCS" : "REFRESH",
              searching ? "ライブラリ" : pcs ? "パソコン" : "更新", UI_BUTTON_NORMAL, pressed(app, l->library));
    ui_button(l->search, "SEARCH", "検索",
              app->search_text[0] ? UI_BUTTON_ACTIVE : UI_BUTTON_NORMAL, pressed(app, l->search));
    ui_button(l->settings, "SETTINGS", "設定", UI_BUTTON_NORMAL, pressed(app, l->settings));
}

/* The symbol for a face button slot of the pad: 0 top, 1 right, 2 bottom,
 * 3 left (Triangle / Y, Circle / B, Cross / A, Square / X). */
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

/* The symbol a 3DS face button sends in the current layout.
 * key: 0 X (top), 1 A (right), 2 B (bottom), 3 Y (left). */
static void draw_ps_symbol_for_key(int key, bool position_layout, bool xbox, float cx, float cy, float size)
{
    static const int by_letter[4] = { 3, 2, 1, 0 };
    draw_face_slot(position_layout ? key : by_letter[key], xbox, cx, cy, size);
}

static void draw_face_diamond(float cx, float cy, bool playstation, bool position_layout, bool xbox)
{
    const float d = 17.0f, r = 10.0f;
    /* Order: top, right, bottom, left. */
    const float px[4] = { cx, cx + d, cx, cx - d };
    const float py[4] = { cy - d, cy, cy + d, cy };
    static const char *const letters[4] = { "X", "A", "B", "Y" };
    for (int i = 0; i < 4; ++i) {
        ui_ring(px[i], py[i], r, 1.2f, UI_LINE_STRONG, UI_SURFACE);
        if (playstation) draw_ps_symbol_for_key(i, position_layout, xbox, px[i], py[i], 11);
        else ui_text(px[i], py[i] - 6.5f, 12, UI_TEXT, UI_ALIGN_CENTER, letters[i]);
    }
}

/* A console tilting back and forth: the gyro preview. */
static void draw_gyro_preview(const App *app)
{
    const bool on = app->settings.gyro_mode != GFN_GYRO_OFF;
    const float t = (float)ui_ticks() / 1000.0f;
    const float sway = on ? sinf(t * 2.2f) * 10.0f : 0.0f;
    const float cx = 160 + sway, cy = 118;
    ui_rect(cx - 34, cy - 20, 68, 40, on ? UI_ACCENT : UI_LINE_STRONG);
    ui_rect(cx - 32, cy - 18, 64, 36, UI_SURFACE);
    ui_rect(cx - 20, cy - 12, 40, 24, on ? UI_ACCENT_DEEP : UI_BG);
    ui_circle(cx - 26, cy - 6, 3, UI_LINE_STRONG);
    for (int side = -1; side <= 1; side += 2) {
        const float ax = 160 + side * 58.0f;
        ui_triangle(ax, cy - 6, ax, cy + 6, ax + side * 8.0f, cy, on ? UI_ACCENT : UI_LINE_STRONG);
    }
    if (app->settings.gyro_mode == GFN_GYRO_WHILE_AIMING)
        ui_label(160, cy + 28, 11, UI_TEXT_FAINT, UI_ALIGN_CENTER,
                 app->settings.swap_shoulders ? "HOLD L TO AIM" : "HOLD ZL TO AIM");
}

/* Smooth <-> sharp meter for the bitrate choice. */
static void draw_bitrate_preview(const App *app)
{
    const float x0 = 60, x1 = 260, y = 124;
    ui_hline(x0, y, x1 - x0, UI_LINE_STRONG);
    ui_label(x0, y + 8, 11, UI_TEXT_FAINT, UI_ALIGN_LEFT, "SMOOTH");
    ui_label(x1, y + 8, 11, UI_TEXT_FAINT, UI_ALIGN_RIGHT, "SHARP");
    /* Positions by rate: 1 Mbps at the left, Sharp (~1.8-2) at the right. */
    static const float stops[STREAM_BITRATE_COUNT] = { 0.4f, 0.0f, 0.25f, 0.6f, 1.0f };
    for (int i = 1; i < STREAM_BITRATE_COUNT; ++i)
        ui_circle(x0 + (x1 - x0) * stops[i], y, 2.0f, UI_LINE_STRONG);
    const float at = x0 + (x1 - x0) * stops[app->settings.bitrate_mode];
    ui_circle(at, y, 6.0f, UI_ACCENT);
    ui_circle(at, y, 2.5f, UI_BG);
    if (app->settings.bitrate_mode == STREAM_BITRATE_ADAPTIVE)
        ui_rect(x0 + (x1 - x0) * 0.4f, y - 1, (x1 - x0) * 0.4f, 2, ui_with_alpha(UI_ACCENT, 0x70));
}

/* Settings home, lower screen: a tile per section. */
static void draw_settings_grid(const App *app)
{
    ui_text(160, 2, 12, UI_ACCENT, UI_ALIGN_CENTER, "設定");
    ui_label(160, 16, 11, UI_TEXT, UI_ALIGN_CENTER, "SETTINGS");
    ui_hline(0, 29, UI_BOTTOM_WIDTH, UI_LINE);
    const int count = screens_section_count();
    for (int i = 0; i < count && i < 6; ++i) {
        const UiRect r = set_tile(i);
        const bool on = i == app->settings_grid;
        const UiRect f = ui_key(r, on ? UI_BUTTON_ACTIVE : UI_BUTTON_NORMAL, pressed(app, r));
        const float cx = f.x + f.w / 2;
        /* Brush icons (gfx/sec_*.png), in the theme colour when chosen. */
        if (!ui_image_tint((UiImage)(UI_IMAGE_SEC_CONTROLS + i), cx - 20, f.y + 5, 1.0f,
                           on ? UI_ACCENT : UI_TEXT_DIM))
            ui_text(cx, f.y + 13, 17, on ? UI_ACCENT : UI_TEXT_DIM, UI_ALIGN_CENTER, SECTION_INFO[i].kanji);
        const SettingEntry *h = section_header(i);
        ui_text_fit(cx, f.y + 48, 10, on ? UI_TEXT : UI_TEXT_DIM, UI_ALIGN_CENTER, f.w - 8, h ? h->en : "");
    }
    ui_button(SET_BACK, "BACK", "戻る", UI_BUTTON_NORMAL, pressed(app, SET_BACK));
}

static void draw_settings_bottom(const App *app)
{
    if (app->mapping_open) {
        draw_mapping_bottom(app);
        return;
    }
    if (app->gallery_open) {
        draw_gallery_bottom(app);
        return;
    }
    if (app->settings_section < 0) {
        draw_settings_grid(app);
        return;
    }
    const int setting = screens_setting_at(app->setting_index);
    ui_text(160, 2, 12, UI_ACCENT, UI_ALIGN_CENTER, SETTING_JP[setting]);
    ui_label(160, 16, 11, UI_TEXT, UI_ALIGN_CENTER, SETTING_LABELS[setting]);
    ui_hline(0, 29, UI_BOTTOM_WIDTH, UI_LINE);
    ui_text_wrap(160, 36, 11, UI_TEXT_DIM, UI_ALIGN_CENTER, 292, 3, 14,
                 setting_description(app, setting));

    const bool account = setting == SETTING_ACCOUNT;
    if (setting == SETTING_LAYOUT || setting == SETTING_TRIGGERS) {
        const bool position = app->settings.button_layout == GFN_LAYOUT_POSITION;
        const bool xbox = app->settings.xbox_names;
        draw_face_diamond(96, 116, false, position, xbox);
        draw_face_diamond(224, 116, true, position, xbox);
        ui_triangle(152, 110, 152, 122, 164, 116, UI_ACCENT);
        ui_label(96, 146, 11, UI_TEXT_FAINT, UI_ALIGN_CENTER, "3DS");
        ui_label(224, 146, 11, UI_TEXT_FAINT, UI_ALIGN_CENTER, xbox ? "XBOX" : "PLAYSTATION");
        const bool swap = app->settings.swap_shoulders;
        const char *bumper[2] = { xbox ? "LB" : "L1", xbox ? "RB" : "R1" };
        const char *trigger[2] = { xbox ? "LT" : "L2", xbox ? "RT" : "R2" };
        ui_textf(160, 163, 11, UI_TEXT_DIM, UI_ALIGN_CENTER, "L  %s     ZL  %s     R  %s     ZR  %s",
                 swap ? trigger[0] : bumper[0], swap ? bumper[0] : trigger[0],
                 swap ? trigger[1] : bumper[1], swap ? bumper[1] : trigger[1]);
    } else {
        const float value_y = setting == SETTING_GYRO || setting == SETTING_BITRATE ? 146.0f : 104.0f;
        if (setting == SETTING_GYRO) draw_gyro_preview(app);
        if (setting == SETTING_THEME) {
            for (int i = 0; i < UI_THEME_COUNT; ++i) {
                const float sx = 160 + (i - (UI_THEME_COUNT - 1) / 2.0f) * 27.0f;
                const bool on = (unsigned)i == app->settings.theme;
                if (on) ui_ring(sx, 88, 12, 1.5f, UI_TEXT, UI_BG);
                ui_circle(sx, 88, 8, ui_theme_color((UiTheme)i));
            }
        }
        if (setting == SETTING_BITRATE) draw_bitrate_preview(app);
        /* The QR art carries its own white margin, so it scans on every theme. */
        if (setting == SETTING_COMMUNITY) ui_image(UI_IMAGE_DISCORD, 108, 79, 1.0f, 1.0f);
        else ui_text(160, value_y, setting == SETTING_GYRO || setting == SETTING_BITRATE ? 16 : 22,
                account ? UI_DANGER : UI_TEXT, UI_ALIGN_CENTER, setting_value(app, setting));
        unsigned count = 0;
        const unsigned option = setting_option(app, setting, &count);
        if (count > 1 && setting != SETTING_BITRATE)
            ui_dots(160, value_y + (setting == SETTING_GYRO ? 24.0f : 34.0f), count, option,
                    UI_ACCENT, UI_LINE_STRONG);
    }

    const bool arrows = !account && setting != SETTING_COMMUNITY;
    draw_arrow(SET_PREV, -1, arrows, pressed(app, SET_PREV));
    draw_arrow(SET_NEXT, 1, arrows, pressed(app, SET_NEXT));
    const bool sign_out = account && gfn_has_session(app->client);
    ui_button(SET_BACK, sign_out ? "SIGN OUT" : "BACK", sign_out ? "サインアウト" : "戻る",
              sign_out ? UI_BUTTON_DANGER : UI_BUTTON_NORMAL, pressed(app, SET_BACK));
}

static void draw_session_bottom(const App *app)
{
    const GfnClient *client = app->client;
    draw_status_strip(app, app->status);
    const UiRect card = { 16, 34, 288, 70 };
    ui_panel(card, UI_ACCENT);
    const bool art = app->current_game && app->current_game->app_id[0];
    if (art) draw_game_art(app->current_game, card.x + 8, card.y + 7, 42, 1.0f);
    const float text_x = art ? card.x + 58 : card.x + 10, text_w = art ? card.w - 66 : card.w - 20;
    ui_text_wrap(text_x + text_w / 2, card.y + 12, 14, UI_TEXT, UI_ALIGN_CENTER, text_w, 2, 17,
                 app->game_title[0] ? app->game_title : "Cloud session");
    ui_text_fit(text_x + text_w / 2, card.y + 50, 11, UI_TEXT_DIM, UI_ALIGN_CENTER, text_w,
                stream_profile_name());

    /* Two symmetric facts under the card. */
    ui_label(88, 116, 11, UI_TEXT_FAINT, UI_ALIGN_CENTER, "WI-FI");
    ui_wifi_icon(70, 133, app->wifi_bars, UI_ACCENT, UI_LINE_STRONG);
    ui_textf(96, 131, 15, UI_TEXT, UI_ALIGN_LEFT, "%u/3", app->wifi_bars);
    ui_vline(160, 116, 34, UI_LINE);
    const bool queued = client->session_state == GFN_SESSION_QUEUED;
    ui_label(232, 116, 11, UI_TEXT_FAINT, UI_ALIGN_CENTER, queued && app->queue_eta >= 0 ? "EST. WAIT" : "QUEUE");
    if (queued && app->queue_eta > 90)
        ui_textf(232, 131, 15, UI_TEXT, UI_ALIGN_CENTER, "~%d min", (app->queue_eta + 30) / 60);
    else if (queued && app->queue_eta >= 0)
        ui_text(232, 131, 15, UI_TEXT, UI_ALIGN_CENTER, app->queue_eta ? "~1 min" : "Soon");
    else if (queued && client->queue_position > 0)
        ui_textf(232, 131, 15, UI_TEXT, UI_ALIGN_CENTER, "#%d", client->queue_position);
    else
        ui_text(232, 131, 15, UI_TEXT, UI_ALIGN_CENTER,
                client->session_state == GFN_SESSION_QUEUED ? "-" : "Done");
    if (app->wifi_bars < 2)
        ui_text(160, 160, 11, UI_KIN, UI_ALIGN_CENTER, "Weak Wi-Fi: move closer to the router.");

    if (screens_session_failed(app) && !app->waiting_wifi && (app->reconnect_attempt == 0 || app->reconnect_attempt > 3)) {
        ui_button(PAIR_LEFT, "RETRY", "再試行", UI_BUTTON_PRIMARY, pressed(app, PAIR_LEFT));
        ui_button(PAIR_RIGHT, "LEAVE", "退出", UI_BUTTON_DANGER, pressed(app, PAIR_RIGHT));
    } else {
        ui_button(SINGLE, "LEAVE", "退出", UI_BUTTON_DANGER, pressed(app, SINGLE));
    }
}

static void draw_zoom_map(void)
{
    const UiRect frame = { STR_PANEL.x + 4, STR_PANEL.y + 6, STR_PANEL.w - 8, (STR_PANEL.w - 8) * 9 / 16 };
    ui_rect_r(frame, UI_SURFACE);
    ui_outline(frame.x, frame.y, frame.w, frame.h, 1.0f, UI_LINE_STRONG);
    const unsigned level = mvd_video_zoom_level();
    const float fraction = level == 1 ? 0.833f : level == 2 ? 0.667f : 0.5f;
    unsigned px = 50, py = 50;
    mvd_video_zoom_position(&px, &py);
    const float vw = frame.w * fraction, vh = frame.h * fraction;
    float vx = frame.x + frame.w * px / 100.0f - vw / 2;
    float vy = frame.y + frame.h * py / 100.0f - vh / 2;
    if (vx < frame.x) vx = frame.x;
    if (vy < frame.y) vy = frame.y;
    if (vx + vw > frame.x + frame.w) vx = frame.x + frame.w - vw;
    if (vy + vh > frame.y + frame.h) vy = frame.y + frame.h - vh;
    ui_rect(vx, vy, vw, vh, ui_with_alpha(UI_ACCENT, 0x30));
    ui_outline(vx, vy, vw, vh, 1.5f, UI_ACCENT);
    ui_label(160, frame.y + frame.h + 4, 11, UI_TEXT_DIM, UI_ALIGN_CENTER, "DRAG TO MOVE THE VIEW");
}

static void draw_touchpad(void)
{
    const UiRect p = STR_PANEL;
    ui_rect_r(p, UI_SURFACE);
    /* Dashed border. */
    for (float x = p.x; x < p.x + p.w; x += 8) {
        ui_rect(x, p.y, 4, 1, UI_LINE_STRONG);
        ui_rect(x, p.y + p.h - 1, 4, 1, UI_LINE_STRONG);
    }
    for (float y = p.y; y < p.y + p.h; y += 8) {
        ui_rect(p.x, y, 1, 4, UI_LINE_STRONG);
        ui_rect(p.x + p.w - 1, y, 1, 4, UI_LINE_STRONG);
    }
    ui_text(160, p.y + 22, 12, UI_ACCENT, UI_ALIGN_CENTER, "マウス・キーボード");
    ui_label(160, p.y + 40, 11, UI_TEXT, UI_ALIGN_CENTER, "MOUSE & KEYBOARD");
    ui_text_fit(160, p.y + 58, 11, UI_TEXT_DIM, UI_ALIGN_CENTER, 176, "Drag here to move, tap to click");
    ui_text_fit(160, p.y + 73, 11, UI_TEXT_DIM, UI_ALIGN_CENTER, 176, "C-Stick moves, A clicks, KEYS types");
    ui_text_fit(160, p.y + 88, 11, UI_TEXT_FAINT, UI_ALIGN_CENTER, 176, "Tap MOUSE again for controller mode");
}

static void draw_stats(const App *app)
{
    const WebRtcTransport *t = app->transport;
    const UiRect p = STR_PANEL;
    if (!app->settings.show_stats) {
        ui_panel(p, UI_ACCENT);
        ui_text_wrap(160, p.y + 26, 14, UI_TEXT, UI_ALIGN_CENTER, p.w - 16, 2, 17, app->game_title);
        ui_label(160, p.y + 78, 11, UI_TEXT_DIM, UI_ALIGN_CENTER,
                 app->settings.button_layout == GFN_LAYOUT_POSITION ? "POSITION LAYOUT" : "LETTER LAYOUT");
        return;
    }
    const float tw = (p.w - 4) / 2, th = (p.h - 4) / 2;
    char values[4][24];
    snprintf(values[0], sizeof(values[0]), "%u", app->fps);
    snprintf(values[1], sizeof(values[1]), "%.1f", t->video_kbps / 1000.0f);
    snprintf(values[2], sizeof(values[2]), "%d", t->rtt_ms);
    snprintf(values[3], sizeof(values[3]), "%u", app->resent_per_second);
    static const char *const labels[4] = { "FPS", "MBPS", "PING MS", "RESENT/S" };
    for (int i = 0; i < 4; ++i) {
        const float x = p.x + (i % 2) * (tw + 4), y = p.y + (i / 2) * (th + 4);
        bool warn = false;
        if (i == 0 && app->fps > 0 && app->fps < 24) warn = true;
        if (i == 2 && t->rtt_ms > 80) warn = true;
        if (i == 3 && app->resent_per_second > 2) warn = true;
        ui_rect(x, y, tw, th, UI_SURFACE);
        ui_outline(x, y, tw, th, 1.0f, warn ? UI_KIN : UI_LINE);
        ui_rect(x, y + th - 2, tw, 2, warn ? UI_KIN : UI_ACCENT_DEEP);
        ui_text(x + tw / 2, y + 9, 20, warn ? UI_KIN : UI_TEXT, UI_ALIGN_CENTER, values[i]);
        ui_label(x + tw / 2, y + th - 17, 11, UI_TEXT_FAINT, UI_ALIGN_CENTER, labels[i]);
    }
}

static void draw_stick_button(UiRect r, const char *label, const char *jp, bool is_pressed)
{
    ui_rect_r(r, is_pressed ? UI_ACCENT : UI_BG);
    ui_outline(r.x, r.y, r.w, r.h, 1.0f, is_pressed ? UI_ACCENT : UI_LINE_STRONG);
    const float cx = r.x + r.w / 2, cy = r.y + r.h / 2;
    ui_ring(cx, cy - 8, 16, 1.2f, is_pressed ? UI_BG : UI_LINE_STRONG, is_pressed ? UI_ACCENT : UI_BG);
    ui_text(cx, cy - 16, 15, is_pressed ? UI_BG : UI_TEXT, UI_ALIGN_CENTER, label);
    ui_text(cx, cy + 16, 12, is_pressed ? UI_BG : UI_TEXT_FAINT, UI_ALIGN_CENTER, jp);
}

static void draw_stream_menu(const App *app, float p)
{
    ui_rect(0, 0, UI_BOTTOM_WIDTH, UI_HEIGHT, ui_with_alpha(UI_BG, (u8)(0xC8 * p)));
    ui_offset(0.0f, (1.0f - p) * 10.0f);
    draw_card(MENU_PANEL, p);
    ui_text(160, MENU_PANEL.y + 8, 12, UI_ACCENT, UI_ALIGN_CENTER, "一時停止");
    ui_label(160, MENU_PANEL.y + 23, 11, UI_TEXT, UI_ALIGN_CENTER, "STREAM MENU");
    const char *layout = app->settings.button_layout == GFN_LAYOUT_POSITION ? "LAYOUT: POS" : "LAYOUT: ABC";
    const char *gyro = app->settings.gyro_mode == GFN_GYRO_ALWAYS ? "GYRO: ON" :
                       app->settings.gyro_mode == GFN_GYRO_WHILE_AIMING ? "GYRO: AIM" : "GYRO: OFF";
    const char *zone = mvd_video_zoomed() ? "SAVE ZONE" : zoom_zones_count() ? "CLEAR ZONES" : "SAVE ZONE";
    const char *labels[STREAM_MENU_COUNT] = {
        "RESUME", "SCREENSHOT", "CONTROLS", zone, gyro,
        app->sound_muted ? "SOUND: OFF" : "SOUND: ON", layout, "DISCONNECT"
    };
    static const char *const jp[STREAM_MENU_COUNT] = {
        "再開", "撮影", "操作", "ズーム", "ジャイロ", "音声", "ボタン配置", "切断"
    };
    for (int i = 0; i < STREAM_MENU_COUNT; ++i) {
        const UiRect r = menu_item(i);
        const bool focus = i == app->stream_menu_index;
        const UiButtonStyle style = i == STREAM_MENU_DISCONNECT ? UI_BUTTON_DANGER :
                                    i == STREAM_MENU_RESUME ? UI_BUTTON_PRIMARY :
                                    focus ? UI_BUTTON_ACTIVE : UI_BUTTON_NORMAL;
        ui_button(r, labels[i], jp[i], style, pressed(app, r));
        if (focus) ui_outline(r.x - 3, r.y - 3, r.w + 6, r.h + 6, 1.5f, UI_ACCENT);
    }
    ui_offset(0.0f, 0.0f);
}

/* One line of the controls sheet: a 3DS input on the left, what it does. */
static void controls_row(float y, const char *input, const char *action)
{
    const float chip_w = ui_button_chip(24, y, input, UI_TEXT_DIM);
    (void)chip_w;
    ui_text(132, y + 1, 12, UI_TEXT, UI_ALIGN_LEFT, action);
}

static void draw_controls_sheet(const App *app, float p)
{
    ui_rect(0, 0, UI_BOTTOM_WIDTH, UI_HEIGHT, ui_with_alpha(UI_BG, (u8)(0xF0 * p)));
    ui_offset(0.0f, (1.0f - p) * 10.0f);
    ui_text(160, 4, 12, UI_ACCENT, UI_ALIGN_CENTER, "操作");
    ui_label(160, 18, 11, UI_TEXT, UI_ALIGN_CENTER, "CONTROLS");
    ui_hline(16, 33, 288, UI_LINE);

    const AppSettings *s = &app->settings;
    const bool position = s->button_layout == GFN_LAYOUT_POSITION;
    /* Face buttons with the symbol each one sends. */
    const bool xbox = s->xbox_names;
    static const char *const keys[4] = { "X", "A", "B", "Y" };
    float x = 26;
    for (int i = 0; i < 4; ++i) {
        ui_button_chip(x, 40, keys[i], UI_TEXT_DIM);
        draw_ps_symbol_for_key(i, position, xbox, x + 28, 47.5f, 11);
        x += 72;
    }
    float y = 64;
    const float dy = 17;
    controls_row(y, "CIRCLE", "Left stick"); y += dy;
    controls_row(y, "C-STICK", s->gyro_mode != GFN_GYRO_OFF ? "Right stick + gyro" : "Right stick"); y += dy;
    controls_row(y, s->swap_shoulders ? "ZL ZR" : "L R", xbox ? "LB / RB" : "L1 / R1"); y += dy;
    controls_row(y, s->swap_shoulders ? "L R" : "ZL ZR", xbox ? "LT / RT triggers" : "L2 / R2 triggers"); y += dy;
    controls_row(y, "START", xbox ? "Menu" : "Options"); y += dy;
    controls_row(y, "SELECT", xbox ? "View" : "Share / View"); y += dy;
    controls_row(y, "TOUCH", xbox ? "LS / RS / Xbox · middle: both" : "L3 / R3 / PS · middle: both"); y += dy;
    controls_row(y, "START+SELECT", "Hold for the stream menu"); y += dy;
    if (gfn_input_custom_map_active())
        ui_text(160, y + 4, 11, UI_ACCENT, UI_ALIGN_CENTER, "Your own button mapping is in use");
    else
        ui_textf(160, y + 4, 11, UI_TEXT_FAINT, UI_ALIGN_CENTER, "Gyro aim: %s  ·  change it in the stream menu",
                 gyro_mode_name(s->gyro_mode));
    static const char *const hints[] = { "B", "Close", NULL };
    ui_hint_row(160, 222, hints);
    ui_offset(0.0f, 0.0f);
}

static void draw_stream_header(const App *app)
{
    const WebRtcTransport *t = app->transport;
    const u64 now = plat_time_ms();
    const u64 elapsed = app->stream_started_at ? now - app->stream_started_at : 0;
    /* Free rigs end after an hour: count down over the last five minutes. */
    const bool ending = app->free_tier_guess && elapsed >= 55ull * 60 * 1000;
    if (app->video_stalled) {
        ui_rect(0, 0, UI_BOTTOM_WIDTH, 23, UI_KIN);
        ui_text(160, 5, 11, UI_BG, UI_ALIGN_CENTER, "Connection unstable - waiting for video");
        return;
    }
    if (app->toast) {
        ui_rect(0, 0, UI_BOTTOM_WIDTH, 23, UI_ACCENT_DEEP);
        ui_text_fit(160, 5, 11, UI_TEXT, UI_ALIGN_CENTER, 296, app->toast);
        ui_hline(0, 23, UI_BOTTOM_WIDTH, UI_ACCENT);
        return;
    }
    ui_circle(12, 11, 3.5f, t->input_ready ? UI_ACCENT : UI_KIN);
    /* Which way the buttons go right now: a gamepad, or mouse and keyboard. */
    if (t->pointer_mode) {
        ui_rect(19, 4, 44, 15, UI_ACCENT);
        ui_label(41, 6, 11, UI_BG, UI_ALIGN_CENTER, "MOUSE");
    } else {
        ui_outline(19, 4, 44, 15, 1.0f, UI_LINE_STRONG);
        ui_label(41, 6, 11, UI_TEXT_DIM, UI_ALIGN_CENTER, "PAD");
    }
    /* For the first seconds, teach the menu shortcut instead of the title. */
    if (app->stream_started_at && elapsed < 7000)
        ui_text_fit(160, 5, 11, UI_TEXT_DIM, UI_ALIGN_CENTER, 190, "Hold START + SELECT for the menu");
    else
        ui_text_fit(160, 5, 11, UI_TEXT, UI_ALIGN_CENTER, 190, app->game_title);
    char timer[24];
    if (ending) {
        const u64 left = elapsed >= 60ull * 60 * 1000 ? 0 : 60ull * 60 * 1000 - elapsed;
        format_elapsed(timer, sizeof(timer), left);
        ui_text(308, 5, 11, UI_KIN, UI_ALIGN_RIGHT, timer);
    } else {
        format_elapsed(timer, sizeof(timer), elapsed);
        ui_text(308, 5, 11, UI_TEXT_DIM, UI_ALIGN_RIGHT, timer);
    }
    ui_hline(0, 23, UI_BOTTOM_WIDTH, ending ? UI_KIN : UI_LINE);
}

static void draw_welcome_back(const App *app);
static void draw_stick_button(UiRect r, const char *label, const char *jp, bool is_pressed);

/* ---- Touch camera ----------------------------------------------------------- */

/* The layout while it is on: L3 and PS on the left, one line of stats on
 * top, and the rest is the pad, with an R3 corner. */
static const UiRect LOOK_L3 = { 6, 30, 56, 112 };
static const UiRect LOOK_PS = { 6, 146, 56, 42 };
static const UiRect LOOK_STATS = { 68, 30, 246, 18 };
static const UiRect LOOK_PAD = { 68, 52, 246, 136 };
static const UiRect LOOK_R3 = { 262, 162, 48, 22 };
/* HIDE in the pad's top corner, C-STICK on the rule right of PS. */
static const UiRect LOOK_HIDE = { 262, 56, 48, 20 };
/* L3 and R3 together (two fingers would land on the pad). */
static const UiRect LOOK_BOTH = { 262, 136, 48, 22 };
/* Voice chat's mute button: under HIDE on the pad, or on the left rule
 * between L3 and PS in the usual layout. */
static const UiRect LOOK_MIC = { 262, 80, 48, 20 };
static const UiRect STR_MIC = { 70, 156, 56, 22 };
static const UiRect STR_LOOK = { 194, 156, 58, 22 };
#define LOOK_TRAIL_MS 320.0f
#define LOOK_RELEASE_MS 260.0f

UiRect screens_look_pad(void) { return LOOK_PAD; }
UiRect screens_look_r3(void) { return LOOK_R3; }
UiRect screens_look_hide(void) { return LOOK_HIDE; }
UiRect screens_look_both(void) { return LOOK_BOTH; }
UiRect screens_look_mic(void) { return LOOK_MIC; }

/* MIC ON (lit, with a level bar) or MIC OFF. */
static void mic_chip(UiRect r)
{
    const bool live = !mic_capture_muted();
    ui_rect_r(r, live ? UI_ACCENT : ui_with_alpha(UI_BG, 0xC0));
    ui_outline(r.x, r.y, r.w, r.h, 1.0f, live ? UI_ACCENT : UI_LINE_STRONG);
    ui_label(r.x + r.w / 2, r.y + (r.h - 11) / 2, 11, live ? UI_BG : UI_TEXT_DIM, UI_ALIGN_CENTER,
             live ? "MIC ON" : "MIC OFF");
    if (live) {
        float level = mic_capture_level() * 3.0f;
        if (level > 1.0f) level = 1.0f;
        ui_rect(r.x + 3, r.y + r.h - 3, (r.w - 6) * level, 2, UI_BG);
    }
}

/* A small square key with one word, lit while active. */
static void look_chip(UiRect r, const char *label, bool lit, bool down)
{
    ui_rect_r(r, down || lit ? UI_ACCENT : ui_with_alpha(UI_BG, 0xC0));
    ui_outline(r.x, r.y, r.w, r.h, 1.0f, down || lit ? UI_ACCENT : UI_LINE_STRONG);
    ui_label(r.x + r.w / 2, r.y + (r.h - 11) / 2, 11, down || lit ? UI_BG : UI_TEXT, UI_ALIGN_CENTER, label);
}

static void draw_look_stats(const App *app)
{
    const WebRtcTransport *t = app->transport;
    const UiRect r = LOOK_STATS;
    ui_rect_r(r, UI_SURFACE);
    ui_outline(r.x, r.y, r.w, r.h, 1.0f, UI_LINE);
    if (!app->settings.show_stats) {
        ui_text_fit(r.x + r.w / 2, r.y + 2, 11, UI_TEXT_DIM, UI_ALIGN_CENTER, r.w - 12, app->game_title);
        return;
    }
    /* Four readings across, amber when one is a problem. */
    char values[4][16];
    snprintf(values[0], sizeof(values[0]), i18n_text("%u FPS"), app->fps);
    snprintf(values[1], sizeof(values[1]), i18n_text("%.1f MBPS"), t->video_kbps / 1000.0f);
    snprintf(values[2], sizeof(values[2]), i18n_text("%d MS"), t->rtt_ms);
    snprintf(values[3], sizeof(values[3]), i18n_text("%u RESENT"), app->resent_per_second);
    const bool warn[4] = { app->fps > 0 && app->fps < 24, false, t->rtt_ms > 80, app->resent_per_second > 2 };
    const float cell = r.w / 4;
    for (int i = 0; i < 4; ++i) {
        ui_label(r.x + cell * i + cell / 2, r.y + 3, 11, warn[i] ? UI_KIN : UI_TEXT_DIM, UI_ALIGN_CENTER, values[i]);
        if (i) ui_vline(r.x + cell * i, r.y + 4, r.h - 8, UI_LINE);
    }
}

/* Small L marks in the pad's corners, like the buttons'. */
static void look_corners(UiRect r, u32 color)
{
    const float m = 9.0f, t = 1.5f;
    ui_rect(r.x, r.y, m, t, color);
    ui_rect(r.x, r.y + t, t, m - t, color);
    ui_rect(r.x + r.w - m, r.y, m, t, color);
    ui_rect(r.x + r.w - t, r.y + t, t, m - t, color);
    ui_rect(r.x, r.y + r.h - t, m, t, color);
    ui_rect(r.x, r.y + r.h - m, t, m - t, color);
    ui_rect(r.x + r.w - m, r.y + r.h - t, m, t, color);
    ui_rect(r.x + r.w - t, r.y + r.h - m, t, m - t, color);
}

/* The 96 px ensō, centred at a size in pixels. */
static void look_ring(float cx, float cy, float size, u32 color)
{
    const float scale = size / 96.0f;
    ui_image_tint(UI_IMAGE_LOOK_RING, cx - 48.0f * scale, cy - 48.0f * scale, scale, color);
}

static void look_dot(float cx, float cy, float size, u32 color)
{
    const float scale = size / 32.0f;
    ui_image_tint(UI_IMAGE_LOOK_DOT, cx - 16.0f * scale, cy - 16.0f * scale, scale, color);
}

static void draw_look_pad(const App *app, bool r3_held, bool both_held)
{
    const UiRect p = LOOK_PAD;
    /* The theme's own wallpaper, calmed down so the ink reads over it. */
    if (!ui_wallpaper(p)) ui_rect_r(p, UI_SURFACE);
    ui_rect_r(p, UI_RGBA(0x00, 0x00, 0x00, 0x60));
    ui_outline(p.x, p.y, p.w, p.h, 1.0f, ui_with_alpha(UI_ACCENT, 0x70));
    look_corners(p, UI_ACCENT);

    const u64 now = ui_ticks();
    const bool stick = app->look_mode == 1;
    ui_text(p.x + 8, p.y + 5, 11, UI_ACCENT, UI_ALIGN_LEFT, "視点");
    ui_label(p.x + 32, p.y + 6, 11, UI_TEXT_DIM, UI_ALIGN_LEFT, stick ? "PUSH TO LOOK" : "DRAG TO LOOK");
    ui_label(p.x + 8, p.y + p.h - 18, 11, UI_TEXT_FAINT, UI_ALIGN_LEFT,
             app->settings.xbox_names ? "DOUBLE-TAP  RS" : "DOUBLE-TAP  R3");

    /* R3 in the corner, for presses without a double tap; HIDE above it
     * brings the stats back. */
    const bool xbox = app->settings.xbox_names;
    look_chip(LOOK_R3, xbox ? "RS" : "R3", r3_held && !both_held, false);
    look_chip(LOOK_BOTH, xbox ? "LS+RS" : "L3+R3", both_held, false);
    look_chip(LOOK_HIDE, "HIDE", false, pressed(app, LOOK_HIDE));
    if (app->mic_available) mic_chip(LOOK_MIC);

    /* Idle: a faint ensō says "touch here". */
    const float since_release = app->look_released_at ? (float)(now - app->look_released_at) : 1e9f;
    if (!app->look_active && since_release > LOOK_RELEASE_MS)
        look_ring(p.x + p.w / 2, p.y + p.h / 2 + 4, 58, ui_with_alpha(UI_TEXT, 0x38));

    /* The ink trail: newest dots biggest, fading out. */
    for (unsigned i = 0; i < 10; ++i) {
        const u64 at = app->look_trail_at[i];
        if (!at || now - at > (u64)LOOK_TRAIL_MS) continue;
        const float k = 1.0f - (float)(now - at) / LOOK_TRAIL_MS;
        look_dot(app->look_trail_x[i], app->look_trail_y[i], 6.0f + 8.0f * k,
                 ui_with_alpha(UI_ACCENT, (u8)(0xB0 * k)));
    }

    if (app->look_active) {
        const float fx = app->look_x, fy = app->look_y;
        if (stick) {
            /* The ring is the stick's rim, centred where the finger landed
             * (dragged along past the rim); the dot is the finger. */
            const float ax = app->look_anchor_x, ay = app->look_anchor_y;
            float dx = fx - ax, dy = fy - ay;
            const float limit = app->look_radius > 0.0f ? app->look_radius : 34.0f;
            const float d = sqrtf(dx * dx + dy * dy);
            if (d > limit) { dx *= limit / d; dy *= limit / d; }
            look_ring(ax, ay, limit * 2.0f + 12.0f, ui_with_alpha(UI_TEXT, 0xE0));
            look_dot(ax + dx, ay + dy, 22, UI_ACCENT);
        } else {
            /* The ring rides under the finger and swells as it turns. */
            look_ring(fx, fy, 48 + 18 * app->look_amount, ui_with_alpha(UI_TEXT, 0xE0));
            look_dot(fx, fy, 12, UI_ACCENT);
        }
    } else if (since_release <= LOOK_RELEASE_MS) {
        /* Lifting the finger: the ring opens out and fades. */
        const float k = since_release / LOOK_RELEASE_MS;
        look_ring(app->look_release_x, app->look_release_y, 56 + 30 * k,
                  ui_with_alpha(UI_TEXT, (u8)(0xC0 * (1.0f - k))));
    }
}

/* The PS / Xbox button: a ring with its name. */
static void draw_guide_button(const App *app, float gx, float gy, bool guide)
{
    const bool xbox = app->settings.xbox_names;
    ui_ring(gx, gy, 17, 1.5f, guide ? UI_ACCENT : UI_LINE_STRONG, guide ? UI_ACCENT : UI_BG);
    ui_text(gx, gy - (xbox ? 5.5f : 7.0f), xbox ? 10.0f : 12.0f, guide ? UI_BG : UI_TEXT, UI_ALIGN_CENTER,
            xbox ? "XBOX" : "PS");
}

static void draw_look_layout(const App *app, uint16_t held)
{
    const bool xbox = app->settings.xbox_names;
    const uint16_t both = GFN_PAD_LEFT_THUMB | GFN_PAD_RIGHT_THUMB;
    draw_stick_button(LOOK_L3, xbox ? "LS" : "L3", "左", (held & GFN_PAD_LEFT_THUMB) != 0);
    draw_guide_button(app, LOOK_PS.x + LOOK_PS.w / 2, LOOK_PS.y + LOOK_PS.h / 2, (held & GFN_PAD_GUIDE) != 0);
    draw_look_stats(app);
    draw_look_pad(app, (held & GFN_PAD_RIGHT_THUMB) != 0, (held & both) == both);
}

/* The usual layout: L3 and R3 columns, stats (or the mouse pad or zoom map)
 * between them, PS below. */
static void draw_classic_layout(const App *app, uint16_t held)
{
    const WebRtcTransport *t = app->transport;
    const bool xbox = app->settings.xbox_names;
    draw_stick_button(STR_L3, xbox ? "LS" : "L3", "左", (held & GFN_PAD_LEFT_THUMB) != 0);
    draw_stick_button(STR_R3, xbox ? "RS" : "R3", "右", (held & GFN_PAD_RIGHT_THUMB) != 0);

    if (t->pointer_mode) {
        draw_touchpad();
    } else if (mvd_video_zoomed()) {
        draw_zoom_map();
    } else {
        draw_stats(app);
        /* The stats panel is also both sticks pressed together. */
        const uint16_t both = GFN_PAD_LEFT_THUMB | GFN_PAD_RIGHT_THUMB;
        if ((held & both) == both) {
            ui_rect_r(STR_PANEL, ui_with_alpha(UI_ACCENT, 0xE0));
            ui_text(STR_PANEL.x + STR_PANEL.w / 2, STR_PANEL.y + STR_PANEL.h / 2 - 9, 18, UI_BG, UI_ALIGN_CENTER,
                    xbox ? "LS + RS" : "L3 + R3");
        }
    }

    /* Guide / PS button, centred between the stick buttons. */
    const float gx = STR_GUIDE.x + STR_GUIDE.w / 2, gy = STR_GUIDE.y + STR_GUIDE.h / 2;
    draw_guide_button(app, gx, gy, (held & GFN_PAD_GUIDE) != 0);
    ui_hline(STR_L3.x + STR_L3.w + 6, gy, gx - 17 - (STR_L3.x + STR_L3.w + 6) - 4, UI_LINE);
    if (app->look_available) {
        /* C-STICK sits on the right-hand rule: tap it for the touch camera. */
        ui_hline(gx + 21, gy, STR_LOOK.x - 4 - (gx + 21), UI_LINE);
        look_chip(STR_LOOK, "C-STICK", false, pressed(app, STR_LOOK));
    } else {
        ui_hline(gx + 21, gy, STR_R3.x - 6 - (gx + 21), UI_LINE);
    }
    if (app->mic_available) {
        /* Voice chat's button takes the left rule (gyro shows in the menu). */
        mic_chip(STR_MIC);
    } else if (app->settings.gyro_mode != GFN_GYRO_OFF) {
        /* Gyro badge on the left rule, lit while gyro is steering. */
        const bool live = gfn_input_gyro_active();
        ui_rect(78, gy - 8, 44, 16, UI_BG);
        ui_label(100, gy - 6, 11, live ? UI_ACCENT : UI_TEXT_FAINT, UI_ALIGN_CENTER, "GYRO");
    }
}

static void draw_stream_bottom(const App *app, float overlay_p)
{
    const WebRtcTransport *t = app->transport;
    if (app->keyboard_open) {
        remote_keyboard_draw(t, app->touching, app->touch_x, app->touch_y);
        return;
    }
    draw_stream_header(app);

    const uint16_t held = app->touch_buttons | (app->look_r3 ? GFN_PAD_RIGHT_THUMB : 0);
    if (app->look_mode) draw_look_layout(app, held);
    else draw_classic_layout(app, held);

    char zoom[16];
    const unsigned level = mvd_video_zoom_level();
    if (app->zone_index >= 0 && level) snprintf(zoom, sizeof(zoom), i18n_text("ZONE %d"), app->zone_index + 1);
    else snprintf(zoom, sizeof(zoom), "%s", level == 3 ? "2.0x" : level == 2 ? "1.5x" : level == 1 ? "1.2x" : "拡大");
    const char *labels[4] = { "KEYS", "MOUSE", "ZOOM", "MENU" };
    const char *jp[4] = { "キー", "マウス", zoom, "メニュー" };
    const bool active[4] = { false, t->pointer_mode, level != 0, false };
    for (int i = 0; i < 4; ++i) {
        const UiRect r = stream_button(i);
        ui_button(r, labels[i], jp[i], active[i] ? UI_BUTTON_ACTIVE : UI_BUTTON_NORMAL, pressed(app, r));
    }
    if (app->controls_open) draw_controls_sheet(app, overlay_p);
    else if (app->stream_menu) draw_stream_menu(app, overlay_p);
    else draw_welcome_back(app);
}

/* After a lid pause: a short card over the lower screen's panel. */
#define WELCOME_MS 3200
static bool welcome_visible(const App *app)
{
    return app->welcome_at && plat_time_ms() - app->welcome_at < WELCOME_MS;
}

static void draw_welcome_back(const App *app)
{
    if (!welcome_visible(app)) return;
    const u64 t = plat_time_ms() - app->welcome_at;
    float a = 1.0f;
    if (t < 250) a = t / 250.0f;
    else if (t > WELCOME_MS - 500) a = (WELCOME_MS - t) / 500.0f;
    const float e = ui_ease_out(a);
    const u8 alpha = (u8)(0xFF * e);
    const UiRect p = STR_PANEL;
    ui_offset(0.0f, (1.0f - e) * 6.0f);
    ui_rect(p.x, p.y, p.w, p.h, ui_with_alpha(UI_BG, (u8)(0xF0 * e)));
    ui_outline(p.x, p.y, p.w, p.h, 1.0f, ui_with_alpha(UI_ACCENT, alpha));
    ui_enso(160, p.y + 26, 15, ui_with_alpha(UI_ACCENT, alpha));
    ui_text(160, p.y + 48, 15, ui_with_alpha(UI_ACCENT, alpha), UI_ALIGN_CENTER, "おかえり");
    ui_label(160, p.y + 67, 11, ui_with_alpha(UI_TEXT, alpha), UI_ALIGN_CENTER, "WELCOME BACK");
    char away[48];
    if (app->welcome_away_s >= 90)
        snprintf(away, sizeof(away), i18n_text("Paused %u min · still connected"), (app->welcome_away_s + 30) / 60);
    else
        snprintf(away, sizeof(away), i18n_text("Paused %u s · still connected"), app->welcome_away_s);
    ui_text(160, p.y + 83, 11, ui_with_alpha(UI_TEXT_DIM, alpha), UI_ALIGN_CENTER, away);
    ui_offset(0.0f, 0.0f);
}

static void draw_modal_bottom(const App *app, float p)
{
    /* Nearly opaque: the library's own text showed through the title. */
    ui_rect(0, 0, UI_BOTTOM_WIDTH, UI_HEIGHT, ui_with_alpha(UI_BG, (u8)(0xF0 * p)));
    ui_offset(0.0f, (1.0f - p) * 10.0f);
    ui_text(160, 68, 12, UI_ACCENT, UI_ALIGN_CENTER, app->modal_jp);
    ui_label(160, 84, 11, UI_TEXT, UI_ALIGN_CENTER, app->modal_title);
    const bool error = app->modal == MODAL_ERROR, resume = app->modal == MODAL_RESUME;
    if (app->modal == MODAL_REPORT_SENT) {
        ui_text(160, 104, 22, UI_TEXT, UI_ALIGN_CENTER, app->report_code);
        ui_button(MODAL_ONLY, "OK", "了解", UI_BUTTON_PRIMARY, pressed(app, MODAL_ONLY));
        ui_offset(0.0f, 0.0f);
        return;
    }
    const bool send = app->modal == MODAL_SEND_REPORT, share = app->modal == MODAL_SHARE_ASK;
    if (app->modal == MODAL_PROVIDER_PICK) {
        GfnProvider partner;
        providers_partner_here(&partner, NULL);
        ui_button(MODAL_LEFT, partner.name, "推奨", UI_BUTTON_PRIMARY, pressed(app, MODAL_LEFT));
        ui_button(MODAL_RIGHT, "NVIDIA", "エヌビディア", UI_BUTTON_NORMAL, pressed(app, MODAL_RIGHT));
        ui_offset(0.0f, 0.0f);
        return;
    }
    if (app->modal == MODAL_PAID_ONLY) {
        ui_button(MODAL_LEFT, "PLAY ANYWAY", "開始", UI_BUTTON_PRIMARY, pressed(app, MODAL_LEFT));
        ui_button(MODAL_RIGHT, "BACK", "戻る", UI_BUTTON_NORMAL, pressed(app, MODAL_RIGHT));
        ui_offset(0.0f, 0.0f);
        return;
    }
    if (app->modal == MODAL_STEAM_LEAVE) {
        ui_text_wrap(160, 100, 11, UI_TEXT_DIM, UI_ALIGN_CENTER, 288, 2, 14, app->modal_text);
        ui_button(MODAL_LEFT, "DISCONNECT", "切断", UI_BUTTON_PRIMARY, pressed(app, MODAL_LEFT));
        ui_button(MODAL_RIGHT, "QUIT GAME", "終了", UI_BUTTON_DANGER, pressed(app, MODAL_RIGHT));
        ui_offset(0.0f, 0.0f);
        return;
    }
    if (app->modal == MODAL_CONFLICT || app->modal == MODAL_LIMIT_WAIT) {
        const bool wait = app->modal == MODAL_LIMIT_WAIT, same = app->conflict_same_game;
        ui_button(MODAL_LEFT, wait ? "TRY NOW" : same ? "RESUME" : "END IT", wait ? "再試行" : same ? "再開" : "終了",
                  wait || same ? UI_BUTTON_PRIMARY : UI_BUTTON_DANGER, pressed(app, MODAL_LEFT));
        ui_button(MODAL_RIGHT, wait ? "STOP" : "BACK", wait ? "中止" : "戻る", UI_BUTTON_NORMAL,
                  pressed(app, MODAL_RIGHT));
        ui_offset(0.0f, 0.0f);
        return;
    }
    ui_button(MODAL_LEFT, resume ? "RESUME" : error ? "RETRY" : send ? "SEND" : share ? "SHARE" : "YES",
              resume ? "再開" : error ? "再試行" : send ? "送信" : share ? "協力" : "はい",
              app->modal == MODAL_EXIT || app->modal == MODAL_SIGN_OUT ? UI_BUTTON_DANGER
                                                                         : UI_BUTTON_PRIMARY,
              pressed(app, MODAL_LEFT));
    ui_button(MODAL_RIGHT, resume ? "END GAME" : error ? "BACK" : send ? "CANCEL" : share ? "NO THANKS" : "NO",
              resume ? "終了" : error ? "戻る" : send ? "取消" : share ? "不要" : "いいえ",
              resume ? UI_BUTTON_DANGER : UI_BUTTON_NORMAL, pressed(app, MODAL_RIGHT));
    ui_offset(0.0f, 0.0f);
}

static void draw_bottom_screen(const App *app);

void screens_draw_bottom(const App *app)
{
    push_service_accent(app);
    draw_bottom_screen(app);
    ui_pop_accent();
}

static void draw_bottom_screen(const App *app)
{
    /* The overlay id folds the modal, menu and controls sheet together so
     * any of them opening restarts the overlay fade. */
    const int overlay = app->modal != MODAL_NONE ? 10 + (int)app->modal :
                        app->controls_open ? 2 : app->stream_menu ? 1 : app->options_open ? 3 : 0;
    const float p = view_progress(&g_bottom_anim, (int)app->view, overlay);
    const float op = overlay_progress(&g_bottom_anim);
    g_bottom_busy_animating = p < 1.0f || (overlay && op < 1.0f) || welcome_visible(app) ||
                              (app->view == VIEW_SETTINGS && app->setting_index >= 0 &&
                               (screens_setting_at(app->setting_index) == SETTING_GYRO));
    const bool menus = app->view != VIEW_STREAM;
    /* The menus' backdrop: a quiet dot grid with a cross every 32 px, and
     * a glow of the theme colour fading down from the top. Drawn before the
     * slide offset, so it stays put while screens move over it. */
    if (menus) {
        ui_offset(0.0f, 0.0f);
        if (draw_painted_bottom(app)) {
        } else if (ui_backdrop()) {
            /* The theme's wallpaper; the header band is glass, so the status
             * line and titles read over any pattern. */
            const UiRect band = { 0, 0, UI_BOTTOM_WIDTH, 30 };
            ui_glass(band);
            ui_rect(0, 0, UI_BOTTOM_WIDTH, 30, UI_RGBA(0x00, 0x00, 0x00, 0x50));
            ui_hline(0, 30, UI_BOTTOM_WIDTH, UI_RGBA(0xFF, 0xFF, 0xFF, 0x30));
        } else {
            ui_gradient(0, 0, UI_BOTTOM_WIDTH, 110, ui_with_alpha(UI_ACCENT, 0x14), ui_with_alpha(UI_ACCENT, 0x00));
            ui_texture(UI_IMAGE_GRID, 0, 0, UI_BOTTOM_WIDTH, UI_HEIGHT, UI_RGBA(0xFF, 0xFF, 0xFF, 0x48));
        }
    }
    ui_offset(0.0f, (1.0f - p) * 8.0f);
    const bool guide = app->guide_page >= 0 && menus;
    if (app->whats_new_open && menus) draw_whats_new_bottom(app);
    else if (app->discord_open && menus) draw_discord_bottom(app);
    else if (guide) draw_guide_bottom(app);
    else if (app->update_open && menus) draw_update_bottom(app);
    else switch (app->view) {
    case VIEW_HUB: draw_hub_bottom(app); break;
    case VIEW_LOGIN: draw_login_bottom(app); break;
    case VIEW_LIBRARY:
        if (gfn_has_session(app->client)) draw_library_bottom(app);
        else draw_signin_bottom(app);
        break;
    case VIEW_SETTINGS: draw_settings_bottom(app); break;
    case VIEW_SESSION: draw_session_bottom(app); break;
    case VIEW_DETAILS: draw_details_bottom(app, op); break;
    case VIEW_STREAM: ui_offset(0.0f, 0.0f); draw_stream_bottom(app, op); break;
    }
    ui_offset(0.0f, 0.0f);
    fade_in_veil(UI_BOTTOM_WIDTH, 0.0f, p);
    if (app->modal != MODAL_NONE) draw_modal_bottom(app, op);
    if (app->busy) {
        ui_rect(0, 0, UI_BOTTOM_WIDTH, UI_HEIGHT, ui_with_alpha(UI_BG, 0xE8));
        const UiRect card = { 70, 64, 180, 112 };
        ui_rect_r(card, UI_SURFACE);
        ui_outline(card.x, card.y, card.w, card.h, 1.0f, UI_LINE_STRONG);
        ui_rect(card.x, card.y, card.w, 2, UI_ACCENT);
        ui_enso(160, 104, 18, UI_ACCENT);
        static const char *const hints[] = { "B", "Cancel", NULL };
        ui_hint_row(160, 142, hints);
    }
}

/* ---- Touch ---------------------------------------------------------------- */

uint16_t screens_stream_held_buttons(const App *app, int x, int y)
{
    if (app->view != VIEW_STREAM || app->keyboard_open || app->stream_menu || app->controls_open)
        return 0;
    const uint16_t both = GFN_PAD_LEFT_THUMB | GFN_PAD_RIGHT_THUMB;
    if (app->look_mode) {
        if (ui_hit(LOOK_BOTH, x, y)) return both;
        if (ui_hit(LOOK_L3, x, y)) return GFN_PAD_LEFT_THUMB;
        if (ui_hit(LOOK_R3, x, y)) return GFN_PAD_RIGHT_THUMB;
        if (ui_hit(LOOK_PS, x, y)) return GFN_PAD_GUIDE;
        return 0;
    }
    if (ui_hit(STR_L3, x, y)) return GFN_PAD_LEFT_THUMB;
    if (ui_hit(STR_R3, x, y)) return GFN_PAD_RIGHT_THUMB;
    if (ui_hit(STR_GUIDE, x, y)) return GFN_PAD_GUIDE;
    /* The stats panel, unless it is the mouse pad or the zoom map. */
    if (!app->transport->pointer_mode && !mvd_video_zoomed() && ui_hit(STR_PANEL, x, y)) return both;
    return 0;
}

static AppAction mapping_touch(int x, int y)
{
    g_touched_map_field = -1;
    for (int f = 0; f < 3; ++f) {
        if (!ui_hit(map_row(f), x, y)) continue;
        g_touched_map_field = f;
        if (ui_hit(map_prev(f), x, y)) return ACTION_MAP_PREV;
        if (ui_hit(map_next(f), x, y)) return ACTION_MAP_NEXT;
        return ACTION_MAP_FIELD;
    }
    if (ui_hit(MAP_RESET, x, y)) return ACTION_MAP_RESET;
    if (ui_hit(MAP_CANCEL, x, y)) return ACTION_MAP_CANCEL;
    if (ui_hit(MAP_DONE, x, y)) return ACTION_MAP_DONE;
    return ACTION_NONE;
}

AppAction screens_touch(const App *app, int x, int y)
{
    if (app->busy) return ACTION_NONE;
    g_touched_option_row = -1;
    if (app->whats_new_open && app->view != VIEW_STREAM)
        return ui_hit(NEW_CONTINUE, x, y) ? ACTION_WHATS_NEW_CLOSE : ACTION_NONE;
    if (app->discord_open && app->view != VIEW_STREAM)
        return ui_hit(NEW_CONTINUE, x, y) ? ACTION_DISCORD_CLOSE : ACTION_NONE;
    if (app->update_open && app->view != VIEW_STREAM && app->guide_page < 0) {
        if (ui_hit(UPD_PRIMARY, x, y)) return ACTION_UPDATE_PRIMARY;
        if (updater_info().state == UPDATE_AVAILABLE) {
            if (ui_hit(UPD_LATER, x, y)) return ACTION_UPDATE_LATER;
            if (ui_hit(UPD_CLOSE, x, y)) return ACTION_UPDATE_CLOSE;
        } else if (ui_hit((UiRect){ 90, 200, 140, 34 }, x, y)) {
            return ACTION_UPDATE_CLOSE;
        }
        return ACTION_NONE;
    }
    if (app->guide_page >= 0 && app->view != VIEW_STREAM) {
        if (ui_hit(GUIDE_BACK, x, y)) return ACTION_GUIDE_BACK;
        if (ui_hit(GUIDE_SKIP, x, y)) return ACTION_GUIDE_SKIP;
        if (ui_hit(GUIDE_NEXT, x, y)) return ACTION_GUIDE_NEXT;
        return ACTION_NONE;
    }
    if (app->modal != MODAL_NONE) {
        if (app->modal == MODAL_REPORT_SENT) return ui_hit(MODAL_ONLY, x, y) ? ACTION_CONFIRM : ACTION_NONE;
        if (ui_hit(MODAL_LEFT, x, y)) return app->modal == MODAL_ERROR ? ACTION_RETRY : ACTION_CONFIRM;
        if (app->modal != MODAL_REPORT_SENT && ui_hit(MODAL_RIGHT, x, y)) return ACTION_DISMISS;
        return ACTION_NONE;
    }
    switch (app->view) {
    case VIEW_HUB:
        if (hub_last_game(app) && ui_hit(HOME_PANEL, x, y)) return ACTION_CONTINUE;
        if (ui_hit(HOME_PANEL, x, y)) {
            g_touched_service = app->hub_index;
            return ACTION_SERVICE;
        }
        for (int i = 0; i < SERVICE_COUNT; ++i)
            if (ui_hit(home_tile(i), x, y)) {
                g_touched_service = i;
                return ACTION_SERVICE;
            }
        if (ui_hit(HUB_SETTINGS, x, y)) return ACTION_SETTINGS;
        if (ui_hit(HUB_EXIT, x, y)) return ACTION_EXIT;
        break;
    case VIEW_LOGIN:
        if (ui_hit(PAIR_LEFT, x, y)) return ACTION_NEW_CODE;
        if (ui_hit(PAIR_RIGHT, x, y)) return ACTION_CANCEL;
        break;
    case VIEW_LIBRARY:
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
        if (ui_hit(HOME_BACK, x, y) && !app->toast) return ACTION_HUB;
        if (!gfn_has_session(app->client)) {
            if (ui_hit(SIGNIN_MAIN, x, y)) return ACTION_SIGN_IN;
            if (ui_hit(SIGNIN_HUB, x, y)) return ACTION_HUB;
            if (ui_hit(SIGNIN_SETTINGS, x, y)) return ACTION_SETTINGS;
            break;
        }
        const LibraryLayout *l = library_layout(app);
        if (l == &LIB_COMPACT && ui_hit(l->cont, x, y)) return ACTION_CONTINUE;
        if (ui_hit(l->prev, x, y)) return ACTION_PREV;
        if (ui_hit(l->next, x, y)) return ACTION_NEXT;
        if (ui_hit(l->play, x, y))
            return app->list_count ? ACTION_PLAY : ACTION_LIBRARY;
        if (ui_hit(l->library, x, y)) return ACTION_LIBRARY;
        if (ui_hit(l->search, x, y)) return ACTION_SEARCH;
        if (ui_hit(l->settings, x, y)) return ACTION_SETTINGS;
    }
        break;
    case VIEW_SETTINGS:
        if (app->mapping_open) return mapping_touch(x, y);
        if (app->gallery_open) {
            if (ui_hit(SET_PREV, x, y)) return ACTION_GALLERY_PREV;
            if (ui_hit(SET_NEXT, x, y)) return ACTION_GALLERY_NEXT;
            if (ui_hit(GAL_DELETE, x, y)) return ACTION_GALLERY_DELETE;
            if (ui_hit(GAL_BACK, x, y)) return ACTION_GALLERY_CLOSE;
            return ACTION_NONE;
        }
        if (app->settings_section < 0) {
            for (int i = 0; i < screens_section_count() && i < 6; ++i)
                if (ui_hit(set_tile(i), x, y)) {
                    g_touched_section = i;
                    return ACTION_SETTINGS_SECTION;
                }
            return ui_hit(SET_BACK, x, y) ? ACTION_BACK : ACTION_NONE;
        }
        if (ui_hit(SET_PREV, x, y)) return ACTION_VALUE_PREV;
        if (ui_hit(SET_NEXT, x, y)) return ACTION_VALUE_NEXT;
        if (ui_hit(SET_BACK, x, y)) return ACTION_BACK;
        break;
    case VIEW_SESSION:
        if (screens_session_failed(app) && !app->waiting_wifi && (app->reconnect_attempt == 0 || app->reconnect_attempt > 3)) {
            if (ui_hit(PAIR_LEFT, x, y)) return ACTION_RETRY;
            if (ui_hit(PAIR_RIGHT, x, y)) return ACTION_CANCEL;
        } else if (ui_hit(SINGLE, x, y)) {
            return ACTION_CANCEL;
        }
        break;
    case VIEW_DETAILS:
        if (app->shortcut_sheet == SHORTCUT_SHEET_WORKING) return ACTION_NONE;
        if (app->shortcut_sheet == SHORTCUT_SHEET_FAILED) {
            if (ui_hit(PAIR_LEFT, x, y)) return ACTION_RETRY;
            if (ui_hit(PAIR_RIGHT, x, y)) return ACTION_DISMISS;
            return ACTION_NONE;
        }
        if (app->shortcut_sheet != SHORTCUT_SHEET_NONE) return ui_hit(SINGLE, x, y) ? ACTION_CONFIRM : ACTION_NONE;
        if (app->mapping_open) return mapping_touch(x, y);
        if (app->options_open) {
            if (ui_hit(OPT_CLOSE, x, y)) return ACTION_OPTIONS_CLOSE;
            for (int i = 0; i < OPTION_COUNT; ++i) {
                const UiRect row = { 12, OPT_ROW_Y + i * OPT_ROW_H, 296, OPT_ROW_H - 4 };
                if (!ui_hit(row, x, y)) continue;
                g_touched_option_row = i;
                return x < 160 && i != OPTION_CONNECTION ? ACTION_OPTION_PREV : ACTION_OPTION_NEXT;
            }
            break;
        }
        if (ui_hit(DET_PLAY, x, y)) return ACTION_DETAILS_PLAY;
        if (ui_hit(DET_STORE_PREV, x, y)) return ACTION_VARIANT_PREV;
        if (ui_hit(DET_STORE_NEXT, x, y)) return ACTION_VARIANT_NEXT;
        if (ui_hit(DET_FAV, x, y)) return ACTION_FAVOURITE;
        if (ui_hit(DET_OPTIONS, x, y)) return ACTION_OPTIONS;
        if (ui_hit(DET_SHORTCUT, x, y)) return ACTION_SHORTCUT;
        if (ui_hit(DET_BACK, x, y)) return ACTION_BACK;
        break;
    case VIEW_STREAM:
        if (app->controls_open) return ACTION_CONTROLS_CLOSE;
        if (app->stream_menu) {
            for (int i = 0; i < STREAM_MENU_COUNT; ++i)
                if (ui_hit(menu_item(i), x, y)) return (AppAction)(ACTION_MENU_RESUME + i);
            if (!ui_hit(MENU_PANEL, x, y)) return ACTION_MENU_RESUME;
            break;
        }
        if (app->look_mode && ui_hit(LOOK_HIDE, x, y)) return ACTION_LOOK_TOGGLE;
        if (app->mic_available && ui_hit(app->look_mode ? LOOK_MIC : STR_MIC, x, y)) return ACTION_MIC_TOGGLE;
        if (app->look_available && !app->look_mode && ui_hit(STR_LOOK, x, y)) return ACTION_LOOK_TOGGLE;
        if (ui_hit(stream_button(0), x, y)) return ACTION_STREAM_KEYBOARD;
        if (ui_hit(stream_button(1), x, y)) return ACTION_STREAM_POINTER;
        if (ui_hit(stream_button(2), x, y)) return ACTION_STREAM_ZOOM;
        if (ui_hit(stream_button(3), x, y)) return ACTION_STREAM_MENU;
        break;
    }
    return ACTION_NONE;
}

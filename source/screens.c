#include "app.h"
#include "app_paths.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "game_art.h"
#include "game_prefs.h"
#include "mvd_video.h"
#include "play_history.h"
#include "zoom_zones.h"
#include "remote_keyboard.h"
#include "stream_profile.h"

/* ---- Shared geometry (drawing and hit-testing use the same rects) -------- */

/* Library, lower screen. */
static const UiRect LIB_PREV = { 8, 34, 30, 84 };
static const UiRect LIB_NEXT = { 282, 34, 30, 84 };
static const UiRect LIB_PLAY = { 16, 128, 288, 46 };
static const UiRect LIB_LIBRARY = { 16, 184, 90, 48 };
static const UiRect LIB_SEARCH = { 115, 184, 90, 48 };
static const UiRect LIB_SETTINGS = { 214, 184, 90, 48 };

/* Welcome. */
static const UiRect WEL_SIGN_IN = { 40, 100, 240, 50 };
static const UiRect WEL_SETTINGS = { 40, 160, 116, 42 };
static const UiRect WEL_EXIT = { 164, 160, 116, 42 };

/* Login and session: two mirrored buttons, or one centred. */
static const UiRect PAIR_LEFT = { 16, 186, 140, 44 };
static const UiRect PAIR_RIGHT = { 164, 186, 140, 44 };
static const UiRect SINGLE = { 60, 186, 200, 44 };

/* Settings. */
static const UiRect SET_PREV = { 16, 188, 60, 44 };
static const UiRect SET_BACK = { 84, 188, 152, 44 };
static const UiRect SET_NEXT = { 244, 188, 60, 44 };

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
static const UiRect MENU_PANEL = { 12, 6, 296, 228 };

/* Game details, lower screen. */
static const UiRect DET_PLAY = { 16, 34, 288, 58 };
static const UiRect DET_STORE_PREV = { 16, 106, 44, 44 };
static const UiRect DET_STORE_NEXT = { 260, 106, 44, 44 };
static const UiRect DET_FAV = { 16, 188, 92, 44 };
static const UiRect DET_OPTIONS = { 114, 188, 92, 44 };
static const UiRect DET_BACK = { 212, 188, 92, 44 };
static const UiRect OPT_CLOSE = { 84, 194, 152, 40 };
#define OPT_ROW_Y 40.0f
#define OPT_ROW_H 36.0f

/* Guide, lower screen. */
static const UiRect GUIDE_BACK = { 16, 188, 92, 44 };
static const UiRect GUIDE_SKIP = { 114, 188, 92, 44 };
static const UiRect GUIDE_NEXT = { 212, 188, 92, 44 };

/* Which options row a tap landed on (read by main with the action). */
static int g_touched_option_row = -1;
int screens_touched_option_row(void) { return g_touched_option_row; }

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
    const u64 now = osGetTime();
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

/* ---- Settings model ------------------------------------------------------ */

/* The settings list is grouped: each entry is a section heading (setting
 * -1) or a setting. Navigation walks settings only. */
typedef struct {
    int setting;
    const char *jp;
    const char *en;
} SettingEntry;

static const SettingEntry SETTING_ENTRIES[] = {
    { -1, "操作", "CONTROLS" },
    { SETTING_LAYOUT, NULL, NULL },
    { SETTING_TRIGGERS, NULL, NULL },
    { SETTING_DEADZONE, NULL, NULL },
    { SETTING_GYRO, NULL, NULL },
    { SETTING_GYRO_SPEED, NULL, NULL },
    { SETTING_FAST_INPUT, NULL, NULL },
    { -1, "画質", "PICTURE" },
    { SETTING_RESOLUTION, NULL, NULL },
    { SETTING_BITRATE, NULL, NULL },
    { SETTING_FILTER, NULL, NULL },
    { SETTING_STATS, NULL, NULL },
    { -1, "音声", "AUDIO" },
    { SETTING_VOLUME, NULL, NULL },
    { SETTING_MENU_AUDIO, NULL, NULL },
    { -1, "外観", "APPEARANCE" },
    { SETTING_THEME, NULL, NULL },
    { -1, "本体", "SYSTEM" },
    { SETTING_CONNECTION, NULL, NULL },
    { SETTING_LID, NULL, NULL },
    { SETTING_POINTER, NULL, NULL },
    { SETTING_GUIDE, NULL, NULL },
    { -1, "アカウント", "ACCOUNT" },
    { SETTING_ACCOUNT, NULL, NULL },
};
#define SETTING_ENTRY_COUNT (int)(sizeof(SETTING_ENTRIES) / sizeof(SETTING_ENTRIES[0]))

int screens_setting_at(int position)
{
    int seen = 0;
    for (int i = 0; i < SETTING_ENTRY_COUNT; ++i) {
        if (SETTING_ENTRIES[i].setting < 0) continue;
        if (seen++ == position) return SETTING_ENTRIES[i].setting;
    }
    return SETTING_ACCOUNT;
}

static const char *const SETTING_LABELS[SETTING_COUNT] = {
    [SETTING_LAYOUT] = "Button layout", [SETTING_TRIGGERS] = "Triggers",
    [SETTING_DEADZONE] = "Stick deadzone", [SETTING_POINTER] = "Pointer start",
    [SETTING_STATS] = "Stream stats", [SETTING_FAST_INPUT] = "Fast input",
    [SETTING_RESOLUTION] = "Screen mode", [SETTING_BITRATE] = "Bitrate",
    [SETTING_FILTER] = "Encoder filter", [SETTING_GYRO] = "Gyro aim",
    [SETTING_GYRO_SPEED] = "Gyro speed", [SETTING_ACCOUNT] = "NVIDIA account",
    [SETTING_THEME] = "Theme", [SETTING_VOLUME] = "Stream volume",
    [SETTING_MENU_AUDIO] = "Audio in menus", [SETTING_LID] = "Closing the lid",
    [SETTING_CONNECTION] = "Connection check", [SETTING_GUIDE] = "Getting started",
};
static const char *const SETTING_JP[SETTING_COUNT] = {
    [SETTING_LAYOUT] = "ボタン配置", [SETTING_TRIGGERS] = "トリガー",
    [SETTING_DEADZONE] = "デッドゾーン", [SETTING_POINTER] = "ポインタ",
    [SETTING_STATS] = "統計", [SETTING_FAST_INPUT] = "高速入力",
    [SETTING_RESOLUTION] = "表示", [SETTING_BITRATE] = "ビットレート",
    [SETTING_FILTER] = "補正", [SETTING_GYRO] = "ジャイロ",
    [SETTING_GYRO_SPEED] = "感度", [SETTING_ACCOUNT] = "アカウント",
    [SETTING_THEME] = "色", [SETTING_VOLUME] = "音量",
    [SETTING_MENU_AUDIO] = "メニュー音", [SETTING_LID] = "スリープ",
    [SETTING_CONNECTION] = "接続", [SETTING_GUIDE] = "案内",
};

static const char *connection_advice(const GfnClient *c);

static const char *gyro_mode_name(GfnGyroMode mode)
{
    return mode == GFN_GYRO_ALWAYS ? "Always" : mode == GFN_GYRO_WHILE_AIMING ? "While aiming" : "Off";
}

/* Current option and option count, for the dot indicator. */
static unsigned setting_option(const App *app, int setting, unsigned *count)
{
    const AppSettings *s = &app->settings;
    switch (setting) {
    case SETTING_LAYOUT: *count = 2; return s->button_layout == GFN_LAYOUT_POSITION ? 0 : 1;
    case SETTING_TRIGGERS: *count = 2; return s->swap_shoulders ? 1 : 0;
    case SETTING_DEADZONE: *count = DEADZONE_COUNT; return (unsigned)s->deadzone;
    case SETTING_POINTER: *count = 2; return s->auto_pointer ? 0 : 1;
    case SETTING_STATS: *count = 2; return s->show_stats ? 0 : 1;
    case SETTING_FAST_INPUT: *count = 2; return s->fast_input ? 1 : 0;
    case SETTING_RESOLUTION: *count = 2; return s->wide_video ? 0 : 1;
    case SETTING_BITRATE: *count = STREAM_BITRATE_COUNT; return (unsigned)s->bitrate_mode;
    case SETTING_FILTER: *count = 2; return s->sharpen ? 1 : 0;
    case SETTING_GYRO: *count = GFN_GYRO_MODE_COUNT; return (unsigned)s->gyro_mode;
    case SETTING_GYRO_SPEED: *count = 3; return s->gyro_speed;
    case SETTING_THEME: *count = UI_THEME_COUNT; return s->theme;
    case SETTING_VOLUME: *count = 6; return s->volume;
    case SETTING_MENU_AUDIO: *count = 2; return s->mute_in_menus ? 1 : 0;
    case SETTING_LID: *count = 2; return s->lid_keeps_playing ? 1 : 0;
    default: *count = 0; return 0;
    }
}

static const char *setting_value(const App *app, int setting)
{
    const AppSettings *s = &app->settings;
    switch (setting) {
    case SETTING_LAYOUT: return s->button_layout == GFN_LAYOUT_POSITION ? "Position" : "Letters";
    case SETTING_TRIGGERS: return s->swap_shoulders ? "L / R" : "ZL / ZR";
    case SETTING_DEADZONE:
        return s->deadzone == DEADZONE_SMALL ? "Small" : s->deadzone == DEADZONE_LARGE ? "Large" : "Medium";
    case SETTING_POINTER: return s->auto_pointer ? "Genshin" : "Never";
    case SETTING_STATS: return s->show_stats ? "On" : "Off";
    case SETTING_FAST_INPUT: return s->fast_input ? "On (beta)" : "Off";
    case SETTING_RESOLUTION: return s->wide_video ? "Wide 800" : "Classic 400";
    case SETTING_BITRATE: {
        static const char *const names[STREAM_BITRATE_COUNT] = {
            "Adaptive", "Steady 1 Mbps", "Steady 1.2 Mbps", "Steady 1.5 Mbps"
        };
        return names[s->bitrate_mode];
    }
    case SETTING_FILTER: return s->sharpen ? "Sharpen" : "Clean";
    case SETTING_GYRO: return gyro_mode_name(s->gyro_mode);
    case SETTING_GYRO_SPEED: return s->gyro_speed == 0 ? "Low" : s->gyro_speed == 2 ? "High" : "Medium";
    case SETTING_THEME: return ui_theme_name((UiTheme)s->theme);
    case SETTING_VOLUME: {
        static const char *const levels[6] = { "Muted", "20 %", "40 %", "60 %", "80 %", "100 %" };
        return levels[s->volume < 6 ? s->volume : 5];
    }
    case SETTING_MENU_AUDIO: return s->mute_in_menus ? "Muted" : "Keep playing";
    case SETTING_LID: return s->lid_keeps_playing ? "Keep streaming" : "Pause & resume";
    case SETTING_CONNECTION: {
        static char result[48];
        const GfnClient *c = app->client;
        if (!c->conn_tested_at) return "Run test";
        snprintf(result, sizeof(result), "%u ms · %u.%u Mbps", c->conn_latency_ms,
                 c->conn_kbps / 1000, c->conn_kbps % 1000 / 100);
        return result;
    }
    case SETTING_GUIDE: return "Open";
    case SETTING_ACCOUNT: return gfn_has_session(app->client) ? "Sign out" : "Signed out";
    }
    return "";
}

static const char *setting_description(const App *app, int setting)
{
    const AppSettings *s = &app->settings;
    switch (setting) {
    case SETTING_LAYOUT:
        return s->button_layout == GFN_LAYOUT_POSITION
            ? "Buttons match their place on the pad: bottom is Cross, right is Circle. Plays like a PlayStation controller."
            : "The printed letters match: 3DS A sends A. Cross and Circle end up swapped compared with a PlayStation pad.";
    case SETTING_TRIGGERS:
        return s->swap_shoulders
            ? "The big L and R buttons act as the L2 / R2 triggers; ZL and ZR become L1 / R1."
            : "ZL and ZR are the L2 / R2 triggers; L and R are the L1 / R1 bumpers.";
    case SETTING_DEADZONE:
        return "How far a stick moves before the game notices. Raise it if a character drifts on its own.";
    case SETTING_POINTER:
        return "Start Genshin Impact in pointer mode so you can click through its PC login screen.";
    case SETTING_STATS:
        return "Show frame rate, bitrate, ping and resent packets on the lower screen while playing.";
    case SETTING_FAST_INPUT:
        return "Beta. Sends controller input on a channel that never waits for lost packets. If buttons stop responding in game, turn it off.";
    case SETTING_RESOLUTION:
        return s->wide_video
            ? "Uses the top screen's 800-pixel mode: twice the detail across, sharper text."
            : "The normal 400-column top screen. Use only if Wide misbehaves.";
    case SETTING_BITRATE:
        return s->bitrate_mode == STREAM_BITRATE_ADAPTIVE
            ? "NVIDIA moves the rate between 1 and 1.8 Mbps and backs off when Wi-Fi loses packets. Next launch."
            : s->bitrate_mode == STREAM_BITRATE_STEADY_1000
            ? "Smoothest: the rate 3DS Wi-Fi carries with almost no lost packets. Slightly softer picture. Next launch."
            : "Sharper, but 3DS Wi-Fi starts losing packets here. If RESENT/S climbs, step down. Next launch.";
    case SETTING_FILTER:
        return s->sharpen
            ? "NVIDIA sharpens before encoding. Crisper edges, but at this bitrate it costs detail elsewhere. Next launch."
            : "No server sharpening: the bitrate goes to the picture itself, so less blocking and pulsing. Next launch.";
    case SETTING_GYRO:
        return s->gyro_mode == GFN_GYRO_OFF
            ? "Tilt and turn the console to aim, like a Switch or Steam Deck. Adds to the C-Stick; moves the pointer in pointer mode."
            : s->gyro_mode == GFN_GYRO_ALWAYS
            ? "Turning the console always moves the camera. Great for shooters; hold the console still when you don't aim."
            : "Gyro only works while the aim trigger (ZL, or L when triggers are swapped) is held.";
    case SETTING_GYRO_SPEED:
        return "How fast turning the console moves the camera. Start at Medium and lower it if aiming overshoots.";
    case SETTING_THEME:
        return "The accent colour: Seiji celadon, Sakura cherry, Kin gold, Ai indigo or Fuji wisteria.";
    case SETTING_VOLUME:
        return "Game audio volume on this console, on top of the 3DS volume slider.";
    case SETTING_MENU_AUDIO:
        return s->mute_in_menus
            ? "Game audio goes quiet while the stream menu or controls sheet is open."
            : "Game audio keeps playing while the stream menu is open.";
    case SETTING_CONNECTION: return connection_advice(app->client);
    case SETTING_GUIDE: return "Walk through the basics again: signing in, controls, picture and extras.";
    case SETTING_LID:
        return s->lid_keeps_playing
            ? "Closing the lid only turns the screens off; the stream keeps running (and using battery)."
            : "Closing the lid sleeps the console. On opening it, Kasumi reconnects to the same rig if NVIDIA still holds it.";
    case SETTING_ACCOUNT:
        return "Remove the saved NVIDIA login from this console's SD card.";
    }
    return "";
}

void screens_setting_change(App *app, int setting, int direction)
{
    AppSettings *s = &app->settings;
    const int step = direction < 0 ? -1 : 1;
    switch (setting) {
    case SETTING_LAYOUT:
        s->button_layout = s->button_layout == GFN_LAYOUT_POSITION ? GFN_LAYOUT_LABEL
                                                                   : GFN_LAYOUT_POSITION;
        break;
    case SETTING_TRIGGERS: s->swap_shoulders = !s->swap_shoulders; break;
    case SETTING_DEADZONE:
        s->deadzone = (DeadzoneLevel)((s->deadzone + DEADZONE_COUNT + step) % DEADZONE_COUNT);
        break;
    case SETTING_POINTER: s->auto_pointer = !s->auto_pointer; break;
    case SETTING_STATS: s->show_stats = !s->show_stats; break;
    case SETTING_FAST_INPUT: s->fast_input = !s->fast_input; break;
    case SETTING_RESOLUTION: s->wide_video = !s->wide_video; break;
    case SETTING_FILTER: s->sharpen = !s->sharpen; break;
    case SETTING_GYRO:
        s->gyro_mode = (GfnGyroMode)((s->gyro_mode + GFN_GYRO_MODE_COUNT + step) % GFN_GYRO_MODE_COUNT);
        break;
    case SETTING_GYRO_SPEED: s->gyro_speed = (s->gyro_speed + 3 + step) % 3; break;
    case SETTING_THEME: s->theme = (s->theme + UI_THEME_COUNT + step) % UI_THEME_COUNT; break;
    case SETTING_VOLUME: s->volume = (s->volume + 6 + step) % 6; break;
    case SETTING_MENU_AUDIO: s->mute_in_menus = !s->mute_in_menus; break;
    case SETTING_LID: s->lid_keeps_playing = !s->lid_keeps_playing; break;
    case SETTING_BITRATE:
        s->bitrate_mode = (StreamBitrateMode)((s->bitrate_mode + STREAM_BITRATE_COUNT + step) %
                                              STREAM_BITRATE_COUNT);
        break;
    default: break;
    }
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
    ui_text(width - 12.0f, 5.0f, 12.0f, UI_TEXT, UI_ALIGN_RIGHT, clock);

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
    ui_rect_r(r, is_pressed ? UI_RAISED : UI_BG);
    ui_outline(r.x, r.y, r.w, r.h, 1.0f, enabled ? UI_LINE_STRONG : UI_LINE);
    const float cx = r.x + r.w / 2 + (is_pressed ? (float)direction : 0.0f), cy = r.y + r.h / 2;
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

static void draw_welcome_backdrop(void)
{
    if (!ui_image(UI_IMAGE_HERO, 0, 0, 1.0f, 1.0f)) {
        ui_seigaiha(0, 168, UI_TOP_WIDTH, 80, 22, C2D_Color32(0x1C, 0x1C, 0x22, 0xFF), UI_BG);
        C2D_DrawRectangle(0, 160, 0, UI_TOP_WIDTH, 60, UI_BG, UI_BG,
                          ui_with_alpha(UI_BG, 0), ui_with_alpha(UI_BG, 0));
    }
    draw_mist(118, 0.55f);
}

static void draw_welcome_top(void)
{
    /* The hero art leaves the upper centre empty for the mark. A slow
     * breath on the seal keeps the screen alive without distracting. */
    const float breath = 0.5f + 0.5f * sinf((float)ui_ticks() / 2600.0f * 2.0f * (float)M_PI);
    ui_circle(200, 54, 26 + breath * 2.0f, ui_with_alpha(UI_ACCENT, (u8)(18 + breath * 14)));
    ui_seal(180, 34, 40);
    ui_text(200, 80, 26, UI_TEXT, UI_ALIGN_CENTER, APP_NAME);
    ui_text(200, 109, 12, UI_ACCENT, UI_ALIGN_CENTER, "クラウドゲーム");
    ui_label(200, 125, 11, UI_TEXT_DIM, UI_ALIGN_CENTER, "GEFORCE NOW  ·  NEW 3DS");
    static const char *const hints[] = { "A", "Sign in", "SELECT", "Settings", "START", "Exit", NULL };
    ui_rect(0, 216, UI_TOP_WIDTH, 24, ui_with_alpha(UI_BG, 0xB0));
    ui_hint_row(200, 221, hints);
}

static void draw_login_top(const App *app)
{
    const GfnClient *client = app->client;
    draw_title(200, 34, "サインイン", "SIGN IN WITH NVIDIA");
    ui_text(200, 68, 11, UI_TEXT_DIM, UI_ALIGN_CENTER, "On your phone or computer, open");
    ui_text_fit(200, 83, 14, UI_TEXT, UI_ALIGN_CENTER, 368, client->verification_uri);

    /* One cell per character of the code; the cells drop in one by one. */
    const size_t length = strlen(client->user_code);
    const float cell = 26.0f, gap = 5.0f;
    float total = 0;
    for (size_t i = 0; i < length; ++i)
        total += (client->user_code[i] == '-' ? 10.0f : cell) + (i + 1 < length ? gap : 0);
    float x = 200 - total / 2;
    for (size_t i = 0; i < length; ++i) {
        char glyph[2] = { client->user_code[i], 0 };
        if (glyph[0] == '-') {
            ui_rect(x + 2, 128, 6, 2, UI_TEXT_FAINT);
            x += 10.0f + gap;
            continue;
        }
        ui_rect(x, 108, cell, 38, UI_SURFACE);
        ui_outline(x, 108, cell, 38, 1.0f, UI_LINE_STRONG);
        ui_rect(x, 144, cell, 2, UI_ACCENT);
        ui_text(x + cell / 2, 114, 22, UI_TEXT, UI_ALIGN_CENTER, glyph);
        x += cell + gap;
    }

    const char *waiting = "Waiting for approval";
    const float w = ui_text_width(waiting, 11);
    ui_enso(200 - w / 2 - 12, 172, 6, UI_ACCENT);
    ui_text(200 - w / 2 + 2, 166, 11, UI_TEXT_DIM, UI_ALIGN_LEFT, waiting);
    const long remaining = (long)(client->challenge_expires_at - (int64_t)time(NULL));
    if (remaining > 0)
        ui_textf(200, 186, 11, remaining < 60 ? UI_KIN : UI_TEXT_FAINT, UI_ALIGN_CENTER,
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
    game_art_want(game);
    if (!game_art_draw(game, x, y, scale, alpha)) draw_art_placeholder(game, x, y, w, h);
    ui_outline(x - 1, y - 1, w + 2, h + 2, 1.0f, UI_LINE_STRONG);
}

static void draw_library_art(const App *app)
{
    /* Cross-fade when the selection changes. */
    static const GfnGame *shown;
    static u64 changed_at;
    const GfnGame *game = app_game(app, app->selected);
    if (!game) return;
    if (game != shown) {
        shown = game;
        changed_at = osGetTime();
    }
    const float t = ui_ease_out(ui_progress(changed_at, 200.0f));
    ui_rect(LIB_ART_X - 6, LIB_ART_Y - 4, GAME_ART_WIDTH + 12, GAME_ART_HEIGHT + 30,
            UI_SURFACE);
    draw_game_art(game, LIB_ART_X, LIB_ART_Y + (1.0f - t) * 4.0f, GAME_ART_WIDTH, 0.35f + 0.65f * t);
    ui_rect(LIB_ART_X + GAME_ART_WIDTH / 2 - 12, LIB_ART_Y + GAME_ART_HEIGHT + 6, 24, 1, UI_ACCENT);
    ui_text_fit(LIB_ART_X + GAME_ART_WIDTH / 2, LIB_ART_Y + GAME_ART_HEIGHT + 11, 11, UI_TEXT_DIM,
                UI_ALIGN_CENTER, GAME_ART_WIDTH + 8, game->store[0] ? game->store : "GeForce NOW");
    /* Warm the neighbours so scrolling feels instant. */
    if (app->selected > 0) game_art_want(app_game(app, app->selected - 1));
    if (app->selected + 1 < app->list_count) game_art_want(app_game(app, app->selected + 1));
}

/* ALL / FAV / RECENT, switched with L and R. */
static void draw_library_tabs(const App *app)
{
    static const char *const names[LIBRARY_TAB_COUNT] = { "ALL", "FAV", "RECENT" };
    float x = 18;
    for (int i = 0; i < LIBRARY_TAB_COUNT; ++i) {
        const bool on = i == app->library_tab;
        ui_label(x, 40, 11, on ? UI_ACCENT : UI_TEXT_FAINT, UI_ALIGN_LEFT, names[i]);
        const float w = ui_text_width(names[i], 11) + 4;
        if (on) ui_rect(x, 53, w, 2, UI_ACCENT);
        x += w + 12;
    }
}

static void draw_library_top(const App *app, bool entering)
{
    static float bar_y;
    const bool searching = app->search_text[0] != '\0';
    const size_t count = app->list_count;
    draw_title(200, 31, searching ? "検索" : "ライブラリ", searching ? "SEARCH" : "LIBRARY");
    if (searching) {
        char query[96];
        snprintf(query, sizeof(query), "\"%s\"", app->search_text);
        ui_text_fit(18, 40, 11, UI_TEXT_DIM, UI_ALIGN_LEFT, 120, query);
    } else if (app->client->game_count) {
        draw_library_tabs(app);
    } else {
        ui_label(18, 41, 11, UI_TEXT_FAINT, UI_ALIGN_LEFT, "YOUR GAMES");
    }
    if (count)
        ui_textf(382, 40, 11, UI_TEXT_DIM, UI_ALIGN_RIGHT, "%02lu / %02lu",
                 (unsigned long)(app->selected + 1), (unsigned long)count);
    ui_hline(16, 57, 368, UI_LINE);
    if (count) draw_library_art(app);

    if (!count) {
        const char *title = "No games here yet";
        const char *hint = searching ? "Try a different search." : "Press Y to load your library, or X to search.";
        if (!searching && app->client->game_count && app->library_tab == LIBRARY_TAB_FAVOURITES) {
            title = "No favourites yet";
            hint = "Open a game and press Y to add it here.";
        } else if (!searching && app->client->game_count && app->library_tab == LIBRARY_TAB_RECENT) {
            title = "Nothing played on Kasumi yet";
            hint = "Games you play show up here, newest first.";
        }
        if (!ui_image(UI_IMAGE_LANTERN, 164, 66, 0.75f, 1.0f)) {
            ui_ring(200, 110, 26, 1.5f, UI_LINE_STRONG, UI_BG);
            ui_text(200, 100, 18, UI_TEXT_FAINT, UI_ALIGN_CENTER, "空");
        }
        ui_text(200, 146, 13, UI_TEXT, UI_ALIGN_CENTER, title);
        ui_text(200, 164, 11, UI_TEXT_DIM, UI_ALIGN_CENTER, hint);
    } else {
        const float row_h = 25.0f;
        const float target = 61.0f + (float)(app->selected - app->list_top) * row_h;
        bar_y = entering || bar_y == 0.0f ? target : ui_approach(bar_y, target, 22.0f);
        ui_rect(14, bar_y, LIB_LIST_W, 24, UI_RAISED);
        ui_rect(14, bar_y, 2, 24, UI_ACCENT);
    }

    for (size_t row = 0; row < LIBRARY_ROWS; ++row) {
        const size_t index = app->list_top + row;
        const GfnGame *game = app_game(app, index);
        if (!game) break;
        const float y = 61.0f + row * 25.0f;
        const bool selected = index == app->selected;
        game_art_want(game);
        if (row > 0 && !selected && index != app->selected + 1)
            ui_hline(44, y - 1, LIB_LIST_W - 36, C2D_Color32(0x16, 0x16, 0x1A, 0xFF));
        ui_textf(36, y + 7, 11, selected ? UI_ACCENT : UI_TEXT_FAINT, UI_ALIGN_RIGHT,
                 "%02lu", (unsigned long)(index + 1));
        /* Favourites carry a small accent mark before the title. */
        const bool favourite = game_prefs_favourite(game->app_id);
        if (favourite) ui_rounded(42, y + 9, 5, 5, 2.5f, UI_ACCENT);
        const float title_x = favourite ? 51.0f : 44.0f;
        const char *store = game->store[0] ? game->store : "GFN";
        const float store_w = ui_text_width(store, 11) + 12;
        ui_text_fit(title_x, y + 5, 13, selected ? UI_TEXT : UI_TEXT_DIM, UI_ALIGN_LEFT,
                    LIB_LIST_W - title_x - store_w - 4, game->title);
        ui_pill(10 + LIB_LIST_W, y + 4, selected ? UI_TEXT_DIM : UI_LINE_STRONG, UI_ALIGN_RIGHT, store);
    }

    /* Scroll rail. */
    if (count > LIBRARY_ROWS) {
        const float rail_y = 61, rail_h = 149;
        const float thumb_h = rail_h * LIBRARY_ROWS / (float)count;
        const float thumb_y = rail_y + (rail_h - thumb_h) * (float)app->list_top /
                              (float)(count - LIBRARY_ROWS);
        ui_vline(14 + LIB_LIST_W + 4, rail_y, rail_h, UI_LINE);
        ui_rect(14 + LIB_LIST_W + 3, thumb_y, 3, thumb_h, UI_ACCENT);
    }

    static const char *const hints[] = {
        "A", "Open", "L R", "Tabs", "X", "Search", "Y", "Refresh", NULL
    };
    static const char *const search_hints[] = {
        "A", "Open", "X", "Search", "B", "Library", "SELECT", "Settings", NULL
    };
    draw_footer(UI_TOP_WIDTH, searching ? search_hints : hints);
}

static int session_stage(const App *app)
{
    const GfnClient *client = app->client;
    if (client->session_state == GFN_SESSION_QUEUED) return 0;
    if (client->session_state == GFN_SESSION_SETUP) return 1;
    if (webrtc_transport_gameplay_ready(app->transport)) return 3;
    return 2;
}

static bool session_failed(const App *app)
{
    return app->client->session_state == GFN_SESSION_ERROR ||
           app->transport->state == WEBRTC_FAILED ||
           app->signal->state == NVST_SIGNAL_ERROR;
}

static void draw_session_top(const App *app)
{
    static float progress_x;
    const GfnClient *client = app->client;
    const bool failed = session_failed(app);
    const int stage = session_stage(app);
    const bool reconnecting = app->reconnect_attempt > 0 && app->reconnect_attempt <= 3;
    static const char *const kanji[] = { "待", "準", "接", "始" };
    static const char *const jp[] = { "待機中", "準備中", "接続中", "開始" };
    static const char *const en[] = { "IN QUEUE", "PREPARING RIG", "CONNECTING", "STARTING STREAM" };

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

    ui_enso(200, 84, 34, reconnecting ? UI_KIN : UI_ACCENT);
    if (reconnecting) {
        ui_text(200, 70, 26, UI_TEXT, UI_ALIGN_CENTER, "再");
        draw_title(200, 126, "再接続中", "RECONNECTING");
        ui_text_fit(200, 156, 13, UI_TEXT_DIM, UI_ALIGN_CENTER, 360, app->game_title);
        ui_textf(200, 172, 11, UI_TEXT_FAINT, UI_ALIGN_CENTER,
                 "The connection dropped. Attempt %u of 3; your game keeps running.",
                 app->reconnect_attempt);
    } else {
        if (stage == 0 && client->queue_position > 0)
            ui_textf(200, 70, 26, UI_TEXT, UI_ALIGN_CENTER, "%d", client->queue_position);
        else
            ui_text(200, 70, 26, UI_TEXT, UI_ALIGN_CENTER, kanji[stage]);
        draw_title(200, 126, jp[stage], en[stage]);
        ui_text_fit(200, 156, 13, UI_TEXT_DIM, UI_ALIGN_CENTER, 360,
                    app->game_title[0] ? app->game_title : "GeForce NOW");
        if (stage == 0)
            ui_text(200, 172, 11, UI_TEXT_FAINT, UI_ALIGN_CENTER,
                    client->queue_position > 0 ? "Your place in NVIDIA's queue" : "Waiting for a free rig");
        else if (stage >= 2)
            ui_text_fit(200, 172, 11, UI_TEXT_FAINT, UI_ALIGN_CENTER, 360,
                        app->transport->peer ? app->transport->status : app->signal->status);
    }

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

static void draw_settings_top(const App *app, bool entering)
{
    static float scroll, bar_y;
    draw_title(200, 31, "設定", "SETTINGS");
    ui_hline(16, 57, 368, UI_LINE);

    /* Lay the grouped list out once per frame. */
    enum { HEADER_H = 22, ROW_H = 20 };
    float ys[SETTING_ENTRY_COUNT];
    float content = 0.0f, selected_y = 0.0f;
    const int selected = screens_setting_at(app->setting_index);
    for (int i = 0; i < SETTING_ENTRY_COUNT; ++i) {
        ys[i] = content;
        if (SETTING_ENTRIES[i].setting == selected) selected_y = content;
        content += SETTING_ENTRIES[i].setting < 0 ? HEADER_H : ROW_H;
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
    static const char *const hints[] = { "A", "Change", "B", "Save & back", NULL };
    draw_footer(UI_TOP_WIDTH, hints);
}

static void draw_modal_top(const App *app, float p)
{
    ui_rect(0, 26, UI_TOP_WIDTH, 214, ui_with_alpha(UI_BG, (u8)(0xC8 * p)));
    ui_offset(0.0f, (1.0f - p) * 10.0f);
    const UiRect panel = { 60, 56, 280, 128 };
    draw_card(panel, p);
    draw_title(200, 66, app->modal_jp, app->modal_title);
    ui_text_wrap(200, 98, 11, UI_TEXT_DIM, UI_ALIGN_CENTER, 250, 4, 14, app->modal_text);
    static const char *const confirm[] = { "A", "Confirm", "B", "Cancel", NULL };
    static const char *const error[] = { "A", "Retry", "B", "Back", NULL };
    ui_hint_row(200, 162, app->modal == MODAL_ERROR ? error : confirm);
    ui_offset(0.0f, 0.0f);
}

/* ---- Game details ---------------------------------------------------------- */

static const char *details_store(const App *app, const GfnGame *game)
{
    if (app->details_variant < game->variant_count) return game->variants[app->details_variant].store;
    return game->store[0] ? game->store : "GeForce NOW";
}

static void format_played(char *out, size_t size, uint32_t seconds)
{
    if (seconds >= 3600) snprintf(out, size, "%lu h %02lu m", (unsigned long)(seconds / 3600),
                                  (unsigned long)(seconds / 60 % 60));
    else snprintf(out, size, "%lu min", (unsigned long)(seconds / 60));
}

static void details_fact(float x, float y, const char *label, const char *value)
{
    ui_label(x, y, 11, UI_TEXT_FAINT, UI_ALIGN_LEFT, label);
    ui_text_fit(x, y + 14, 13, UI_TEXT, UI_ALIGN_LEFT, 108, value);
}

static void draw_details_top(const App *app)
{
    const GfnGame *game = app_game(app, app->selected);
    if (!game) return;
    /* Cover on the left, facts on the right. */
    draw_game_art(game, 22, 38, 120, 1.0f);
    const float x = 160, w = 224;
    const int lines = ui_text_wrap(x, 36, 16, UI_TEXT, UI_ALIGN_LEFT, w, 2, 19, game->title);
    float y = 40 + lines * 19.0f;
    /* Store versions, the chosen one lit. */
    float px = x;
    if (game->variant_count > 1) {
        for (unsigned i = 0; i < game->variant_count; ++i) {
            const bool on = i == app->details_variant;
            px += ui_pill(px, y, on ? UI_ACCENT : UI_LINE_STRONG, UI_ALIGN_LEFT, game->variants[i].store) + 6;
        }
    } else {
        px += ui_pill(px, y, UI_TEXT_DIM, UI_ALIGN_LEFT, details_store(app, game)) + 6;
    }
    if (game_prefs_favourite(game->app_id)) ui_pill(px, y, UI_ACCENT, UI_ALIGN_LEFT, "FAVOURITE");
    y += 28;
    ui_hline(x, y, w, UI_LINE);
    y += 8;

    PlayHistory history = {0};
    const bool played = play_history_get(game->app_id, &history) ||
                        (app->details_variant < game->variant_count &&
                         play_history_get(game->variants[app->details_variant].id, &history));
    char value[48];
    if (played && history.seconds) format_played(value, sizeof(value), history.seconds);
    else snprintf(value, sizeof(value), played ? "Under a minute" : "Not yet");
    details_fact(x, y, "PLAYED ON KASUMI", value);
    snprintf(value, sizeof(value), "%lu", (unsigned long)history.sessions);
    details_fact(x + 118, y, "SESSIONS", value);
    y += 38;
    if (played && history.last_played) {
        const time_t when = (time_t)history.last_played;
        const struct tm *t = gmtime(&when);
        if (t) strftime(value, sizeof(value), "%d %b %Y", t);
        else snprintf(value, sizeof(value), "-");
    } else {
        snprintf(value, sizeof(value), "-");
    }
    details_fact(x, y, "LAST PLAYED", value);
    const GamePrefs prefs = game_prefs_get(game->app_id);
    details_fact(x + 118, y, "GAME OPTIONS",
                 prefs.bitrate >= 0 || prefs.gyro >= 0 || prefs.layout >= 0 ? "Custom" : "Default");
    y += 38;
    ui_label(x, y, 11, UI_TEXT_FAINT, UI_ALIGN_LEFT, "STREAM");
    ui_text_fit(x, y + 14, 12, UI_TEXT_DIM, UI_ALIGN_LEFT, w, stream_profile_name());

    static const char *const hints[] = { "A", "Play", "Y", "Favourite", "X", "Options", "B", "Back", NULL };
    draw_footer(UI_TOP_WIDTH, hints);
}

/* The per-game options sheet: rows of "setting  < value >". */
static const char *option_value(const App *app, const GamePrefs *prefs, int row, char *buffer, size_t size)
{
    static const char *const bitrates[STREAM_BITRATE_COUNT] = { "Adaptive", "Steady 1", "Steady 1.2", "Steady 1.5" };
    static const char *const gyros[GFN_GYRO_MODE_COUNT] = { "Off", "Always", "While aiming" };
    static const char *const layouts[2] = { "Position", "Letters" };
    const GfnClient *c = app->client;
    switch (row) {
    case OPTION_BITRATE: return prefs->bitrate < 0 ? "Default" : bitrates[prefs->bitrate % STREAM_BITRATE_COUNT];
    case OPTION_GYRO: return prefs->gyro < 0 ? "Default" : gyros[prefs->gyro % GFN_GYRO_MODE_COUNT];
    case OPTION_LAYOUT: return prefs->layout < 0 ? "Default" : layouts[prefs->layout % 2];
    case OPTION_CONNECTION:
        if (!c->conn_tested_at) return "Press A to test";
        snprintf(buffer, size, "%u ms  ·  %u.%u Mbps  ·  %u/3", c->conn_latency_ms, c->conn_kbps / 1000,
                 c->conn_kbps % 1000 / 100, c->conn_bars);
        return buffer;
    }
    return "";
}

/* What the last connection check means for play. */
static const char *connection_advice(const GfnClient *c)
{
    if (!c->conn_tested_at) return "Measures Wi-Fi, latency and speed to NVIDIA.";
    if (c->conn_bars < 2 || c->conn_kbps < 2000) return "Weak link: choose Steady 1 Mbps for this game.";
    if (c->conn_latency_ms > 150) return "High latency: expect some input lag.";
    return "Good connection: Adaptive should run smoothly.";
}

static void draw_options_sheet(const App *app, float p)
{
    const GfnGame *game = app_game(app, app->selected);
    if (!game) return;
    const GamePrefs prefs = game_prefs_get(game->app_id);
    ui_rect(0, 0, UI_BOTTOM_WIDTH, UI_HEIGHT, ui_with_alpha(UI_BG, (u8)(0xF0 * p)));
    ui_offset(0.0f, (1.0f - p) * 10.0f);
    ui_text(160, 3, 12, UI_ACCENT, UI_ALIGN_CENTER, "設定");
    ui_label(160, 18, 11, UI_TEXT, UI_ALIGN_CENTER, "OPTIONS FOR THIS GAME");
    ui_hline(16, 33, 288, UI_LINE);
    static const char *const labels[OPTION_COUNT] = { "Bitrate", "Gyro aim", "Button layout", "Connection" };
    char buffer[64];
    for (int i = 0; i < OPTION_COUNT; ++i) {
        const float y = OPT_ROW_Y + i * OPT_ROW_H;
        const bool focus = i == app->options_index;
        if (focus) {
            ui_rect(12, y, 296, OPT_ROW_H - 4, UI_RAISED);
            ui_rect(12, y, 2, OPT_ROW_H - 4, UI_ACCENT);
        }
        ui_text(24, y + 9, 12, focus ? UI_TEXT : UI_TEXT_DIM, UI_ALIGN_LEFT, labels[i]);
        const char *value = option_value(app, &prefs, i, buffer, sizeof(buffer));
        const bool custom = (i == OPTION_BITRATE && prefs.bitrate >= 0) ||
                            (i == OPTION_GYRO && prefs.gyro >= 0) || (i == OPTION_LAYOUT && prefs.layout >= 0);
        ui_text_fit(296, y + 9, 12, custom ? UI_ACCENT : focus ? UI_TEXT : UI_TEXT_DIM, UI_ALIGN_RIGHT,
                    180, value);
    }
    ui_text_wrap(160, OPT_ROW_Y + OPTION_COUNT * OPT_ROW_H + 2, 11, UI_TEXT_DIM, UI_ALIGN_CENTER, 292, 1, 14,
                 app->options_index == OPTION_CONNECTION ? connection_advice(app->client)
                 : "Default follows Settings. Only this game's sessions use these.");
    ui_button(OPT_CLOSE, "DONE", "完了", UI_BUTTON_NORMAL, pressed(app, OPT_CLOSE));
    ui_offset(0.0f, 0.0f);
}

static void draw_details_bottom(const App *app, float overlay_p)
{
    const GfnGame *game = app_game(app, app->selected);
    if (!game) return;
    draw_status_strip(app, app->status);
    ui_button(DET_PLAY, "PLAY", "遊ぶ", UI_BUTTON_PRIMARY, pressed(app, DET_PLAY));
    const bool choice = game->variant_count > 1;
    draw_arrow(DET_STORE_PREV, -1, choice, pressed(app, DET_STORE_PREV));
    draw_arrow(DET_STORE_NEXT, 1, choice, pressed(app, DET_STORE_NEXT));
    ui_label(160, 110, 11, UI_TEXT_FAINT, UI_ALIGN_CENTER, choice ? "LAUNCH FROM  (LEFT / RIGHT)" : "LAUNCH FROM");
    ui_text_fit(160, 126, 15, UI_TEXT, UI_ALIGN_CENTER, 180, details_store(app, game));
    if (choice) ui_dots(160, 152, game->variant_count, app->details_variant, UI_ACCENT, UI_LINE_STRONG);
    const bool favourite = game_prefs_favourite(game->app_id);
    ui_button(DET_FAV, favourite ? "SAVED" : "FAVOURITE", "お気に入り",
              favourite ? UI_BUTTON_ACTIVE : UI_BUTTON_NORMAL, pressed(app, DET_FAV));
    ui_button(DET_OPTIONS, "OPTIONS", "設定", UI_BUTTON_NORMAL, pressed(app, DET_OPTIONS));
    ui_button(DET_BACK, "BACK", "戻る", UI_BUTTON_NORMAL, pressed(app, DET_BACK));
    if (app->options_open) draw_options_sheet(app, overlay_p);
}

/* ---- First-run guide ------------------------------------------------------- */

typedef struct {
    const char *kanji, *jp, *en, *body, *tip;
} GuidePage;

static const GuidePage GUIDE[GUIDE_PAGES] = {
    { "霞", "ようこそ", "WELCOME TO KASUMI",
      "Play your GeForce NOW games on the New 3DS. The games run on NVIDIA's servers; the 3DS shows "
      "the picture and sends your buttons.",
      "You need an NVIDIA account and Wi-Fi with a good signal." },
    { "鍵", "サインイン", "SIGN IN",
      "Kasumi shows a web address and a short code. Enter the code on your phone or computer; no "
      "password is ever typed on the 3DS.",
      "Your login stays only on this console's SD card." },
    { "操", "操作", "CONTROLS",
      "The 3DS plays like a PlayStation pad: bottom is Cross, right is Circle. L3, R3 and PS are on "
      "the lower screen.",
      "Hold START + SELECT during play for the stream menu." },
    { "画", "画質", "PICTURE & WI-FI",
      "Stay close to your router (3 bars). Adaptive bitrate is the default; choose Steady 1 Mbps if "
      "the picture stutters. ZOOM crops the picture for small text.",
      "Settings > System > Connection check tests your Wi-Fi." },
    { "遊", "機能", "EXTRAS",
      "Gyro aiming, screenshots, zoom zones, themes, favourites and options for each game. Open a "
      "game from the library to see them.",
      "Settings > System > Getting started shows this guide again." },
};

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
    const float p = view_progress(&g_top_anim, (int)app->view, (int)app->modal);
    const bool entering = p < 1.0f;
    if (app->guide_page >= 0 && app->view != VIEW_STREAM) draw_mist(150, 0.35f);
    else if (app->view == VIEW_WELCOME) draw_welcome_backdrop();
    else if (app->view == VIEW_SESSION) draw_mist(132, 0.45f);
    else if (app->view == VIEW_LOGIN) draw_mist(150, 0.35f);
    draw_status_bar(app, UI_TOP_WIDTH);
    /* The view's content rises 8 px as it fades in. */
    ui_offset(0.0f, (1.0f - p) * 8.0f);
    const bool guide = app->guide_page >= 0 && app->view != VIEW_STREAM;
    if (guide) draw_guide_top(app);
    else switch (app->view) {
    case VIEW_WELCOME: draw_welcome_top(); break;
    case VIEW_LOGIN: draw_login_top(app); break;
    case VIEW_LIBRARY: draw_library_top(app, entering); break;
    case VIEW_SETTINGS: draw_settings_top(app, entering); break;
    case VIEW_SESSION: draw_session_top(app); break;
    case VIEW_DETAILS: draw_details_top(app); break;
    case VIEW_STREAM: break;
    }
    ui_offset(0.0f, 0.0f);
    fade_in_veil(UI_TOP_WIDTH, 26.0f, p);
    if (app->modal != MODAL_NONE) draw_modal_top(app, overlay_progress(&g_top_anim));
    if (app->busy) draw_busy_top(app);
}

/* ---- Bottom screens ------------------------------------------------------ */

static void draw_welcome_bottom(const App *app)
{
    draw_status_strip(app, app->status);
    ui_text(160, 40, 12, UI_TEXT, UI_ALIGN_CENTER, "Your GeForce NOW games on the New 3DS.");
    ui_text(160, 58, 11, UI_TEXT_DIM, UI_ALIGN_CENTER, "Sign-in happens on your phone or computer:");
    ui_text(160, 73, 11, UI_TEXT_DIM, UI_ALIGN_CENTER, "no password is typed on this console.");
    ui_button(WEL_SIGN_IN, "SIGN IN", "サインイン", UI_BUTTON_PRIMARY, pressed(app, WEL_SIGN_IN));
    ui_button(WEL_SETTINGS, "SETTINGS", "設定", UI_BUTTON_NORMAL, pressed(app, WEL_SETTINGS));
    ui_button(WEL_EXIT, "EXIT", "終了", UI_BUTTON_NORMAL, pressed(app, WEL_EXIT));
    ui_label(160, 222, 11, UI_TEXT_FAINT, UI_ALIGN_CENTER, "BUILD " APP_BUILD);
}

static void draw_login_bottom(const App *app)
{
    draw_status_strip(app, app->status);
    static const char *const steps[] = {
        "Open the address shown above.",
        "Sign in to your NVIDIA account.",
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
                 "Your login stays on this console's SD card only. Kasumi is unofficial, not made by NVIDIA.");
    ui_button(PAIR_LEFT, "NEW CODE", "再発行", UI_BUTTON_NORMAL, pressed(app, PAIR_LEFT));
    ui_button(PAIR_RIGHT, "CANCEL", "取消", UI_BUTTON_NORMAL, pressed(app, PAIR_RIGHT));
}

static void draw_library_bottom(const App *app)
{
    const GfnClient *client = app->client;
    /* When idle, the strip says how fresh the saved library is. */
    char synced[80];
    const char *strip = app->status;
    if (client->library_saved_at && !app->search_text[0]) {
        const long age = (long)((int64_t)time(NULL) - client->library_saved_at);
        if (age < 120) snprintf(synced, sizeof(synced), "%lu games · synced just now",
                                (unsigned long)client->game_count);
        else if (age < 7200) snprintf(synced, sizeof(synced), "%lu games · synced %ld min ago",
                                      (unsigned long)client->game_count, age / 60);
        else if (age < 172800) snprintf(synced, sizeof(synced), "%lu games · synced %ld h ago",
                                        (unsigned long)client->game_count, age / 3600);
        else snprintf(synced, sizeof(synced), "%lu games · synced %ld days ago",
                      (unsigned long)client->game_count, age / 86400);
        strip = synced;
    }
    draw_status_strip(app, strip);
    const bool has_game = app_game(app, app->selected) != NULL;
    draw_arrow(LIB_PREV, -1, has_game && app->selected > 0, pressed(app, LIB_PREV));
    draw_arrow(LIB_NEXT, 1, has_game && app->selected + 1 < app->list_count,
               pressed(app, LIB_NEXT));

    const UiRect card = { 44, 34, 232, 84 };
    ui_panel(card, UI_ACCENT);
    if (has_game) {
        const GfnGame *game = app_game(app, app->selected);
        /* Thumbnail on the left, text centred in the rest of the card. */
        draw_game_art(game, card.x + 8, card.y + 10, 48, 1.0f);
        const float text_x = card.x + 64, text_w = card.w - 72, cx = text_x + text_w / 2;
        const int lines = ui_text_wrap(cx, card.y + 12, 14, UI_TEXT, UI_ALIGN_CENTER,
                                       text_w, 2, 17, game->title);
        const float meta_y = card.y + 18 + lines * 17.0f;
        const char *store = game->store[0] ? game->store : "GFN";
        ui_pill(cx, meta_y - 2, UI_TEXT_DIM, UI_ALIGN_CENTER, store);
        ui_text_fit(cx, meta_y + 17, 11, UI_TEXT_FAINT, UI_ALIGN_CENTER, text_w, stream_profile_name());
    } else {
        ui_text(160, card.y + 22, 13, UI_TEXT_DIM, UI_ALIGN_CENTER, "No game selected");
        ui_text(160, card.y + 44, 11, UI_TEXT_FAINT, UI_ALIGN_CENTER, "Load your library or search");
    }

    /* Open the game's page; an empty tab offers the full list instead. */
    const char *main_label = has_game ? "OPEN GAME" : client->game_count ? "SHOW ALL GAMES" : "LOAD LIBRARY";
    const char *main_jp = has_game ? "詳細" : client->game_count ? "全て" : "ライブラリ";
    ui_button(LIB_PLAY, main_label, main_jp, has_game ? UI_BUTTON_PRIMARY : UI_BUTTON_NORMAL,
              pressed(app, LIB_PLAY));
    /* In the library this button refreshes it; after a search it goes back. */
    const bool searching = app->search_text[0] != '\0';
    ui_button(LIB_LIBRARY, searching ? "LIBRARY" : "REFRESH", searching ? "ライブラリ" : "更新",
              UI_BUTTON_NORMAL, pressed(app, LIB_LIBRARY));
    ui_button(LIB_SEARCH, "SEARCH", "検索",
              app->search_text[0] ? UI_BUTTON_ACTIVE : UI_BUTTON_NORMAL, pressed(app, LIB_SEARCH));
    ui_button(LIB_SETTINGS, "SETTINGS", "設定", UI_BUTTON_NORMAL, pressed(app, LIB_SETTINGS));
}

/* PlayStation symbol produced by a 3DS face button in the current layout.
 * key: 0 X (top), 1 A (right), 2 B (bottom), 3 Y (left). */
static void draw_ps_symbol_for_key(int key, bool position_layout, float cx, float cy, float size)
{
    static const int by_position[4] = { 0, 1, 2, 3 };
    static const int by_letter[4] = { 3, 2, 1, 0 };
    switch (position_layout ? by_position[key] : by_letter[key]) {
    case 0: ui_ps_triangle(cx, cy, size, UI_MATCHA); break;
    case 1: ui_ps_circle(cx, cy, size, UI_DANGER); break;
    case 2: ui_ps_cross(cx, cy, size, UI_AI); break;
    default: ui_ps_square(cx, cy, size, UI_SAKURA); break;
    }
}

static void draw_face_diamond(float cx, float cy, bool playstation, bool position_layout)
{
    const float d = 17.0f, r = 10.0f;
    /* Order: top, right, bottom, left. */
    const float px[4] = { cx, cx + d, cx, cx - d };
    const float py[4] = { cy - d, cy, cy + d, cy };
    static const char *const letters[4] = { "X", "A", "B", "Y" };
    for (int i = 0; i < 4; ++i) {
        ui_ring(px[i], py[i], r, 1.2f, UI_LINE_STRONG, UI_SURFACE);
        if (playstation) draw_ps_symbol_for_key(i, position_layout, px[i], py[i], 11);
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
    ui_rounded(cx - 34, cy - 20, 68, 40, 6, on ? UI_ACCENT : UI_LINE_STRONG);
    ui_rounded(cx - 32, cy - 18, 64, 36, 5, UI_SURFACE);
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
    static const float stops[STREAM_BITRATE_COUNT] = { 0.25f, 0.0f, 0.4f, 1.0f };
    for (int i = 1; i < STREAM_BITRATE_COUNT; ++i)
        ui_circle(x0 + (x1 - x0) * stops[i], y, 2.0f, UI_LINE_STRONG);
    const float at = x0 + (x1 - x0) * stops[app->settings.bitrate_mode];
    ui_circle(at, y, 6.0f, UI_ACCENT);
    ui_circle(at, y, 2.5f, UI_BG);
    if (app->settings.bitrate_mode == STREAM_BITRATE_ADAPTIVE)
        ui_rect(x0 + (x1 - x0) * 0.0f, y - 1, (x1 - x0) * 0.55f, 2, ui_with_alpha(UI_ACCENT, 0x70));
}

static void draw_settings_bottom(const App *app)
{
    const int setting = screens_setting_at(app->setting_index);
    ui_text(160, 2, 12, UI_ACCENT, UI_ALIGN_CENTER, SETTING_JP[setting]);
    ui_label(160, 16, 11, UI_TEXT, UI_ALIGN_CENTER, SETTING_LABELS[setting]);
    ui_hline(0, 29, UI_BOTTOM_WIDTH, UI_LINE);
    ui_text_wrap(160, 36, 11, UI_TEXT_DIM, UI_ALIGN_CENTER, 292, 3, 14,
                 setting_description(app, setting));

    const bool account = setting == SETTING_ACCOUNT;
    if (setting == SETTING_LAYOUT || setting == SETTING_TRIGGERS) {
        const bool position = app->settings.button_layout == GFN_LAYOUT_POSITION;
        draw_face_diamond(96, 116, false, position);
        draw_face_diamond(224, 116, true, position);
        ui_triangle(152, 110, 152, 122, 164, 116, UI_ACCENT);
        ui_label(96, 146, 11, UI_TEXT_FAINT, UI_ALIGN_CENTER, "3DS");
        ui_label(224, 146, 11, UI_TEXT_FAINT, UI_ALIGN_CENTER, "PLAYSTATION");
        const bool swap = app->settings.swap_shoulders;
        ui_textf(160, 163, 11, UI_TEXT_DIM, UI_ALIGN_CENTER, "L  %s     ZL  %s     R  %s     ZR  %s",
                 swap ? "L2" : "L1", swap ? "L1" : "L2", swap ? "R2" : "R1", swap ? "R1" : "R2");
    } else {
        const float value_y = setting == SETTING_GYRO || setting == SETTING_BITRATE ? 146.0f : 104.0f;
        if (setting == SETTING_GYRO) draw_gyro_preview(app);
        if (setting == SETTING_THEME) {
            for (int i = 0; i < UI_THEME_COUNT; ++i) {
                const float sx = 160 + (i - 2) * 34.0f;
                const bool on = (unsigned)i == app->settings.theme;
                if (on) ui_ring(sx, 88, 13, 1.5f, UI_TEXT, UI_BG);
                ui_circle(sx, 88, 9, ui_theme_color((UiTheme)i));
            }
        }
        if (setting == SETTING_BITRATE) draw_bitrate_preview(app);
        ui_text(160, value_y, setting == SETTING_GYRO || setting == SETTING_BITRATE ? 16 : 22,
                account ? UI_DANGER : UI_TEXT, UI_ALIGN_CENTER, setting_value(app, setting));
        unsigned count = 0;
        const unsigned option = setting_option(app, setting, &count);
        if (count > 1 && setting != SETTING_BITRATE)
            ui_dots(160, value_y + (setting == SETTING_GYRO ? 24.0f : 34.0f), count, option,
                    UI_ACCENT, UI_LINE_STRONG);
    }

    draw_arrow(SET_PREV, -1, !account, pressed(app, SET_PREV));
    draw_arrow(SET_NEXT, 1, !account, pressed(app, SET_NEXT));
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
    ui_label(232, 116, 11, UI_TEXT_FAINT, UI_ALIGN_CENTER, "QUEUE");
    if (client->session_state == GFN_SESSION_QUEUED && client->queue_position > 0)
        ui_textf(232, 131, 15, UI_TEXT, UI_ALIGN_CENTER, "#%d", client->queue_position);
    else
        ui_text(232, 131, 15, UI_TEXT, UI_ALIGN_CENTER,
                client->session_state == GFN_SESSION_QUEUED ? "-" : "Done");
    if (app->wifi_bars < 2)
        ui_text(160, 160, 11, UI_KIN, UI_ALIGN_CENTER, "Weak Wi-Fi: move closer to the router.");

    if (session_failed(app) && (app->reconnect_attempt == 0 || app->reconnect_attempt > 3)) {
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
    ui_text(160, p.y + 30, 12, UI_ACCENT, UI_ALIGN_CENTER, "タッチパッド");
    ui_label(160, p.y + 48, 11, UI_TEXT, UI_ALIGN_CENTER, "TOUCHPAD");
    ui_text(160, p.y + 66, 11, UI_TEXT_DIM, UI_ALIGN_CENTER, "Drag to move, tap to click");
    ui_text(160, p.y + 81, 11, UI_TEXT_DIM, UI_ALIGN_CENTER, "C-Stick moves, A clicks");
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
    /* Face buttons with the PlayStation symbol each one sends. */
    static const char *const keys[4] = { "X", "A", "B", "Y" };
    float x = 26;
    for (int i = 0; i < 4; ++i) {
        ui_button_chip(x, 40, keys[i], UI_TEXT_DIM);
        draw_ps_symbol_for_key(i, position, x + 28, 47.5f, 11);
        x += 72;
    }
    float y = 64;
    const float dy = 17;
    controls_row(y, "CIRCLE", "Left stick"); y += dy;
    controls_row(y, "C-STICK", s->gyro_mode != GFN_GYRO_OFF ? "Right stick + gyro" : "Right stick"); y += dy;
    controls_row(y, s->swap_shoulders ? "ZL ZR" : "L R", "L1 / R1"); y += dy;
    controls_row(y, s->swap_shoulders ? "L R" : "ZL ZR", "L2 / R2 triggers"); y += dy;
    controls_row(y, "START", "Options"); y += dy;
    controls_row(y, "SELECT", "Share / View"); y += dy;
    controls_row(y, "TOUCH", "L3 / R3 / PS buttons"); y += dy;
    controls_row(y, "START+SELECT", "Hold for the stream menu"); y += dy;
    ui_textf(160, y + 4, 11, UI_TEXT_FAINT, UI_ALIGN_CENTER, "Gyro aim: %s  ·  change it in the stream menu",
             gyro_mode_name(s->gyro_mode));
    static const char *const hints[] = { "B", "Close", NULL };
    ui_hint_row(160, 222, hints);
    ui_offset(0.0f, 0.0f);
}

static void format_elapsed(char *out, size_t size, u64 ms)
{
    const unsigned total = (unsigned)(ms / 1000);
    if (total >= 3600)
        snprintf(out, size, "%u:%02u:%02u", total / 3600, total / 60 % 60, total % 60);
    else
        snprintf(out, size, "%u:%02u", total / 60, total % 60);
}

static void draw_stream_header(const App *app)
{
    const WebRtcTransport *t = app->transport;
    const u64 now = osGetTime();
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
    ui_text(20, 4, 12, UI_ACCENT, UI_ALIGN_LEFT, "配信");
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

static void draw_stream_bottom(const App *app, float overlay_p)
{
    const WebRtcTransport *t = app->transport;
    if (app->keyboard_open) {
        remote_keyboard_draw(t, app->touching, app->touch_x, app->touch_y);
        return;
    }
    draw_stream_header(app);

    const uint16_t held = app->touching ? screens_stream_held_buttons(app, app->touch_x, app->touch_y) : 0;
    draw_stick_button(STR_L3, "L3", "左", (held & GFN_PAD_LEFT_THUMB) != 0);
    draw_stick_button(STR_R3, "R3", "右", (held & GFN_PAD_RIGHT_THUMB) != 0);

    if (t->pointer_mode) draw_touchpad();
    else if (mvd_video_zoomed()) draw_zoom_map();
    else draw_stats(app);

    /* Guide / PS button, centred between the stick buttons. */
    const bool guide = (held & GFN_PAD_GUIDE) != 0;
    const float gx = STR_GUIDE.x + STR_GUIDE.w / 2, gy = STR_GUIDE.y + STR_GUIDE.h / 2;
    ui_ring(gx, gy, 17, 1.5f, guide ? UI_ACCENT : UI_LINE_STRONG, guide ? UI_ACCENT : UI_BG);
    ui_text(gx, gy - 7, 12, guide ? UI_BG : UI_TEXT, UI_ALIGN_CENTER, "PS");
    ui_hline(STR_L3.x + STR_L3.w + 6, gy, gx - 17 - (STR_L3.x + STR_L3.w + 6) - 4, UI_LINE);
    ui_hline(gx + 21, gy, STR_R3.x - 6 - (gx + 21), UI_LINE);
    if (app->settings.gyro_mode != GFN_GYRO_OFF) {
        /* Gyro badge on the left rule, lit while gyro is steering. */
        const bool live = gfn_input_gyro_active();
        ui_rect(78, gy - 8, 44, 16, UI_BG);
        ui_label(100, gy - 6, 11, live ? UI_ACCENT : UI_TEXT_FAINT, UI_ALIGN_CENTER, "GYRO");
    }

    char zoom[16];
    const unsigned level = mvd_video_zoom_level();
    if (app->zone_index >= 0 && level) snprintf(zoom, sizeof(zoom), "ZONE %d", app->zone_index + 1);
    else snprintf(zoom, sizeof(zoom), "%s", level == 3 ? "2.0x" : level == 2 ? "1.5x" : level == 1 ? "1.2x" : "拡大");
    const char *labels[4] = { "KEYS", "POINTER", "ZOOM", "MENU" };
    const char *jp[4] = { "キー", "ポインタ", zoom, "メニュー" };
    const bool active[4] = { false, t->pointer_mode, level != 0, false };
    for (int i = 0; i < 4; ++i) {
        const UiRect r = stream_button(i);
        ui_button(r, labels[i], jp[i], active[i] ? UI_BUTTON_ACTIVE : UI_BUTTON_NORMAL, pressed(app, r));
    }
    if (app->controls_open) draw_controls_sheet(app, overlay_p);
    else if (app->stream_menu) draw_stream_menu(app, overlay_p);
}

static void draw_modal_bottom(const App *app, float p)
{
    ui_rect(0, 0, UI_BOTTOM_WIDTH, UI_HEIGHT, ui_with_alpha(UI_BG, (u8)(0xC8 * p)));
    ui_offset(0.0f, (1.0f - p) * 10.0f);
    ui_text(160, 68, 12, UI_ACCENT, UI_ALIGN_CENTER, app->modal_jp);
    ui_label(160, 84, 11, UI_TEXT, UI_ALIGN_CENTER, app->modal_title);
    const bool error = app->modal == MODAL_ERROR;
    ui_button(MODAL_LEFT, error ? "RETRY" : "YES", error ? "再試行" : "はい",
              app->modal == MODAL_EXIT || app->modal == MODAL_SIGN_OUT ? UI_BUTTON_DANGER
                                                                         : UI_BUTTON_PRIMARY,
              pressed(app, MODAL_LEFT));
    ui_button(MODAL_RIGHT, error ? "BACK" : "NO", error ? "戻る" : "いいえ", UI_BUTTON_NORMAL,
              pressed(app, MODAL_RIGHT));
    ui_offset(0.0f, 0.0f);
}

void screens_draw_bottom(const App *app)
{
    /* The overlay id folds the modal, menu and controls sheet together so
     * any of them opening restarts the overlay fade. */
    const int overlay = app->modal != MODAL_NONE ? 10 + (int)app->modal :
                        app->controls_open ? 2 : app->stream_menu ? 1 : app->options_open ? 3 : 0;
    const float p = view_progress(&g_bottom_anim, (int)app->view, overlay);
    const float op = overlay_progress(&g_bottom_anim);
    g_bottom_busy_animating = p < 1.0f || (overlay && op < 1.0f) ||
                              (app->view == VIEW_SETTINGS && app->setting_index >= 0 &&
                               (screens_setting_at(app->setting_index) == SETTING_GYRO));
    ui_offset(0.0f, (1.0f - p) * 8.0f);
    const bool guide = app->guide_page >= 0 && app->view != VIEW_STREAM;
    if (guide) draw_guide_bottom(app);
    else switch (app->view) {
    case VIEW_WELCOME: draw_welcome_bottom(app); break;
    case VIEW_LOGIN: draw_login_bottom(app); break;
    case VIEW_LIBRARY: draw_library_bottom(app); break;
    case VIEW_SETTINGS: draw_settings_bottom(app); break;
    case VIEW_SESSION: draw_session_bottom(app); break;
    case VIEW_DETAILS: draw_details_bottom(app, op); break;
    case VIEW_STREAM: ui_offset(0.0f, 0.0f); draw_stream_bottom(app, op); break;
    }
    ui_offset(0.0f, 0.0f);
    fade_in_veil(UI_BOTTOM_WIDTH, 0.0f, p);
    if (app->modal != MODAL_NONE) draw_modal_bottom(app, op);
    if (app->busy) {
        ui_rect(0, 0, UI_BOTTOM_WIDTH, UI_HEIGHT, UI_SCRIM);
        ui_enso(160, 96, 16, UI_ACCENT);
        static const char *const hints[] = { "B", "Cancel", NULL };
        ui_hint_row(160, 128, hints);
    }
}

/* ---- Touch ---------------------------------------------------------------- */

uint16_t screens_stream_held_buttons(const App *app, int x, int y)
{
    if (app->view != VIEW_STREAM || app->keyboard_open || app->stream_menu || app->controls_open)
        return 0;
    if (ui_hit(STR_L3, x, y)) return GFN_PAD_LEFT_THUMB;
    if (ui_hit(STR_R3, x, y)) return GFN_PAD_RIGHT_THUMB;
    if (ui_hit(STR_GUIDE, x, y)) return GFN_PAD_GUIDE;
    return 0;
}

AppAction screens_touch(const App *app, int x, int y)
{
    if (app->busy) return ACTION_NONE;
    g_touched_option_row = -1;
    if (app->guide_page >= 0 && app->view != VIEW_STREAM) {
        if (ui_hit(GUIDE_BACK, x, y)) return ACTION_GUIDE_BACK;
        if (ui_hit(GUIDE_SKIP, x, y)) return ACTION_GUIDE_SKIP;
        if (ui_hit(GUIDE_NEXT, x, y)) return ACTION_GUIDE_NEXT;
        return ACTION_NONE;
    }
    if (app->modal != MODAL_NONE) {
        if (ui_hit(MODAL_LEFT, x, y)) return app->modal == MODAL_ERROR ? ACTION_RETRY : ACTION_CONFIRM;
        if (ui_hit(MODAL_RIGHT, x, y)) return ACTION_DISMISS;
        return ACTION_NONE;
    }
    switch (app->view) {
    case VIEW_WELCOME:
        if (ui_hit(WEL_SIGN_IN, x, y)) return ACTION_SIGN_IN;
        if (ui_hit(WEL_SETTINGS, x, y)) return ACTION_SETTINGS;
        if (ui_hit(WEL_EXIT, x, y)) return ACTION_EXIT;
        break;
    case VIEW_LOGIN:
        if (ui_hit(PAIR_LEFT, x, y)) return ACTION_NEW_CODE;
        if (ui_hit(PAIR_RIGHT, x, y)) return ACTION_CANCEL;
        break;
    case VIEW_LIBRARY:
        if (ui_hit(LIB_PREV, x, y)) return ACTION_PREV;
        if (ui_hit(LIB_NEXT, x, y)) return ACTION_NEXT;
        if (ui_hit(LIB_PLAY, x, y))
            return app->list_count ? ACTION_PLAY : ACTION_LIBRARY;
        if (ui_hit(LIB_LIBRARY, x, y)) return ACTION_LIBRARY;
        if (ui_hit(LIB_SEARCH, x, y)) return ACTION_SEARCH;
        if (ui_hit(LIB_SETTINGS, x, y)) return ACTION_SETTINGS;
        break;
    case VIEW_SETTINGS:
        if (ui_hit(SET_PREV, x, y)) return ACTION_VALUE_PREV;
        if (ui_hit(SET_NEXT, x, y)) return ACTION_VALUE_NEXT;
        if (ui_hit(SET_BACK, x, y)) return ACTION_BACK;
        break;
    case VIEW_SESSION:
        if (session_failed(app) && (app->reconnect_attempt == 0 || app->reconnect_attempt > 3)) {
            if (ui_hit(PAIR_LEFT, x, y)) return ACTION_RETRY;
            if (ui_hit(PAIR_RIGHT, x, y)) return ACTION_CANCEL;
        } else if (ui_hit(SINGLE, x, y)) {
            return ACTION_CANCEL;
        }
        break;
    case VIEW_DETAILS:
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
        if (ui_hit(stream_button(0), x, y)) return ACTION_STREAM_KEYBOARD;
        if (ui_hit(stream_button(1), x, y)) return ACTION_STREAM_POINTER;
        if (ui_hit(stream_button(2), x, y)) return ACTION_STREAM_ZOOM;
        if (ui_hit(stream_button(3), x, y)) return ACTION_STREAM_MENU;
        break;
    }
    return ACTION_NONE;
}

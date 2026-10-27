#include "screens_model.h"
#include "platform.h"
#include "app_paths.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "audio_output.h"
#include "game_art.h"
#include "game_prefs.h"
#include "gallery.h"
#include "i18n.h"
#include "menu_audio.h"
#include "provider.h"
#include "regions.h"
#include "report.h"
#include "steam_link.h"
#include "stream_profile.h"
#include "updater.h"
#include "webrtc_transport.h"

/* What the screens say, whatever console draws them: the settings and
 * their values and descriptions, each service's name and colour, the
 * guide, the per-game options. Each console's screens.c lays it out. */

const char *const SERVICE_TABS[SERVICE_COUNT] = { "GEFORCE NOW", "XBOX CLOUD", "STEAM LINK" };
const ServiceInfo SERVICE_INFO[SERVICE_COUNT] = {
    { "GeForce NOW", "Your PC games from Steam, Epic and more, running on NVIDIA's servers.",
      "An NVIDIA account (free or paid).", "SIGN IN WITH NVIDIA", "サインイン" },
    { "Xbox Cloud Gaming", "Game Pass games, and free ones like Fortnite, from Microsoft's servers. Beta.",
      "A Microsoft account; most games need Game Pass Ultimate.", "SIGN IN WITH MICROSOFT", "サインイン" },
    { "Steam Link", "Your own PC: play whatever Steam on it can run, over your Wi-Fi.",
      "A PC with Steam on, on the same Wi-Fi as the 3DS.", "PAIR WITH YOUR PC", "ペアリング" },
};

/* ---- Settings model ------------------------------------------------------ */

/* The settings list is grouped: each entry is a section heading (setting
 * -1) or a setting. Navigation walks settings only. */
/* Six sections; Settings opens on a grid of them (33 settings in one list
 * was a long scroll on a 3DS). A header starts each section. */
const SettingEntry SETTING_ENTRIES[] = {
    { -1, "操作", "CONTROLS" },
    { SETTING_LAYOUT, NULL, NULL },
    { SETTING_PAD_NAMES, NULL, NULL },
    { SETTING_MAPPING, NULL, NULL },
    { SETTING_TRIGGERS, NULL, NULL },
    { SETTING_DEADZONE, NULL, NULL },
    { SETTING_CAMERA_SPEED, NULL, NULL },
    { SETTING_CAMERA_INVERT, NULL, NULL },
    { SETTING_TOUCH_CAMERA, NULL, NULL },
    { SETTING_TOUCH_STICK_SIZE, NULL, NULL },
#if PLAT_HAS_EXTRA_BUTTONS
    { SETTING_EXTRA_BUTTONS, NULL, NULL },
#endif
#if PLAT_HAS_3D_SLIDER
    { SETTING_SLIDER, NULL, NULL },
#endif
    { SETTING_GYRO, NULL, NULL },
    { SETTING_GYRO_SPEED, NULL, NULL },
    { SETTING_POINTER, NULL, NULL },
    { SETTING_FAST_INPUT, NULL, NULL },
    { -1, "画質", "PICTURE" },
#if PLAT_HAS_WIDE_MODE
    { SETTING_RESOLUTION, NULL, NULL },
#endif
    { SETTING_FRAME_RATE, NULL, NULL },
    { SETTING_BITRATE, NULL, NULL },
#if PLAT_HAS_VIDEO_FILTER
    { SETTING_VIDEO_SHARPEN, NULL, NULL },
    { SETTING_VIDEO_COLOR, NULL, NULL },
#endif
    { SETTING_FILTER, NULL, NULL },
    { SETTING_STATS, NULL, NULL },
    { -1, "音と色", "SOUND & LOOK" },
    { SETTING_VOLUME, NULL, NULL },
    { SETTING_MENU_AUDIO, NULL, NULL },
    { SETTING_MIC, NULL, NULL },
    { SETTING_MUSIC, NULL, NULL },
    { SETTING_VOICE, NULL, NULL },
    { SETTING_SFX, NULL, NULL },
    { SETTING_THEME, NULL, NULL },
#if PLAT_HAS_HOME_THEME
    { SETTING_HOME_THEME, NULL, NULL },
#endif
    { -1, "接続", "NETWORK" },
    { SETTING_CONNECTION, NULL, NULL },
    { SETTING_NETWORK, NULL, NULL },
    { SETTING_SERVER, NULL, NULL },
    { -1, "本体", "SYSTEM" },
    { SETTING_LANGUAGE, NULL, NULL },
#if PLAT_HAS_LID
    { SETTING_LID, NULL, NULL },
#endif
    { SETTING_GUIDE, NULL, NULL },
    { SETTING_SHARE, NULL, NULL },
    { SETTING_SHARE_STATS, NULL, NULL },
    { SETTING_REPORT, NULL, NULL },
    { SETTING_SCREENSHOTS, NULL, NULL },
    { SETTING_COMMUNITY, NULL, NULL },
    { -1, "アカウント", "ACCOUNT & UPDATES" },
    { SETTING_UPDATES, NULL, NULL },
    { SETTING_AUTO_UPDATE, NULL, NULL },
    { SETTING_UPDATE_CHANNEL, NULL, NULL },
    { SETTING_PROVIDER, NULL, NULL },
    { SETTING_ACCOUNT, NULL, NULL },
};

/* Each section's tile: one kanji in an ensō, and what is inside. */
const SectionInfo SECTION_INFO[] = {
    { "操", "Buttons, sticks, camera, gyro, mouse" },
    { "画", "Screen mode, bitrate, sharpness, colour" },
    { "音", "Volume, music, voice, sounds, theme" },
    { "網", "Connection check, type, server" },
    { "本", "Lid, guide, screenshots, reports" },
    { "鍵", "Updates, provider, sign out" },
};
const int SETTING_ENTRY_COUNT = (int)(sizeof(SETTING_ENTRIES) / sizeof(SETTING_ENTRIES[0]));

/* The header entry of a section (NULL past the end). */
const SettingEntry *section_header(int section)
{
    int seen = -1;
    for (int i = 0; i < SETTING_ENTRY_COUNT; ++i)
        if (SETTING_ENTRIES[i].setting < 0 && ++seen == section) return &SETTING_ENTRIES[i];
    return NULL;
}

int screens_section_count(void)
{
    int count = 0;
    for (int i = 0; i < SETTING_ENTRY_COUNT; ++i) count += SETTING_ENTRIES[i].setting < 0;
    return count;
}

int screens_section_of(int position)
{
    int section = -1, seen = 0;
    for (int i = 0; i < SETTING_ENTRY_COUNT; ++i) {
        if (SETTING_ENTRIES[i].setting < 0) ++section;
        else if (seen++ == position) return section;
    }
    return 0;
}

int screens_section_first(int section)
{
    int current = -1, seen = 0;
    for (int i = 0; i < SETTING_ENTRY_COUNT; ++i) {
        if (SETTING_ENTRIES[i].setting < 0) { ++current; continue; }
        if (current == section) return seen;
        ++seen;
    }
    return 0;
}

int screens_section_size(int section)
{
    int current = -1, count = 0;
    for (int i = 0; i < SETTING_ENTRY_COUNT; ++i) {
        if (SETTING_ENTRIES[i].setting < 0) ++current;
        else if (current == section) ++count;
    }
    return count;
}

int screens_setting_at(int position)
{
    int seen = 0;
    for (int i = 0; i < SETTING_ENTRY_COUNT; ++i) {
        if (SETTING_ENTRIES[i].setting < 0) continue;
        if (seen++ == position) return SETTING_ENTRIES[i].setting;
    }
    return SETTING_ACCOUNT;
}

const char *const SETTING_LABELS[SETTING_COUNT] = {
    [SETTING_LAYOUT] = "Button layout", [SETTING_TRIGGERS] = "Triggers",
    [SETTING_PAD_NAMES] = "Button names", [SETTING_MAPPING] = "Button mapping",
    [SETTING_MIC] = "Microphone",
    [SETTING_DEADZONE] = "Stick deadzone", [SETTING_POINTER] = "Mouse mode at start",
    [SETTING_STATS] = "Stream stats", [SETTING_FAST_INPUT] = "Fast input",
    [SETTING_RESOLUTION] = "Screen mode", [SETTING_BITRATE] = "Bitrate",
    [SETTING_FILTER] = "Server sharpening", [SETTING_GYRO] = "Gyro aim",
    [SETTING_VIDEO_SHARPEN] = "Sharpness", [SETTING_VIDEO_COLOR] = "Colour",
    [SETTING_TOUCH_CAMERA] = "Touch camera", [SETTING_TOUCH_STICK_SIZE] = "Touch C-stick size",
    [SETTING_SLIDER] = "3D slider",
    [SETTING_FRAME_RATE] = "Frame rate",
    [SETTING_GYRO_SPEED] = "Gyro speed", [SETTING_ACCOUNT] = "Account",
    [SETTING_CAMERA_SPEED] = "Camera stick speed", [SETTING_CAMERA_INVERT] = "Invert camera",
    [SETTING_THEME] = "Theme", [SETTING_VOLUME] = "Stream volume",
    [SETTING_MENU_AUDIO] = "Audio in menus", [SETTING_LID] = "Closing the lid",
    [SETTING_CONNECTION] = "Connection check", [SETTING_GUIDE] = "Getting started",
    [SETTING_NETWORK] = "Connection type", [SETTING_SERVER] = "Server",
    [SETTING_REPORT] = "Send diagnostic report", [SETTING_SHARE] = "Share problem reports",
    [SETTING_SHARE_STATS] = "Share performance stats", [SETTING_COMMUNITY] = "Kasumi Discord", [SETTING_SCREENSHOTS] = "Screenshots",
    [SETTING_HOME_THEME] = "HOME Menu theme",
    [SETTING_MUSIC] = "Menu music", [SETTING_VOICE] = "Voice", [SETTING_SFX] = "Sound effects",
    [SETTING_UPDATES] = "Software update", [SETTING_AUTO_UPDATE] = "Check automatically",
    [SETTING_UPDATE_CHANNEL] = "Update channel", [SETTING_PROVIDER] = "GeForce NOW provider",
    [SETTING_SERVICE] = "Cloud service", [SETTING_LANGUAGE] = "Language",
    [SETTING_EXTRA_BUTTONS] = "L2 / R2 / L3 / R3",
};
const char *const SETTING_JP[SETTING_COUNT] = {
    [SETTING_LAYOUT] = "ボタン配置", [SETTING_TRIGGERS] = "トリガー",
    [SETTING_PAD_NAMES] = "表記", [SETTING_MAPPING] = "割り当て",
    [SETTING_MIC] = "マイク",
    [SETTING_DEADZONE] = "デッドゾーン", [SETTING_POINTER] = "ポインタ",
    [SETTING_STATS] = "統計", [SETTING_FAST_INPUT] = "高速入力",
    [SETTING_RESOLUTION] = "表示", [SETTING_BITRATE] = "ビットレート",
    [SETTING_FILTER] = "鋭化", [SETTING_GYRO] = "ジャイロ",
    [SETTING_VIDEO_SHARPEN] = "鮮明", [SETTING_VIDEO_COLOR] = "色彩",
    [SETTING_TOUCH_CAMERA] = "タッチ視点", [SETTING_TOUCH_STICK_SIZE] = "大きさ",
    [SETTING_SLIDER] = "3D",
    [SETTING_FRAME_RATE] = "フレーム",
    [SETTING_GYRO_SPEED] = "感度", [SETTING_ACCOUNT] = "アカウント",
    [SETTING_CAMERA_SPEED] = "カメラ速度", [SETTING_CAMERA_INVERT] = "カメラ反転",
    [SETTING_THEME] = "色", [SETTING_VOLUME] = "音量",
    [SETTING_MENU_AUDIO] = "メニュー音", [SETTING_LID] = "スリープ",
    [SETTING_CONNECTION] = "接続", [SETTING_GUIDE] = "案内",
    [SETTING_NETWORK] = "回線", [SETTING_SERVER] = "サーバー",
    [SETTING_REPORT] = "報告", [SETTING_SHARE] = "協力", [SETTING_SHARE_STATS] = "統計",
    [SETTING_COMMUNITY] = "仲間", [SETTING_SCREENSHOTS] = "写真", [SETTING_HOME_THEME] = "壁紙", [SETTING_MUSIC] = "音楽", [SETTING_VOICE] = "声", [SETTING_SFX] = "効果音",
    [SETTING_UPDATES] = "更新", [SETTING_AUTO_UPDATE] = "自動確認",
    [SETTING_UPDATE_CHANNEL] = "チャンネル", [SETTING_PROVIDER] = "提供元", [SETTING_SERVICE] = "サービス",
    [SETTING_LANGUAGE] = "言語",
    [SETTING_EXTRA_BUTTONS] = "追加ボタン",
};

/* NVIDIA's store codes, as people know the stores. */
const char *store_label(const char *code)
{
    static const struct { const char *code, *label; } STORES[] = {
        { "STEAM", "Steam" }, { "EPIC", "Epic Games" }, { "EPIC_GAMES_STORE", "Epic Games" },
        { "EGS", "Epic Games" }, { "EA_APP", "EA app" }, { "ORIGIN", "EA app" },
        { "UBISOFT", "Ubisoft" }, { "UPLAY", "Ubisoft" }, { "UBISOFT_CONNECT", "Ubisoft" },
        { "BATTLENET", "Battle.net" }, { "BATTLE_NET", "Battle.net" }, { "XBOX", "Xbox" },
        { "MICROSOFT", "Xbox" }, { "MICROSOFT_STORE", "Xbox" }, { "GOG", "GOG" },
        { "NV_BUNDLE", "GeForce NOW" }, { "GFN", "GeForce NOW" }, { "UNKNOWN", "Own launcher" },
        { "NONE", "Own launcher" }, { "ROCKSTAR", "Rockstar" }, { "WARGAMING", "Wargaming" },
    };
    if (!code || !code[0]) return "GeForce NOW";
    for (size_t i = 0; i < sizeof(STORES) / sizeof(STORES[0]); ++i)
        if (!strcmp(code, STORES[i].code)) return STORES[i].label;
    return code;
}

const char *gyro_mode_name(GfnGyroMode mode)
{
    return mode == GFN_GYRO_ALWAYS ? "Always" : mode == GFN_GYRO_WHILE_AIMING ? "While aiming" : "Off";
}

/* Server choices: 0 Auto, 1 NVIDIA's pick, then the listed regions. */
static unsigned server_index(const AppSettings *s)
{
    if (!s->server[0]) return 0;
    if (!strcmp(s->server, REGION_CHOICE_NVIDIA)) return 1;
    const unsigned count = regions_count();
    Region region;
    for (unsigned i = 0; i < count; ++i)
        if (regions_get(i, &region) && !strcmp(region.name, s->server)) return 2 + i;
    return 0;
}

static void set_server_index(AppSettings *s, unsigned index)
{
    Region region;
    if (index == 0) s->server[0] = '\0';
    else if (index == 1) snprintf(s->server, sizeof(s->server), "%s", REGION_CHOICE_NVIDIA);
    else if (regions_get(index - 2, &region)) snprintf(s->server, sizeof(s->server), "%s", region.name);
}

/* Provider setting: 0 = Auto, then NVIDIA's list in order. */
static unsigned provider_index(const AppSettings *s)
{
    if (!s->provider[0]) return 0;
    GfnProvider p;
    for (unsigned i = 0; providers_get(i, &p); ++i)
        if (!strcmp(p.code, s->provider)) return i + 1;
    return 0;
}

static void set_provider_index(AppSettings *s, unsigned index)
{
    GfnProvider p;
    if (index == 0 || !providers_get(index - 1, &p)) s->provider[0] = '\0';
    else snprintf(s->provider, sizeof(s->provider), "%s", p.code);
}

/* Current option and option count, for the dot indicator. */
unsigned setting_option(const App *app, int setting, unsigned *count)
{
    const AppSettings *s = &app->settings;
    switch (setting) {
    case SETTING_LAYOUT: *count = 2; return s->button_layout == GFN_LAYOUT_POSITION ? 0 : 1;
    case SETTING_PAD_NAMES: *count = 2; return s->xbox_names ? 1 : 0;
    case SETTING_MIC: *count = 2; return s->mic ? 1 : 0;
    case SETTING_TRIGGERS: *count = 2; return s->swap_shoulders ? 1 : 0;
    case SETTING_DEADZONE: *count = DEADZONE_COUNT; return (unsigned)s->deadzone;
    case SETTING_POINTER: *count = 2; return s->auto_pointer ? 0 : 1;
    case SETTING_STATS: *count = 2; return s->show_stats ? 0 : 1;
    case SETTING_FAST_INPUT: *count = 2; return s->fast_input ? 1 : 0;
    case SETTING_RESOLUTION: *count = 2; return s->wide_video ? 0 : 1;
    case SETTING_BITRATE: *count = STREAM_BITRATE_COUNT; return (unsigned)s->bitrate_mode;
    case SETTING_FILTER: *count = 2; return s->sharpen ? 1 : 0;
    case SETTING_VIDEO_SHARPEN: *count = 4; return s->video_sharpen;
    case SETTING_VIDEO_COLOR: *count = 3; return s->video_color;
    case SETTING_GYRO: *count = GFN_GYRO_MODE_COUNT; return (unsigned)s->gyro_mode;
    case SETTING_GYRO_SPEED: *count = 3; return s->gyro_speed;
    case SETTING_CAMERA_SPEED: *count = 4; return s->camera_speed;
    case SETTING_CAMERA_INVERT: *count = 3; return s->camera_invert;
    case SETTING_TOUCH_CAMERA: *count = 3; return s->touch_camera;
    case SETTING_TOUCH_STICK_SIZE: *count = 3; return s->touch_stick_size;
    case SETTING_SLIDER: *count = gfn_slider_choice_count(); return s->slider;
    case SETTING_FRAME_RATE: *count = 2; return s->fps60 ? 1 : 0;
    case SETTING_THEME: *count = UI_THEME_COUNT; return s->theme;
    case SETTING_LANGUAGE: *count = LANG_COUNT; return s->language;
    case SETTING_VOLUME: *count = 6; return s->volume;
    case SETTING_MENU_AUDIO: *count = 2; return s->mute_in_menus ? 1 : 0;
    case SETTING_LID: *count = LID_MODE_COUNT; return s->lid_mode;
    case SETTING_NETWORK: *count = 2; return s->net_weak ? 1 : 0;
    case SETTING_SHARE: *count = 2; return s->share_reports == SHARE_YES ? 0 : 1;
    case SETTING_SHARE_STATS: *count = 2; return s->share_stats ? 0 : 1;
    case SETTING_SERVER: *count = 2 + regions_count(); return server_index(s);
    case SETTING_PROVIDER: *count = 1 + providers_count(); return provider_index(s);
    case SETTING_SERVICE: *count = 3; return s->steam_service ? 2 : s->xbox_service ? 1 : 0;
    case SETTING_AUTO_UPDATE: *count = 2; return s->auto_update ? 0 : 1;
    case SETTING_UPDATE_CHANNEL: *count = 2; return s->update_beta ? 1 : 0;
    case SETTING_MUSIC: *count = MENU_MUSIC_MODE_COUNT; return s->music_mode;
    case SETTING_VOICE: *count = 2; return s->voice_cues ? 0 : 1;
    case SETTING_SFX: *count = 2; return s->sound_effects ? 0 : 1;
    case SETTING_EXTRA_BUTTONS: *count = EXTRA_BUTTONS_COUNT; return s->extra_buttons;
    default: *count = 0; return 0;
    }
}

const char *setting_value(const App *app, int setting)
{
    const AppSettings *s = &app->settings;
    switch (setting) {
    case SETTING_LAYOUT: return s->button_layout == GFN_LAYOUT_POSITION ? "Position" : "Letters";
    case SETTING_PAD_NAMES: return s->xbox_names ? "Xbox" : "PlayStation";
    case SETTING_MIC: return s->mic ? "On" : "Off";
    case SETTING_MAPPING: return s->has_map ? "Custom" : "Default";
    case SETTING_TRIGGERS: return s->swap_shoulders ? "L / R" : "ZL / ZR";
    case SETTING_DEADZONE:
        return s->deadzone == DEADZONE_SMALL ? "Small" : s->deadzone == DEADZONE_LARGE ? "Large" : "Medium";
    case SETTING_POINTER: return s->auto_pointer ? "Genshin" : "Never";
    case SETTING_STATS: return s->show_stats ? "On" : "Off";
    case SETTING_FAST_INPUT: return s->fast_input ? "On (beta)" : "Off";
    case SETTING_RESOLUTION: return s->wide_video ? "Wide 800" : "Classic 400";
    case SETTING_BITRATE: {
        static const char *const names[STREAM_BITRATE_COUNT] = {
            "Adaptive", "Steady 1 Mbps", "Steady 1.2 Mbps", "Steady 1.5 Mbps", "Sharp"
        };
        return names[s->bitrate_mode];
    }
    case SETTING_FILTER: return s->sharpen ? "On" : "Off";
    case SETTING_VIDEO_SHARPEN: {
        static const char *const names[4] = { "Off", "Low", "Medium", "High" };
        return names[s->video_sharpen < 4 ? s->video_sharpen : 0];
    }
    case SETTING_VIDEO_COLOR: {
        static const char *const names[3] = { "Natural", "Vivid", "Extra vivid" };
        return names[s->video_color < 3 ? s->video_color : 0];
    }
    case SETTING_GYRO: return gyro_mode_name(s->gyro_mode);
    case SETTING_GYRO_SPEED: return s->gyro_speed == 0 ? "Low" : s->gyro_speed == 2 ? "High" : "Medium";
    case SETTING_CAMERA_SPEED:
        return s->camera_speed == 0 ? "Slow" : s->camera_speed == 2 ? "Fast" : s->camera_speed == 3 ? "Fastest" : "Normal";
    case SETTING_CAMERA_INVERT: return s->camera_invert == 1 ? "Up-down" : s->camera_invert == 2 ? "Both" : "Off";
    case SETTING_TOUCH_CAMERA: return s->touch_camera == 1 ? "Stick" : s->touch_camera == 2 ? "Trackpad" : "Off";
    case SETTING_TOUCH_STICK_SIZE:
        return s->touch_stick_size == 0 ? "Small" : s->touch_stick_size == 2 ? "Large" : "Medium";
    case SETTING_SLIDER:
        return s->slider == 0 ? "Off" : s->slider == 1 ? "Zoom" : gfn_output_short_name(gfn_slider_choice_output(s->slider));
    case SETTING_FRAME_RATE: return s->fps60 ? "60 fps (beta)" : "30 fps";
    case SETTING_THEME: return ui_theme_name((UiTheme)s->theme);
    case SETTING_LANGUAGE: return i18n_name((Lang)s->language);
    case SETTING_VOLUME: {
        static const char *const levels[6] = { "Muted", "20 %", "40 %", "60 %", "80 %", "100 %" };
        return levels[s->volume < 6 ? s->volume : 5];
    }
    case SETTING_MENU_AUDIO: return s->mute_in_menus ? "Muted" : "Keep playing";
    case SETTING_LID: return s->lid_mode == LID_KEEP_PLAYING ? "Keep playing" :
                             s->lid_mode == LID_SLEEP ? "Sleep" : "Pause";
    case SETTING_CONNECTION: {
        static char result[48];
        const GfnClient *c = app->client;
        if (!c->conn_tested_at) return "Run test";
        snprintf(result, sizeof(result), i18n_text("%u ms · %u.%u Mbps"), c->conn_latency_ms,
                 c->conn_kbps / 1000, c->conn_kbps % 1000 / 100);
        return result;
    }
    case SETTING_NETWORK: return s->net_weak ? "Weak / hotspot" : "Standard";
    case SETTING_SERVER: {
        static char text[72];
        Region region;
        const unsigned index = server_index(s);
        if (index == 1) return "NVIDIA picks";
        if (index == 0) {
            const int best = regions_fastest();
            if (best >= 0 && regions_get((unsigned)best, &region))
                snprintf(text, sizeof(text), i18n_text("Auto · %s"), region.name);
            else
                snprintf(text, sizeof(text), i18n_text("Auto (lowest ping)"));
            return text;
        }
        if (!regions_get(index - 2, &region)) return s->server;
        if (region.ms >= 0) snprintf(text, sizeof(text), "%s · %d ms", region.name, region.ms);
        else snprintf(text, sizeof(text), "%s", region.name);
        return text;
    }
    case SETTING_GUIDE: return "Open";
    case SETTING_HOME_THEME: return "Download";
    case SETTING_COMMUNITY: return "Scan";
    case SETTING_SCREENSHOTS: {
        static char text[24];
        const unsigned n = gallery_saved_count();
        if (!n) return "None yet";
        snprintf(text, sizeof(text), i18n_text("%u saved"), n);
        return text;
    }
    case SETTING_MUSIC:
        return s->music_mode == MENU_MUSIC_QUIET ? "Quiet" : s->music_mode == MENU_MUSIC_OFF ? "Off" : "On";
    case SETTING_VOICE: return s->voice_cues ? "On" : "Off";
    case SETTING_SFX: return s->sound_effects ? "On" : "Off";
    case SETTING_EXTRA_BUTTONS:
        return s->extra_buttons == EXTRA_BUTTONS_FRONT ? "Touch screen"
             : s->extra_buttons == EXTRA_BUTTONS_OFF ? "Off" : "Rear touchpad";
    case SETTING_REPORT: return report_available() ? "Send" : "Unavailable";
    case SETTING_SHARE: return s->share_reports == SHARE_YES ? "On" : "Off";
    case SETTING_SHARE_STATS: return s->share_stats ? "On" : "Off";
    case SETTING_UPDATES: {
        static char text[48];
        const UpdateInfo info = updater_info();
        if (info.state == UPDATE_AVAILABLE) snprintf(text, sizeof(text), "%s ready", info.latest);
        else if (info.state == UPDATE_UP_TO_DATE) snprintf(text, sizeof(text), i18n_text("Up to date"));
        else if (info.state == UPDATE_INSTALLED) snprintf(text, sizeof(text), i18n_text("Restart to finish"));
        else snprintf(text, sizeof(text), "v%s", APP_VERSION);
        return text;
    }
    case SETTING_AUTO_UPDATE: return s->auto_update ? "On" : "Off";
    case SETTING_UPDATE_CHANNEL: return updater_dev_mode() ? "Dev (your PC)" : s->update_beta ? "Beta" : "Stable";
    case SETTING_ACCOUNT: return gfn_has_session(app->client) ? "Sign out" : "Signed out";
    case SETTING_SERVICE:
        return s->steam_service ? "Steam Link" : s->xbox_service ? "Xbox Cloud (beta)" : "GeForce NOW";
    case SETTING_PROVIDER: {
        static char text[56];
        GfnProvider p;
        if (!s->provider[0]) {
            snprintf(text, sizeof(text), i18n_text("NVIDIA (default)"));
        } else if (providers_find(s->provider, &p)) {
            snprintf(text, sizeof(text), "%s", p.name);
        } else {
            snprintf(text, sizeof(text), "%s", s->provider);
        }
        return text;
    }
    }
    return "";
}

#ifdef __vita__
/* Where the Vita's hardware or numbers differ from the 3DS's (the rest is
 * shared, with "3DS", "C-Stick" and ZL / ZR reworded by i18n.c). */
static const char *vita_description(const App *app, int setting)
{
    const AppSettings *s = &app->settings;
    switch (setting) {
    case SETTING_LAYOUT:
        if (s->has_map) return NULL;
        if (s->xbox_names)
            return s->button_layout == GFN_LAYOUT_POSITION
                ? "Buttons match their place on the pad: Cross is A, Circle is B, like an Xbox controller."
                : "By letter: Circle sends A and Cross sends B, the Nintendo way round.";
        return s->button_layout == GFN_LAYOUT_POSITION
            ? "Buttons match their place: Cross is Cross, Circle is Circle. Plays like a PlayStation controller."
            : "Cross and Circle swapped, the Nintendo way round: Circle confirms in most games.";
    case SETTING_MIC:
        return s->mic
            ? "Voice chat in games: MIC at the top of the picture (or in the stream menu) talks, tap again to mute "
              "(starts muted). Use headphones, or your team hears the game. Next game."
            : "Talk in games with voice chat, using the Vita's microphone. Off: nothing is ever recorded.";
    case SETTING_MAPPING:
        return s->has_map ? NULL
                          : "Choose what each Vita button sends, for every game: two buttons at once, turbo, toggle, "
                            "or a stick push.";
    case SETTING_TRIGGERS:
        if (s->xbox_names)
            return s->swap_shoulders ? "L and R act as the LT / RT triggers; the touch L2 / R2 become LB / RB."
                                     : "L and R are the LB / RB bumpers; L2 / R2 (rear touchpad or screen edges) are LT / RT.";
        return s->swap_shoulders ? "L and R act as the L2 / R2 triggers; the touch L2 / R2 become L1 / R1."
                                 : "L and R are the L1 / R1 bumpers; L2 / R2 come from the rear touchpad or the screen's edges.";
    case SETTING_STATS:
        return "Show frame rate, bitrate, ping and resent packets at the bottom of the picture while playing.";
    case SETTING_BITRATE:
        return s->bitrate_mode == STREAM_BITRATE_ADAPTIVE
            ? "About 3-5 Mbps: a clean, detailed picture on home Wi-Fi. The default. Next launch."
            : s->bitrate_mode == STREAM_BITRATE_SHARP_TEST
            ? "About 5-8 Mbps: the sharpest picture. Needs strong Wi-Fi, near the router. Next launch."
            : s->bitrate_mode == STREAM_BITRATE_STEADY_1000
            ? "About 1 Mbps: smoothest on weak Wi-Fi or a phone hotspot, but soft on the Vita's screen. Next launch."
            : "A fixed rate. If the stats show RESENT climbing, pick a lower one. Next launch.";
    case SETTING_FRAME_RATE:
        return s->fps60 ? "60 frames a second: smoother motion and quicker controls. The Vita keeps up. Next launch."
                        : "30 frames a second: a little more detail in each frame on weak Wi-Fi. Next launch.";
    case SETTING_TOUCH_STICK_SIZE:
        return s->touch_stick_size == 0 ? "A short drag turns at full speed: quick, for fast games."
             : s->touch_stick_size == 2 ? "A long drag for full speed: finer control when aiming slowly."
                                        : "How far you drag the touch camera for full speed. Medium suits most games.";
    case SETTING_TOUCH_CAMERA:
        return s->touch_camera == 0
            ? "Touching the picture does nothing (except the mouse, when it is on)."
            : s->touch_camera == 1
            ? "Turn on TOUCH CAM in the stream menu, then touch and push like a stick: hold it out to keep turning. "
              "Double-tap for R3."
            : "Turn on TOUCH CAM in the stream menu, then drag to turn the camera; it stops when your finger does. "
              "Double-tap for R3.";
    case SETTING_GYRO:
        return s->gyro_mode == GFN_GYRO_OFF
            ? "Tilt and turn the Vita to aim, like a Switch or Steam Deck. Adds to the right stick; moves the mouse in mouse mode."
            : s->gyro_mode == GFN_GYRO_ALWAYS
            ? "Turning the Vita always moves the camera. Great for shooters; hold it still when you don't aim."
            : "Gyro only works while the aim trigger (L2, or L when triggers are swapped) is held.";
    case SETTING_GYRO_SPEED:
        return "How fast turning the Vita moves the camera. Start at Medium and lower it if aiming overshoots.";
    case SETTING_CAMERA_SPEED:
        return s->camera_speed == 0 ? "The right stick turns the camera at most 70 % as fast. For precise aiming."
             : s->camera_speed == 1 ? "The right stick as it is: a full push turns the camera at full speed."
             : s->camera_speed == 2 ? "A lighter push on the right stick turns the camera at full speed."
                                    : "Full camera speed with a light push. For games that turn slowly.";
    case SETTING_CAMERA_INVERT:
        return s->camera_invert == 0 ? "The right stick moves the camera the usual way."
             : s->camera_invert == 1 ? "Pushing the right stick up looks down, like a flight stick. Gyro aim is not inverted."
                                     : "Both directions of the right stick are reversed. Gyro aim is not inverted.";
    case SETTING_THEME:
        return "The colour and the wallpaper behind the menus. Seiji, Sakura, Kin, Ai, Fuji, Beni, Matcha, Kaki, Sumi or Shiro.";
    case SETTING_VOLUME:
        return "Game audio volume in Kasumi, on top of the Vita's own volume buttons.";
    case SETTING_MUSIC: {
        static char text[240];
        const char *song = menu_audio_now_playing();
        if (s->music_mode != MENU_MUSIC_OFF && song[0])
            snprintf(text, sizeof(text), "Now playing: %.80s. Triangle: next song. Your own MP3 or Opus songs in "
                     "ux0:data/kasumi/music play instead.", song);
        else
            snprintf(text, sizeof(text), "Soft music in the menus that fades out when a game starts. Your own MP3 "
                     "or Opus songs in ux0:data/kasumi/music play instead of the built-in ones.");
        return text;
    }
    case SETTING_SCREENSHOTS:
        return "Look through the screenshots you took in games (stream menu > Screenshot). Tap to open; the arrows "
               "browse, DELETE removes one.";
    default:
        return NULL;
    }
}
#endif

const char *setting_description(const App *app, int setting)
{
    const AppSettings *s = &app->settings;
#ifdef __vita__
    const char *vita = vita_description(app, setting);
    if (vita) return vita;
#endif
    switch (setting) {
    case SETTING_LAYOUT:
        if (s->has_map) return "Your button mapping (below) decides every button, so this layout is not used.";
        if (s->xbox_names)
            return s->button_layout == GFN_LAYOUT_POSITION
                ? "Buttons match their place on the pad: bottom is A, right is B, like an Xbox controller. 3DS A sends B."
                : "The printed letters match: 3DS A sends A, 3DS B sends B. A and B sit swapped compared with an Xbox pad.";
        return s->button_layout == GFN_LAYOUT_POSITION
            ? "Buttons match their place on the pad: bottom is Cross, right is Circle. Plays like a PlayStation controller."
            : "The printed letters match: 3DS A sends A. Cross and Circle end up swapped compared with a PlayStation pad.";
    case SETTING_MIC:
        return s->mic
            ? "Voice chat in games: tap MIC on the lower screen to talk, again to mute (starts muted). Use headphones, "
              "or your team hears the game through the mic. Next game."
            : "Talk in games with voice chat, using the 3DS microphone. Off: nothing is ever recorded.";
    case SETTING_PAD_NAMES:
        return s->xbox_names
            ? "Kasumi shows Xbox buttons: A, B, LB, RT, Menu. Most PC games on GeForce NOW show these too."
            : "Kasumi shows PlayStation buttons: Cross, Circle, L1, R2, Options.";
    case SETTING_MAPPING:
        return s->has_map
            ? "Your own map for every game: press A to change it. A game's own mapping (game page > X > Options) goes first."
            : "Choose what each 3DS button sends, for every game: two buttons at once, turbo, toggle, or a stick push.";
    case SETTING_TRIGGERS:
        if (s->xbox_names)
            return s->swap_shoulders
                ? "The big L and R buttons act as the LT / RT triggers; ZL and ZR become LB / RB."
                : "ZL and ZR are the LT / RT triggers; L and R are the LB / RB bumpers.";
        return s->swap_shoulders
            ? "The big L and R buttons act as the L2 / R2 triggers; ZL and ZR become L1 / R1."
            : "ZL and ZR are the L2 / R2 triggers; L and R are the L1 / R1 bumpers.";
    case SETTING_DEADZONE:
        return "How far a stick moves before the game notices. Raise it if a character drifts on its own.";
    case SETTING_POINTER:
        return "Start Genshin Impact in mouse and keyboard mode so you can click through its PC login screen. Tap MOUSE in game to go back to controller mode.";
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
            ? "About 1.3 Mbps: a clean, detailed picture on good home Wi-Fi. The default. Next launch."
            : s->bitrate_mode == STREAM_BITRATE_SHARP_TEST
            ? "About 1.8-2 Mbps: the sharpest picture. Needs strong Wi-Fi (3 bars, near the router). Next launch."
            : s->bitrate_mode == STREAM_BITRATE_STEADY_1000
            ? "About 1 Mbps: smoothest on weak Wi-Fi or a phone hotspot, a little softer. Next launch."
            : "A fixed rate. If the stats show RESENT/S climbing, pick a lower one. Next launch.";
    case SETTING_FRAME_RATE:
        if (!s->wide_video) return "Needs Screen mode: Wide 800. 60 frames a second for smoother motion.";
        return s->fps60
            ? "Smoother motion and quicker controls. Most games keep up; a very heavy one can hitch now and then (Kasumi says so), and 30 suits it better. Next launch."
            : "30 frames a second: the most detail in every frame. Next launch.";
    case SETTING_EXTRA_BUTTONS:
        return s->extra_buttons == EXTRA_BUTTONS_FRONT
            ? "In games, four buttons on the screen's edges: L2 above L3 on the left, R2 above R3 on the right. "
              "The rear touchpad does nothing."
            : s->extra_buttons == EXTRA_BUTTONS_OFF
            ? "No L2 / R2 / L3 / R3 from touch. A PS TV controller still has them."
            : "In games, the rear touchpad's corners: top left L2, top right R2, bottom left L3, bottom right R3. "
              "Its middle, where fingers rest, does nothing.";
    case SETTING_TOUCH_STICK_SIZE:
        return s->touch_stick_size == 0 ? "A short push turns at full speed: quick, for small thumbs or fast games."
             : s->touch_stick_size == 2 ? "A long push for full speed: finer control when aiming slowly."
                                        : "How far you push the touch C-stick for full speed. Medium suits most games.";
    case SETTING_SLIDER: {
        static char text[140];
        if (s->slider == 0) return "The 3D slider does nothing in games. Slide it to choose zoom or a button.";
        if (s->slider == 1) return "In a game, slide the 3D slider up to zoom in, in three steps; all the way down shows the whole picture.";
        snprintf(text, sizeof(text), i18n_text("In a game, the 3D slider past halfway holds %s until you slide it back down."),
                 gfn_output_name(gfn_slider_choice_output(s->slider)));
        return text;
    }
    case SETTING_TOUCH_CAMERA:
        return s->touch_camera == 0
            ? "No C-STICK button in games: the lower screen keeps its stats and buttons."
            : s->touch_camera == 1
            ? "In a game, tap C-STICK by PS. Touch and push, like the C-Stick: hold it out to keep turning. Double-tap for R3."
            : "In a game, tap C-STICK by PS, then drag to turn the camera; it stops when your finger does. Double-tap for R3.";
    case SETTING_VIDEO_SHARPEN:
        if (!s->wide_video) return "Needs Screen mode: Wide 800. Sharpens edges and text on the console, at no cost to the stream.";
        return s->video_sharpen == 0 ? "The picture exactly as it arrives."
             : s->video_sharpen == 3 ? "Strong sharpening on the console: crispest text, but blocky spots in fast scenes stand out more."
                                     : "Sharpens edges and small text on the console, at no cost to the stream or the frame rate.";
    case SETTING_VIDEO_COLOR:
        if (!s->wide_video) return "Needs Screen mode: Wide 800. Richer colour and contrast for the 3DS screen.";
        return s->video_color == 0 ? "Colours exactly as the game sends them."
             : s->video_color == 1 ? "A little more colour and contrast, so games look less washed out on the 3DS screen."
                                   : "Strong colour and contrast. Fun for colourful games; skin and skies can look overdone.";
    case SETTING_FILTER:
        return s->sharpen
            ? "NVIDIA sharpens before compressing, which spends scarce bitrate on edges: more blocking and pulsing. Sharpness above does it on the console for free. Next launch."
            : "Recommended. The bitrate goes to the picture itself; use Sharpness above to sharpen on the console instead. Next launch.";
    case SETTING_GYRO:
        return s->gyro_mode == GFN_GYRO_OFF
            ? "Tilt and turn the console to aim, like a Switch or Steam Deck. Adds to the C-Stick; moves the mouse in mouse mode."
            : s->gyro_mode == GFN_GYRO_ALWAYS
            ? "Turning the console always moves the camera. Great for shooters; hold the console still when you don't aim."
            : "Gyro only works while the aim trigger (ZL, or L when triggers are swapped) is held.";
    case SETTING_GYRO_SPEED:
        return "How fast turning the console moves the camera. Start at Medium and lower it if aiming overshoots.";
    case SETTING_CAMERA_SPEED:
        return s->camera_speed == 0 ? "The C-Stick turns the camera at most 70 % as fast. For precise aiming."
             : s->camera_speed == 1 ? "The C-Stick as it is: a full push turns the camera at full speed."
             : s->camera_speed == 2 ? "A lighter push on the C-Stick turns the camera at full speed."
                                    : "Full camera speed with a light push. For games that turn slowly.";
    case SETTING_CAMERA_INVERT:
        return s->camera_invert == 0 ? "The C-Stick moves the camera the usual way."
             : s->camera_invert == 1 ? "Pushing the C-Stick up looks down, like a flight stick. Gyro aim is not inverted."
                                     : "Both directions of the C-Stick are reversed. Gyro aim is not inverted.";
    case SETTING_THEME:
        return "The colour and the lower screen's wallpaper. Seiji, Sakura, Kin, Ai, Fuji, Beni, Matcha, Kaki, Sumi or Shiro.";
    case SETTING_LANGUAGE:
        return "The language of Kasumi's menus. Game titles and store names stay as they are.";
    case SETTING_HOME_THEME:
        return "A Kasumi theme for the 3DS HOME Menu, made by morrisonion. A downloads his latest version to "
               "/Themes; install it with Anemone3DS.";
    case SETTING_VOLUME:
        if (audio_system_firmware_missing())
            return "No sound on this 3DS yet: its sound firmware (dspfirm.cdc) is missing. Run DSP1 once from the "
                   "Homebrew Launcher, then restart Kasumi.";
        return "Game audio volume on this console, on top of the 3DS volume slider.";
    case SETTING_MENU_AUDIO:
        return s->mute_in_menus
            ? "Game audio goes quiet while the stream menu or controls sheet is open."
            : "Game audio keeps playing while the stream menu is open.";
    case SETTING_CONNECTION: return connection_advice(app->client);
    case SETTING_NETWORK:
        return s->net_weak
            ? "For far-away Wi-Fi or a phone hotspot: a steadier 0.6-1 Mbps picture, a longer wait for lost packets and a bigger buffer. Softer image, a little more delay. Next launch."
            : "For home Wi-Fi near the router: the sharpest picture and lowest delay. Next launch.";
    case SETTING_SERVER: {
        const unsigned index = server_index(s);
        if (index == 0)
            return "Kasumi pings every GeForce NOW server and uses the fastest, measured again on each new Wi-Fi network (like a phone hotspot).";
        if (index == 1)
            return "NVIDIA chooses from your internet address. On mobile data that can be a far-away server.";
        return "Always use this server. Your ping and queue depend on it; run Connection check to see ping to each one.";
    }
    case SETTING_GUIDE: return "Walk through the basics again: signing in, controls, picture and extras.";
    case SETTING_SHARE:
        return s->share_reports == SHARE_YES
            ? "When something goes wrong (a crash, freeze or failed stream), Kasumi sends its log to the developer on its own, at most once per run. No login or passwords."
            : "Kasumi never sends anything on its own. Turn on to send the log automatically when something goes wrong, which helps fix bugs faster.";
    case SETTING_SHARE_STATS:
        return s->share_stats
            ? "After each launch and session, Kasumi sends a few numbers: did the game start, queue time, ping, smoothness, lost frames. No log text, no addresses, no account."
            : "Turn on to send a few numbers after each launch and session (did the game start, ping, smoothness). It shows what to improve for real players.";
    case SETTING_MUSIC: {
        static char text[240];
        const char *song = menu_audio_now_playing();
        if (s->music_mode != MENU_MUSIC_OFF && song[0])
            snprintf(text, sizeof(text), "Now playing: %.80s. X: next song. Your own MP3 or Opus songs in "
                     "3ds/kasumi/music play instead.", song);
        else
            snprintf(text, sizeof(text), "Soft music in the menus that fades out when a game starts. Your own MP3 "
                     "or Opus songs in 3ds/kasumi/music play instead of the built-in ones.");
        return text;
    }
    case SETTING_VOICE:
        return s->voice_cues ? "Tsumugi welcomes you back when Kasumi opens and sees you off when a game starts."
                             : "No voice lines. Turn on for Tsumugi's welcome and send-off.";
    case SETTING_SFX:
        return s->sound_effects ? "Soft koto, wood and water sounds in the menus. In a game, only the stream menu and screenshots make a sound."
                                : "Silent menus. The \"your game is ready\" chime still plays when a queue ends.";
    case SETTING_SCREENSHOTS:
        return "Look through the screenshots you took in games (stream menu > Screenshot). Press A to open; L and R browse, X deletes.";
    case SETTING_COMMUNITY:
        return "Chat with other players, get help and hear about new versions first. Scan with your phone, or visit discord.gg/K9Jy3t7YHE";
    case SETTING_REPORT:
        return "Having a problem? Send this run's and the last run's log to Kasumi's developer and get a code to share. Only when you choose; nothing is sent otherwise.";
    case SETTING_UPDATES:
        return "See what's new and install the latest Kasumi from GitHub. Your login, library and settings stay.";
    case SETTING_AUTO_UPDATE:
        return s->auto_update ? "Kasumi looks for a new version every few hours, only in the menus, never while you play."
                              : "Kasumi only looks for updates when you open Software update.";
    case SETTING_UPDATE_CHANNEL:
        if (updater_dev_mode())
            return "dev_server.txt is on the SD card: updates come from tools/dev_server.py on your PC. Delete the file to go back.";
        return s->update_beta ? "Beta: get test versions first. They may have rough edges."
                              : "Stable: only finished releases.";
    case SETTING_LID:
        return s->lid_mode == LID_KEEP_PLAYING
            ? "Closing the lid turns the screens off; the game and its sound keep running."
            : s->lid_mode == LID_SLEEP
            ? "Closing the lid sleeps the console to save battery. On opening it, Kasumi reconnects to the same rig."
            : "Closing the lid turns the screens and sound off but stays connected: open it and you are straight back in.";
    case SETTING_ACCOUNT:
        if (app->settings.steam_service)
            return "Forget the paired PC. Pair again to stream from it; your cloud logins stay.";
        return app->settings.xbox_service
            ? "Remove the saved Microsoft (Xbox) login from this console's SD card. The NVIDIA one stays."
            : "Remove the saved NVIDIA login from this console's SD card. An Xbox one stays.";
    case SETTING_SERVICE:
        return "GeForce NOW, Xbox Cloud Gaming (Game Pass, and free games like Fortnite), or Steam Link: play "
               "from your own PC running Steam, on the same Wi-Fi. Each keeps its own login. Xbox and Steam Link "
               "are experimental.";
    case SETTING_PROVIDER: {
        /* Signed in through another provider than the one chosen: say how
         * to switch (the login belongs to its provider). */
        static char text[240];
        GfnProvider active, chosen, local;
        provider_active(&active);
        if (!s->provider[0] || !providers_find(s->provider, &chosen)) provider_nvidia(&chosen);
        if (gfn_has_session(app->client) && strcmp(active.code, chosen.code)) {
            snprintf(text, sizeof(text), i18n_text("Signed in with %s. To use %s, sign out below and sign in again."),
                     active.name, chosen.name);
            return text;
        }
        providers_recommended(&local);
        if (strcmp(local.code, PROVIDER_NVIDIA)) {
            snprintf(text, sizeof(text), i18n_text("Most accounts are NVIDIA's. Here GeForce NOW is also sold by %.30s: "
                     "if your subscription is from %.30s, pick it and sign in again. Beta."), local.name, local.name);
            return text;
        }
        return "Most accounts are NVIDIA's. In some countries (Japan, Korea, Taiwan, the Middle East...) "
               "GeForce NOW is sold by a local partner: if yours is, pick it, then sign in. Beta.";
    }
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
    case SETTING_PAD_NAMES: s->xbox_names = !s->xbox_names; break;
    case SETTING_MIC: s->mic = !s->mic; break;
    case SETTING_DEADZONE:
        s->deadzone = (DeadzoneLevel)((s->deadzone + DEADZONE_COUNT + step) % DEADZONE_COUNT);
        break;
    case SETTING_POINTER: s->auto_pointer = !s->auto_pointer; break;
    case SETTING_STATS: s->show_stats = !s->show_stats; break;
    case SETTING_FAST_INPUT: s->fast_input = !s->fast_input; break;
    case SETTING_RESOLUTION: s->wide_video = !s->wide_video; break;
    case SETTING_FILTER: s->sharpen = !s->sharpen; break;
    case SETTING_VIDEO_SHARPEN: s->video_sharpen = (s->video_sharpen + 4 + step) % 4; break;
    case SETTING_VIDEO_COLOR: s->video_color = (s->video_color + 3 + step) % 3; break;
    case SETTING_GYRO:
        s->gyro_mode = (GfnGyroMode)((s->gyro_mode + GFN_GYRO_MODE_COUNT + step) % GFN_GYRO_MODE_COUNT);
        break;
    case SETTING_GYRO_SPEED: s->gyro_speed = (s->gyro_speed + 3 + step) % 3; break;
    case SETTING_CAMERA_SPEED: s->camera_speed = (s->camera_speed + 4 + step) % 4; break;
    case SETTING_CAMERA_INVERT: s->camera_invert = (s->camera_invert + 3 + step) % 3; break;
    case SETTING_TOUCH_CAMERA: s->touch_camera = (s->touch_camera + 3 + step) % 3; break;
    case SETTING_TOUCH_STICK_SIZE: s->touch_stick_size = (s->touch_stick_size + 3 + step) % 3; break;
    case SETTING_EXTRA_BUTTONS:
        s->extra_buttons = (s->extra_buttons + EXTRA_BUTTONS_COUNT + (unsigned)step) % EXTRA_BUTTONS_COUNT;
        break;
    case SETTING_SLIDER: {
        const unsigned n = gfn_slider_choice_count();
        s->slider = (s->slider + n + step) % n;
        break;
    }
    case SETTING_FRAME_RATE: s->fps60 = !s->fps60; break;
    case SETTING_THEME: s->theme = (s->theme + UI_THEME_COUNT + step) % UI_THEME_COUNT; break;
    case SETTING_LANGUAGE:
        /* Applied at once, so the menu on screen changes language too. */
        s->language = (s->language + LANG_COUNT + step) % LANG_COUNT;
        i18n_set((Lang)s->language);
        break;
    case SETTING_VOLUME: s->volume = (s->volume + 6 + step) % 6; break;
    case SETTING_MENU_AUDIO: s->mute_in_menus = !s->mute_in_menus; break;
    case SETTING_MUSIC:
        s->music_mode = (s->music_mode + MENU_MUSIC_MODE_COUNT + (unsigned)step) % MENU_MUSIC_MODE_COUNT;
        break;
    case SETTING_VOICE: s->voice_cues = !s->voice_cues; break;
    case SETTING_SFX: s->sound_effects = !s->sound_effects; break;
    case SETTING_LID: s->lid_mode = (s->lid_mode + LID_MODE_COUNT + step) % LID_MODE_COUNT; break;
    case SETTING_NETWORK: s->net_weak = !s->net_weak; break;
    case SETTING_SHARE: s->share_reports = s->share_reports == SHARE_YES ? SHARE_NO : SHARE_YES; break;
    case SETTING_SHARE_STATS: s->share_stats = !s->share_stats; break;
    case SETTING_SERVER: {
        const unsigned count = 2 + regions_count();
        set_server_index(s, (server_index(s) + (step < 0 ? count - 1 : 1)) % count);
        break;
    }
    case SETTING_SERVICE: {
        /* GeForce NOW -> Xbox -> Steam Link, and back. */
        const unsigned index = (unsigned)((s->steam_service ? 2 : s->xbox_service ? 1 : 0) + 3 + step) % 3;
        s->xbox_service = index == 1;
        s->steam_service = index == 2;
        break;
    }
    case SETTING_PROVIDER: {
        const unsigned count = 1 + providers_count();
        set_provider_index(s, (provider_index(s) + (step < 0 ? count - 1 : 1)) % count);
        break;
    }
    case SETTING_AUTO_UPDATE: s->auto_update = !s->auto_update; break;
    case SETTING_UPDATE_CHANNEL: s->update_beta = !s->update_beta; break;
    case SETTING_BITRATE:
        s->bitrate_mode = (StreamBitrateMode)((s->bitrate_mode + STREAM_BITRATE_COUNT + step) %
                                              STREAM_BITRATE_COUNT);
        break;
    default: break;
    }
}

u32 service_light(int service)
{
    if (service == SERVICE_XBOX) return UI_RGBA(0x46, 0xD2, 0x5C, 0xFF);
    if (service == SERVICE_STEAM) return UI_RGBA(0x66, 0xC0, 0xF4, 0xFF);
    return UI_RGBA(0x92, 0xD4, 0x16, 0xFF);
}

/* Each service's mark on its seal. */
const char *const SERVICE_KANJI[SERVICE_COUNT] = { "雲", "竹", "湯" };

/* Each service's banner art and emblem (tools/gen_service_art.py). */
const UiImage SERVICE_ART[SERVICE_COUNT] = { UI_IMAGE_SVC_GFN, UI_IMAGE_SVC_XBOX, UI_IMAGE_SVC_STEAM };
const UiImage SERVICE_ICON[SERVICE_COUNT] = { UI_IMAGE_ICON_GFN, UI_IMAGE_ICON_XBOX, UI_IMAGE_ICON_STEAM };

/* Steam Link's own entries have covers of their own. */
UiImage special_cover(const GfnGame *game)
{
    if (!game) return UI_IMAGE_COUNT;
    if (!strcmp(game->app_id, "steam:bigpicture")) return UI_IMAGE_COVER_BIGPICTURE;
    if (!strcmp(game->app_id, "steam:desktop")) return UI_IMAGE_COVER_DESKTOP;
    return UI_IMAGE_COUNT;
}

/* The hub's card for the service in use: its last game, if any. */
const GfnGame *hub_last_game(const App *app)
{
    if (app->hub_index != app_service(app) || !gfn_has_session(app->client)) return NULL;
    if (app->continue_index < 0 || (size_t)app->continue_index >= app->client->game_count) return NULL;
    return &app->client->games[app->continue_index];
}

/* A sign-in in progress through Xbox Cloud Gaming (Microsoft's code). */
bool login_is_xbox(const GfnClient *client)
{
    return !strcmp(client->login_provider.code, PROVIDER_XBOX);
}

/* A pairing with a PC (Steam Link): the code goes into Steam there. */
bool login_is_steam(const GfnClient *client)
{
    return !strcmp(client->login_provider.code, PROVIDER_STEAM);
}

/* Which service a sign-in in progress belongs to. */
int login_service(const GfnClient *client)
{
    return login_is_steam(client) ? SERVICE_STEAM : login_is_xbox(client) ? SERVICE_XBOX : SERVICE_GFN;
}

/* "synced 2 h ago" for the saved library ("" when unknown). */
void library_age(const GfnClient *client, char *out, size_t size)
{
    out[0] = '\0';
    if (!client->library_saved_at) return;
    const long age = (long)((int64_t)time(NULL) - client->library_saved_at);
    if (age < 120) snprintf(out, size, i18n_text("synced just now"));
    else if (age < 7200) snprintf(out, size, i18n_text("synced %ld min ago"), age / 60);
    else if (age < 172800) snprintf(out, size, i18n_text("synced %ld h ago"), age / 3600);
    else snprintf(out, size, i18n_text("synced %ld days ago"), age / 86400);
}

/* How a game will be streamed, for the library card and the game page. */
const char *stream_line(const App *app)
{
    static char line[96];
    switch (app_service(app)) {
    case SERVICE_STEAM:
        snprintf(line, sizeof(line), i18n_text("From %s  ·  800x480 wide  ·  %u fps"),
                 steam_link_host_name()[0] ? steam_link_host_name() : i18n_text("your PC"), stream_profile_fps());
        return line;
    case SERVICE_XBOX:
        snprintf(line, sizeof(line), i18n_text("Xbox Cloud  ·  800x480 wide  ·  60 fps"));
        return line;
    default:
        return stream_profile_name();
    }
}

int session_stage(const App *app)
{
    const GfnClient *client = app->client;
    if (client->session_state == GFN_SESSION_QUEUED) return 0;
    if (client->session_state == GFN_SESSION_SETUP) return 1;
    if (webrtc_transport_gameplay_ready(app->transport)) return 3;
    return 2;
}

bool screens_session_failed(const App *app)
{
    return app->client->session_state == GFN_SESSION_ERROR ||
           app->transport->state == WEBRTC_FAILED ||
           app->signal->state == NVST_SIGNAL_ERROR;
}

const char *details_store(const App *app, const GfnGame *game)
{
    if (app->details_variant < game->variant_count) return store_label(game->variants[app->details_variant].store);
    return store_label(game->store);
}

void format_played(char *out, size_t size, uint32_t seconds)
{
    if (seconds >= 3600) snprintf(out, size, i18n_text("%lu h %02lu m"), (unsigned long)(seconds / 3600),
                                  (unsigned long)(seconds / 60 % 60));
    else snprintf(out, size, i18n_text("%lu min"), (unsigned long)(seconds / 60));
}

/* The per-game options sheet: rows of "setting  < value >". */
const char *option_value(const App *app, const GamePrefs *prefs, int row, char *buffer, size_t size)
{
    static const char *const bitrates[STREAM_BITRATE_COUNT] = { "Adaptive", "Steady 1", "Steady 1.2", "Steady 1.5", "Sharp" };
    static const char *const gyros[GFN_GYRO_MODE_COUNT] = { "Off", "Always", "While aiming" };
    static const char *const layouts[2] = { "Position", "Letters" };
    static const char *const speeds[4] = { "Slow", "Normal", "Fast", "Fastest" };
    static const char *const inverts[3] = { "Off", "Up-down", "Both" };
    static const char *const gyro_speeds[3] = { "Low", "Medium", "High" };
    static const char *const touches[3] = { "Off", "Stick", "Trackpad" };
    const AppSettings *s = &app->settings;
    const GfnClient *c = app->client;
    /* This game's value, or "Default (what Settings says)", so a game's
     * own choice is never a surprise. */
    const char *const *names = NULL;
    int own = -1;
    unsigned global = 0, count = 1;
    switch (row) {
    case OPTION_BITRATE: names = bitrates; own = prefs->bitrate; global = s->bitrate_mode; count = STREAM_BITRATE_COUNT; break;
    case OPTION_CAMERA_SPEED: names = speeds; own = prefs->camera_speed; global = s->camera_speed; count = 4; break;
    case OPTION_CAMERA_INVERT: names = inverts; own = prefs->camera_invert; global = s->camera_invert; count = 3; break;
    case OPTION_TOUCH_CAMERA: names = touches; own = prefs->touch_camera; global = s->touch_camera; count = 3; break;
    case OPTION_GYRO: names = gyros; own = prefs->gyro; global = s->gyro_mode; count = GFN_GYRO_MODE_COUNT; break;
    case OPTION_GYRO_SPEED: names = gyro_speeds; own = prefs->gyro_speed; global = s->gyro_speed; count = 3; break;
    case OPTION_LAYOUT: names = layouts; own = prefs->layout; global = s->button_layout; count = 2; break;
    case OPTION_MAPPING: return prefs->has_map ? "Custom" : "Default";
    case OPTION_CONNECTION:
        if (!c->conn_tested_at) return "Not tested";
        snprintf(buffer, size, i18n_text("%u ms  ·  %u.%u Mbps  ·  %u/3"), c->conn_latency_ms, c->conn_kbps / 1000,
                 c->conn_kbps % 1000 / 100, c->conn_bars);
        return buffer;
    }
    if (!names) return "";
    if (own >= 0) return names[(unsigned)own % count];
    snprintf(buffer, size, i18n_text("Default (%s)"), names[global % count]);
    return buffer;
}

/* Whether this game changes the row from Settings. */
bool option_custom(const GamePrefs *prefs, int row)
{
    switch (row) {
    case OPTION_BITRATE: return prefs->bitrate >= 0;
    case OPTION_CAMERA_SPEED: return prefs->camera_speed >= 0;
    case OPTION_CAMERA_INVERT: return prefs->camera_invert >= 0;
    case OPTION_TOUCH_CAMERA: return prefs->touch_camera >= 0;
    case OPTION_GYRO: return prefs->gyro >= 0;
    case OPTION_GYRO_SPEED: return prefs->gyro_speed >= 0;
    case OPTION_LAYOUT: return prefs->layout >= 0;
    case OPTION_MAPPING: return prefs->has_map;
    }
    return false;
}

/* What the last connection check means for play. */
const char *connection_advice(const GfnClient *c)
{
    static char text[160];
    if (!c->conn_tested_at) return "Measures Wi-Fi, latency and speed to NVIDIA, and pings every server.";
    const char *advice = c->conn_bars < 2 || c->conn_kbps < 2000
        ? "Weak link: set Connection type to Weak / hotspot."
        : c->conn_latency_ms > 150 ? "High latency: expect some input lag; Weak / hotspot may help."
        /* Sharp needs ~2 Mbps; the check's download speed has headroom here. */
        : c->conn_bars >= 3 && c->conn_kbps >= 3500 && c->conn_latency_ms <= 100
            ? "Strong connection: Bitrate Sharp should run cleanly for the most detail."
            : "Good connection: Standard with Adaptive should run smoothly.";
    const int best = regions_fastest();
    Region region;
    if (best >= 0 && regions_get((unsigned)best, &region))
        snprintf(text, sizeof(text), "%s Fastest server: %s, %d ms.", advice, region.name, region.ms);
    else
        snprintf(text, sizeof(text), "%s", advice);
    return text;
}

/* What an input sends, short enough for the list: "LB + RB". */
void binding_text(char *out, size_t size, const GfnButtonMap *map, int input)
{
    if (map->also[input] != GFN_OUT_NONE)
        snprintf(out, size, "%s + %s", gfn_output_short_name(map->out[input]),
                 gfn_output_short_name(map->also[input]));
    else
        snprintf(out, size, "%s", gfn_output_short_name(map->out[input]));
}

bool binding_changed(const App *app, int input)
{
    const GfnButtonMap *a = &app->mapping, *b = &app->mapping_default;
    return a->out[input] != b->out[input] || a->also[input] != b->also[input] || a->mode[input] != b->mode[input];
}

const char *update_phase_title(const UpdateInfo *info)
{
    switch (info->state) {
    case UPDATE_CHECKING: return "Checking GitHub for updates";
    case UPDATE_DOWNLOADING: return "Downloading";
    case UPDATE_VERIFYING: return "Checking the download";
    case UPDATE_INSTALLING: return "Installing - keep the console on";
    case UPDATE_INSTALLED: return "Update installed";
    case UPDATE_FAILED: return "Update didn't finish";
    case UPDATE_AVAILABLE: return "A new version is ready";
    case UPDATE_UP_TO_DATE: return "Kasumi is up to date";
    default: return "Software update";
    }
}

const GuidePage GUIDE[GUIDE_PAGES] = {
    { "霞", "ようこそ", "WELCOME TO KASUMI",
      "Play on the New 3DS from GeForce NOW, Xbox Cloud Gaming, or your own PC with Steam Link. The game "
      "runs elsewhere; the 3DS shows the picture and sends your buttons.",
      "Next you pick a service on the hub. B always brings you back to it." },
    { "鍵", "サインイン", "SIGN IN",
      "Cloud services show a short code to enter on your phone or computer; no password is typed on the "
      "3DS. Steam Link shows a code to type into Steam on your PC.",
      "Logins and paired PCs stay only on this console's SD card." },
    { "操", "操作", "CONTROLS",
      "The 3DS plays like a PlayStation or Xbox pad: bottom is Cross / A, right is Circle / B. L3, R3 "
      "and Home / PS are on the lower screen.",
      "Hold START + SELECT during play for the stream menu." },
    { "画", "画質", "PICTURE & WI-FI",
      "Stay close to your router (3 bars). For cloud games, Adaptive is the default; Steady 1 Mbps helps "
      "if the picture stutters. ZOOM crops for small text.",
      "Settings > Network > Connection check tests your Wi-Fi." },
    { "遊", "機能", "EXTRAS",
      "Gyro aiming, screenshots, zoom zones, themes, favourites and options for each game. Open a "
      "game from the library to see them.",
      "Settings > System > Getting started shows this guide again." },
};

/* Was this status line written from this message (in the chosen language)?
 * Compares the text before the first value. */
bool status_from(const char *status, const char *english_format)
{
    const char *format = i18n_text(english_format);
    const size_t n = strcspn(format, "%");
    return n > 0 && !strncmp(status, format, n);
}

void format_elapsed(char *out, size_t size, u64 ms)
{
    const unsigned total = (unsigned)(ms / 1000);
    if (total >= 3600)
        snprintf(out, size, "%u:%02u:%02u", total / 3600, total / 60 % 60, total % 60);
    else
        snprintf(out, size, "%u:%02u", total / 60, total % 60);
}

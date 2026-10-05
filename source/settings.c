#include "settings.h"

#include <jansson.h>

#include "audio_output.h"
#include "file_worker.h"
#include "game_prefs.h"
#include "regions.h"
#include "stream_profile.h"
#include "ui.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

void settings_defaults(AppSettings *settings)
{
    settings->button_layout = GFN_LAYOUT_POSITION;
    settings->xbox_names = false;
    settings->has_map = false;
    memset(&settings->map, 0, sizeof(settings->map));
    settings->deadzone = DEADZONE_MEDIUM;
    settings->swap_shoulders = false;
    settings->auto_pointer = true;
    settings->show_stats = true;
    settings->fast_input = false;
    settings->wide_video = true;
    settings->bitrate_mode = STREAM_BITRATE_ADAPTIVE;
    settings->sharpen = false;
    settings->fps60 = false;
    settings->video_sharpen = 1;
    settings->video_color = 1;
    settings->gyro_mode = GFN_GYRO_OFF;
    settings->gyro_speed = 1;
    settings->camera_speed = 1;
    settings->camera_invert = 0;
    settings->touch_camera = 1;
    settings->touch_camera_shown = false;
    settings->touch_stick_size = 1;
    settings->theme = UI_THEME_AI; /* indigo: the default look since beta.30 */
    settings->volume = 5;
    settings->mute_in_menus = false;
    settings->music_mode = 0; /* MENU_MUSIC_ON */
    settings->voice_cues = true;
    settings->sound_effects = true;
    settings->mic = false;
    settings->discord_seen = false;
    settings->lid_mode = LID_PAUSE;
    settings->guide_done = false;
    settings->auto_update = true;
    /* Kasumi's own releases are betas for now, so beta is the default
     * channel; otherwise a beta build would never see its successor. */
    settings->update_beta = true;
    settings->net_weak = false;
    settings->share_reports = SHARE_ASK;
    settings->share_stats = false;
    settings->share_consent = 0;
    settings->install_id[0] = '\0';
    settings->server[0] = '\0';
    settings->provider[0] = '\0';
}

unsigned settings_deadzone_percent(DeadzoneLevel level)
{
    static const unsigned percent[DEADZONE_COUNT] = { 7, 12, 20 };
    return level < DEADZONE_COUNT ? percent[level] : 12;
}

static bool read_bool(json_t *root, const char *key, bool fallback)
{
    json_t *value = json_object_get(root, key);
    return json_is_boolean(value) ? json_is_true(value) : fallback;
}

static int read_int(json_t *root, const char *key, int fallback, int count)
{
    json_t *value = json_object_get(root, key);
    if (!json_is_integer(value)) return fallback;
    const json_int_t number = json_integer_value(value);
    return number >= 0 && number < count ? (int)number : fallback;
}

bool settings_load(AppSettings *settings)
{
    settings_defaults(settings);
    json_error_t error;
    json_t *root = json_load_file(SETTINGS_PATH, 0, &error);
    if (!json_is_object(root)) {
        json_decref(root);
        return false;
    }
    settings->button_layout = (GfnButtonLayout)read_int(root, "button_layout",
                                                        settings->button_layout, 2);
    settings->deadzone = (DeadzoneLevel)read_int(root, "deadzone", settings->deadzone,
                                                 DEADZONE_COUNT);
    settings->swap_shoulders = read_bool(root, "swap_shoulders", settings->swap_shoulders);
    settings->xbox_names = read_bool(root, "xbox_names", settings->xbox_names);
    settings->has_map = game_prefs_read_map(root, &settings->map);
    settings->auto_pointer = read_bool(root, "auto_pointer", settings->auto_pointer);
    settings->show_stats = read_bool(root, "show_stats", settings->show_stats);
    settings->fast_input = read_bool(root, "fast_input", settings->fast_input);
    settings->wide_video = read_bool(root, "wide_video", settings->wide_video);
    /* The key changes whenever the list of rates does (build 66 dropped
     * 2 and 2.5 Mbps and added 1 Mbps), so an old index is never misread. */
    settings->bitrate_mode = (StreamBitrateMode)read_int(root, "bitrate66", settings->bitrate_mode,
                                                         STREAM_BITRATE_COUNT);
    settings->sharpen = read_bool(root, "sharpen", settings->sharpen);
    settings->fps60 = read_bool(root, "fps60", settings->fps60);
    settings->video_sharpen = (unsigned)read_int(root, "video_sharpen", (int)settings->video_sharpen, 4);
    settings->video_color = (unsigned)read_int(root, "video_color", (int)settings->video_color, 3);
    settings->gyro_mode = (GfnGyroMode)read_int(root, "gyro_mode", settings->gyro_mode,
                                                GFN_GYRO_MODE_COUNT);
    settings->gyro_speed = (unsigned)read_int(root, "gyro_speed", (int)settings->gyro_speed, 3);
    settings->camera_speed = (unsigned)read_int(root, "camera_speed", (int)settings->camera_speed, 4);
    settings->camera_invert = (unsigned)read_int(root, "camera_invert", (int)settings->camera_invert, 3);
    settings->touch_camera = (unsigned)read_int(root, "touch_camera", (int)settings->touch_camera, 3);
    settings->touch_camera_shown = read_bool(root, "touch_camera_shown", settings->touch_camera_shown);
    settings->touch_stick_size = (unsigned)read_int(root, "touch_stick_size", (int)settings->touch_stick_size, 3);
    settings->theme = (unsigned)read_int(root, "theme", (int)settings->theme, UI_THEME_COUNT);
    settings->volume = (unsigned)read_int(root, "volume", (int)settings->volume, 6);
    settings->mute_in_menus = read_bool(root, "mute_in_menus", settings->mute_in_menus);
    settings->music_mode = (unsigned)read_int(root, "music_mode", (int)settings->music_mode, 3);
    settings->voice_cues = read_bool(root, "voice_cues", settings->voice_cues);
    settings->sound_effects = read_bool(root, "sound_effects", settings->sound_effects);
    settings->mic = read_bool(root, "mic", settings->mic);
    settings->discord_seen = read_bool(root, "discord_seen", settings->discord_seen);
    /* Older settings files only had lid_keeps_playing (true: keep playing). */
    const int lid_fallback = read_bool(root, "lid_keeps_playing", false) ? LID_KEEP_PLAYING : LID_PAUSE;
    settings->lid_mode = (unsigned)read_int(root, "lid_mode", lid_fallback, LID_MODE_COUNT);
    settings->guide_done = read_bool(root, "guide_done", settings->guide_done);
    settings->auto_update = read_bool(root, "auto_update", settings->auto_update);
    settings->update_beta = read_bool(root, "update_beta", settings->update_beta);
    settings->net_weak = read_bool(root, "net_weak", settings->net_weak);
    settings->share_reports = (unsigned)read_int(root, "share_reports", (int)settings->share_reports, 3);
    settings->share_stats = read_bool(root, "share_stats", settings->share_stats);
    settings->share_consent = (unsigned)read_int(root, "share_consent", 0, 1000);
    json_t *install = json_object_get(root, "install_id");
    if (json_is_string(install)) snprintf(settings->install_id, sizeof(settings->install_id), "%s", json_string_value(install));
    json_t *server = json_object_get(root, "server");
    if (json_is_string(server)) snprintf(settings->server, sizeof(settings->server), "%s", json_string_value(server));
    json_t *provider = json_object_get(root, "provider");
    if (json_is_string(provider))
        snprintf(settings->provider, sizeof(settings->provider), "%s", json_string_value(provider));
    json_decref(root);
    return true;
}

static json_t *settings_json(const AppSettings *settings)
{
    json_t *root = json_pack("{s:i,s:i,s:b,s:b,s:b,s:b,s:b,s:i,s:b,s:i,s:i,s:i,s:i,s:b,s:i,s:b,s:b,s:b,s:b,s:s,s:i,s:s,s:b,s:i,s:s,s:i,s:b,s:b}",
                             "button_layout", (int)settings->button_layout,
                             "deadzone", (int)settings->deadzone,
                             "swap_shoulders", settings->swap_shoulders,
                             "auto_pointer", settings->auto_pointer,
                             "show_stats", settings->show_stats,
                             "fast_input", settings->fast_input,
                             "wide_video", settings->wide_video,
                             "bitrate66", (int)settings->bitrate_mode,
                             "sharpen", settings->sharpen,
                             "gyro_mode", (int)settings->gyro_mode,
                             "gyro_speed", (int)settings->gyro_speed,
                             "theme", (int)settings->theme,
                             "volume", (int)settings->volume,
                             "mute_in_menus", settings->mute_in_menus,
                             "lid_mode", (int)settings->lid_mode,
                             "guide_done", settings->guide_done,
                             "auto_update", settings->auto_update,
                             "update_beta", settings->update_beta,
                             "net_weak", settings->net_weak,
                             "server", settings->server,
                             "share_reports", (int)settings->share_reports,
                             "install_id", settings->install_id,
                             "share_stats", settings->share_stats,
                             "share_consent", (int)settings->share_consent,
                             "provider", settings->provider,
                             "music_mode", (int)settings->music_mode,
                             "voice_cues", settings->voice_cues,
                             "sound_effects", settings->sound_effects);
    if (!root) return NULL;
    json_object_set_new(root, "camera_speed", json_integer((json_int_t)settings->camera_speed));
    json_object_set_new(root, "camera_invert", json_integer((json_int_t)settings->camera_invert));
    json_object_set_new(root, "touch_camera", json_integer((json_int_t)settings->touch_camera));
    json_object_set_new(root, "fps60", json_boolean(settings->fps60));
    json_object_set_new(root, "touch_camera_shown", json_boolean(settings->touch_camera_shown));
    json_object_set_new(root, "touch_stick_size", json_integer((json_int_t)settings->touch_stick_size));
    json_object_set_new(root, "video_sharpen", json_integer((json_int_t)settings->video_sharpen));
    json_object_set_new(root, "video_color", json_integer((json_int_t)settings->video_color));
    json_object_set_new(root, "xbox_names", json_boolean(settings->xbox_names));
    json_object_set_new(root, "mic", json_boolean(settings->mic));
    json_object_set_new(root, "discord_seen", json_boolean(settings->discord_seen));
    if (settings->has_map) game_prefs_write_map(root, &settings->map);
    return root;
}

bool settings_save(const AppSettings *settings)
{
    mkdir("sdmc:/3ds", 0777);
    mkdir(APP_DATA_DIR, 0777);
    json_t *root = settings_json(settings);
    if (!root) return false;
    const bool ok = json_dump_file(root, SETTINGS_PATH, JSON_INDENT(2)) == 0;
    json_decref(root);
    return ok;
}

/* ---- Background saving ------------------------------------------------------ */

static AppSettings g_last_queued;
static bool g_has_queued;

void settings_save_async(const AppSettings *settings)
{
    /* Leaving Settings saved every time, changed or not: on a slow card
     * that was a visible pause before the menu went back (beta.34). */
    if (g_has_queued && memcmp(&g_last_queued, settings, sizeof(*settings)) == 0) return;
    g_last_queued = *settings;
    g_has_queued = true;
    file_worker_save_json(SETTINGS_PATH, settings_json(settings), JSON_INDENT(2));
}

/* The background writer logs a failed write; nothing to report here. */
bool settings_save_failed(void) { return false; }

void settings_flush(void) { file_worker_flush(); }

void settings_apply_picture(const AppSettings *settings)
{
    stream_profile_configure(settings->wide_video, settings->bitrate_mode);
    stream_profile_set_sharpen(settings->sharpen);
    /* 60 fps only in Wide mode (GPU pacing; the classic path writes the
     * framebuffer directly). A probe.txt run sets its own rate. Probe of
     * build 103: a steady 60, every frame decoded at 15.8 ms average. */
    if (!stream_profile_probing()) stream_profile_set_fps60(settings->fps60 && settings->wide_video);
    stream_profile_set_weak(settings->net_weak);
    regions_set_choice(settings->server);
    ui_set_theme((UiTheme)settings->theme);
    ui_set_video_look(settings->video_sharpen, settings->video_color);
    audio_output_set_volume((float)settings->volume / 5.0f);
}

void settings_apply_input(const AppSettings *settings)
{
    const GfnInputConfig config = {
        .layout = settings->button_layout,
        .deadzone_percent = settings_deadzone_percent(settings->deadzone),
        .swap_shoulders = settings->swap_shoulders,
        .gyro_mode = settings->gyro_mode,
        .gyro_speed = settings->gyro_speed,
        .camera_speed = settings->camera_speed,
        .camera_invert = settings->camera_invert,
    };
    gfn_input_configure(&config);
    gfn_input_set_xbox_names(settings->xbox_names);
    /* Every game's own map, if there is one (a game's own map or layout
     * replaces it at launch, main.c). */
    if (settings->has_map) gfn_input_set_custom_map(&settings->map);
}

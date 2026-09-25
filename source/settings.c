#include "settings.h"

#include <jansson.h>

#include "audio_output.h"
#include "ui.h"
#include <stdio.h>
#include <sys/stat.h>

void settings_defaults(AppSettings *settings)
{
    settings->button_layout = GFN_LAYOUT_POSITION;
    settings->deadzone = DEADZONE_MEDIUM;
    settings->swap_shoulders = false;
    settings->auto_pointer = true;
    settings->show_stats = true;
    settings->fast_input = false;
    settings->wide_video = true;
    settings->bitrate_mode = STREAM_BITRATE_ADAPTIVE;
    settings->sharpen = false;
    settings->gyro_mode = GFN_GYRO_OFF;
    settings->gyro_speed = 1;
    settings->theme = 0;
    settings->volume = 5;
    settings->mute_in_menus = false;
    settings->lid_keeps_playing = false;
    settings->guide_done = false;
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
    settings->auto_pointer = read_bool(root, "auto_pointer", settings->auto_pointer);
    settings->show_stats = read_bool(root, "show_stats", settings->show_stats);
    settings->fast_input = read_bool(root, "fast_input", settings->fast_input);
    settings->wide_video = read_bool(root, "wide_video", settings->wide_video);
    /* The key changes whenever the list of rates does (build 66 dropped
     * 2 and 2.5 Mbps and added 1 Mbps), so an old index is never misread. */
    settings->bitrate_mode = (StreamBitrateMode)read_int(root, "bitrate66", settings->bitrate_mode,
                                                         STREAM_BITRATE_COUNT);
    settings->sharpen = read_bool(root, "sharpen", settings->sharpen);
    settings->gyro_mode = (GfnGyroMode)read_int(root, "gyro_mode", settings->gyro_mode,
                                                GFN_GYRO_MODE_COUNT);
    settings->gyro_speed = (unsigned)read_int(root, "gyro_speed", (int)settings->gyro_speed, 3);
    settings->theme = (unsigned)read_int(root, "theme", (int)settings->theme, UI_THEME_COUNT);
    settings->volume = (unsigned)read_int(root, "volume", (int)settings->volume, 6);
    settings->mute_in_menus = read_bool(root, "mute_in_menus", settings->mute_in_menus);
    settings->lid_keeps_playing = read_bool(root, "lid_keeps_playing", settings->lid_keeps_playing);
    settings->guide_done = read_bool(root, "guide_done", settings->guide_done);
    json_decref(root);
    return true;
}

bool settings_save(const AppSettings *settings)
{
    mkdir("sdmc:/3ds", 0777);
    mkdir(APP_DATA_DIR, 0777);
    json_t *root = json_pack("{s:i,s:i,s:b,s:b,s:b,s:b,s:b,s:i,s:b,s:i,s:i,s:i,s:i,s:b,s:b,s:b}",
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
                             "lid_keeps_playing", settings->lid_keeps_playing,
                             "guide_done", settings->guide_done);
    if (!root) return false;
    const bool ok = json_dump_file(root, SETTINGS_PATH, JSON_INDENT(2)) == 0;
    json_decref(root);
    return ok;
}

void settings_apply_picture(const AppSettings *settings)
{
    stream_profile_configure(settings->wide_video, settings->bitrate_mode);
    stream_profile_set_sharpen(settings->sharpen);
    ui_set_theme((UiTheme)settings->theme);
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
    };
    gfn_input_configure(&config);
}

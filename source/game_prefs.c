#include "game_prefs.h"

#include <jansson.h>
#include <stdio.h>
#include <string.h>

#include "app_paths.h"
#include "file_worker.h"

#define PREFS_PATH APP_DATA_DIR "/games.json"

static json_t *g_prefs;
static unsigned g_version;

void game_prefs_load(void)
{
    json_error_t error;
    json_decref(g_prefs);
    g_prefs = json_load_file(PREFS_PATH, 0, &error);
    if (!json_is_object(g_prefs)) {
        json_decref(g_prefs);
        g_prefs = json_object();
    }
    ++g_version;
}

static int read_int(json_t *e, const char *key)
{
    json_t *v = json_object_get(e, key);
    return json_is_integer(v) ? (int)json_integer_value(v) : -1;
}

/* "map" is what each input sends (the only key before beta.35, so older
 * builds still read it); "map2" the combo's second output and "mapmode"
 * turbo / toggle, each written only when used. */
static bool read_map_array(json_t *e, const char *key, unsigned char *out)
{
    json_t *array = json_object_get(e, key);
    if (!json_is_array(array) || json_array_size(array) != GFN_INPUT_COUNT) return false;
    for (size_t i = 0; i < GFN_INPUT_COUNT; ++i)
        out[i] = (unsigned char)json_integer_value(json_array_get(array, i));
    return true;
}

bool game_prefs_read_map(json_t *e, GfnButtonMap *map)
{
    memset(map, 0, sizeof(*map));
    if (!read_map_array(e, "map", map->out)) return false;
    read_map_array(e, "map2", map->also);
    read_map_array(e, "mapmode", map->mode);
    gfn_button_map_clean(map);
    return true;
}

static void write_map_array(json_t *e, const char *key, const unsigned char *values, bool always)
{
    bool used = always;
    for (size_t i = 0; i < GFN_INPUT_COUNT; ++i) used |= values[i] != 0;
    if (!used) return;
    json_t *array = json_array();
    for (size_t i = 0; i < GFN_INPUT_COUNT; ++i) json_array_append_new(array, json_integer(values[i]));
    json_object_set_new(e, key, array);
}

void game_prefs_write_map(json_t *e, const GfnButtonMap *map)
{
    write_map_array(e, "map", map->out, true);
    write_map_array(e, "map2", map->also, false);
    write_map_array(e, "mapmode", map->mode, false);
}

GamePrefs game_prefs_none(void)
{
    return (GamePrefs){ .bitrate = -1, .gyro = -1, .layout = -1, .camera_speed = -1, .camera_invert = -1,
                        .gyro_speed = -1, .touch_camera = -1 };
}

bool game_prefs_custom(const GamePrefs *prefs)
{
    return prefs->bitrate >= 0 || prefs->gyro >= 0 || prefs->layout >= 0 || prefs->camera_speed >= 0 ||
           prefs->camera_invert >= 0 || prefs->gyro_speed >= 0 || prefs->touch_camera >= 0 || prefs->has_map;
}

GamePrefs game_prefs_get(const char *app_id)
{
    GamePrefs prefs = game_prefs_none();
    json_t *e = g_prefs && app_id ? json_object_get(g_prefs, app_id) : NULL;
    if (!json_is_object(e)) return prefs;
    prefs.favourite = json_is_true(json_object_get(e, "fav"));
    prefs.bitrate = read_int(e, "bitrate");
    prefs.gyro = read_int(e, "gyro");
    prefs.layout = read_int(e, "layout");
    prefs.camera_speed = read_int(e, "camera_speed");
    prefs.camera_invert = read_int(e, "camera_invert");
    prefs.gyro_speed = read_int(e, "gyro_speed");
    prefs.touch_camera = read_int(e, "touch_camera");
    prefs.has_map = game_prefs_read_map(e, &prefs.map);
    return prefs;
}

void game_prefs_set(const char *app_id, const GamePrefs *prefs)
{
    if (!g_prefs || !app_id || !app_id[0]) return;
    const bool empty = !prefs->favourite && !game_prefs_custom(prefs);
    if (empty) {
        json_object_del(g_prefs, app_id);
    } else {
        json_t *entry = json_pack("{s:b,s:i,s:i,s:i}", "fav", prefs->favourite, "bitrate", prefs->bitrate,
                                  "gyro", prefs->gyro, "layout", prefs->layout);
        /* Newer options only when set, so older builds read the file as before. */
        if (prefs->camera_speed >= 0) json_object_set_new(entry, "camera_speed", json_integer(prefs->camera_speed));
        if (prefs->camera_invert >= 0) json_object_set_new(entry, "camera_invert", json_integer(prefs->camera_invert));
        if (prefs->gyro_speed >= 0) json_object_set_new(entry, "gyro_speed", json_integer(prefs->gyro_speed));
        if (prefs->touch_camera >= 0) json_object_set_new(entry, "touch_camera", json_integer(prefs->touch_camera));
        if (prefs->has_map) game_prefs_write_map(entry, &prefs->map);
        json_object_set_new(g_prefs, app_id, entry);
    }
    file_worker_save_json(PREFS_PATH, json_deep_copy(g_prefs), JSON_COMPACT);
    ++g_version;
}

bool game_prefs_favourite(const char *app_id)
{
    return game_prefs_get(app_id).favourite;
}

unsigned game_prefs_version(void) { return g_version; }

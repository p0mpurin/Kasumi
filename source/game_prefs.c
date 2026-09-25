#include "game_prefs.h"

#include <jansson.h>
#include <stdio.h>

#include "app_paths.h"

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

GamePrefs game_prefs_get(const char *app_id)
{
    GamePrefs prefs = { false, -1, -1, -1, false, { 0 } };
    json_t *e = g_prefs && app_id ? json_object_get(g_prefs, app_id) : NULL;
    if (!json_is_object(e)) return prefs;
    prefs.favourite = json_is_true(json_object_get(e, "fav"));
    prefs.bitrate = read_int(e, "bitrate");
    prefs.gyro = read_int(e, "gyro");
    prefs.layout = read_int(e, "layout");
    json_t *map = json_object_get(e, "map");
    if (json_is_array(map) && json_array_size(map) == sizeof(prefs.map)) {
        prefs.has_map = true;
        for (size_t i = 0; i < sizeof(prefs.map); ++i)
            prefs.map[i] = (unsigned char)json_integer_value(json_array_get(map, i));
    }
    return prefs;
}

void game_prefs_set(const char *app_id, const GamePrefs *prefs)
{
    if (!g_prefs || !app_id || !app_id[0]) return;
    const bool empty = !prefs->favourite && prefs->bitrate < 0 && prefs->gyro < 0 && prefs->layout < 0 &&
                       !prefs->has_map;
    if (empty) {
        json_object_del(g_prefs, app_id);
    } else {
        json_t *entry = json_pack("{s:b,s:i,s:i,s:i}", "fav", prefs->favourite, "bitrate", prefs->bitrate,
                                  "gyro", prefs->gyro, "layout", prefs->layout);
        if (prefs->has_map) {
            json_t *map = json_array();
            for (size_t i = 0; i < sizeof(prefs->map); ++i) json_array_append_new(map, json_integer(prefs->map[i]));
            json_object_set_new(entry, "map", map);
        }
        json_object_set_new(g_prefs, app_id, entry);
    }
    json_dump_file(g_prefs, PREFS_PATH, JSON_COMPACT);
    ++g_version;
}

bool game_prefs_favourite(const char *app_id)
{
    return game_prefs_get(app_id).favourite;
}

unsigned game_prefs_version(void) { return g_version; }

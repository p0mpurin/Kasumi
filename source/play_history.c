#include "play_history.h"

#include <jansson.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "app_paths.h"

#define HISTORY_PATH APP_DATA_DIR "/history.json"

static json_t *g_history;

void play_history_load(void)
{
    json_error_t error;
    json_decref(g_history);
    g_history = json_load_file(HISTORY_PATH, 0, &error);
    if (!json_is_object(g_history)) {
        json_decref(g_history);
        g_history = json_object();
    }
}

static void save(void)
{
    if (g_history) json_dump_file(g_history, HISTORY_PATH, JSON_COMPACT);
}

static json_t *entry(const char *app_id, bool create)
{
    if (!g_history || !app_id || !app_id[0]) return NULL;
    json_t *e = json_object_get(g_history, app_id);
    if (!json_is_object(e) && create) {
        e = json_object();
        json_object_set_new(g_history, app_id, e);
    }
    return json_is_object(e) ? e : NULL;
}

static json_int_t number(json_t *e, const char *key)
{
    json_t *v = json_object_get(e, key);
    return json_is_integer(v) ? json_integer_value(v) : 0;
}

void play_history_begin(const char *app_id, const char *title)
{
    json_t *e = entry(app_id, true);
    if (!e) return;
    json_object_set_new(e, "title", json_string(title ? title : ""));
    json_object_set_new(e, "sessions", json_integer(number(e, "sessions") + 1));
    json_object_set_new(e, "last", json_integer((json_int_t)time(NULL)));
    save();
}

void play_history_end(const char *app_id, uint32_t seconds)
{
    json_t *e = entry(app_id, false);
    if (!e || !seconds) return;
    json_object_set_new(e, "seconds", json_integer(number(e, "seconds") + seconds));
    save();
}

bool play_history_get(const char *app_id, PlayHistory *out)
{
    json_t *e = entry(app_id, false);
    if (!e) return false;
    out->seconds = (uint32_t)number(e, "seconds");
    out->sessions = (uint32_t)number(e, "sessions");
    out->last_played = (int64_t)number(e, "last");
    return true;
}

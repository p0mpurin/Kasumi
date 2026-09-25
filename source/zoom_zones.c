#include "zoom_zones.h"

#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_paths.h"

#define ZONES_PATH APP_DATA_DIR "/zones.json"

static char g_app_id[48];
static ZoomZone g_zones[ZOOM_ZONES_MAX];
static unsigned g_count;

void zoom_zones_select(const char *app_id)
{
    snprintf(g_app_id, sizeof(g_app_id), "%s", app_id ? app_id : "");
    g_count = 0;
    json_error_t error;
    json_t *root = json_load_file(ZONES_PATH, 0, &error);
    json_t *list = json_is_object(root) ? json_object_get(root, g_app_id) : NULL;
    size_t index; json_t *item;
    json_array_foreach(list, index, item) {
        if (g_count >= ZOOM_ZONES_MAX || !json_is_array(item) || json_array_size(item) != 3) continue;
        ZoomZone *z = &g_zones[g_count];
        z->level = (unsigned)json_integer_value(json_array_get(item, 0));
        z->x = (unsigned)json_integer_value(json_array_get(item, 1));
        z->y = (unsigned)json_integer_value(json_array_get(item, 2));
        if (z->level >= 1 && z->level <= 3 && z->x <= 100 && z->y <= 100) ++g_count;
    }
    json_decref(root);
}

static void save(void)
{
    if (!g_app_id[0]) return;
    json_error_t error;
    json_t *root = json_load_file(ZONES_PATH, 0, &error);
    if (!json_is_object(root)) {
        json_decref(root);
        root = json_object();
    }
    json_t *list = json_array();
    for (unsigned i = 0; i < g_count; ++i)
        json_array_append_new(list, json_pack("[i,i,i]", (int)g_zones[i].level,
                                              (int)g_zones[i].x, (int)g_zones[i].y));
    if (g_count) json_object_set_new(root, g_app_id, list);
    else { json_decref(list); json_object_del(root, g_app_id); }
    json_dump_file(root, ZONES_PATH, JSON_COMPACT);
    json_decref(root);
}

unsigned zoom_zones_count(void) { return g_count; }

const ZoomZone *zoom_zones_get(unsigned index)
{
    return index < g_count ? &g_zones[index] : NULL;
}

unsigned zoom_zones_add(unsigned level, unsigned x, unsigned y)
{
    if (!level || g_count >= ZOOM_ZONES_MAX) return 0;
    for (unsigned i = 0; i < g_count; ++i)
        if (g_zones[i].level == level && abs((int)g_zones[i].x - (int)x) < 4 &&
            abs((int)g_zones[i].y - (int)y) < 4)
            return 0;
    g_zones[g_count] = (ZoomZone){ level, x, y };
    ++g_count;
    save();
    return g_count;
}

void zoom_zones_clear(void)
{
    g_count = 0;
    save();
}

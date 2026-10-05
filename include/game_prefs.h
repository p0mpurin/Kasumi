#pragma once

#include <jansson.h>
#include <stdbool.h>

#include "gfn_input.h"

/* Favourites and per-game options ("before you play"), keyed by the game's
 * library ID and kept in APP_DATA_DIR/games.json. */

typedef struct {
    bool favourite;
    /* -1 = use the global setting, otherwise the setting's value. */
    int bitrate;
    int gyro;
    int layout;
    /* GfnInputConfig: C-Stick speed and inversion, gyro speed. */
    int camera_speed;
    int camera_invert;
    int gyro_speed;
    /* Touch camera (AppSettings.touch_camera). */
    int touch_camera;
    /* Custom button mapping, when has_map. */
    bool has_map;
    GfnButtonMap map;
} GamePrefs;

void game_prefs_load(void);
GamePrefs game_prefs_get(const char *app_id);
/* No options: every value follows Settings. */
GamePrefs game_prefs_none(void);
/* Whether any option differs from Settings (favourite aside). */
bool game_prefs_custom(const GamePrefs *prefs);
void game_prefs_set(const char *app_id, const GamePrefs *prefs);
bool game_prefs_favourite(const char *app_id);
/* Bumped whenever favourites change, so the library list can refresh. */
unsigned game_prefs_version(void);
/* A button map inside a JSON object (also used by settings.json). */
bool game_prefs_read_map(json_t *object, GfnButtonMap *map);
void game_prefs_write_map(json_t *object, const GfnButtonMap *map);

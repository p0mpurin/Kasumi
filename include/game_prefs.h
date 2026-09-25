#pragma once

#include <stdbool.h>

/* Favourites and per-game options ("before you play"), keyed by the game's
 * library ID and kept in APP_DATA_DIR/games.json. */

typedef struct {
    bool favourite;
    /* -1 = use the global setting, otherwise the setting's value. */
    int bitrate;
    int gyro;
    int layout;
} GamePrefs;

void game_prefs_load(void);
GamePrefs game_prefs_get(const char *app_id);
void game_prefs_set(const char *app_id, const GamePrefs *prefs);
bool game_prefs_favourite(const char *app_id);
/* Bumped whenever favourites change, so the library list can refresh. */
unsigned game_prefs_version(void);

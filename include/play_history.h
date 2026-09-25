#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Per-game play time, session count and last played date, kept in
 * APP_DATA_DIR/history.json. Written when a stream starts and ends, never
 * during play. */

typedef struct {
    uint32_t seconds;
    uint32_t sessions;
    int64_t last_played;
} PlayHistory;

void play_history_load(void);
/* A stream of this game started / ended after `seconds` of play. */
void play_history_begin(const char *app_id, const char *title);
void play_history_end(const char *app_id, uint32_t seconds);
bool play_history_get(const char *app_id, PlayHistory *out);

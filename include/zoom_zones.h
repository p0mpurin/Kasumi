#pragma once

#include <stdbool.h>

/* Saved zoom views per game ("zones"): a zoom level and the centre of the
 * view in percent of the picture. ZOOM cycles through a game's zones. */

#define ZOOM_ZONES_MAX 4

typedef struct {
    unsigned level;
    unsigned x, y;
} ZoomZone;

/* Load the zones of this game (call when a game launches). */
void zoom_zones_select(const char *app_id);
unsigned zoom_zones_count(void);
const ZoomZone *zoom_zones_get(unsigned index);
/* Returns the new zone's number (1-based), or 0 if full or a duplicate. */
unsigned zoom_zones_add(unsigned level, unsigned x, unsigned y);
void zoom_zones_clear(void);

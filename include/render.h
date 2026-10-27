#pragma once

/* Putting each frame on the screen: the menus (screens.c) and, while a game
 * streams, decoded video paced to the display. Each console has its own
 * (source/3ds/render.c, source/vita/render.c). */

#include <stdbool.h>

#include "app.h"
#include "platform.h"

/* One pass of the main loop. draw_ui: the menus or the in-game overlay
 * changed and must be redrawn (while streaming, video alone is cheaper). */
void render(const App *app, bool draw_ui);
/* Frames put on the screen so far, for the FPS readout. The readout once
 * counted decoded frames, so a picture held back by the pacer or a stalled
 * loop still read "30" while it looked like 5 (player report, 2026-10-10). */
unsigned render_shown_frames(void);
/* Save the next video frame shown as a screenshot. */
void render_request_screenshot(void);
/* What the player should be told ("Saving screenshot..."), once; NULL when
 * there is nothing. */
const char *render_take_notice(void);
/* How long a main-loop pass took while streaming, logged with the pacing. */
void render_note_loop(unsigned loop_ms);
/* A touch on the touch screen in the lower view's coordinates (screens.c's
 * 320x240): the same point on the 3DS, wherever render() put that view on a
 * console with one screen. */
PlatTouch render_map_touch(PlatTouch touch);

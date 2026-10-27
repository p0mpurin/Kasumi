#pragma once

/* The Vita side of ui.h: one 960x544 screen. screens.c draws its views at
 * the 3DS's sizes (the upper 400x240, the lower 320x240); render.c places
 * each one on the screen as a region, scaled. */

#include "ui.h"

#define UI_SCREEN_WIDTH 960.0f
#define UI_SCREEN_HEIGHT 544.0f

void ui_frame_begin(void);
void ui_frame_end(void);
/* The frame's drawing is sent (ui_frame_end then only shows it). */
void ui_frame_finish(void);
/* What is drawn next lands at ox + x * scale, oy + y * scale and is clipped
 * to w x h (in the view's units). lower: the lower view, where the theme's
 * wallpaper and frosted glass belong. */
void ui_region(float ox, float oy, float scale, float w, float h, bool lower);
/* Back to plain screen pixels, unclipped. */
void ui_screen(void);

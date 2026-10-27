#pragma once

/* Between ui_common.c (themes, timing, text layout and every component,
 * built from ui.h's primitives) and each console's ui.c (the primitives:
 * rects, circles, images, text, frames). Not for screens.c. */

#include "ui.h"

/* Console side, called by ui_common.c. */
/* Loads the theme's lower-screen wallpaper and its blurred copy. */
void ui_load_wallpaper(UiTheme theme);
/* Drawing where frosted glass can show: the lower screen, over the
 * wallpaper (in a game the lower screen stays black). */
bool ui_glass_ready(void);

/* Shared side, called by each console's ui.c. */
/* Once at start-up, for ui_ticks(). */
void ui_common_init(void);
/* At the start of each frame, for ui_dt(). */
void ui_frame_tick(void);
/* The current theme (art that is celadon gets recoloured for the others). */
UiTheme ui_theme(void);
/* Kana or kanji in the text (UTF-8 lead bytes E3-E9). */
bool ui_text_is_japanese(const char *text);

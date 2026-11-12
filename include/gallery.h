#pragma once

#include <stdbool.h>

/* The screenshot viewer (Settings > System > Screenshots). Lists the PNGs in
 * APP_DATA_DIR/screenshots, newest first, and shows one at a time: a thread
 * decodes and shrinks it, and gallery_pump() turns it into a texture
 * between frames. */

/* How many screenshots there are (cached; rescanned after a new one). */
unsigned gallery_saved_count(void);
/* A screenshot was just saved: count again next time. */
void gallery_mark_dirty(void);

bool gallery_open(void);
void gallery_close(void);
/* Frees the closed gallery's picture (512 KB of video memory) for a stream. */
void gallery_release(void);
unsigned gallery_count(void);
unsigned gallery_index(void);
void gallery_step(int direction);
/* Deletes the shown screenshot; false if it couldn't be removed. */
bool gallery_delete_current(void);
/* "3 Oct 2026, 14:22", from the file name. */
const char *gallery_caption(void);

/* UI thread, outside a frame: upload a freshly decoded picture. */
void gallery_pump(void);
/* UI thread, in a frame: draw the picture fitted in w x h at (x, y). False
 * while it is loading. */
bool gallery_draw(float x, float y, float w, float h);

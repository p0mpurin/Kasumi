#pragma once

#include <stdbool.h>

/* Saves the stream picture as a 16:9 PNG in APP_DATA_DIR/screenshots.
 * The copy is taken on the UI thread; encoding and the SD write happen on
 * a low-priority thread so the stream never waits for them. */

/* Copy an 800x480 frame from a decoder surface (1024-pixel stride). */
bool screenshot_capture(const void *surface);
/* A finished save since the last call: 1 saved, -1 failed, 0 nothing. */
int screenshot_poll(char *name, unsigned size);

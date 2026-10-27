#pragma once

#include <stdbool.h>

#include "platform.h"

/* Saves the stream picture as a 16:9 PNG in APP_DATA_DIR/screenshots.
 * The copy is taken on the UI thread; encoding and the SD write happen on
 * a low-priority thread so the stream never waits for them. */

/* Copy an 800x480 frame from a decoder surface (1024-pixel stride). */
bool screenshot_capture(const void *surface);
/* Copy a picture of 0xAABBGGRR words (stride in pixels), saved at its size. */
bool screenshot_capture_rgba(const u32 *pixels, unsigned width, unsigned height, unsigned stride);
/* The same, saved as APP_DATA_DIR/ui-shots/<name> with no notice: pictures
 * of the menus for checking a layout (the Vita's ui-shots switch). */
bool screenshot_capture_ui(const u32 *pixels, unsigned width, unsigned height, unsigned stride, const char *name);
/* A finished save since the last call: 1 saved, -1 failed, 0 nothing. */
int screenshot_poll(char *name, unsigned size);

#pragma once

/* The 3DS side of ui.h: frames on its two screens, and the video picture on
 * the top one. Only source/3ds uses these (render.c composes the screens). */

#include "ui.h"

/* One frame: begin, draw into one or both targets, end. */
void ui_frame_begin(bool sync_vblank);
/* Begin a frame only if the GPU has finished the last one (false: it is
 * still busy, try again shortly). The stream uses it so the main loop
 * never sits waiting on the GPU while buttons go unread. */
bool ui_frame_try_begin(void);
void ui_frame_end(void);
void ui_begin_top(void);
void ui_begin_bottom(void);

/* Wide video: the top screen in its 800-column mode, fed by the GPU. Upload a
 * 1024x512 linear RGB565 decoder surface (tiled into a texture by one display
 * transfer), then draw its 800x480 region at half height: bilinear sampling
 * lands exactly between each pair of rows and averages them. */
bool ui_video_upload(const void *surface);
void ui_begin_top_video(void);
/* Classic (400-column) video: the decoder writes the visible top buffer,
 * so it must be single-buffered. Menus double-buffer it again. */
void ui_top_classic_video(void);
void ui_draw_video(void);
/* The picture's size inside the wide surface (mvd_video_wide_size). */
void ui_set_video_size(unsigned width, unsigned height);
/* Show only part of the picture (fractions, 0..1), filling the same area:
 * the Wide zoom. 0, 0, 1, 1 shows all of it. */
void ui_set_video_crop(float x, float y, float w, float h);

#pragma once

/* The Vita side of mvd_video.h: decoded frames are GPU textures that
 * render.c draws directly. */

#include <stdbool.h>

#include <vita2d.h>

/* Makes the frame textures (on the UI thread, at start-up). */
bool video_vita_prepare(void);
/* The newest decoded frame, or NULL before the first. *fresh is true when
 * it is new since the last call; *width x *height is the picture inside the
 * texture. The texture stays untouched until two newer frames have been
 * taken (the GPU may still be drawing it). */
vita2d_texture *video_vita_take_frame(bool *fresh, unsigned *width, unsigned *height);
/* The newest frame's pixels for a screenshot (0xAABBGGRR words, stride in
 * pixels); false when there is none. */
bool video_vita_frame_pixels(const unsigned **pixels, unsigned *width, unsigned *height, unsigned *stride);

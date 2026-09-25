#pragma once

#include <stdbool.h>
#include <stddef.h>

/* Wide output: 800x480 inside a 1024x512 RGB565 surface (GPU texture size). */
#define MVD_VIDEO_TEX_WIDTH 1024
#define MVD_VIDEO_TEX_HEIGHT 512
#define MVD_VIDEO_WIDE_WIDTH 800
#define MVD_VIDEO_WIDE_HEIGHT 480

bool mvd_video_init(unsigned input_width, unsigned input_height);
/* True while decoding for the 800-column wide screen via the GPU. */
bool mvd_video_wide(void);
/* The oldest ready wide frame's 1024x512 linear surface; NULL if none. The
 * decoder won't overwrite it until mvd_video_release_gpu_frame(). */
const void *mvd_video_take_gpu_frame(void);
void mvd_video_release_gpu_frame(void);
/* Decoded wide frames waiting to be shown, oldest first. */
unsigned mvd_video_ready_frames(void);
void mvd_video_skip_oldest_frame(void);
/* Encoded size of the index-th ready frame (0 = oldest): a small P-frame
 * means little changed on screen, so it is the least visible one to drop. */
size_t mvd_video_ready_frame_bytes(unsigned index);
/* Discard one decoded frame anywhere in the ready queue. */
void mvd_video_skip_ready_frame(unsigned index);
/* Skip decoding until the next IDR (a frame was lost upstream). */
void mvd_video_resync(void);
/* True once after the decode queue overflowed and a keyframe is needed. */
bool mvd_video_take_resync_request(void);
bool mvd_video_submit(const unsigned char *annex_b, size_t size);
bool mvd_video_toggle_zoom(void);
/* Jump to a zoom level (0 = off) centred at x, y percent of the picture. */
bool mvd_video_set_zoom(unsigned level, unsigned x_percent, unsigned y_percent);
bool mvd_video_pan_to_touch(unsigned touch_x, unsigned touch_y);
/* Center the magnified view on a point given in thousandths of the frame. */
bool mvd_video_pan_to(unsigned x_permille, unsigned y_permille);
bool mvd_video_zoomed(void);
unsigned mvd_video_zoom_level(void);
void mvd_video_zoom_position(unsigned *x, unsigned *y);
void mvd_video_close(void);
bool mvd_video_active(void);
unsigned mvd_video_decoded_frames(void);
unsigned mvd_video_errors(void);
const char *mvd_video_status(void);

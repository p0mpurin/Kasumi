#pragma once

#include <stdbool.h>
#include <stddef.h>

/* Wide output: 800x480 inside a 1024x512 RGB565 surface (GPU texture size). */
#define MVD_VIDEO_TEX_WIDTH 1024
#define MVD_VIDEO_TEX_HEIGHT 512
#define MVD_VIDEO_WIDE_WIDTH 800
#define MVD_VIDEO_WIDE_HEIGHT 480

bool mvd_video_init(unsigned input_width, unsigned input_height);
/* Decode 1280x720 like 960x544: whole access units, shrunk by MVD to the
 * top screen (Xbox). Off, 720p takes the older NAL-by-NAL path. Set before
 * mvd_video_init. */
void mvd_video_set_shrink_hd(bool on);
/* The decoder refused a frame and stopped taking any. */
bool mvd_video_failed(void);
/* True while decoding for the 800-column wide screen via the GPU. */
bool mvd_video_wide(void);
/* The picture inside the wide surface: 800x480, or smaller for a stream
 * narrower than 5:3 (drawn centred with bars). */
void mvd_video_wide_size(unsigned *width, unsigned *height);
/* The picture's size inside the coded one (the SPS crop), so the padding
 * rows below it are not drawn; 0, 0 shows the whole coded picture. */
void mvd_video_set_visible(unsigned width, unsigned height);
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
/* Latency guard: throw away the encoded frames waiting behind the one being
 * decoded and resume at the next keyframe (asked for at once). Returns how
 * many were dropped. */
unsigned mvd_video_drop_backlog(void);
bool mvd_video_submit(const unsigned char *annex_b, size_t size);
bool mvd_video_toggle_zoom(void);
/* Jump to a zoom level (0 = off) centred at x, y percent of the picture. */
bool mvd_video_set_zoom(unsigned level, unsigned x_percent, unsigned y_percent);
bool mvd_video_pan_to_touch(unsigned touch_x, unsigned touch_y);
/* Center the magnified view on a point given in thousandths of the frame. */
bool mvd_video_pan_to(unsigned x_permille, unsigned y_permille);
bool mvd_video_zoomed(void);
/* The part of the picture shown (fractions, 0..1): the Wide zoom, drawn by
 * the GPU. The whole picture when not zoomed. */
void mvd_video_view_crop(float *x, float *y, float *w, float *h);
unsigned mvd_video_zoom_level(void);
void mvd_video_zoom_position(unsigned *x, unsigned *y);
void mvd_video_close(void);
bool mvd_video_active(void);
unsigned mvd_video_decoded_frames(void);
/* Encoded frames waiting for the decoder (a rough read, no lock). */
unsigned mvd_video_pending_units(void);
/* Frames lost upstream (each held the picture until a keyframe), ever. */
unsigned mvd_video_frames_lost(void);
/* Decode time totals since start (microseconds); `reset_max` restarts the
 * running maximum. For the session performance summary. */
void mvd_video_decode_totals(unsigned long long *sum_us, unsigned *count, unsigned *max_us, bool reset_max);
unsigned mvd_video_errors(void);
/* The last brightness check found an almost black picture. */
bool mvd_video_picture_dark(void);
const char *mvd_video_status(void);

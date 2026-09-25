#pragma once

#include <stdbool.h>

/* What we ask NVIDIA to encode. Chosen in Settings > Picture and applied at
 * the next launch; every consumer (CloudMatch request, NVST SDP, decoder)
 * reads it from here so the three can never disagree. */

/* Build 55 measured the cost of bitrate on 3DS Wi-Fi: ~0.94 Mbps (adaptive)
 * needed 1 retransmission in 6.5 minutes, ~1.7 Mbps needed ~4400 in 2
 * minutes (about 1 packet in 10), and each one stalls a frame for a round
 * trip. Adaptive is therefore the default; steady floors are opt-in. */
typedef enum {
    STREAM_BITRATE_ADAPTIVE,
    STREAM_BITRATE_STEADY_1000,
    STREAM_BITRATE_STEADY_1200,
    STREAM_BITRATE_STEADY_1500,
    STREAM_BITRATE_COUNT
} StreamBitrateMode;

void stream_profile_configure(bool wide, StreamBitrateMode bitrate);
bool stream_profile_wide(void);
const char *stream_profile_name(void);
unsigned stream_profile_width(void);
unsigned stream_profile_height(void);
unsigned stream_profile_initial_bitrate(void);
unsigned stream_profile_min_bitrate(void);
unsigned stream_profile_max_bitrate(void);
/* NVIDIA vqos.dynamicStreamingMode: 3 adapts aggressively, 0 holds rate. */
unsigned stream_profile_dynamic_mode(void);
/* Developer resolution probe: request WxH and only log the SPS NVIDIA sends
 * (no decoding, so it also runs in an emulator without MVD). */
void stream_profile_set_override(unsigned width, unsigned height);
void stream_profile_set_probing(bool probing);
bool stream_profile_probing(void);
/* NVIDIA pre-encode filter. Sharpen is the old prefilter (mode 1, level 50);
 * off matches OpenNOW. (Build 62 also tried video.enableIntraRefresh; NVIDIA
 * ignored it and kept an IDR every 10.1 s, so that option was removed.) */
void stream_profile_set_sharpen(bool sharpen);
bool stream_profile_sharpen(void);

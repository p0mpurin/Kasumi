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
    /* Sharp: ~1.8-2 Mbps (floor 1.8) for strong Wi-Fi. The name is kept
     * from its beta.25 test; saved settings store its index. */
    STREAM_BITRATE_SHARP_TEST,
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
unsigned stream_profile_pacing_groups(void);
/* Sharp is running: its sessions don't mark a network as choppy. */
bool stream_profile_test_mode(void);
unsigned stream_profile_pacing_delay_us(void);
/* Developer resolution probe: request WxH and only log the SPS NVIDIA sends
 * (no decoding, so it also runs in an emulator without MVD). */
void stream_profile_set_override(unsigned width, unsigned height);
void stream_profile_set_probing(bool probing);
bool stream_profile_probing(void);
/* A probe run that decodes and shows the video too (probe.txt "decode" or
 * "@60"), so decode time is measured; plain probe runs only read the SPS. */
void stream_profile_set_probe_decode(bool on);
bool stream_profile_probe_decode(void);
/* NVIDIA pre-encode filter. Sharpen is the old prefilter (mode 1, level 50);
 * off matches OpenNOW. (Build 62 also tried video.enableIntraRefresh; NVIDIA
 * ignored it and kept an IDR every 10.1 s, so that option was removed.) */
void stream_profile_set_sharpen(bool sharpen);
/* Test: ask NVIDIA for 60 frames a second instead of 30 (next launch). */
void stream_profile_set_fps60(bool on);
unsigned stream_profile_fps(void);
/* 60 fps asked for in Settings. */
bool stream_profile_fps60_requested(void);
/* Weak Wi-Fi / phone hotspot: 0.6-1 Mbps whatever the bitrate setting (fewer
 * packets per frame, so fewer frames hit by a loss), a longer wait for
 * retransmissions and a bigger frame reserve. */
void stream_profile_set_weak(bool weak);
bool stream_profile_weak(void);
bool stream_profile_sharpen(void);

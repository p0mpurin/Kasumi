#pragma once

#include <3ds.h>
#include <stdbool.h>

/* Anonymous performance summary of one play session (Share performance
 * stats): numbers only, no log text, no addresses. Sampled once a second
 * while the stream is on screen; the counters are bumped where the events
 * happen. */
typedef struct {
    bool active;
    u64 started_at;
    char game[48];
    char region[40];
    bool weak;
    unsigned bitrate_mode;
    unsigned seconds;
    u64 ping_sum;
    unsigned ping_samples, ping_max;
    unsigned wifi_sum;
    u64 kbps_sum;
    unsigned kbps_min, kbps_max;
    unsigned fps_sum;
    /* Frame pacer. */
    unsigned repeated, skipped, drained;
    /* Main loop iterations over 25 ms while streaming, and the longest. */
    unsigned slow_loops, loop_max_ms;
    /* 60 fps latency guard: backlogs dropped to a keyframe. */
    unsigned catchups;
    /* Totals over the session (baselines absorb counter resets). */
    unsigned lost, keyframes, resent, concealed, reconnects;
    unsigned last_lost, last_keyframes, last_concealed;
    char last_error[48];
    bool user_left;
} PerfStats;

extern PerfStats g_perf;

void perf_begin(const char *game, const char *region, bool weak, unsigned bitrate_mode);
/* Once a second while streaming. Totals are the running counters of the
 * decoder / transport / audio; the per-second resend count is added. */
void perf_sample(int rtt_ms, unsigned wifi_bars, unsigned kbps, unsigned fps, unsigned resent_per_second,
                 unsigned lost_total, unsigned keyframes_total, unsigned concealed_total);
void perf_note_error(const char *what);
/* Finishes the session. Writes the summary to the SD card for sending (it
 * survives an exit) when it lasted long enough; false otherwise. */
bool perf_end(const char *install_id);

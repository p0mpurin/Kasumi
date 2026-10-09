#include "perf_stats.h"

#include <jansson.h>
#include <stdio.h>
#include <string.h>

#include "app_paths.h"
#include "diagnostic.h"
#include "file_worker.h"
#include "mvd_video.h"
#include "report.h"
#include "stream_profile.h"
#include <sys/stat.h>

PerfStats g_perf;

/* Sessions shorter than this say little about performance. */
#define PERF_MIN_SECONDS 30

static unsigned long long g_decode_sum_base;
static unsigned g_decode_count_base;

void perf_begin(const char *game, const char *region, bool weak, unsigned bitrate_mode)
{
    memset(&g_perf, 0, sizeof(g_perf));
    g_perf.active = true;
    g_perf.started_at = osGetTime();
    snprintf(g_perf.game, sizeof(g_perf.game), "%s", game ? game : "");
    snprintf(g_perf.region, sizeof(g_perf.region), "%s", region ? region : "");
    g_perf.weak = weak;
    g_perf.bitrate_mode = bitrate_mode;
    g_perf.kbps_min = ~0u;
    unsigned max_us = 0;
    mvd_video_decode_totals(&g_decode_sum_base, &g_decode_count_base, &max_us, true);
}

static unsigned delta(unsigned now, unsigned *last)
{
    /* A counter that went backwards was reset (a reconnect): count from 0. */
    const unsigned d = now >= *last ? now - *last : now;
    *last = now;
    return d;
}

void perf_sample(int rtt_ms, unsigned wifi_bars, unsigned kbps, unsigned fps, unsigned resent_per_second,
                 unsigned lost_total, unsigned keyframes_total, unsigned concealed_total)
{
    if (!g_perf.active) return;
    if (!g_perf.seconds) {
        /* The first sample only sets the baselines. */
        g_perf.last_lost = lost_total;
        g_perf.last_keyframes = keyframes_total;
        g_perf.last_concealed = concealed_total;
    }
    ++g_perf.seconds;
    if (rtt_ms > 0) {
        g_perf.ping_sum += (unsigned)rtt_ms;
        ++g_perf.ping_samples;
        if ((unsigned)rtt_ms > g_perf.ping_max) g_perf.ping_max = (unsigned)rtt_ms;
    }
    g_perf.wifi_sum += wifi_bars;
    g_perf.kbps_sum += kbps;
    if (kbps && kbps < g_perf.kbps_min) g_perf.kbps_min = kbps;
    if (kbps > g_perf.kbps_max) g_perf.kbps_max = kbps;
    g_perf.fps_sum += fps;
    g_perf.resent += resent_per_second;
    g_perf.lost += delta(lost_total, &g_perf.last_lost);
    g_perf.keyframes += delta(keyframes_total, &g_perf.last_keyframes);
    g_perf.concealed += delta(concealed_total, &g_perf.last_concealed);
}

void perf_note_error(const char *what)
{
    if (g_perf.active && what) snprintf(g_perf.last_error, sizeof(g_perf.last_error), "%s", what);
}

bool perf_end(const char *install_id)
{
    if (!g_perf.active) return false;
    g_perf.active = false;
    if (g_perf.seconds < PERF_MIN_SECONDS) return false;
    unsigned long long decode_sum = 0;
    unsigned decode_count = 0, decode_max = 0;
    mvd_video_decode_totals(&decode_sum, &decode_count, &decode_max, false);
    const unsigned frames = decode_count - g_decode_count_base;
    const unsigned decode_avg = frames ? (unsigned)((decode_sum - g_decode_sum_base) / frames) : 0;
    const unsigned n = g_perf.seconds;
    const char *end = g_perf.last_error[0] ? g_perf.last_error : g_perf.user_left ? "user" : "ended";
    /* Short keys: the service keeps each summary in 1 KB of metadata. */
    json_t *s = json_pack(
        "{s:s,s:s,s:s,s:s,s:s,s:s,s:i,s:i,s:i,s:i,s:i,s:i,s:i,s:i,s:i,s:i,s:i,s:i,s:i,s:i,s:i,s:i,s:i,s:i,s:i,s:i,s:s}",
        "app", APP_NAME, "v", APP_VERSION, "b", APP_BUILD, "i", install_id ? install_id : "",
        "g", g_perf.game, "r", g_perf.region,
        "m", g_perf.weak ? 1 : 0, "br", (int)g_perf.bitrate_mode, "d", (int)n,
        "p", g_perf.ping_samples ? (int)(g_perf.ping_sum / g_perf.ping_samples) : -1,
        "pm", (int)g_perf.ping_max, "w", (int)(g_perf.wifi_sum * 10 / n),
        "k", (int)(g_perf.kbps_sum / n), "kn", g_perf.kbps_min == ~0u ? 0 : (int)g_perf.kbps_min,
        "kx", (int)g_perf.kbps_max, "f", (int)(g_perf.fps_sum * 10 / n),
        "rp", (int)g_perf.repeated, "sk", (int)g_perf.skipped, "dr", (int)g_perf.drained,
        "lo", (int)g_perf.lost, "kf", (int)g_perf.keyframes, "rs", (int)g_perf.resent,
        "cc", (int)g_perf.concealed, "rc", (int)g_perf.reconnects,
        "da", (int)decode_avg, "dx", (int)decode_max, "e", end);
    if (!s) return false;
    json_object_set_new(s, "sl", json_integer(g_perf.slow_loops));
    json_object_set_new(s, "lm", json_integer(g_perf.loop_max_ms));
    /* 60 fps asked for, and whether it fell back to 30 (experimental). */
    json_object_set_new(s, "fr", json_integer(stream_profile_fps60_requested() ? 60 : 30));
    /* fb (fell back to 30) is gone with the fallback (build 114); cu counts
     * the latency guard's catch-ups instead. */
    json_object_set_new(s, "fb", json_integer(0));
    json_object_set_new(s, "cu", json_integer(g_perf.catchups));
    diagnostic_log("REPORT", "session summary %us ping=%d lost=%u repeated=%u saved=1", n,
                   g_perf.ping_samples ? (int)(g_perf.ping_sum / g_perf.ping_samples) : -1,
                   g_perf.lost, g_perf.repeated);
    /* In the background: this runs as the game closes (file_worker.h). */
    file_worker_save_json(REPORT_STATS_PENDING_PATH, s, JSON_COMPACT);
    report_stats_mark_pending(true);
    return true;
}

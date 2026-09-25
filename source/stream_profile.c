#include "stream_profile.h"

#include <stdio.h>

static bool g_wide = true;
static StreamBitrateMode g_bitrate = STREAM_BITRATE_ADAPTIVE;
static unsigned g_override_width, g_override_height;
static bool g_probing;
static bool g_sharpen;

void stream_profile_configure(bool wide, StreamBitrateMode bitrate)
{
    g_wide = wide;
    g_bitrate = bitrate < STREAM_BITRATE_COUNT ? bitrate : STREAM_BITRATE_ADAPTIVE;
}

bool stream_profile_wide(void) { return g_wide; }

const char *stream_profile_name(void)
{
    static char name[40];
    static const char *const rates[STREAM_BITRATE_COUNT] = {
        "adaptive", "1 Mbps", "1.2 Mbps", "1.5 Mbps"
    };
    snprintf(name, sizeof(name), "540p %s · %s", g_wide ? "wide" : "classic", rates[g_bitrate]);
    return name;
}

/* Both modes request the proven 960x544 stream (NVIDIA sends 960x540).
 * Build 51 asked for 800x480 and NVIDIA substituted 1280x800, which MVD
 * cannot decode, so "wide" is purely a display choice: see mvd_video.c. */
unsigned stream_profile_width(void) { return g_override_width ? g_override_width : 960; }
unsigned stream_profile_height(void) { return g_override_height ? g_override_height : 544; }

/* Developer resolution probe: see probe.txt handling in main.c. */
void stream_profile_set_override(unsigned width, unsigned height)
{
    g_override_width = width;
    g_override_height = height;
}

void stream_profile_set_probing(bool probing) { g_probing = probing; }

void stream_profile_set_sharpen(bool sharpen) { g_sharpen = sharpen; }
bool stream_profile_sharpen(void) { return g_sharpen; }
bool stream_profile_probing(void) { return g_probing; }

static unsigned steady_rate(void)
{
    switch (g_bitrate) {
    case STREAM_BITRATE_STEADY_1200: return 1200;
    case STREAM_BITRATE_STEADY_1500: return 1500;
    default: return 1000;
    }
}

/* Adaptive used to allow 1.6-4 Mbps. Build 58 showed NVIDIA climbing to
 * ~1.4 Mbps and the 3DS radio losing ~1.5 packets/s there; build 64 at a
 * steady ~1.3 Mbps hit a loss burst. Adaptive starts at 1.2 and may move
 * within 1-1.8 Mbps. Steady modes hold a floor, like the official mode 0. */
unsigned stream_profile_initial_bitrate(void)
{
    return g_bitrate == STREAM_BITRATE_ADAPTIVE ? 1200 : steady_rate();
}

unsigned stream_profile_min_bitrate(void)
{
    return g_bitrate == STREAM_BITRATE_ADAPTIVE ? 1000 : steady_rate();
}

unsigned stream_profile_max_bitrate(void)
{
    /* Steady peaks stay close to the floor: keyframe bursts above the
     * average are what the 3DS radio loses first. */
    return g_bitrate == STREAM_BITRATE_ADAPTIVE ? 1800 : steady_rate() + 250;
}

unsigned stream_profile_dynamic_mode(void)
{
    return g_bitrate == STREAM_BITRATE_ADAPTIVE ? 3 : 0;
}

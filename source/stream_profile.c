#include "stream_profile.h"

#include <stdio.h>

static bool g_wide = true;
static StreamBitrateMode g_bitrate = STREAM_BITRATE_ADAPTIVE;
static unsigned g_override_width, g_override_height;
static bool g_probing;
static bool g_sharpen;
static bool g_weak;

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
        "adaptive", "1 Mbps", "1.2 Mbps", "1.5 Mbps", "sharp"
    };
    snprintf(name, sizeof(name), "540p %s · %s", g_wide ? "wide" : "classic",
             g_weak ? "weak link" : rates[g_bitrate]);
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

static bool g_fps60;
void stream_profile_set_fps60(bool on) { g_fps60 = on; }
unsigned stream_profile_fps(void) { return g_fps60 ? 60 : 30; }
bool stream_profile_fps60_requested(void) { return g_fps60; }

/* Beta.33-34 held 60 fps to 1-1.5 Mbps on the idea that bits cost decode
 * time; beta.34's export says they don't (about 13 ms a frame at 0.8 and at
 * 1.8 Mbps alike, 14 at 60 fps). Only Sharp's 2-2.5 Mbps is trimmed, where
 * DOOM Eternal lagged (report EM7YGV). */
static bool sixty_sharp(void) { return g_fps60 && g_bitrate == STREAM_BITRATE_SHARP_TEST; }
void stream_profile_set_weak(bool weak) { g_weak = weak; }
bool stream_profile_weak(void) { return g_weak; }
bool stream_profile_sharpen(void) { return g_sharpen; }
bool stream_profile_probing(void) { return g_probing; }

static bool g_probe_decode;
void stream_profile_set_probe_decode(bool on) { g_probe_decode = on; }
bool stream_profile_probe_decode(void) { return g_probe_decode; }

static unsigned steady_rate(void)
{
    switch (g_bitrate) {
    case STREAM_BITRATE_STEADY_1200: return 1200;
    case STREAM_BITRATE_STEADY_1500: return 1500;
    default: return 1000;
    }
}

/* NVIDIA parks at the floor it is given (the 3DS sends no bandwidth
 * feedback), so the floor is the rate. Until beta.25 every rate above ~1
 * Mbps lost packets, which looked like the 3DS radio's limit: it was the
 * media socket's receive buffer. The 3DS refuses 128 KiB and libpeer then
 * kept the tiny default; stepping down to the 64 KiB it grants, a 1.8 Mbps
 * floor ran 4.6 minutes with one resent packet. Adaptive now sits at ~1.3,
 * Sharp at ~1.8-2. (Moonlight-N3DS receives 3 Mbps on the same hardware.)
 * Adaptive used to allow 1.6-4 Mbps. Build 58 showed NVIDIA climbing to
 * ~1.4 Mbps and the 3DS radio losing ~1.5 packets/s there; build 64 at a
 * steady ~1.3 Mbps hit a loss burst. Adaptive starts at 1.2 and may move
 * within 1-1.8 Mbps. Steady modes hold a floor, like the official mode 0. */
unsigned stream_profile_initial_bitrate(void)
{
    if (g_weak) return 800;
    if (sixty_sharp()) return 1600;
    if (g_bitrate == STREAM_BITRATE_SHARP_TEST) return 2000;
    return g_bitrate == STREAM_BITRATE_ADAPTIVE ? 1400 : steady_rate();
}

unsigned stream_profile_min_bitrate(void)
{
    if (g_weak) return 600;
    if (sixty_sharp()) return 1500;
    /* NVIDIA parks at the floor without bandwidth feedback (beta.25 tests:
     * a flat ~0.92 Mbps under Adaptive), so the test mode sets a high one. */
    if (g_bitrate == STREAM_BITRATE_SHARP_TEST) return 1800;
    return g_bitrate == STREAM_BITRATE_ADAPTIVE ? 1300 : steady_rate();
}

unsigned stream_profile_max_bitrate(void)
{
    /* Steady peaks stay close to the floor: keyframe bursts above the
     * average are what the 3DS radio loses first. */
    if (g_weak) return 1000;
    if (sixty_sharp()) return 1800;
    if (g_bitrate == STREAM_BITRATE_SHARP_TEST) return 2500;
    return g_bitrate == STREAM_BITRATE_ADAPTIVE ? 1800 : steady_rate() + 250;
}

/* How NVIDIA spreads a frame's packets. Sharp spreads a frame over ~9 ms
 * in more, smaller groups (beta.25: with the old buffer it cut frozen frames
 * from 23 to 6 at ~1.6 Mbps); the other modes keep the proven 3 ms. */
bool stream_profile_test_mode(void) { return g_bitrate == STREAM_BITRATE_SHARP_TEST; }
unsigned stream_profile_pacing_groups(void) { return g_bitrate == STREAM_BITRATE_SHARP_TEST ? 16 : 10; }
unsigned stream_profile_pacing_delay_us(void) { return g_bitrate == STREAM_BITRATE_SHARP_TEST ? 9000 : 3000; }

unsigned stream_profile_dynamic_mode(void)
{
    return g_bitrate == STREAM_BITRATE_ADAPTIVE || g_bitrate == STREAM_BITRATE_SHARP_TEST || g_weak ? 3 : 0;
}

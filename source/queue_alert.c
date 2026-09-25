#include "queue_alert.h"

#include <3ds.h>
#include <math.h>
#include <string.h>

#include "audio_output.h"
#include "diagnostic.h"
#include "ui.h"

/* The stream plays on NDSP channel 0; the chime has its own channel. */
#define CHIME_CHANNEL 8
#define CHIME_RATE 22050
#define CHIME_SAMPLES (CHIME_RATE * 9 / 10)

static bool g_active;
static bool g_screens_off;
static s16 *g_chime;
static ndspWaveBuf g_chime_buf;

/* Three bell-like notes (E6, G#6, B6) with a soft attack and long decay. */
static void build_chime(void)
{
    if (g_chime) return;
    g_chime = linearAlloc(CHIME_SAMPLES * sizeof(s16));
    if (!g_chime) return;
    static const float notes[3] = { 1318.5f, 1661.2f, 1975.5f };
    for (int i = 0; i < CHIME_SAMPLES; ++i) {
        float sample = 0.0f;
        for (int n = 0; n < 3; ++n) {
            const int start = n * CHIME_RATE / 7;
            if (i < start) continue;
            const float t = (float)(i - start) / CHIME_RATE;
            const float envelope = fminf(1.0f, t * 200.0f) * expf(-t * 5.0f);
            sample += sinf(2.0f * (float)M_PI * notes[n] * t) * envelope * 0.22f;
        }
        g_chime[i] = (s16)(sample * 32767.0f);
    }
    DSP_FlushDataCache(g_chime, CHIME_SAMPLES * sizeof(s16));
}

static void play_chime(void)
{
    build_chime();
    if (!g_chime) return;
    if (!audio_system_init()) return;
    ndspChnReset(CHIME_CHANNEL);
    ndspChnSetInterp(CHIME_CHANNEL, NDSP_INTERP_LINEAR);
    ndspChnSetRate(CHIME_CHANNEL, CHIME_RATE);
    ndspChnSetFormat(CHIME_CHANNEL, NDSP_FORMAT_MONO_PCM16);
    float mix[12] = { 0.9f, 0.9f };
    ndspChnSetMix(CHIME_CHANNEL, mix);
    memset(&g_chime_buf, 0, sizeof(g_chime_buf));
    g_chime_buf.data_pcm16 = g_chime;
    g_chime_buf.nsamples = CHIME_SAMPLES;
    ndspChnWaveBufAdd(CHIME_CHANNEL, &g_chime_buf);
}

/* A slow breathing pulse in the accent colour, repeating until stopped. */
static void set_led(bool on)
{
    if (R_FAILED(mcuHwcInit())) return;
    InfoLedPattern pattern;
    memset(&pattern, 0, sizeof(pattern));
    if (on) {
        const u32 accent = UI_ACCENT; /* 0xAABBGGRR */
        const u8 r = accent & 0xFF, g = (accent >> 8) & 0xFF, b = (accent >> 16) & 0xFF;
        pattern.delay = 2;
        pattern.smoothing = 0x40;
        pattern.loopDelay = 0;
        for (int i = 0; i < 32; ++i) {
            const float level = 0.5f - 0.5f * cosf((float)i / 32.0f * 2.0f * (float)M_PI);
            pattern.redPattern[i] = (u8)(r * level);
            pattern.greenPattern[i] = (u8)(g * level);
            pattern.bluePattern[i] = (u8)(b * level);
        }
    }
    MCUHWC_SetInfoLedPattern(&pattern);
    mcuHwcExit();
}

void queue_alert_start(void)
{
    if (g_active) return;
    g_active = true;
    diagnostic_log("APP", "queue alert: rig ready");
    set_led(true);
    play_chime();
}

void queue_alert_stop(void)
{
    if (!g_active) return;
    g_active = false;
    set_led(false);
}

bool queue_alert_active(void) { return g_active; }

bool queue_alert_lid_closed(void)
{
    u8 open = 1;
    if (R_FAILED(PTMU_GetShellState(&open))) return false;
    return open == 0;
}

void queue_alert_screens(bool lid_closed_waiting)
{
    if (lid_closed_waiting == g_screens_off) return;
    if (R_FAILED(gspLcdInit())) return;
    if (lid_closed_waiting) GSPLCD_PowerOffBacklight(GSPLCD_SCREEN_BOTH);
    else GSPLCD_PowerOnBacklight(GSPLCD_SCREEN_BOTH);
    gspLcdExit();
    g_screens_off = lid_closed_waiting;
}

void queue_alert_exit(void)
{
    queue_alert_stop();
    queue_alert_screens(false);
    if (audio_system_ready()) ndspChnReset(CHIME_CHANNEL);
    if (g_chime) linearFree(g_chime);
    g_chime = NULL;
}

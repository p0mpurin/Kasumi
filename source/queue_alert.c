#include "queue_alert.h"
#include "platform.h"

#include <math.h>
#include <string.h>

#include "audio_output.h"
#include "diagnostic.h"
#include "sfx.h"
#include "ui.h"

/* The stream plays on channel 0; the chime has its own channel. */
#define CHIME_CHANNEL 8
#define CHIME_RATE 22050
#define CHIME_SAMPLES (CHIME_RATE * 9 / 10)

static bool g_active;
static bool g_screens_off;
static s16 *g_chime;
static PlatAudioBuffer g_chime_buf;

/* Three bell-like notes (E6, G#6, B6) with a soft attack and long decay. */
static void build_chime(void)
{
    if (g_chime) return;
    g_chime = plat_audio_alloc(CHIME_SAMPLES * sizeof(s16));
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
}

static void play_chime(void)
{
    /* The rin-and-koto "your game is ready" (sfx.h), even with sound effects
     * off; the synthesised bells if it could not load. */
    if (sfx_play_always(SFX_QUEUE_READY)) return;
    build_chime();
    if (!g_chime) return;
    if (!audio_system_init()) return;
    plat_audio_channel_open(CHIME_CHANNEL, CHIME_RATE, 1, false);
    plat_audio_channel_volume(CHIME_CHANNEL, 0.9f, 0.9f);
    g_chime_buf.data = g_chime;
    g_chime_buf.frames = CHIME_SAMPLES;
    plat_audio_queue(CHIME_CHANNEL, &g_chime_buf);
}

/* A slow breathing pulse in the accent colour, repeating until stopped. */
static void set_led(bool on)
{
    plat_notify_light(on ? UI_ACCENT : 0);
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
    return plat_lid_closed();
}

void queue_alert_screens(bool lid_closed_waiting)
{
    if (lid_closed_waiting == g_screens_off) return;
    plat_screens_on(!lid_closed_waiting);
    g_screens_off = lid_closed_waiting;
}

void queue_alert_exit(void)
{
    queue_alert_stop();
    queue_alert_screens(false);
    if (audio_system_ready()) plat_audio_channel_close(CHIME_CHANNEL);
    if (g_chime) plat_audio_free(g_chime);
    g_chime = NULL;
}

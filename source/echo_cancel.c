#include "echo_cancel.h"

#include <3ds.h>
#include <string.h>

#include "diagnostic.h"

/* AECM's C API (vendor/webrtc-aecm/.../echo_control_mobile.h, extern "C"
 * inside a C++ namespace, so the names are plain). */
typedef struct {
    int16_t cngMode;
    int16_t echoMode;
} AecmConfig;
void *WebRtcAecm_Create(void);
void WebRtcAecm_Free(void *inst);
int32_t WebRtcAecm_Init(void *inst, int32_t rate);
int32_t WebRtcAecm_set_config(void *inst, AecmConfig config);
int32_t WebRtcAecm_BufferFarend(void *inst, const int16_t *farend, size_t samples);
int32_t WebRtcAecm_Process(void *inst, const int16_t *noisy, const int16_t *clean, int16_t *out,
                           size_t samples, int16_t delay_ms);

#define RATE 16000
/* The most aggressive of AECM's five modes: the speakers sit right next to
 * the mic (TriCord uses the same). */
#define ECHO_MODE 4
/* Delay hint. AECM finds the real delay itself and recovers from a low
 * guess, not a high one (TriCord caps its hint at 20 ms). */
#define DELAY_HINT_MS 20

/* The far end is fed from the media thread and the mic is processed on the
 * mic thread. */
static LightLock g_lock = 1;
static void *g_aecm;
static bool g_bypassed;
static int16_t g_far[ECHO_CANCEL_BLOCK];
static unsigned g_far_fill;
static unsigned g_failures;

bool echo_cancel_start(void)
{
    LightLock_Lock(&g_lock);
    if (!g_aecm) {
        g_aecm = WebRtcAecm_Create();
        const AecmConfig config = { 1, ECHO_MODE };
        if (!g_aecm || WebRtcAecm_Init(g_aecm, RATE) != 0 || WebRtcAecm_set_config(g_aecm, config) != 0) {
            if (g_aecm) WebRtcAecm_Free(g_aecm);
            g_aecm = NULL;
            LightLock_Unlock(&g_lock);
            diagnostic_log("MIC", "echo canceller failed to start");
            return false;
        }
        g_far_fill = 0;
        g_failures = 0;
        g_bypassed = false;
    }
    LightLock_Unlock(&g_lock);
    diagnostic_log("MIC", "echo canceller on (AECM mode %d)", ECHO_MODE);
    return true;
}

void echo_cancel_stop(void)
{
    LightLock_Lock(&g_lock);
    if (g_aecm) WebRtcAecm_Free(g_aecm);
    g_aecm = NULL;
    LightLock_Unlock(&g_lock);
}

/* Headphones: no speaker echo to remove, and the canceller would only
 * thin the voice out. Its learned echo path is stale when the speakers come
 * back, so it starts over then (as TriCord does). */
static bool bypass_for_headphones(void)
{
    const bool headphones = osIsHeadsetConnected();
    if (headphones != g_bypassed) {
        g_bypassed = headphones;
        if (!headphones) WebRtcAecm_Init(g_aecm, RATE);
        g_far_fill = 0;
    }
    return headphones;
}

void echo_cancel_far_end(const int16_t *pcm, size_t frames)
{
    LightLock_Lock(&g_lock);
    if (!g_aecm || bypass_for_headphones()) {
        LightLock_Unlock(&g_lock);
        return;
    }
    /* 48 kHz stereo -> 16 kHz mono: average each three stereo frames.
     * Averaged rather than picked, so aliasing doesn't blur the envelope
     * AECM's delay estimator follows (TriCord's note). */
    for (size_t i = 0; i + 3 <= frames; i += 3) {
        const int32_t sum = pcm[i * 2] + pcm[i * 2 + 1] + pcm[i * 2 + 2] + pcm[i * 2 + 3] +
                            pcm[i * 2 + 4] + pcm[i * 2 + 5];
        g_far[g_far_fill++] = (int16_t)(sum / 6);
        if (g_far_fill == ECHO_CANCEL_BLOCK) {
            if (WebRtcAecm_BufferFarend(g_aecm, g_far, ECHO_CANCEL_BLOCK) != 0 && ++g_failures <= 5)
                diagnostic_log("MIC", "echo canceller: far end refused");
            g_far_fill = 0;
        }
    }
    LightLock_Unlock(&g_lock);
}

void echo_cancel_process(int16_t *mic)
{
    LightLock_Lock(&g_lock);
    if (g_aecm && !bypass_for_headphones()) {
        int16_t out[ECHO_CANCEL_BLOCK];
        if (WebRtcAecm_Process(g_aecm, mic, NULL, out, ECHO_CANCEL_BLOCK, DELAY_HINT_MS) == 0)
            memcpy(mic, out, sizeof(out));
        else if (++g_failures <= 5)
            diagnostic_log("MIC", "echo canceller: process failed");
    }
    LightLock_Unlock(&g_lock);
}

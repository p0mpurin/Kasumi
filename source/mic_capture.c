#include "mic_capture.h"
#include "platform.h"

#include <opus/opus.h>
#include <string.h>

#include "diagnostic.h"
#include "echo_cancel.h"
#include "webrtc_transport.h"

/* The mic records at PLAT_MIC_RATE (16360 Hz on the 3DS); Opus wants 16000.
 * Voice needs no more. */
#define OPUS_RATE 16000
/* 10 ms frames: NVIDIA's mic is set up for them (mic.frameSize:10) and
 * stays silent on 20 ms ones (OpenNOW's native client; beta.35 sent 20 and
 * nothing reached the game). libpeer's RTP step matches (config.h). */
#define FRAME_SAMPLES (OPUS_RATE / 100)
/* Mono voice; the official client sends 16 kbps. */
#define VOICE_BITRATE 20000
#define SOFTWARE_GAIN 1

static OpusEncoder *g_encoder;
static PlatThread g_thread;
static volatile bool g_quit, g_muted = true;
static volatile float g_level;
static bool g_running;
static unsigned g_frames_sent, g_send_failures;

static void encode_and_send(int16_t *frame)
{
    if (g_muted) memset(frame, 0, FRAME_SAMPLES * sizeof(*frame));
    /* Take the game sound from the speakers back out (FRAME_SAMPLES is
     * ECHO_CANCEL_BLOCK: 10 ms at 16 kHz). */
    else echo_cancel_process(frame);
    unsigned char packet[256];
    const int bytes = opus_encode(g_encoder, frame, FRAME_SAMPLES, packet, sizeof(packet));
    if (bytes <= 0) return;
    if (webrtc_transport_send_mic(packet, (size_t)bytes)) ++g_frames_sent;
    else ++g_send_failures;
}

static void mic_main(void *arg)
{
    (void)arg;
    int16_t frame[FRAME_SAMPLES];
    unsigned fill = 0;
    /* Linear resampler state: where the next output sample falls between
     * the previous input sample and the current one. */
    const float step = PLAT_MIC_RATE / (float)OPUS_RATE;
    float position = 0.0f;
    int16_t previous = 0;
    float peak = 0.0f;
    s16 recorded[256];
    size_t count = 0, at = 0;
    while (!g_quit) {
        plat_sleep_ns(10000000LL);
        while (!g_quit) {
            if (at == count) {
                count = plat_mic_read(recorded, sizeof(recorded) / sizeof(recorded[0]));
                at = 0;
                if (!count) break;
            }
            int32_t sample = recorded[at++] * SOFTWARE_GAIN;
            if (sample > 32767) sample = 32767;
            if (sample < -32768) sample = -32768;
            while (position <= 1.0f) {
                const float value = (float)previous + (float)(sample - previous) * position;
                frame[fill++] = (int16_t)value;
                const float magnitude = (value < 0 ? -value : value) / 32768.0f;
                if (magnitude > peak) peak = magnitude;
                position += step;
                if (fill == FRAME_SAMPLES) {
                    g_level = g_muted ? 0.0f : peak;
                    peak = 0.0f;
                    encode_and_send(frame);
                    fill = 0;
                }
            }
            position -= 1.0f;
            previous = (int16_t)sample;
        }
    }
}

bool mic_capture_start(void)
{
    if (g_running) return true;
    if (!plat_mic_open()) return false;
    int error = OPUS_OK;
    g_encoder = opus_encoder_create(OPUS_RATE, 1, OPUS_APPLICATION_VOIP, &error);
    if (!g_encoder || error != OPUS_OK) {
        diagnostic_log("MIC", "opus encoder failed %d", error);
        plat_mic_close();
        return false;
    }
    opus_encoder_ctl(g_encoder, OPUS_SET_BITRATE(VOICE_BITRATE));
    /* Light enough to share core 2 with the media thread. */
    opus_encoder_ctl(g_encoder, OPUS_SET_COMPLEXITY(3));
    opus_encoder_ctl(g_encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
    g_quit = false;
    g_muted = true;
    g_level = 0.0f;
    g_frames_sent = g_send_failures = 0;
    const int priority = plat_thread_priority();
    /* Core 2, below the media thread (priority - 1) so video and input
     * always come first. */
    g_thread = plat_thread_create(mic_main, NULL, 32 * 1024, priority, PLAT_CORE_MEDIA, false);
    if (!g_thread) g_thread = plat_thread_create(mic_main, NULL, 32 * 1024, priority + 1, PLAT_CORE_DEFAULT, false);
    if (!g_thread) {
        opus_encoder_destroy(g_encoder);
        g_encoder = NULL;
        plat_mic_close();
        return false;
    }
    g_running = true;
    echo_cancel_start();
    diagnostic_log("MIC", "capture started (muted)");
    return true;
}

void mic_capture_stop(void)
{
    if (!g_running) return;
    g_quit = true;
    plat_thread_join(g_thread);
    echo_cancel_stop();
    g_thread = NULL;
    plat_mic_close();
    opus_encoder_destroy(g_encoder);
    g_encoder = NULL;
    g_running = false;
    g_level = 0.0f;
    diagnostic_log("MIC", "capture stopped: %u frames sent, %u not sent", g_frames_sent, g_send_failures);
}

bool mic_capture_running(void) { return g_running; }

void mic_capture_set_muted(bool muted)
{
    if (g_muted != muted) diagnostic_log("MIC", "%s", muted ? "muted" : "live");
    g_muted = muted;
    if (muted) g_level = 0.0f;
}

bool mic_capture_muted(void) { return g_muted; }
float mic_capture_level(void) { return g_level; }

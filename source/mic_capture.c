#include "mic_capture.h"

#include <3ds.h>
#include <malloc.h>
#include <opus/opus.h>
#include <string.h>

#include "diagnostic.h"
#include "echo_cancel.h"
#include "webrtc_transport.h"

/* The shared buffer the MIC service records into (a ring, about 3 s). */
#define MIC_BUFFER_SIZE 0x30000
/* The 3DS records at 16360 Hz; Opus wants 16000. Voice needs no more. */
#define MIC_RATE_IN 16360.0f
#define OPUS_RATE 16000
/* 10 ms frames: NVIDIA's mic is set up for them (mic.frameSize:10) and
 * stays silent on 20 ms ones (OpenNOW's native client; beta.35 sent 20 and
 * nothing reached the game). libpeer's RTP step matches (config.h). */
#define FRAME_SAMPLES (OPUS_RATE / 100)
/* Mono voice; the official client sends 16 kbps. */
#define VOICE_BITRATE 20000
/* The mic amplifier's gain (0-119), as TriCord (the 3DS Discord client,
 * whose voice chat works well) sets it. Beta.35 left the amplifier at its
 * default and lifted the samples x3 in software instead: the level bar
 * moved, but the voice may have been too quiet to hear. */
#define MIC_GAIN 60
#define SOFTWARE_GAIN 1

static u8 *g_buffer;
static u32 g_data_size;
static OpusEncoder *g_encoder;
static Thread g_thread;
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
    const float step = MIC_RATE_IN / (float)OPUS_RATE;
    float position = 0.0f;
    int16_t previous = 0;
    float peak = 0.0f;
    u32 read = micGetLastSampleOffset();
    while (!g_quit) {
        svcSleepThread(10000000LL);
        const u32 last = micGetLastSampleOffset();
        while (read != last && !g_quit) {
            int32_t sample = *(const int16_t *)(g_buffer + read) * SOFTWARE_GAIN;
            read += 2;
            if (read >= g_data_size) read = 0;
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
    g_buffer = memalign(0x1000, MIC_BUFFER_SIZE);
    if (!g_buffer) return false;
    Result rc = micInit(g_buffer, MIC_BUFFER_SIZE);
    if (R_FAILED(rc)) {
        diagnostic_log("MIC", "mic service unavailable rc=%08lX", (unsigned long)rc);
        free(g_buffer);
        g_buffer = NULL;
        return false;
    }
    g_data_size = micGetSampleDataSize();
    MICU_SetPower(true);
    MICU_SetGain(MIC_GAIN);
    MICU_SetClamp(false);
    rc = MICU_StartSampling(MICU_ENCODING_PCM16_SIGNED, MICU_SAMPLE_RATE_16360, 0, g_data_size, true);
    if (R_FAILED(rc)) {
        diagnostic_log("MIC", "sampling refused rc=%08lX", (unsigned long)rc);
        micExit();
        free(g_buffer);
        g_buffer = NULL;
        return false;
    }
    int error = OPUS_OK;
    g_encoder = opus_encoder_create(OPUS_RATE, 1, OPUS_APPLICATION_VOIP, &error);
    if (!g_encoder || error != OPUS_OK) {
        diagnostic_log("MIC", "opus encoder failed %d", error);
        MICU_StopSampling();
        micExit();
        free(g_buffer);
        g_buffer = NULL;
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
    s32 priority = 0x30;
    svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);
    /* Core 2, below the media thread (priority - 1) so video and input
     * always come first. */
    g_thread = threadCreate(mic_main, NULL, 32 * 1024, priority, 2, false);
    if (!g_thread) g_thread = threadCreate(mic_main, NULL, 32 * 1024, priority + 1, -2, false);
    if (!g_thread) {
        opus_encoder_destroy(g_encoder);
        g_encoder = NULL;
        MICU_StopSampling();
        micExit();
        free(g_buffer);
        g_buffer = NULL;
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
    threadJoin(g_thread, U64_MAX);
    threadFree(g_thread);
    echo_cancel_stop();
    g_thread = NULL;
    MICU_StopSampling();
    micExit();
    opus_encoder_destroy(g_encoder);
    g_encoder = NULL;
    free(g_buffer);
    g_buffer = NULL;
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

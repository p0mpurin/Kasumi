#include "audio_output.h"
#include "diagnostic.h"

#include <3ds.h>
#include <opus/opus.h>
#include <stdio.h>
#include <string.h>

#define AUDIO_CHANNEL 0
#define AUDIO_SAMPLE_RATE 48000
#define AUDIO_CHANNELS 2
/* GFN currently sends one Opus packet every 10 ms. Allow up to 20 ms. */
#define AUDIO_MAX_SAMPLES 960
/* Build 47 lost ~4.5% of packets because a 6-buffer (60 ms) ring overflowed
 * whenever the main loop stalled. The ring is now roomy, while latency is
 * bounded separately: past AUDIO_MAX_QUEUED the oldest audio is trimmed, and
 * after an underrun playback resumes only once a small cushion is queued. */
#define AUDIO_WAVEBUFS 24
#define AUDIO_MAX_QUEUED 12
#define AUDIO_PREBUFFER 3
/* Conceal at most this many lost packets (30 ms) with Opus PLC. */
#define AUDIO_MAX_CONCEAL 3
#define AUDIO_RED_PAYLOAD_TYPE 63

static OpusDecoder *g_decoder;
static float g_volume = 1.0f;
static bool g_muted;
static bool g_mix_ready;

static void apply_mix(void)
{
    if (!g_mix_ready) return;
    const float v = g_muted ? 0.0f : g_volume;
    float mix[12] = { v, v };
    ndspChnSetMix(AUDIO_CHANNEL, mix);
}

void audio_output_set_volume(float volume)
{
    if (volume < 0.0f) volume = 0.0f;
    if (volume > 1.0f) volume = 1.0f;
    if (volume == g_volume) return;
    g_volume = volume;
    apply_mix();
}

void audio_output_set_muted(bool muted)
{
    if (muted == g_muted) return;
    g_muted = muted;
    apply_mix();
}
static int16_t *g_pcm;
static ndspWaveBuf g_wavebufs[AUDIO_WAVEBUFS];
static unsigned g_next_wavebuf;
static bool g_prebuffering = true;
static bool g_have_sequence;
static uint16_t g_last_sequence;
static int g_last_frame_samples = 480;
static unsigned g_concealed;
static bool g_attempted;
static bool g_ndsp_ready;
static char g_status[96] = "not started";
static bool g_system_ready;
static bool g_system_attempted;

/* Beta.3 crashed inside ndspInit (crash dump 30: a write to 0x1ff57ffe in
 * ndspSetCounter). ndspInit reads variable addresses back from the DSP in a
 * handshake; it was called from the first audio packet, in the middle of
 * the DTLS handshake and decoder start-up, and got an incomplete reply. The
 * driver now starts once at launch, while the app is idle, and stays up. */
bool audio_system_init(void)
{
    if (g_system_ready) return true;
    if (g_system_attempted) return false;
    g_system_attempted = true;
    diagnostic_log("AUDIO", "dsp init begin");
    diagnostic_checkpoint();
    const Result result = ndspInit();
    g_system_ready = R_SUCCEEDED(result);
    diagnostic_log("AUDIO", "dsp init %s rc=%08lX", g_system_ready ? "ok" : "failed (no sound)",
                   (unsigned long)result);
    return g_system_ready;
}

bool audio_system_ready(void) { return g_system_ready; }

void audio_system_exit(void)
{
    if (!g_system_ready) return;
    ndspExit();
    g_system_ready = false;
}

bool audio_output_init(void)
{
    if (g_decoder && g_ndsp_ready) return true;
    if (g_attempted) return false;
    g_attempted = true;

    int opus_error = OPUS_OK;
    g_decoder = opus_decoder_create(AUDIO_SAMPLE_RATE, AUDIO_CHANNELS, &opus_error);
    if (!g_decoder || opus_error != OPUS_OK) {
        snprintf(g_status, sizeof(g_status), "Opus init failed %d", opus_error);
        diagnostic_log("AUDIO", "%s", g_status);
        return false;
    }

    if (!audio_system_init()) {
        snprintf(g_status, sizeof(g_status), "DSP unavailable: playing without sound");
        diagnostic_log("AUDIO", "%s", g_status);
        opus_decoder_destroy(g_decoder);
        g_decoder = NULL;
        return false;
    }
    g_ndsp_ready = true;

    const size_t samples_per_buffer = AUDIO_MAX_SAMPLES * AUDIO_CHANNELS;
    g_pcm = linearAlloc(samples_per_buffer * AUDIO_WAVEBUFS * sizeof(*g_pcm));
    if (!g_pcm) {
        snprintf(g_status, sizeof(g_status), "audio linearAlloc failed");
        diagnostic_log("AUDIO", "%s", g_status);
        audio_output_close();
        return false;
    }
    memset(g_pcm, 0, samples_per_buffer * AUDIO_WAVEBUFS * sizeof(*g_pcm));
    memset(g_wavebufs, 0, sizeof(g_wavebufs));

    ndspChnWaveBufClear(AUDIO_CHANNEL);
    ndspChnReset(AUDIO_CHANNEL);
    ndspSetOutputMode(NDSP_OUTPUT_STEREO);
    ndspChnSetInterp(AUDIO_CHANNEL, NDSP_INTERP_LINEAR);
    ndspChnSetRate(AUDIO_CHANNEL, AUDIO_SAMPLE_RATE);
    ndspChnSetFormat(AUDIO_CHANNEL, NDSP_FORMAT_STEREO_PCM16);
    g_mix_ready = true;
    apply_mix();
    for (unsigned i = 0; i < AUDIO_WAVEBUFS; ++i) {
        g_wavebufs[i].data_pcm16 = g_pcm + i * samples_per_buffer;
        g_wavebufs[i].status = NDSP_WBUF_DONE;
    }
    /* Paused until the prebuffer fills; queue_frame() starts playback. */
    ndspChnSetPaused(AUDIO_CHANNEL, true);
    g_prebuffering = true;
    snprintf(g_status, sizeof(g_status), "ready 48k stereo, %u buffers, cap %u",
             AUDIO_WAVEBUFS, AUDIO_MAX_QUEUED);
    diagnostic_log("AUDIO", "%s", g_status);
    return true;
}

static bool red_primary(const uint8_t **packet, size_t *packet_size)
{
    size_t header = 0;
    size_t redundant_bytes = 0;
    while (header < *packet_size && ((*packet)[header] & 0x80) != 0) {
        if (header + 4 > *packet_size) return false;
        redundant_bytes += ((size_t)((*packet)[header + 2] & 0x03) << 8) |
                           (size_t)(*packet)[header + 3];
        header += 4;
    }
    if (header >= *packet_size) return false;
    header++;
    if (header + redundant_bytes >= *packet_size) return false;
    *packet += header + redundant_bytes;
    *packet_size -= header + redundant_bytes;
    return true;
}

static unsigned queued_buffers(void)
{
    unsigned queued = 0;
    for (unsigned i = 0; i < AUDIO_WAVEBUFS; ++i)
        if (g_wavebufs[i].status != NDSP_WBUF_DONE && g_wavebufs[i].status != NDSP_WBUF_FREE)
            ++queued;
    return queued;
}

/* Decode one packet (or conceal one when packet is NULL) into the next free
 * buffer and queue it. Returns samples decoded, 0 if trimmed, <0 on error. */
static int queue_frame(const uint8_t *packet, size_t packet_size)
{
    const unsigned queued = queued_buffers();
    if (queued == 0 && !g_prebuffering) {
        /* Underrun: hold playback until a cushion builds so it doesn't crackle. */
        g_prebuffering = true;
        ndspChnSetPaused(AUDIO_CHANNEL, true);
    }
    if (queued >= AUDIO_MAX_QUEUED) return 0;
    ndspWaveBuf *wave = &g_wavebufs[g_next_wavebuf];
    if (wave->status != NDSP_WBUF_DONE && wave->status != NDSP_WBUF_FREE) return 0;

    const int decoded = packet
        ? opus_decode(g_decoder, packet, (opus_int32)packet_size, wave->data_pcm16,
                      AUDIO_MAX_SAMPLES, 0)
        : opus_decode(g_decoder, NULL, 0, wave->data_pcm16, g_last_frame_samples, 0);
    if (decoded <= 0) return decoded < 0 ? decoded : 0;
    DSP_FlushDataCache(wave->data_pcm16, (size_t)decoded * AUDIO_CHANNELS * sizeof(int16_t));
    wave->nsamples = (u32)decoded;
    ndspChnWaveBufAdd(AUDIO_CHANNEL, wave);
    g_next_wavebuf = (g_next_wavebuf + 1) % AUDIO_WAVEBUFS;
    if (g_prebuffering && queued + 1 >= AUDIO_PREBUFFER) {
        g_prebuffering = false;
        ndspChnSetPaused(AUDIO_CHANNEL, false);
    }
    return decoded;
}

int audio_output_submit(const uint8_t *packet, size_t packet_size, uint8_t payload_type,
                        uint16_t sequence)
{
    if (!packet || !packet_size || !audio_output_init()) return -1;
    if (payload_type == AUDIO_RED_PAYLOAD_TYPE && !red_primary(&packet, &packet_size)) {
        snprintf(g_status, sizeof(g_status), "malformed RED audio");
        return -1;
    }

    /* Fill short gaps with Opus packet-loss concealment instead of silence. */
    if (g_have_sequence) {
        const uint16_t gap = (uint16_t)(sequence - g_last_sequence);
        if (gap == 0 || gap > 0x8000) return 0; /* duplicate or late packet */
        for (uint16_t missing = 1; missing < gap && missing <= AUDIO_MAX_CONCEAL; ++missing)
            if (queue_frame(NULL, 0) > 0) ++g_concealed;
    }
    g_have_sequence = true;
    g_last_sequence = sequence;

    const int decoded = queue_frame(packet, packet_size);
    if (decoded < 0) {
        snprintf(g_status, sizeof(g_status), "Opus decode failed %d", decoded);
        diagnostic_log("AUDIO", "%s pt=%u bytes=%lu", g_status, payload_type,
                       (unsigned long)packet_size);
        return -1;
    }
    if (decoded > 0) g_last_frame_samples = decoded;
    return decoded;
}

unsigned audio_output_concealed(void) { return g_concealed; }

void audio_output_close(void)
{
    if (g_ndsp_ready) {
        /* The driver itself stays up for the next stream. */
        ndspChnWaveBufClear(AUDIO_CHANNEL);
        ndspChnReset(AUDIO_CHANNEL);
        g_ndsp_ready = false;
    }
    if (g_pcm) {
        linearFree(g_pcm);
        g_pcm = NULL;
    }
    if (g_decoder) {
        opus_decoder_destroy(g_decoder);
        g_decoder = NULL;
    }
    memset(g_wavebufs, 0, sizeof(g_wavebufs));
    g_next_wavebuf = 0;
    g_attempted = false;
    g_prebuffering = true;
    g_have_sequence = false;
    g_last_frame_samples = 480;
    g_concealed = 0;
    snprintf(g_status, sizeof(g_status), "closed");
}

const char *audio_output_status(void)
{
    return g_status;
}

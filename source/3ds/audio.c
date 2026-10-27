#include "platform.h"
#include "diagnostic.h"

#include <3ds.h>
#include <malloc.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

_Static_assert(sizeof(ndspWaveBuf) <= sizeof(PlatAudioVoice), "PlatAudioVoice must hold an ndspWaveBuf");

static bool g_firmware_missing;

/* ndsp writes its variables into DSP shared memory at 0x1ff50000 and
 * 0x1ff70000. A CIA only has those mapped if its exheader asks for them
 * (resources/app.rsf); beta.3 and beta.4 CIAs did not and crashed in ndspInit
 * (crash dumps 30-32: a write to 0x1ff57ffe). Check first, and play without
 * sound rather than crash if they are missing. */
static bool dsp_memory_mapped(void)
{
    static const u32 regions[2] = { 0x1FF50000, 0x1FF70000 };
    for (int i = 0; i < 2; ++i) {
        MemInfo info;
        PageInfo page;
        if (R_FAILED(svcQueryMemory(&info, &page, regions[i])) || info.state == MEMSTATE_FREE ||
            info.base_addr + info.size < regions[i] + 0x8000) {
            diagnostic_log("AUDIO", "dsp memory at %08lX not mapped (state=%lu)",
                           (unsigned long)regions[i], (unsigned long)info.state);
            return false;
        }
    }
    return true;
}

bool plat_audio_init(void)
{
    if (!dsp_memory_mapped()) {
        diagnostic_log("AUDIO", "dsp init skipped: playing without sound");
        return false;
    }
    diagnostic_log("AUDIO", "dsp init begin");
    diagnostic_checkpoint();
    const Result result = ndspInit();
    const bool ready = R_SUCCEEDED(result);
    /* D880A7FA: the DSP firmware dump (dspfirm.cdc) is not on the card. */
    g_firmware_missing = result == (Result)0xD880A7FA;
    diagnostic_log("AUDIO", "dsp init %s rc=%08lX", ready ? "ok" : "failed (no sound)", (unsigned long)result);
    if (ready) ndspSetOutputMode(NDSP_OUTPUT_STEREO);
    return ready;
}

bool plat_audio_firmware_missing(void) { return g_firmware_missing; }
void plat_audio_exit(void) { ndspExit(); }

void *plat_audio_alloc(size_t bytes) { return linearAlloc(bytes); }
void plat_audio_free(void *memory) { linearFree(memory); }

void plat_audio_channel_open(int channel, float rate, unsigned channels, bool smooth)
{
    ndspChnWaveBufClear(channel);
    ndspChnReset(channel);
    ndspChnSetInterp(channel, smooth ? NDSP_INTERP_POLYPHASE : NDSP_INTERP_LINEAR);
    ndspChnSetRate(channel, rate);
    ndspChnSetFormat(channel, channels == 1 ? NDSP_FORMAT_MONO_PCM16 : NDSP_FORMAT_STEREO_PCM16);
}

void plat_audio_channel_close(int channel)
{
    ndspChnWaveBufClear(channel);
    ndspChnReset(channel);
}

void plat_audio_channel_clear(int channel) { ndspChnWaveBufClear(channel); }

void plat_audio_channel_volume(int channel, float left, float right)
{
    float mix[12] = { left, right };
    ndspChnSetMix(channel, mix);
}

void plat_audio_channel_pause(int channel, bool paused) { ndspChnSetPaused(channel, paused); }

void plat_audio_queue(int channel, PlatAudioBuffer *buffer)
{
    /* The wave buffer lives in PlatAudioVoice's storage; it is built here
     * and copied in, and only the DSP code touches it as an ndspWaveBuf. */
    ndspWaveBuf wave;
    memset(&wave, 0, sizeof(wave));
    wave.data_pcm16 = buffer->data;
    wave.nsamples = buffer->frames;
    /* ndspChnGetFormat: two samples a frame when stereo. */
    const size_t samples = (ndspChnGetFormat(channel) & NDSP_CHANNELS(2)) ? buffer->frames * 2 : buffer->frames;
    DSP_FlushDataCache(buffer->data, samples * sizeof(s16));
    memcpy(&buffer->voice, &wave, sizeof(wave));
    ndspChnWaveBufAdd(channel, (ndspWaveBuf *)(void *)&buffer->voice);
}

bool plat_audio_buffer_idle(const PlatAudioBuffer *buffer)
{
    u8 status;
    memcpy(&status, (const u8 *)&buffer->voice + offsetof(ndspWaveBuf, status), sizeof(status));
    return status == NDSP_WBUF_FREE || status == NDSP_WBUF_DONE;
}

/* The shared buffer the MIC service records into (a ring, about 3 s). */
#define MIC_BUFFER_SIZE 0x30000
/* The mic amplifier's gain (0-119), as TriCord (the 3DS Discord client,
 * whose voice chat works well) sets it. Beta.35 left the amplifier at its
 * default and lifted the samples x3 in software instead: the level bar
 * moved, but the voice may have been too quiet to hear. */
#define MIC_GAIN 60

static u8 *g_mic_buffer;
static u32 g_mic_size, g_mic_read;

bool plat_mic_open(void)
{
    g_mic_buffer = memalign(0x1000, MIC_BUFFER_SIZE);
    if (!g_mic_buffer) return false;
    Result rc = micInit(g_mic_buffer, MIC_BUFFER_SIZE);
    if (R_FAILED(rc)) {
        diagnostic_log("MIC", "mic service unavailable rc=%08lX", (unsigned long)rc);
        free(g_mic_buffer);
        g_mic_buffer = NULL;
        return false;
    }
    g_mic_size = micGetSampleDataSize();
    MICU_SetPower(true);
    MICU_SetGain(MIC_GAIN);
    MICU_SetClamp(false);
    rc = MICU_StartSampling(MICU_ENCODING_PCM16_SIGNED, MICU_SAMPLE_RATE_16360, 0, g_mic_size, true);
    if (R_FAILED(rc)) {
        diagnostic_log("MIC", "sampling refused rc=%08lX", (unsigned long)rc);
        micExit();
        free(g_mic_buffer);
        g_mic_buffer = NULL;
        return false;
    }
    g_mic_read = micGetLastSampleOffset();
    return true;
}

void plat_mic_close(void)
{
    if (!g_mic_buffer) return;
    MICU_StopSampling();
    micExit();
    free(g_mic_buffer);
    g_mic_buffer = NULL;
}

size_t plat_mic_read(s16 *out, size_t max)
{
    if (!g_mic_buffer) return 0;
    const u32 last = micGetLastSampleOffset();
    size_t count = 0;
    while (g_mic_read != last && count < max) {
        out[count++] = *(const s16 *)(g_mic_buffer + g_mic_read);
        g_mic_read += 2;
        if (g_mic_read >= g_mic_size) g_mic_read = 0;
    }
    return count;
}

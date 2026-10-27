#include "platform.h"
#include "diagnostic.h"

#include <psp2/audioin.h>
#include <psp2/audioout.h>
#include <psp2/kernel/threadmgr.h>
#include <stdlib.h>
#include <string.h>

/* The Vita has one output for all of Kasumi's sound, so the channels are
 * mixed here (the 3DS's DSP does it in hardware): a thread fills 256-frame
 * blocks at 48 kHz, resampling each channel and adding them up. */
#define OUT_RATE 48000
#define GRAIN 256

/* As ndsp's: free, waiting, playing, finished. */
enum { BUF_FREE, BUF_QUEUED, BUF_PLAYING, BUF_DONE };

typedef struct {
    bool open, paused, smooth;
    unsigned channels;
    float step; /* source frames per output frame */
    float left, right;
    PlatAudioBuffer *head, *tail;
    float position;
} Channel;

static Channel g_channels[PLAT_AUDIO_CHANNELS];
static PlatLock g_lock = PLAT_LOCK_INIT;
static int g_port = -1;
static PlatThread g_thread;
static volatile bool g_running;

static PlatAudioBuffer *next_of(PlatAudioBuffer *buffer) { return (PlatAudioBuffer *)buffer->voice.next; }

static void drop_queue(Channel *c)
{
    for (PlatAudioBuffer *b = c->head; b;) {
        PlatAudioBuffer *next = next_of(b);
        b->voice.next = NULL;
        b->voice.status = BUF_DONE;
        b = next;
    }
    c->head = c->tail = NULL;
    c->position = 0.0f;
}

/* One sample of the buffer at frame index, both sides. */
static void sample_at(const Channel *c, const PlatAudioBuffer *b, u32 index, float *l, float *r)
{
    if (c->channels == 1) {
        *l = *r = b->data[index];
    } else {
        *l = b->data[index * 2];
        *r = b->data[index * 2 + 1];
    }
}

static void mix_channel(Channel *c, float *out)
{
    for (unsigned i = 0; i < GRAIN && c->head;) {
        PlatAudioBuffer *b = c->head;
        u32 index = (u32)c->position;
        if (index >= b->frames) {
            c->position -= (float)b->frames;
            c->head = next_of(b);
            if (!c->head) c->tail = NULL;
            b->voice.next = NULL;
            b->voice.status = BUF_DONE;
            continue;
        }
        b->voice.status = BUF_PLAYING;
        float l, r;
        sample_at(c, b, index, &l, &r);
        if (c->smooth || c->step != 1.0f) {
            /* Linear between this sample and the next one in the buffer. */
            const float t = c->position - (float)index;
            if (t > 0.0f && index + 1 < b->frames) {
                float l2, r2;
                sample_at(c, b, index + 1, &l2, &r2);
                l += (l2 - l) * t;
                r += (r2 - r) * t;
            }
        }
        out[i * 2] += l * c->left;
        out[i * 2 + 1] += r * c->right;
        c->position += c->step;
        ++i;
    }
}

static void mixer_main(void *arg)
{
    (void)arg;
    static float mix[GRAIN * 2];
    static s16 blocks[2][GRAIN * 2];
    unsigned which = 0;
    while (g_running) {
        memset(mix, 0, sizeof(mix));
        plat_lock(&g_lock);
        for (unsigned ch = 0; ch < PLAT_AUDIO_CHANNELS; ++ch) {
            Channel *c = &g_channels[ch];
            if (c->open && !c->paused && c->head) mix_channel(c, mix);
        }
        plat_unlock(&g_lock);
        s16 *block = blocks[which];
        which ^= 1;
        for (unsigned i = 0; i < GRAIN * 2; ++i) {
            const float v = mix[i];
            block[i] = (s16)(v > 32767.0f ? 32767 : v < -32768.0f ? -32768 : (int)v);
        }
        /* Waits until the hardware has room: this paces the thread. */
        sceAudioOutOutput(g_port, block);
    }
}

bool plat_audio_init(void)
{
    g_port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_MAIN, GRAIN, OUT_RATE, SCE_AUDIO_OUT_MODE_STEREO);
    if (g_port < 0) {
        diagnostic_log("AUDIO", "output port refused 0x%08X: playing without sound", (unsigned)g_port);
        return false;
    }
    int volume[2] = { SCE_AUDIO_VOLUME_0DB, SCE_AUDIO_VOLUME_0DB };
    sceAudioOutSetVolume(g_port, SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH, volume);
    g_running = true;
    /* Above the UI: a late block is a click everyone hears. */
    g_thread = plat_thread_create(mixer_main, NULL, 64 * 1024, plat_thread_priority() - 2, PLAT_CORE_DEFAULT, false);
    if (!g_thread) {
        g_running = false;
        sceAudioOutReleasePort(g_port);
        g_port = -1;
        return false;
    }
    diagnostic_log("AUDIO", "mixer ready 48k stereo, %u-frame blocks", GRAIN);
    return true;
}

bool plat_audio_firmware_missing(void) { return false; }

void plat_audio_exit(void)
{
    if (!g_thread) return;
    g_running = false;
    plat_thread_join(g_thread);
    g_thread = NULL;
    sceAudioOutReleasePort(g_port);
    g_port = -1;
    plat_lock(&g_lock);
    for (unsigned ch = 0; ch < PLAT_AUDIO_CHANNELS; ++ch) drop_queue(&g_channels[ch]);
    plat_unlock(&g_lock);
}

void *plat_audio_alloc(size_t bytes) { return malloc(bytes); }
void plat_audio_free(void *memory) { free(memory); }

static Channel *channel(int index)
{
    return index >= 0 && index < PLAT_AUDIO_CHANNELS ? &g_channels[index] : NULL;
}

void plat_audio_channel_open(int index, float rate, unsigned channels, bool smooth)
{
    Channel *c = channel(index);
    if (!c) return;
    plat_lock(&g_lock);
    drop_queue(c);
    c->open = true;
    c->paused = false;
    c->smooth = smooth;
    c->channels = channels == 1 ? 1 : 2;
    c->step = rate > 0.0f ? rate / (float)OUT_RATE : 1.0f;
    c->left = c->right = 1.0f;
    plat_unlock(&g_lock);
}

void plat_audio_channel_close(int index)
{
    Channel *c = channel(index);
    if (!c) return;
    plat_lock(&g_lock);
    drop_queue(c);
    c->open = false;
    plat_unlock(&g_lock);
}

void plat_audio_channel_clear(int index)
{
    Channel *c = channel(index);
    if (!c) return;
    plat_lock(&g_lock);
    drop_queue(c);
    plat_unlock(&g_lock);
}

void plat_audio_channel_volume(int index, float left, float right)
{
    Channel *c = channel(index);
    if (!c) return;
    plat_lock(&g_lock);
    c->left = left;
    c->right = right;
    plat_unlock(&g_lock);
}

void plat_audio_channel_pause(int index, bool paused)
{
    Channel *c = channel(index);
    if (!c) return;
    plat_lock(&g_lock);
    c->paused = paused;
    plat_unlock(&g_lock);
}

void plat_audio_queue(int index, PlatAudioBuffer *buffer)
{
    Channel *c = channel(index);
    if (!c || !buffer->frames) return;
    plat_lock(&g_lock);
    buffer->voice.next = NULL;
    buffer->voice.status = BUF_QUEUED;
    if (c->tail) c->tail->voice.next = buffer;
    else c->head = buffer;
    c->tail = buffer;
    plat_unlock(&g_lock);
}

bool plat_audio_buffer_idle(const PlatAudioBuffer *buffer)
{
    const u32 status = buffer->voice.status;
    return status == BUF_FREE || status == BUF_DONE;
}

/* ---- Microphone ---------------------------------------------------------- */

/* The mic port hands over 256 samples (16 ms) at a time; a thread keeps a
 * second of them for plat_mic_read. */
#define MIC_GRAIN 256
#define MIC_RING 16384

static int g_mic_port = -1;
static PlatThread g_mic_thread;
static volatile bool g_mic_running;
static s16 g_mic_ring[MIC_RING];
static volatile u32 g_mic_write, g_mic_read;

static void mic_main(void *arg)
{
    (void)arg;
    s16 grain[MIC_GRAIN];
    while (g_mic_running) {
        if (sceAudioInInput(g_mic_port, grain) < 0) {
            plat_sleep_ns(10000000LL);
            continue;
        }
        u32 write = g_mic_write;
        for (unsigned i = 0; i < MIC_GRAIN; ++i) {
            g_mic_ring[write % MIC_RING] = grain[i];
            ++write;
        }
        g_mic_write = write;
    }
}

bool plat_mic_open(void)
{
    g_mic_port = sceAudioInOpenPort(SCE_AUDIO_IN_PORT_TYPE_VOICE, MIC_GRAIN, 16000, SCE_AUDIO_IN_PARAM_FORMAT_S16_MONO);
    if (g_mic_port < 0) {
        diagnostic_log("MIC", "mic port refused 0x%08X", (unsigned)g_mic_port);
        return false;
    }
    g_mic_write = g_mic_read = 0;
    g_mic_running = true;
    g_mic_thread = plat_thread_create(mic_main, NULL, 32 * 1024, plat_thread_priority() - 1, PLAT_CORE_DEFAULT, false);
    if (!g_mic_thread) {
        g_mic_running = false;
        sceAudioInReleasePort(g_mic_port);
        g_mic_port = -1;
        return false;
    }
    return true;
}

void plat_mic_close(void)
{
    if (g_mic_port < 0) return;
    g_mic_running = false;
    plat_thread_join(g_mic_thread);
    g_mic_thread = NULL;
    sceAudioInReleasePort(g_mic_port);
    g_mic_port = -1;
}

size_t plat_mic_read(s16 *out, size_t max)
{
    const u32 write = g_mic_write;
    u32 read = g_mic_read;
    /* More than the ring holds went by: skip to what is still there. */
    if (write - read > MIC_RING) read = write - MIC_RING;
    size_t count = 0;
    while (read != write && count < max) out[count++] = g_mic_ring[read++ % MIC_RING];
    g_mic_read = read;
    return count;
}

#include "sfx.h"
#include "platform.h"

#include <opus/opus.h>
#include <stdlib.h>
#include <string.h>

#include "audio_output.h"
#include "diagnostic.h"

/* The stream plays on 0, the old chime on 8, music on 9, voice on 10. */
#define FIRST_CHANNEL 11
#define CHANNELS 3
/* Half of Opus's 48 kHz: the effects were made at 32 kHz, and this halves
 * the sound memory (about 0.8 MB for all of them). */
#define RATE 24000
#define MAX_SECONDS 3
#define VOLUME 0.8f

#define SFX_SYMBOLS(name) \
    extern const unsigned char _binary_sfx_##name##_opus_start[]; \
    extern const unsigned char _binary_sfx_##name##_opus_end[];
SFX_SYMBOLS(move)
SFX_SYMBOLS(bump)
SFX_SYMBOLS(select)
SFX_SYMBOLS(back)
SFX_SYMBOLS(open)
SFX_SYMBOLS(close)
SFX_SYMBOLS(toggle_on)
SFX_SYMBOLS(toggle_off)
SFX_SYMBOLS(tab)
SFX_SYMBOLS(notice)
SFX_SYMBOLS(error)
SFX_SYMBOLS(screenshot)
SFX_SYMBOLS(queue_ready)
#define SFX_ENTRY(name) { _binary_sfx_##name##_opus_start, _binary_sfx_##name##_opus_end, #name }

static const struct {
    const unsigned char *start, *end;
    const char *name;
} EMBEDDED[SFX_COUNT] = {
    SFX_ENTRY(move), SFX_ENTRY(bump), SFX_ENTRY(select), SFX_ENTRY(back), SFX_ENTRY(open),
    SFX_ENTRY(close), SFX_ENTRY(toggle_on), SFX_ENTRY(toggle_off), SFX_ENTRY(tab),
    SFX_ENTRY(notice), SFX_ENTRY(error), SFX_ENTRY(screenshot), SFX_ENTRY(queue_ready),
};

static s16 *g_pcm[SFX_COUNT];
static unsigned g_frames[SFX_COUNT];
static PlatAudioBuffer g_buf[CHANNELS];
static unsigned g_next;
static bool g_ready, g_enabled = true;

/* Every Opus packet of an in-memory Ogg stream, in order. */
typedef struct {
    const unsigned char *data;
    size_t size, page, pos;
    const unsigned char *segs;
    unsigned nsegs, seg;
    unsigned char packet[4096];
} Ogg;

static bool ogg_next(Ogg *o, size_t *length)
{
    size_t used = 0;
    for (;;) {
        if (o->seg >= o->nsegs) {
            const size_t p = o->page;
            if (p + 27 > o->size || memcmp(o->data + p, "OggS", 4)) return false;
            const unsigned n = o->data[p + 26];
            if (p + 27 + n > o->size) return false;
            size_t body = 0;
            for (unsigned i = 0; i < n; ++i) body += o->data[p + 27 + i];
            if (p + 27 + n + body > o->size) return false;
            o->segs = o->data + p + 27;
            o->nsegs = n;
            o->seg = 0;
            o->pos = p + 27 + n;
            o->page = o->pos + body;
            continue;
        }
        const unsigned seg = o->segs[o->seg++];
        if (used + seg > sizeof(o->packet)) return false;
        memcpy(o->packet + used, o->data + o->pos, seg);
        used += seg;
        o->pos += seg;
        if (seg < 255) {
            *length = used;
            return true;
        }
    }
}

static bool decode(Sfx sfx, OpusDecoder *decoder, s16 *scratch, unsigned capacity)
{
    Ogg o = { .data = EMBEDDED[sfx].start, .size = (size_t)(EMBEDDED[sfx].end - EMBEDDED[sfx].start) };
    opus_decoder_ctl(decoder, OPUS_RESET_STATE);
    unsigned frames = 0, skip = 0;
    size_t length;
    while (ogg_next(&o, &length)) {
        if (length >= 8 && !memcmp(o.packet, "OpusHead", 8)) {
            /* Pre-skip is counted at 48 kHz. */
            if (length >= 12) skip = (unsigned)(o.packet[10] | (o.packet[11] << 8)) * RATE / 48000;
            continue;
        }
        if (!length || (length >= 8 && !memcmp(o.packet, "OpusTags", 8))) continue;
        if (frames >= capacity) break;
        const int got = opus_decode(decoder, o.packet, (opus_int32)length, scratch + frames * 2,
                                    (int)(capacity - frames), 0);
        if (got > 0) frames += (unsigned)got;
    }
    if (frames <= skip) return false;
    frames -= skip;
    g_pcm[sfx] = plat_audio_alloc(frames * 2 * sizeof(s16));
    if (!g_pcm[sfx]) return false;
    memcpy(g_pcm[sfx], scratch + skip * 2, frames * 2 * sizeof(s16));
    g_frames[sfx] = frames;
    return true;
}

void sfx_init(void)
{
    if (g_ready || !audio_system_ready()) return;
    const u64 started = plat_time_ms();
    int error = OPUS_OK;
    OpusDecoder *decoder = opus_decoder_create(RATE, 2, &error);
    const unsigned capacity = RATE * MAX_SECONDS;
    s16 *scratch = malloc(capacity * 2 * sizeof(s16));
    unsigned loaded = 0;
    if (decoder && error == OPUS_OK && scratch)
        for (unsigned i = 0; i < SFX_COUNT; ++i) loaded += decode((Sfx)i, decoder, scratch, capacity);
    free(scratch);
    if (decoder) opus_decoder_destroy(decoder);
    for (unsigned c = 0; c < CHANNELS; ++c) plat_audio_channel_open(FIRST_CHANNEL + c, RATE, 2, false);
    g_ready = loaded > 0;
    diagnostic_log("SFX", "%u of %u sounds ready in %llu ms (linear free %lu KiB)", loaded, (unsigned)SFX_COUNT,
                   (unsigned long long)(plat_time_ms() - started), (unsigned long)(plat_device_memory_free() / 1024));
}

void sfx_set_enabled(bool enabled) { g_enabled = enabled; }

bool sfx_play_always(Sfx sfx)
{
    if (!g_ready || (unsigned)sfx >= SFX_COUNT || !g_pcm[sfx]) return false;
    const unsigned c = g_next;
    g_next = (g_next + 1) % CHANNELS;
    const int channel = FIRST_CHANNEL + (int)c;
    plat_audio_channel_clear(channel);
    plat_audio_channel_volume(channel, VOLUME, VOLUME);
    g_buf[c].data = g_pcm[sfx];
    g_buf[c].frames = g_frames[sfx];
    plat_audio_queue(channel, &g_buf[c]);
    return true;
}

void sfx_play(Sfx sfx)
{
    if (g_enabled) sfx_play_always(sfx);
}

void sfx_exit(void)
{
    if (audio_system_ready())
        for (unsigned c = 0; c < CHANNELS; ++c) plat_audio_channel_clear(FIRST_CHANNEL + c);
    for (unsigned i = 0; i < SFX_COUNT; ++i) {
        if (g_pcm[i]) plat_audio_free(g_pcm[i]);
        g_pcm[i] = NULL;
    }
    g_ready = false;
}

#include "menu_audio.h"
#include "platform.h"

#include <dirent.h>
#include <math.h>
#include <opus/opus.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "audio_output.h"
#include "diagnostic.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#define DR_MP3_IMPLEMENTATION
#include "../vendor/dr_libs/dr_mp3.h"
#pragma GCC diagnostic pop

/* The stream plays on channel 0 and the queue chime on 8. */
#define MUSIC_CHANNEL 9
#define VOICE_CHANNEL 10
/* 8 x ~43 ms queued: enough to ride out a busy main loop. */
#define BUF_FRAMES 2048
#define NBUF 8
#define OPUS_MAX_FRAMES 5760
#define MP3_CHUNK 2048
#define MAX_USER_SONGS 64
#define MAX_SONG_FILE (32u * 1024 * 1024)
/* Music sits well under the voice lines (built-in songs are leveled to
 * -24 LUFS, the voice to about -11), and dips further while she speaks. */
#define DUCK 0.35f
#define QUIET 0.45f
#define WAITING 0.5f
/* Players' songs are leveled toward the built-in songs' RMS (-25.7 dBFS). */
#define LEVEL_TARGET 0.052f

#define AUDIO_SYMBOLS(name) \
    extern const unsigned char _binary_##name##_opus_start[]; \
    extern const unsigned char _binary_##name##_opus_end[];
AUDIO_SYMBOLS(music1)
AUDIO_SYMBOLS(music2)
AUDIO_SYMBOLS(music3)
AUDIO_SYMBOLS(okaeri)
AUDIO_SYMBOLS(itterasshai)
#define AUDIO_ENTRY(name) _binary_##name##_opus_start, _binary_##name##_opus_end

typedef struct {
    const unsigned char *start, *end;
    const char *title;
} Embedded;

/* Pixabay Content License; see audio/CREDITS.txt. */
static const Embedded BUILTIN_SONGS[] = {
    { AUDIO_ENTRY(music1), "Bossa Nova Cafe Morning Breeze - Alex Morgan" },
    { AUDIO_ENTRY(music2), "Bossa Nova Morning Music - Andriih" },
    { AUDIO_ENTRY(music3), "Bossa Nova or Lofi - TheBoysBeats" },
};
#define BUILTIN_COUNT (sizeof(BUILTIN_SONGS) / sizeof(BUILTIN_SONGS[0]))
/* Indexed by MenuCue. */
static const Embedded CUES[] = {
    { AUDIO_ENTRY(okaeri), "okaeri" },
    { AUDIO_ENTRY(itterasshai), "itterasshai" },
};

typedef enum { SRC_NONE, SRC_OGG, SRC_MP3 } SourceKind;

typedef struct {
    int channel;
    SourceKind kind;
    /* Ogg Opus, from memory (built in, or a whole file read from the SD card). */
    const unsigned char *data;
    size_t size;
    unsigned char *owned;
    size_t page_end, seg_pos;
    const unsigned char *segs;
    unsigned nsegs, seg_index;
    unsigned char packet[8192];
    size_t packet_len;
    OpusDecoder *opus;
    int preskip;
    /* MP3, streamed from the SD card. */
    drmp3 mp3;
    unsigned mp3_channels;
    /* Decoded stereo frames waiting to go into a wave buffer. */
    s16 fifo[OPUS_MAX_FRAMES * 2];
    unsigned fifo_len, fifo_pos;
    unsigned rate;
    bool eof;
    /* Output. */
    s16 *pcm;
    PlatAudioBuffer wb[NBUF];
    unsigned next;
    /* Loudness leveling (players' songs only). */
    bool level;
    float env, gain;
} Source;

static Source g_music, g_voice;
static PlatThread g_thread;
static volatile bool g_running;
static volatile int g_mode = MENU_MUSIC_ON;
static volatile int g_scene = MENU_SCENE_MENUS;
static volatile int g_cue = -1;
static volatile u64 g_cue_at;
static volatile bool g_next;
static PlatLock g_title_lock;
static char g_title[96];
static char g_title_copy[96];

static char g_user_songs[MAX_USER_SONGS][160];
static unsigned g_user_count;
static bool g_scanned;
static int g_last_song = -1;
static u32 g_rng;

static u32 random_next(void)
{
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return g_rng;
}

static void set_title(const char *title)
{
    plat_lock(&g_title_lock);
    snprintf(g_title, sizeof(g_title), "%s", title ? title : "");
    plat_unlock(&g_title_lock);
}

/* ---- Decoding ------------------------------------------------------------ */

/* Next complete packet of the Ogg stream; false at the end. */
static bool ogg_packet(Source *s, const unsigned char **out, size_t *len)
{
    s->packet_len = 0;
    bool too_big = false;
    for (;;) {
        if (s->seg_index >= s->nsegs) {
            const size_t p = s->page_end;
            if (p + 27 > s->size || memcmp(s->data + p, "OggS", 4) != 0) return false;
            const unsigned n = s->data[p + 26];
            if (p + 27 + n > s->size) return false;
            size_t body = 0;
            for (unsigned i = 0; i < n; ++i) body += s->data[p + 27 + i];
            if (p + 27 + n + body > s->size) return false;
            s->segs = s->data + p + 27;
            s->nsegs = n;
            s->seg_index = 0;
            s->seg_pos = p + 27 + n;
            s->page_end = s->seg_pos + body;
            continue;
        }
        const unsigned seg = s->segs[s->seg_index++];
        if (s->packet_len + seg <= sizeof(s->packet))
            memcpy(s->packet + s->packet_len, s->data + s->seg_pos, seg);
        else
            too_big = true;
        s->packet_len += seg;
        s->seg_pos += seg;
        if (seg < 255) {
            if (too_big) {
                s->packet_len = 0;
                too_big = false;
                continue;
            }
            *out = s->packet;
            *len = s->packet_len;
            return true;
        }
    }
}

/* Refill the FIFO; false when the song is over. */
static bool decode_more(Source *s)
{
    s->fifo_len = s->fifo_pos = 0;
    if (s->kind == SRC_OGG) {
        for (;;) {
            const unsigned char *packet;
            size_t len;
            if (!ogg_packet(s, &packet, &len)) return false;
            if (len >= 8 && memcmp(packet, "OpusHead", 8) == 0) {
                if (len >= 12) s->preskip = packet[10] | (packet[11] << 8);
                continue;
            }
            if ((len >= 8 && memcmp(packet, "OpusTags", 8) == 0) || len == 0) continue;
            int frames = opus_decode(s->opus, packet, (opus_int32)len, s->fifo, OPUS_MAX_FRAMES, 0);
            if (frames <= 0) continue;
            int skip = 0;
            if (s->preskip > 0) {
                skip = s->preskip < frames ? s->preskip : frames;
                s->preskip -= skip;
            }
            if (skip >= frames) continue;
            if (skip) memmove(s->fifo, s->fifo + skip * 2, (size_t)(frames - skip) * 2 * sizeof(s16));
            s->fifo_len = (unsigned)(frames - skip);
            return true;
        }
    }
    if (s->kind == SRC_MP3) {
        const drmp3_uint64 got = drmp3_read_pcm_frames_s16(&s->mp3, MP3_CHUNK, s->fifo);
        if (!got) return false;
        if (s->mp3_channels == 1)
            for (int i = (int)got - 1; i >= 0; --i) s->fifo[i * 2] = s->fifo[i * 2 + 1] = s->fifo[i];
        s->fifo_len = (unsigned)got;
        return true;
    }
    return false;
}

static unsigned fill(Source *s, s16 *out, unsigned frames)
{
    unsigned done = 0;
    while (done < frames) {
        if (s->fifo_pos >= s->fifo_len && (s->eof || !decode_more(s))) {
            s->eof = true;
            break;
        }
        unsigned take = s->fifo_len - s->fifo_pos;
        if (take > frames - done) take = frames - done;
        memcpy(out + done * 2, s->fifo + s->fifo_pos * 2, take * 2 * sizeof(s16));
        done += take;
        s->fifo_pos += take;
    }
    return done;
}

/* Slow automatic gain toward LEVEL_TARGET, so a loud MP3 doesn't drown the
 * voice and a quiet one isn't lost. Ramped across the block: no clicks. */
static void level(Source *s, s16 *pcm, unsigned frames)
{
    double sum = 0.0;
    for (unsigned i = 0; i < frames * 2; ++i) sum += (double)pcm[i] * pcm[i];
    const float ms = (float)(sum / (frames * 2)) / (32768.0f * 32768.0f);
    const float dt = (float)frames / (float)s->rate;
    if (s->env <= 0.0f) s->env = fmaxf(ms, LEVEL_TARGET * LEVEL_TARGET);
    else s->env += (ms - s->env) * (1.0f - expf(-dt / 3.0f));
    float want = LEVEL_TARGET / sqrtf(fmaxf(s->env, 1e-7f));
    want = fminf(2.5f, fmaxf(0.2f, want));
    const float from = s->gain;
    s->gain += (want - s->gain) * (1.0f - expf(-dt / 1.0f));
    for (unsigned i = 0; i < frames; ++i) {
        const float g = from + (s->gain - from) * (float)i / (float)frames;
        for (unsigned c = 0; c < 2; ++c) {
            const float v = pcm[i * 2 + c] * g;
            pcm[i * 2 + c] = (s16)(v > 32767.0f ? 32767.0f : v < -32768.0f ? -32768.0f : v);
        }
    }
}

static void source_close(Source *s)
{
    if (s->opus) opus_decoder_destroy(s->opus);
    s->opus = NULL;
    if (s->kind == SRC_MP3) drmp3_uninit(&s->mp3);
    free(s->owned);
    s->owned = NULL;
    s->data = NULL;
    s->kind = SRC_NONE;
}

static bool source_open_ogg(Source *s, const unsigned char *data, size_t size, unsigned char *owned)
{
    int error = OPUS_OK;
    s->opus = opus_decoder_create(48000, 2, &error);
    if (!s->opus || error != OPUS_OK) {
        free(owned);
        s->opus = NULL;
        return false;
    }
    s->kind = SRC_OGG;
    s->data = data;
    s->size = size;
    s->owned = owned;
    s->page_end = s->seg_pos = 0;
    s->nsegs = s->seg_index = 0;
    s->preskip = 0;
    s->rate = 48000;
    return true;
}

static bool source_open_file(Source *s, const char *path)
{
    const char *dot = strrchr(path, '.');
    if (dot && strcasecmp(dot, ".mp3") == 0) {
        if (!drmp3_init_file(&s->mp3, path, NULL)) return false;
        if (s->mp3.channels < 1 || s->mp3.channels > 2 || !s->mp3.sampleRate) {
            drmp3_uninit(&s->mp3);
            return false;
        }
        s->kind = SRC_MP3;
        s->mp3_channels = s->mp3.channels;
        s->rate = s->mp3.sampleRate;
        return true;
    }
    FILE *file = fopen(path, "rb");
    if (!file) return false;
    fseek(file, 0, SEEK_END);
    const long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    unsigned char *data = size > 0 && (unsigned long)size <= MAX_SONG_FILE ? malloc((size_t)size) : NULL;
    const bool read = data && fread(data, 1, (size_t)size, file) == (size_t)size;
    fclose(file);
    if (!read) {
        free(data);
        return false;
    }
    return source_open_ogg(s, data, (size_t)size, data);
}

/* ---- Output -------------------------------------------------------------- */

static void set_volume(int channel, float volume)
{
    plat_audio_channel_volume(channel, volume, volume);
}

static void channel_begin(Source *s, float volume)
{
    plat_audio_channel_open(s->channel, (float)s->rate, 2, true);
    set_volume(s->channel, volume);
    memset(s->wb, 0, sizeof(s->wb));
    for (unsigned i = 0; i < NBUF; ++i) s->wb[i].data = s->pcm + i * BUF_FRAMES * 2;
    s->next = 0;
    s->fifo_len = s->fifo_pos = 0;
    s->eof = false;
    s->env = 0.0f;
    s->gain = 1.0f;
}

static void channel_end(Source *s)
{
    plat_audio_channel_close(s->channel);
    source_close(s);
}

/* Keep the wave buffers full; false once the song has finished playing. */
static bool pump(Source *s)
{
    for (unsigned k = 0; k < NBUF && !s->eof; ++k) {
        PlatAudioBuffer *w = &s->wb[s->next];
        if (!plat_audio_buffer_idle(w)) break;
        const unsigned frames = fill(s, w->data, BUF_FRAMES);
        if (!frames) break;
        if (s->level) level(s, w->data, frames);
        w->frames = frames;
        plat_audio_queue(s->channel, w);
        s->next = (s->next + 1) % NBUF;
    }
    for (unsigned i = 0; i < NBUF; ++i)
        if (!plat_audio_buffer_idle(&s->wb[i])) return true;
    return !s->eof;
}

/* ---- Playlist ------------------------------------------------------------ */

static void scan_user_songs(void)
{
    g_scanned = true;
    mkdir(MENU_MUSIC_DIR, 0777); /* so players can find where songs go */
    DIR *dir = opendir(MENU_MUSIC_DIR);
    if (!dir) return;
    struct dirent *entry;
    while ((entry = readdir(dir)) && g_user_count < MAX_USER_SONGS) {
        const char *dot = strrchr(entry->d_name, '.');
        if (!dot || (strcasecmp(dot, ".mp3") != 0 && strcasecmp(dot, ".opus") != 0)) continue;
        if (strlen(entry->d_name) > 130) continue; /* path would not fit */
        snprintf(g_user_songs[g_user_count++], sizeof(g_user_songs[0]), "%s/%.130s", MENU_MUSIC_DIR, entry->d_name);
    }
    closedir(dir);
    diagnostic_log("MUSIC", "%u song%s of the player's own in %s", g_user_count,
                   g_user_count == 1 ? "" : "s", MENU_MUSIC_DIR);
}

static bool start_song(void)
{
    if (!g_scanned) scan_user_songs();
    const unsigned count = g_user_count ? g_user_count : BUILTIN_COUNT;
    unsigned pick = random_next() % count;
    if (count > 1 && (int)pick == g_last_song) pick = (pick + 1 + random_next() % (count - 1)) % count;
    for (unsigned tries = 0; tries < count; ++tries, pick = (pick + 1) % count) {
        bool opened;
        char title[96];
        if (g_user_count) {
            const char *path = g_user_songs[pick];
            opened = source_open_file(&g_music, path);
            const char *name = strrchr(path, '/');
            snprintf(title, sizeof(title), "%s", name ? name + 1 : path);
            char *dot = strrchr(title, '.');
            if (dot) *dot = '\0';
            if (!opened) diagnostic_log("MUSIC", "can't play %s", path);
        } else {
            const Embedded *song = &BUILTIN_SONGS[pick];
            opened = source_open_ogg(&g_music, song->start, (size_t)(song->end - song->start), NULL);
            snprintf(title, sizeof(title), "%s", song->title);
        }
        if (!opened) continue;
        g_music.level = g_user_count > 0;
        channel_begin(&g_music, 0.0f);
        g_last_song = (int)pick;
        set_title(title);
        diagnostic_log("MUSIC", "playing %s (%u Hz)", title, g_music.rate);
        return true;
    }
    return false;
}

/* ---- Thread -------------------------------------------------------------- */

static float approach(float value, float target, float up, float down, float dt)
{
    if (value < target) return fminf(target, value + up * dt);
    return fmaxf(target, value - down * dt);
}

static void audio_main(void *arg)
{
    (void)arg;
    float volume = 0.0f;
    bool fading_in = false, skipping = false, gave_up = false;
    u64 next_song_at = 0, last = plat_time_ms();
    while (g_running) {
        const u64 now = plat_time_ms();
        const float dt = fminf(0.2f, (float)(now - last) / 1000.0f);
        last = now;

        const int cue = g_cue;
        if (cue >= 0 && now >= g_cue_at) {
            g_cue = -1;
            if (g_voice.kind != SRC_NONE) channel_end(&g_voice);
            const Embedded *line = &CUES[cue];
            if (source_open_ogg(&g_voice, line->start, (size_t)(line->end - line->start), NULL)) {
                g_voice.level = false;
                channel_begin(&g_voice, 1.0f);
                diagnostic_log("MUSIC", "voice %s", line->title);
            }
        }
        const bool speaking = g_voice.kind != SRC_NONE && pump(&g_voice);
        if (g_voice.kind != SRC_NONE && !speaking) channel_end(&g_voice);

        const int mode = g_mode, scene = g_scene;
        float base = mode == MENU_MUSIC_ON ? 1.0f : mode == MENU_MUSIC_QUIET ? QUIET : 0.0f;
        if (scene == MENU_SCENE_WAITING) base *= WAITING;
        else if (scene == MENU_SCENE_GAME) base = 0.0f;
        if (g_next) {
            g_next = false;
            if (g_music.kind != SRC_NONE) skipping = true;
            else next_song_at = 0;
        }
        if (base <= 0.0f) gave_up = false; /* try again next time music is wanted */

        if (g_music.kind == SRC_NONE && base > 0.0f && !gave_up && now >= next_song_at) {
            if (start_song()) {
                volume = 0.0f;
                fading_in = true;
            } else {
                gave_up = true;
                diagnostic_log("MUSIC", "no playable songs");
            }
        }
        if (g_music.kind != SRC_NONE) {
            const float target = skipping ? 0.0f : base * (speaking ? DUCK : 1.0f);
            /* Fade in over ~2 s, back up from a dip in ~0.6 s, dip in ~0.3 s,
             * fade out over ~1 s. */
            volume = approach(volume, target, fading_in ? 0.5f : 1.1f, speaking ? 2.5f : 1.0f, dt);
            if (fading_in && volume >= target) fading_in = false;
            set_volume(MUSIC_CHANNEL, volume);
            const bool playing = pump(&g_music);
            if (!playing || (volume <= 0.0f && target <= 0.0f)) {
                channel_end(&g_music);
                set_title("");
                next_song_at = !playing && !skipping ? now + 1500 : now;
                skipping = false;
            }
        }
        plat_sleep_ns(10 * 1000000LL);
    }
}

/* ---- API ----------------------------------------------------------------- */

void menu_audio_start(void)
{
    if (g_thread || !audio_system_ready()) return;
    plat_lock_init(&g_title_lock);
    const size_t bytes = NBUF * BUF_FRAMES * 2 * sizeof(s16);
    g_music.pcm = plat_audio_alloc(bytes);
    g_voice.pcm = plat_audio_alloc(bytes);
    if (!g_music.pcm || !g_voice.pcm) {
        diagnostic_log("MUSIC", "sound memory allocation failed: no menu music");
        menu_audio_exit();
        return;
    }
    g_music.channel = MUSIC_CHANNEL;
    g_voice.channel = VOICE_CHANNEL;
    g_rng = (u32)plat_ticks() | 1u;
    g_running = true;
    const int priority = plat_thread_priority();
    /* Above the main loop: it only wakes every 10 ms and decodes a little. */
    g_thread = plat_thread_create(audio_main, NULL, 64 * 1024, priority - 1, -2, false);
    if (!g_thread) {
        g_running = false;
        diagnostic_log("MUSIC", "thread failed: no menu music");
        menu_audio_exit();
    }
}

void menu_audio_set(MenuMusicMode mode, MenuScene scene)
{
    g_mode = mode;
    g_scene = scene;
}

void menu_audio_cue(MenuCue cue)
{
    if (!g_thread || (unsigned)cue >= sizeof(CUES) / sizeof(CUES[0])) return;
    g_cue_at = plat_time_ms() + (cue == MENU_CUE_OKAERI ? 700 : 0);
    g_cue = cue;
}

void menu_audio_next(void) { g_next = true; }

const char *menu_audio_now_playing(void)
{
    if (!g_thread) return "";
    plat_lock(&g_title_lock);
    snprintf(g_title_copy, sizeof(g_title_copy), "%s", g_title);
    plat_unlock(&g_title_lock);
    return g_title_copy;
}

void menu_audio_exit(void)
{
    if (g_thread) {
        g_running = false;
        plat_thread_join(g_thread);
        g_thread = NULL;
    }
    if (audio_system_ready()) {
        if (g_music.kind != SRC_NONE) channel_end(&g_music);
        if (g_voice.kind != SRC_NONE) channel_end(&g_voice);
    }
    source_close(&g_music);
    source_close(&g_voice);
    if (g_music.pcm) plat_audio_free(g_music.pcm);
    if (g_voice.pcm) plat_audio_free(g_voice.pcm);
    g_music.pcm = g_voice.pcm = NULL;
}

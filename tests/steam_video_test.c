/* Offline wire test: encoder capture failure, recovery, and IDR requests.
 * Include the session implementation to drive the same decrypted messages
 * the host sends, with a fake UDP socket and clock. */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <windows.h>
#include <bcrypt.h>
#include "../source/steam_session.c"

static uint64_t now_ms = 1000;
static uint8_t sent[SLOT_DATA];
static size_t sent_size;
static unsigned sent_count, pictures;
static uint8_t assembled[256];
static size_t assembled_size;

static void assembled_picture(void *user, const uint8_t *data, size_t size, bool keyframe)
{
    (void)user;
    assert(keyframe && size <= sizeof(assembled));
    memcpy(assembled, data, size);
    assembled_size = size;
    ++pictures;
}

/* Go through the outer Steam data header too: frame ID is distinct from
 * the delivery sequence, and all slices of a picture have the same ID. */
static void slice(SteamSession *s, uint16_t sequence, uint16_t first, uint16_t last,
                  uint8_t flags, const uint8_t *data, size_t size)
{
    uint8_t message[256] = {1};
    assert(size + 20 <= sizeof(message));
    put16(message + 1, 77);
    put16(message + 13, sequence);
    message[15] = flags;
    put16(message + 16, first);
    put16(message + 18, last);
    memcpy(message + 20, data, size);
    on_data_message(s, 4, message, size + 20);
}

static void test_parallel_slices(void)
{
    SteamSession *s = calloc(1, sizeof(*s));
    assert(s);
    s->video_channel = 4;
    s->cb.video = assembled_picture;
    pictures = 0;
    const uint8_t sps[] = {0x67, 0x4d, 0x00, 0x16};
    const uint8_t top[] = {0x65, 0x11};
    const uint8_t bottom[] = {0x65, 0x33};
    const uint8_t middle[] = {0x65, 0x22};
    const uint8_t expected[] = {0,0,0,1,0x67,0x4d,0,0x16,
                               0,0,0,1,0x65,0x11, 0,0,0,1,0x65,0x22, 0,0,0,1,0x65,0x33};
    /* 0x04 closes this slice range. The final slice can finish encoding
     * before the middle one, without any UDP packet loss or reordering. */
    slice(s, 100, 0, 9, VF_KEYFRAME | VF_ESCAPE | VF_START_SEQUENCE, sps, sizeof(sps));
    slice(s, 101, 0, 9, 0x04 | VF_ESCAPE | VF_START_SEQUENCE, top, sizeof(top));
    slice(s, 102, 20, 29, 0x04 | VF_FRAME_FINISH | VF_ESCAPE | VF_START_SEQUENCE, bottom, sizeof(bottom));
    assert(pictures == 0); /* Never publish an incomplete picture. */
    slice(s, 103, 10, 19, 0x04 | VF_ESCAPE | VF_START_SEQUENCE, middle, sizeof(middle));
    assert(pictures == 1 && assembled_size == sizeof(expected));
    assert(!memcmp(assembled, expected, sizeof(expected)));
    assert(!s->video_part_count && !s->video_part_bytes);

    /* A missing middle slice must expire, free its queued tail, and ask for
     * an IDR. A healthy audio connection must not keep this frame forever. */
    s->state = STEAM_SESSION_STREAMING;
    s->last_heard_at = now_ms;
    slice(s, 104, 0, 9, VF_KEYFRAME | VF_ESCAPE | VF_START_SEQUENCE | 0x04, top, sizeof(top));
    slice(s, 105, 20, 29, 0x04 | VF_FRAME_FINISH | VF_ESCAPE | VF_START_SEQUENCE, bottom, sizeof(bottom));
    const unsigned requests = s->stats.keyframe_requests;
    now_ms += 201;
    steam_session_tick(s);
    assert(s->waiting_key && !s->video_part_count && !s->video_part_bytes && !s->frame_size);
    assert(s->stats.keyframe_requests == requests + 1 && pictures == 1);

    /* A new IDR also clears an incomplete old picture, without combining
     * their bytes. Sequence wrap is valid. */
    slice(s, 65534, 0, 9, VF_KEYFRAME | VF_ESCAPE | VF_START_SEQUENCE | 0x04, top, sizeof(top));
    slice(s, 65535, 20, 29, 0x04 | VF_FRAME_FINISH | VF_ESCAPE | VF_START_SEQUENCE, bottom, sizeof(bottom));
    slice(s, 0, 0, 0, VF_KEYFRAME | VF_ESCAPE | VF_START_SEQUENCE | VF_FRAME_FINISH, top, sizeof(top));
    assert(pictures == 2 && assembled_size == 6 && !s->video_part_count);
    assert(!memcmp(assembled + 4, top, sizeof(top)));
    free(s);
}

static FILE *replay_output;
static void replay_picture(void *user, const uint8_t *data, size_t size, bool keyframe)
{
    (void)user; (void)keyframe;
    assert(fwrite(data, 1, size, replay_output) == size);
    ++pictures;
}

static int replay(const char *input_path, const char *output_path)
{
    FILE *input = fopen(input_path, "rb");
    replay_output = fopen(output_path, "wb");
    assert(input && replay_output);
    SteamSession *s = calloc(1, sizeof(*s));
    assert(s);
    s->video_channel = 4;
    s->cb.video = replay_picture;
    uint8_t length[4];
    while (fread(length, 1, 4, input) == 4) {
        const size_t size = get32(length);
        assert(size >= 20 && size <= FRAME_MAX + 20);
        uint8_t *message = malloc(size);
        assert(message && fread(message, 1, size, input) == size);
        on_data_message(s, 4, message, size);
        free(message);
    }
    assert(!s->video_part_count && !s->frame_size && !s->waiting_key);
    printf("Steam replay: %u complete pictures, %u reordered slices, %u lost\n",
           pictures, s->video_reordered, s->stats.video_lost);
    clear_video_frame(s);
    free(s);
    fclose(input);
    fclose(replay_output);
    return 0;
}

uint64_t steam_now_ms(void) { return now_ms; }
int steam_udp_open(void) { return 1; }
void steam_udp_close(int sock) { (void)sock; }
bool steam_udp_send(int sock, uint32_t ip, uint16_t port, const void *data, size_t size)
{
    (void)sock; (void)ip; (void)port;
    assert(size <= sizeof(sent));
    memcpy(sent, data, size);
    sent_size = size;
    ++sent_count;
    return true;
}
int steam_udp_recv(int sock, void *data, size_t cap, uint32_t *ip, uint16_t *port)
{ (void)sock; (void)data; (void)cap; (void)ip; (void)port; return 0; }
bool steam_udp_wait(int sock, int timeout_ms) { (void)sock; (void)timeout_ms; return false; }
void steam_ip_format(uint32_t ip, char *out, size_t size)
{ (void)ip; snprintf(out, size, "127.0.0.1"); }
void diagnostic_log(const char *component, const char *format, ...)
{ (void)component; (void)format; }
int mbedtls_hardware_poll(void *data, unsigned char *output, size_t len, size_t *olen)
{
    (void)data;
    *olen = BCryptGenRandom(NULL, output, (ULONG)len, BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0 ? len : 0;
    return *olen == len ? 0 : -1;
}

static void encoder(SteamSession *s, const char *name)
{
    uint8_t plain[160], message[256];
    PbWriter p;
    pb_writer(&p, plain, sizeof(plain));
    pb_string(&p, 1, name);
    message[0] = CTL_VIDEO_ENCODER_INFO;
    const size_t n = steam_frame_encrypt(s->config.key, s->config.key_size,
                                         s->recv_sequence, plain, p.len, message + 1, sizeof(message) - 1);
    assert(n);
    on_control(s, message, n + 1);
}

static void picture(void *user, const uint8_t *data, size_t size, bool keyframe)
{
    (void)user;
    const uint8_t expected[] = { 0, 0, 0, 1, 0x65, 0x88 };
    assert(keyframe && size == sizeof(expected) && !memcmp(data, expected, size));
    ++pictures;
}

int main(int argc, char **argv)
{
    if (argc == 4 && !strcmp(argv[1], "replay")) return replay(argv[2], argv[3]);
    SteamSession *s = calloc(1, sizeof(*s));
    assert(s);
    s->state = STEAM_SESSION_STREAMING;
    s->config.key_size = 16;
    s->video_channel = -1;
    s->cb.video = picture;
    assert(!steam_session_request_keyframe(s) && !sent_count);
    s->video_channel = 4;
    assert(steam_session_request_keyframe(s));
    assert(sent_size == HEADER_SIZE + 1 + 4 && sent[4] == 4 && sent[HEADER_SIZE] == 2);
    assert(get32(sent + sent_size - 4) == steam_crc32c(sent, sent_size - 4));
    assert(!steam_session_request_keyframe(s) && sent_count == 1);
    now_ms += 200;
    assert(steam_session_request_keyframe(s) && sent_count == 2);

    encoder(s, "Desktop Black Frame + libx264 main (4 threads)");
    SteamSessionStats stats;
    steam_session_stats(s, &stats);
    assert(stats.capture_unavailable && strstr(steam_session_status(s), "capture"));
    uint8_t idr[] = { 0, 0, VF_KEYFRAME | VF_START_SEQUENCE | VF_ESCAPE | VF_FRAME_FINISH,
                      0, 0, 0, 0, 0x65, 0x88 };
    video_frame(s, 0, idr, sizeof(idr));
    assert(!pictures);
    now_ms += 200;
    encoder(s, "Desktop OpenGL NV12 + libx264 main (4 threads)");
    steam_session_stats(s, &stats);
    assert(!stats.capture_unavailable && s->waiting_key && sent_count == 3);
    uint8_t delta[] = { 1, 0, VF_START_SEQUENCE | VF_ESCAPE | VF_FRAME_FINISH,
                        0, 0, 0, 0, 0x41, 0x88 };
    video_frame(s, 1, delta, sizeof(delta));
    assert(!pictures);
    idr[0] = 2;
    video_frame(s, 2, idr, sizeof(idr));
    assert(pictures == 1 && !s->waiting_key);
    free(s);
    test_parallel_slices();
    puts("Steam video: IDR requests, capture recovery, slice ordering, timeout, IDR reset and sequence wrap passed");
    return 0;
}

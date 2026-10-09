/* PC test of the Steam Link code against a real Steam on the network:
 * discovery, then (with a pairing already made) a stream request and a
 * session that writes the video to a file and moves the right stick.
 *
 *   steam_host_test discover
 *   steam_host_test pair HOST PIN DEVICE_ID SECRET_HEX
 *   steam_host_test stream HOST SECONDS DEVICE_ID HOST_SECRET_HEX OUT.h264 [gameid]
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "steam_crypto.h"
#include "steam_remote.h"
#include "steam_session.h"
#include "steam_udp.h"

void diagnostic_log(const char *component, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    printf("%7.3f [%s] ", (double)GetTickCount64() / 1000.0, component);
    vprintf(format, args);
    printf("\n");
    va_end(args);
    fflush(stdout);
}

/* vendor/mbedtls is configured for the 3DS (MBEDTLS_ENTROPY_HARDWARE_ALT). */
#include <bcrypt.h>
int mbedtls_hardware_poll(void *data, unsigned char *output, size_t len, size_t *olen)
{
    (void)data;
    *olen = BCryptGenRandom(NULL, output, (ULONG)len, BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0 ? len : 0;
    return 0;
}

static bool hex_bytes(const char *hex, uint8_t *out, size_t size)
{
    if (strlen(hex) != size * 2) return false;
    for (size_t i = 0; i < size; ++i) {
        unsigned v;
        if (sscanf(hex + 2 * i, "%2x", &v) != 1) return false;
        out[i] = (uint8_t)v;
    }
    return true;
}

static FILE *g_video;
static unsigned g_frames, g_audio;

static void on_video_start(void *user, unsigned width, unsigned height)
{
    (void)user;
    printf("video start %ux%u\n", width, height);
}

static void on_video(void *user, const uint8_t *data, size_t size, bool keyframe)
{
    (void)user;
    if (g_video) fwrite(data, 1, size, g_video);
    if (++g_frames <= 3 || g_frames % 60 == 0)
        printf("frame %u %zu bytes%s\n", g_frames, size, keyframe ? " key" : "");
}

static void on_audio(void *user, const uint8_t *data, size_t size, uint16_t sequence)
{
    (void)user; (void)data; (void)size; (void)sequence;
    g_audio++;
}

static void on_activity(void *user, int activity, uint64_t gameid, const char *name)
{
    (void)user;
    printf("activity %d game %llu %s\n", activity, (unsigned long long)gameid, name);
}

int main(int argc, char **argv)
{
    SteamIdentity id;
    memset(&id, 0, sizeof(id));
    snprintf(id.name, sizeof(id.name), "Kasumi Test");
    if (argc >= 2 && !strcmp(argv[1], "selftest")) {
        /* RFC 7748 section 6.1, and Steam's secret = SHA256(shared). */
        uint8_t alice[32], bob[32], alice_pub[32], bob_pub[32], expect_shared[32], expect_secret[32];
        uint8_t got[32], secret_a[32], secret_b[32];
        hex_bytes("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a", alice, 32);
        hex_bytes("5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb", bob, 32);
        hex_bytes("8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a", alice_pub, 32);
        hex_bytes("de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f", bob_pub, 32);
        hex_bytes("4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742", expect_shared, 32);
        steam_sha256(expect_shared, 32, expect_secret);
        bool ok = steam_key_exchange(alice, bob_pub, secret_a) && steam_key_exchange(bob, alice_pub, secret_b) &&
                  !memcmp(secret_a, expect_secret, 32) && !memcmp(secret_b, expect_secret, 32);
        printf("x25519 exchange: %s\n", ok ? "ok" : "FAIL");
        uint8_t priv[32], pub[32];
        ok = steam_x25519_keypair(priv, pub);
        printf("x25519 keypair: %s\n", ok ? "ok" : "FAIL");
        uint8_t key[32], plain[40] = "Kasumi Steam Link crypto self test", cipher[96], back[96];
        steam_random(key, sizeof(key));
        const size_t n = steam_sym_encrypt(key, 32, plain, sizeof(plain), cipher, sizeof(cipher));
        const int m = steam_sym_decrypt(key, 32, cipher, n, back, sizeof(back));
        printf("symmetric round trip: %s\n", m == (int)sizeof(plain) && !memcmp(back, plain, sizeof(plain)) ? "ok" : "FAIL");
        uint64_t seq;
        const uint8_t *msg;
        const size_t f = steam_frame_encrypt(key, 16, 77, plain, sizeof(plain), cipher, sizeof(cipher));
        const int g = steam_frame_decrypt(key, 16, cipher, f, back, sizeof(back), &seq, &msg);
        printf("frame round trip: %s\n", g == (int)sizeof(plain) && seq == 77 && !memcmp(msg, plain, sizeof(plain)) ? "ok" : "FAIL");
        uint8_t rsa[256];
        printf("rsa oaep: %s\n", steam_rsa_encrypt(plain, sizeof(plain), rsa, sizeof(rsa)) == 256 ? "ok" : "FAIL");
        printf("crc32c(Connect)=%08x (expect 3c8f3dc7)\n", steam_crc32c("Connect", 7));
        (void)got;
        return 0;
    }
    if (argc >= 2 && !strcmp(argv[1], "discover")) {
        id.device_id = 1;
        SteamHost hosts[8];
        const int n = steam_discover(&id, 0, hosts, 8, 1500);
        for (int i = 0; i < n; ++i) {
            char ip[20];
            steam_ip_format(hosts[i].ip, ip, sizeof(ip));
            printf("%s %s client=%llu steamid=%llu\n", hosts[i].name, ip,
                   (unsigned long long)hosts[i].client_id, (unsigned long long)hosts[i].steamid);
        }
        return n ? 0 : 1;
    }
    if (argc >= 6 && !strcmp(argv[1], "pair")) {
        id.device_id = _strtoui64(argv[4], NULL, 10);
        if (!hex_bytes(argv[5], id.secret, 32)) return 2;
        SteamHost host;
        if (steam_discover(&id, steam_ip_parse(argv[2]), &host, 1, 2000) != 1) return 3;
        SteamPairing p;
        if (!steam_pair_begin(&p, &id, &host, argv[3])) return 4;
        printf("type PIN %s into Steam\n", argv[3]);
        for (;;) {
            const SteamPairState state = steam_pair_poll(&p);
            if (state == STEAM_PAIR_DONE) {
                printf("paired steamid=%llu secret=", (unsigned long long)p.steamid);
                for (int i = 0; i < 32; ++i) printf("%02x", p.secret[i]);
                printf("\n");
                return 0;
            }
            if (state != STEAM_PAIR_WAITING) {
                printf("pairing ended state=%d result=%d\n", state, p.result);
                return 5;
            }
            Sleep(50);
        }
    }
    if (argc >= 7 && !strcmp(argv[1], "stream")) {
        id.device_id = _strtoui64(argv[4], NULL, 10);
        uint8_t secret[32];
        if (!hex_bytes(argv[5], secret, 32)) return 2;
        SteamHost host;
        if (steam_discover(&id, steam_ip_parse(argv[2]), &host, 1, 2000) != 1) return 3;
        SteamStreamRequest request = { 800, 480, 30, argc >= 8 ? _strtoui64(argv[7], NULL, 10) : 0, 3 };
        /* gameid 0 with a later argument: the desktop. */
        if (request.gameid) request.stream_interface = 2;
        SteamStreamGrant grant;
        if (!steam_request_stream(&id, secret, &host, &request, &grant, 15000, NULL)) {
            printf("refused: %s\n", steam_stream_result_text(grant.result));
            return 4;
        }
        SteamSessionConfig config;
        memset(&config, 0, sizeof(config));
        config.host_ip = host.ip;
        config.port = grant.port;
        memcpy(config.key, grant.session_key, grant.session_key_size);
        config.key_size = grant.session_key_size;
        config.steamid = host.steamid;
        config.width = 800; config.height = 480; config.fps = 30; config.kbps = 3000;
        SteamSessionCallbacks cb = { on_video_start, on_video, on_audio, on_activity, NULL };
        g_video = fopen(argv[6], "wb");
        SteamSession *s = steam_session_open(&config, &cb);
        if (!s) return 5;
        const uint64_t end = steam_now_ms() + (uint64_t)atoi(argv[3]) * 1000;
        const uint64_t start = steam_now_ms();
        while (steam_now_ms() < end) {
            steam_udp_wait(steam_session_socket(s), 4);
            while (steam_session_receive(s) > 0) {}
            /* The right stick to the right from 4 to 6 s in: the PC's
             * desktop layout moves the mouse. */
            const uint64_t t = steam_now_ms() - start;
            SteamPad pad;
            memset(&pad, 0, sizeof(pad));
            const bool mouse = argc >= 9 && !strcmp(argv[8], "mouse");
            static uint64_t last_mouse;
            if (t > 4000 && t < 6000) {
                if (!mouse) pad.axes[2] = 30000;
                else if (steam_now_ms() - last_mouse >= 16) {
                    /* Through Steam's mouse messages instead of the pad. */
                    steam_session_mouse_move(s, -12, 0);
                    last_mouse = steam_now_ms();
                }
            }
            steam_session_set_pad(s, &pad);
            steam_session_tick(s);
            const SteamSessionState state = steam_session_state(s);
            if (state == STEAM_SESSION_FAILED || state == STEAM_SESSION_CLOSED) break;
        }
        if (argc >= 9 && !strcmp(argv[8], "quit")) {
            printf("quit game sent=%d\n", steam_session_stop_game(s));
            /* Watch the PC's activity change for a few seconds. */
            const uint64_t watch = steam_now_ms() + 5000;
            while (steam_now_ms() < watch) {
                steam_udp_wait(steam_session_socket(s), 4);
                while (steam_session_receive(s) > 0) {}
                steam_session_tick(s);
            }
        }
        SteamSessionStats stats;
        steam_session_stats(s, &stats);
        printf("state=%d status=%s frames=%u key=%u lost=%u audio=%u rtt=%d hid=%u input=%d\n",
               steam_session_state(s), steam_session_status(s), stats.video_frames, stats.video_keyframes,
               stats.video_lost, g_audio, stats.rtt_ms, stats.hid_reports, stats.input_ready);
        steam_session_close(s);
        if (g_video) fclose(g_video);
        return stats.video_frames ? 0 : 6;
    }
    fprintf(stderr, "usage: discover | pair HOST PIN ID SECRET | stream HOST SECONDS ID HOST_SECRET OUT [gameid]\n");
    return 1;
}

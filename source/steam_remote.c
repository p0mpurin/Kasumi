#include "steam_remote.h"

#include <stdio.h>
#include <string.h>

#include "diagnostic.h"
#include "steam_crypto.h"
#include "steam_proto.h"
#include "steam_udp.h"

/* ERemoteClientBroadcastMsg */
enum {
    MSG_DISCOVERY = 0, MSG_STATUS = 1, MSG_AUTH_REQUEST = 3, MSG_AUTH_RESPONSE = 4,
    MSG_STREAM_REQUEST = 5, MSG_STREAM_RESPONSE = 6, MSG_PROOF_REQUEST = 7, MSG_PROOF_RESPONSE = 8,
    MSG_AUTH_CANCEL = 9, MSG_STREAM_CANCEL = 10, MSG_STREAM_PROGRESS = 13, MSG_AUTH_CONFIRMED = 14
};
enum { AUTH_SUCCESS = 0, AUTH_DENIED = 1, AUTH_IN_PROGRESS = 5 };
enum { STREAM_SUCCESS = 0, STREAM_IN_PROGRESS = 5 };

static const uint8_t MAGIC[8] = { 0xff, 0xff, 0xff, 0xff, 0x21, 0x4c, 0x5f, 0xa0 };

static size_t frame(uint8_t *out, size_t cap, uint64_t client_id, unsigned type, const uint8_t *body, size_t size)
{
    uint8_t header_data[32];
    PbWriter header;
    pb_writer(&header, header_data, sizeof(header_data));
    pb_varint(&header, 1, client_id);
    pb_varint(&header, 2, type);
    const size_t total = 8 + 4 + header.len + 4 + size;
    if (!pb_ok(&header) || total > cap) return 0;
    memcpy(out, MAGIC, 8);
    for (unsigned i = 0; i < 4; ++i) out[8 + i] = (uint8_t)(header.len >> (8 * i));
    memcpy(out + 12, header.data, header.len);
    uint8_t *p = out + 12 + header.len;
    for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(size >> (8 * i));
    if (size) memcpy(p + 4, body, size);
    return total;
}

static bool send_message(int sock, uint32_t ip, uint64_t client_id, unsigned type, const PbWriter *body)
{
    uint8_t packet[1200];
    const size_t n = pb_ok(body) ? frame(packet, sizeof(packet), client_id, type, body->data, body->len) : 0;
    return n && steam_udp_send(sock, ip, STEAM_PORT, packet, n);
}

static uint32_t le32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

typedef struct {
    uint64_t client_id, instance_id;
    unsigned type;
    const uint8_t *body;
    size_t size;
} Message;

static bool parse(const uint8_t *data, size_t size, Message *m)
{
    if (size < 16 || memcmp(data, MAGIC, 8)) return false;
    const uint32_t header_size = le32(data + 8);
    if (header_size > size - 16) return false;
    const uint32_t body_size = le32(data + 12 + header_size);
    if (body_size > size - 16 - header_size) return false;
    memset(m, 0, sizeof(*m));
    PbReader r;
    PbField f;
    pb_reader(&r, data + 12, header_size);
    while (pb_next(&r, &f)) {
        if (f.field == 1) m->client_id = f.value;
        else if (f.field == 2) m->type = (unsigned)f.value;
        else if (f.field == 3) m->instance_id = f.value;
    }
    m->body = data + 16 + header_size;
    m->size = body_size;
    return true;
}

static void device_token(const SteamIdentity *id, const uint8_t secret[32], PbWriter *w, unsigned field)
{
    uint8_t plain[8], token[48];
    for (unsigned i = 0; i < 8; ++i) plain[i] = (uint8_t)(id->device_id >> (8 * i));
    const size_t n = steam_sym_encrypt(secret, 32, plain, sizeof(plain), token, sizeof(token));
    if (n) pb_bytes(w, field, token, n);
    else w->overflow = true;
}

/* ---- Discovery ----------------------------------------------------------------- */

static bool parse_status(const Message *m, uint32_t ip, SteamHost *host)
{
    memset(host, 0, sizeof(*host));
    host->ip = ip;
    host->client_id = m->client_id;
    host->instance_id = m->instance_id;
    PbReader r;
    PbField f;
    pb_reader(&r, m->body, m->size);
    while (pb_next(&r, &f)) {
        switch (f.field) {
        case 4: pb_copy_string(&f, host->name, sizeof(host->name)); break;
        case 7: host->ostype = (int)(int32_t)f.value; break;
        case 9: {
            PbReader u;
            PbField g;
            pb_reader(&u, f.data, f.size);
            while (pb_next(&u, &g))
                if (g.field == 1 && !host->steamid) host->steamid = g.value;
            break;
        }
        case 11: host->universe = (int)f.value; break;
        case 13: host->screen_locked = f.value != 0; break;
        case 14: host->games_running = f.value != 0; break;
        case 22: host->remoteplay_active = f.value != 0; break;
        default: break;
        }
    }
    return host->client_id != 0;
}

int steam_discover(const SteamIdentity *id, uint32_t ip, SteamHost *hosts, int max, unsigned timeout_ms)
{
    const int sock = steam_udp_open();
    if (sock == STEAM_UDP_INVALID) return 0;
    int found = 0;
    uint32_t seq = 0;
    const uint64_t end = steam_now_ms() + timeout_ms;
    uint64_t next_send = 0;
    while (steam_now_ms() < end && found < max) {
        const uint64_t now = steam_now_ms();
        if (now >= next_send) {
            uint8_t body[16];
            PbWriter w;
            pb_writer(&w, body, sizeof(body));
            pb_varint(&w, 1, seq++);
            if (ip) {
                send_message(sock, ip, id->device_id, MSG_DISCOVERY, &w);
            } else {
                /* Both: some routers drop one or the other. */
                send_message(sock, steam_udp_broadcast(), id->device_id, MSG_DISCOVERY, &w);
                send_message(sock, 0xFFFFFFFFu, id->device_id, MSG_DISCOVERY, &w);
            }
            next_send = now + 400;
        }
        steam_udp_wait(sock, 50);
        uint8_t packet[1500];
        uint32_t from;
        int n;
        while ((n = steam_udp_recv(sock, packet, sizeof(packet), &from, NULL)) > 0) {
            Message m;
            SteamHost host;
            if (!parse(packet, (size_t)n, &m) || m.type != MSG_STATUS || !parse_status(&m, from, &host)) continue;
            bool seen = false;
            for (int i = 0; i < found; ++i) seen |= hosts[i].client_id == host.client_id;
            if (!seen && found < max) {
                hosts[found++] = host;
                char text[20];
                steam_ip_format(from, text, sizeof(text));
                diagnostic_log("STEAM", "host \"%s\" at %s games_running=%d locked=%d", host.name, text,
                               host.games_running ? 1 : 0, host.screen_locked ? 1 : 0);
            }
        }
        /* One address asked: its answer is all there is to wait for. */
        if (ip && found) break;
    }
    steam_udp_close(sock);
    return found;
}

/* ---- Pairing ------------------------------------------------------------------- */

bool steam_pair_begin(SteamPairing *p, const SteamIdentity *id, const SteamHost *host, const char *pin)
{
    memset(p, 0, sizeof(*p));
    p->sock = STEAM_UDP_INVALID;
    p->id = *id;
    p->host = *host;
    uint8_t public_key[32];
    if (!steam_x25519_keypair(p->private_key, public_key) || !steam_sha256(pin, strlen(pin), p->pin_hash))
        return false;

    /* The key escrow ticket, for Valve's servers: still part of a request. */
    uint8_t ticket_data[160], encrypted[256];
    PbWriter ticket;
    pb_writer(&ticket, ticket_data, sizeof(ticket_data));
    pb_bytes(&ticket, 1, pin, strlen(pin));
    pb_varint(&ticket, 2, id->device_id);
    pb_bytes(&ticket, 3, id->secret, sizeof(id->secret));
    pb_varint(&ticket, 5, 0);
    pb_string(&ticket, 6, id->name);
    const size_t encrypted_size = pb_ok(&ticket)
        ? steam_rsa_encrypt(ticket.data, ticket.len, encrypted, sizeof(encrypted)) : 0;
    if (!encrypted_size) return false;

    uint8_t auth_key[32];
    for (unsigned i = 0; i < 32; ++i) auth_key[i] = public_key[i] ^ p->pin_hash[i];
    uint32_t request_id;
    steam_random(&request_id, sizeof(request_id));
    PbWriter w;
    pb_writer(&w, p->request, sizeof(p->request));
    device_token(id, id->secret, &w, 1);
    pb_string(&w, 2, id->name);
    pb_bytes(&w, 3, encrypted, encrypted_size);
    pb_bytes(&w, 4, auth_key, sizeof(auth_key));
    pb_varint(&w, 5, request_id);
    if (!pb_ok(&w)) return false;
    p->request_size = w.len;
    p->sock = steam_udp_open();
    if (p->sock == STEAM_UDP_INVALID) return false;
    p->started_at = steam_now_ms();
    diagnostic_log("STEAM", "pairing with \"%s\"", host->name);
    return true;
}

static bool finish_exchange(SteamPairing *p, const PbField *auth_key, const PbField *token)
{
    if (!auth_key->data || auth_key->size != 32 || !token->data) return false;
    uint8_t host_public[32], secret[32], plain[64];
    for (unsigned i = 0; i < 32; ++i) host_public[i] = auth_key->data[i] ^ p->pin_hash[i];
    if (!steam_key_exchange(p->private_key, host_public, secret)) return false;
    /* The host proves it has the same secret: our device id, encrypted. */
    const int n = steam_sym_decrypt(secret, 32, token->data, token->size, plain, sizeof(plain));
    uint64_t device = 0;
    for (int i = 0; n == 8 && i < 8; ++i) device |= (uint64_t)plain[i] << (8 * i);
    if (n != 8 || device != p->id.device_id) return false;
    memcpy(p->secret, secret, sizeof(secret));
    return true;
}

SteamPairState steam_pair_poll(SteamPairing *p)
{
    if (p->sock == STEAM_UDP_INVALID) return STEAM_PAIR_ERROR;
    const uint64_t now = steam_now_ms();
    if (now - p->started_at > 5 * 60 * 1000) return STEAM_PAIR_TIMEOUT;
    if (now >= p->next_send_at) {
        uint8_t packet[1200];
        const size_t n = frame(packet, sizeof(packet), p->id.device_id, MSG_AUTH_REQUEST, p->request, p->request_size);
        if (n) steam_udp_send(p->sock, p->host.ip, STEAM_PORT, packet, n);
        p->next_send_at = now + 1000;
    }
    uint8_t packet[1500];
    int n;
    while ((n = steam_udp_recv(p->sock, packet, sizeof(packet), NULL, NULL)) > 0) {
        Message m;
        if (!parse(packet, (size_t)n, &m) || m.type != MSG_AUTH_RESPONSE) continue;
        int result = -1;
        PbField auth_key = {0}, token = {0}, f;
        PbReader r;
        pb_reader(&r, m.body, m.size);
        while (pb_next(&r, &f)) {
            if (f.field == 1) result = (int)f.value;
            else if (f.field == 2) p->steamid = f.value;
            else if (f.field == 3) auth_key = f;
            else if (f.field == 4) token = f;
        }
        if (auth_key.data) {
            /* The host's half of the exchange, whatever `result` says. */
            const bool ok = finish_exchange(p, &auth_key, &token);
            uint8_t body[8];
            PbWriter w;
            pb_writer(&w, body, sizeof(body));
            pb_varint(&w, 1, ok ? AUTH_SUCCESS : AUTH_DENIED);
            send_message(p->sock, p->host.ip, p->id.device_id, MSG_AUTH_CONFIRMED, &w);
            diagnostic_log("STEAM", "pairing key exchange %s", ok ? "done" : "failed (wrong PIN)");
            return ok ? STEAM_PAIR_DONE : STEAM_PAIR_WRONG_PIN;
        }
        if (result == AUTH_IN_PROGRESS) continue;
        p->result = result;
        diagnostic_log("STEAM", "pairing refused: %s", steam_pair_result_name(result));
        return STEAM_PAIR_REFUSED;
    }
    return STEAM_PAIR_WAITING;
}

void steam_pair_cancel(SteamPairing *p)
{
    if (p->sock == STEAM_UDP_INVALID) return;
    PbWriter w;
    uint8_t body[1];
    pb_writer(&w, body, sizeof(body));
    send_message(p->sock, p->host.ip, p->id.device_id, MSG_AUTH_CANCEL, &w);
    steam_udp_close(p->sock);
    p->sock = STEAM_UDP_INVALID;
    memset(p->private_key, 0, sizeof(p->private_key));
}

const char *steam_pair_result_name(int result)
{
    switch (result) {
    case 0: return "success";
    case 1: return "denied";
    case 2: return "not logged in";
    case 3: return "offline";
    case 4: return "busy";
    case 5: return "in progress";
    case 6: return "timed out";
    case 7: return "failed";
    case 8: return "canceled";
    default: return "no answer";
    }
}

/* ---- Streaming request --------------------------------------------------------- */

bool steam_request_stream(const SteamIdentity *id, const uint8_t host_secret[32], const SteamHost *host,
                          const SteamStreamRequest *req, SteamStreamGrant *grant, unsigned timeout_ms,
                          volatile bool *cancel)
{
    memset(grant, 0, sizeof(*grant));
    grant->result = -1;
    const int sock = steam_udp_open();
    if (sock == STEAM_UDP_INVALID) return false;
    uint32_t request_id;
    steam_random(&request_id, sizeof(request_id));
    request_id &= 0x7fffffff;

    uint8_t body[400];
    PbWriter w;
    pb_writer(&w, body, sizeof(body));
    pb_varint(&w, 1, request_id);
    pb_int(&w, 2, (int)req->width);
    pb_int(&w, 3, (int)req->height);
    pb_int(&w, 4, 2);
    pb_string(&w, 5, "Kasumi");
    pb_bool(&w, 6, req->stream_interface == 3);
    device_token(id, host_secret, &w, 7);
    pb_bool(&w, 9, true);
    pb_bool(&w, 10, true);
    pb_bool(&w, 11, true);
    pb_varint(&w, 13, host->client_id);
    pb_varint(&w, 14, 1);                   /* k_EStreamTransportUDP */
    pb_varint(&w, 16, 1);                   /* k_EStreamDeviceFormFactorPhone: a handheld */
    if (req->gameid) pb_varint(&w, 19, req->gameid);
    pb_int(&w, 20, req->stream_interface);
    if (req->fps) {
        pb_int(&w, 21, (int)req->fps);
        pb_int(&w, 22, 1);
    }
    if (!pb_ok(&w)) {
        steam_udp_close(sock);
        return false;
    }

    bool done = false;
    uint64_t last_heard = steam_now_ms(), next_send = 0;
    while (!done && steam_now_ms() - last_heard < timeout_ms) {
        if (cancel && *cancel) break;
        const uint64_t now = steam_now_ms();
        if (now >= next_send) {
            send_message(sock, host->ip, id->device_id, MSG_STREAM_REQUEST, &w);
            next_send = now + 3000;
        }
        steam_udp_wait(sock, 100);
        uint8_t packet[1500];
        int n;
        while (!done && (n = steam_udp_recv(sock, packet, sizeof(packet), NULL, NULL)) > 0) {
            Message m;
            if (!parse(packet, (size_t)n, &m)) continue;
            PbReader r;
            PbField f;
            pb_reader(&r, m.body, m.size);
            if (m.type == MSG_PROOF_REQUEST) {
                PbField challenge = {0};
                bool has_id = false;
                uint32_t proof_id = 0;
                while (pb_next(&r, &f)) {
                    if (f.field == 1) challenge = f;
                    else if (f.field == 2) { has_id = true; proof_id = (uint32_t)f.value; }
                }
                if (!challenge.data || challenge.size > 256 || (has_id && proof_id != request_id)) continue;
                uint8_t answer[300], reply[400];
                const size_t size = steam_sym_encrypt(host_secret, 32, challenge.data, challenge.size, answer,
                                                      sizeof(answer));
                PbWriter p;
                pb_writer(&p, reply, sizeof(reply));
                pb_bytes(&p, 1, answer, size);
                if (has_id) pb_varint(&p, 2, proof_id);
                if (size) send_message(sock, host->ip, id->device_id, MSG_PROOF_RESPONSE, &p);
                last_heard = steam_now_ms();
            } else if (m.type == MSG_STREAM_PROGRESS) {
                last_heard = steam_now_ms();
            } else if (m.type == MSG_STREAM_RESPONSE) {
                uint32_t answer_id = 0;
                int result = -1;
                PbField key = {0};
                uint16_t port = 0;
                while (pb_next(&r, &f)) {
                    if (f.field == 1) answer_id = (uint32_t)f.value;
                    else if (f.field == 2) result = (int)f.value;
                    else if (f.field == 3) port = (uint16_t)f.value;
                    else if (f.field == 4) key = f;
                }
                if (answer_id != request_id) continue;
                last_heard = steam_now_ms();
                if (result == STREAM_IN_PROGRESS) continue;
                grant->result = result;
                done = true;
                if (result == STREAM_SUCCESS && key.data) {
                    uint8_t plain[64];
                    const int size = steam_sym_decrypt(host_secret, 32, key.data, key.size, plain, sizeof(plain));
                    if (size == 16 || size == 32) {
                        memcpy(grant->session_key, plain, (size_t)size);
                        grant->session_key_size = (size_t)size;
                        grant->port = port;
                    } else {
                        grant->result = -2;
                    }
                }
            }
        }
    }
    if (!done) {
        /* Steam may still be starting the stream: tell it not to. */
        uint8_t cancel_body[8];
        PbWriter c;
        pb_writer(&c, cancel_body, sizeof(cancel_body));
        pb_varint(&c, 1, request_id);
        send_message(sock, host->ip, id->device_id, MSG_STREAM_CANCEL, &c);
    }
    steam_udp_close(sock);
    diagnostic_log("STEAM", "stream request result=%d port=%u key=%u", grant->result, grant->port,
                   (unsigned)grant->session_key_size);
    return grant->result == STREAM_SUCCESS && grant->session_key_size;
}

const char *steam_stream_result_text(int result)
{
    switch (result) {
    case 0: return "Streaming started.";
    case 1: return "This PC no longer knows this 3DS. Press Y in the library (your PCs) and pair it again.";
    case 2: return "The PC's screen is locked. Unlock it, then try again.";
    case 3: return "Steam couldn't start streaming.";
    case 4: return "The PC is busy streaming to another device.";
    case 6: return "Streaming was canceled on the PC.";
    case 7: return "Steam needs its streaming drivers installed on the PC.";
    case 8: return "Remote Play is turned off in Steam on the PC (Settings > Remote Play).";
    case 9: return "The PC is broadcasting. Stop the broadcast first.";
    case 10: return "SteamVR is running on the PC. Close it first.";
    case 11: return "The PC asks for a PIN to stream. Remove it in Steam > Settings > Remote Play.";
    case 12: return "Steam couldn't set up the stream.";
    case 13: return "Steam is set to Invisible on the PC. Go online, then try again.";
    case 14: return "Steam couldn't launch the game on the PC.";
    case -2: return "The PC's answer couldn't be decrypted. Pair this 3DS again.";
    default: return "The PC didn't answer. Is Steam running, on the same Wi-Fi?";
    }
}

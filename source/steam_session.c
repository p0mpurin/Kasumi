#include "steam_session.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "diagnostic.h"
#include "steam_crypto.h"
#include "steam_proto.h"
#include "steam_udp.h"

/* Packet types (header byte 0, bit 7 = has CRC). */
enum {
    PKT_UNCONNECTED = 0, PKT_CONNECT = 1, PKT_CONNECT_ACK = 2, PKT_UNRELIABLE = 3, PKT_UNRELIABLE_FRAG = 4,
    PKT_RELIABLE = 5, PKT_RELIABLE_FRAG = 6, PKT_ACK = 7, PKT_NACK = 8, PKT_DISCONNECT = 9
};
enum { CH_DISCOVERY = 0, CH_CONTROL = 1, CH_STATS = 2 };
/* EStreamControlMessage */
enum {
    CTL_AUTH_REQUEST = 1, CTL_AUTH_RESPONSE = 2, CTL_NEGOTIATION_INIT = 3, CTL_NEGOTIATION_SET_CONFIG = 4,
    CTL_NEGOTIATION_COMPLETE = 5, CTL_CLIENT_HANDSHAKE = 6, CTL_SERVER_HANDSHAKE = 7, CTL_KEEPALIVE = 9,
    CTL_START_AUDIO = 50, CTL_STOP_AUDIO = 51, CTL_START_VIDEO = 52, CTL_STOP_VIDEO = 53,
    CTL_MOUSE_MOTION = 54, CTL_MOUSE_DOWN = 56, CTL_MOUSE_UP = 57, CTL_KEY_DOWN = 58, CTL_KEY_UP = 59,
    CTL_VIDEO_DECODER_INFO = 80, CTL_SET_TITLE = 81, CTL_VIDEO_ENCODER_INFO = 90, CTL_SET_TARGET_BITRATE = 94,
    CTL_SET_ACTIVITY = 98, CTL_REMOTE_HID = 106
};
/* Video data frame flags. */
enum {
    VF_START_SEQUENCE = 0x01, VF_ESCAPE = 0x02, VF_SUBFRAME_ADVANCE = 0x04,
    VF_FRAME_FINISH = 0x08, VF_KEYFRAME = 0x10, VF_ENCRYPTED = 0x20
};

#define HEADER_SIZE 13
#define SLOT_DATA 1500
#define PENDING_MAX 128
/* Packets of the control channel held for reordering: a window icon from a
 * Linux PC (SetIcon, raw RGBA) can take a hundred or more. */
#define CONTROL_WINDOW 256
#define VIDEO_WINDOW 512
#define AUDIO_WINDOW 64
#define FRAME_MAX (512 * 1024)
#define VIDEO_PARTS_MAX 64
#define VIDEO_PART_TIMEOUT_MS 150
#define HID_REPORT_SIZE 48
#define HID_DEVICE 1

typedef struct {
    bool used;
    uint8_t channel;
    uint16_t id;
    uint16_t size;
    uint64_t sent_at;
    unsigned retries;
    uint8_t data[SLOT_DATA];
} Pending;

typedef struct {
    bool used;
    uint8_t type;
    int16_t fragment;
    uint16_t id;
    uint16_t size;
    uint32_t timestamp;
    uint8_t data[SLOT_DATA];
} Slot;

typedef struct {
    uint16_t next;          /* the next packet id to deliver */
    bool delivered_any;
    uint32_t last_timestamp;  /* of the newest delivered packet, for the ACK echo */
    uint64_t last_received_at;
    Slot slots[CONTROL_WINDOW];
} ReliableIn;

typedef struct {
    uint8_t *data;
    size_t size;
    uint16_t frame_id, first, last;
    uint8_t flags;
} VideoPart;

struct SteamSession {
    SteamSessionConfig config;
    SteamSessionCallbacks cb;
    int sock;
    SteamSessionState state;
    char status[128];
    uint8_t cid, host_cid;
    unsigned mtu;
    uint16_t next_id[256];
    uint64_t send_sequence, recv_sequence;
    uint64_t opened_at, last_heard_at, last_keepalive_at, last_connect_at, last_keyframe_request_at;
    Pending pending[PENDING_MAX];
    ReliableIn control_in, stats_in;
    int video_channel, audio_channel;
    Slot video_slots[VIDEO_WINDOW];
    Slot audio_slots[AUDIO_WINDOW];
    uint8_t frame[FRAME_MAX];
    size_t frame_size;
    bool frame_key, waiting_key, have_sequence;
    /* Software encoders can finish their slices in a different order from
     * their position in the picture. UDP sequence alone cannot order them.
     * Steam's seven-byte video header carries the inclusive slice range. */
    VideoPart video_parts[VIDEO_PARTS_MAX];
    unsigned video_part_count, video_part_logs, video_reordered;
    size_t video_part_bytes;
    uint64_t video_part_since;
    uint16_t video_next_slice, video_frame_id;
    bool video_frame_active;
    /* Steam encodes a black placeholder ("Desktop Black Frame") while it
     * has nothing to capture, at another size than the picture: each switch
     * would rebuild the decoder, and a PC that switched every few seconds
     * wore out MVD (report E9BPVX). It is not shown. */
    bool placeholder;
    /* The quit keys were sent: closing waits a moment for the PC to take
     * them. */
    bool stop_sent;
    /* What the PC is showing (SetActivity): 1 Big Picture, 2 a game, 3 the
     * desktop. Quitting only ever closes a game. */
    int activity;
    uint16_t video_sequence;
    /* RemoteHID gamepad */
    bool hid_supported, hid_open, hid_started;
    uint8_t hid_version;
    uint8_t hid_report[HID_REPORT_SIZE], hid_sent[HID_REPORT_SIZE];
    bool hid_sent_valid;
    uint64_t hid_sent_at;
    SteamSessionStats stats;
    uint8_t scratch[FRAME_MAX / 8];
};

static void clear_video_frame(SteamSession *s)
{
    for (unsigned i = 0; i < s->video_part_count; ++i) free(s->video_parts[i].data);
    s->video_part_count = 0;
    s->video_part_bytes = 0;
    s->video_part_since = 0;
    s->video_next_slice = 0;
    s->video_frame_active = false;
    s->frame_size = 0;
    s->frame_key = false;
}

static uint32_t stream_time(void)
{
    const uint64_t ms = steam_now_ms();
    return (uint32_t)(ms * 65536u / 1000u);
}

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(v >> (8 * i)); }
static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t get32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

/* Builds header + body + CRC into out; returns the size. */
static size_t build(SteamSession *s, uint8_t *out, unsigned type, unsigned channel, uint16_t id, int16_t fragment,
                    const void *body, size_t size, bool ids)
{
    if (HEADER_SIZE + size + 4 > SLOT_DATA) return 0;
    out[0] = (uint8_t)(0x80 | type);
    out[1] = 0;
    out[2] = ids ? s->cid : 0;
    out[3] = ids ? s->host_cid : 0;
    out[4] = (uint8_t)channel;
    put16(out + 5, (uint16_t)fragment);
    put16(out + 7, id);
    put32(out + 9, stream_time());
    if (size) memcpy(out + HEADER_SIZE, body, size);
    put32(out + HEADER_SIZE + size, steam_crc32c(out, HEADER_SIZE + size));
    return HEADER_SIZE + size + 4;
}

static void send_raw(SteamSession *s, const uint8_t *data, size_t size)
{
    if (size) steam_udp_send(s->sock, s->config.host_ip, s->config.port, data, size);
}

static void send_unreliable(SteamSession *s, unsigned channel, const void *body, size_t size)
{
    uint8_t packet[SLOT_DATA];
    send_raw(s, packet, build(s, packet, PKT_UNRELIABLE, channel, s->next_id[channel]++, 0, body, size, true));
}

static bool send_reliable(SteamSession *s, unsigned channel, const uint8_t *body, size_t size)
{
    const size_t limit = (s->mtu ? s->mtu : 1200) - HEADER_SIZE - 4;
    const size_t parts = size ? (size + limit - 1) / limit : 1;
    unsigned free_slots = 0;
    for (unsigned i = 0; i < PENDING_MAX; ++i) free_slots += !s->pending[i].used;
    if (parts > free_slots || parts > 64) return false;
    for (size_t part = 0; part < parts; ++part) {
        Pending *p = NULL;
        for (unsigned i = 0; i < PENDING_MAX && !p; ++i)
            if (!s->pending[i].used) p = &s->pending[i];
        const size_t offset = part * limit;
        const size_t chunk = size - offset < limit ? size - offset : limit;
        /* The first packet says how many fragments follow; they take the next ids. */
        const unsigned type = part ? PKT_RELIABLE_FRAG : PKT_RELIABLE;
        const int16_t fragment = part ? (int16_t)(part - 1) : (int16_t)(parts - 1);
        p->id = s->next_id[channel]++;
        p->channel = (uint8_t)channel;
        p->size = (uint16_t)build(s, p->data, type, channel, p->id, fragment, body + offset, chunk, true);
        p->used = p->size != 0;
        p->sent_at = steam_now_ms();
        p->retries = 0;
        send_raw(s, p->data, p->size);
    }
    return true;
}

static bool encrypted(unsigned type)
{
    return type != CTL_AUTH_REQUEST && type != CTL_AUTH_RESPONSE && type != CTL_CLIENT_HANDSHAKE &&
           type != CTL_SERVER_HANDSHAKE;
}

static bool send_control(SteamSession *s, unsigned type, const PbWriter *message)
{
    if (!pb_ok(message)) return false;
    uint8_t *body = s->scratch;
    body[0] = (uint8_t)type;
    size_t size;
    if (encrypted(type)) {
        size = steam_frame_encrypt(s->config.key, s->config.key_size, s->send_sequence, message->data,
                                   message->len, body + 1, sizeof(s->scratch) - 1);
        if (!size) return false;
        s->send_sequence++;
    } else {
        if (message->len + 1 > sizeof(s->scratch)) return false;
        memcpy(body + 1, message->data, message->len);
        size = message->len;
    }
    return send_reliable(s, CH_CONTROL, body, size + 1);
}

static void set_status(SteamSession *s, const char *text)
{
    snprintf(s->status, sizeof(s->status), "%s", text);
}

static void fail(SteamSession *s, const char *why)
{
    if (s->state == STEAM_SESSION_FAILED || s->state == STEAM_SESSION_CLOSED) return;
    s->state = STEAM_SESSION_FAILED;
    set_status(s, why);
    diagnostic_log("STEAM", "session failed: %s", why);
}

/* ---- Session setup ----------------------------------------------------------- */

SteamSession *steam_session_open(const SteamSessionConfig *config, const SteamSessionCallbacks *callbacks)
{
    SteamSession *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->config = *config;
    if (callbacks) s->cb = *callbacks;
    s->sock = steam_udp_open();
    if (s->sock == STEAM_UDP_INVALID) {
        free(s);
        return NULL;
    }
    uint8_t cid = 0;
    while (!cid) steam_random(&cid, 1);
    s->cid = cid;
    s->mtu = 1200;
    s->video_channel = s->audio_channel = -1;
    s->waiting_key = true;
    s->opened_at = s->last_heard_at = steam_now_ms();
    s->state = STEAM_SESSION_CONNECTING;
    set_status(s, "Connecting to the PC...");
    char ip[20];
    steam_ip_format(config->host_ip, ip, sizeof(ip));
    diagnostic_log("STEAM", "session to %s:%u %ux%u@%u %u kbps", ip, config->port, config->width, config->height,
                   config->fps, config->kbps);
    return s;
}

int steam_session_socket(const SteamSession *s) { return s ? s->sock : -1; }
SteamSessionState steam_session_state(const SteamSession *s) { return s->state; }
const char *steam_session_status(const SteamSession *s) { return s->status; }

void steam_session_stats(const SteamSession *s, SteamSessionStats *out)
{
    *out = s->stats;
    out->input_ready = s->hid_started;
    out->capture_unavailable = s->placeholder;
}

static void send_connect(SteamSession *s)
{
    uint8_t body[4], packet[64];
    put32(body, steam_crc32c("Connect", 7));
    send_raw(s, packet, build(s, packet, PKT_CONNECT, CH_DISCOVERY, 0, 0, body, sizeof(body), true));
    s->last_connect_at = steam_now_ms();
}

static void send_client_handshake(SteamSession *s)
{
    uint8_t data[8], info_data[4];
    PbWriter m, info;
    pb_writer(&info, info_data, sizeof(info_data));
    pb_writer(&m, data, sizeof(data));
    pb_message(&m, 1, &info);
    send_control(s, CTL_CLIENT_HANDSHAKE, &m);
}

static void on_server_handshake(SteamSession *s, const uint8_t *data, size_t size)
{
    if (s->state != STEAM_SESSION_HANDSHAKE) return;
    PbReader r, info;
    PbField f, g;
    pb_reader(&r, data, size);
    while (pb_next(&r, &f)) {
        if (f.field != 1) continue;
        pb_reader(&info, f.data, f.size);
        while (pb_next(&info, &g))
            if (g.field == 1 && g.value >= 576 && g.value <= SLOT_DATA) s->mtu = (unsigned)g.value;
    }
    static const char text[] = "Steam In-Home Streaming";
    uint8_t token[32], message[64];
    if (!steam_hmac_sha256(s->config.key, s->config.key_size, text, sizeof(text) - 1, token)) {
        fail(s, "Couldn't compute the session token");
        return;
    }
    PbWriter m;
    pb_writer(&m, message, sizeof(message));
    pb_bytes(&m, 1, token, sizeof(token));
    pb_varint(&m, 2, 1);                 /* k_EStreamVersionCurrent */
    pb_varint(&m, 3, s->config.steamid);
    send_control(s, CTL_AUTH_REQUEST, &m);
    s->state = STEAM_SESSION_NEGOTIATING;
    set_status(s, "Signing in to the stream...");
    diagnostic_log("STEAM", "server handshake mtu=%u", s->mtu);
}

static void on_auth_response(SteamSession *s, const uint8_t *data, size_t size)
{
    PbReader r;
    PbField f;
    int result = 0;
    pb_reader(&r, data, size);
    while (pb_next(&r, &f))
        if (f.field == 1) result = (int)f.value;
    diagnostic_log("STEAM", "authentication %s", result ? "refused" : "accepted");
    if (result) fail(s, "The PC refused the stream's sign-in. Pair this 3DS again.");
}

static bool list_has(const PbField *f, unsigned value)
{
    if (f->type == PB_VARINT) return f->value == value;
    /* Packed repeated enum (each value fits a byte). */
    for (const uint8_t *p = f->data; p < f->data + f->size; ++p)
        if (*p == value) return true;
    return false;
}

static void on_negotiation_init(SteamSession *s, const uint8_t *data, size_t size)
{
    bool opus = false, h264 = false;
    PbReader r;
    PbField f;
    pb_reader(&r, data, size);
    while (pb_next(&r, &f)) {
        if (f.field == 2 && list_has(&f, 3)) opus = true;
        else if (f.field == 3 && list_has(&f, 4)) h264 = true;
        else if (f.field == 4) s->hid_supported = f.value != 0;
    }
    diagnostic_log("STEAM", "negotiation: opus=%d h264=%d remote_hid=%d", opus, h264, s->hid_supported);
    if (!h264) {
        fail(s, "The PC can't stream H.264 video.");
        return;
    }
    const SteamSessionConfig *c = &s->config;
    uint8_t config_data[32], client_data[256], limit_data[64], mode_data[32], caps_data[600], message[1024];
    PbWriter config, client, limit, mode, caps, m;
    pb_writer(&config, config_data, sizeof(config_data));
    pb_bool(&config, 1, false);          /* reliable_data: our media path is unreliable */
    if (opus) pb_varint(&config, 2, 3);
    pb_varint(&config, 3, 4);            /* H.264 */
    if (s->hid_supported) pb_bool(&config, 5, true);

    pb_writer(&mode, mode_data, sizeof(mode_data));
    pb_varint(&mode, 1, c->width);
    pb_varint(&mode, 2, c->height);
    pb_varint(&mode, 4, c->fps);
    pb_varint(&mode, 5, 1);
    pb_writer(&limit, limit_data, sizeof(limit_data));
    pb_varint(&limit, 1, 4);
    pb_message(&limit, 2, &mode);
    pb_int(&limit, 3, (int)c->kbps);
    pb_int(&limit, 4, (int)c->kbps * 2);

    pb_writer(&client, client_data, sizeof(client_data));
    pb_int(&client, 1, 2);               /* k_EStreamQualityBalanced */
    pb_varint(&client, 2, c->width);
    pb_varint(&client, 3, c->height);
    pb_varint(&client, 4, c->fps);
    pb_varint(&client, 5, 1);
    pb_int(&client, 6, (int)c->kbps);
    pb_bool(&client, 7, true);
    pb_bool(&client, 9, true);
    pb_bool(&client, 10, opus);
    pb_bool(&client, 11, true);
    pb_int(&client, 12, 2);
    pb_bool(&client, 13, false);
    pb_bool(&client, 14, false);
    pb_string(&client, 16, "auto");
    pb_message(&client, 24, &limit);

    char info[512];
    snprintf(info, sizeof(info),
             "\"SystemInfo\"\n{\n\t\"OSType\"\t\t\"-300\"\n\t\"CPUID\"\t\t\"ARM\"\n\t\"CPUGhz\"\t\t\"0.804000\"\n"
             "\t\"PhysicalCPUCount\"\t\"4\"\n\t\"LogicalCPUCount\"\t\"4\"\n\t\"SystemRAM\"\t\t\"256\"\n"
             "\t\"VideoVendorID\"\t\"0\"\n\t\"VideoDeviceID\"\t\"0\"\n\t\"VideoRevision\"\t\"0\"\n"
             "\t\"VideoRAM\"\t\t\"0\"\n\t\"VideoDisplayX\"\t\"%u\"\n\t\"VideoDisplayY\"\t\"%u\"\n"
             "\t\"VideoDisplayNameID\"\t\"Kasumi\"\n}\n", c->width, c->height);
    pb_writer(&caps, caps_data, sizeof(caps_data));
    pb_string(&caps, 1, info);
    pb_bool(&caps, 2, true);
    pb_int(&caps, 3, (int)c->kbps);
    pb_int(&caps, 4, (int)c->kbps * 2);
    /* The PC draws the mouse pointer into the picture: Steam otherwise sends
     * it apart (SetCursorImage) for the client to draw, and on the desktop
     * Kasumi showed no pointer at all. */
    pb_bool(&caps, 7, true);             /* disable_client_cursor */
    pb_int(&caps, 11, 1);                /* phone-sized: a handheld */
    if (opus) pb_varint(&caps, 14, 3);
    pb_varint(&caps, 15, 4);

    pb_writer(&m, message, sizeof(message));
    pb_message(&m, 1, &config);
    pb_message(&m, 2, &client);
    pb_message(&m, 3, &caps);
    if (!send_control(s, CTL_NEGOTIATION_SET_CONFIG, &m)) fail(s, "Couldn't send the stream settings");
}

static void send_keepalive(SteamSession *s)
{
    PbWriter m;
    uint8_t data[1];
    pb_writer(&m, data, sizeof(data));
    send_control(s, CTL_KEEPALIVE, &m);
    s->last_keepalive_at = steam_now_ms();
}

static void hid_announce(SteamSession *s);

static void on_negotiation_set_config(SteamSession *s)
{
    if (s->state == STEAM_SESSION_STREAMING) return;
    PbWriter m;
    uint8_t data[1];
    pb_writer(&m, data, sizeof(data));
    send_control(s, CTL_NEGOTIATION_COMPLETE, &m);
    s->state = STEAM_SESSION_STREAMING;
    set_status(s, "Streaming from your PC");
    diagnostic_log("STEAM", "negotiation complete after %llu ms",
                   (unsigned long long)(steam_now_ms() - s->opened_at));
    send_keepalive(s);
    if (s->hid_supported) hid_announce(s);
}

static void on_start_data(SteamSession *s, unsigned type, const uint8_t *data, size_t size)
{
    PbReader r;
    PbField f;
    unsigned channel = 0, codec = 0, width = 0, height = 0, frequency = 0, channels = 0;
    pb_reader(&r, data, size);
    while (pb_next(&r, &f)) {
        if (type == CTL_START_VIDEO) {
            if (f.field == 1) channel = (unsigned)f.value;
            else if (f.field == 2) codec = (unsigned)f.value;
            else if (f.field == 4) width = (unsigned)f.value;
            else if (f.field == 5) height = (unsigned)f.value;
        } else {
            if (f.field == 2) channel = (unsigned)f.value;
            else if (f.field == 3) codec = (unsigned)f.value;
            else if (f.field == 5) frequency = (unsigned)f.value;
            else if (f.field == 6) channels = (unsigned)f.value;
        }
    }
    if (channel < 3 || channel > 255) return;
    if (type == CTL_START_VIDEO) {
        s->video_channel = (int)channel;
        s->waiting_key = true;
        s->have_sequence = false;
        clear_video_frame(s);
        memset(s->video_slots, 0, sizeof(s->video_slots));
        s->stats.video_width = width;
        s->stats.video_height = height;
        diagnostic_log("STEAM", "video channel=%u codec=%u %ux%u", channel, codec, width, height);
        if (s->cb.video_start) s->cb.video_start(s->cb.user, width, height);
        uint8_t message[48];
        PbWriter m;
        pb_writer(&m, message, sizeof(message));
        pb_string(&m, 1, "Kasumi New 3DS MVD");
        pb_int(&m, 2, 1);
        send_control(s, CTL_VIDEO_DECODER_INFO, &m);
    } else {
        s->audio_channel = (int)channel;
        memset(s->audio_slots, 0, sizeof(s->audio_slots));
        diagnostic_log("STEAM", "audio channel=%u codec=%u %u Hz x%u", channel, codec, frequency, channels);
    }
}

static void on_activity(SteamSession *s, const uint8_t *data, size_t size)
{
    PbReader r;
    PbField f;
    int activity = 0;
    uint64_t gameid = 0;
    char name[96] = "";
    pb_reader(&r, data, size);
    while (pb_next(&r, &f)) {
        if (f.field == 1) activity = (int)f.value;
        else if (f.field == 2 && !gameid) gameid = f.value;
        else if (f.field == 3) gameid = f.value;
        else if (f.field == 4) pb_copy_string(&f, name, sizeof(name));
    }
    diagnostic_log("STEAM", "activity=%d gameid=%llu name=%s", activity, (unsigned long long)gameid, name);
    s->activity = activity;
    if (s->cb.activity) s->cb.activity(s->cb.user, activity, gameid, name);
}

/* ---- RemoteHID: the virtual gamepad ------------------------------------------ */

static void hid_send(SteamSession *s, const PbWriter *message, bool active)
{
    uint8_t data[400];
    PbWriter m;
    pb_writer(&m, data, sizeof(data));
    pb_bytes(&m, 1, message->data, message->len);
    if (active) pb_bool(&m, 2, true);
    if (pb_ok(message)) send_control(s, CTL_REMOTE_HID, &m);
}

static void hid_announce(SteamSession *s)
{
    uint8_t info_data[160], list_data[192], message[200];
    PbWriter info, list, m;
    pb_writer(&info, info_data, sizeof(info_data));
    pb_varint(&info, 1, 0);              /* k_EDeviceLocationLocal */
    pb_string(&info, 2, "kasumi://gamepad/0");
    pb_varint(&info, 3, 0x057e);
    pb_varint(&info, 4, 0x2009);
    pb_string(&info, 5, "KASUMI3DS");
    pb_varint(&info, 6, 0x100);
    pb_string(&info, 8, "Kasumi 3DS");
    pb_varint(&info, 9, 1);              /* generic desktop */
    pb_varint(&info, 10, 5);             /* game pad */
    pb_int(&info, 12, -203);             /* Linux: a generic SDL gamepad */
    pb_bool(&info, 13, true);
    /* ABXY, D-pad, sticks and their buttons, shoulders, triggers, Back,
     * Start, Guide, and the flags SDL gamepads report. */
    pb_varint(&info, 15, 0x8343FF);
    pb_writer(&list, list_data, sizeof(list_data));
    pb_message(&list, 1, &info);
    pb_writer(&m, message, sizeof(message));
    pb_message(&m, 1, &list);
    hid_send(s, &m, false);
    diagnostic_log("STEAM", "gamepad announced");
}

static void hid_respond(SteamSession *s, uint32_t request_id, int result, const void *data, size_t size)
{
    uint8_t response_data[96], message[112];
    PbWriter response, m;
    pb_writer(&response, response_data, sizeof(response_data));
    pb_varint(&response, 1, request_id);
    pb_int(&response, 2, result);
    if (data) pb_bytes(&response, 3, data, size);
    pb_writer(&m, message, sizeof(message));
    pb_message(&m, 2, &response);
    hid_send(s, &m, false);
}

static void hid_send_report(SteamSession *s, bool force)
{
    if (!s->hid_started) return;
    s->hid_report[27] = s->hid_version;
    if (!force && s->hid_sent_valid && !memcmp(s->hid_report, s->hid_sent, HID_REPORT_SIZE)) return;
    uint8_t item_data[64], device_data[80], reports_data[96], message[112];
    PbWriter item, device, reports, m;
    pb_writer(&item, item_data, sizeof(item_data));
    pb_bytes(&item, 1, s->hid_report, HID_REPORT_SIZE);
    pb_writer(&device, device_data, sizeof(device_data));
    pb_varint(&device, 1, HID_DEVICE);
    pb_message(&device, 2, &item);
    pb_writer(&reports, reports_data, sizeof(reports_data));
    pb_message(&reports, 1, &device);
    pb_writer(&m, message, sizeof(message));
    pb_message(&m, 3, &reports);
    bool active = false;
    for (unsigned i = 0; i < 18; ++i) active |= s->hid_report[i] != 0;
    hid_send(s, &m, active);
    memcpy(s->hid_sent, s->hid_report, HID_REPORT_SIZE);
    s->hid_sent_valid = true;
    s->hid_sent_at = steam_now_ms();
    s->stats.hid_reports++;
}

static void on_remote_hid(SteamSession *s, const uint8_t *data, size_t size)
{
    PbReader r, c;
    PbField f, g, command = {0};
    const uint8_t *inner = NULL;
    size_t inner_size = 0;
    pb_reader(&r, data, size);
    while (pb_next(&r, &f))
        if (f.field == 1) {
            inner = f.data;
            inner_size = f.size;
        }
    if (!inner) return;
    uint32_t request_id = 0;
    pb_reader(&r, inner, inner_size);
    while (pb_next(&r, &f)) {
        if (f.field == 1) request_id = (uint32_t)f.value;
        else if (f.field >= 2 && f.field <= 13) command = f;
    }
    if (!command.field) return;
    /* Fields of the command: device, then data or a number. */
    uint64_t number = 0;
    PbField payload = {0};
    pb_reader(&c, command.data, command.size);
    while (pb_next(&c, &g)) {
        if (g.field == 2 && g.type == PB_BYTES) payload = g;
        else if (g.field == 2 || g.field == 3) number = g.value;
    }
    switch (command.field) {
    case 2: /* device open */
        s->hid_open = true;
        hid_respond(s, request_id, HID_DEVICE, NULL, 0);
        diagnostic_log("STEAM", "gamepad opened by the PC");
        break;
    case 3: /* device close */
    case 13: /* disconnect */
        s->hid_open = s->hid_started = false;
        break;
    case 4: /* write: the PC configures the device */
        if (payload.data && payload.size >= 2 && payload.data[0] == 0x09) s->hid_version = payload.data[1];
        break;
    case 5: { /* read */
        uint8_t zeros[64] = {0};
        hid_respond(s, request_id, 0, zeros, number < sizeof(zeros) ? (size_t)number : sizeof(zeros));
        break;
    }
    case 7: { /* get feature report */
        const uint8_t report = payload.data && payload.size ? payload.data[0] : 0;
        uint8_t out[24] = { report };
        size_t out_size = 2;
        if (report == 0x04) {
            /* Capabilities: valid, not XInput, an unknown non-Steam
             * controller (30), player 0, not HIDAPI. */
            out[1] = 1;
            put32(out + 3, 30);
            out_size = 21;
        } else if (report == 0x02) {
            out[1] = 4; /* SDL_JOYSTICK_POWER_WIRED */
        } else if (report == 0x07) {
            memcpy(out + 1, "KASUMI3DS", 10);
            out_size = 11;
        }
        hid_respond(s, request_id, 0, out, out_size);
        break;
    }
    case 8: hid_respond(s, request_id, 0, "Nintendo", 9); break;
    case 9: hid_respond(s, request_id, 0, "Kasumi 3DS", 11); break;
    case 10: hid_respond(s, request_id, 0, "KASUMI3DS", 10); break;
    case 11: /* start input reports */
        s->hid_started = true;
        diagnostic_log("STEAM", "gamepad reports started (length %u)", (unsigned)number);
        hid_send_report(s, true);
        break;
    case 12: /* request full report */
        hid_send_report(s, true);
        break;
    default:
        break;
    }
}

void steam_session_set_pad(SteamSession *s, const SteamPad *pad)
{
    if (!s) return;
    for (unsigned i = 0; i < 6; ++i) put16(s->hid_report + 2 * i, (uint16_t)pad->axes[i]);
    put16(s->hid_report + 16, pad->buttons);
    if (s->state != STEAM_SESSION_STREAMING) return;
    /* A new state at most every 8 ms (the official client polls at 125 Hz). */
    if (steam_now_ms() - s->hid_sent_at >= 8) hid_send_report(s, false);
}

/* ---- Control channel --------------------------------------------------------- */

static void on_control(SteamSession *s, const uint8_t *message, size_t size)
{
    if (!size) return;
    const unsigned type = message[0];
    const uint8_t *data = message + 1;
    size_t data_size = size - 1;
    uint8_t *heap = NULL;
    if (encrypted(type)) {
        uint64_t sequence;
        const uint8_t *plain;
        /* The receive counter moves on before decrypting, as Steam's does. */
        const uint64_t expected = s->recv_sequence++;
        uint8_t *out = s->scratch;
        size_t cap = sizeof(s->scratch);
        if (data_size > cap) {
            heap = malloc(data_size);
            out = heap;
            cap = data_size;
        }
        const int n = out ? steam_frame_decrypt(s->config.key, s->config.key_size, data, data_size, out, cap,
                                                &sequence, &plain) : -1;
        if (n < 0) {
            diagnostic_log("STEAM", "control message %u failed to decrypt", type);
            free(heap);
            return;
        }
        if (sequence != expected) {
            diagnostic_log("STEAM", "control sequence %llu, expected %llu", (unsigned long long)sequence,
                           (unsigned long long)expected);
            s->recv_sequence = sequence + 1;
        }
        data = plain;
        data_size = (size_t)n;
    }
    switch (type) {
    case CTL_SERVER_HANDSHAKE: on_server_handshake(s, data, data_size); break;
    case CTL_AUTH_RESPONSE: on_auth_response(s, data, data_size); break;
    case CTL_NEGOTIATION_INIT: on_negotiation_init(s, data, data_size); break;
    case CTL_NEGOTIATION_SET_CONFIG: on_negotiation_set_config(s); break;
    case CTL_START_AUDIO:
    case CTL_START_VIDEO: on_start_data(s, type, data, data_size); break;
    case CTL_STOP_VIDEO:
        diagnostic_log("STEAM", "video stopped by the PC");
        s->video_channel = -1;
        clear_video_frame(s);
        break;
    case CTL_STOP_AUDIO: s->audio_channel = -1; break;
    case CTL_SET_ACTIVITY: on_activity(s, data, data_size); break;
    case CTL_REMOTE_HID: on_remote_hid(s, data, data_size); break;
    case CTL_SET_TITLE:
    case CTL_VIDEO_ENCODER_INFO: {
        PbReader r;
        PbField f;
        char text[96] = "";
        pb_reader(&r, data, data_size);
        while (pb_next(&r, &f))
            if (f.field == 1 && f.type == PB_BYTES) pb_copy_string(&f, text, sizeof(text));
        diagnostic_log("STEAM", "%s %s", type == CTL_SET_TITLE ? "title" : "encoder", text);
        if (type == CTL_VIDEO_ENCODER_INFO) {
            const bool placeholder = strstr(text, "Black Frame") != NULL;
            /* Back to the picture: start again at its keyframe. */
            if (s->placeholder && !placeholder) {
                s->waiting_key = true;
                clear_video_frame(s);
                steam_session_request_keyframe(s);
            }
            if (!s->placeholder && placeholder) clear_video_frame(s);
            s->placeholder = placeholder;
            if (s->state == STEAM_SESSION_STREAMING)
                set_status(s, placeholder ? "Steam can't capture the PC screen. Check screen-sharing permissions."
                                          : "Streaming from your PC");
        }
        break;
    }
    case CTL_SET_TARGET_BITRATE: {
        PbReader r;
        PbField f;
        pb_reader(&r, data, data_size);
        while (pb_next(&r, &f))
            if (f.field == 1) diagnostic_log("STEAM", "target bitrate %lld kbps", (long long)(int32_t)f.value);
        break;
    }
    default: break;
    }
    free(heap);
}

/* Reliable channels: deliver in order, acknowledge the last packet
 * delivered in order (never one beyond a gap: the PC resends the gap). */
static void on_reliable(SteamSession *s, unsigned channel, const uint8_t *h, const uint8_t *body, size_t size)
{
    ReliableIn *in = channel == CH_CONTROL ? &s->control_in : &s->stats_in;
    const unsigned type = h[0] & 0x7f;
    const int16_t fragment = (int16_t)get16(h + 5);
    const uint16_t id = get16(h + 7);
    const uint16_t ahead = (uint16_t)(id - in->next);
    if (ahead < CONTROL_WINDOW && size <= SLOT_DATA) {
        Slot *slot = &in->slots[id % CONTROL_WINDOW];
        if (!slot->used || slot->id != id) {
            slot->used = true;
            slot->type = (uint8_t)type;
            slot->fragment = fragment;
            slot->id = id;
            slot->size = (uint16_t)size;
            slot->timestamp = get32(h + 9);
            memcpy(slot->data, body, size);
        }
    }
    for (;;) {
        Slot *head = &in->slots[in->next % CONTROL_WINDOW];
        if (!head->used || head->id != in->next) break;
        if (head->type != PKT_RELIABLE || head->fragment < 0 || head->fragment >= CONTROL_WINDOW) {
            head->used = false;   /* a stray fragment */
            in->next++;
            continue;
        }
        const unsigned count = (unsigned)head->fragment + 1;
        size_t total = 0;
        bool complete = true;
        for (unsigned i = 0; i < count && complete; ++i) {
            const Slot *part = &in->slots[(uint16_t)(in->next + i) % CONTROL_WINDOW];
            complete = part->used && part->id == (uint16_t)(in->next + i);
            total += part->size;
        }
        if (!complete) break;
        in->last_timestamp = in->slots[(uint16_t)(in->next + count - 1) % CONTROL_WINDOW].timestamp;
        in->last_received_at = steam_now_ms();
        in->delivered_any = true;
        /* The message leaves the window before it is handled: handling it
         * sends packets, and decrypting uses the scratch buffer. */
        uint8_t single[SLOT_DATA];
        uint8_t *message = count == 1 ? single : malloc(total);
        size_t at = 0;
        for (unsigned i = 0; i < count; ++i) {
            Slot *part = &in->slots[(uint16_t)(in->next + i) % CONTROL_WINDOW];
            if (message) memcpy(message + at, part->data, part->size);
            at += part->size;
            part->used = false;
        }
        in->next = (uint16_t)(in->next + count);
        if (message && channel == CH_CONTROL) on_control(s, message, total);
        if (message != single) free(message);
    }
    if (in->delivered_any) {
        /* The echo lets the PC time the round trip: its packet's timestamp
         * plus the time we held it. */
        uint8_t body_out[4], packet[64];
        const uint32_t held = (uint32_t)((steam_now_ms() - in->last_received_at) * 65536u / 1000u);
        put32(body_out, in->last_timestamp + held);
        send_raw(s, packet, build(s, packet, PKT_ACK, channel, (uint16_t)(in->next - 1), 0, body_out, 4, true));
    }
}

static void on_ack(SteamSession *s, unsigned channel, uint16_t id, const uint8_t *body, size_t size)
{
    (void)body;
    (void)size;
    /* Everything up to and including `id` arrived. The round trip is timed
     * on packets that went out once (a resend's ACK could be either's). */
    const uint64_t now = steam_now_ms();
    for (unsigned i = 0; i < PENDING_MAX; ++i) {
        Pending *p = &s->pending[i];
        if (!p->used || p->channel != channel || (uint16_t)(id - p->id) >= 0x8000) continue;
        if (p->id == id && !p->retries) {
            const int sample = (int)(now - p->sent_at);
            s->stats.rtt_ms = s->stats.rtt_ms ? (s->stats.rtt_ms * 7 + sample) / 8 : sample;
        }
        p->used = false;
    }
}

/* ---- Media channels ---------------------------------------------------------- */

static bool video_part_ready(const SteamSession *s, const VideoPart *part)
{
    if (s->video_frame_active && part->frame_id != s->video_frame_id) return false;
    return !part->last || part->first == s->video_next_slice;
}

static bool append_video_part(SteamSession *s, const VideoPart *part)
{
    const uint8_t flags = part->flags;
    const uint8_t *data = part->data;
    const size_t size = part->size;
    s->video_frame_active = true;
    s->video_frame_id = part->frame_id;
    /* Escaping is per Steam data fragment, after decryption, as in ihslib's
     * frame_h264.c. The start sequence is not part of the bytes to escape. */
    if (flags & VF_ESCAPE) {
        if (size > FRAME_MAX || s->frame_size + 4 + size * 3 / 2 + 2 > FRAME_MAX) return false;
        uint8_t *out = s->frame + s->frame_size;
        size_t n = 0;
        if (flags & VF_START_SEQUENCE) {
            out[0] = out[1] = out[2] = 0;
            out[3] = 1;
            n = 4;
        }
        const size_t start = n;
        for (size_t i = 0; i < size; ++i) {
            if (i >= 2 && data[i] <= 3 && n - start >= 2 && out[n - 1] == 0 && out[n - 2] == 0) out[n++] = 3;
            out[n++] = data[i];
        }
        s->frame_size += n;
    } else {
        if (size > FRAME_MAX - s->frame_size) return false;
        memcpy(s->frame + s->frame_size, data, size);
        s->frame_size += size;
    }
    if (part->last && (flags & VF_SUBFRAME_ADVANCE)) s->video_next_slice = (uint16_t)(part->last + 1u);
    if (flags & VF_KEYFRAME) s->frame_key = true;
    if (flags & VF_FRAME_FINISH) {
        s->stats.video_frames++;
        s->stats.video_bytes += s->frame_size;
        if (s->frame_key) s->stats.video_keyframes++;
        if (s->cb.video) s->cb.video(s->cb.user, s->frame, s->frame_size, s->frame_key);
        s->frame_size = 0;
        s->frame_key = false;
        s->video_frame_active = false;
        s->video_next_slice = 0;
    }
    return true;
}

static void video_frame(SteamSession *s, uint16_t frame_id, const uint8_t *data, size_t size)
{
    if (size < 7) return;
    const uint16_t sequence = get16(data);
    const uint8_t flags = data[2];
    const uint16_t first = get16(data + 3), last = get16(data + 5);
    if (s->video_part_logs++ < 16)
        diagnostic_log("STEAM", "video part frame=%u seq=%u flags=%02x range=%u..%u bytes=%lu",
                       frame_id, sequence, flags, first, last, (unsigned long)(size - 7));
    data += 7;
    size -= 7;
    if (flags & VF_KEYFRAME) {
        if (s->waiting_key) diagnostic_log("STEAM", "keyframe");
        s->waiting_key = false;
        clear_video_frame(s);
        s->frame_key = true;
    } else if (s->have_sequence && sequence != (uint16_t)(s->video_sequence + 1)) {
        /* A piece of a frame never came: the picture would smear until the
         * next keyframe, so drop to one now. */
        s->stats.video_lost++;
        if (!s->waiting_key) steam_session_request_keyframe(s);
        s->waiting_key = true;
        clear_video_frame(s);
    }
    s->have_sequence = true;
    s->video_sequence = sequence;
    if (s->waiting_key || s->placeholder) return;
    if (flags & VF_ENCRYPTED) {
        static const uint8_t zero_iv[16];
        const int n = steam_cbc_decrypt(s->config.key, s->config.key_size, zero_iv, data, size, s->scratch,
                                        sizeof(s->scratch));
        if (n < 0) {
            s->waiting_key = true;
            clear_video_frame(s);
            steam_session_request_keyframe(s);
            return;
        }
        data = s->scratch;
        size = (size_t)n;
    }
    if (last && first > last) goto overflow;
    VideoPart part = { (uint8_t *)data, size, frame_id, first, last, flags };
    if (!s->video_part_count && video_part_ready(s, &part)) {
        if (!append_video_part(s, &part)) goto overflow;
    } else {
        if (s->video_part_count == VIDEO_PARTS_MAX || size > FRAME_MAX - s->video_part_bytes ||
            s->frame_size > FRAME_MAX - s->video_part_bytes - size) goto overflow;
        part.data = malloc(size ? size : 1);
        if (!part.data) goto overflow;
        memcpy(part.data, data, size);
        unsigned at = s->video_part_count;
        for (unsigned i = 0; i < s->video_part_count; ++i) {
            if (s->video_parts[i].frame_id == frame_id && last < s->video_parts[i].first) {
                at = i;
                break;
            }
        }
        memmove(s->video_parts + at + 1, s->video_parts + at,
                (s->video_part_count - at) * sizeof(VideoPart));
        s->video_parts[at] = part;
        if (!s->video_part_count) s->video_part_since = steam_now_ms();
        ++s->video_part_count;
        s->video_part_bytes += size;
        if (at + 1 < s->video_part_count && ++s->video_reordered <= 3)
            diagnostic_log("STEAM", "reordered video slice frame=%u range=%u..%u pending=%u",
                           frame_id, first, last, s->video_part_count);
    }
    /* FRAME_FINISH belongs to the final range in picture order. It cannot
     * publish a frame until all preceding ranges have been appended. */
    while (s->video_part_count && video_part_ready(s, &s->video_parts[0])) {
        VideoPart next = s->video_parts[0];
        --s->video_part_count;
        s->video_part_bytes -= next.size;
        memmove(s->video_parts, s->video_parts + 1, s->video_part_count * sizeof(VideoPart));
        const bool ok = append_video_part(s, &next);
        free(next.data);
        if (!ok) goto overflow;
    }
    if (!s->video_part_count) s->video_part_since = 0;
    return;
overflow:
    diagnostic_log("STEAM", "video assembly refused frame=%u range=%u..%u; requesting keyframe", frame_id, first, last);
    clear_video_frame(s);
    s->waiting_key = true;
    steam_session_request_keyframe(s);
}

static void on_data_message(SteamSession *s, unsigned channel, const uint8_t *data, size_t size)
{
    /* EStreamDataMessage 1 (a packet), then the frame header: id, timestamp,
     * input mark, input receive time (12 bytes). */
    if (size <= 13 || data[0] != 1) return;
    const uint16_t frame_id = get16(data + 1);
    if ((int)channel == s->video_channel) {
        video_frame(s, frame_id, data + 13, size - 13);
    } else if ((int)channel == s->audio_channel) {
        s->stats.audio_packets++;
        if (s->cb.audio) s->cb.audio(s->cb.user, data + 13, size - 13, frame_id);
    }
}

static void on_unreliable(SteamSession *s, unsigned channel, const uint8_t *h, const uint8_t *body, size_t size)
{
    Slot *slots;
    unsigned window;
    if ((int)channel == s->video_channel) {
        slots = s->video_slots;
        window = VIDEO_WINDOW;
    } else if ((int)channel == s->audio_channel) {
        slots = s->audio_slots;
        window = AUDIO_WINDOW;
    } else {
        return;
    }
    const unsigned type = h[0] & 0x7f;
    const int16_t fragment = (int16_t)get16(h + 5);
    const uint16_t id = get16(h + 7);
    if (type == PKT_UNRELIABLE && fragment == 0) {
        on_data_message(s, channel, body, size);
        return;
    }
    if (size > SLOT_DATA) return;
    Slot *slot = &slots[id % window];
    slot->used = true;
    slot->type = (uint8_t)type;
    slot->fragment = fragment;
    slot->id = id;
    slot->size = (uint16_t)size;
    memcpy(slot->data, body, size);
    /* Find the message's first packet (it says how many follow), then
     * check that all of them are here. */
    for (unsigned back = 0; back < window; ++back) {
        const uint16_t head_id = (uint16_t)(id - back);
        Slot *head = &slots[head_id % window];
        if (!head->used || head->id != head_id) return;
        if (head->type != PKT_UNRELIABLE) continue;
        const unsigned count = (unsigned)head->fragment + 1;
        if (head->fragment < 0 || count > window || back >= count) return;
        size_t total = 0;
        for (unsigned i = 0; i < count; ++i) {
            const Slot *part = &slots[(uint16_t)(head_id + i) % window];
            if (!part->used || part->id != (uint16_t)(head_id + i)) return;
            total += part->size;
        }
        uint8_t *message = malloc(total);
        if (!message) return;
        size_t at = 0;
        for (unsigned i = 0; i < count; ++i) {
            Slot *part = &slots[(uint16_t)(head_id + i) % window];
            memcpy(message + at, part->data, part->size);
            at += part->size;
            part->used = false;
        }
        on_data_message(s, channel, message, total);
        free(message);
        return;
    }
}

bool steam_session_request_keyframe(SteamSession *s)
{
    if (!s || s->video_channel < 0) return false;
    const uint64_t now = steam_now_ms();
    if (now - s->last_keyframe_request_at < 200) return false;
    s->last_keyframe_request_at = now;
    s->stats.keyframe_requests++;
    const uint8_t body[1] = { 2 };   /* k_EStreamDataLost, empty CStreamDataLostMsg */
    send_unreliable(s, (unsigned)s->video_channel, body, sizeof(body));
    return true;
}

/* ---- Discovery channel ------------------------------------------------------- */

static void on_ping(SteamSession *s, const uint8_t *body, size_t size)
{
    if (size < 5 || body[0] != 1) return;
    const uint32_t length = get32(body + 1);
    if (length > size - 5) return;
    uint32_t sequence = 0, requested = 0;
    PbReader r;
    PbField f;
    pb_reader(&r, body + 5, length);
    while (pb_next(&r, &f)) {
        if (f.field == 1) sequence = (uint32_t)f.value;
        else if (f.field == 2) requested = (uint32_t)f.value;
    }
    uint8_t message_data[16], out[SLOT_DATA];
    PbWriter m;
    pb_writer(&m, message_data, sizeof(message_data));
    pb_varint(&m, 1, sequence);
    pb_varint(&m, 2, HEADER_SIZE + size);
    size_t n = 0;
    out[n++] = 2;   /* k_EStreamDiscoveryPingResponse */
    put32(out + n, (uint32_t)m.len);
    n += 4;
    memcpy(out + n, m.data, m.len);
    n += m.len;
    /* Steam measures the path MTU with these: the reply must come back at
     * the size asked (header and body; the CRC is extra). */
    if (requested > HEADER_SIZE + n && requested - HEADER_SIZE <= sizeof(out) - 4 - HEADER_SIZE) {
        memset(out + n, 0xFE, requested - HEADER_SIZE - n);
        n = requested - HEADER_SIZE;
    }
    uint8_t packet[SLOT_DATA + 32];
    if (HEADER_SIZE + n + 4 <= SLOT_DATA)
        send_raw(s, packet, build(s, packet, PKT_UNCONNECTED, CH_DISCOVERY, 0, 0, out, n, false));
}

/* ---- Receive and timers ------------------------------------------------------ */

int steam_session_receive(SteamSession *s)
{
    uint8_t packet[2048];
    uint32_t from;
    const int n = steam_udp_recv(s->sock, packet, sizeof(packet), &from, NULL);
    if (n <= 0) return n < 0 ? -1 : 0;
    if (from != s->config.host_ip || n < HEADER_SIZE) return 1;
    s->stats.packets_in++;
    size_t size = (size_t)n;
    const bool has_crc = (packet[0] & 0x80) != 0;
    if (has_crc) {
        if (size < HEADER_SIZE + 4 || steam_crc32c(packet, size - 4) != get32(packet + size - 4)) {
            s->stats.packets_bad++;
            return 1;
        }
        size -= 4;
    }
    const unsigned type = packet[0] & 0x7f;
    const unsigned channel = packet[4];
    const uint8_t *body = packet + HEADER_SIZE;
    const size_t body_size = size - HEADER_SIZE;
    if (type != PKT_UNCONNECTED && type != PKT_CONNECT_ACK && s->host_cid && packet[2] != s->host_cid) return 1;
    s->last_heard_at = steam_now_ms();
    switch (type) {
    case PKT_CONNECT_ACK:
        if (s->state == STEAM_SESSION_CONNECTING && packet[3] == s->cid) {
            s->host_cid = packet[2];
            s->state = STEAM_SESSION_HANDSHAKE;
            set_status(s, "Connected; setting up the stream...");
            diagnostic_log("STEAM", "connected after %llu ms", (unsigned long long)(steam_now_ms() - s->opened_at));
            send_client_handshake(s);
        }
        break;
    case PKT_UNCONNECTED:
        on_ping(s, body, body_size);
        break;
    case PKT_RELIABLE:
    case PKT_RELIABLE_FRAG:
        if (channel == CH_CONTROL || channel == CH_STATS) on_reliable(s, channel, packet, body, body_size);
        break;
    case PKT_UNRELIABLE:
    case PKT_UNRELIABLE_FRAG:
        on_unreliable(s, channel, packet, body, body_size);
        break;
    case PKT_ACK:
        on_ack(s, channel, get16(packet + 7), body, body_size);
        break;
    case PKT_NACK:
        /* The PC is missing some of ours: resend them all at the next tick. */
        for (unsigned i = 0; i < PENDING_MAX; ++i)
            if (s->pending[i].used && s->pending[i].channel == channel) s->pending[i].sent_at = 0;
        break;
    case PKT_DISCONNECT:
        if (s->state != STEAM_SESSION_FAILED) {
            s->state = STEAM_SESSION_CLOSED;
            set_status(s, "The PC ended the stream.");
            diagnostic_log("STEAM", "the PC disconnected");
        }
        break;
    default:
        break;
    }
    return 1;
}

void steam_session_tick(SteamSession *s)
{
    if (!s || s->state == STEAM_SESSION_FAILED || s->state == STEAM_SESSION_CLOSED) return;
    const uint64_t now = steam_now_ms();
    if (s->state == STEAM_SESSION_CONNECTING) {
        if (now - s->last_connect_at >= 100) send_connect(s);
        if (now - s->opened_at > 10000) fail(s, "The PC didn't answer the stream connection.");
        return;
    }
    if (s->state != STEAM_SESSION_STREAMING && now - s->opened_at > 20000) {
        fail(s, "The stream didn't start (Steam stopped answering during setup).");
        return;
    }
    if (now - s->last_heard_at > 10000) {
        fail(s, "Lost the connection to the PC.");
        return;
    }
    /* Resend what the PC hasn't acknowledged: quickly at first, backing off. */
    const unsigned base = s->stats.rtt_ms > 0 ? (unsigned)s->stats.rtt_ms * 2 + 30 : 80;
    for (unsigned i = 0; i < PENDING_MAX; ++i) {
        Pending *p = &s->pending[i];
        if (!p->used) continue;
        const unsigned timeout = base << (p->retries < 4 ? p->retries : 4);
        if (now - p->sent_at < timeout) continue;
        p->retries++;
        p->data[1] = (uint8_t)(p->retries < 255 ? p->retries : 255);
        put32(p->data + p->size - 4, steam_crc32c(p->data, p->size - 4u));
        send_raw(s, p->data, p->size);
        p->sent_at = now;
        s->stats.resends++;
    }
    if (s->state == STEAM_SESSION_STREAMING) {
        if (s->video_part_count && now - s->video_part_since >= VIDEO_PART_TIMEOUT_MS) {
            diagnostic_log("STEAM", "video slice timeout expected=%u pending=%u; requesting keyframe",
                           s->video_next_slice, s->video_part_count);
            clear_video_frame(s);
            s->waiting_key = true;
            ++s->stats.video_lost;
        }
        if (s->waiting_key && !s->placeholder && now - s->last_keyframe_request_at >= 200)
            steam_session_request_keyframe(s);
        if (now - s->last_keepalive_at >= 10000) send_keepalive(s);
        if (s->hid_started && now - s->hid_sent_at >= 8) hid_send_report(s, false);
    }
}

/* ---- Mouse and keyboard ------------------------------------------------------ */

void steam_session_mouse_move(SteamSession *s, int dx, int dy)
{
    if (!s || s->state != STEAM_SESSION_STREAMING || (!dx && !dy)) return;
    uint8_t data[24];
    PbWriter m;
    pb_writer(&m, data, sizeof(data));
    pb_int(&m, 4, dx);
    pb_int(&m, 5, dy);
    send_control(s, CTL_MOUSE_MOTION, &m);
}

bool steam_session_mouse_button(SteamSession *s, bool down)
{
    if (!s || s->state != STEAM_SESSION_STREAMING) return false;
    uint8_t data[8];
    PbWriter m;
    pb_writer(&m, data, sizeof(data));
    pb_varint(&m, 2, 1);   /* k_EStreamMouseButtonLeft */
    return send_control(s, down ? CTL_MOUSE_DOWN : CTL_MOUSE_UP, &m);
}

bool steam_session_key(SteamSession *s, unsigned scancode, bool down, unsigned modifiers)
{
    if (!s || s->state != STEAM_SESSION_STREAMING || !scancode) return false;
    uint8_t data[16];
    PbWriter m;
    pb_writer(&m, data, sizeof(data));
    pb_varint(&m, 2, scancode);
    if (modifiers) pb_varint(&m, 3, modifiers);
    return send_control(s, down ? CTL_KEY_DOWN : CTL_KEY_UP, &m);
}

bool steam_session_game_running(const SteamSession *s)
{
    return s && s->state == STEAM_SESSION_STREAMING && s->activity == 2;
}

bool steam_session_stop_game(SteamSession *s)
{
    /* Steam's StopRequest (129) and QuitRequest (83) only end the stream:
     * the game kept running and the next start streamed it again (report
     * KA5BCU, Minecraft through Prism Launcher; QuitRequest tried on the PC
     * too). Alt+F4 closes it, the way a player at the PC would, and the
     * launcher with it. Only while a game is in front: on the desktop
     * Alt+F4 opens Windows' "Shut Down" dialog. */
    if (!steam_session_game_running(s)) {
        diagnostic_log("STEAM", "quit skipped: no game in front (activity %d)", s ? s->activity : -1);
        return false;
    }
    bool ok = steam_session_key(s, 226, true, 0x100);        /* left Alt */
    ok = steam_session_key(s, 61, true, 0x100) && ok;        /* F4 */
    ok = steam_session_key(s, 61, false, 0x100) && ok;
    ok = steam_session_key(s, 226, false, 0) && ok;
    s->stop_sent = ok;
    diagnostic_log("STEAM", "closed the game with Alt+F4 (%s)", ok ? "sent" : "not sent");
    return ok;
}

void steam_session_close(SteamSession *s)
{
    if (!s) return;
    if (s->stop_sent) {
        /* The StopRequest must arrive before the goodbye: resend it until
         * the PC acknowledges, for up to a second. Nothing is shown now. */
        s->cb.video = NULL;
        s->cb.audio = NULL;
        s->cb.activity = NULL;
        const uint64_t until = steam_now_ms() + 1000;
        for (;;) {
            bool waiting = false;
            for (unsigned i = 0; i < PENDING_MAX && !waiting; ++i)
                waiting = s->pending[i].used && s->pending[i].channel == CH_CONTROL;
            if (!waiting || steam_now_ms() >= until || s->state == STEAM_SESSION_CLOSED) break;
            steam_udp_wait(s->sock, 10);
            while (steam_session_receive(s) > 0) {}
            steam_session_tick(s);
        }
    }
    if (s->host_cid) {
        uint8_t packet[64];
        for (unsigned i = 0; i < 3; ++i) {
            const size_t n = build(s, packet, PKT_DISCONNECT, CH_DISCOVERY, 0, 0, NULL, 0, true);
            packet[1] = (uint8_t)i;
            put32(packet + n - 4, steam_crc32c(packet, n - 4));
            send_raw(s, packet, n);
        }
    }
    diagnostic_log("STEAM", "session closed: frames=%u keyframes=%u lost=%u kfreq=%u audio=%u resends=%u bad=%u",
                   s->stats.video_frames, s->stats.video_keyframes, s->stats.video_lost, s->stats.keyframe_requests,
                   s->stats.audio_packets, s->stats.resends, s->stats.packets_bad);
    steam_udp_close(s->sock);
    clear_video_frame(s);
    free(s);
}

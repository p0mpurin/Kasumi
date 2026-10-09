#include "xcloud_stream.h"

#include <3ds.h>
#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "diagnostic.h"
#include "gfn_input.h"

#include "peer_connection.h"

/* The console is the DTLS client, so its channels take even stream ids. */
enum { SID_CHAT = 0, SID_CONTROL = 2, SID_INPUT = 4, SID_MESSAGE = 6 };
enum {
    REPORT_METADATA = 1, REPORT_GAMEPAD = 2, REPORT_CLIENT_METADATA = 8, REPORT_SERVER_METADATA = 16,
    REPORT_VIBRATION = 128
};
/* Changes go out at once (4 ms apart at most); an unchanged controller is
 * repeated every 100 ms. Frame metadata rides along, or goes alone when
 * five frames have piled up. */
enum { INPUT_MIN_MS = 4, INPUT_REPEAT_MS = 100, FRAME_RING = 16, FRAMES_ALONE = 5, PENDING_MAX = 8 };
/* xbox.com's client key for the control channel (the same in every client). */
#define CONTROL_ACCESS_KEY "4BDB3609-C1F1-4195-9B37-FEFF45DA8B8E"

static struct {
    bool opened, handshake_sent, handshake_ack, configured, disconnect_seen;
    unsigned handshake_tries;
    u64 handshake_at, start_at, last_input_at;
    uint32_t sequence;
    GfnGamepadState last;
    bool sent_once;
    struct { uint32_t key, arrival; } frames[FRAME_RING];
    unsigned frame_count;
    /* Transactions the service started, to complete from the tick. */
    char pending[PENDING_MAX][64];
    unsigned pending_count;
    unsigned rx_logged;
    char install_id[40];
} x;

void xcloud_stream_reset(void)
{
    memset(&x, 0, sizeof(x));
}

static uint32_t ms_since_start(void)
{
    if (!x.start_at) x.start_at = osGetTime();
    return (uint32_t)(osGetTime() - x.start_at);
}

static void uuid(char out[40])
{
    static const char hex[] = "0123456789abcdef";
    int k = 0;
    for (int i = 0; i < 36; ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) out[k++] = '-';
        else out[k++] = hex[(rand() ^ (int)(svcGetSystemTick() >> (i % 7))) & 15];
    }
    out[k] = '\0';
}

void xcloud_stream_note_frame(uint32_t rtp_timestamp)
{
    if (x.frame_count == FRAME_RING) {
        memmove(&x.frames[0], &x.frames[1], sizeof(x.frames[0]) * (FRAME_RING - 1));
        --x.frame_count;
    }
    x.frames[x.frame_count].key = rtp_timestamp;
    x.frames[x.frame_count].arrival = ms_since_start();
    ++x.frame_count;
}

static bool send_text(PeerConnection *pc, uint16_t sid, const char *text)
{
    return peer_connection_datachannel_send_string_sid(pc, (char *)text, strlen(text), sid) >= 0;
}

/* A message-channel message: {"type","content"(JSON text),"id","target","cv"}. */
static bool send_message(PeerConnection *pc, const char *type, const char *target, json_t *content)
{
    char *content_text = content ? json_dumps(content, JSON_COMPACT) : strdup("");
    json_decref(content);
    char id[40];
    uuid(id);
    json_t *message = json_pack("{s:s,s:s,s:s,s:s,s:s}", "type", type, "content", content_text ? content_text : "",
                                "id", id, "target", target, "cv", "");
    free(content_text);
    char *text = message ? json_dumps(message, JSON_COMPACT) : NULL;
    json_decref(message);
    const bool ok = text && send_text(pc, SID_MESSAGE, text);
    free(text);
    return ok;
}

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, (uint16_t)v); put16(p + 2, (uint16_t)(v >> 16)); }

/* Report header: type, sequence, then a timestamp in ms as a double. */
static size_t report_header(uint8_t *out, uint16_t type)
{
    put16(out, type);
    put32(out + 2, x.sequence++);
    const double ms = (double)ms_since_start();
    memcpy(out + 6, &ms, sizeof(ms)); /* little-endian ARM: as on the wire */
    return 14;
}

static uint16_t xbox_buttons(uint16_t pad)
{
    static const struct { uint16_t from, to; } map[] = {
        { GFN_PAD_GUIDE, 2 }, { GFN_PAD_START, 4 }, { GFN_PAD_BACK, 8 },
        { GFN_PAD_A, 16 }, { GFN_PAD_B, 32 }, { GFN_PAD_X, 64 }, { GFN_PAD_Y, 128 },
        { GFN_PAD_DPAD_UP, 256 }, { GFN_PAD_DPAD_DOWN, 512 }, { GFN_PAD_DPAD_LEFT, 1024 },
        { GFN_PAD_DPAD_RIGHT, 2048 }, { GFN_PAD_LEFT_SHOULDER, 4096 }, { GFN_PAD_RIGHT_SHOULDER, 8192 },
        { GFN_PAD_LEFT_THUMB, 16384 }, { GFN_PAD_RIGHT_THUMB, 32768 },
    };
    uint16_t out = 0;
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); ++i)
        if (pad & map[i].from) out |= map[i].to;
    return out;
}

static bool send_report(PeerConnection *pc, const GfnGamepadState *pad)
{
    uint8_t packet[14 + 1 + FRAME_RING * 28 + 1 + 23];
    const uint16_t type = (x.frame_count ? REPORT_METADATA : 0) | (pad ? REPORT_GAMEPAD : 0);
    if (!type) return true;
    size_t at = report_header(packet, type);
    if (x.frame_count) {
        const uint32_t now = ms_since_start();
        packet[at++] = (uint8_t)x.frame_count;
        for (unsigned i = 0; i < x.frame_count; ++i) {
            put32(packet + at, x.frames[i].key);
            put32(packet + at + 4, x.frames[i].arrival);   /* first packet */
            put32(packet + at + 8, x.frames[i].arrival);   /* submitted */
            put32(packet + at + 12, now);                  /* decoded */
            put32(packet + at + 16, now);                  /* rendered */
            put32(packet + at + 20, now);                  /* packet time */
            put32(packet + at + 24, now);
            at += 28;
        }
        x.frame_count = 0;
    }
    if (pad) {
        packet[at++] = 1;
        packet[at++] = 0; /* gamepad index */
        put16(packet + at, xbox_buttons(pad->buttons));
        put16(packet + at + 2, (uint16_t)pad->left_x);
        put16(packet + at + 4, (uint16_t)pad->left_y);
        put16(packet + at + 6, (uint16_t)pad->right_x);
        put16(packet + at + 8, (uint16_t)pad->right_y);
        put16(packet + at + 10, (uint16_t)(pad->left_trigger * 257u));
        put16(packet + at + 12, (uint16_t)(pad->right_trigger * 257u));
        put32(packet + at + 14, 1);                        /* physical physicality */
        packet[at + 18] = 0; packet[at + 19] = 0; packet[at + 20] = 0; packet[at + 21] = 1; /* virtual, big-endian */
        at += 22;
    }
    return peer_connection_datachannel_send_binary_sid(pc, (char *)packet, at, SID_INPUT) >= 0;
}

static bool open_channels(PeerConnection *pc)
{
    static const struct { const char *label, *protocol; uint16_t sid; } channels[] = {
        { "chat", "chatV1", SID_CHAT }, { "control", "controlV1", SID_CONTROL },
        { "input", "1.0", SID_INPUT }, { "message", "messageV1", SID_MESSAGE },
    };
    for (size_t i = 0; i < sizeof(channels) / sizeof(channels[0]); ++i) {
        char label[16], protocol[16];
        snprintf(label, sizeof(label), "%s", channels[i].label);
        snprintf(protocol, sizeof(protocol), "%s", channels[i].protocol);
        if (peer_connection_create_datachannel_sid(pc, DATA_CHANNEL_RELIABLE, 0, 0, label, protocol,
                                                   channels[i].sid) < 0)
            return false;
    }
    diagnostic_log("XCLOUD", "data channels opened");
    return true;
}

static void send_handshake(PeerConnection *pc)
{
    char id[40], text[160];
    uuid(id);
    snprintf(text, sizeof(text), "{\"type\":\"Handshake\",\"version\":\"messageV1\",\"id\":\"%s\",\"cv\":\"0\"}", id);
    const bool ok = send_text(pc, SID_MESSAGE, text);
    x.handshake_sent = true;
    x.handshake_at = osGetTime();
    ++x.handshake_tries;
    diagnostic_log("XCLOUD", "message handshake %u sent=%d", x.handshake_tries, ok ? 1 : 0);
}

/* After the handshake: the control channel's key and controller, the input
 * channel's client metadata, and what kind of screen this is. */
static void send_configuration(WebRtcTransport *t, PeerConnection *pc)
{
    send_text(pc, SID_CONTROL, "{\"message\":\"authorizationRequest\",\"accessKey\":\"" CONTROL_ACCESS_KEY "\"}");
    send_text(pc, SID_CONTROL, "{\"message\":\"gamepadChanged\",\"gamepadIndex\":0,\"wasAdded\":true}");
    uint8_t metadata[15];
    report_header(metadata, REPORT_CLIENT_METADATA);
    metadata[14] = 0; /* touch points */
    peer_connection_datachannel_send_binary_sid(pc, (char *)metadata, sizeof(metadata), SID_INPUT);
    if (!x.install_id[0]) uuid(x.install_id);
    /* No system UIs of our own: the console draws its dialogs and keyboard
     * into the stream. */
    send_message(pc, "Message", "/streaming/systemUi/configuration",
                 json_pack("{s:[i,i,i],s:[]}", "version", 0, 2, 0, "systemUis"));
    send_message(pc, "Message", "/streaming/properties/clientappinstallidchanged",
                 json_pack("{s:s}", "clientAppInstallId", x.install_id));
    send_message(pc, "Message", "/streaming/characteristics/orientationchanged", json_pack("{s:i}", "orientation", 0));
    send_message(pc, "Message", "/streaming/characteristics/touchinputenabledchanged",
                 json_pack("{s:b}", "touchInputEnabled", 0));
    send_message(pc, "Message", "/streaming/characteristics/clientdevicecapabilities", json_object());
    /* The service fits the picture to this: the top screen's wide size, not
     * a 1080p one to shrink (2026-10-07: 800x480 came at 60 fps). Classic
     * mode has the decoder shrink it to 400x240. */
    const int w = XCLOUD_STREAM_WIDTH, h = XCLOUD_STREAM_HEIGHT;
    send_message(pc, "Message", "/streaming/characteristics/dimensionschanged",
                 json_pack("{s:i,s:i,s:i,s:i,s:i,s:i,s:i,s:i,s:b}", "horizontal", w, "vertical", h,
                           "preferredWidth", w, "preferredHeight", h, "safeAreaLeft", 0, "safeAreaTop", 0,
                           "safeAreaRight", w, "safeAreaBottom", h, "supportsCustomResolution", 1));
    x.configured = true;
    t->input_ready = true;
    snprintf(t->status, sizeof(t->status), "Xbox stream ready (%dx%d)", w, h);
    diagnostic_log("XCLOUD", "configured: %dx%d", w, h);
}

void xcloud_stream_on_data(WebRtcTransport *t, const char *data, size_t length, uint16_t sid)
{
    t->data_messages++;
    if (sid == SID_INPUT) {
        if (length >= 2) {
            const unsigned type = (uint8_t)data[0] | ((uint8_t)data[1] << 8);
            if (type == REPORT_SERVER_METADATA && length >= 10 && x.rx_logged < 40) {
                ++x.rx_logged;
                const uint32_t h = (uint8_t)data[2] | ((uint8_t)data[3] << 8) | ((uint8_t)data[4] << 16) |
                                   ((uint32_t)(uint8_t)data[5] << 24);
                const uint32_t w = (uint8_t)data[6] | ((uint8_t)data[7] << 8) | ((uint8_t)data[8] << 16) |
                                   ((uint32_t)(uint8_t)data[9] << 24);
                diagnostic_log("XCLOUD", "server video %lux%lu", (unsigned long)w, (unsigned long)h);
            }
        }
        return;
    }
    if (sid != SID_MESSAGE && sid != SID_CONTROL) return;
    char text[2048];
    const size_t n = length < sizeof(text) - 1 ? length : sizeof(text) - 1;
    memcpy(text, data, n);
    text[n] = '\0';
    json_error_t error;
    json_t *root = json_loads(text, 0, &error);
    const char *type = json_string_value(json_object_get(root, "type"));
    const char *target = json_string_value(json_object_get(root, "target"));
    const char *id = json_string_value(json_object_get(root, "id"));
    if (type && !strcmp(type, "HandshakeAck")) {
        x.handshake_ack = true;
        diagnostic_log("XCLOUD", "message handshake answered");
    } else if (target && !strcmp(target, "/streaming/sessionLifetimeManagement/serverInitiatedDisconnect")) {
        x.disconnect_seen = true;
        diagnostic_log("XCLOUD", "the service ended the stream");
    }
    if (type && !strcmp(type, "TransactionStart") && id && strlen(id) < sizeof(x.pending[0]) &&
        x.pending_count < PENDING_MAX)
        snprintf(x.pending[x.pending_count++], sizeof(x.pending[0]), "%s", id);
    if (x.rx_logged < 40 && (!target || strcmp(target, "/streaming/properties/titleinfo"))) {
        ++x.rx_logged;
        diagnostic_log("XCLOUD", "rx sid=%u type=%s target=%.80s", sid, type ? type : "-", target ? target : "-");
    }
    json_decref(root);
}

void xcloud_stream_tick(WebRtcTransport *t, void *peer)
{
    PeerConnection *pc = peer;
    const u64 now = osGetTime();
    if (!t->data_open) return;
    if (!x.opened) {
        x.opened = open_channels(pc);
        if (!x.opened) return;
    }
    if (!x.handshake_sent || (!x.handshake_ack && now - x.handshake_at >= 3000 && x.handshake_tries < 4))
        send_handshake(pc);
    if (!x.handshake_ack) return;
    if (!x.configured) send_configuration(t, pc);
    for (unsigned i = 0; i < x.pending_count; ++i) {
        char text[160];
        snprintf(text, sizeof(text), "{\"type\":\"TransactionComplete\",\"content\":\"\",\"id\":\"%s\",\"cv\":\"\"}",
                 x.pending[i]);
        send_text(pc, SID_MESSAGE, text);
    }
    x.pending_count = 0;
    if (x.disconnect_seen && t->state == WEBRTC_CONNECTED) {
        t->state = WEBRTC_FAILED;
        snprintf(t->status, sizeof(t->status), "Xbox Cloud Gaming ended the stream");
        return;
    }

    GfnGamepadState pad;
    gfn_input_read_3ds(&pad);
    if (t->keyboard_mode) memset(&pad, 0, sizeof(pad));
    const bool changed = !x.sent_once || memcmp(&pad, &x.last, sizeof(pad)) != 0;
    const u64 since = now - x.last_input_at;
    if ((changed && since >= INPUT_MIN_MS) || since >= INPUT_REPEAT_MS) {
        if (send_report(pc, &pad)) {
            t->input_reports++;
            if (changed && pad.buttons != x.last.buttons)
                diagnostic_log("INPUT", "xbox report=%u buttons=%04x triggers=%u/%u", t->input_reports,
                               pad.buttons, pad.left_trigger, pad.right_trigger);
            x.last = pad;
            x.sent_once = true;
        }
        x.last_input_at = now;
    } else if (x.frame_count >= FRAMES_ALONE) {
        send_report(pc, NULL);
    }
}

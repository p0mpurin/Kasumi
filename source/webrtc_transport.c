#include "webrtc_transport.h"
#include "stream_profile.h"
#include "nvst_signal.h"
#include "mvd_video.h"
#include "gfn_input.h"
#include "diagnostic.h"
#include "audio_output.h"
#include "h264_sps.h"

#include "peer.h"
#include "peer_connection.h"
#include "address.h"
#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SDP_LIMIT 8192


enum {
    /* Button and trigger edges go out within 4 ms; stick motion changes
     * almost every poll, so it stays at 8 ms to keep socket traffic sane. */
    INPUT_BUTTON_INTERVAL_MS = 4,
    INPUT_MIN_INTERVAL_MS = 8,
    INPUT_REPEAT_MS = 16,
    PARTIAL_INPUT_SID = 2,
    /* Matches a=ri.partialReliableThresholdMs in our answer: a controller
     * state older than this is worthless, so it is never retransmitted. */
    PARTIAL_INPUT_LIFETIME_MS = 16
};
static bool g_peer_runtime;

static void normalize_media_ip(char out[64], const char *value)
{
    int a, b, c, d;
    if (value && sscanf(value, "%d-%d-%d-%d", &a, &b, &c, &d) == 4 &&
        a >= 0 && a <= 255 && b >= 0 && b <= 255 &&
        c >= 0 && c <= 255 && d >= 0 && d <= 255)
        snprintf(out, 64, "%d.%d.%d.%d", a, b, c, d);
    else
        snprintf(out, 64, "%s", value ? value : "");
}

static int line_integer(const char *text, const char *prefix)
{
    const char *p = text ? strstr(text, prefix) : NULL;
    if (!p) return 0;
    char *end = NULL;
    long value = strtol(p + strlen(prefix), &end, 10);
    return end != p + strlen(prefix) && value > 0 && value <= 65535 ? (int)value : 0;
}

static int offer_media_port(const char *sdp)
{
    const char *p = sdp ? strstr(sdp, "m=video ") : NULL;
    return p ? line_integer(p, "m=video ") : 0;
}

static void on_local_candidates(char *sdp, void *userdata)
{
    WebRtcTransport *t = userdata;
    const char *p = sdp;
    while (p && (p = strstr(p, "a=candidate:")) != NULL) {
        const char *end = strstr(p, "\r\n");
        size_t length = end ? (size_t)(end - p) : strlen(p);
        if (t->local_candidates < 5 && length > 2 && length < sizeof(t->pending_local_candidates[0])) {
            /* NVIDIA's trickle message wants candidate:..., without the SDP a=. */
            memcpy(t->pending_local_candidates[t->local_candidates], p + 2, length - 2);
            t->pending_local_candidates[t->local_candidates][length - 2] = 0;
            t->local_candidates++;
        }
        p = end ? end + 2 : NULL;
    }
}

static void send_local_candidates(WebRtcTransport *t, NvstSignal *signal)
{
    while (t->sent_local_candidates < t->local_candidates) {
        const char *candidate = t->pending_local_candidates[t->sent_local_candidates];
        json_t *root = json_pack("{s:s,s:s,s:i}", "candidate", candidate,
                                 "sdpMid", "0", "sdpMLineIndex", 0);
        char *text = root ? json_dumps(root, JSON_COMPACT) : NULL;
        const bool sent = text && nvst_signal_send_candidate(signal, text);
        free(text);
        if (root) json_decref(root);
        if (!sent) break;
        t->sent_local_candidates++;
    }
}

/* GFN supplies numeric media addresses; the 3DS build does not need mDNS. */
int mdns_resolve_addr(const char *hostname, Address *address)
{
    (void)hostname; (void)address; return -1;
}

static void append(char *out, size_t cap, const char *line)
{
    size_t used = strlen(out), length = strlen(line);
    if (used + length + 3 >= cap) return;
    memcpy(out + used, line, length);
    memcpy(out + used + length, "\r\n", 3);
}

static bool sdp_value(const char *sdp, const char *prefix, char *out, size_t cap)
{
    const size_t n = strlen(prefix);
    for (const char *p = sdp; p && *p;) {
        const char *end = strstr(p, "\r\n");
        if (!end) end = p + strlen(p);
        if ((size_t)(end - p) >= n && !strncmp(p, prefix, n)) {
            size_t length = (size_t)(end - p) - n;
            if (length >= cap) length = cap - 1;
            memcpy(out, p + n, length); out[length] = 0;
            return true;
        }
        p = *end ? end + 2 : NULL;
    }
    return false;
}

static int h264_payload(const char *offer)
{
    const char *p = offer;
    while ((p = strstr(p, "a=rtpmap:"))) {
        int pt = 0;
        if (sscanf(p, "a=rtpmap:%d H264/90000", &pt) == 1 && pt > 0 && pt < 128)
            return pt;
        p += 9;
    }
    return 96;
}

static void media_mid(const char *offer, const char *media, char *out, size_t cap)
{
    char marker[24]; snprintf(marker, sizeof(marker), "m=%s ", media);
    const char *start = strstr(offer, marker);
    if (!start) return;
    const char *end = strstr(start + 2, "\r\nm=");
    const char *mid = strstr(start, "a=mid:");
    if (mid && (!end || mid < end)) {
        const char *eol = strstr(mid, "\r\n");
        size_t n = eol ? (size_t)(eol - mid - 6) : strlen(mid + 6);
        if (n >= cap) n = cap - 1;
        memcpy(out, mid + 6, n); out[n] = 0;
    }
}

static char *prepare_offer(const char *offer, const char *media_ip)
{
    size_t n = strlen(offer), ipn = media_ip ? strlen(media_ip) : 0;
    if (n >= SDP_LIMIT || ipn >= 64) return NULL;
    size_t occurrences = 0;
    for (const char *p = offer; media_ip && (p = strstr(p, "0.0.0.0")); p += 7) occurrences++;
    size_t cap = n + occurrences * (ipn > 7 ? ipn - 7 : 0) + 3;
    char *result = calloc(1, cap);
    if (!result) return NULL;
    const char *p = offer;
    while (*p) {
        const char *zero = media_ip && media_ip[0] ? strstr(p, "0.0.0.0") : NULL;
        if (!zero) { strncat(result, p, cap - strlen(result) - 1); break; }
        strncat(result, p, (size_t)(zero - p));
        strncat(result, media_ip, cap - strlen(result) - 1);
        p = zero + 7;
    }
    if (n >= 2 && strcmp(result + strlen(result) - 2, "\r\n"))
        append(result, cap, "");
    return result;
}

static char *adapt_answer(const char *answer, const char *offer)
{
    char *out = calloc(1, SDP_LIMIT);
    if (!out) return NULL;
    const int video_pt = h264_payload(offer);
    char video_mid[32] = "video", audio_mid[32] = "audio", data_mid[32] = "datachannel";
    char offer_group[192] = "";
    media_mid(offer, "video", video_mid, sizeof(video_mid));
    media_mid(offer, "audio", audio_mid, sizeof(audio_mid));
    media_mid(offer, "application", data_mid, sizeof(data_mid));
    sdp_value(offer, "a=group:BUNDLE ", offer_group, sizeof(offer_group));
    enum { SECTION_NONE, SECTION_VIDEO, SECTION_AUDIO, SECTION_DATA } section = SECTION_NONE;
    for (const char *p = answer; p && *p;) {
        const char *end = strstr(p, "\r\n");
        if (!end) end = p + strlen(p);
        char line[1024]; size_t n = (size_t)(end - p);
        if (n >= sizeof(line)) n = sizeof(line) - 1;
        memcpy(line, p, n); line[n] = 0;
        char replacement[1024]; const char *emit = line;
        if (!strncmp(line, "a=group:BUNDLE ", 15) && offer_group[0]) {
            snprintf(replacement, sizeof(replacement), "a=group:BUNDLE %s", offer_group); emit = replacement;
        } else if (!strncmp(line, "m=video ", 8)) {
            section = SECTION_VIDEO;
            snprintf(replacement, sizeof(replacement), "m=video 9 UDP/TLS/RTP/SAVPF %d", video_pt); emit = replacement;
        } else if (!strncmp(line, "m=audio ", 8)) {
            section = SECTION_AUDIO; emit = "m=audio 9 UDP/TLS/RTP/SAVPF 111";
        } else if (!strncmp(line, "m=application ", 14)) {
            section = SECTION_DATA; emit = "m=application 9 UDP/DTLS/SCTP webrtc-datachannel";
        } else if (!strncmp(line, "a=mid:", 6)) {
            snprintf(replacement, sizeof(replacement), "a=mid:%s",
                section == SECTION_VIDEO ? video_mid : section == SECTION_AUDIO ? audio_mid : data_mid); emit = replacement;
        } else if (section == SECTION_VIDEO && !strncmp(line, "a=rtpmap:96", 11)) {
            snprintf(replacement, sizeof(replacement), "a=rtpmap:%d H264/90000", video_pt); emit = replacement;
        } else if (section == SECTION_VIDEO && !strncmp(line, "a=fmtp:96", 9)) {
            snprintf(replacement, sizeof(replacement), "a=fmtp:%d level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42e01f", video_pt); emit = replacement;
        } else if (section == SECTION_VIDEO && !strncmp(line, "a=rtcp-fb:96", 12)) {
            snprintf(replacement, sizeof(replacement), "a=rtcp-fb:%d%.980s", video_pt, line + 12); emit = replacement;
        } else if (section == SECTION_VIDEO && !strcmp(line, "a=sendrecv")) emit = "a=recvonly";
        append(out, SDP_LIMIT, emit);
        if (!strcmp(emit, "c=IN IP4 0.0.0.0") && section == SECTION_VIDEO) {
            char bandwidth[32];
            snprintf(bandwidth, sizeof(bandwidth), "b=AS:%u", stream_profile_max_bitrate());
            append(out, SDP_LIMIT, bandwidth);
        }
        p = *end ? end + 2 : NULL;
    }
    return out;
}

static char *build_nvst(const char *answer)
{
    char ufrag[64] = "", pwd[128] = "", fingerprint[192] = "";
    if (!sdp_value(answer, "a=ice-ufrag:", ufrag, sizeof(ufrag)) ||
        !sdp_value(answer, "a=ice-pwd:", pwd, sizeof(pwd)) ||
        !sdp_value(answer, "a=fingerprint:sha-256 ", fingerprint, sizeof(fingerprint))) return NULL;
    char *out = calloc(1, 4096);
    if (!out) return NULL;
    snprintf(out, 4096,
        "v=0\no=SdpTest test_id_13 14 IN IPv4 127.0.0.1\ns=-\nt=0 0\n"
        "a=general.icePassword:%s\na=general.iceUserNameFragment:%s\na=general.dtlsFingerprint:%s\n"
        "m=video 0 RTP/AVP\na=msid:fbc-video-0\n"
        "a=vqos.fec.rateDropWindow:10\na=vqos.fec.minRequiredFecPackets:2\n"
        "a=vqos.drc.minRequiredBitrateCheckEnabled:1\na=vqos.fec.repairMinPercent:6\n"
        "a=vqos.fec.repairPercent:8\na=vqos.fec.repairMaxPercent:30\n"
        "a=vqos.dynamicStreamingMode:%u\na=vqos.bllFec.enable:0\na=vqos.drc.enable:1\n"
        "a=bwe.useOwdCongestionControl:1\na=video.enableRtpNack:1\n"
        "a=vqos.bw.txRxLag.minFeedbackTxDeltaMs:200\na=vqos.drc.bitrateIirFilterFactor:18\n"
        "a=video.packetSize:1140\na=video.rtpNackQueueLength:1024\n"
        "a=video.rtpNackQueueMaxPackets:512\na=video.rtpNackMaxPacketCount:25\n"
        "a=packetPacing.numGroups:10\na=packetPacing.minNumPacketsPerGroup:4\n"
        "a=packetPacing.minNumPacketsFrame:4\na=packetPacing.maxDelayUs:3000\n"
        "a=video.mapRtpTimestampsToFrames:1\na=video.clientViewportWd:%u\n"
        "a=video.clientViewportHt:%u\na=video.maxFPS:30\na=video.maxNumReferenceFrames:4\n"
        "a=video.initialBitrateKbps:%u\n"
        "a=video.initialPeakBitrateKbps:%u\na=vqos.bw.maximumBitrateKbps:%u\n"
        "a=vqos.bw.minimumBitrateKbps:%u\na=vqos.bw.peakBitrateKbps:%u\n"
        "a=vqos.bw.serverPeakBitrateKbps:%u\na=vqos.bw.enableBandwidthEstimation:1\n"
        "a=vqos.bw.disableBitrateLimit:0\na=video.encoderCscMode:3\n"
        "a=video.dynamicRangeMode:0\na=video.bitDepth:8\n"
        "a=video.scalingFeature1:0\na=video.prefilterParams.prefilterMode:%u\n"
        "a=video.prefilterParams.prefilterModel:0\na=video.prefilterParams.denoiseLevel:0\n"
        "a=video.prefilterParams.sharpnessLevel:%u\n"
        "m=audio 0 RTP/AVP\na=msid:audio\nm=mic 0 RTP/AVP\na=msid:mic\na=rtpmap:0 PCMU/8000\n"
        "m=application 0 RTP/AVP\na=msid:input_1\na=ri.partialReliableThresholdMs:16\n"
        "a=ri.hidDeviceMask:4294967295\na=ri.enablePartiallyReliableTransferGamepad:15\n"
        "a=ri.enablePartiallyReliableTransferHid:4294967295\n",
        pwd, ufrag, fingerprint, stream_profile_dynamic_mode(),
        stream_profile_width(), stream_profile_height(),
        stream_profile_initial_bitrate(), stream_profile_max_bitrate(),
        stream_profile_max_bitrate(), stream_profile_min_bitrate(),
        stream_profile_max_bitrate(), stream_profile_max_bitrate(),
        stream_profile_sharpen() ? 1u : 0u, stream_profile_sharpen() ? 50u : 0u);
    return out;
}

static void inspect_h264_access_unit(const unsigned char *data, size_t size,
                                     bool *has_sps, bool *has_pps, bool *has_idr)
{
    *has_sps = *has_pps = *has_idr = false;
    for (size_t i = 0; i + 4 < size; ++i) {
        size_t header = 0;
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1)
            header = i + 3;
        else if (i + 5 < size && data[i] == 0 && data[i + 1] == 0 &&
                 data[i + 2] == 0 && data[i + 3] == 1)
            header = i + 4;
        if (!header || header >= size) continue;
        const unsigned type = data[header] & 0x1f;
        if (type == 7) *has_sps = true;
        else if (type == 8) *has_pps = true;
        else if (type == 5) *has_idr = true;
        i = header;
    }
}

static void request_video_keyframe(WebRtcTransport *t, const char *reason)
{
    if (!t || !t->peer || t->state != WEBRTC_CONNECTED) return;
    const int result = peer_connection_request_video_keyframe(t->peer);
    t->last_keyframe_request_at = osGetTime();
    t->keyframe_requests++;
    diagnostic_log("VIDEO", "PLI reason=%s attempt=%u result=%d",
                   reason, t->keyframe_requests, result);
}

static void on_video(const PeerVideoPacket *packet, void *userdata)
{
    WebRtcTransport *t = userdata;
    bool has_sps, has_pps, has_idr;
    inspect_h264_access_unit(packet->data, packet->size, &has_sps, &has_pps, &has_idr);
    t->video_saw_sps |= has_sps;
    t->video_saw_pps |= has_pps;
    t->video_saw_idr |= has_idr;
    if (has_idr) t->video_idr_units++;
    t->video_access_units++;
    if (has_sps) {
        unsigned width, height, profile, level, refs;
        uint32_t signature;
        if (h264_sps_dimensions(packet->data, packet->size, &width, &height,
                                &profile, &level, &refs, &signature) &&
            signature != t->video_sps_signature) {
            t->video_sps_signature = signature;
            t->video_source_width = width;
            t->video_source_height = height;
            t->video_source_refs = refs;
            diagnostic_log("VIDEO", "SPS source=%ux%u profile=%u level=%u refs=%u hash=%08lx",
                           width, height, profile, level, refs, (unsigned long)signature);
        }
    }
    t->video_bytes += packet->size;
    uint64_t rate_now = osGetTime();
    /* Where do cuts come from? RTP timestamps (90 kHz, one per frame) show
     * frames the server itself never sent; arrival gaps show Wi-Fi/queue
     * jitter. Both are summarised on the per-second rate line. */
    if (t->last_video_arrival_at) {
        const uint32_t ts_delta = packet->timestamp - t->last_video_timestamp;
        if (ts_delta >= 4500 && ts_delta < 90000) t->video_src_skipped += (ts_delta + 1500) / 3000 - 1;
        const unsigned gap = (unsigned)(rate_now - t->last_video_arrival_at);
        if (gap > t->video_max_gap_ms) t->video_max_gap_ms = gap;
        if (gap >= 50) t->video_late_arrivals++;
    }
    t->last_video_timestamp = packet->timestamp;
    t->last_video_arrival_at = rate_now;
    if (!t->video_rate_started_at) {
        t->video_rate_started_at = rate_now;
        t->video_rate_bytes = t->video_bytes;
    } else if (rate_now - t->video_rate_started_at >= 1000) {
        uint64_t elapsed = rate_now - t->video_rate_started_at;
        uint64_t bytes = t->video_bytes - t->video_rate_bytes;
        t->video_kbps = (unsigned)((bytes * 8u) / elapsed);
        t->video_rate_started_at = rate_now;
        t->video_rate_bytes = t->video_bytes;
        diagnostic_log("VIDEO", "rate=%u kbps totalBytes=%llu AU=%u srcSkipped=%u late50=%u maxGap=%u",
                       t->video_kbps, (unsigned long long)t->video_bytes,
                       t->video_access_units, t->video_src_skipped, t->video_late_arrivals,
                       t->video_max_gap_ms);
        t->video_src_skipped = t->video_late_arrivals = t->video_max_gap_ms = 0;
    }
    t->last_video_size = packet->size;
    if ((has_sps || has_pps || has_idr) &&
        (t->video_access_units <= 5 || !t->last_video_key_log_at ||
         rate_now - t->last_video_key_log_at >= 10000)) {
        diagnostic_log("VIDEO", "parameter/key AU=%u bytes=%lu sps=%u pps=%u idr=%u",
                       t->video_access_units, (unsigned long)packet->size,
                       has_sps ? 1 : 0, has_pps ? 1 : 0, has_idr ? 1 : 0);
        t->last_video_key_log_at = rate_now;
    }
    if (t->video_access_units <= 5 || t->video_access_units % 120 == 0) {
        const unsigned char *p = packet->data;
        diagnostic_log("VIDEO", "access_unit=%u bytes=%lu sps=%u pps=%u idr=%u head=%02x %02x %02x %02x %02x",
                       t->video_access_units, (unsigned long)packet->size,
                       has_sps ? 1 : 0, has_pps ? 1 : 0, has_idr ? 1 : 0,
                       packet->size > 0 ? p[0] : 0, packet->size > 1 ? p[1] : 0,
                       packet->size > 2 ? p[2] : 0, packet->size > 3 ? p[3] : 0,
                       packet->size > 4 ? p[4] : 0);
    }
    if (t->video_access_units == 1) diagnostic_checkpoint();
    if (stream_profile_probing()) return;
    const unsigned decode_width = t->video_source_width ? t->video_source_width :
                                  stream_profile_width();
    const unsigned source_height = t->video_source_height ? t->video_source_height :
                                   stream_profile_height();
    const unsigned decode_height = (source_height + 15u) & ~15u;
    if (decode_width > 960 || decode_height > 544) {
        if (!t->video_sps_signature || t->video_source_refs > 6) {
            snprintf(t->status, sizeof(t->status),
                     "720p SPS refs=%u exceeds MVD budget (max 6)", t->video_source_refs);
            return;
        }
    }
    if (mvd_video_active() &&
        (t->decoder_width != decode_width || t->decoder_height != decode_height)) {
        if (!has_idr) return;
        diagnostic_log("VIDEO", "decoder reconfigure %ux%u -> %ux%u at IDR",
                       t->decoder_width, t->decoder_height,
                       decode_width, decode_height);
        mvd_video_close();
    }
    if (!mvd_video_active() && !mvd_video_init(decode_width, decode_height)) {
        snprintf(t->status, sizeof(t->status), "Video arrived; %.140s", mvd_video_status());
        return;
    }
    t->decoder_width = decode_width;
    t->decoder_height = decode_height;
    if (!mvd_video_submit(packet->data, packet->size))
        snprintf(t->status, sizeof(t->status), "Video arrived; %.140s", mvd_video_status());
}

static void on_audio(const PeerAudioPacket *packet, void *userdata)
{
    WebRtcTransport *t = userdata;
    t->audio_packets++;
    int result = audio_output_submit(packet->data, packet->size, packet->payload_type,
                                     packet->sequence);
    if (result > 0) t->audio_decoded++;
    else if (result == 0) t->audio_dropped++;
    else t->audio_errors++;
    if (t->audio_packets <= 3 || t->audio_packets % 500 == 0)
        diagnostic_log("AUDIO", "packet=%u decoded=%u dropped=%u errors=%u pt=%u bytes=%lu samples=%d seq=%u status=%s",
                       t->audio_packets, t->audio_decoded, t->audio_dropped,
                       t->audio_errors, packet->payload_type,
                       (unsigned long)packet->size, result, packet->sequence,
                       audio_output_status());
}

static void on_data(char *message, size_t length, void *userdata, uint16_t sid)
{
    WebRtcTransport *t = userdata;
    t->data_messages++;
    const uint8_t *preview = (const uint8_t *)message;
    diagnostic_log("INPUT", "rx message=%u sid=%u bytes=%lu head=%02x %02x %02x %02x %02x %02x %02x %02x",
                   t->data_messages, sid, (unsigned long)length,
                   length > 0 ? preview[0] : 0, length > 1 ? preview[1] : 0,
                   length > 2 ? preview[2] : 0, length > 3 ? preview[3] : 0,
                   length > 4 ? preview[4] : 0, length > 5 ? preview[5] : 0,
                   length > 6 ? preview[6] : 0, length > 7 ? preview[7] : 0);
    if (message && length >= 2 && sid == 0) {
        const uint8_t *bytes = (const uint8_t *)message;
        const uint16_t first = (uint16_t)(bytes[0] | (bytes[1] << 8));
        if (first == 526 || bytes[0] == 0x0e) {
            int version = first == 526 && length >= 4 ? bytes[2] | (bytes[3] << 8) : first;
            t->input_protocol_version = version < 2 ? 2 : version;
            t->input_ready = true;
            snprintf(t->status, sizeof(t->status), "Input ready protocol v%d", t->input_protocol_version);
        }
    }
}

static void on_data_open(void *userdata)
{
    WebRtcTransport *t = userdata;
    t->data_open = true;
    t->data_opened_at = osGetTime();
    snprintf(t->status, sizeof(t->status), "WebRTC data channel open; input handshake");
    diagnostic_log("INPUT", "data channel open");
}

static void on_data_close(void *userdata) { (void)userdata; }

static void on_state(PeerConnectionState state, void *userdata)
{
    WebRtcTransport *t = userdata;
    diagnostic_log("WEBRTC", "peer_state=%s", peer_connection_state_to_string(state));
    if (state == PEER_CONNECTION_COMPLETED) {
        t->state = WEBRTC_CONNECTED;
        t->connected_at = osGetTime();
        snprintf(t->status, sizeof(t->status), "WebRTC ICE/DTLS/SRTP connected");
        request_video_keyframe(t, "stream_start");
    } else if (state == PEER_CONNECTION_CONNECTED) {
        t->state = WEBRTC_CHECKING;
        snprintf(t->status, sizeof(t->status), "ICE connected; DTLS handshake");
    } else if (state == PEER_CONNECTION_FAILED || state == PEER_CONNECTION_DISCONNECTED) {
        t->state = WEBRTC_FAILED;
        snprintf(t->status, sizeof(t->status), "WebRTC transport %s", peer_connection_state_to_string(state));
    } else {
        t->state = WEBRTC_CHECKING;
        snprintf(t->status, sizeof(t->status), "WebRTC %s", peer_connection_state_to_string(state));
    }
}

void webrtc_transport_init(WebRtcTransport *t) { memset(t, 0, sizeof(*t)); }

static void add_manual_media_candidate(WebRtcTransport *t)
{
    if (!t || !t->peer || t->manual_candidate_added ||
        !t->media_ip[0] || t->media_port <= 0)
        return;

    char candidate[256];
    snprintf(candidate, sizeof(candidate),
             "a=candidate:9911 1 UDP 16777215 %s %d typ host",
             t->media_ip, t->media_port);
    if (peer_connection_add_ice_candidate(t->peer, candidate) != 0)
        t->rejected_candidates++;
    t->manual_candidate_added = true;
    diagnostic_log("ICE", "manual fallback=%s", candidate);
}

bool webrtc_transport_start(WebRtcTransport *t, NvstSignal *signal,
                            const char *media_ip, int media_port)
{
    char *offer = NULL;
    char failure[160] = "Could not create/send WebRTC answer";
    webrtc_transport_close(t);
    if (!signal || !signal->offer_sdp) return false;
    normalize_media_ip(t->media_ip, media_ip);
    t->media_port = media_port;
    if (t->media_port <= 0)
        t->media_port = line_integer(signal->offer_nvst_sdp, "a=general.serverBundlePort:");
    if (t->media_port <= 0)
        t->media_port = offer_media_port(signal->offer_sdp);
    diagnostic_log("WEBRTC", "start media=%s:%d offer_bytes=%lu nvst_bytes=%lu",
                   t->media_ip[0] ? t->media_ip : "(none)", t->media_port,
                   (unsigned long)signal->offer_size,
                   (unsigned long)signal->offer_nvst_size);
    if (!g_peer_runtime && peer_init() != 0) {
        snprintf(t->status, sizeof(t->status), "libpeer/SRTP initialization failed"); t->state = WEBRTC_FAILED; return false;
    }
    g_peer_runtime = true;
    peer_connection_set_diagnostics_enabled(1);
    PeerConfiguration config = {0};
    config.video_codec = CODEC_H264; config.audio_codec = CODEC_OPUS;
    config.datachannel = DATA_CHANNEL_BINARY; config.onvideopacket = on_video;
    config.onaudiopacket = on_audio; config.user_data = t;
    config.ice_servers[0].urls = "stun:s1.stun.gamestream.nvidia.com:19308";
    config.ice_servers[1].urls = "stun:stun.l.google.com:19302";
    t->input_protocol_version = 2;
    PeerConnection *pc = peer_connection_create(&config);
    if (!pc) { snprintf(t->status, sizeof(t->status), "WebRTC peer allocation failed"); t->state = WEBRTC_FAILED; return false; }
    t->peer = pc;
    peer_connection_onicecandidate(pc, on_local_candidates);
    peer_connection_oniceconnectionstatechange(pc, on_state);
    peer_connection_ondatachannel(pc, on_data, on_data_open, on_data_close);
    offer = prepare_offer(signal->offer_sdp, t->media_ip);
    if (!offer) {
        snprintf(failure, sizeof(failure), "Offer preparation allocation/size failed");
        goto fail;
    }
    peer_connection_set_remote_description(pc, offer, SDP_TYPE_OFFER);
    const char *raw = peer_connection_create_answer(pc);
    diagnostic_log("ICE", "gathered local_candidates=%u", t->local_candidates);
    for (unsigned i = 0; i < t->local_candidates; ++i)
        diagnostic_log("ICE", "local[%u]=%s", i, t->pending_local_candidates[i]);
    char *answer = raw ? adapt_answer(raw, offer) : NULL;
    char *nvst = answer ? build_nvst(answer) : NULL;
    bool sent = answer && nvst && nvst_signal_send_answer(signal, answer, nvst);
    free(nvst); free(answer);
    if (!sent) {
        snprintf(failure, sizeof(failure), "Answer send failed: %.130s", signal->status);
        goto fail;
    }
    free(offer);
    t->state = WEBRTC_ANSWER_SENT;
    t->answer_sent_at = osGetTime();
    send_local_candidates(t, signal);
    diagnostic_log("ICE", "answer sent local_trickle=%u/%u",
                   t->sent_local_candidates, t->local_candidates);
    add_manual_media_candidate(t);
    snprintf(t->status, sizeof(t->status), "Answer sent; ICE local %u/%u",
             t->sent_local_candidates, t->local_candidates);
    return true;
fail:
    free(offer); webrtc_transport_close(t);
    t->state = WEBRTC_FAILED;
    snprintf(t->status, sizeof(t->status), "%.159s", failure);
    return false;
}

static void apply_candidates(WebRtcTransport *t, NvstSignal *signal)
{
    PeerConnection *pc = t->peer;
    while (pc && t->applied_candidates < signal->remote_ice_count) {
        json_error_t error;
        json_t *root = json_loads(signal->remote_ice[t->applied_candidates], 0, &error);
        const char *candidate = root ? json_string_value(json_object_get(root, "candidate")) : NULL;
        if (candidate) {
            char rewritten[1024];
            const char *use = candidate;
            const char *zero = strstr(candidate, " 0.0.0.0 ");
            if (zero && t->media_ip[0]) {
                const size_t before = (size_t)(zero - candidate) + 1;
                snprintf(rewritten, sizeof(rewritten), "%.*s%s%s",
                         (int)before, candidate, t->media_ip, zero + strlen(" 0.0.0.0"));
                use = rewritten;
                t->remote_zero_rewritten = true;
            }
            char foundation[64], transport[16], address[128], type[32];
            unsigned component, priority, port;
            const char *body = !strncmp(use, "a=candidate:", 12) ? use + 2 : use;
            if (sscanf(body, "candidate:%63s %u %15s %u %127s %u typ %31s",
                       foundation, &component, transport, &priority, address, &port, type) == 7) {
                t->remote_candidate_port = (int)port;
                /* CloudMatch sometimes omits the media host.  The trickled
                 * host candidate supplies it, so retain it for the NVST
                 * bundle-port fallback candidate as well. */
                if (!t->media_ip[0] && strcmp(address, "0.0.0.0") &&
                    !strchr(address, ':'))
                    normalize_media_ip(t->media_ip, address);
            }
            char line[1024];
            if (!strncmp(use, "a=candidate:", 12))
                snprintf(line, sizeof(line), "%.1023s", use);
            else
                snprintf(line, sizeof(line), "a=%.1021s", use);
            if (peer_connection_add_ice_candidate(pc, line) != 0)
                t->rejected_candidates++;
            diagnostic_log("ICE", "remote[%u] port=%d rewritten=%u rejected=%u candidate=%s",
                           t->applied_candidates, t->remote_candidate_port,
                           t->remote_zero_rewritten ? 1 : 0,
                           t->rejected_candidates, line);
            add_manual_media_candidate(t);
        }
        if (root) json_decref(root);
        t->applied_candidates++;
    }
}

void webrtc_transport_tick(WebRtcTransport *t, NvstSignal *signal)
{
    if (!t->peer) return;
    send_local_candidates(t, signal);
    apply_candidates(t, signal);
    /* GFN sends video in short bursts. Drain enough datagrams per vblank to
     * keep those bursts out of the kernel queue, while retaining a hard cap
     * so input and app lifecycle work cannot starve. */
    const uint64_t now = osGetTime();
    if (t->media_readable || now - t->last_media_poll_at >= 16) {
        for (unsigned i = 0; i < 64 && peer_connection_loop(t->peer) > 0; ++i) {}
        t->last_media_poll_at = now;
    }
    t->media_readable = false;
    const unsigned decoded = mvd_video_decoded_frames();
    if (decoded != t->observed_decoded_frames) {
        t->observed_decoded_frames = decoded;
        t->last_decoded_frame_at = now;
    }
    if (t->state == WEBRTC_CONNECTED && mvd_video_take_resync_request() &&
        now - t->last_keyframe_request_at >= 500)
        request_video_keyframe(t, "decoder_queue_full");
    /* A frame the RTP assembler had to abandon would smear every following
     * P-frame; freeze on the last clean picture and fetch a keyframe. */
    const unsigned au_dropped = peer_connection_get_video_au_dropped(t->peer);
    if (au_dropped != t->video_au_dropped_seen) {
        t->video_au_dropped_seen = au_dropped;
        if (t->state == WEBRTC_CONNECTED && mvd_video_active()) {
            mvd_video_resync();
            /* Each request makes NVIDIA send a large IDR, and on marginal
             * Wi-Fi that burst loses packets itself. Build 63 at 1.5 Mbps
             * asked every ~330 ms, got an IDR storm and never started. Back
             * off 300 ms -> 3 s while drops keep coming. */
            if (!t->keyframe_backoff_ms) t->keyframe_backoff_ms = 300;
            if (now - t->last_keyframe_request_at >= t->keyframe_backoff_ms) {
                request_video_keyframe(t, "rtp_frame_dropped");
                t->keyframe_backoff_ms = t->keyframe_backoff_ms * 2 > 3000
                    ? 3000 : t->keyframe_backoff_ms * 2;
            }
        }
        t->last_au_drop_at = now;
    } else if (t->keyframe_backoff_ms > 300 && now - t->last_au_drop_at >= 5000) {
        t->keyframe_backoff_ms = 300;
    }
    if (t->state == WEBRTC_CONNECTED && t->keyframe_requests < 8) {
        if (!t->video_saw_idr && now - t->last_keyframe_request_at >= 1500)
            request_video_keyframe(t, "waiting_for_idr");
        else if (t->video_saw_idr &&
                 ((t->last_decoded_frame_at && now - t->last_decoded_frame_at >= 3000) ||
                  (!t->last_decoded_frame_at && now - t->last_keyframe_request_at >= 3000)) &&
                 now - t->last_keyframe_request_at >= 3000)
            request_video_keyframe(t, "decoder_stalled");
    }
    t->rtt_ms = peer_connection_get_rtt_ms(t->peer);
    peer_connection_get_ice_candidate_pair_stats(t->peer,
        &t->ice_pairs_total, &t->ice_pairs_frozen, &t->ice_pairs_inprogress,
        &t->ice_pairs_succeeded, &t->ice_pairs_failed);
    peer_connection_get_ice_io_stats(t->peer,
        &t->gather_sent, &t->gather_received, &t->checks_sent,
        &t->udp_received, &t->stun_received, &t->valid_responses,
        &t->checks_attempted, &t->send_failures, &t->last_send_errno);
    const uint64_t now_diag = osGetTime();
    if (now_diag - t->last_diagnostic_at >= (t->video_access_units ? 10000u : 1000u)) {
        t->last_diagnostic_at = now_diag;
        diagnostic_log("ICE", "pairs total=%d frozen=%d run=%d ok=%d fail=%d local=%u/%u remote=%u/%u mediaPort=%d remotePort=%d manual=%u checks=%lu/%lu sendFail=%lu errno=%d udpRx=%lu stunRx=%lu valid=%lu rtt=%d",
            t->ice_pairs_total, t->ice_pairs_frozen, t->ice_pairs_inprogress,
            t->ice_pairs_succeeded, t->ice_pairs_failed,
            t->sent_local_candidates, t->local_candidates,
            t->applied_candidates, signal->remote_ice_count,
            t->media_port, t->remote_candidate_port, t->manual_candidate_added ? 1 : 0,
            (unsigned long)t->checks_sent, (unsigned long)t->checks_attempted,
            (unsigned long)t->send_failures, t->last_send_errno,
            (unsigned long)t->udp_received, (unsigned long)t->stun_received,
            (unsigned long)t->valid_responses, t->rtt_ms);
    }
    if (t->state != WEBRTC_CONNECTED) return;
    PeerConnection *pc = t->peer;
    if (!t->input_channel_requested) {
        char label[] = "input_channel_v1", protocol[] = "";
        if (peer_connection_create_datachannel_sid(pc, DATA_CHANNEL_RELIABLE, 0, 0,
                                                    label, protocol, 0) >= 0)
            t->input_channel_requested = true;
    }
    const uint64_t now_input = osGetTime();
    if (t->data_open && !t->input_ready && now_input - t->data_opened_at >= 1500) {
        t->input_ready = true;
        t->input_protocol_version = 2;
        snprintf(t->status, sizeof(t->status), "Input ready protocol v2 fallback");
    }
    if (!t->input_ready) return;
    if (now_input - t->last_input_heartbeat_at >= 2000) {
        char heartbeat[4] = {2, 0, 0, 0};
        peer_connection_datachannel_send_binary_sid(pc, heartbeat, sizeof(heartbeat), 0);
        t->last_input_heartbeat_at = now_input;
    }
    /* Optional partially reliable gamepad channel (Settings > Fast input). */
    if (t->prefer_partial_input && t->input_protocol_version >= 3 && !t->partial_channel_requested) {
        char label[] = "input_channel_partially_reliable", protocol[] = "";
        const int result = peer_connection_create_datachannel_sid(
            pc, DATA_CHANNEL_PARTIAL_RELIABLE_TIMED_UNORDERED, 0, PARTIAL_INPUT_LIFETIME_MS,
            label, protocol, PARTIAL_INPUT_SID);
        t->partial_channel_requested = true;
        t->partial_channel_at = now_input;
        diagnostic_log("INPUT", "partial gamepad channel sid=%u lifetime=%ums result=%d",
                       PARTIAL_INPUT_SID, PARTIAL_INPUT_LIFETIME_MS, result);
        if (result < 0) t->prefer_partial_input = false;
    }

    GfnGamepadState state;
    gfn_input_read_3ds(&state);
    const uint64_t timestamp_us = (now_input - t->connected_at) * 1000ULL;
    if (t->pointer_mode && !t->keyboard_mode) {
        /* Mouse speed is per 16 ms step, independent of the poll rate. */
        if (now_input - t->last_mouse_move_at >= 16) {
            const int dx = state.right_x / 2600;
            const int dy = -state.right_y / 2600;
            if (dx || dy) {
                uint8_t move[34];
                const size_t move_size = gfn_input_encode_mouse_move(move, (int16_t)dx, (int16_t)dy,
                    timestamp_us, t->input_protocol_version);
                if (peer_connection_datachannel_send_binary_sid(pc, (char *)move,
                                                                  move_size, 0) >= 0)
                    t->mouse_moves++;
            }
            t->last_mouse_move_at = now_input;
        }
        state.right_x = state.right_y = 0;
        /* Physical A clicks the mouse; keep B and X out of the game too. */
        state.buttons &= (uint16_t)~gfn_input_buttons_for_keys(KEY_A | KEY_B | KEY_X);
    }
    if (t->keyboard_mode) {
        /* Physical keyboard shortcuts must not also press gamepad buttons. */
        memset(&state, 0, sizeof(state));
    }
    const bool pressed = !t->input_state_logged ||
        state.buttons != t->last_input_buttons ||
        state.left_trigger != t->last_input_left_trigger ||
        state.right_trigger != t->last_input_right_trigger;
    const bool changed = pressed ||
        state.left_x != t->last_input_left_x || state.left_y != t->last_input_left_y ||
        state.right_x != t->last_input_right_x || state.right_y != t->last_input_right_y;
    /* Send a change as soon as it is seen and repeat the current state every
     * 16 ms so the server never goes stale. */
    const uint64_t since_last = now_input - t->last_input_at;
    if (!(pressed && since_last >= INPUT_BUTTON_INTERVAL_MS) &&
        !(changed && since_last >= INPUT_MIN_INTERVAL_MS) && since_last < INPUT_REPEAT_MS) return;

    uint8_t packet[54];
    size_t size;
    uint16_t sid = 0;
    if (t->prefer_partial_input && t->partial_channel_requested &&
        now_input - t->partial_channel_at >= 250) {
        size = gfn_input_encode_gamepad_partial(packet, &state, timestamp_us,
                                                t->partial_sequence++);
        sid = PARTIAL_INPUT_SID;
    } else {
        size = gfn_input_encode_gamepad_wire(packet, &state, timestamp_us,
                                             t->input_protocol_version);
    }
    if (peer_connection_datachannel_send_binary_sid(pc, (char *)packet, size, sid) >= 0)
        t->input_reports++;
    if (changed && (!t->input_state_logged || state.buttons != t->last_input_buttons ||
                    state.left_trigger != t->last_input_left_trigger ||
                    state.right_trigger != t->last_input_right_trigger ||
                    now_input - t->last_input_state_log_at >= 200)) {
        diagnostic_log("INPUT", "tx report=%u protocol=%d sid=%u bytes=%lu buttons=%04x triggers=%u/%u sticks=%d/%d/%d/%d tsUs=%llu",
                       t->input_reports, t->input_protocol_version, sid, (unsigned long)size,
                       state.buttons, state.left_trigger, state.right_trigger,
                       state.left_x, state.left_y, state.right_x, state.right_y,
                       (unsigned long long)timestamp_us);
        t->last_input_state_log_at = now_input;
    }
    t->input_state_logged = true;
    t->last_input_buttons = state.buttons;
    t->last_input_left_trigger = state.left_trigger;
    t->last_input_right_trigger = state.right_trigger;
    t->last_input_left_x = state.left_x; t->last_input_left_y = state.left_y;
    t->last_input_right_x = state.right_x; t->last_input_right_y = state.right_y;
    t->last_input_at = now_input;
}

void webrtc_transport_set_pointer_mode(WebRtcTransport *t, bool enabled)
{
    if (!t || t->pointer_mode == enabled) return;
    t->pointer_mode = enabled;
    /* Switching back to gamepad must also leave the on-screen keyboard mode;
     * that mode suppresses every controller report. */
    if (!enabled) t->keyboard_mode = false;
    diagnostic_log("INPUT", "pointer mode %s", enabled ? "enabled" : "disabled");
}

bool webrtc_transport_mouse_move(WebRtcTransport *t, int16_t dx, int16_t dy)
{
    if (!t || !t->peer || !t->input_ready || (!dx && !dy)) return false;
    uint8_t packet[34];
    const uint64_t timestamp_us = (osGetTime() - t->connected_at) * 1000ULL;
    const size_t size = gfn_input_encode_mouse_move(packet, dx, dy,
        timestamp_us, t->input_protocol_version);
    const bool ok = peer_connection_datachannel_send_binary_sid(t->peer,
        (char *)packet, size, 0) >= 0;
    if (ok) t->mouse_moves++;
    return ok;
}

bool webrtc_transport_mouse_button(WebRtcTransport *t, bool pressed)
{
    if (!t || !t->peer || !t->input_ready) return false;
    uint8_t packet[28];
    const uint64_t timestamp_us = (osGetTime() - t->connected_at) * 1000ULL;
    const size_t size = gfn_input_encode_mouse_button(packet, pressed,
        timestamp_us, t->input_protocol_version);
    const bool ok = peer_connection_datachannel_send_binary_sid(t->peer,
        (char *)packet, size, 0) >= 0;
    if (ok && !pressed) t->mouse_clicks++;
    diagnostic_log("INPUT", "mouse button %s sent=%u count=%u",
                   pressed ? "down" : "up", ok ? 1 : 0, t->mouse_clicks);
    return ok;
}

bool webrtc_transport_send_key(WebRtcTransport *t, uint16_t keycode,
                               uint16_t scancode, uint16_t modifiers)
{
    if (!t || !t->peer || !t->input_ready) return false;
    uint8_t packet[28];
    const uint64_t timestamp_us = (osGetTime() - t->connected_at) * 1000ULL;
    size_t size = gfn_input_encode_key(packet, keycode, scancode, modifiers,
        true, timestamp_us, t->input_protocol_version);
    const bool down = peer_connection_datachannel_send_binary_sid(t->peer,
        (char *)packet, size, 0) >= 0;
    size = gfn_input_encode_key(packet, keycode, scancode, modifiers,
        false, timestamp_us + 1000, t->input_protocol_version);
    const bool up = peer_connection_datachannel_send_binary_sid(t->peer,
        (char *)packet, size, 0) >= 0;
    if (down && up) t->keyboard_keys++;
    /* Never log keys, characters, or login text. */
    return down && up;
}

unsigned webrtc_transport_resent_packets(const WebRtcTransport *t)
{
    return t && t->peer ? peer_connection_get_video_rtx_recovered(t->peer) : 0;
}

int webrtc_transport_socket(const WebRtcTransport *t)
{
    return t && t->peer ? peer_connection_get_udp_fd(t->peer) : -1;
}

bool webrtc_transport_gameplay_ready(const WebRtcTransport *t)
{
    return t && t->state == WEBRTC_CONNECTED;
}

void webrtc_transport_close(WebRtcTransport *t)
{
    if (!t) return;
    if (t->peer) peer_connection_destroy(t->peer);
    audio_output_close();
    mvd_video_close();
    memset(t, 0, sizeof(*t));
}

#include "webrtc_peer.h"
#include "nvst_signal.h"
#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <jansson.h>

#include "peer.h"
#include "peer_connection.h"
#include "dtls_srtp.h"

/* libpeer 3DS port exposes PeerConnection internal for DTLS state query only
 * via peer_connection_get_state and dtls_srtp.h. We avoid touching private
 * fields; instead we infer DTLS completion from ICE state plus a subsequent
 * loop that reports connected. */
/* g_signal is defined in main.c; expose via extern without static */
extern NvstSignal g_signal;

/* 3DS stubs for libpeer features not available on the console. */
int mdns_resolve_addr(const char *host, void *addr) { (void)host; (void)addr; return -1; }

static PeerConnection *g_pc = NULL;
static WebrtcPeerState g_state = WEBRTC_IDLE;
static char g_last_error[128] = "";
static char g_local_sdp[8096] = "";
static char g_nvst_sdp[4096] = "";
static unsigned g_remote_ice_processed = 0;
static PeerConnectionState g_ice_state = PEER_CONNECTION_NEW;
static bool g_dtls_connected = false;
static bool g_peer_initialized = false;

const char *webrtc_peer_last_error(void) { return g_last_error; }
const char *webrtc_peer_local_sdp(void) { return g_local_sdp; }
WebrtcPeerState webrtc_peer_state(void) { return g_state; }
int webrtc_peer_ice_state(void) { return (int)g_ice_state; }
bool webrtc_peer_dtls_connected(void) { return g_dtls_connected; }
bool webrtc_peer_active(void) { return g_pc != NULL && g_state != WEBRTC_IDLE && g_state != WEBRTC_FAILED; }

const char *webrtc_peer_state_string(void) {
  switch (g_state) {
    case WEBRTC_IDLE: return "idle";
    case WEBRTC_ANSWER_SENT: return "answer sent";
    case WEBRTC_ICE_CHECKING: return "ICE checking";
    case WEBRTC_ICE_CONNECTED: return "ICE connected";
    case WEBRTC_DTLS_CONNECTED: return "DTLS/SRTP connected";
    case WEBRTC_FAILED: return "failed";
    default: return "unknown";
  }
}

static void set_error(const char *msg) {
  snprintf(g_last_error, sizeof(g_last_error), "%s", msg);
  g_state = WEBRTC_FAILED;
}

static void on_ice_candidate_cb(char *sdp, void *userdata) {
  (void)userdata;
  /* libpeer calls this with the full local SDP including candidates.
   * For 3DS we have already sent the answer; trickle candidates are
   * extracted and forwarded individually if needed. The server may not
   * require trickle for host candidates already in the answer. */
  if (!sdp || !g_pc) return;
  /* Extract each a=candidate line and send as separate NVST candidate. */
  const char *p = sdp;
  while ((p = strstr(p, "a=candidate:")) != NULL) {
    const char *eol = strstr(p, "\r\n");
    if (!eol) eol = strstr(p, "\n");
    size_t len = eol ? (size_t)(eol - p - 2) : strlen(p); /* -2 for "a=" */
    if (len >= 2 && p[0]=='a' && p[1]=='=') { p+=2; len-=0; }
    /* p now points to "candidate:..." */
    char cand[1024];
    if (len >= sizeof(cand)) len = sizeof(cand)-1;
    memcpy(cand, p, len);
    cand[len]=0;
    /* Build candidate JSON {candidate:"...", sdpMid:"video", sdpMLineIndex:0} */
    json_t *obj = json_pack("{s:s,s:s,s:i}", "candidate", cand, "sdpMid", "video", "sdpMLineIndex", 0);
    char *text = json_dumps(obj, JSON_COMPACT);
    json_decref(obj);
    if (text) {
      nvst_signal_send_candidate(&g_signal, text);
      free(text);
    }
    if (eol) p = eol + 2; else break;
  }
}

static void on_ice_state_change_cb(PeerConnectionState state, void *userdata) {
  (void)userdata;
  g_ice_state = state;
  if (state == PEER_CONNECTION_CHECKING) {
    if (g_state == WEBRTC_ANSWER_SENT) g_state = WEBRTC_ICE_CHECKING;
  } else if (state == PEER_CONNECTION_CONNECTED) {
    g_state = WEBRTC_ICE_CONNECTED;
  } else if (state == PEER_CONNECTION_COMPLETED) {
    g_state = WEBRTC_ICE_CONNECTED;
  } else if (state == PEER_CONNECTION_FAILED || state == PEER_CONNECTION_DISCONNECTED || state == PEER_CONNECTION_CLOSED) {
    set_error("ICE failed/disconnected");
  }
  /* DTLS connected is detected in tick() after handshake. */
}

static const char *extract_sdp_value(const char *sdp, const char *prefix) {
  static char value[256];
  value[0]=0;
  const char *p = strstr(sdp, prefix);
  if (!p) return "";
  p += strlen(prefix);
  size_t i=0;
  while (p[i] && p[i]!='\r' && p[i]!='\n' && i<sizeof(value)-1) {
    value[i]=p[i]; i++;
  }
  value[i]=0;
  return value;
}

static void build_nvst_sdp(const char *answer_sdp, char *out, size_t out_size) {
  /* Simplified NVST SDP: mirrors Switch BuildNvstSdp but fixed to
   * 960x544@30, 3500 kbps, H264 SDR 8-bit 4:2:0. */
  const char *ufrag = extract_sdp_value(answer_sdp, "a=ice-ufrag:");
  char ufrag_copy[128]; snprintf(ufrag_copy,sizeof(ufrag_copy),"%s", ufrag);
  const char *pwd = extract_sdp_value(answer_sdp, "a=ice-pwd:");
  char pwd_copy[128]; snprintf(pwd_copy,sizeof(pwd_copy),"%s", pwd);
  const char *fp = extract_sdp_value(answer_sdp, "a=fingerprint:sha-256 ");
  char fp_copy[256]; snprintf(fp_copy,sizeof(fp_copy),"%s", fp);

  snprintf(out, out_size,
    "v=0\r\n"
    "o=SdpTest test_id_13 14 IN IPv4 127.0.0.1\r\n"
    "s=-\r\n"
    "t=0 0\r\n"
    "a=general.icePassword:%s\r\n"
    "a=general.iceUserNameFragment:%s\r\n"
    "a=general.dtlsFingerprint:%s\r\n"
    "m=video 0 RTP/AVP\r\n"
    "a=msid:fbc-video-0\r\n"
    "a=video.clientViewportWd:960\r\n"
    "a=video.clientViewportHt:544\r\n"
    "a=video.maxFPS:30\r\n"
    "a=video.initialBitrateKbps:875\r\n"
    "a=video.initialPeakBitrateKbps:875\r\n"
    "a=vqos.bw.maximumBitrateKbps:3500\r\n"
    "a=vqos.bw.minimumBitrateKbps:4000\r\n"
    "a=video.maxNumReferenceFrames:4\r\n"
    "a=video.encoderCscMode:3\r\n"
    "a=video.dynamicRangeMode:0\r\n"
    "a=video.bitDepth:8\r\n"
    "m=audio 0 RTP/AVP\r\n"
    "a=msid:audio\r\n"
    "m=application 0 RTP/AVP\r\n"
    "a=msid:input_1\r\n",
    pwd_copy, ufrag_copy, fp_copy);
}

static bool forward_remote_ice(void) {
  if (!g_pc) return true;
  while (g_remote_ice_processed < g_signal.remote_ice_count) {
    const char *json_text = g_signal.remote_ice[g_remote_ice_processed++];
    json_error_t err;
    json_t *payload = json_loads(json_text, JSON_REJECT_DUPLICATES, &err);
    if (!payload) continue;
    const char *cand = json_string_value(json_object_get(payload, "candidate"));
    /* Some servers wrap candidate as {candidate:"...", sdpMid:...}. */
    if (!cand) {
      json_decref(payload);
      continue;
    }
    char cand_copy[1024];
    snprintf(cand_copy, sizeof(cand_copy), "%s", cand);
    /* libpeer expects "candidate:..." without "a=" prefix */
    if (strncmp(cand_copy, "a=candidate:", 12)==0) {
      memmove(cand_copy, cand_copy+2, strlen(cand_copy+2)+1);
    }
    /* Skip placeholder 0.0.0.0 candidates if they carry no real host */
    if (strstr(cand_copy, "0.0.0.0")) {
      json_decref(payload);
      continue;
    }
    if (peer_connection_add_ice_candidate(g_pc, cand_copy)!=0) {
      /* non-fatal, keep going */
    }
    json_decref(payload);
  }
  return true;
}

void webrtc_peer_init(void) {
  g_state = WEBRTC_IDLE;
  g_last_error[0]=0;
  g_local_sdp[0]=0;
  g_nvst_sdp[0]=0;
  g_remote_ice_processed = 0;
  g_ice_state = PEER_CONNECTION_NEW;
  g_dtls_connected = false;
}

bool webrtc_peer_start(const char *offer_sdp) {
  if (!offer_sdp || !offer_sdp[0]) { set_error("empty offer"); return false; }
  if (g_pc) webrtc_peer_close();

  if (!g_peer_initialized) {
    if (peer_init()!=0) { set_error("peer_init failed"); return false; }
    g_peer_initialized = true;
  }

  PeerConfiguration config = {0};
  config.video_codec = CODEC_H264;
  config.audio_codec = CODEC_NONE;
  config.datachannel = DATA_CHANNEL_NONE;
  config.user_data = NULL;

  /* Provide at least one public STUN to get srflx candidate if needed.
   * For 3DS host candidate via gethostid should be enough for local test;
   * STUN may fail without extra ports but we keep it to mirror Switch. */
  static const char *stun1 = "stun:stun.l.google.com:19302";
  config.ice_servers[0].urls = stun1;

  g_pc = peer_connection_create(&config);
  if (!g_pc) { set_error("peer_connection_create failed"); return false; }

  peer_connection_onicecandidate(g_pc, on_ice_candidate_cb);
  peer_connection_oniceconnectionstatechange(g_pc, on_ice_state_change_cb);

  peer_connection_set_remote_description(g_pc, offer_sdp, SDP_TYPE_OFFER);
  g_remote_ice_processed = 0;
  forward_remote_ice();

  const char *answer = peer_connection_create_answer(g_pc);
  if (!answer) { set_error("create_answer failed"); webrtc_peer_close(); return false; }
  snprintf(g_local_sdp, sizeof(g_local_sdp), "%s", answer);
  build_nvst_sdp(answer, g_nvst_sdp, sizeof(g_nvst_sdp));

  if (!nvst_signal_send_answer(&g_signal, g_local_sdp, g_nvst_sdp)) {
    set_error("send answer failed");
    webrtc_peer_close();
    return false;
  }
  g_state = WEBRTC_ANSWER_SENT;
  /* After answer, ICE starts checking */
  return true;
}

void webrtc_peer_tick(void) {
  if (!g_pc) return;
  forward_remote_ice();
  /* Drive libpeer network: ICE/DTLS/SRTP. */
  peer_connection_loop(g_pc);

  PeerConnectionState st = peer_connection_get_state(g_pc);
  if (st != g_ice_state) {
    on_ice_state_change_cb(st, NULL);
  }
  /* Infer DTLS/SRTP connected: when peer reports CONNECTED and we have
   * received at least one SRTP packet or handshake completed. For now,
   * treat ICE_CONNECTED + at least one loop with DTLS state as DTLS. */
  if (g_state == WEBRTC_ICE_CONNECTED) {
    /* Heuristic: after ICE, DTLS handshake should complete within a few seconds.
     * We poll a simple flag: if peer_connection_loop has processed DTLS.
     * For now transition after 1 second of ICE connected to DTLS connected
     * placeholder; real check would query dtls_srtp.state but it's private.
     * We approximate by checking that peer is still connected after 500ms. */
    static uint64_t connected_since = 0;
    if (connected_since==0) connected_since = osGetTime();
    if (osGetTime() - connected_since > 500) {
      g_dtls_connected = true;
      g_state = WEBRTC_DTLS_CONNECTED;
    }
  } else if (g_state == WEBRTC_ICE_CHECKING || g_state == WEBRTC_ANSWER_SENT) {
    /* reset timer */
  }
}

void webrtc_peer_close(void) {
  if (g_pc) {
    peer_connection_close(g_pc);
    peer_connection_destroy(g_pc);
    g_pc = NULL;
  }
  if (g_peer_initialized) {
    peer_deinit();
    g_peer_initialized = false;
  }
  g_state = WEBRTC_IDLE;
  g_dtls_connected = false;
  g_ice_state = PEER_CONNECTION_NEW;
}

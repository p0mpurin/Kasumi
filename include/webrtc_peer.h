#pragma once
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  WEBRTC_IDLE = 0,
  WEBRTC_ANSWER_SENT,
  WEBRTC_ICE_CHECKING,
  WEBRTC_ICE_CONNECTED,
  WEBRTC_DTLS_CONNECTED,
  WEBRTC_FAILED
} WebrtcPeerState;

void webrtc_peer_init(void);
bool webrtc_peer_start(const char *offer_sdp);
void webrtc_peer_tick(void);
void webrtc_peer_close(void);
bool webrtc_peer_active(void);
WebrtcPeerState webrtc_peer_state(void);
const char *webrtc_peer_state_string(void);
int webrtc_peer_ice_state(void);
bool webrtc_peer_dtls_connected(void);
const char *webrtc_peer_last_error(void);
const char *webrtc_peer_local_sdp(void);

#ifdef __cplusplus
}
#endif

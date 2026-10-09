#pragma once

/* Xbox Cloud Gaming inside a running stream: the four data channels the
 * console opens (chat, control, input, message), the message channel's
 * handshake and client description, and controller reports in xCloud's
 * input format (report types: 1 frame metadata, 2 gamepad, 8 client
 * metadata). Called by webrtc_transport.c under its peer lock. */

#include <stddef.h>
#include <stdint.h>

#include "webrtc_transport.h"

/* The picture asked of the service (the top screen in wide mode). */
#define XCLOUD_STREAM_WIDTH 800
#define XCLOUD_STREAM_HEIGHT 480

void xcloud_stream_reset(void);
/* A video frame arrived (its RTP timestamp keys the frame metadata the
 * service uses to measure latency). */
void xcloud_stream_note_frame(uint32_t rtp_timestamp);
/* A data channel message. Only notes what to answer: sending from inside
 * the SCTP receive path would overwrite the acknowledgement being built. */
void xcloud_stream_on_data(WebRtcTransport *t, const char *data, size_t length, uint16_t sid);
/* Opens the channels, answers, and sends controller state (UI thread). */
void xcloud_stream_tick(WebRtcTransport *t, void *peer);

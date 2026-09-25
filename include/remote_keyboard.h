#pragma once

/* Bottom-screen keyboard that types into the remote game over the input
 * data channel. Nothing typed is stored or logged. */

#include <3ds.h>
#include <stdbool.h>

#include "webrtc_transport.h"

void remote_keyboard_open(void);
void remote_keyboard_draw(const WebRtcTransport *transport, bool touching, int touch_x, int touch_y);
/* Both return true when the keyboard asked to close. */
bool remote_keyboard_touch(WebRtcTransport *transport, int x, int y);
bool remote_keyboard_buttons(WebRtcTransport *transport, u32 down);

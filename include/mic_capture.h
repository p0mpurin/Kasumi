#pragma once

/* Voice chat: the 3DS microphone, encoded as Opus on its own thread and
 * sent to the game on the session's mic track (webrtc_transport_send_mic).
 * It starts muted; nothing is recorded or stored, and nothing is sent until
 * the player unmutes (silence frames keep the track's timing meanwhile). */

#include <stdbool.h>

/* Starts capture (needs the mic:u service); false if the mic is unusable. */
bool mic_capture_start(void);
void mic_capture_stop(void);
bool mic_capture_running(void);
void mic_capture_set_muted(bool muted);
bool mic_capture_muted(void);
/* Recent loudness 0..1 while unmuted, for the MIC button's meter. */
float mic_capture_level(void);

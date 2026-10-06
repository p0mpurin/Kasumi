#pragma once

/* Echo cancellation for voice chat: the game sound coming out of the 3DS
 * speakers, a few centimetres from the mic, is taken back out of what the
 * mic hears, so teammates don't hear the game (or themselves) through it.
 * WebRTC's mobile echo canceller (AECM, vendor/webrtc-aecm), set up the way
 * TriCord (the 3DS Discord client) runs it: 16 kHz, 10 ms blocks. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* 10 ms at 16 kHz: the mic frame, and the size AECM takes. */
#define ECHO_CANCEL_BLOCK 160

bool echo_cancel_start(void);
void echo_cancel_stop(void);
/* Game sound as it goes to the speakers: 48 kHz stereo, interleaved. */
void echo_cancel_far_end(const int16_t *pcm, size_t frames);
/* One mic block (ECHO_CANCEL_BLOCK samples at 16 kHz), cleaned in place. */
void echo_cancel_process(int16_t *mic);

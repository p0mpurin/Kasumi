#pragma once

/* Type from a phone: a small web page on the home Wi-Fi (port 8080, behind
 * a random path that only the QR code shows) whose text goes to the game
 * as key presses. It runs while a game streams. Nothing typed is stored or
 * logged. */

#include <stdbool.h>

#include "webrtc_transport.h"

/* False when there is no network to serve on. */
bool phone_keyboard_start(void);
void phone_keyboard_stop(void);
bool phone_keyboard_running(void);
/* Answers the phone and sends what it typed (every frame while running). */
void phone_keyboard_tick(WebRtcTransport *transport);
const char *phone_keyboard_url(void);
/* The URL's QR code: size x size modules, row by row, 1 = dark (0: none). */
int phone_keyboard_qr(const unsigned char **modules);
/* A phone has used the page in the last two minutes. */
bool phone_keyboard_connected(void);

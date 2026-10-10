#pragma once

#include <stdbool.h>

/* What each Wi-Fi network delivered last time, kept on the SD card. Beta.18
 * stats: 15 of 54 sessions were choppy (mostly far from the server), yet only
 * 3 sessions used Weak / hotspot. When the last Standard session on this
 * network was choppy, the next one starts in Weak / hotspot by itself. */

/* The last Standard session on this network (within two weeks) was choppy. */
/* Read the file into memory (startup, before the background writer runs). */
void net_memory_load(void);
bool net_memory_choppy_here(void);
/* After a session: how it went on this network. weak: the player chose
 * Weak / hotspot (says nothing about Standard, so it is not recorded). */
void net_memory_note(bool weak, unsigned seconds, unsigned lost, unsigned repeated);
/* A session clearly choppy enough for Weak / hotspot (the tip after a game
 * and the switch next time use the same test). */
bool net_memory_session_choppy(unsigned seconds, unsigned lost, unsigned repeated);

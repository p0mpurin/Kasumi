#pragma once

#include <stdbool.h>

/* Menu sound effects (tools/audio/sfx.py: koto, wood, rin, furin, water).
 * Decoded once at start-up into sound memory and played on channels
 * 11-13 in turn, so a quick run of presses overlaps instead of cutting off. */
typedef enum {
    SFX_MOVE,
    SFX_BUMP,
    SFX_SELECT,
    SFX_BACK,
    SFX_OPEN,
    SFX_CLOSE,
    SFX_TOGGLE_ON,
    SFX_TOGGLE_OFF,
    SFX_TAB,
    SFX_NOTICE,
    SFX_ERROR,
    SFX_SCREENSHOT,
    SFX_QUEUE_READY,
    SFX_COUNT
} Sfx;

/* After audio_system_init(); does nothing without the DSP. */
void sfx_init(void);
void sfx_set_enabled(bool enabled);
/* Main thread. Skipped while sound effects are off. */
void sfx_play(Sfx sfx);
/* Plays even with sound effects off (the queue's "rig ready" alert); false
 * when it could not play. */
bool sfx_play_always(Sfx sfx);
void sfx_exit(void);

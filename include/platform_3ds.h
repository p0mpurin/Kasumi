#pragma once

/* 3DS types behind platform.h. Only the type headers are pulled in, so a
 * shared file that still calls libctru directly fails to build. */

#include <3ds/types.h>
#include <3ds/synchronization.h>
#include <3ds/thread.h>

typedef LightLock PlatLock;
#define PLAT_LOCK_INIT 1
typedef RecursiveLock PlatRecursiveLock;
typedef LightEvent PlatEvent;
typedef Thread PlatThread;

/* SYSCLOCK_ARM11: the ARM11 runs its tick counter at 268,111,856 Hz. */
#define PLAT_TICKS_PER_SEC 268111856
#define PLAT_PRIO_LOWEST 0x3F
/* In the startup log (only New 3DS models get that far). */
#define PLAT_MODEL_FAMILY "new3ds-family"

/* The Circle Pad reaches about 150 at its rim; the C-Stick is shorter. */
#define PLAT_LSTICK_REACH 150
#define PLAT_RSTICK_REACH 140
/* The bottom screen. */
#define PLAT_TOUCH_WIDTH 320
#define PLAT_TOUCH_HEIGHT 240
/* Touch distances (a stick's rim, a jump) are tuned on this screen. */
#define PLAT_TOUCH_SCALE 1.0f

/* Room for the DSP's wave buffer (ndspWaveBuf, size checked in
 * source/3ds/audio.c), so shared code needs no ndsp header. */
typedef struct {
    u32 storage[8];
} PlatAudioVoice;
/* The microphone records at 16360 Hz. */
#define PLAT_MIC_RATE 16360.0f

/* What this console has, for the menus. */
#define PLAT_HAS_3D_SLIDER 1
#define PLAT_HAS_LID 1
#define PLAT_HAS_HOME_THEME 1
/* Wide (800-column) or classic video, and the GPU picture filter. */
#define PLAT_HAS_WIDE_MODE 1
#define PLAT_HAS_VIDEO_FILTER 1
/* Face buttons are lettered A B X Y (not PlayStation shapes). */
#define PLAT_PS_BUTTONS 0
/* No buttons to add elsewhere (plat_input_set_extra_buttons). */
#define PLAT_HAS_EXTRA_BUTTONS 0

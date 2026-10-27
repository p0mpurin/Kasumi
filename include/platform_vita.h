#pragma once

/* PS Vita types behind platform.h. The kernel headers stay out of shared
 * code: locks and events carry their own storage and are set up on first
 * use, so a static PlatLock needs no init call (as on the 3DS). */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;

/* A kernel lightweight mutex (SceKernelLwMutexWork, 32 bytes), made the
 * first time it is locked. */
typedef struct {
    volatile int state;
    int recursive;
    u64 work[4];
} PlatLock;
#define PLAT_LOCK_INIT { 0, 0, { 0, 0, 0, 0 } }
typedef PlatLock PlatRecursiveLock;
/* A kernel event flag. */
typedef struct {
    int id;
    bool sticky;
} PlatEvent;
typedef struct PlatThreadImpl *PlatThread;

/* sceKernelGetProcessTimeWide counts microseconds. */
#define PLAT_TICKS_PER_SEC 1000000
/* Priorities follow the 3DS's numbers (0x30 is the main thread). */
#define PLAT_PRIO_LOWEST 0x3F
#define PLAT_MODEL_FAMILY "vita"

/* Sticks read about +-127 at the rim (some never quite reach it). */
#define PLAT_LSTICK_REACH 120
#define PLAT_RSTICK_REACH 120
/* The front touch screen. */
#define PLAT_TOUCH_WIDTH 960
#define PLAT_TOUCH_HEIGHT 544
/* Its pixels are finer than the 3DS lower screen's (about 8.7 against 5.2
 * a millimetre): touch distances tuned there grow by this much. */
#define PLAT_TOUCH_SCALE 1.7f

/* The mixer's bookkeeping for a queued buffer. */
typedef struct {
    volatile u32 status;
    void *next;
    u32 position;
} PlatAudioVoice;
/* The microphone records at 16 kHz. */
#define PLAT_MIC_RATE 16000.0f

/* What this console has, for the menus: no 3D slider, lid or HOME Menu
 * theme; one video mode with no GPU filter; PlayStation face buttons. */
#define PLAT_HAS_3D_SLIDER 0
#define PLAT_HAS_LID 0
#define PLAT_HAS_HOME_THEME 0
#define PLAT_HAS_WIDE_MODE 0
#define PLAT_HAS_VIDEO_FILTER 0
#define PLAT_PS_BUTTONS 1
/* L2 / R2 / L3 / R3 come from the rear touchpad or the touch screen. */
#define PLAT_HAS_EXTRA_BUTTONS 1

#pragma once

/* What Kasumi needs from the console it runs on: clocks, sleeping, locks,
 * threads and a few system queries. Shared code includes only this; each
 * console supplies the types (platform_3ds.h, platform_vita.h) and the
 * functions (source/3ds/platform.c, source/vita/platform.c). */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if defined(__3DS__)
#include "platform_3ds.h"
#elif defined(__vita__)
#include "platform_vita.h"
#else
#error "Kasumi builds for the 3DS (__3DS__) or the PS Vita (__vita__)."
#endif

/* Each platform header provides:
 *   u8 u16 u32 u64 s8 s16 s32 s64   fixed-width integers
 *   PlatLock, PLAT_LOCK_INIT         a plain lock; PLAT_LOCK_INIT is an
 *                                    unlocked lock for static storage
 *   PlatRecursiveLock                a lock the holder may take again
 *   PlatEvent                        a flag threads can wait on
 *   PlatThread                       a thread handle (NULL when none)
 *   PLAT_TICKS_PER_SEC               the rate of plat_ticks()
 *   PLAT_PRIO_LOWEST                 the least urgent thread priority
 *   PLAT_MODEL_FAMILY                the console, for the startup log
 *   PLAT_LSTICK_REACH, PLAT_RSTICK_REACH
 *                                    stick readings at the rim
 *   PLAT_TOUCH_WIDTH, PLAT_TOUCH_HEIGHT
 *                                    the touch screen's size in pixels
 *   PLAT_TOUCH_SCALE                 its pixels per 3DS lower-screen pixel,
 *                                    for distances on a finger's scale
 *   PlatAudioVoice                   per-buffer storage for the sound driver
 *   PLAT_MIC_RATE                    the microphone's sample rate (float)
 *   PLAT_HAS_3D_SLIDER, PLAT_HAS_LID, PLAT_HAS_HOME_THEME,
 *   PLAT_HAS_WIDE_MODE, PLAT_HAS_VIDEO_FILTER
 *                                    features the menus offer (0 or 1)
 *   PLAT_PS_BUTTONS                  1 where face buttons are PlayStation
 *                                    shapes, drawn in hints */

/* Wall-clock milliseconds since 1 January 1900 (the 3DS's osGetTime). Also
 * used for timeouts, so it must never step backwards while running. */
u64 plat_time_ms(void);
/* A fast counter for measuring short spans, PLAT_TICKS_PER_SEC per second. */
u64 plat_ticks(void);
void plat_sleep_ns(s64 ns);

void plat_lock_init(PlatLock *lock);
void plat_lock(PlatLock *lock);
void plat_unlock(PlatLock *lock);

void plat_rlock_init(PlatRecursiveLock *lock);
void plat_rlock(PlatRecursiveLock *lock);
void plat_runlock(PlatRecursiveLock *lock);

/* A one-shot event wakes one waiter and clears itself; a sticky event stays
 * signalled until plat_event_clear(). */
void plat_event_init(PlatEvent *event, bool sticky);
void plat_event_signal(PlatEvent *event);
void plat_event_clear(PlatEvent *event);
void plat_event_wait(PlatEvent *event);
/* Returns 0 when signalled, nonzero on timeout. */
int plat_event_wait_timeout(PlatEvent *event, s64 timeout_ns);

/* Priorities follow the 3DS: a lower number is more urgent, and threads are
 * started relative to the caller's priority (plat_thread_priority() + n). */
#define PLAT_CORE_DEFAULT (-2) /* the application's main core */
#define PLAT_CORE_ANY (-1)
#define PLAT_CORE_MEDIA 2      /* the spare core for the stream */
int plat_thread_priority(void);
PlatThread plat_thread_create(void (*entry)(void *), void *arg, size_t stack_size,
                              int priority, int core, bool detached);
/* Waits for the thread to return, then frees it. */
void plat_thread_join(PlatThread thread);

/* Wi-Fi signal in bars, 0-3. */
int plat_wifi_strength(void);
/* The connected network's name; false (and "") when there is none. */
bool plat_wifi_ssid(char *out, size_t size);
/* This console's address, netmask and broadcast address in host byte order. */
bool plat_ip_info(u32 *ip, u32 *mask, u32 *broadcast);
/* The console's own services (network stack, power, Wi-Fi status), started
 * once the UI is up. On failure, error says which one. */
bool plat_services_init(char *error, size_t error_size);
void plat_services_exit(void);
/* False on a model Kasumi can't run on (the original 3DS: no decoder). */
bool plat_model_supported(void);
/* Full CPU speed for decoding and the network. */
void plat_full_speed(void);
/* False once the system asks the app to close. */
bool plat_main_loop(void);
/* Whether closing the lid (or the power button) may put the console to
 * sleep. */
void plat_allow_sleep(bool allowed);
/* HOME, sleep and other system events, delivered on a system thread: the
 * handler must only record them. NULL stops delivery. */
typedef enum {
    PLAT_SYS_SLEEP,   /* going to sleep */
    PLAT_SYS_SUSPEND, /* HOME Menu or another applet took over */
    PLAT_SYS_WAKE,    /* back from sleep */
    PLAT_SYS_RESTORE, /* back from HOME */
    PLAT_SYS_OTHER
} PlatSystemEvent;
void plat_on_system_event(void (*handler)(PlatSystemEvent event));
/* Battery in steps of 0 (empty) to 5 (full). */
void plat_battery(u8 *level, bool *charging);
bool plat_wifi_connected(void);
/* Keeps the radio on the access point while a game streams (the 3DS's
 * background StreetPass and SpotPass scans drop packets). */
void plat_wifi_exclusive(bool on);
/* Free memory the GPU and the video decoder can use. */
u32 plat_device_memory_free(void);

/* The system keyboard, over the app; blocks until it closes. text holds
 * the starting text and receives what was typed. Returns true when the
 * player confirmed something other than blanks. */
typedef struct {
    const char *hint;
    const char *cancel; /* button labels */
    const char *ok;
    unsigned max_length;
    bool numbers;    /* digits and '.' (an IP address) */
    bool predictive; /* word suggestions */
} PlatKeyboard;
bool plat_keyboard(const PlatKeyboard *keyboard, char *text, size_t size);

/* The notification light pulsing slowly in an RGBA colour (UI_RGBA); 0
 * turns it off. Consoles without one ignore it. */
void plat_notify_light(u32 color);
/* True while the lid is shut (never, on a console without one). */
bool plat_lid_closed(void);
/* Screen backlights, off to save power while the lid is shut. */
void plat_screens_on(bool on);

/* Sound: numbered channels, each playing a queue of 16-bit PCM buffers in
 * order, mixed together. Kasumi's channels: 0 the stream, 8 the queue
 * chime, 9-10 menu music and voice, 11-13 sound effects. */
#define PLAT_AUDIO_CHANNELS 24

/* One buffer of samples. data comes from plat_audio_alloc (interleaved when
 * stereo); frames is how many sample frames to play. A zeroed buffer is
 * free; once queued it stays busy until it has played. */
typedef struct {
    s16 *data;
    u32 frames;
    PlatAudioVoice voice; /* the console's own bookkeeping */
} PlatAudioBuffer;

/* Starts the sound hardware; false means no sound this run. */
bool plat_audio_init(void);
/* The 3DS's DSP firmware dump (sdmc:/3ds/dspfirm.cdc) is missing. */
bool plat_audio_firmware_missing(void);
void plat_audio_exit(void);
/* Memory the sound hardware can play from. */
void *plat_audio_alloc(size_t bytes);
void plat_audio_free(void *memory);
/* Empties the channel and sets it up. smooth: a better resampler for music
 * at rates other than the output's. */
void plat_audio_channel_open(int channel, float rate, unsigned channels, bool smooth);
/* Stops the channel and forgets its queue. */
void plat_audio_channel_close(int channel);
/* Forgets the queue but keeps the channel's setup. */
void plat_audio_channel_clear(int channel);
void plat_audio_channel_volume(int channel, float left, float right);
void plat_audio_channel_pause(int channel, bool paused);
void plat_audio_queue(int channel, PlatAudioBuffer *buffer);
/* Not waiting or playing: free to fill again. */
bool plat_audio_buffer_idle(const PlatAudioBuffer *buffer);

/* The microphone, 16-bit mono at PLAT_MIC_RATE Hz. Open, it records into
 * a ring that plat_mic_read drains; false when there is no usable mic. */
bool plat_mic_open(void);
void plat_mic_close(void);
/* Copies up to max samples recorded since the last read; returns how many. */
size_t plat_mic_read(s16 *out, size_t max);

/* Fills the buffer from the console's hardware random source. */
bool plat_random_bytes(void *out, size_t size);
bool plat_headset_connected(void);

/* Buttons, one bit each, laid out like the 3DS's so it reads them as they
 * come. Names are by position on a Nintendo pad: A is the right face
 * button, B the bottom one, X the top and Y the left (Vita: Circle, Cross,
 * Triangle, Square). Consoles without ZL/ZR or a second stick map what
 * they have instead (the Vita's rear touchpad). */
#define PLAT_KEY_A            (1u << 0)
#define PLAT_KEY_B            (1u << 1)
#define PLAT_KEY_SELECT       (1u << 2)
#define PLAT_KEY_START        (1u << 3)
#define PLAT_KEY_DRIGHT       (1u << 4)
#define PLAT_KEY_DLEFT        (1u << 5)
#define PLAT_KEY_DUP          (1u << 6)
#define PLAT_KEY_DDOWN        (1u << 7)
#define PLAT_KEY_R            (1u << 8)
#define PLAT_KEY_L            (1u << 9)
#define PLAT_KEY_X            (1u << 10)
#define PLAT_KEY_Y            (1u << 11)
/* Stick clicks: the 3DS has none (the touch screen stands in); the Vita
 * takes them from its rear touchpad or a PS TV controller. */
#define PLAT_KEY_L3           (1u << 12)
#define PLAT_KEY_R3           (1u << 13)
#define PLAT_KEY_ZL           (1u << 14)
#define PLAT_KEY_ZR           (1u << 15)
/* Set while anything touches the touch screen. */
#define PLAT_KEY_TOUCH        (1u << 20)
#define PLAT_KEY_CSTICK_RIGHT (1u << 24)
#define PLAT_KEY_CSTICK_LEFT  (1u << 25)
#define PLAT_KEY_CSTICK_UP    (1u << 26)
#define PLAT_KEY_CSTICK_DOWN  (1u << 27)
#define PLAT_KEY_CPAD_RIGHT   (1u << 28)
#define PLAT_KEY_CPAD_LEFT    (1u << 29)
#define PLAT_KEY_CPAD_UP      (1u << 30)
#define PLAT_KEY_CPAD_DOWN    (1u << 31)
/* The D-Pad or the left stick, for moving around menus. */
#define PLAT_KEY_UP    (PLAT_KEY_DUP | PLAT_KEY_CPAD_UP)
#define PLAT_KEY_DOWN  (PLAT_KEY_DDOWN | PLAT_KEY_CPAD_DOWN)
#define PLAT_KEY_LEFT  (PLAT_KEY_DLEFT | PLAT_KEY_CPAD_LEFT)
#define PLAT_KEY_RIGHT (PLAT_KEY_DRIGHT | PLAT_KEY_CPAD_RIGHT)

/* One finger (or the stylus). The id stays the same while it stays down,
 * so gestures can follow each finger on a multi-touch screen. */
typedef struct {
    s16 x, y;
    u8 id;
} PlatTouch;

#define PLAT_TOUCH_MAX 6

/* Everything read from the controls in one scan. */
typedef struct {
    u32 down, held, up;
    /* down, plus repeats while a button stays held (plat_input_set_repeat). */
    u32 repeat;
    /* Up is positive; PLAT_LSTICK_REACH / PLAT_RSTICK_REACH at the rim. */
    s16 lstick_x, lstick_y, rstick_x, rstick_y;
    /* The touch screen, in its pixels. The 3DS reports one point. */
    u8 touch_count;
    PlatTouch touch[PLAT_TOUCH_MAX];
    /* A touchpad on the back (Vita), scaled to the touch screen's pixels
     * so a finger behind a spot on the screen reads as that spot. */
    u8 rear_count;
    PlatTouch rear[PLAT_TOUCH_MAX];
    /* 0..1; consoles without a 3D slider read 0. */
    float slider_3d;
} PlatInput;

/* Reads the controls; once a frame, on the main thread. */
void plat_input_scan(void);
/* The last scan. */
const PlatInput *plat_input(void);
/* Held buttons repeat after delay frames, then every interval frames. */
void plat_input_set_repeat(u32 delay, u32 interval);
/* The buttons a console adds where its controls lack them (the Vita: L2,
 * R2, L3, R3): on the rear touchpad's corners, as buttons on the touch
 * screen, or nowhere. The app sets it each frame (off in the menus). */
typedef enum { PLAT_EXTRA_OFF, PLAT_EXTRA_REAR, PLAT_EXTRA_FRONT } PlatExtraButtons;
void plat_input_set_extra_buttons(PlatExtraButtons where);
/* With PLAT_EXTRA_FRONT, the touch screen buttons for drawing (touch
 * screen pixels). Touches on them never reach the touch list. */
typedef struct {
    s16 x, y, w, h;
    u32 key;
    bool held;
} PlatExtraButton;
/* The index-th one; false past the last (or when there are none). */
bool plat_input_extra_button(unsigned index, PlatExtraButton *out);
/* The gyroscope draws power, so it runs only while asked for. Returns
 * whether it is running. */
bool plat_gyro_enable(bool on);
/* Rotation in degrees per second about the vertical (yaw) and sideways
 * (pitch) axes, with the 3DS's signs; other consoles match them. */
void plat_gyro_read(float *yaw_dps, float *pitch_dps);

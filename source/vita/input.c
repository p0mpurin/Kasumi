#include "platform.h"

#include <psp2/ctrl.h>
#include <psp2/motion.h>
#include <psp2/touch.h>
#include <string.h>

/* Buttons by position, like the 3DS: Circle is A (right), Cross is B
 * (bottom), Triangle is X (top), Square is Y (left). The Vita's L and R are
 * L and R; a PS TV controller adds L2 / R2 (ZL / ZR) and L3 / R3. */
static const struct { unsigned vita; u32 key; } BUTTONS[] = {
    { SCE_CTRL_CIRCLE, PLAT_KEY_A }, { SCE_CTRL_CROSS, PLAT_KEY_B },
    { SCE_CTRL_TRIANGLE, PLAT_KEY_X }, { SCE_CTRL_SQUARE, PLAT_KEY_Y },
    { SCE_CTRL_L1, PLAT_KEY_L }, { SCE_CTRL_R1, PLAT_KEY_R },
    { SCE_CTRL_LTRIGGER, PLAT_KEY_ZL }, { SCE_CTRL_RTRIGGER, PLAT_KEY_ZR },
    { SCE_CTRL_L3, PLAT_KEY_L3 }, { SCE_CTRL_R3, PLAT_KEY_R3 },
    { SCE_CTRL_START, PLAT_KEY_START }, { SCE_CTRL_SELECT, PLAT_KEY_SELECT },
    { SCE_CTRL_UP, PLAT_KEY_DUP }, { SCE_CTRL_DOWN, PLAT_KEY_DDOWN },
    { SCE_CTRL_LEFT, PLAT_KEY_DLEFT }, { SCE_CTRL_RIGHT, PLAT_KEY_DRIGHT },
};

/* A stick pushed past this reads as a direction too (menus). */
#define STICK_DIRECTION 60

static PlatInput g_input;
static bool g_ready;
static u32 g_repeat_delay = 18, g_repeat_interval = 5;
static unsigned g_held_frames[32];
static SceTouchPanelInfo g_front_info, g_back_info;

static void start(void)
{
    sceCtrlSetSamplingModeExt(SCE_CTRL_MODE_ANALOG_WIDE);
    sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
    sceTouchSetSamplingState(SCE_TOUCH_PORT_BACK, SCE_TOUCH_SAMPLING_STATE_START);
    if (sceTouchGetPanelInfo(SCE_TOUCH_PORT_FRONT, &g_front_info) < 0 || g_front_info.maxAaX <= g_front_info.minAaX) {
        g_front_info.minAaX = 0; g_front_info.maxAaX = 1919;
        g_front_info.minAaY = 0; g_front_info.maxAaY = 1087;
    }
    if (sceTouchGetPanelInfo(SCE_TOUCH_PORT_BACK, &g_back_info) < 0 || g_back_info.maxAaX <= g_back_info.minAaX) {
        g_back_info.minAaX = 0; g_back_info.maxAaX = 1919;
        g_back_info.minAaY = 108; g_back_info.maxAaY = 889;
    }
    g_ready = true;
}

/* Panel units to screen pixels. */
static PlatTouch to_screen(const SceTouchReport *report, const SceTouchPanelInfo *info)
{
    const int w = info->maxAaX - info->minAaX, h = info->maxAaY - info->minAaY;
    int x = (report->x - info->minAaX) * PLAT_TOUCH_WIDTH / (w > 0 ? w : 1);
    int y = (report->y - info->minAaY) * PLAT_TOUCH_HEIGHT / (h > 0 ? h : 1);
    x = x < 0 ? 0 : x >= PLAT_TOUCH_WIDTH ? PLAT_TOUCH_WIDTH - 1 : x;
    y = y < 0 ? 0 : y >= PLAT_TOUCH_HEIGHT ? PLAT_TOUCH_HEIGHT - 1 : y;
    return (PlatTouch){ (s16)x, (s16)y, report->id };
}

static PlatExtraButtons g_extra = PLAT_EXTRA_OFF;

void plat_input_set_extra_buttons(PlatExtraButtons where) { g_extra = where; }

/* The rear touchpad's corners stand in for the buttons the Vita lacks, as
 * in PlayStation's own Remote Play: the upper corners are L2 / R2 and the
 * lower ones L3 / R3. The middle is left alone, where fingers rest while
 * holding the console. */
static u32 rear_buttons(const PlatInput *in)
{
    if (g_extra != PLAT_EXTRA_REAR) return 0;
    u32 keys = 0;
    for (unsigned i = 0; i < in->rear_count; ++i) {
        const PlatTouch t = in->rear[i];
        const bool left = t.x < PLAT_TOUCH_WIDTH * 3 / 10, right = t.x >= PLAT_TOUCH_WIDTH * 7 / 10;
        if (!left && !right) continue;
        const bool upper = t.y < PLAT_TOUCH_HEIGHT / 2;
        keys |= upper ? (left ? PLAT_KEY_ZL : PLAT_KEY_ZR) : (left ? PLAT_KEY_L3 : PLAT_KEY_R3);
    }
    return keys;
}

/* Or, for players who hold the console with fingers on the back, the same
 * four as buttons on the touch screen's edges, where the thumbs reach
 * past the sticks and face buttons: L2 above L3 on the left, R2 above R3
 * on the right. */
#define FRONT_W 112
#define FRONT_H 104
static const struct { s16 x, y; u32 key; } FRONT[4] = {
    { 0, 120, PLAT_KEY_ZL }, { 0, 120 + FRONT_H + 16, PLAT_KEY_L3 },
    { PLAT_TOUCH_WIDTH - FRONT_W, 120, PLAT_KEY_ZR }, { PLAT_TOUCH_WIDTH - FRONT_W, 120 + FRONT_H + 16, PLAT_KEY_R3 },
};
static u32 g_front_held;

/* Takes the touches on the front buttons out of the list. */
static u32 front_buttons(PlatInput *in)
{
    if (g_extra != PLAT_EXTRA_FRONT) return 0;
    u32 keys = 0;
    unsigned kept = 0;
    for (unsigned i = 0; i < in->touch_count; ++i) {
        const PlatTouch t = in->touch[i];
        bool on_button = false;
        for (unsigned b = 0; b < 4; ++b)
            if (t.x >= FRONT[b].x && t.x < FRONT[b].x + FRONT_W && t.y >= FRONT[b].y && t.y < FRONT[b].y + FRONT_H) {
                keys |= FRONT[b].key;
                on_button = true;
            }
        if (!on_button) in->touch[kept++] = t;
    }
    in->touch_count = (u8)kept;
    return keys;
}

bool plat_input_extra_button(unsigned index, PlatExtraButton *out)
{
    if (g_extra != PLAT_EXTRA_FRONT || index >= 4) return false;
    out->x = FRONT[index].x;
    out->y = FRONT[index].y;
    out->w = FRONT_W;
    out->h = FRONT_H;
    out->key = FRONT[index].key;
    out->held = (g_front_held & FRONT[index].key) != 0;
    return true;
}

void plat_input_scan(void)
{
    if (!g_ready) start();
    PlatInput *in = &g_input;
    SceCtrlData pad;
    memset(&pad, 0, sizeof(pad));
    /* Ext2: the Vita's L / R come back as L1 / R1, and a PS TV controller's
     * L2 / R2 / L3 / R3 as themselves. */
    if (sceCtrlPeekBufferPositiveExt2(0, &pad, 1) < 0) {
        pad.lx = pad.ly = pad.rx = pad.ry = 128;
    }
    u32 held = 0;
    for (unsigned i = 0; i < sizeof(BUTTONS) / sizeof(BUTTONS[0]); ++i)
        if (pad.buttons & BUTTONS[i].vita) held |= BUTTONS[i].key;

    in->lstick_x = (s16)((int)pad.lx - 128);
    in->lstick_y = (s16)(128 - (int)pad.ly);
    in->rstick_x = (s16)((int)pad.rx - 128);
    in->rstick_y = (s16)(128 - (int)pad.ry);
    if (in->lstick_x > STICK_DIRECTION) held |= PLAT_KEY_CPAD_RIGHT;
    if (in->lstick_x < -STICK_DIRECTION) held |= PLAT_KEY_CPAD_LEFT;
    if (in->lstick_y > STICK_DIRECTION) held |= PLAT_KEY_CPAD_UP;
    if (in->lstick_y < -STICK_DIRECTION) held |= PLAT_KEY_CPAD_DOWN;
    if (in->rstick_x > STICK_DIRECTION) held |= PLAT_KEY_CSTICK_RIGHT;
    if (in->rstick_x < -STICK_DIRECTION) held |= PLAT_KEY_CSTICK_LEFT;
    if (in->rstick_y > STICK_DIRECTION) held |= PLAT_KEY_CSTICK_UP;
    if (in->rstick_y < -STICK_DIRECTION) held |= PLAT_KEY_CSTICK_DOWN;

    SceTouchData touch;
    in->touch_count = 0;
    if (sceTouchPeek(SCE_TOUCH_PORT_FRONT, &touch, 1) >= 0)
        for (unsigned i = 0; i < touch.reportNum && in->touch_count < PLAT_TOUCH_MAX; ++i)
            in->touch[in->touch_count++] = to_screen(&touch.report[i], &g_front_info);
    g_front_held = front_buttons(in);
    held |= g_front_held;
    if (in->touch_count) held |= PLAT_KEY_TOUCH;
    in->rear_count = 0;
    if (sceTouchPeek(SCE_TOUCH_PORT_BACK, &touch, 1) >= 0)
        for (unsigned i = 0; i < touch.reportNum && in->rear_count < PLAT_TOUCH_MAX; ++i)
            in->rear[in->rear_count++] = to_screen(&touch.report[i], &g_back_info);
    held |= rear_buttons(in);

    const u32 previous = in->held;
    in->held = held;
    in->down = held & ~previous;
    in->up = previous & ~held;
    /* Repeats, counted in scans like the 3DS's HID: first after delay,
     * then every interval. */
    in->repeat = in->down;
    for (unsigned bit = 0; bit < 32; ++bit) {
        if (!(held & (1u << bit))) {
            g_held_frames[bit] = 0;
            continue;
        }
        const unsigned frames = ++g_held_frames[bit];
        if (frames > g_repeat_delay && (frames - g_repeat_delay) % (g_repeat_interval ? g_repeat_interval : 1) == 0)
            in->repeat |= 1u << bit;
    }
    in->slider_3d = 0.0f;
}

const PlatInput *plat_input(void) { return &g_input; }

void plat_input_set_repeat(u32 delay, u32 interval)
{
    g_repeat_delay = delay;
    g_repeat_interval = interval;
}

static bool g_gyro_on;

bool plat_gyro_enable(bool on)
{
    if (on && !g_gyro_on) g_gyro_on = sceMotionStartSampling() >= 0;
    else if (!on && g_gyro_on) {
        sceMotionStopSampling();
        g_gyro_on = false;
    }
    return g_gyro_on;
}

void plat_gyro_read(float *yaw_dps, float *pitch_dps)
{
    SceMotionState state;
    *yaw_dps = *pitch_dps = 0.0f;
    if (!g_gyro_on || sceMotionGetState(&state) < 0) return;
    /* Radians a second to degrees. Held in landscape, turning left and
     * right is about the screen's vertical axis (y) and tilting is about
     * its horizontal one (x). */
    const float to_degrees = 57.29578f;
    *yaw_dps = state.angularVelocity.y * to_degrees;
    *pitch_dps = state.angularVelocity.x * to_degrees;
}

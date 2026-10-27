#include "platform.h"

#include <3ds.h>
#include <string.h>

/* platform.h copies libctru's bit layout, so the keys need no translation. */
_Static_assert(PLAT_KEY_A == KEY_A && PLAT_KEY_ZR == KEY_ZR && PLAT_KEY_TOUCH == KEY_TOUCH &&
               PLAT_KEY_CSTICK_DOWN == KEY_CSTICK_DOWN && PLAT_KEY_CPAD_DOWN == KEY_CPAD_DOWN &&
               PLAT_KEY_UP == KEY_UP && PLAT_KEY_RIGHT == KEY_RIGHT,
               "platform.h keys must match libctru");

static PlatInput g_input;
static float g_gyro_raw_per_dps = 14.375f;

void plat_input_scan(void)
{
    hidScanInput();
    PlatInput *in = &g_input;
    in->down = hidKeysDown();
    in->held = hidKeysHeld();
    in->up = hidKeysUp();
    in->repeat = hidKeysDownRepeat();
    circlePosition circle, cstick;
    hidCircleRead(&circle);
    hidCstickRead(&cstick);
    in->lstick_x = circle.dx;
    in->lstick_y = circle.dy;
    in->rstick_x = cstick.dx;
    in->rstick_y = cstick.dy;
    in->touch_count = 0;
    if (in->held & KEY_TOUCH) {
        touchPosition touch;
        hidTouchRead(&touch);
        in->touch[0] = (PlatTouch){ (s16)touch.px, (s16)touch.py, 0 };
        in->touch_count = 1;
    }
    in->rear_count = 0;
    in->slider_3d = osGet3DSliderState();
}

const PlatInput *plat_input(void) { return &g_input; }

void plat_input_set_repeat(u32 delay, u32 interval) { hidSetRepeatParameters(delay, interval); }

bool plat_gyro_enable(bool on)
{
    static bool enabled;
    if (on && !enabled && R_SUCCEEDED(HIDUSER_EnableGyroscope())) {
        enabled = true;
        /* Build 67-69 multiplied by this coefficient; it is raw units per
         * dps, so readings came out ~200x too large and the camera spun. */
        float coefficient = 0.0f;
        if (R_SUCCEEDED(HIDUSER_GetGyroscopeRawToDpsCoefficient(&coefficient)) &&
            coefficient > 1.0f && coefficient < 100.0f)
            g_gyro_raw_per_dps = coefficient;
    } else if (!on && enabled) {
        HIDUSER_DisableGyroscope();
        enabled = false;
    }
    return enabled;
}

void plat_gyro_read(float *yaw_dps, float *pitch_dps)
{
    angularRate rate;
    hidGyroRead(&rate);
    /* libctru: x = roll, y = pitch, z = yaw. */
    *yaw_dps = (float)rate.z / g_gyro_raw_per_dps;
    *pitch_dps = (float)rate.y / g_gyro_raw_per_dps;
}

/* The 3DS has every button the stream needs (L3 / R3 sit on the lower
 * screen, drawn by screens.c). */
void plat_input_set_extra_buttons(PlatExtraButtons where) { (void)where; }

bool plat_input_extra_button(unsigned index, PlatExtraButton *out)
{
    (void)index;
    (void)out;
    return false;
}

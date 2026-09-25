#pragma once

#include <3ds.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint16_t buttons;
    uint8_t left_trigger;
    uint8_t right_trigger;
    int16_t left_x;
    int16_t left_y;
    int16_t right_x;
    int16_t right_y;
} GfnGamepadState;

typedef enum {
    /* Match physical positions: the bottom face button is Cross / Xbox A. */
    GFN_LAYOUT_POSITION = 0,
    /* Match printed letters: 3DS A sends Xbox A. */
    GFN_LAYOUT_LABEL = 1
} GfnButtonLayout;

typedef enum {
    GFN_GYRO_OFF = 0,
    GFN_GYRO_ALWAYS = 1,
    /* Only while the left trigger (aim) is held, like most shooters. */
    GFN_GYRO_WHILE_AIMING = 2,
    GFN_GYRO_MODE_COUNT
} GfnGyroMode;

typedef struct {
    GfnButtonLayout layout;
    unsigned deadzone_percent;
    bool swap_shoulders;
    GfnGyroMode gyro_mode;
    /* 0 low, 1 medium, 2 high. */
    unsigned gyro_speed;
} GfnInputConfig;

enum {
    GFN_PAD_DPAD_UP = 0x0001, GFN_PAD_DPAD_DOWN = 0x0002,
    GFN_PAD_DPAD_LEFT = 0x0004, GFN_PAD_DPAD_RIGHT = 0x0008,
    GFN_PAD_START = 0x0010, GFN_PAD_BACK = 0x0020,
    GFN_PAD_LEFT_THUMB = 0x0040, GFN_PAD_RIGHT_THUMB = 0x0080,
    GFN_PAD_LEFT_SHOULDER = 0x0100, GFN_PAD_RIGHT_SHOULDER = 0x0200,
    GFN_PAD_GUIDE = 0x0400,
    GFN_PAD_A = 0x1000, GFN_PAD_B = 0x2000, GFN_PAD_X = 0x4000, GFN_PAD_Y = 0x8000
};

void gfn_input_configure(const GfnInputConfig *config);
/* Buttons the 3DS lacks (L3, R3, Guide), held from the touch screen. */
void gfn_input_set_virtual_buttons(uint16_t buttons);
/* While suppressed, reads return a neutral pad (menus and overlays). */
void gfn_input_set_suppressed(bool suppressed);
uint16_t gfn_input_buttons_for_keys(u32 keys);
void gfn_input_read_3ds(GfnGamepadState *state);
size_t gfn_input_encode_gamepad(uint8_t output[38], const GfnGamepadState *state,
                                uint64_t timestamp_us);
size_t gfn_input_encode_gamepad_wire(uint8_t output[50], const GfnGamepadState *state,
                                     uint64_t timestamp_us, int protocol_version);
/* Partially reliable gamepad framing (0x26): unordered delivery with a short
 * lifetime, so one lost packet never delays newer controller states. */
size_t gfn_input_encode_gamepad_partial(uint8_t output[54], const GfnGamepadState *state,
                                        uint64_t timestamp_us, uint16_t sequence);
size_t gfn_input_encode_mouse_move(uint8_t output[34], int16_t dx, int16_t dy,
                                   uint64_t timestamp_us, int protocol_version);
size_t gfn_input_encode_mouse_button(uint8_t output[28], bool pressed,
                                     uint64_t timestamp_us, int protocol_version);
size_t gfn_input_encode_key(uint8_t output[28], uint16_t keycode, uint16_t scancode,
                            uint16_t modifiers, bool pressed, uint64_t timestamp_us,
                            int protocol_version);
bool gfn_input_key_for_char(char character, uint16_t *keycode, uint16_t *scancode,
                            uint16_t *modifiers);
bool gfn_input_self_test(void);
/* Gyro aim adds console rotation to the right stick (or the pointer). */
bool gfn_input_gyro_active(void);

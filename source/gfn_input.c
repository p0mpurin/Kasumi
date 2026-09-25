#include "gfn_input.h"

#include <math.h>
#include <stdbool.h>
#include <string.h>

static GfnInputConfig g_config = { GFN_LAYOUT_POSITION, 12, false, GFN_GYRO_OFF, 1 };
static uint16_t g_virtual_buttons;
static bool g_suppressed;
static bool g_gyro_enabled;
/* Raw gyro units per degree per second (HID reports 14.375). */
static float g_gyro_raw_per_dps = 14.375f;
static float g_gyro_bias_yaw, g_gyro_bias_pitch;
static float g_gyro_smooth_x, g_gyro_smooth_y;
static unsigned g_gyro_calibration;
static bool g_gyro_active;

void gfn_input_configure(const GfnInputConfig *config)
{
    if (config) g_config = *config;
    if (g_config.deadzone_percent > 40) g_config.deadzone_percent = 40;
    if (g_config.gyro_speed > 2) g_config.gyro_speed = 1;
    /* The gyroscope draws power, so it only runs while gyro aim is on. */
    const bool want = g_config.gyro_mode != GFN_GYRO_OFF;
    if (want && !g_gyro_enabled && R_SUCCEEDED(HIDUSER_EnableGyroscope())) {
        g_gyro_enabled = true;
        /* Build 67-69 multiplied by this coefficient; it is raw units per
         * dps, so readings came out ~200x too large and the camera spun. */
        float coefficient = 0.0f;
        if (R_SUCCEEDED(HIDUSER_GetGyroscopeRawToDpsCoefficient(&coefficient)) &&
            coefficient > 1.0f && coefficient < 100.0f)
            g_gyro_raw_per_dps = coefficient;
        g_gyro_bias_yaw = g_gyro_bias_pitch = 0.0f;
        g_gyro_calibration = 0;
    } else if (!want && g_gyro_enabled) {
        HIDUSER_DisableGyroscope();
        g_gyro_enabled = false;
    }
}

bool gfn_input_gyro_active(void) { return g_gyro_active; }

/* Console rotation (degrees per second) becomes right-stick deflection, the
 * "gyro as joystick" model: turning at a steady rate turns the camera at a
 * steady rate. The resting bias is tracked while the console is still, a
 * small deadzone hides hand tremor, and a floor jumps the game's own stick
 * deadzone so slow, careful aiming still registers. */
static void apply_gyro(GfnGamepadState *state, u32 held)
{
    g_gyro_active = false;
    if (!g_gyro_enabled) return;
    angularRate rate;
    hidGyroRead(&rate);
    /* libctru: x = roll, y = pitch, z = yaw. */
    const float yaw = (float)rate.z / g_gyro_raw_per_dps;
    const float pitch = (float)rate.y / g_gyro_raw_per_dps;
    /* The first half second after enabling calibrates the resting drift
     * quickly; afterwards it is tracked slowly whenever the console is
     * nearly still, so a steady turn is never mistaken for drift. */
    if (g_gyro_calibration < 30) {
        const float k = 1.0f / (float)(++g_gyro_calibration);
        g_gyro_bias_yaw += (yaw - g_gyro_bias_yaw) * k;
        g_gyro_bias_pitch += (pitch - g_gyro_bias_pitch) * k;
        return;
    }
    const float still = 2.5f;
    if (fabsf(yaw - g_gyro_bias_yaw) < still && fabsf(pitch - g_gyro_bias_pitch) < still) {
        g_gyro_bias_yaw += (yaw - g_gyro_bias_yaw) * 0.01f;
        g_gyro_bias_pitch += (pitch - g_gyro_bias_pitch) * 0.01f;
    }
    /* A light low-pass hides sensor noise and hand tremor. */
    g_gyro_smooth_x += (-(yaw - g_gyro_bias_yaw) - g_gyro_smooth_x) * 0.5f;
    g_gyro_smooth_y += ((pitch - g_gyro_bias_pitch) - g_gyro_smooth_y) * 0.5f;
    const u32 aim_key = g_config.swap_shoulders ? KEY_L : KEY_ZL;
    if (g_config.gyro_mode == GFN_GYRO_WHILE_AIMING && !(held & aim_key)) return;
    g_gyro_active = true;
    /* Degrees per second for full deflection: low, medium, high. */
    static const float full_scale[3] = { 220.0f, 150.0f, 100.0f };
    const float scale = full_scale[g_config.gyro_speed];
    float x = g_gyro_smooth_x, y = g_gyro_smooth_y;
    const float deadzone = 2.0f;
    const float magnitude = sqrtf(x * x + y * y);
    if (magnitude <= deadzone) return;
    float amount = (magnitude - deadzone) / scale;
    if (amount > 1.0f) amount = 1.0f;
    /* Gentle curve for fine aim, plus a small floor over the game's own
     * stick deadzone. */
    amount = 0.10f + 0.90f * (0.5f * amount + 0.5f * amount * amount);
    x = x / magnitude * amount * 32767.0f;
    y = y / magnitude * amount * 32767.0f;
    float rx = (float)state->right_x + x, ry = (float)state->right_y + y;
    if (rx > 32767.0f) rx = 32767.0f;
    if (rx < -32767.0f) rx = -32767.0f;
    if (ry > 32767.0f) ry = 32767.0f;
    if (ry < -32767.0f) ry = -32767.0f;
    state->right_x = (int16_t)rx;
    state->right_y = (int16_t)ry;
}

void gfn_input_set_virtual_buttons(uint16_t buttons) { g_virtual_buttons = buttons; }
void gfn_input_set_suppressed(bool suppressed) { g_suppressed = suppressed; }

uint16_t gfn_input_buttons_for_keys(u32 keys)
{
    uint16_t buttons = 0;
    if (keys & KEY_DUP) buttons |= GFN_PAD_DPAD_UP;
    if (keys & KEY_DDOWN) buttons |= GFN_PAD_DPAD_DOWN;
    if (keys & KEY_DLEFT) buttons |= GFN_PAD_DPAD_LEFT;
    if (keys & KEY_DRIGHT) buttons |= GFN_PAD_DPAD_RIGHT;
    if (keys & KEY_START) buttons |= GFN_PAD_START;
    if (keys & KEY_SELECT) buttons |= GFN_PAD_BACK;
    const u32 left_shoulder = g_config.swap_shoulders ? KEY_ZL : KEY_L;
    const u32 right_shoulder = g_config.swap_shoulders ? KEY_ZR : KEY_R;
    if (keys & left_shoulder) buttons |= GFN_PAD_LEFT_SHOULDER;
    if (keys & right_shoulder) buttons |= GFN_PAD_RIGHT_SHOULDER;
    if (g_config.layout == GFN_LAYOUT_LABEL) {
        if (keys & KEY_A) buttons |= GFN_PAD_A;
        if (keys & KEY_B) buttons |= GFN_PAD_B;
        if (keys & KEY_X) buttons |= GFN_PAD_X;
        if (keys & KEY_Y) buttons |= GFN_PAD_Y;
    } else {
        /* 3DS diamond: X top, A right, B bottom, Y left. PlayStation/Xbox:
         * Triangle/Y top, Circle/B right, Cross/A bottom, Square/X left. */
        if (keys & KEY_X) buttons |= GFN_PAD_Y;
        if (keys & KEY_A) buttons |= GFN_PAD_B;
        if (keys & KEY_B) buttons |= GFN_PAD_A;
        if (keys & KEY_Y) buttons |= GFN_PAD_X;
    }
    return buttons;
}

/* Radial deadzone, rescaled so the deadzone edge is zero and the stick's
 * physical rim reaches full deflection in every direction. */
static void scale_stick(int raw_x, int raw_y, int rim, int16_t *out_x, int16_t *out_y)
{
    const float x = (float)raw_x, y = (float)raw_y;
    const float magnitude = sqrtf(x * x + y * y);
    const float deadzone = (float)rim * (float)g_config.deadzone_percent / 100.0f;
    if (magnitude <= deadzone || magnitude <= 0.0f) { *out_x = *out_y = 0; return; }
    float scaled = (magnitude - deadzone) / ((float)rim - deadzone);
    if (scaled > 1.0f) scaled = 1.0f;
    const float factor = scaled * 32767.0f / magnitude;
    float fx = x * factor, fy = y * factor;
    if (fx > 32767.0f) fx = 32767.0f;
    if (fx < -32767.0f) fx = -32767.0f;
    if (fy > 32767.0f) fy = 32767.0f;
    if (fy < -32767.0f) fy = -32767.0f;
    *out_x = (int16_t)fx;
    *out_y = (int16_t)fy;
}

void gfn_input_read_3ds(GfnGamepadState *state)
{
    memset(state, 0, sizeof(*state));
    if (g_suppressed) return;
    const u32 held = hidKeysHeld();
    state->buttons = gfn_input_buttons_for_keys(held) | g_virtual_buttons;
    const u32 left_trigger = g_config.swap_shoulders ? KEY_L : KEY_ZL;
    const u32 right_trigger = g_config.swap_shoulders ? KEY_R : KEY_ZR;
    if (held & left_trigger) state->left_trigger = 255;
    if (held & right_trigger) state->right_trigger = 255;
    circlePosition circle;
    circlePosition cstick;
    hidCircleRead(&circle);
    hidCstickRead(&cstick);
    /* The Circle Pad reaches about 150 at its rim; the C-Stick is shorter. */
    scale_stick(circle.dx, circle.dy, 150, &state->left_x, &state->left_y);
    scale_stick(cstick.dx, cstick.dy, 140, &state->right_x, &state->right_y);
    apply_gyro(state, held);
}

static void put_u16_le(uint8_t *out, uint16_t value)
{
    out[0] = (uint8_t)value; out[1] = (uint8_t)(value >> 8);
}

static void put_u32_le(uint8_t *out, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i) out[i] = (uint8_t)(value >> (i * 8));
}

static void put_u64_le(uint8_t *out, uint64_t value)
{
    for (unsigned i = 0; i < 8; ++i) out[i] = (uint8_t)(value >> (i * 8));
}

static void put_u64_be(uint8_t *out, uint64_t value)
{
    for (unsigned i = 0; i < 8; ++i) out[i] = (uint8_t)(value >> ((7 - i) * 8));
}

static void put_u16_be(uint8_t *out, uint16_t value)
{
    out[0] = (uint8_t)(value >> 8); out[1] = (uint8_t)value;
}

static size_t wrap_input(uint8_t *output, const uint8_t *payload, size_t length,
                         uint64_t timestamp_us, int version, bool single)
{
    if (version <= 2) { memcpy(output, payload, length); return length; }
    output[0] = 0x23;
    put_u64_be(output + 1, timestamp_us);
    output[9] = single ? 0x22 : 0x21;
    const size_t offset = single ? 10 : 12;
    if (!single) put_u16_be(output + 10, (uint16_t)length);
    memcpy(output + offset, payload, length);
    return offset + length;
}

size_t gfn_input_encode_mouse_move(uint8_t output[34], int16_t dx, int16_t dy,
                                   uint64_t timestamp_us, int version)
{
    uint8_t payload[22] = {0};
    put_u32_le(payload, 7);
    put_u16_be(payload + 4, (uint16_t)dx);
    put_u16_be(payload + 6, (uint16_t)dy);
    put_u64_be(payload + 14, timestamp_us);
    return wrap_input(output, payload, sizeof(payload), timestamp_us, version, false);
}

size_t gfn_input_encode_mouse_button(uint8_t output[28], bool pressed,
                                     uint64_t timestamp_us, int version)
{
    uint8_t payload[18] = {0};
    put_u32_le(payload, pressed ? 8 : 9);
    payload[4] = 1;
    put_u64_be(payload + 10, timestamp_us);
    return wrap_input(output, payload, sizeof(payload), timestamp_us, version, true);
}

size_t gfn_input_encode_key(uint8_t output[28], uint16_t keycode, uint16_t scancode,
                            uint16_t modifiers, bool pressed, uint64_t timestamp_us,
                            int version)
{
    uint8_t payload[18] = {0};
    put_u32_le(payload, pressed ? 3 : 4);
    put_u16_be(payload + 4, keycode);
    put_u16_be(payload + 6, modifiers);
    put_u16_be(payload + 8, scancode);
    put_u64_be(payload + 10, timestamp_us);
    return wrap_input(output, payload, sizeof(payload), timestamp_us, version, true);
}

bool gfn_input_key_for_char(char c, uint16_t *keycode, uint16_t *scancode,
                            uint16_t *modifiers)
{
    static const uint8_t letters[26] = {
        0x1e,0x30,0x2e,0x20,0x12,0x21,0x22,0x23,0x17,0x24,0x25,0x26,0x32,
        0x31,0x18,0x19,0x10,0x13,0x1f,0x14,0x16,0x2f,0x11,0x2d,0x15,0x2c
    };
    static const char symbols[] = "-=[]\\;',./`";
    static const uint8_t scans[] = {0x0c,0x0d,0x1a,0x1b,0x2b,0x27,0x28,0x33,0x34,0x35,0x29};
    static const uint16_t virtuals[] = {0xbd,0xbb,0xdb,0xdd,0xdc,0xba,0xde,0xbc,0xbe,0xbf,0xc0};
    static const char shifted[] = "!@#$%^&*()_+{}|:\"<>?~";
    static const char bases[] = "1234567890-=[]\\;',./`";
    *modifiers = 0;
    if (c >= 'A' && c <= 'Z') { *modifiers = 1; c = (char)(c + 32); }
    if (c >= 'a' && c <= 'z') { *keycode = (uint16_t)(c - 32); *scancode = letters[c-'a']; return true; }
    if (c >= '0' && c <= '9') { *keycode = (uint16_t)c; *scancode = c == '0' ? 0x0b : (uint16_t)(c-'1'+2); return true; }
    if (c == ' ') { *keycode = 0x20; *scancode = 0x39; return true; }
    for (size_t i = 0; i < sizeof(shifted)-1; ++i)
        if (c == shifted[i]) {
            if (!gfn_input_key_for_char(bases[i], keycode, scancode, modifiers))
                return false;
            *modifiers |= 1;
            return true;
        }
    for (size_t i = 0; i < sizeof(symbols)-1; ++i)
        if (c == symbols[i]) { *keycode = virtuals[i]; *scancode = scans[i]; return true; }
    return false;
}

size_t gfn_input_encode_gamepad(uint8_t output[38], const GfnGamepadState *state,
                                uint64_t timestamp_us)
{
    memset(output, 0, 38);
    put_u32_le(output, 12);
    put_u16_le(output + 4, 26);
    put_u16_le(output + 6, 0);
    put_u16_le(output + 8, 1);
    put_u16_le(output + 10, 20);
    put_u16_le(output + 12, state->buttons);
    put_u16_le(output + 14, (uint16_t)(state->left_trigger | (state->right_trigger << 8)));
    put_u16_le(output + 16, (uint16_t)state->left_x);
    put_u16_le(output + 18, (uint16_t)state->left_y);
    put_u16_le(output + 20, (uint16_t)state->right_x);
    put_u16_le(output + 22, (uint16_t)state->right_y);
    put_u16_le(output + 26, 85);
    put_u64_le(output + 30, timestamp_us);
    return 38;
}

size_t gfn_input_encode_gamepad_partial(uint8_t output[54], const GfnGamepadState *state,
                                        uint64_t timestamp_us, uint16_t sequence)
{
    output[0] = 0x23;
    put_u64_be(output + 1, timestamp_us);
    output[9] = 0x26;
    output[10] = 0; /* controller slot */
    put_u16_be(output + 11, sequence);
    output[13] = 0x21;
    put_u16_be(output + 14, 38);
    gfn_input_encode_gamepad(output + 16, state, timestamp_us);
    return 54;
}

size_t gfn_input_encode_gamepad_wire(uint8_t output[50], const GfnGamepadState *state,
                                     uint64_t timestamp_us, int protocol_version)
{
    if (protocol_version <= 2)
        return gfn_input_encode_gamepad(output, state, timestamp_us);

    output[0] = 0x23;
    put_u64_be(output + 1, timestamp_us);
    output[9] = 0x21;
    output[10] = 0;
    output[11] = 38;
    gfn_input_encode_gamepad(output + 12, state, timestamp_us);
    return 50;
}

bool gfn_input_self_test(void)
{
    const GfnGamepadState state = {
        .buttons = 0x1234, .left_trigger = 0x56, .right_trigger = 0x78,
        .left_x = 1, .left_y = -2, .right_x = 3, .right_y = -4
    };
    uint8_t packet[38];
    uint8_t wire[50];
    uint8_t mouse[34], button[28], key[28], partial[54];
    uint16_t vk = 0, scan = 0, modifiers = 0;
    return gfn_input_encode_gamepad(packet, &state, 0x0102030405060708ULL) == 38 &&
           packet[0] == 12 && packet[4] == 26 && packet[8] == 1 &&
           packet[12] == 0x34 && packet[13] == 0x12 &&
           packet[14] == 0x56 && packet[15] == 0x78 &&
           packet[26] == 85 && packet[30] == 0x08 && packet[37] == 0x01 &&
           gfn_input_encode_gamepad_wire(wire, &state, 0x0102030405060708ULL, 3) == 50 &&
           wire[0] == 0x23 && wire[1] == 0x01 && wire[8] == 0x08 &&
           wire[9] == 0x21 && wire[10] == 0 && wire[11] == 38 && wire[12] == 12 &&
           gfn_input_encode_gamepad_partial(partial, &state, 0x0102030405060708ULL, 1) == 54 &&
           partial[0] == 0x23 && partial[9] == 0x26 && partial[10] == 0 &&
           partial[12] == 1 && partial[13] == 0x21 && partial[15] == 38 && partial[16] == 12 &&
           gfn_input_encode_mouse_move(mouse, 12, -8, 1, 3) == 34 &&
           mouse[9] == 0x21 && mouse[11] == 22 && mouse[12] == 7 &&
           mouse[16] == 0 && mouse[17] == 12 && mouse[18] == 0xff && mouse[19] == 0xf8 &&
           gfn_input_encode_mouse_button(button, true, 1, 3) == 28 &&
           button[9] == 0x22 && button[10] == 8 && button[14] == 1 &&
           gfn_input_encode_key(key, 0x41, 0x1e, 0, true, 1, 3) == 28 &&
           key[9] == 0x22 && key[10] == 3 && key[15] == 0x41 && key[19] == 0x1e &&
           gfn_input_key_for_char('@', &vk, &scan, &modifiers) &&
           vk == '2' && scan == 3 && modifiers == 1;
}

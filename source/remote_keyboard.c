#include "remote_keyboard.h"

#include <string.h>

#include "gfn_input.h"
#include "ui.h"

typedef enum {
    KEY_KIND_CHAR, KEY_KIND_SHIFT, KEY_KIND_BACKSPACE, KEY_KIND_SYMBOLS,
    KEY_KIND_SPACE, KEY_KIND_ENTER, KEY_KIND_TAB, KEY_KIND_ESC,
    KEY_KIND_MOUSE, KEY_KIND_DONE
} KeyKind;

typedef struct {
    UiRect rect;
    KeyKind kind;
    char character;
    const char *label;
} Key;

enum { MAX_KEYS = 56, SHIFT_OFF = 0, SHIFT_ONCE = 1, SHIFT_LOCK = 2 };

#define MARGIN 4.0f
#define GAP 3.0f
#define ROW_TOP 50.0f
#define ROW_HEIGHT 34.0f
#define ROW_GAP 3.5f

static int g_shift;
static bool g_symbols;
static int g_focus = 14;
static int g_flash = -1;
static u64 g_flash_until;
static u64 g_last_shift_at;
static bool g_last_send_ok = true;
static char g_char_labels[MAX_KEYS][2];

static const char *const LETTER_ROWS[] = { "1234567890", "qwertyuiop", "asdfghjkl", "zxcvbnm" };
static const char *const SYMBOL_ROWS[] = { "1234567890", "!@#$%^&*()", "-_=+[]{}\\", ";:'\",.?" };

static float key_width(void) { return (UI_BOTTOM_WIDTH - 2 * MARGIN - 9 * GAP) / 10.0f; }

static int add_key(Key *keys, int count, float x, float y, float w, KeyKind kind,
                   char character, const char *label)
{
    if (count >= MAX_KEYS) return count;
    keys[count].rect = (UiRect){ x, y, w, y < ROW_TOP ? 22.0f : ROW_HEIGHT };
    keys[count].kind = kind;
    keys[count].character = character;
    if (kind == KEY_KIND_CHAR) {
        char shown = character;
        if (g_shift != SHIFT_OFF && shown >= 'a' && shown <= 'z') shown = (char)(shown - 32);
        g_char_labels[count][0] = shown;
        g_char_labels[count][1] = '\0';
        keys[count].label = g_char_labels[count];
    } else {
        keys[count].label = label;
    }
    return count + 1;
}

static int build_layout(Key *keys)
{
    int count = 0;
    /* Header: two tools on each side, mirrored. */
    count = add_key(keys, count, 4.0f, 4.0f, 50.0f, KEY_KIND_ESC, 0, "ESC");
    count = add_key(keys, count, 58.0f, 4.0f, 50.0f, KEY_KIND_TAB, 0, "TAB");
    count = add_key(keys, count, 212.0f, 4.0f, 50.0f, KEY_KIND_MOUSE, 0, "MOUSE");
    count = add_key(keys, count, 266.0f, 4.0f, 50.0f, KEY_KIND_DONE, 0, "DONE");

    const char *const *rows = g_symbols ? SYMBOL_ROWS : LETTER_ROWS;
    const float kw = key_width();
    for (int row = 0; row < 3; ++row) {
        const char *chars = rows[row];
        const int n = (int)strlen(chars);
        const float span = n * kw + (n - 1) * GAP;
        float x = (UI_BOTTOM_WIDTH - span) / 2.0f;
        const float y = ROW_TOP + row * (ROW_HEIGHT + ROW_GAP);
        for (int i = 0; i < n; ++i, x += kw + GAP)
            count = add_key(keys, count, x, y, kw, KEY_KIND_CHAR, chars[i], NULL);
    }
    /* Row 4: shift, seven keys, backspace. */
    {
        const float y = ROW_TOP + 3 * (ROW_HEIGHT + ROW_GAP);
        const float wide = (UI_BOTTOM_WIDTH - 2 * MARGIN - 7 * kw - 8 * GAP) / 2.0f;
        float x = MARGIN;
        count = add_key(keys, count, x, y, wide, KEY_KIND_SHIFT, 0, "SHIFT");
        x += wide + GAP;
        const char *chars = rows[3];
        for (int i = 0; chars[i]; ++i, x += kw + GAP)
            count = add_key(keys, count, x, y, kw, KEY_KIND_CHAR, chars[i], NULL);
        count = add_key(keys, count, x, y, wide, KEY_KIND_BACKSPACE, 0, "DEL");
    }
    /* Row 5: mode, two punctuation keys around space, enter. Mirrored. */
    {
        const float y = ROW_TOP + 4 * (ROW_HEIGHT + ROW_GAP);
        const float side = 56.0f;
        const float space = UI_BOTTOM_WIDTH - 2 * MARGIN - 2 * side - 2 * kw - 4 * GAP;
        float x = MARGIN;
        count = add_key(keys, count, x, y, side, KEY_KIND_SYMBOLS, 0, g_symbols ? "ABC" : "#+=");
        x += side + GAP;
        count = add_key(keys, count, x, y, kw, KEY_KIND_CHAR, g_symbols ? '/' : '@', NULL);
        x += kw + GAP;
        count = add_key(keys, count, x, y, space, KEY_KIND_SPACE, 0, "SPACE");
        x += space + GAP;
        count = add_key(keys, count, x, y, kw, KEY_KIND_CHAR, g_symbols ? '~' : '.', NULL);
        x += kw + GAP;
        count = add_key(keys, count, x, y, side, KEY_KIND_ENTER, 0, "ENTER");
    }
    return count;
}

void remote_keyboard_open(void)
{
    g_shift = SHIFT_OFF;
    g_symbols = false;
    g_focus = 14;
    g_flash = -1;
    g_last_send_ok = true;
}

static bool send_special(WebRtcTransport *t, uint16_t vk, uint16_t scan)
{
    return webrtc_transport_send_key(t, vk, scan, 0);
}

/* Returns true when the keyboard should close. */
static bool press(WebRtcTransport *t, const Key *key, int index)
{
    g_flash = index;
    g_flash_until = osGetTime() + 140;
    switch (key->kind) {
    case KEY_KIND_CHAR: {
        char c = key->character;
        if (g_shift != SHIFT_OFF && c >= 'a' && c <= 'z') c = (char)(c - 32);
        uint16_t vk, scan, modifiers;
        if (gfn_input_key_for_char(c, &vk, &scan, &modifiers))
            g_last_send_ok = webrtc_transport_send_key(t, vk, scan, modifiers);
        if (g_shift == SHIFT_ONCE) g_shift = SHIFT_OFF;
        break;
    }
    case KEY_KIND_SHIFT: {
        /* Tap once for one capital, twice quickly for caps lock. */
        const u64 now = osGetTime();
        if (g_shift == SHIFT_ONCE && now - g_last_shift_at < 400) g_shift = SHIFT_LOCK;
        else g_shift = g_shift == SHIFT_OFF ? SHIFT_ONCE : SHIFT_OFF;
        g_last_shift_at = now;
        break;
    }
    case KEY_KIND_SYMBOLS: g_symbols = !g_symbols; break;
    case KEY_KIND_BACKSPACE: g_last_send_ok = send_special(t, 0x08, 0x0e); break;
    case KEY_KIND_SPACE: g_last_send_ok = send_special(t, 0x20, 0x39); break;
    case KEY_KIND_ENTER: g_last_send_ok = send_special(t, 0x0d, 0x1c); break;
    case KEY_KIND_TAB: g_last_send_ok = send_special(t, 0x09, 0x0f); break;
    case KEY_KIND_ESC: g_last_send_ok = send_special(t, 0x1b, 0x01); break;
    case KEY_KIND_MOUSE:
        webrtc_transport_set_pointer_mode(t, true);
        return true;
    case KEY_KIND_DONE: return true;
    }
    return false;
}

bool remote_keyboard_touch(WebRtcTransport *t, int x, int y)
{
    Key keys[MAX_KEYS];
    const int count = build_layout(keys);
    for (int i = 0; i < count; ++i) {
        /* Grow each key by half the gap so taps between keys still land. */
        UiRect hit = keys[i].rect;
        hit.x -= GAP / 2; hit.y -= ROW_GAP / 2; hit.w += GAP; hit.h += ROW_GAP;
        if (ui_hit(hit, x, y)) {
            if (i >= 4) g_focus = i;
            return press(t, &keys[i], i);
        }
    }
    return false;
}

static void move_focus(const Key *keys, int count, int dx, int dy)
{
    if (g_focus < 0 || g_focus >= count) g_focus = 14;
    const float cx = keys[g_focus].rect.x + keys[g_focus].rect.w / 2;
    const float cy = keys[g_focus].rect.y + keys[g_focus].rect.h / 2;
    int best = -1;
    float best_score = 1e9f;
    for (int i = 4; i < count; ++i) {
        if (i == g_focus) continue;
        const float x = keys[i].rect.x + keys[i].rect.w / 2 - cx;
        const float y = keys[i].rect.y + keys[i].rect.h / 2 - cy;
        const float along = dx ? x * dx : y * dy;
        const float across = dx ? (y < 0 ? -y : y) : (x < 0 ? -x : x);
        if (along < 4.0f) continue;
        /* Prefer the same row/column strongly over the nearest key. */
        const float score = along + across * 3.0f;
        if (score < best_score) { best_score = score; best = i; }
    }
    if (best >= 0) g_focus = best;
}

bool remote_keyboard_buttons(WebRtcTransport *t, u32 down)
{
    Key keys[MAX_KEYS];
    int count = build_layout(keys);
    if (g_focus < 4 || g_focus >= count) g_focus = 14;
    if (down & KEY_DUP) move_focus(keys, count, 0, -1);
    if (down & KEY_DDOWN) move_focus(keys, count, 0, 1);
    if (down & KEY_DLEFT) move_focus(keys, count, -1, 0);
    if (down & KEY_DRIGHT) move_focus(keys, count, 1, 0);
    if (down & KEY_A) return press(t, &keys[g_focus], g_focus);
    for (int i = 0; i < count; ++i) {
        const KeyKind kind = keys[i].kind;
        if (((down & KEY_B) && kind == KEY_KIND_BACKSPACE) ||
            ((down & KEY_Y) && kind == KEY_KIND_SPACE) ||
            ((down & KEY_START) && kind == KEY_KIND_ENTER) ||
            ((down & KEY_L) && kind == KEY_KIND_SHIFT) ||
            ((down & KEY_R) && kind == KEY_KIND_SYMBOLS) ||
            ((down & KEY_ZL) && kind == KEY_KIND_TAB))
            press(t, &keys[i], i);
    }
    return (down & (KEY_X | KEY_SELECT)) != 0;
}

void remote_keyboard_draw(const WebRtcTransport *t, bool touching, int tx, int ty)
{
    Key keys[MAX_KEYS];
    const int count = build_layout(keys);
    const u64 now = osGetTime();

    ui_text(UI_BOTTOM_WIDTH / 2, 1.0f, 12.0f, UI_ACCENT, UI_ALIGN_CENTER, "キーボード");
    ui_label(UI_BOTTOM_WIDTH / 2, 15.0f, 10.0f,
             !g_last_send_ok ? UI_DANGER : t->input_ready ? UI_ACCENT : UI_KIN, UI_ALIGN_CENTER,
             !g_last_send_ok ? "NOT SENT" : t->input_ready ? "CONNECTED" : "CONNECTING");
    ui_hline(0, 30.0f, UI_BOTTOM_WIDTH, UI_LINE);
    ui_rect(UI_BOTTOM_WIDTH / 2 - 12, 30.0f, 24.0f, 1.0f, UI_ACCENT);
    static const char *const hints[] = {
        "A", "Type", "B", "Delete", "Y", "Space", "START", "Enter", "X", "Close", NULL
    };
    ui_hint_row(UI_BOTTOM_WIDTH / 2, 34.0f, hints);

    for (int i = 0; i < count; ++i) {
        const Key *key = &keys[i];
        const bool held = touching && ui_hit(key->rect, tx, ty);
        const bool flash = i == g_flash && now < g_flash_until;
        const bool header = i < 4;
        u32 fill = header ? UI_BG : UI_SURFACE;
        u32 border = UI_LINE;
        u32 text = UI_TEXT;
        if (key->kind == KEY_KIND_ENTER) { fill = UI_ACCENT_DEEP; border = UI_ACCENT; }
        if (key->kind == KEY_KIND_DONE) { border = UI_ACCENT; text = UI_ACCENT; }
        if (key->kind == KEY_KIND_SHIFT && g_shift != SHIFT_OFF) {
            border = UI_ACCENT; text = UI_ACCENT;
            if (g_shift == SHIFT_LOCK) fill = UI_ACCENT_DEEP;
        }
        if (key->kind == KEY_KIND_SYMBOLS && g_symbols) { border = UI_ACCENT; text = UI_ACCENT; }
        if (key->kind == KEY_KIND_MOUSE && t->pointer_mode) { border = UI_ACCENT; text = UI_ACCENT; }
        if (held || flash) { fill = UI_ACCENT; text = UI_BG; border = UI_ACCENT; }
        ui_rect_r(key->rect, fill);
        ui_outline(key->rect.x, key->rect.y, key->rect.w, key->rect.h, 1.0f, border);
        if (i == g_focus && !held && !flash)
            ui_outline(key->rect.x - 1, key->rect.y - 1, key->rect.w + 2, key->rect.h + 2,
                       2.0f, UI_ACCENT);
        const bool small = key->kind != KEY_KIND_CHAR;
        const float size = small ? 11.0f : 16.0f;
        const float cy = key->rect.y + (key->rect.h - size) / 2.0f - (small ? 0.0f : 1.0f);
        if (small) ui_label(key->rect.x + key->rect.w / 2, cy, size, text, UI_ALIGN_CENTER, key->label);
        else ui_text(key->rect.x + key->rect.w / 2, cy, size, text, UI_ALIGN_CENTER, key->label);
        if (key->kind == KEY_KIND_SHIFT && g_shift == SHIFT_LOCK)
            ui_rect(key->rect.x + key->rect.w / 2 - 8, key->rect.y + key->rect.h - 6, 16, 2, UI_ACCENT);
    }
}

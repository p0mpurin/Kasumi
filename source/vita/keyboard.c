#include "platform.h"
#include "ui_vita.h"

#include <psp2/common_dialog.h>
#include <psp2/ime_dialog.h>
#include <psp2/libime.h>
#include <stdio.h>
#include <string.h>
#include <vita2d.h>

/* The Vita's own keyboard (the IME dialog): typing with fingers, in any
 * language the console has. It speaks UTF-16. */

#define TEXT_MAX 256

static size_t utf8_to_utf16(const char *in, SceWChar16 *out, size_t max)
{
    size_t n = 0;
    const unsigned char *p = (const unsigned char *)in;
    while (*p && n + 1 < max) {
        unsigned code = *p++;
        if (code >= 0xF0 && p[0] && p[1] && p[2]) {
            code = ((code & 0x07) << 18) | ((p[0] & 0x3F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
            p += 3;
        } else if (code >= 0xE0 && p[0] && p[1]) {
            code = ((code & 0x0F) << 12) | ((p[0] & 0x3F) << 6) | (p[1] & 0x3F);
            p += 2;
        } else if (code >= 0xC0 && p[0]) {
            code = ((code & 0x1F) << 6) | (p[0] & 0x3F);
            p += 1;
        }
        if (code > 0xFFFF) {
            if (n + 2 >= max) break;
            code -= 0x10000;
            out[n++] = (SceWChar16)(0xD800 | (code >> 10));
            out[n++] = (SceWChar16)(0xDC00 | (code & 0x3FF));
        } else {
            out[n++] = (SceWChar16)code;
        }
    }
    out[n] = 0;
    return n;
}

static void utf16_to_utf8(const SceWChar16 *in, char *out, size_t size)
{
    size_t n = 0;
    for (; *in; ++in) {
        unsigned code = *in;
        if (code >= 0xD800 && code < 0xDC00 && in[1] >= 0xDC00 && in[1] < 0xE000) {
            code = 0x10000 + ((code - 0xD800) << 10) + (in[1] - 0xDC00);
            ++in;
        }
        unsigned char bytes[4];
        size_t len;
        if (code < 0x80) { bytes[0] = (unsigned char)code; len = 1; }
        else if (code < 0x800) { bytes[0] = 0xC0 | (code >> 6); bytes[1] = 0x80 | (code & 0x3F); len = 2; }
        else if (code < 0x10000) {
            bytes[0] = 0xE0 | (code >> 12); bytes[1] = 0x80 | ((code >> 6) & 0x3F); bytes[2] = 0x80 | (code & 0x3F); len = 3;
        } else {
            bytes[0] = 0xF0 | (code >> 18); bytes[1] = 0x80 | ((code >> 12) & 0x3F);
            bytes[2] = 0x80 | ((code >> 6) & 0x3F); bytes[3] = 0x80 | (code & 0x3F); len = 4;
        }
        if (n + len >= size) break;
        memcpy(out + n, bytes, len);
        n += len;
    }
    out[n] = '\0';
}

static bool blank(const char *text)
{
    for (; *text; ++text)
        if (*text != ' ') return false;
    return true;
}

bool plat_keyboard(const PlatKeyboard *keyboard, char *text, size_t size)
{
    static SceWChar16 title[SCE_IME_DIALOG_MAX_TITLE_LENGTH];
    static SceWChar16 initial[TEXT_MAX + 1], input[TEXT_MAX + 1];
    utf8_to_utf16(keyboard->hint ? keyboard->hint : "", title, SCE_IME_DIALOG_MAX_TITLE_LENGTH);
    utf8_to_utf16(text, initial, TEXT_MAX + 1);
    memset(input, 0, sizeof(input));
    SceImeDialogParam param;
    sceImeDialogParamInit(&param);
    param.supportedLanguages = 0;
    param.languagesForced = SCE_FALSE;
    /* An IP address needs the dot, which plain numbers lack. */
    param.type = keyboard->numbers ? SCE_IME_TYPE_EXTENDED_NUMBER : SCE_IME_TYPE_DEFAULT;
    param.option = keyboard->predictive ? 0 : SCE_IME_OPTION_NO_ASSISTANCE;
    param.textBoxMode = SCE_IME_DIALOG_TEXTBOX_MODE_WITH_CLEAR;
    param.title = title;
    unsigned max = keyboard->max_length ? keyboard->max_length : TEXT_MAX;
    if (max > TEXT_MAX) max = TEXT_MAX;
    if (max > size - 1) max = (unsigned)(size - 1);
    param.maxTextLength = max;
    param.initialText = initial;
    param.inputTextBuffer = input;
    param.enterLabel = keyboard->predictive ? SCE_IME_ENTER_LABEL_SEARCH : SCE_IME_ENTER_LABEL_GO;
    if (sceImeDialogInit(&param) < 0) return false;
    bool confirmed = false;
    for (;;) {
        /* The dialog draws itself over whatever the app shows: a dark
         * screen behind it here. */
        vita2d_start_drawing();
        vita2d_set_clear_color(UI_BG);
        vita2d_clear_screen();
        vita2d_end_drawing();
        vita2d_common_dialog_update();
        vita2d_swap_buffers();
        if (sceImeDialogGetStatus() != SCE_COMMON_DIALOG_STATUS_FINISHED) continue;
        SceImeDialogResult result;
        memset(&result, 0, sizeof(result));
        sceImeDialogGetResult(&result);
        confirmed = result.button == SCE_IME_DIALOG_BUTTON_ENTER;
        sceImeDialogTerm();
        break;
    }
    if (!confirmed) return false;
    char typed[TEXT_MAX * 3 + 1];
    utf16_to_utf8(input, typed, sizeof(typed));
    if (blank(typed)) return false;
    snprintf(text, size, "%s", typed);
    return true;
}

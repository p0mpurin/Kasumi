#pragma once

/* The languages the app can show. Strings are looked up with tr(); a missing
 * translation falls back to English, and Japanese falls back to English when
 * the system font has no kana/kanji (ui_has_japanese). */
typedef enum {
    LANG_EN,
    LANG_JA,
    LANG_FR,
    LANG_DE,
    LANG_ES,
    LANG_IT,
    LANG_PT,
    LANG_COUNT
} Lang;

/* Keys for every translated string. Add a key here and in TR_TEXT (i18n.c). */
typedef enum {
    TR_BACK,
    TR_SKIP,
    TR_NEXT,
    TR_START,
    TR_COUNT
} TrKey;

void i18n_set(Lang lang);
Lang i18n_get(void);
/* The language's name in English, for the picker (no accents, so it draws
 * with any font). */
const char *i18n_name(Lang lang);
const char *tr(TrKey key);
/* Any English text the app shows, in the chosen language: looked up in the
 * table built from i18n/<code>.tsv (tools/i18n_gen.py). Text with no
 * translation, and every text in English, comes back unchanged. The drawing
 * functions in ui.c call it, so fixed text needs nothing more; a format
 * string for snprintf goes through it before the values are filled in. */
const char *i18n_text(const char *english);

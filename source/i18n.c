#include "i18n.h"

#include <stddef.h>

#include "ui.h"

static Lang g_lang = LANG_EN;

/* One row per key, one column per language. Empty entries fall back to
 * English. The French, German, Spanish, Italian and Portuguese words need a
 * native review before release. */
static const char *const TR_TEXT[TR_COUNT][LANG_COUNT] = {
    [TR_BACK] = { "BACK", "戻る", "RETOUR", "ZURÜCK", "ATRÁS", "INDIETRO", "VOLTAR" },
    [TR_SKIP] = { "SKIP", "省略", "PASSER", "ÜBERSPRINGEN", "OMITIR", "SALTA", "PULAR" },
    [TR_NEXT] = { "NEXT", "次へ", "SUIVANT", "WEITER", "SIGUIENTE", "AVANTI", "PRÓXIMO" },
    [TR_START] = { "START", "開始", "DÉMARRER", "STARTEN", "INICIAR", "AVVIA", "INICIAR" },
};

static const char *const LANG_NAMES[LANG_COUNT] = {
    "English", "Japanese", "French", "German", "Spanish", "Italian", "Portuguese",
};

void i18n_set(Lang lang)
{
    g_lang = lang < LANG_COUNT ? lang : LANG_EN;
}

Lang i18n_get(void)
{
    return g_lang;
}

const char *i18n_name(Lang lang)
{
    return LANG_NAMES[lang < LANG_COUNT ? lang : LANG_EN];
}

const char *tr(TrKey key)
{
    if (key >= TR_COUNT) return "";
    /* Without the Japanese font, kana and kanji would draw as nothing. */
    Lang lang = g_lang;
    if (lang == LANG_JA && !ui_has_japanese()) lang = LANG_EN;
    const char *text = TR_TEXT[key][lang];
    return text ? text : TR_TEXT[key][LANG_EN];
}

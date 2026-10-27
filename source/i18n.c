#include "i18n.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

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

/* The generated table (source/i18n_table.c): per language, entries sorted
 * by the FNV-1a hash of the English text. */
typedef struct {
    uint32_t hash;
    const char *text;
} I18nEntry;
extern const I18nEntry *const I18N_TABLES[LANG_COUNT];
extern const unsigned I18N_COUNTS[LANG_COUNT];

static uint32_t fnv1a(const char *text)
{
    uint32_t h = 2166136261u;
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p) h = (h ^ *p) * 16777619u;
    return h;
}

#ifdef __vita__
/* The menus and messages were written for the 3DS. On the Vita every string
 * on its way to the screen (and every translation) is reworded here: the
 * console's name, its sticks and shoulder buttons, and the face buttons in
 * the PlayStation marks (A is Circle, B Cross, X Triangle, Y Square, by
 * position as input.c reads them). */
/* lang -1: every language. A language's own word comes before the shared
 * one (German and Portuguese say C-Stick too). */
static const struct {
    int lang;
    const char *from, *to;
} VITA_WORDS[] = {
    { -1, "System Settings > Other Settings", "Settings > Date & Time" },
    { -1, "New 3DS XL", "PS Vita" },
    { -1, "New 3DS", "PS Vita" },
    { -1, "Nintendo 3DS", "PS Vita" },
    { -1, "3DS's", "Vita's" },
    { -1, "3DS", "Vita" },
    { -1, "Circle Pad", "left stick" },
    { -1, "C-STICK", "TOUCH CAM" },
    { LANG_JA, "C スティック", "右スティック" },
    { LANG_JA, "Cスティック", "右スティック" },
    { LANG_FR, "stick C", "stick droit" },
    { LANG_ES, "stick C", "stick derecho" },
    { LANG_IT, "stick C", "stick destro" },
    { LANG_DE, "C-Stick", "rechter Stick" },
    { LANG_PT, "C-Stick", "analógico direito" },
    { LANG_PT, "C-stick", "analógico direito" },
    { -1, "C-Stick", "right stick" },
    { -1, "C-stick", "right stick" },
    { -1, "HOME Menu", "home screen" },
    { -1, "lower screen", "screen" },
    { -1, "SD card", "memory card" },
    { -1, "ZL", "L2" },
    { -1, "ZR", "R2" },
};

static bool is_word_char(unsigned char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); }

/* A lone A / B / X / Y that names a button: after "Press", "(", ...; before
 * "to", ":", "で", ... ("A Kasumi theme" and "A NVIDIA" are words, and
 * so are the Xbox button names in "A, B, LB, RT"). */
static const char *const BUTTON_BEFORE[] = {
    "Press ", "press ", "Tap ", "tap ", "(", "（", "then ", "or ", "and ", "> ", "Aperte ", "aperte ", "Appuyez sur ",
    "appuyez sur ", "Premi ", "premi ", "Pulsa ", "pulsa ", "Drücke ", "drücke ", "Drück ", "drück ", "mit ", "sur ",
    "con ", "com ", "puis ", "dann ", "luego ", "poi ", "depois ", "、", "：", ": ", "; ",
};
static const char *const BUTTON_AFTER[] = {
    " to ", ")", "）", ":", " >", " für", " zum", " zur", " pour", " para", " per ", " で", "で", " を", "を", " or ",
    " oder ", " ou ", " o ", " deletes", " löscht", " searches", " sucht", " cherche", " busca", " cerca", " procura",
    " loads", " lädt", " charge", " carga", " carica", " carrega", " startet", " spielt", " geht",
};

static bool starts_with(const char *text, const char *prefix) { return !strncmp(text, prefix, strlen(prefix)); }

static bool ends_with(const char *start, const char *at, const char *suffix)
{
    const size_t n = strlen(suffix);
    return (size_t)(at - start) >= n && !strncmp(at - n, suffix, n);
}

static const char *button_mark(const char *start, const char *p)
{
    const char c = *p;
    if (c != 'A' && c != 'B' && c != 'X' && c != 'Y') return NULL;
    if (p > start && (is_word_char((unsigned char)p[-1]) || p[-1] == '-' || p[-1] == '/' || p[-1] == '.')) return NULL;
    if (is_word_char((unsigned char)p[1]) || p[1] == '-' || p[1] == '/') return NULL;
    if (strstr(start, "LB, RT") || strstr(start, "LB、RT")) return NULL;
    bool button = false;
    for (size_t i = 0; i < sizeof(BUTTON_BEFORE) / sizeof(BUTTON_BEFORE[0]) && !button; ++i)
        button = ends_with(start, p, BUTTON_BEFORE[i]);
    for (size_t i = 0; i < sizeof(BUTTON_AFTER) / sizeof(BUTTON_AFTER[0]) && !button; ++i)
        button = starts_with(p + 1, BUTTON_AFTER[i]);
    if (!button) return NULL;
    return c == 'A' ? "\xE2\x97\x8B" : c == 'B' ? "\xC3\x97" : c == 'X' ? "\xE2\x96\xB3" : "\xE2\x96\xA1";
}

static const char *vita_words(const char *text)
{
    if (!text || !text[0]) return text;
    /* Most strings have nothing to reword: no copy for those. */
    bool any = false;
    for (size_t i = 0; i < sizeof(VITA_WORDS) / sizeof(VITA_WORDS[0]) && !any; ++i)
        any = strstr(text, VITA_WORDS[i].from) != NULL;
    for (const char *p = text; *p && !any; ++p) any = button_mark(text, p) != NULL;
    if (!any) return text;
    /* A ring of buffers: callers use the text at once, and reworded text has
     * nothing left to reword, so drawing it again copies nothing. */
    static char ring[64][640];
    static unsigned next;
    char *out = ring[__atomic_fetch_add(&next, 1, __ATOMIC_RELAXED) % 64];
    size_t n = 0;
    for (const char *p = text; *p && n < sizeof(ring[0]) - 8;) {
        const char *to = NULL;
        size_t from_len = 0;
        for (size_t i = 0; i < sizeof(VITA_WORDS) / sizeof(VITA_WORDS[0]); ++i) {
            if (VITA_WORDS[i].lang >= 0 && VITA_WORDS[i].lang != (int)g_lang) continue;
            const size_t len = strlen(VITA_WORDS[i].from);
            if (strncmp(p, VITA_WORDS[i].from, len)) continue;
            /* ZL / ZR only as words (not inside a longer name). */
            if (len == 2 && ((p > text && is_word_char((unsigned char)p[-1])) || is_word_char((unsigned char)p[2])))
                continue;
            to = VITA_WORDS[i].to;
            from_len = len;
            break;
        }
        if (!to && (to = button_mark(text, p)) != NULL) from_len = 1;
        if (!to) {
            out[n++] = *p++;
            continue;
        }
        const size_t len = strlen(to);
        if (n + len >= sizeof(ring[0]) - 8) break;
        memcpy(out + n, to, len);
        n += len;
        p += from_len;
    }
    out[n] = '\0';
    return out;
}
#endif

static const char *lookup(const char *english)
{
    if (!english || !english[0] || g_lang == LANG_EN) return english;
    Lang lang = g_lang;
    if (lang == LANG_JA && !ui_has_japanese()) return english;
    const I18nEntry *table = I18N_TABLES[lang];
    unsigned lo = 0, hi = I18N_COUNTS[lang];
    if (!table || !hi) return english;
    const uint32_t h = fnv1a(english);
    while (lo < hi) {
        const unsigned mid = (lo + hi) / 2;
        if (table[mid].hash < h) lo = mid + 1;
        else hi = mid;
    }
    return lo < I18N_COUNTS[lang] && table[lo].hash == h ? table[lo].text : english;
}

const char *i18n_text(const char *english)
{
#ifdef __vita__
    return vita_words(lookup(english));
#else
    return lookup(english);
#endif
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

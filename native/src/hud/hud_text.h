/* Texts for the HUD text areas. The phone sends anything (NBSP, direction marks, emoji, decomposed letters,
 * names longer than the 32 characters a text area takes); the cluster font is unknown. So every text is
 * cleaned to plain BMP characters, shortened the way a map would ("вул.", "Str."), cut to fit with a
 * trailing dot ("Васильк."), and can be transliterated to ASCII Latin if the HUD shows no Cyrillic. Pure code. */
#ifndef HUD_TEXT_H
#define HUD_TEXT_H

#include <stddef.h>

enum hud_lang { HUD_LANG_UK, HUD_LANG_EN, HUD_LANG_DE, HUD_LANG_PL, HUD_LANG_COUNT };
/* NATIVE keeps every BMP letter; LATIN gives ASCII only (Ukrainian per the 2010 national transliteration). */
enum hud_script { HUD_SCRIPT_NATIVE, HUD_SCRIPT_LATIN };
enum { HUD_STREET_MAX_DEFAULT = 20, HUD_TEXT_UNITS = 32, HUD_TEXT_BYTES = HUD_TEXT_UNITS * 3 + 1 };

typedef struct {
    int lang;          /* distance units and fixed words */
    int script;
    int street_max;    /* characters, 8..32 */
} hud_text_style;

void hud_text_style_default(hud_text_style *s);
const char *hud_lang_name(int lang);
int hud_lang_parse(const char *s, size_t n);   /* -1 when unknown */

/* Cleans UTF-8 text: drops control, format and non-BMP characters, joins decomposed letters, folds typographic
   quotes and dashes, collapses spaces; LATIN also transliterates. Cuts to max_units characters (dot rule).
   Returns the characters written (each is one UTF-16 unit). out is always terminated. */
int hud_text_clean(const char *in, int script, int max_units, char *out, size_t cap);
/* A street name: hud_text_clean with the usual abbreviations, cut to style->street_max. */
int hud_text_street(const char *in, const hud_text_style *s, char *out, size_t cap);
/* "450 м", "1,2 км", "12 км", "Зараз", "Ви прибули" (or the style's language), cleaned for its script. */
void hud_text_distance(int dist_m, int arrive, const hud_text_style *s, char *out, size_t cap);

#endif

#include "hud_text.h"

#include <stdio.h>
#include <string.h>

enum { MAX_CP = 256 };
#define BAD 0xFFFDu

static const char *const k_lang[HUD_LANG_COUNT] = { "uk", "en", "de", "pl" };

void hud_text_style_default(hud_text_style *s)
{
    s->lang = HUD_LANG_UK;
    s->script = HUD_SCRIPT_NATIVE;
    s->street_max = HUD_STREET_MAX_DEFAULT;
}

const char *hud_lang_name(int lang) { return lang >= 0 && lang < HUD_LANG_COUNT ? k_lang[lang] : k_lang[0]; }

int hud_lang_parse(const char *s, size_t n)
{
    int i;
    for (i = 0; i < HUD_LANG_COUNT; i++)
        if (strlen(k_lang[i]) == n && !memcmp(s, k_lang[i], n)) return i;
    return -1;
}

/* ---- code points ---- */

/* Strict UTF-8: overlong forms, surrogates, stray bytes and anything outside the BMP become BAD. */
static int decode(const char *in, unsigned *cp, int cap)
{
    const unsigned char *s = (const unsigned char *)in;
    int n = 0;
    while (*s && n < cap) {
        unsigned c = *s++, v, need, min;
        if (c < 0x80) { cp[n++] = c; continue; }
        if ((c & 0xE0) == 0xC0) { v = c & 0x1F; need = 1; min = 0x80; }
        else if ((c & 0xF0) == 0xE0) { v = c & 0x0F; need = 2; min = 0x800; }
        else if ((c & 0xF8) == 0xF0) { v = c & 0x07; need = 3; min = 0x10000; }
        else { cp[n++] = BAD; continue; }
        while (need && (*s & 0xC0) == 0x80) { v = v << 6 | (*s++ & 0x3F); need--; }
        cp[n++] = need || v < min || v > 0xFFFF || (v >= 0xD800 && v <= 0xDFFF) ? BAD : v;
    }
    return n;
}

static void encode(const unsigned *cp, int n, char *out, size_t cap)
{
    size_t k = 0;
    int i;
    if (!cap) return;
    for (i = 0; i < n; i++) {
        unsigned c = cp[i];
        size_t need = c < 0x80 ? 1 : c < 0x800 ? 2 : 3;
        if (k + need >= cap) break;
        if (need == 1) out[k++] = (char)c;
        else if (need == 2) { out[k++] = (char)(0xC0 | c >> 6); out[k++] = (char)(0x80 | (c & 0x3F)); }
        else {
            out[k++] = (char)(0xE0 | c >> 12);
            out[k++] = (char)(0x80 | (c >> 6 & 0x3F));
            out[k++] = (char)(0x80 | (c & 0x3F));
        }
    }
    out[k] = 0;
}

static int is_cyr(unsigned c) { return (c >= 0x400 && c <= 0x45F) || c == 0x490 || c == 0x491; }
static int is_cyr_upper(unsigned c) { return (c >= 0x400 && c <= 0x42F) || c == 0x490; }

static unsigned cyr_lower(unsigned c)
{
    if (c >= 0x410 && c <= 0x42F) return c + 0x20;
    if (c >= 0x400 && c <= 0x40F) return c + 0x50;
    return c == 0x490 ? 0x491 : c;
}

static int is_letter(unsigned c)
{
    return ((c | 0x20) >= 'a' && (c | 0x20) <= 'z') || (c >= 0xC0 && c <= 0x24F && c != 0xD7 && c != 0xF7) ||
           (c >= 0x370 && c <= 0x3FF) || is_cyr(c) || (c >= 0x500 && c <= 0x52F);
}

static int is_space(unsigned c)
{
    return c == ' ' || c < 0x20 || (c >= 0x7F && c <= 0xA0) || c == 0x1680 || (c >= 0x2000 && c <= 0x200A) ||
           c == 0x2028 || c == 0x2029 || c == 0x202F || c == 0x205F || c == 0x3000;
}

/* Zero-width, direction and other format characters, variation selectors, private use, specials. */
static int is_invisible(unsigned c)
{
    return c == BAD || c == 0xAD || c == 0x34F || c == 0x61C || c == 0x180E || (c >= 0x200B && c <= 0x200F) ||
           (c >= 0x202A && c <= 0x202E) || (c >= 0x2060 && c <= 0x206F) || (c >= 0xE000 && c <= 0xF8FF) ||
           (c >= 0xFE00 && c <= 0xFE0F) || c == 0xFEFF || c >= 0xFFF0;
}

static int is_mark(unsigned c)
{
    return (c >= 0x300 && c <= 0x36F) || (c >= 0x483 && c <= 0x489) || (c >= 0x1AB0 && c <= 0x1AFF) ||
           (c >= 0x1DC0 && c <= 0x1DFF) || (c >= 0x20D0 && c <= 0x20FF) || (c >= 0xFE20 && c <= 0xFE2F);
}

/* Letter + combining mark the phone may send decomposed (й as и + U+0306). Others lose the mark. */
static unsigned compose(unsigned base, unsigned mark)
{
    static const unsigned k[][3] = {
        { 0x438, 0x306, 0x439 }, { 0x418, 0x306, 0x419 }, { 0x456, 0x308, 0x457 }, { 0x406, 0x308, 0x407 },
        { 0x435, 0x308, 0x451 }, { 0x415, 0x308, 0x401 }, { 0x443, 0x306, 0x45E }, { 0x423, 0x306, 0x40E },
        { 'a', 0x301, 0xE1 }, { 'e', 0x301, 0xE9 }, { 'i', 0x301, 0xED }, { 'o', 0x301, 0xF3 }, { 'u', 0x301, 0xFA },
        { 'E', 0x301, 0xC9 }, { 'a', 0x300, 0xE0 }, { 'e', 0x300, 0xE8 }, { 'a', 0x308, 0xE4 }, { 'o', 0x308, 0xF6 },
        { 'u', 0x308, 0xFC }, { 'A', 0x308, 0xC4 }, { 'O', 0x308, 0xD6 }, { 'U', 0x308, 0xDC }, { 'n', 0x303, 0xF1 },
        { 'c', 0x301, 0x107 }, { 'n', 0x301, 0x144 }, { 's', 0x301, 0x15B }, { 'z', 0x301, 0x17A },
        { 'S', 0x301, 0x15A }, { 'z', 0x307, 0x17C }, { 'Z', 0x307, 0x17B }, { 'a', 0x328, 0x105 },
        { 'e', 0x328, 0x119 }, { 'c', 0x30C, 0x10D }, { 's', 0x30C, 0x161 }, { 'z', 0x30C, 0x17E },
        { 'S', 0x30C, 0x160 }, { 'Z', 0x30C, 0x17D },
    };
    size_t i;
    for (i = 0; i < sizeof k / sizeof k[0]; i++)
        if (k[i][0] == base && k[i][1] == mark) return k[i][2];
    return 0;
}

/* Typographic punctuation the cluster font may not have, and the comma: the 6WB cluster's text areas drop it
   ("5,4" shows as "5 4", JayJay3071/nbt-android-auto, car 2026-11-10), so it becomes a space. 0: keep it. */
static unsigned fold_punct(unsigned c)
{
    if (c == ',') return ' ';
    if (c == 0x2018 || c == 0x2019 || c == 0x201A || c == 0x201B || c == 0x2032 || c == 0x2BC || c == 0x2BB ||
        c == 0xB4 || c == '`')
        return '\'';
    if ((c >= 0x201C && c <= 0x201F) || c == 0xAB || c == 0xBB || c == 0x2033 || c == 0x2039 || c == 0x203A)
        return '"';
    if ((c >= 0x2010 && c <= 0x2015) || c == 0x2212 || c == 0x2E3A || c == 0x2E3B || c == 0x2022) return '-';
    return 0;
}

/* Pass 1: drop what can't show, join decomposed letters, fold punctuation, one space between words. */
static int clean(const unsigned *in, int n, unsigned *out, int cap)
{
    int i, k = 0;
    for (i = 0; i < n && k < cap - 3; i++) {
        unsigned c = in[i], f;
        if (is_mark(c)) {
            if (k && (f = compose(out[k - 1], c))) out[k - 1] = f;
            continue;
        }
        if (is_invisible(c)) continue;
        if (is_space(c)) {
            if (k && out[k - 1] != ' ') out[k++] = ' ';
            continue;
        }
        if (c == 0x2026) {   /* … */
            out[k++] = '.'; out[k++] = '.'; out[k++] = '.';
            continue;
        }
        f = fold_punct(c);
        if (f == ' ') {
            if (k && out[k - 1] != ' ') out[k++] = ' ';
            continue;
        }
        out[k++] = f ? f : c;
    }
    while (k && out[k - 1] == ' ') k--;
    return k;
}

/* ---- abbreviations (whole words; a few as word endings: "Hauptstraße" -> "Hauptstr.") ---- */

static const struct { const char *from, *to; int suffix; } k_abbr[] = {
    { "вулиця", "вул.", 0 }, { "Вулиця", "Вул.", 0 }, { "проспект", "просп.", 0 }, { "Проспект", "Просп.", 0 },
    { "провулок", "пров.", 0 }, { "Провулок", "Пров.", 0 }, { "площа", "пл.", 0 }, { "Площа", "Пл.", 0 },
    { "бульвар", "бульв.", 0 }, { "Бульвар", "Бульв.", 0 }, { "набережна", "наб.", 0 }, { "Набережна", "Наб.", 0 },
    { "улица", "ул.", 0 }, { "Улица", "Ул.", 0 }, { "переулок", "пер.", 0 }, { "Переулок", "Пер.", 0 },
    { "площадь", "пл.", 0 }, { "Площадь", "Пл.", 0 }, { "набережная", "наб.", 0 }, { "Набережная", "Наб.", 0 },
    { "Street", "St", 0 }, { "Avenue", "Ave", 0 }, { "Road", "Rd", 0 }, { "Boulevard", "Blvd", 0 },
    { "Drive", "Dr", 0 }, { "Lane", "Ln", 0 }, { "Square", "Sq", 0 }, { "Highway", "Hwy", 0 },
    { "Straße", "Str.", 0 }, { "Strasse", "Str.", 0 }, { "straße", "str.", 1 }, { "strasse", "str.", 1 },
    { "ulica", "ul.", 0 }, { "Ulica", "Ul.", 0 }, { "aleja", "al.", 0 }, { "Aleja", "Al.", 0 },
    { "plac", "pl.", 0 }, { "Plac", "Pl.", 0 },
};

static int abbreviate(unsigned *cp, int n)
{
    unsigned from[16], to[8], tmp[MAX_CP];
    int i = 0, k = 0;
    while (i < n) {
        int j = i, done = 0;
        size_t a;
        if (!is_letter(cp[i])) { tmp[k++] = cp[i++]; continue; }
        while (j < n && is_letter(cp[j])) j++;
        for (a = 0; a < sizeof k_abbr / sizeof k_abbr[0] && !done; a++) {
            int nf = decode(k_abbr[a].from, from, 16), nt = decode(k_abbr[a].to, to, 8), at = j - nf;
            if (at < i || (!k_abbr[a].suffix && at != i) || (k_abbr[a].suffix && at == i)) continue;
            if (memcmp(cp + at, from, sizeof(unsigned) * (size_t)nf)) continue;
            memcpy(tmp + k, cp + i, sizeof(unsigned) * (size_t)(at - i));
            k += at - i;
            memcpy(tmp + k, to, sizeof(unsigned) * (size_t)nt);
            k += nt;
            if (to[nt - 1] == '.' && j < n && cp[j] == '.') j++;   /* "вулиця." keeps one dot */
            done = 1;
        }
        if (!done) { memcpy(tmp + k, cp + i, sizeof(unsigned) * (size_t)(j - i)); k += j - i; }
        i = j;
    }
    memcpy(cp, tmp, sizeof(unsigned) * (size_t)k);
    return k;
}

/* ---- transliteration to ASCII ---- */

static const char *const k_cyr[32] = {   /* а..я */
    "a", "b", "v", "h", "d", "e", "zh", "z", "y", "i", "k", "l", "m", "n", "o", "p",
    "r", "s", "t", "u", "f", "kh", "ts", "ch", "sh", "shch", "", "y", "", "e", "iu", "ia",
};
static const char *const k_cyr_ext[16] = {   /* ѐ..џ */
    "e", "io", "dj", "g", "ie", "dz", "i", "i", "j", "lj", "nj", "c", "k", "i", "u", "dz",
};
/* Latin-1 and Latin Extended-A letters without their marks; '*' marks the two-letter ones handled below. */
static const char k_latin1[] = "AAAAAA*CEEEEIIIIDNOOOOOxOUUUUY*saaaaaa*ceeeeiiiidnooooo/ouuuuy*y";
static const char k_latin_a[] =
    "AaAaAaCcCcCcCcDdDdEeEeEeEeEeGgGgGgGgHhHhIiIiIiIiIiIiJjKkkLlLlLlLlLlNnNnNnnNnOoOoOoOoRrRrRrSsSsSsSsTtTtTtUuUuUuUuUuUuWwYyYZzZzZzs";
typedef char k_latin1_is_c0_to_ff[sizeof k_latin1 == 0x41 ? 1 : -1];
typedef char k_latin_a_is_100_to_17f[sizeof k_latin_a == 0x81 ? 1 : -1];

static const char *latin_of(unsigned c, int word_start, unsigned prev)
{
    static char one[2];
    unsigned l;
    switch (c) {
    case 0xC6: return "AE";
    case 0xE6: return "ae";
    case 0xDE: return "Th";
    case 0xFE: return "th";
    case 0xDF: return "ss";
    case 0x152: return "OE";
    case 0x153: return "oe";
    case 0x2116: return "No";
    }
    if (c >= 0xC0 && c <= 0xFF) { one[0] = k_latin1[c - 0xC0]; one[1] = 0; return one; }
    if (c >= 0x100 && c <= 0x17F) { one[0] = k_latin_a[c - 0x100]; one[1] = 0; return one; }
    if (!is_cyr(c)) return NULL;
    l = cyr_lower(c);
    if (word_start) {   /* є ї й ю я start a word as ye yi y yu ya */
        if (l == 0x454) return "ye";
        if (l == 0x457) return "yi";
        if (l == 0x439) return "y";
        if (l == 0x44E) return "yu";
        if (l == 0x44F) return "ya";
        if (l == 0x451) return "yo";
    }
    if (l == 0x433 && cyr_lower(prev) == 0x437) return "gh";   /* зг -> zgh */
    if (l == 0x491) return "g";
    if (l >= 0x430 && l <= 0x44F) return k_cyr[l - 0x430];
    if (l >= 0x450 && l <= 0x45F) return k_cyr_ext[l - 0x450];
    return NULL;
}

static int to_latin(unsigned *cp, int n)
{
    unsigned tmp[MAX_CP];
    int i, k = 0;
    for (i = 0; i < n && k < MAX_CP - 4; i++) {
        unsigned c = cp[i], prev = i ? cp[i - 1] : ' ', next = i + 1 < n ? cp[i + 1] : ' ';
        const char *s;
        size_t j, len;
        int upper_all;
        if (c < 0x80) {
            if (c == '\'' && is_cyr(prev) && is_cyr(next)) continue;   /* the Ukrainian apostrophe is dropped */
            tmp[k++] = c;
            continue;
        }
        /* after the dropped apostrophe a word goes on: Мар'янівка -> Marianivka */
        if (!(s = latin_of(c, !is_letter(prev) && !(prev == '\'' && i > 1 && is_cyr(cp[i - 2])), prev))) continue;
        len = strlen(s);
        /* Щ -> Shch in a name, SHCH in a word written in capitals */
        upper_all = is_cyr_upper(c) && len > 1 && (is_cyr_upper(next) || (is_cyr_upper(prev) && !is_letter(next)));
        for (j = 0; j < len && k < MAX_CP; j++) {
            unsigned x = (unsigned char)s[j];
            if (is_cyr_upper(c) && (j == 0 || upper_all) && x >= 'a' && x <= 'z') x -= 32;
            tmp[k++] = x;
        }
    }
    return clean(tmp, k, cp, MAX_CP);   /* dropped characters may leave double spaces */
}

/* ---- fitting ---- */

/* Vowels, soft and hard signs: a cut word should not end on one ("Васильк.", not "Васильківсь."). */
static int is_vowelish(unsigned c)
{
    static const unsigned k_cyr_v[] = { 0x430, 0x435, 0x438, 0x43E, 0x443, 0x44A, 0x44B, 0x44C, 0x44D, 0x44E,
                                        0x44F, 0x451, 0x454, 0x456, 0x457 };
    size_t i;
    if (c < 0x80) return c == '\'' || ((c | 0x20) >= 'a' && strchr("aeiouy", (int)(c | 0x20)) != NULL);
    if (c >= 0xC0 && c <= 0x24F) {
        const char *s = latin_of(c, 0, ' ');
        return s && strchr("aeiouyAEIOUY", s[0]) != NULL;
    }
    c = cyr_lower(c);
    for (i = 0; i < sizeof k_cyr_v / sizeof k_cyr_v[0]; i++)
        if (c == k_cyr_v[i]) return 1;
    return 0;
}

/* Cuts to max characters the way a map label would: a word crossing the cut keeps at least 3 letters, ends on
   a consonant and gets a dot ("Васильківська" -> "Васильк."); otherwise the crossing token is dropped. */
static int fit_plain(unsigned *cp, int n, int max)
{
    int keep = max - 1, start, k;
    if (n <= max) return n;
    if (max < 2) return max < 0 ? 0 : max;
    start = keep;
    while (start > 0 && is_letter(cp[start - 1])) start--;
    if (is_letter(cp[keep]) && keep - start >= 3) {
        k = keep;
        while (k - start > 3 && is_vowelish(cp[k - 1])) k--;
        cp[k] = '.';
        return k + 1;
    }
    k = max;
    if (cp[k] != ' ') while (k > 0 && cp[k - 1] != ' ') k--;   /* drop the token crossing the cut */
    while (k > 0 && (cp[k - 1] == ' ' || cp[k - 1] == ',' || cp[k - 1] == ';' || cp[k - 1] == ':' ||
                     cp[k - 1] == '-' || cp[k - 1] == '/' || cp[k - 1] == '('))
        k--;
    if (k > 0) return k;
    cp[keep] = '.';   /* one long token: hard cut */
    return max;
}

/* A trailing house number survives: "вул. Сагайдачного, 25" -> "вул. Сагайдачн., 25". */
static int fit(unsigned *cp, int n, int max)
{
    int tail = n, sep, head, i;
    if (n <= max) return n;
    while (tail > 0 && cp[tail - 1] != ' ') tail--;
    sep = tail;
    while (sep > 0 && (cp[sep - 1] == ' ' || cp[sep - 1] == ',')) sep--;
    if (tail > 0 && sep > 0 && cp[tail] >= '0' && cp[tail] <= '9' && n - tail <= 6 && max - (n - sep) >= 6) {
        head = fit_plain(cp, sep, max - (n - sep));
        for (i = sep; i < n; i++) cp[head + i - sep] = cp[i];
        return head + n - sep;
    }
    return fit_plain(cp, n, max);
}

static int prepare(const char *in, int script, int abbr, int max, char *out, size_t cap)
{
    unsigned cp[MAX_CP], raw[MAX_CP];
    int n;
    if (max > HUD_TEXT_UNITS) max = HUD_TEXT_UNITS;
    if (max < 0) max = 0;
    n = clean(raw, decode(in ? in : "", raw, MAX_CP), cp, MAX_CP);
    if (abbr) n = abbreviate(cp, n);
    if (script == HUD_SCRIPT_LATIN) n = to_latin(cp, n);
    n = fit(cp, n, max);
    encode(cp, n, out, cap);
    return n;
}

int hud_text_clean(const char *in, int script, int max_units, char *out, size_t cap)
{
    return prepare(in, script, 0, max_units, out, cap);
}

int hud_text_street(const char *in, const hud_text_style *s, char *out, size_t cap)
{
    int max = s->street_max < 8 ? 8 : s->street_max;
    return prepare(in, s->script, 1, max, out, cap);
}

void hud_text_distance(int d, int arrive, const hud_text_style *s, char *out, size_t cap)
{
    static const char *const k_words[HUD_LANG_COUNT][4] = {   /* m, km, now, arrived */
        { "м", "км", "Зараз", "Ви прибули" },
        { "m", "km", "Now", "Arrived" },
        { "m", "km", "Jetzt", "Ziel erreicht" },
        { "m", "km", "Teraz", "Na miejscu" },
    };
    const char *const *w = k_words[s->lang >= 0 && s->lang < HUD_LANG_COUNT ? s->lang : 0];
    char raw[64], dec = '.';   /* not the Ukrainian comma: the cluster drops commas (fold_punct) */
    int r;
    if (d < 0) d = 0;
    r = d < 300 ? (d + 5) / 10 * 10 : (d + 25) / 50 * 50;   /* 10 m steps when close, 50 m further out */
    if (arrive && d <= 30) snprintf(raw, sizeof raw, "%s", w[3]);
    else if (r == 0) snprintf(raw, sizeof raw, "%s", w[2]);
    else if (r < 1000) snprintf(raw, sizeof raw, "%d %s", r, w[0]);
    else if (d < 9950) snprintf(raw, sizeof raw, "%d%c%d %s", (d + 50) / 100 / 10, dec, (d + 50) / 100 % 10, w[1]);
    else snprintf(raw, sizeof raw, "%d %s", (d + 500) / 1000, w[1]);
    hud_text_clean(raw, s->script, HUD_TEXT_UNITS, out, cap);
}

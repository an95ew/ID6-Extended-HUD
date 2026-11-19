/* The plugin's CDS probe (car data service, libAppFramework's cdsAddPropertyChangedEventHandler): which properties to
 * watch, what their values say, and the drive_frame they feed the worker. Pure code; extended_hud.cpp makes the calls.
 *
 * Off unless the app folder holds cds.txt, one property per line: "<name> <kind> [interval]", kind log (only log it),
 * rpm, kph, gear or mode (also send it to the worker), interval the handler's unsigned argument (its meaning is not
 * known yet: an update limit in ms, like the Connected apps' intervalLimit, is the guess; default 100). '#' comments.
 * Names and value formats are unknown on this firmware: car run 6 finds them (step C1). */
#ifndef HUD_CDS_H
#define HUD_CDS_H

#include <stddef.h>
#include <stdint.h>

enum { HUD_CDS_MAX = 16, HUD_CDS_NAME = 64, HUD_CDS_TEXT = 160 };
typedef enum { HUD_CDS_LOG, HUD_CDS_RPM, HUD_CDS_KPH, HUD_CDS_GEAR, HUD_CDS_MODE } hud_cds_kind;

typedef struct {
    char name[HUD_CDS_NAME];
    hud_cds_kind kind;
    unsigned interval;
} hud_cds_prop;

/* The lines of cds.txt; bad lines are skipped. Returns how many properties it took (at most HUD_CDS_MAX). */
int hud_cds_parse(const char *text, size_t len, hud_cds_prop *out, int max);
/* A UTF-16 value (as the framework's BasicString<unsigned short> holds it, 0-terminated, read at most max units) as
   printable ASCII ('?' for the rest), and its first integer (an optional '-' then digits), or -1 when it has none.
   Returns the number of units read. */
int hud_cds_value(const uint16_t *s, int max, char *ascii, size_t cap, long *number);
struct drive_frame;
/* The frame for a value of kind (not LOG): only that field set, the rest -1. 0 when there is nothing to send. */
int hud_cds_frame(hud_cds_kind kind, long number, uint64_t now_ms, struct drive_frame *f);

#endif

/*
 * wine-atspi bridge, AT-SPI side: units of text for the Text interface
 * (textrange.c). GLib only, no D-Bus, so tests/unit.c checks it natively
 * (make check).
 */
#ifndef WINE_ATSPI_TEXTRANGE_H
#define WINE_ATSPI_TEXTRANGE_H

#include <glib.h>

/* AtspiTextGranularity */
enum { GRANULARITY_CHAR, GRANULARITY_WORD, GRANULARITY_SENTENCE, GRANULARITY_LINE,
       GRANULARITY_PARAGRAPH };

void text_range(const gunichar *chars, gint count, gint offset, guint granularity,
                gint *start, gint *end);

#endif

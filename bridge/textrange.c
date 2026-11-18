/*
 * wine-atspi bridge, AT-SPI side: units of text for the Text interface.
 * GLib only, see textrange.h. Like atspi.c, no C library calls.
 */
#include "textrange.h"

static gboolean is_word_char(gunichar c)
{
    return g_unichar_isalnum(c) || g_unichar_ismark(c);
}

/*
 * Range [*start, *end) of the unit of granularity at offset in the count
 * chars, with the ATK meaning: a word runs to the start of the next word,
 * a line includes its "\n". At the end of text ending in "\n" the line is
 * empty. Sentences and paragraphs are logical lines for now.
 */
void text_range(const gunichar *c, gint n, gint offset, guint granularity,
                gint *start, gint *end)
{
    gint i;

    offset = CLAMP(offset, 0, n);
    switch (granularity)
    {
    case GRANULARITY_CHAR:
        *start = offset;
        *end = MIN(offset + 1, n);
        return;
    case GRANULARITY_WORD:
        for (i = offset; i > 0; i--)
            if (i < n && is_word_char(c[i]) && !is_word_char(c[i - 1]))
                break;
        *start = i;
        for (i = offset + 1; i < n; i++)
            if (is_word_char(c[i]) && !is_word_char(c[i - 1]))
                break;
        *end = MIN(i, n);
        return;
    default:
        for (i = offset; i > 0 && c[i - 1] != '\n'; i--)
            ;
        *start = i;
        for (i = offset; i < n && c[i] != '\n'; i++)
            ;
        *end = MIN(i + 1, n);
        return;
    }
}

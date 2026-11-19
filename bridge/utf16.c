/*
 * wine-atspi bridge, Win32 core: pure helpers on UTF-16 text. No Windows
 * API here, see utf16.h.
 */
#include <stddef.h>

#include "utf16.h"

static int is_high_surrogate(utf16 c)
{
    return c >= 0xd800 && c <= 0xdbff;
}

static int is_low_surrogate(utf16 c)
{
    return c >= 0xdc00 && c <= 0xdfff;
}

/*
 * Character offset of UTF-16 position pos in text as the AT-SPI side sees
 * it: surrogate pairs count once and the "\r" of "\r\n" is dropped.
 */
int char_offset(const utf16 *text, int len, int pos)
{
    int i, offset = 0;

    for (i = 0; i < pos && i < len; i++)
    {
        if (is_low_surrogate(text[i]) && i > 0 && is_high_surrogate(text[i - 1]))
            continue;
        if (text[i] == '\r' && i + 1 < len && text[i + 1] == '\n')
            continue;
        offset++;
    }
    return offset;
}

/*
 * Code points of len units of text as the AT-SPI side holds them, written
 * to out unless it is NULL: "\r\n" becomes "\n", a surrogate pair one code
 * point, a lone surrogate U+FFFD. Returns their number, which is
 * char_offset(text, len, len).
 */
int utf16_to_chars(const utf16 *text, int len, unsigned int *out)
{
    int i, count = 0;
    unsigned int c;

    for (i = 0; i < len; i++)
    {
        c = text[i];
        if (c == '\r' && i + 1 < len && text[i + 1] == '\n')
            continue;
        if (is_high_surrogate(c) && i + 1 < len && is_low_surrogate(text[i + 1]))
        {
            c = 0x10000 + ((c - 0xd800) << 10) + (text[i + 1] - 0xdc00);
            i++;
        }
        else if (is_high_surrogate(c) || is_low_surrogate(c))
            c = 0xfffd;
        if (out)
            out[count] = c;
        count++;
    }
    return count;
}

/* Whether a cut at pos falls inside a "\r\n" or a surrogate pair. */
static int splits_pair(const utf16 *text, int len, int pos)
{
    return pos > 0 && pos < len &&
           ((text[pos - 1] == '\r' && text[pos] == '\n') ||
            (is_high_surrogate(text[pos - 1]) && is_low_surrogate(text[pos])));
}

/*
 * new_text is old with *removed units at *start replaced by *inserted
 * units. Widens the change so that neither end cuts a "\r\n" or a
 * surrogate pair in either text; then the unchanged parts convert to the
 * same code points in both, and the change can be converted on its own.
 */
void align_change(const utf16 *old, int old_len, const utf16 *new_text, int new_len,
                  int *start, int *removed, int *inserted)
{
    while (*start > 0 &&
           (splits_pair(old, old_len, *start) || splits_pair(new_text, new_len, *start)))
    {
        (*start)--;
        (*removed)++;
        (*inserted)++;
    }
    while (splits_pair(old, old_len, *start + *removed) ||
           splits_pair(new_text, new_len, *start + *inserted))
    {
        (*removed)++;
        (*inserted)++;
    }
}

/*
 * The change from old to new_text as for align_change: the part between
 * their common beginning and common end.
 */
void find_change(const utf16 *old, int old_len, const utf16 *new_text, int new_len,
                 int *start, int *removed, int *inserted)
{
    int prefix = 0, suffix = 0;

    while (prefix < old_len && prefix < new_len && old[prefix] == new_text[prefix])
        prefix++;
    while (suffix < old_len - prefix && suffix < new_len - prefix &&
           old[old_len - 1 - suffix] == new_text[new_len - 1 - suffix])
        suffix++;
    *start = prefix;
    *removed = old_len - prefix - suffix;
    *inserted = new_len - prefix - suffix;
    align_change(old, old_len, new_text, new_len, start, removed, inserted);
}

/*
 * A RichEdit (2.0 and later) ends a paragraph with one "\r" inside, so its
 * EM_GETSEL counts the "\r\n" that WM_GETTEXT gives as one character
 * (riched20/editor.c). Turn such a position into an index into text.
 */
int rich_text_index(const utf16 *text, int len, int pos)
{
    int i = 0;

    for (; pos > 0 && i < len; pos--)
        i += text[i] == '\r' && i + 1 < len && text[i + 1] == '\n' ? 2 : 1;
    return i;
}

/*
 * Index into the text of an Edit (rich 0) or RichEdit of AT-SPI offset in
 * chars: astral code points take two UTF-16 units, and "\n" was "\r\n" in
 * an Edit but one "\r" in a RichEdit (see rich_text_index).
 */
int control_index(const unsigned int *chars, int count, int offset, int rich)
{
    int i, index = 0;

    for (i = 0; i < offset && i < count; i++)
        index += (chars[i] > 0xffff ? 2 : 1) + (!rich && chars[i] == '\n');
    return index;
}

/*
 * AT-SPI offset in chars of the control index, the inverse of
 * control_index. An index inside a "\r\n" or a surrogate pair gives the
 * offset of that character; an index past the end gives count.
 */
int control_offset(const unsigned int *chars, int count, int index, int rich)
{
    int i, pos = 0;

    for (i = 0; i < count; i++)
    {
        pos += (chars[i] > 0xffff ? 2 : 1) + (!rich && chars[i] == '\n');
        if (pos > index)
            return i;
    }
    return count;
}

/* Drops the "&" that marks a mnemonic; "&&" stays as one "&". */
void strip_ampersands(utf16 *buf)
{
    int i, j;

    for (i = j = 0; buf[i]; i++)
    {
        if (buf[i] == '&')
        {
            if (buf[i + 1] != '&')
                continue;
            i++;
        }
        buf[j++] = buf[i];
    }
    buf[j] = 0;
}

/*
 * Splits menu item text "&Open...\tCtrl+O" in place: buf keeps the name
 * without mnemonic, "Open...". Returns the shortcut "Ctrl+O", or NULL if
 * there is no tab.
 */
utf16 *split_menu_text(utf16 *buf)
{
    utf16 *tab;

    for (tab = buf; *tab && *tab != '\t'; tab++)
        ;
    if (*tab)
        *tab++ = 0;
    else
        tab = NULL;
    strip_ampersands(buf);
    return tab;
}

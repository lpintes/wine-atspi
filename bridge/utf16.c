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

/*
 * wine-atspi bridge, Win32 core: pure helpers on UTF-16 text (utf16.c).
 *
 * They need no Windows API, so tests/unit.c builds them with the host
 * compiler and checks them without Wine (make check). In the core utf16
 * is WCHAR; elsewhere it is a 16-bit unsigned type.
 */
#ifndef WINE_ATSPI_UTF16_H
#define WINE_ATSPI_UTF16_H

#ifdef _WIN32
#include <windows.h>
typedef WCHAR utf16;
#else
typedef unsigned short utf16;
#endif

int char_offset(const utf16 *text, int len, int pos);
int utf16_to_chars(const utf16 *text, int len, unsigned int *out);
void align_change(const utf16 *old, int old_len, const utf16 *new_text, int new_len,
                  int *start, int *removed, int *inserted);
void find_change(const utf16 *old, int old_len, const utf16 *new_text, int new_len,
                 int *start, int *removed, int *inserted);
int rich_text_index(const utf16 *text, int len, int pos);
int control_index(const unsigned int *chars, int count, int offset, int rich);
int control_offset(const unsigned int *chars, int count, int index, int rich);
void strip_ampersands(utf16 *buf);
utf16 *split_menu_text(utf16 *buf);

#endif

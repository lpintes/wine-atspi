/*
 * Unit tests of the bridge's pure helpers (bridge/utf16.c,
 * bridge/textrange.c), built natively with the host compiler, no Wine:
 * make check. Prints each failure and exits with 1 if any.
 */
#include <stdio.h>
#include <string.h>
#include <uchar.h>

#include "utf16.h"
#include "textrange.h"

static int failures, checks;

#define CHECK_INT(expr, expected) check_int(__LINE__, #expr, (expr), (expected))

static void check_int(int line, const char *expr, long got, long expected)
{
    checks++;
    if (got == expected)
        return;
    failures++;
    printf("unit.c:%d: %s is %ld, expected %ld\n", line, expr, got, expected);
}

#define CHECK_U16(buf, expected) check_u16(__LINE__, #buf, (buf), (expected))

/* expected is a u"" literal: char16_t is the 16-bit type of utf16 here. */
static void check_u16(int line, const char *expr, const utf16 *got, const char16_t *expected)
{
    int i;

    checks++;
    if (!got && !expected)
        return;
    if (got && expected)
    {
        for (i = 0; got[i] && got[i] == expected[i]; i++)
            ;
        if (got[i] == expected[i])
            return;
    }
    failures++;
    printf("unit.c:%d: %s differs from the expected text\n", line, expr);
}

static int u16len(const char16_t *s)
{
    int n = 0;

    while (s[n])
        n++;
    return n;
}

static int u32len(const char32_t *s)
{
    int n = 0;

    while (s[n])
        n++;
    return n;
}

#define T(s) ((const utf16 *)(s))

static void test_char_offset(void)
{
    static const utf16 lone_low[] = { 0xdc00, 'a', 0 };
    const char16_t *crlf = u"a\r\nb";
    const char16_t *astral = u"a\U0001F600b";

    CHECK_INT(char_offset(T(u"abc"), 3, 2), 2);
    CHECK_INT(char_offset(T(u"abc"), 3, 0), 0);
    /* The "\r" of "\r\n" counts as nothing. */
    CHECK_INT(char_offset(T(crlf), 4, 1), 1);
    CHECK_INT(char_offset(T(crlf), 4, 2), 1);
    CHECK_INT(char_offset(T(crlf), 4, 3), 2);
    CHECK_INT(char_offset(T(crlf), 4, 4), 3);
    /* A lone "\r" counts. */
    CHECK_INT(char_offset(T(u"a\rb"), 3, 3), 3);
    CHECK_INT(char_offset(T(u"a\r"), 2, 2), 2);
    /* A surrogate pair counts once, in the middle too. */
    CHECK_INT(char_offset(T(astral), 4, 2), 2);
    CHECK_INT(char_offset(T(astral), 4, 3), 2);
    CHECK_INT(char_offset(T(astral), 4, 4), 3);
    /* A lone low surrogate counts. */
    CHECK_INT(char_offset(lone_low, 2, 2), 2);
    /* pos beyond the text is clamped. */
    CHECK_INT(char_offset(T(u"ab"), 2, 5), 2);
    CHECK_INT(char_offset(T(u""), 0, 3), 0);
}

static void test_rich_text_index(void)
{
    const char16_t *crlf = u"a\r\nb";

    CHECK_INT(rich_text_index(T(crlf), 4, 0), 0);
    CHECK_INT(rich_text_index(T(crlf), 4, 1), 1);
    CHECK_INT(rich_text_index(T(crlf), 4, 2), 3);
    CHECK_INT(rich_text_index(T(crlf), 4, 3), 4);
    CHECK_INT(rich_text_index(T(crlf), 4, 9), 4);
    CHECK_INT(rich_text_index(T(u"\r\n\r\n"), 4, 1), 2);
    CHECK_INT(rich_text_index(T(u"\r\n\r\n"), 4, 2), 4);
    /* A "\r" at the end of the text is one character. */
    CHECK_INT(rich_text_index(T(u"a\r"), 2, 2), 2);
}

static void test_control_index(void)
{
    /* What the AT-SPI side holds, and the text WM_GETTEXT gives. */
    const char32_t *chars = U"x\ny\U0001F600z\n";
    const char16_t *text = u"x\r\ny\U0001F600z\r\n";
    int count = u32len(chars), len = u16len(text), k;

    CHECK_INT(control_index(U"a\nb", 3, 2, 0), 3);
    CHECK_INT(control_index(U"a\nb", 3, 2, 1), 2);
    CHECK_INT(control_index(U"a\U0001F600b", 3, 2, 0), 3);
    CHECK_INT(control_index(U"a\U0001F600b", 3, 3, 0), 4);
    CHECK_INT(control_index(U"ab", 2, 9, 0), 2);

    CHECK_INT(control_offset(U"a\nb", 3, 1, 0), 1);
    CHECK_INT(control_offset(U"a\nb", 3, 2, 0), 1);
    CHECK_INT(control_offset(U"a\nb", 3, 3, 0), 2);
    CHECK_INT(control_offset(U"a\nb", 3, 2, 1), 2);
    CHECK_INT(control_offset(U"a\U0001F600b", 3, 2, 0), 1);
    CHECK_INT(control_offset(U"a\U0001F600b", 3, 3, 0), 2);
    CHECK_INT(control_offset(U"ab", 2, 9, 0), 2);

    /* Round trips: AT-SPI offset -> control index -> AT-SPI offset. */
    for (k = 0; k <= count; k++)
    {
        CHECK_INT(control_offset(chars, count, control_index(chars, count, k, 0), 0), k);
        CHECK_INT(control_offset(chars, count, control_index(chars, count, k, 1), 1), k);
        CHECK_INT(char_offset(T(text), len, control_index(chars, count, k, 0)), k);
        CHECK_INT(char_offset(T(text), len,
                              rich_text_index(T(text), len, control_index(chars, count, k, 1))),
                  k);
    }
}

static void test_utf16_to_chars(void)
{
    static const utf16 lone[] = { 'a', 0xd800, 'b', 0xdc00, 0 };
    unsigned int out[8];

    CHECK_INT(utf16_to_chars(T(u"a\r\nb\rc"), 6, out), 5);
    CHECK_INT(out[1], '\n');
    CHECK_INT(out[3], '\r');
    CHECK_INT(utf16_to_chars(T(u"a\U0001F600b"), 4, out), 3);
    CHECK_INT(out[1], 0x1F600);
    CHECK_INT(utf16_to_chars(lone, 4, out), 4);
    CHECK_INT(out[1], 0xfffd);
    CHECK_INT(out[3], 0xfffd);
    CHECK_INT(utf16_to_chars(T(u"a\r\n"), 3, NULL), 2);
}

#define CHECK_CHANGE(old, new_text, e_start, e_removed, e_inserted) \
    check_change(__LINE__, (old), (new_text), (e_start), (e_removed), (e_inserted))

/*
 * find_change gives the expected range, and converting the unchanged
 * parts and the change on their own gives the code points of the new text.
 */
static void check_change(int line, const char16_t *old, const char16_t *new_text, int e_start,
                         int e_removed, int e_inserted)
{
    int old_len = u16len(old), new_len = u16len(new_text), start, removed, inserted;
    unsigned int whole[32], pieces[32];
    int count, n;

    find_change(T(old), old_len, T(new_text), new_len, &start, &removed, &inserted);
    check_int(line, "start", start, e_start);
    check_int(line, "removed", removed, e_removed);
    check_int(line, "inserted", inserted, e_inserted);
    count = utf16_to_chars(T(new_text), new_len, whole);
    n = utf16_to_chars(T(new_text), start, pieces);
    n += utf16_to_chars(T(new_text) + start, inserted, pieces + n);
    n += utf16_to_chars(T(old) + start + removed, old_len - start - removed, pieces + n);
    check_int(line, "pieces", n, count);
    check_int(line, "same code points", !memcmp(whole, pieces, count * sizeof(*whole)), 1);
    /* The prefix gives the same code points in both texts. */
    check_int(line, "prefix", utf16_to_chars(T(old), start, NULL),
              utf16_to_chars(T(new_text), start, NULL));
}

static void test_find_change(void)
{
    CHECK_CHANGE(u"abc", u"abXc", 2, 0, 1);
    CHECK_CHANGE(u"abc", u"ac", 1, 1, 0);
    CHECK_CHANGE(u"abc", u"abc", 3, 0, 0);
    CHECK_CHANGE(u"", u"ab", 0, 0, 2);
    CHECK_CHANGE(u"ab", u"", 0, 2, 0);
    /* Return typed at the end of a line, and removed again. */
    CHECK_CHANGE(u"ab\r\ncd", u"ab\r\n\r\ncd", 4, 0, 2);
    CHECK_CHANGE(u"ab\r\n\r\ncd", u"ab\r\ncd", 4, 2, 0);
    /* "\r" turned into "\r\n": the "\r" is part of the change. */
    CHECK_CHANGE(u"a\rb", u"a\r\nb", 1, 1, 2);
    CHECK_CHANGE(u"a\r\nb", u"a\rb", 1, 2, 1);
    /* "\n" after a "\r" that stays. */
    CHECK_CHANGE(u"a\r", u"a\r\n", 1, 1, 2);
    /* A surrogate pair is never cut. */
    CHECK_CHANGE(u"a\U0001F600b", u"a\U0001F601b", 1, 2, 2);
    CHECK_CHANGE(u"a\U0001F600", u"a\U0001F600x", 3, 0, 1);
    CHECK_CHANGE(u"\U0001F600", u"x\U0001F600", 0, 0, 1);
}

static void test_strip_ampersands(void)
{
    char16_t a[] = u"&Open", b[] = u"Save && Exit", c[] = u"a&", d[] = u"&&&x", e[] = u"";

    strip_ampersands((utf16 *)a);
    strip_ampersands((utf16 *)b);
    strip_ampersands((utf16 *)c);
    strip_ampersands((utf16 *)d);
    strip_ampersands((utf16 *)e);
    CHECK_U16(T(a), u"Open");
    CHECK_U16(T(b), u"Save & Exit");
    CHECK_U16(T(c), u"a");
    CHECK_U16(T(d), u"&x");
    CHECK_U16(T(e), u"");
}

static void test_split_menu_text(void)
{
    char16_t a[] = u"&Open...\tCtrl+O", b[] = u"E&xit", c[] = u"A\t", d[] = u"&&Help\tF1\tx";

    CHECK_U16(split_menu_text((utf16 *)a), u"Ctrl+O");
    CHECK_U16(T(a), u"Open...");
    CHECK_U16(split_menu_text((utf16 *)b), NULL);
    CHECK_U16(T(b), u"Exit");
    CHECK_U16(split_menu_text((utf16 *)c), u"");
    CHECK_U16(T(c), u"A");
    /* Only the first tab splits; the shortcut keeps its "&". */
    CHECK_U16(split_menu_text((utf16 *)d), u"F1\tx");
    CHECK_U16(T(d), u"&Help");
}

#define CHECK_RANGE(chars, offset, granularity, exp_start, exp_end) \
    check_range(__LINE__, (chars), (offset), (granularity), (exp_start), (exp_end))

static void check_range(int line, const char32_t *chars, int offset, unsigned int granularity,
                        int exp_start, int exp_end)
{
    gint start = -9, end = -9;

    checks++;
    text_range(chars, u32len(chars), offset, granularity, &start, &end);
    if (start == exp_start && end == exp_end)
        return;
    failures++;
    printf("unit.c:%d: text_range at %d, granularity %u is [%d, %d), expected [%d, %d)\n",
           line, offset, granularity, start, end, exp_start, exp_end);
}

static void test_text_range(void)
{
    /* o0 n1 e2 _3 t4 w5 o6 \n7 t8 h9 r10 e11 e12 \n13, count 14 */
    const char32_t *text = U"one two\nthree\n";

    CHECK_RANGE(text, 2, GRANULARITY_CHAR, 2, 3);
    CHECK_RANGE(text, 14, GRANULARITY_CHAR, 14, 14);
    /* A word runs to the start of the next word. */
    CHECK_RANGE(text, 0, GRANULARITY_WORD, 0, 4);
    CHECK_RANGE(text, 1, GRANULARITY_WORD, 0, 4);
    CHECK_RANGE(text, 3, GRANULARITY_WORD, 0, 4);
    CHECK_RANGE(text, 4, GRANULARITY_WORD, 4, 8);
    CHECK_RANGE(text, 7, GRANULARITY_WORD, 4, 8);
    CHECK_RANGE(text, 14, GRANULARITY_WORD, 8, 14);
    /* A line includes its "\n"; after a final "\n" the line is empty. */
    CHECK_RANGE(text, 2, GRANULARITY_LINE, 0, 8);
    CHECK_RANGE(text, 7, GRANULARITY_LINE, 0, 8);
    CHECK_RANGE(text, 8, GRANULARITY_LINE, 8, 14);
    CHECK_RANGE(text, 13, GRANULARITY_LINE, 8, 14);
    CHECK_RANGE(text, 14, GRANULARITY_LINE, 14, 14);
    CHECK_RANGE(U"ab\ncd", 5, GRANULARITY_LINE, 3, 5);
    /* Sentences and paragraphs are lines. */
    CHECK_RANGE(text, 9, GRANULARITY_SENTENCE, 8, 14);
    CHECK_RANGE(text, 9, GRANULARITY_PARAGRAPH, 8, 14);
    /* Offsets out of the text are clamped. */
    CHECK_RANGE(text, -5, GRANULARITY_LINE, 0, 8);
    CHECK_RANGE(text, 99, GRANULARITY_CHAR, 14, 14);
    CHECK_RANGE(U"", 0, GRANULARITY_WORD, 0, 0);
    CHECK_RANGE(U"", 0, GRANULARITY_LINE, 0, 0);
    /* Letters beyond ASCII and combining marks are word characters. */
    CHECK_RANGE(U"čaj kôň", 5, GRANULARITY_WORD, 4, 7);
    CHECK_RANGE(U"éx y", 0, GRANULARITY_WORD, 0, 4);
    CHECK_RANGE(U"a, b", 0, GRANULARITY_WORD, 0, 3);
}

int main(void)
{
    test_char_offset();
    test_rich_text_index();
    test_control_index();
    test_utf16_to_chars();
    test_find_change();
    test_strip_ampersands();
    test_split_menu_text();
    test_text_range();
    printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}

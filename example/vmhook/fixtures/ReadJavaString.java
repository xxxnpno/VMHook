package vmhook.fixtures;

import vmhook.Harness;

/**
 * Fixture making vmhook's free helper {@code read_java_string(oop)} the SUBJECT
 * under test (migrates and then GREATLY expands the legacy example.cpp
 * test_read_java_string).
 *
 * The native module (tests/jvm/modules/read_java_string.cpp) obtains each static
 * String field's backing OOP exactly the way the library does internally --
 * {@code static_field(name)->get_compressed_oop()} then
 * {@code vmhook::hotspot::decode_oop_pointer(...)} -- and feeds it straight into
 * {@code vmhook::read_java_string}, asserting the returned std::string is
 * BYTE-EXACT UTF-8 for every case.
 *
 * The static String fields below aim to be EXHAUSTIVE over read_java_string's
 * input space and span BOTH backing-storage coders on a modern (JDK 9+)
 * compact-string VM:
 *
 *   LATIN1 (coder 0, every char {@code <= 0xFF} -- one byte per char):
 *     - ascii      "hello"   -> pure ASCII; UTF-8 == backing bytes verbatim.
 *     - oneAscii   "Z"       -> single ASCII char (char_count == 1 boundary).
 *     - cafe       "cafe"    -> U+00E9; LATIN1 byte 0xE9 but UTF-8 OUTPUT is the
 *                              TWO bytes C3 A9 (proves high-byte UTF-8 encode).
 *     - latin1Hi   U+00FF    -> the LATIN1 ceiling char -> UTF-8 C3 BF.
 *     - nulLatin1  a\0b      -> interior NUL in a LATIN1 string (preserved; the
 *                              std::string is sized by length, NOT C-string cut).
 *     - longAscii  1000x 'x' -> a long (>300) LATIN1 string well under the cap.
 *
 *   UTF16 (coder 1, at least one char {@code > 0xFF} -- two bytes per char):
 *     - oneCjk     U+4E2D            -> single BMP >0xFF char (3-byte UTF-8).
 *     - nihongo    U+65E5U+672CU+8A9E -> three BMP CJK code points.
 *     - mixed      "A"U+65E5"B"      -> ASCII + a >0xFF char promotes the WHOLE
 *                                       string to UTF-16, so even the ASCII chars
 *                                       go through the UTF-16 decode path.
 *     - nulUtf16   a\0U+65E5         -> interior NUL char in a UTF-16 string.
 *     - emoji      U+1F600           -> a SURROGATE PAIR -> one astral code point
 *                                       -> 4-byte UTF-8 (F0 9F 98 80), exercising
 *                                       the surrogate-combine branch.
 *     - emojiMix   "X"U+1F600"Y"     -> ASCII, surrogate pair, ASCII: proves the
 *                                       surrogate index-advance does not swallow
 *                                       the trailing 'Y' (6 bytes, 3 codepoints).
 *     - loneHigh   U+D83D            -> a LONE high surrogate (no following low
 *                                       surrogate): read_java_string does NOT
 *                                       combine and emits the 3-byte WTF-8/CESU
 *                                       encoding of the surrogate code unit.
 *                                       Asserted as the ACTUAL (never-crash)
 *                                       behaviour, not as well-formed UTF-8.
 *     - longCjk    300x U+65E5       -> a long (>300) UTF-16 string under the cap.
 *
 *   Length-cap boundary cases (read_java_string range-guards the backing-ARRAY
 *   length to 1..4096; the doc says "truncate", the code RETURNS EMPTY past the
 *   ceiling -- a documented behaviour these assert directly):
 *     - cap4096    4096x 'x'    -> LATIN1 byte[] length 4096 -> passes (inclusive).
 *     - cap4097    4097x 'x'    -> LATIN1 byte[] length 4097 -> REJECTED -> "".
 *     - capUtf2048 2048x U+65E5 -> UTF16 byte[] length 4096 -> passes (decodes).
 *     - capUtf2049 2049x U+65E5 -> UTF16 byte[] length 4098 > 4096 -> REJECTED on
 *                                  JDK 9+ (the asymmetric UTF-16 char ceiling of
 *                                  2048); on JDK 8 the char[] length is 2049
 *                                  (<= 4096) so it DECODES -- the native module
 *                                  branches on the compact-string `coder` field.
 *
 *   Guard paths:
 *     - empty    ""    -> backing array length 0 -> the length<=0 guard -> "".
 *     - nullRef  null  -> field compressed-OOP is 0 -> the null-oop guard -> "".
 *
 * On Java 8 String.value is a char[] (always UTF-16, no {@code coder} field); on
 * Java 9+ it is a byte[] + a {@code coder} field (LATIN1/UTF16).  read_java_string
 * must yield the IDENTICAL UTF-8 bytes on both -- the native module asserts that
 * invariant by comparing against fixed expected byte sequences regardless of JDK.
 *
 * Every field is built with {@code new String(char[])} so it owns a PRIVATE,
 * non-interned backing array; read_java_string is a PURE READER (it never
 * mutates), but private backing keeps this fixture self-contained and mirrors
 * the sibling string fixtures.  ALL non-ASCII chars are {@code \\uXXXX} escapes
 * (lexer-level) and the source file is pure ASCII, so javac agrees on Cp1252
 * (Windows default) and UTF-8 alike, regardless of the {@code -encoding} flag.
 *
 * Java 8 syntax only: anonymous Harness.Probe, no var / records / lambdas / the
 * String.repeat (JDK 11+) or Character.toString(int) (JDK 11+) helpers.
 */
public final class ReadJavaString
{
    // ----- go/done handshake the native run_probe drives -------------------
    public static volatile boolean go;
    public static volatile boolean done;

    // ================= SUBJECT fields (static String) ======================
    // LATIN1 (coder 0) targets ------------------------------------------------
    public static String ascii    = fresh(new char[] { 'h', 'e', 'l', 'l', 'o' });
    // Single ASCII char -- the char_count == 1 boundary of the decode loop.
    public static String oneAscii = fresh(new char[] { 'Z' });
    // "cafe" with an acute e (U+00E9).  All chars <= 0xFF -> LATIN1 coder 0,
    // backing byte 0xE9, but UTF-8 output is the two bytes C3 A9.
    public static String cafe     = fresh(new char[] { 'c', 'a', 'f', '\u00E9' });
    // The LATIN1 ceiling: U+00FF -> still coder 0, UTF-8 C3 BF.
    public static String latin1Hi = fresh(new char[] { '\u00FF' });
    // Interior NUL inside a LATIN1 string: 'a', NUL, 'b'.  The decoded
    // std::string must keep all 3 bytes (sized by length, not C-string cut).
    public static String nulLatin1 = fresh(new char[] { 'a', '\0', 'b' });
    // A long (>300) LATIN1 string, comfortably under the 1..4096 cap.
    public static String longAscii = repeatChar('x', 1000);

    // UTF16 (coder 1) targets -------------------------------------------------
    // A single BMP char > 0xFF -> coder 1, one 3-byte UTF-8 code point.
    public static String oneCjk   = fresh(new char[] { '\u4E2D' });
    // U+65E5 U+672C U+8A9E -> three BMP code points all > 0xFF.
    public static String nihongo  = fresh(new char[] { '\u65E5', '\u672C', '\u8A9E' });
    // ASCII + a >0xFF char -> whole string promoted to UTF-16 coder 1.
    public static String mixed    = fresh(new char[] { 'A', '\u65E5', 'B' });
    // Interior NUL inside a UTF-16 string ('a', NUL, U+65E5): the CJK char forces
    // coder 1, and the NUL char must survive the UTF-16 decode.
    public static String nulUtf16 = fresh(new char[] { 'a', '\0', '\u65E5' });
    // U+1F600 as a UTF-16 surrogate pair -> one astral code point.
    public static String emoji    = fresh(new char[] { '\uD83D', '\uDE00' });
    // ASCII, surrogate pair, ASCII: 'X', U+1F600, 'Y'.  Proves the surrogate
    // combine advances past BOTH units and does not consume the trailing 'Y'.
    public static String emojiMix = fresh(new char[] { 'X', '\uD83D', '\uDE00', 'Y' });
    // A LONE high surrogate with no following low surrogate.  read_java_string
    // cannot combine it, so it emits the surrogate code unit as a 3-byte
    // WTF-8/CESU sequence (ED A0 BD).  Constructible: a Java char[] can legally
    // hold an unpaired surrogate.  Asserted as the ACTUAL behaviour (never crash).
    public static String loneHigh = fresh(new char[] { '\uD83D' });
    // A long (>300) UTF-16 string, under the cap on both layouts.
    public static String longCjk  = repeatChar('\u65E5', 300);

    // Length-cap boundary targets --------------------------------------------
    // Exactly 4096 ASCII chars: LATIN1 byte[] length 4096 -> passes (inclusive).
    public static String cap4096    = repeatChar('x', 4096);
    // 4097 ASCII chars: LATIN1 byte[] length 4097 > 4096 -> REJECTED -> "".
    public static String cap4097    = repeatChar('x', 4097);
    // 2048 CJK chars: on JDK 9+ a UTF16 byte[] length 4096 -> passes (decodes to
    // 2048*3 UTF-8 bytes).  On JDK 8 a char[] length 2048 -> also passes.
    public static String capUtf2048 = repeatChar('\u65E5', 2048);
    // 2049 CJK chars: on JDK 9+ a UTF16 byte[] length 4098 > 4096 -> REJECTED ->
    // "" (the asymmetric UTF-16 char ceiling of 2048).  On JDK 8 a char[] length
    // 2049 <= 4096 -> DECODES (2049*3 UTF-8 bytes).  Native module branches on
    // the presence of the compact-string `coder` field.
    public static String capUtf2049 = repeatChar('\u65E5', 2049);

    // Guard targets -----------------------------------------------------------
    public static String empty    = fresh(new char[0]);   // length 0
    public static String nullRef  = null;                  // null reference

    // ================= Java-published cross-check facts ====================
    // The native side reads these back to PROVE its byte-exact decodes match
    // what Java itself computes for the same fields (length, code points, coder).
    public static volatile int     jAsciiLen;       // 5
    public static volatile int     jOneAsciiCp0;    // 'Z' == 0x5A
    public static volatile int     jCafeLen;        // 4
    public static volatile int     jCafeCp3;        // 0x00E9
    public static volatile int     jLatin1HiCp0;    // 0x00FF
    public static volatile int     jNulLatin1Len;   // 3
    public static volatile int     jNulLatin1Cp1;   // 0 (the interior NUL)
    public static volatile int     jLongAsciiLen;   // 1000
    public static volatile int     jOneCjkCp0;      // 0x4E2D
    public static volatile int     jNihongoLen;     // 3
    public static volatile int     jNihongoCp0;     // 0x65E5
    public static volatile int     jNihongoCp1;     // 0x672C
    public static volatile int     jNihongoCp2;     // 0x8A9E
    public static volatile int     jMixedLen;       // 3 (chars: A, U+65E5, B)
    public static volatile int     jNulUtf16Len;    // 3
    public static volatile int     jNulUtf16Cp1;    // 0 (the interior NUL)
    public static volatile int     jEmojiCpCount;   // 1 (one code point)
    public static volatile int     jEmojiCp0;       // 0x1F600
    public static volatile int     jEmojiMixLen;    // 4 (chars: X, hi, lo, Y)
    public static volatile int     jEmojiMixCpCount;// 3 (X, U+1F600, Y)
    public static volatile int     jLoneHighLen;    // 1
    public static volatile int     jLoneHighCp0;    // 0xD83D (lone surrogate)
    public static volatile int     jLongCjkLen;     // 300
    public static volatile int     jCap4096Len;     // 4096
    public static volatile int     jCap4097Len;     // 4097
    public static volatile int     jCapUtf2048Len;  // 2048
    public static volatile int     jCapUtf2049Len;  // 2049
    public static volatile int     jEmptyLen;       // 0
    public static volatile boolean jNullIsNull;     // true

    // String coder byte for each subject (JDK 9+); -1 on JDK 8 (no field).
    // Lets the native side label which physical coder each case exercised.
    public static volatile int     jCoderAscii;     // 0 (LATIN1) / -1 (JDK8)
    public static volatile int     jCoderCafe;      // 0 (LATIN1) / -1
    public static volatile int     jCoderNihongo;   // 1 (UTF16)  / -1
    public static volatile int     jCoderEmoji;     // 1 (UTF16)  / -1
    public static volatile int     jCoderLongCjk;   // 1 (UTF16)  / -1
    public static volatile int     jCoderLongAscii; // 0 (LATIN1) / -1
    public static volatile boolean jHasCoderField;  // true on JDK 9+, false on 8

    // ---------------------- helpers ----------------------------------------
    /** new String(char[]) -> a PRIVATE backing array (never an interned alias). */
    private static String fresh(final char[] chars)
    {
        return new String(chars);
    }

    /**
     * Build a fresh String of {@code n} repeats of {@code c} without the JDK-11
     * String.repeat helper (this fixture must compile on JDK 8).  The result owns
     * a private char[] via new String(char[]).
     */
    private static String repeatChar(final char c, final int n)
    {
        final char[] buf = new char[n];
        for (int i = 0; i < n; i++)
        {
            buf[i] = c;
        }
        return new String(buf);
    }

    /** Reflective coder() probe; returns -1 when the field is absent (JDK 8). */
    private static int coderOf(final String s)
    {
        try
        {
            final java.lang.reflect.Field cf = String.class.getDeclaredField("coder");
            cf.setAccessible(true);
            return cf.getByte(s);
        }
        catch (final Throwable t)
        {
            return -1;
        }
    }

    static
    {
        Harness.register(new Harness.Probe()
        {
            @Override
            public boolean pending()
            {
                return ReadJavaString.go && !ReadJavaString.done;
            }

            @Override
            public void run()
            {
                jAsciiLen      = ascii.length();
                jOneAsciiCp0   = oneAscii.codePointAt(0);
                jCafeLen       = cafe.length();
                jCafeCp3       = cafe.codePointAt(3);
                jLatin1HiCp0   = latin1Hi.codePointAt(0);
                jNulLatin1Len  = nulLatin1.length();
                jNulLatin1Cp1  = nulLatin1.codePointAt(1);
                jLongAsciiLen  = longAscii.length();
                jOneCjkCp0     = oneCjk.codePointAt(0);
                jNihongoLen    = nihongo.length();
                jNihongoCp0    = nihongo.codePointAt(0);
                jNihongoCp1    = nihongo.codePointAt(1);
                jNihongoCp2    = nihongo.codePointAt(2);
                jMixedLen      = mixed.length();
                jNulUtf16Len   = nulUtf16.length();
                jNulUtf16Cp1   = nulUtf16.codePointAt(1);
                jEmojiCpCount  = emoji.codePointCount(0, emoji.length());
                jEmojiCp0      = emoji.codePointAt(0);
                jEmojiMixLen   = emojiMix.length();
                jEmojiMixCpCount = emojiMix.codePointCount(0, emojiMix.length());
                jLoneHighLen   = loneHigh.length();
                jLoneHighCp0   = loneHigh.codePointAt(0);
                jLongCjkLen    = longCjk.length();
                jCap4096Len    = cap4096.length();
                jCap4097Len    = cap4097.length();
                jCapUtf2048Len = capUtf2048.length();
                jCapUtf2049Len = capUtf2049.length();
                jEmptyLen      = empty.length();
                jNullIsNull    = (nullRef == null);

                jHasCoderField = false;
                try
                {
                    String.class.getDeclaredField("coder");
                    jHasCoderField = true;
                }
                catch (final Throwable t)
                {
                    jHasCoderField = false;
                }
                jCoderAscii     = coderOf(ascii);
                jCoderCafe      = coderOf(cafe);
                jCoderNihongo   = coderOf(nihongo);
                jCoderEmoji     = coderOf(emoji);
                jCoderLongCjk   = coderOf(longCjk);
                jCoderLongAscii = coderOf(longAscii);

                ReadJavaString.done = true;
            }
        });
    }
}

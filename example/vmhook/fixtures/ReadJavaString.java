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
 *     - latin1Lo80 U+0080    -> smallest non-ASCII char (1-byte/2-byte boundary
 *                              of the encoder) -> UTF-8 C2 80.
 *     - latin1Hi   U+00FF    -> the LATIN1 ceiling char -> UTF-8 C3 BF.
 *     - controls   ctl chars -> U+0001/0008/001F/007F, one byte each, verbatim.
 *     - whitespace SP TAB ...-> every ASCII whitespace, preserved (no trimming).
 *     - nulLatin1  a\0b      -> interior NUL in a LATIN1 string (preserved; the
 *                              std::string is sized by length, NOT C-string cut).
 *     - all256     0x00..FF  -> EVERY byte value in one LATIN1 string: 128 ASCII
 *                              chars (1 byte) + 128 high chars (2 bytes) = 384.
 *     - longAscii  1000x 'x' -> a long (>300) LATIN1 string well under the cap.
 *     - interned   "hello"   -> an interned literal; MUST decode identically to
 *                              the new-String `ascii` (pooled vs private backing).
 *
 *   UTF16 (coder 1, at least one char {@code > 0xFF} -- two bytes per char):
 *     - oneCjk     U+4E2D            -> single BMP >0xFF char (3-byte UTF-8).
 *     - nihongo    U+65E5U+672CU+8A9E -> three BMP CJK code points.
 *     - mixed      "A"U+65E5"B"      -> ASCII + a >0xFF char promotes the WHOLE
 *                                       string to UTF-16, so even the ASCII chars
 *                                       go through the UTF-16 decode path.
 *     - bmp2to3a   U+07FF            -> last 2-byte UTF-8 code point (DF BF).
 *     - bmp2to3b   U+0800            -> first 3-byte UTF-8 code point (E0 A0 80).
 *     - belowSurr  U+D7FF            -> last code point BELOW the surrogate gap.
 *     - aboveSurr  U+E000            -> first code point ABOVE the surrogate gap.
 *     - replacement U+FFFD           -> the Unicode replacement char (EF BF BD).
 *     - maxBmp     U+FFFF            -> the MAX BMP code point (EF BF BF).
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
 *     - loneLow    U+DC00            -> a LONE low surrogate: takes the plain
 *                                       append_utf8 path (NOT the combine branch)
 *                                       -> 3-byte CESU (ED B0 80).
 *     - reversedPair U+DC00 U+D83D   -> low THEN high: neither combines, two
 *                                       3-byte CESU sequences (ED B0 80 ED A0 BD).
 *     - highAtEnd  "X" U+D83D        -> a high surrogate as the LAST unit (the
 *                                       (i+1)<count guard is false at a NON-zero
 *                                       index): 'X' then 3-byte CESU (58 ED A0 BD).
 *     - longCjk    300x U+65E5       -> a long (>300) UTF-16 string under the cap.
 *
 *   Long-String read-in-full cases (robustness bug #29 FIXED: the old hard
 *   4096-char cap that decoded any longer String to "" is GONE; read_java_string
 *   now reads IN FULL up to read_java_string_max_units = 16M CHARACTERS, the
 *   ceiling applied UNIFORMLY to the decoded CHARACTER count, so no layout has a
 *   smaller effective limit):
 *     - cap4096    4096x 'x'    -> LATIN1 byte[] length 4096 -> read in full.
 *     - cap4097    4097x 'x'    -> ONE char past the OLD cap -> read in full (4097).
 *     - cap70000   70000x 'x'   -> a length WAY over the OLD cap (and over 65536):
 *                                  proves the int32 length is read intact and the
 *                                  full body is decoded (no overflow) -> 70000 bytes.
 *     - capUtf2048 2048x U+65E5 -> UTF16 byte[] length 4096 -> read in full.
 *     - capUtf2049 2049x U+65E5 -> UTF16 byte[] length 4098 (2049 chars) -> read in
 *                                  full on JDK 9+ too (no more asymmetric UTF-16
 *                                  ceiling); on JDK 8 the char[] length is 2049 and
 *                                  it likewise reads in full -- 2049*3 UTF-8 bytes
 *                                  on EVERY layout.
 *
 *   Guard paths:
 *     - empty    ""    -> backing array length 0 -> the length<=0 guard -> "".
 *     - nullRef  null  -> field compressed-OOP is 0 -> the null-oop guard -> "".
 *     - obj      Object-> a NON-String oop fed straight to read_java_string: it
 *                         reads the String.value/coder offsets out of a foreign
 *                         object, but EVERY read is os::safe_read-guarded, so the
 *                         worst case is a bounded best-effort result, NEVER a crash.
 *     - intArr   int[] -> a NON-String array oop, same graceful-degradation proof.
 *
 * On Java 8 String.value is a char[] (always UTF-16, no {@code coder} field); on
 * Java 9+ it is a byte[] + a {@code coder} field (LATIN1/UTF16).  read_java_string
 * must yield the IDENTICAL UTF-8 bytes on both -- the native module asserts that
 * invariant by comparing against fixed expected byte sequences regardless of JDK.
 *
 * Every String field is built with {@code new String(char[])} so it owns a
 * PRIVATE, non-interned backing array (the one exception is {@code interned},
 * which is deliberately an interned literal to prove pooled backing decodes the
 * same).  read_java_string is a PURE READER (it never mutates), but private
 * backing keeps this fixture self-contained and mirrors the sibling string
 * fixtures.  ALL non-ASCII / control chars are {@code \\uXXXX} escapes
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
    // U+0080 -- the SMALLEST non-ASCII code point: still LATIN1 (0x80 <= 0xFF) so
    // coder 0, but the first char whose UTF-8 needs TWO bytes (C2 80).  The exact
    // 1-byte/2-byte boundary of the append_utf8 encoder, hit on the LATIN1 path.
    public static String latin1Lo80 = fresh(new char[] { '\u0080' });
    // The LATIN1 ceiling: U+00FF -> still coder 0, UTF-8 C3 BF.
    public static String latin1Hi = fresh(new char[] { '\u00FF' });
    // ASCII C0 CONTROL chars (all < 0x80, one UTF-8 byte each, NOT stripped):
    // U+0001 (SOH), U+0008 (BS), U+001F (US), U+007F (DEL).  Proves control bytes
    // are passed through verbatim, never treated as terminators or whitespace.
    public static String controls = fresh(new char[] { '\u0001', '\u0008', '\u001F', '\u007F' });
    // All-WHITESPACE LATIN1: SP, TAB, LF, CR, FF, VT -> 20 09 0A 0D 0C 0B.  No
    // trimming: read_java_string is a verbatim decoder; leading / trailing /
    // interior whitespace is preserved byte-for-byte.  All written as \\u escapes
    // so the source stays pure ASCII (no literal control bytes in the .java file).
    public static String whitespace = fresh(new char[] { ' ', '\t', '\n', '\r', '\f', '\013' });
    // Interior NUL inside a LATIN1 string: 'a', NUL, 'b'.  The decoded
    // std::string must keep all 3 bytes (sized by length, not C-string cut).
    public static String nulLatin1 = fresh(new char[] { 'a', '\0', 'b' });
    // EVERY byte value 0x00..0xFF in one string: char[i] == i for i in 0..255.  All
    // chars <= 0xFF so coder 0 (a 256-byte byte[] on JDK 9+, a 256-char char[] on
    // JDK 8).  The UTF-8 output exercises the FULL LATIN1 high-byte re-encode: the
    // 128 chars 0x00..0x7F emit one byte each (incl. an interior NUL at index 0),
    // and the 128 chars 0x80..0xFF emit TWO bytes each (C2 80 .. C3 BF) -> 384
    // bytes total.  The native module rebuilds the same expected sequence by code.
    public static String all256 = allBytes();
    // A long (>300) LATIN1 string, comfortably under the 1..4096 cap.
    public static String longAscii = repeatChar('x', 1000);
    // INTERNED literal with the SAME content as `ascii` ("hello").  `ascii` owns a
    // private char[]/byte[] (new String); this shares the interned string-pool
    // backing.  Both MUST decode to the identical bytes 68 65 6C 6C 6F -- proving
    // read_java_string does not care whether the backing array is pooled or private.
    public static String interned = "hello";

    // UTF16 (coder 1) targets -------------------------------------------------
    // A single BMP char > 0xFF -> coder 1, one 3-byte UTF-8 code point.
    public static String oneCjk   = fresh(new char[] { '\u4E2D' });
    // U+65E5 U+672C U+8A9E -> three BMP code points all > 0xFF.
    public static String nihongo  = fresh(new char[] { '\u65E5', '\u672C', '\u8A9E' });
    // ASCII + a >0xFF char -> whole string promoted to UTF-16 coder 1.
    public static String mixed    = fresh(new char[] { 'A', '\u65E5', 'B' });
    // U+07FF -- the LAST 2-byte UTF-8 code point (DF BF).  > 0xFF so coder 1.
    public static String bmp2to3a = fresh(new char[] { '\u07FF' });
    // U+0800 -- the FIRST 3-byte UTF-8 code point (E0 A0 80).  The 2-byte/3-byte
    // boundary of append_utf8, hit on the UTF-16 path.
    public static String bmp2to3b = fresh(new char[] { '\u0800' });
    // U+D7FF -- the last code point BELOW the surrogate range: must decode as a
    // plain 3-byte char (ED 9F BF), NOT be mistaken for a surrogate.
    public static String belowSurr = fresh(new char[] { '\uD7FF' });
    // U+E000 -- the first code point ABOVE the surrogate range (EE 80 80).
    public static String aboveSurr = fresh(new char[] { '\uE000' });
    // U+FFFD -- the Unicode REPLACEMENT character (EF BF BD).
    public static String replacement = fresh(new char[] { '\uFFFD' });
    // U+FFFF -- the MAXIMUM BMP code point (EF BF BF).
    public static String maxBmp   = fresh(new char[] { '\uFFFF' });
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
    // A LONE LOW surrogate (U+DC00): NOT a high surrogate, so it never enters the
    // combine branch -- it takes the plain append_utf8 path and emits the 3-byte
    // CESU sequence ED B0 80.  The complementary branch to loneHigh.
    public static String loneLow  = fresh(new char[] { '\uDC00' });
    // REVERSED pair -- a low surrogate THEN a high surrogate (U+DC00 U+D83D).
    // Neither combines (the low isn't a high; the high is last so (i+1)<count is
    // false): two independent 3-byte CESU sequences ED B0 80 ED A0 BD (6 bytes).
    public static String reversedPair = fresh(new char[] { '\uDC00', '\uD83D' });
    // A high surrogate as the LAST unit, preceded by 'X' ('X' U+D83D).  Exercises
    // the (i+1)<count surrogate guard being FALSE at a NON-zero index, and proves
    // the preceding char is untouched: 58 ED A0 BD (4 bytes).
    public static String highAtEnd = fresh(new char[] { 'X', '\uD83D' });
    // A long (>300) UTF-16 string, under the cap on both layouts.
    public static String longCjk  = repeatChar('\u65E5', 300);

    // Long-String read-in-full targets (robustness bug #29 FIXED: no more 4096 cap;
    // read_java_string reads IN FULL up to 16M chars, uniformly per char count) ---
    // Exactly 4096 ASCII chars: LATIN1 byte[] length 4096 -> read in full.
    public static String cap4096    = repeatChar('x', 4096);
    // 4097 ASCII chars: ONE char past the OLD 4096 cap -> read in full (4097 bytes).
    public static String cap4097    = repeatChar('x', 4097);
    // 70000 ASCII chars: a length WAY past the OLD cap and past 65536.  Proves the
    // int32 length is read intact and the full 70000-byte body is decoded.
    public static String cap70000   = repeatChar('x', 70000);
    // 2048 CJK chars: on JDK 9+ a UTF16 byte[] length 4096; on JDK 8 a char[] length
    // 2048.  Decodes to 2048*3 UTF-8 bytes on both layouts.
    public static String capUtf2048 = repeatChar('\u65E5', 2048);
    // 2049 CJK chars: UTF16 byte[] length 4098 (2049 chars) on JDK 9+, char[] length
    // 2049 on JDK 8 -- BOTH read in full to 2049*3 UTF-8 bytes (the ceiling is now
    // on the decoded char count, so there is no more asymmetric UTF-16 limit).
    public static String capUtf2049 = repeatChar('\u65E5', 2049);

    // Guard targets -----------------------------------------------------------
    public static String empty    = fresh(new char[0]);   // length 0
    public static String nullRef  = null;                  // null reference
    // NON-String oops fed straight into read_java_string: it applies the
    // String.value / coder field offsets to a foreign object, but every read is
    // os::safe_read-guarded, so the result is a bounded best-effort value and the
    // call can NEVER crash.  The native module asserts only "no crash + bounded".
    public static Object  obj     = new Object();
    public static int[]   intArr  = new int[] { 0x41424344, 0x45464748, 0x494A4B4C };

    // ================= Java-published cross-check facts ====================
    // The native side reads these back to PROVE its byte-exact decodes match
    // what Java itself computes for the same fields (length, code points, coder).
    public static volatile int     jAsciiLen;       // 5
    public static volatile int     jOneAsciiCp0;    // 'Z' == 0x5A
    public static volatile int     jCafeLen;        // 4
    public static volatile int     jCafeCp3;        // 0x00E9
    public static volatile int     jLatin1Lo80Cp0;  // 0x0080
    public static volatile int     jLatin1HiCp0;    // 0x00FF
    public static volatile int     jControlsLen;    // 4
    public static volatile int     jControlsCp3;    // 0x007F
    public static volatile int     jWhitespaceLen;  // 6
    public static volatile int     jNulLatin1Len;   // 3
    public static volatile int     jNulLatin1Cp1;   // 0 (the interior NUL)
    public static volatile int     jAll256Len;      // 256
    public static volatile int     jAll256Cp0;      // 0   (the leading NUL)
    public static volatile int     jAll256Cp255;    // 0xFF
    public static volatile int     jLongAsciiLen;   // 1000
    public static volatile int     jInternedLen;    // 5
    public static volatile int     jOneCjkCp0;      // 0x4E2D
    public static volatile int     jNihongoLen;     // 3
    public static volatile int     jNihongoCp0;     // 0x65E5
    public static volatile int     jNihongoCp1;     // 0x672C
    public static volatile int     jNihongoCp2;     // 0x8A9E
    public static volatile int     jMixedLen;       // 3 (chars: A, U+65E5, B)
    public static volatile int     jBmp2to3aCp0;    // 0x07FF
    public static volatile int     jBmp2to3bCp0;    // 0x0800
    public static volatile int     jBelowSurrCp0;   // 0xD7FF
    public static volatile int     jAboveSurrCp0;   // 0xE000
    public static volatile int     jReplacementCp0; // 0xFFFD
    public static volatile int     jMaxBmpCp0;      // 0xFFFF
    public static volatile int     jNulUtf16Len;    // 3
    public static volatile int     jNulUtf16Cp1;    // 0 (the interior NUL)
    public static volatile int     jEmojiCpCount;   // 1 (one code point)
    public static volatile int     jEmojiCp0;       // 0x1F600
    public static volatile int     jEmojiMixLen;    // 4 (chars: X, hi, lo, Y)
    public static volatile int     jEmojiMixCpCount;// 3 (X, U+1F600, Y)
    public static volatile int     jLoneHighLen;    // 1
    public static volatile int     jLoneHighCp0;    // 0xD83D (lone surrogate)
    public static volatile int     jLoneLowLen;     // 1
    public static volatile int     jLoneLowCp0;     // 0xDC00 (lone low surrogate)
    public static volatile int     jReversedPairLen;// 2
    public static volatile int     jHighAtEndLen;   // 2 (chars: X, hi surrogate)
    public static volatile int     jLongCjkLen;     // 300
    public static volatile int     jCap4096Len;     // 4096
    public static volatile int     jCap4097Len;     // 4097
    public static volatile int     jCap70000Len;    // 70000
    public static volatile int     jCapUtf2048Len;  // 2048
    public static volatile int     jCapUtf2049Len;  // 2049
    public static volatile int     jEmptyLen;       // 0
    public static volatile boolean jNullIsNull;     // true

    // String coder byte for each subject (JDK 9+); -1 on JDK 8 (no field).
    // Lets the native side label which physical coder each case exercised.
    public static volatile int     jCoderAscii;     // 0 (LATIN1) / -1 (JDK8)
    public static volatile int     jCoderCafe;      // 0 (LATIN1) / -1
    public static volatile int     jCoderAll256;    // 0 (LATIN1) / -1
    public static volatile int     jCoderNihongo;   // 1 (UTF16)  / -1
    public static volatile int     jCoderEmoji;     // 1 (UTF16)  / -1
    public static volatile int     jCoderLongCjk;   // 1 (UTF16)  / -1
    public static volatile int     jCoderLongAscii; // 0 (LATIN1) / -1
    public static volatile int     jCoderMaxBmp;    // 1 (UTF16)  / -1
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

    /**
     * Build a fresh String whose char[i] == i for i in 0..255 (every byte value).
     * All chars are {@code <= 0xFF}, so on JDK 9+ this is a LATIN1 (coder 0) String
     * with a 256-byte backing byte[]; on JDK 8 a 256-char char[].  Owns a private
     * backing array via new String(char[]).
     */
    private static String allBytes()
    {
        final char[] buf = new char[256];
        for (int i = 0; i < 256; i++)
        {
            buf[i] = (char) i;
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
                jLatin1Lo80Cp0 = latin1Lo80.codePointAt(0);
                jLatin1HiCp0   = latin1Hi.codePointAt(0);
                jControlsLen   = controls.length();
                jControlsCp3   = controls.codePointAt(3);
                jWhitespaceLen = whitespace.length();
                jNulLatin1Len  = nulLatin1.length();
                jNulLatin1Cp1  = nulLatin1.codePointAt(1);
                jAll256Len     = all256.length();
                jAll256Cp0     = all256.codePointAt(0);
                jAll256Cp255   = all256.codePointAt(255);
                jLongAsciiLen  = longAscii.length();
                jInternedLen   = interned.length();
                jOneCjkCp0     = oneCjk.codePointAt(0);
                jNihongoLen    = nihongo.length();
                jNihongoCp0    = nihongo.codePointAt(0);
                jNihongoCp1    = nihongo.codePointAt(1);
                jNihongoCp2    = nihongo.codePointAt(2);
                jMixedLen      = mixed.length();
                jBmp2to3aCp0   = bmp2to3a.codePointAt(0);
                jBmp2to3bCp0   = bmp2to3b.codePointAt(0);
                jBelowSurrCp0  = belowSurr.codePointAt(0);
                jAboveSurrCp0  = aboveSurr.codePointAt(0);
                jReplacementCp0 = replacement.codePointAt(0);
                jMaxBmpCp0     = maxBmp.codePointAt(0);
                jNulUtf16Len   = nulUtf16.length();
                jNulUtf16Cp1   = nulUtf16.codePointAt(1);
                jEmojiCpCount  = emoji.codePointCount(0, emoji.length());
                jEmojiCp0      = emoji.codePointAt(0);
                jEmojiMixLen   = emojiMix.length();
                jEmojiMixCpCount = emojiMix.codePointCount(0, emojiMix.length());
                jLoneHighLen   = loneHigh.length();
                jLoneHighCp0   = loneHigh.codePointAt(0);
                jLoneLowLen    = loneLow.length();
                jLoneLowCp0    = loneLow.codePointAt(0);
                jReversedPairLen = reversedPair.length();
                jHighAtEndLen  = highAtEnd.length();
                jLongCjkLen    = longCjk.length();
                jCap4096Len    = cap4096.length();
                jCap4097Len    = cap4097.length();
                jCap70000Len   = cap70000.length();
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
                jCoderAll256    = coderOf(all256);
                jCoderNihongo   = coderOf(nihongo);
                jCoderEmoji     = coderOf(emoji);
                jCoderLongCjk   = coderOf(longCjk);
                jCoderLongAscii = coderOf(longAscii);
                jCoderMaxBmp    = coderOf(maxBmp);

                ReadJavaString.done = true;
            }
        });
    }
}

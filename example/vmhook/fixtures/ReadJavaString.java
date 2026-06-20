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
 *     - firstAstral U+10000          -> the FIRST astral code point (surrogate
 *                                       pair D800 DC00): the 3-byte/4-byte
 *                                       boundary of append_utf8 and the MINIMUM of
 *                                       the surrogate-combine formula -> F0 90 80 80.
 *     - maxAstral  U+10FFFF          -> the MAX valid code point (surrogate pair
 *                                       DBFF DFFF): the top of the 4-byte range and
 *                                       the MAXIMUM combine result -> F4 8F BF BF.
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
 *     - cap4095    4095x 'x'    -> ONE char below the OLD cap (last length the old
 *                                  guard accepted) -> read in full (4095).
 *     - cap4096    4096x 'x'    -> LATIN1 byte[] length 4096 -> read in full.
 *     - cap4097    4097x 'x'    -> ONE char past the OLD cap -> read in full (4097).
 *     - cap70000   70000x 'x'   -> a length WAY over the OLD cap (and over 65536):
 *                                  proves the int32 length is read intact and the
 *                                  full body is decoded (no overflow) -> 70000 bytes.
 *     - cap1M      1000000x 'x' -> a MULTI-MEGABYTE LATIN1 String (byte[] length
 *                                  1e6), far over the old cap, well under the 16M
 *                                  ceiling -> read in full (1e6 bytes); stresses the
 *                                  ~1 MB body safe_read into the sized heap buffer.
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
    // U+10000 -- the FIRST astral code point (the 3-byte/4-byte boundary of
    // append_utf8 and the MINIMUM of the surrogate-combine formula: high D800 +
    // low DC00 -> 0x10000 + 0 + 0).  Carried as the lowest surrogate pair
    // (D800 DC00); read_java_string must combine it into the single 4-byte UTF-8
    // sequence F0 90 80 80.  Complements `emoji` (an interior astral code point)
    // by pinning the LOW edge of the astral / 4-byte range.
    public static String firstAstral = fresh(new char[] { '\uD800', '\uDC00' });
    // U+10FFFF -- the MAXIMUM valid Unicode code point (the top of the 4-byte
    // range and the MAXIMUM of the surrogate-combine formula: high DBFF + low
    // DFFF -> 0x10000 + 0xFFC00 + 0x3FF = 0x10FFFF).  Carried as the highest
    // surrogate pair (DBFF DFFF); read_java_string must combine it into the
    // single 4-byte UTF-8 sequence F4 8F BF BF.  Pins the HIGH edge of astral.
    public static String maxAstral = fresh(new char[] { '\uDBFF', '\uDFFF' });
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

    // --- EXPANDED: more surrogate-combine / NUL-position / BMP-boundary shapes ---
    // TWO CONSECUTIVE surrogate pairs (U+1F600 U+1F601): after combining the first
    // pair and advancing the index by 2, the loop must pick up the SECOND pair and
    // combine it too -> two independent 4-byte UTF-8 sequences (8 bytes total,
    // F0 9F 98 80 F0 9F 98 81).  No ASCII separator (unlike emojiMix), so this is
    // the back-to-back combine case.
    public static String twoEmoji = fresh(new char[] { '\uD83D', '\uDE00', '\uD83D', '\uDE01' });
    // A high surrogate followed by a NON-low-surrogate BMP char (U+D83D then 'A'):
    // the combine INNER guard (low in DC00..DFFF) is FALSE at a NON-end index, so
    // the high surrogate emits as a lone 3-byte CESU and the following 'A' decodes
    // normally -> ED A0 BD 41 (4 bytes).  Distinct from highAtEnd (high is LAST so
    // the (i+1)<count OUTER guard is false) and reversedPair (low THEN high).
    public static String highThenBmp = fresh(new char[] { '\uD83D', 'A' });
    // A high surrogate followed by ANOTHER high surrogate (U+D83D U+D83D): the inner
    // guard is false (the second unit is not a LOW surrogate), so the first emits as
    // a lone 3-byte CESU and the loop re-examines the second high surrogate -- which,
    // now last, also emits as a lone CESU -> ED A0 BD ED A0 BD (6 bytes).
    public static String highThenHigh = fresh(new char[] { '\uD83D', '\uD83D' });
    // A surrogate pair with an interior NUL AFTER it (emoji then NUL): proves the
    // NUL following an astral code point is preserved and the index advance from the
    // combine landed correctly -> F0 9F 98 80 00 (5 bytes).
    public static String emojiThenNul = fresh(new char[] { '\uD83D', '\uDE00', '\0' });

    // LEADING / TRAILING / ALL-NUL strings: the std::string is sized by the array
    // length, NOT cut at the first NUL like a C string -- prove that at every NUL
    // position, on BOTH coders.
    // Leading NUL then ASCII (LATIN1): 00 61 62 (3 bytes).
    public static String leadingNul = fresh(new char[] { '\0', 'a', 'b' });
    // ASCII then trailing NUL (LATIN1): 61 62 00 (3 bytes).
    public static String trailingNul = fresh(new char[] { 'a', 'b', '\0' });
    // ALL NULs (LATIN1): three U+0000 chars -> 00 00 00 (3 bytes), not "".
    public static String allNul = fresh(new char[] { '\0', '\0', '\0' });
    // A NUL char promoted to the UTF-16 path by a neighbouring >0xFF char, with the
    // NUL FIRST (U+0000 then U+65E5): 00 E6 97 A5 (4 bytes).  Complements nulUtf16
    // (interior NUL) by putting the NUL at index 0 of a UTF-16 backing.
    public static String leadingNulUtf16 = fresh(new char[] { '\0', '\u65E5' });

    // BMP boundary fillers the existing set is missing:
    // U+007F -- the LAST 1-byte UTF-8 code point, as a STANDALONE single char (the
    // 1-byte/2-byte boundary from below; latin1Lo80 pins it from above at U+0080).
    public static String last1Byte = fresh(new char[] { '\u007F' });
    // U+0100 (Latin Capital A with macron) -- a >0xFF char so coder 1 (UTF16): a
    // mid-range 2-byte UTF-8 code point on the UTF-16 path (C4 80).  bmp2to3a pins
    // the TOP of the 2-byte range (U+07FF); this fills the interior.
    public static String midTwoByte = fresh(new char[] { '\u0100' });
    // U+0080 forced through the UTF-16 path: U+0080 alone is LATIN1, so pair it with
    // a >0xFF char to promote the whole string to coder 1.  Proves the FIRST 2-byte
    // code point (C2 80) is produced by the UTF-16 decode path too, not only LATIN1.
    public static String firstTwoByteUtf16 = fresh(new char[] { '\u0080', '\u4E2D' });
    // U+0801 -- ONE past the first 3-byte code point (U+0800), confirms the 3-byte
    // encoder is not an off-by-one at its low edge (E0 A0 81).
    public static String firstThreeBytePlus1 = fresh(new char[] { '\u0801' });
    // U+FEFF -- a byte-order mark carried as a normal BMP char (NOT stripped):
    // read_java_string is a verbatim decoder, so a leading BOM stays EF BB BF.
    public static String bom = fresh(new char[] { '\uFEFF', 'a' });
    // 'e' + U+0301 (combining acute accent): a two-code-point grapheme through the
    // UTF-16 path (the U+0301 > 0xFF promotes it) -> 65 CC 81 (3 bytes).  Realistic
    // decomposed text; proves sequential BMP decode across a 1-byte then 2-byte char.
    public static String combining = fresh(new char[] { 'e', '\u0301' });

    // BULK boundary cases (modest size -- keep heap small):
    // 500x U+00E9 (LATIN1): every char takes the 2-byte LATIN1 encode -> 1000 bytes
    // of repeating C3 A9.  longAscii exercises the 1-byte LATIN1 loop in bulk; this
    // exercises the 2-byte LATIN1 encode in bulk (all256 only hits each high byte
    // once).
    public static String bulkLatin1Hi = repeatChar('\u00E9', 500);
    // 200x U+1F600 (UTF-16 surrogate pairs): 200 astral code points back-to-back,
    // exercising the surrogate-combine + index-advance loop IN BULK (longCjk is BMP
    // only; the bulk combine path was otherwise untested) -> 200*4 = 800 bytes.
    public static String bulkEmoji = repeatAstral(0x1F600, 200);

    // --- BATCH-18: astral PLANE-WALK + 3-byte boundary + coder-boundary shapes ---
    // The append_utf8 4-byte branch derives the lead byte from (cp >> 18): U+10000
    // ..U+3FFFF lead with F0, U+40000..U+FFFFF with F1/F2/F3, U+100000..U+10FFFF with
    // F4.  firstAstral (U+10000) and maxAstral (U+10FFFF) pin only the two extremes;
    // these fillers walk the lead byte and the continuation-byte arithmetic through
    // the interior so a (cp >> 18) / (cp >> 12) shift-or-mask error cannot hide.  All
    // non-ASCII chars are written as \\uXXXX escapes so the source stays pure ASCII.
    // U+10001 -- first astral + 1: combine low bit set (lo == DC01) -> F0 90 80 81.
    public static String astral10001  = fresh(new char[] { '\uD800', '\uDC01' });
    // U+1FFFF -- TOP of plane 1 (still F0 lead): pair D83F DFFF -> F0 9F BF BF.
    public static String astral1FFFF  = fresh(new char[] { '\uD83F', '\uDFFF' });
    // U+20000 -- START of plane 2 (F0 lead, second byte rolls to A0): pair D840 DC00
    // -> F0 A0 80 80.  Proves the (cp >> 12) & 3F continuation advances correctly.
    public static String astral20000  = fresh(new char[] { '\uD840', '\uDC00' });
    // U+FFFFF -- TOP of plane 15 (F3 lead): pair DBBF DFFF -> F3 BF BF BF.  Walks the
    // 4-byte lead nibble OFF F0 to F3 -- the interior firstAstral/maxAstral skip.
    public static String astralFFFFF  = fresh(new char[] { '\uDBBF', '\uDFFF' });
    // U+100000 -- START of plane 16 (F4 lead, all continuation bytes 80): pair DBC0
    // DC00 -> F4 80 80 80.  The low edge of the F4 lead range (maxAstral is its top).
    public static String astral100000 = fresh(new char[] { '\uDBC0', '\uDC00' });

    // U+07FE -- ONE below the 2-byte top (U+07FF): interior of the 2-byte range,
    // DF BE.  bmp2to3a pins the top; this guards the (cp >> 6) low bits.
    public static String bmp07FE      = fresh(new char[] { '\u07FE' });
    // U+FFFE -- a NONCHARACTER just below U+FFFF: a verbatim 3-byte char (EF BF BE),
    // NOT stripped or replaced.  Distinct from replacement (U+FFFD) and maxBmp (FFFF):
    // proves read_java_string does not special-case Unicode noncharacters.
    public static String noncharFFFE  = fresh(new char[] { '\uFFFE' });
    // U+1000 -- the FIRST E1-lead 3-byte code point (E1 80 80): bmp2to3b pins the
    // FIRST 3-byte point (U+0800, E0 lead); this walks the 3-byte lead nibble off E0.
    public static String bmp1000      = fresh(new char[] { '\u1000' });

    // COMPACT-STRING CODER BOUNDARY (identical-output property): the SAME ASCII
    // content decoded out of a LATIN1 backing and out of a UTF16 backing must yield
    // byte-identical UTF-8.  asciiPlain is pure ASCII "AB" so the JVM stores it
    // LATIN1 (coder 0) -> 41 42.  asciiInUtf16 is "AB" + a >0xFF CJK char, which
    // promotes the WHOLE String to UTF16 (coder 1); its first two chars are the same
    // ASCII 'A','B' pushed through the UTF-16 decode path -> 41 42 E4 B8 AD.  The
    // module asserts the ASCII prefix (first two bytes) of the UTF16 decode equals
    // asciiPlain byte-for-byte: ASCII content decodes the same on either coder.
    public static String asciiPlain   = fresh(new char[] { 'A', 'B' });
    public static String asciiInUtf16 = fresh(new char[] { 'A', 'B', '\u4E2D' });

    // MULTI-SCRIPT single String: Latin 'A', Greek alpha (U+03B1, 2-byte CE B1),
    // Cyrillic (U+0414, 2-byte D0 94), Hebrew (U+05D0, 2-byte D7 90), CJK (U+4E2D,
    // 3-byte E4 B8 AD), astral emoji (U+1F600, 4-byte F0 9F 98 80) -- every UTF-8
    // width 1..4 in one decode pass, forcing the UTF-16 path (the astral + >0xFF
    // chars) to handle mixed-width output back-to-back.
    public static String multiScript  = fresh(new char[] {
        'A', '\u03B1', '\u0414', '\u05D0', '\u4E2D', '\uD83D', '\uDE00' });

    // U+FFFD (replacement char) used as CONTENT, flanked by ASCII: 'a', U+FFFD, 'b'
    // -> 61 EF BF BD 62 (5 bytes).  Proves a replacement char that is genuinely part
    // of the string survives as its 3-byte UTF-8 -- read_java_string never INSERTS or
    // removes U+FFFD (it is a passthrough decoder, not a sanitiser).
    public static String replInAscii  = fresh(new char[] { 'a', '\uFFFD', 'b' });

    // A NUL immediately BEFORE a surrogate pair (NUL, hi, lo): proves the combine's
    // index advance is correct when a pair starts at index 1, and the leading NUL is
    // preserved -> 00 F0 9F 98 80 (5 bytes).  Complements emojiThenNul (NUL after).
    public static String nulThenEmoji = fresh(new char[] { '\0', '\uD83D', '\uDE00' });

    // A lone LOW surrogate THEN a VALID pair (lo, hi, lo): the leading lone low emits
    // as 3-byte CESU, then the decoder RESYNCS and combines the following hi+lo into
    // one 4-byte astral char -> ED B0 80 F0 9F 98 80 (7 bytes).  Proves a malformed
    // unit does not desync the combine for the well-formed pair that follows.
    public static String lowThenPair  = fresh(new char[] { '\uDC00', '\uD83D', '\uDE00' });

    // A VALID pair flanked by 3-byte CJK (emojiMix is the 1-byte-ASCII-flanked
    // variant): CJK U+4E2D, emoji U+1F600, CJK U+4E2D -> E4 B8 AD F0 9F 98 80
    // E4 B8 AD (11 bytes).  Proves the surrogate combine advances correctly when
    // flanked by 3-byte (not 1-byte ASCII) neighbours.
    public static String emojiCjkFlank = fresh(new char[] { '\u4E2D', '\uD83D', '\uDE00', '\u4E2D' });

    // Long-String read-in-full targets (robustness bug #29 FIXED: no more 4096 cap;
    // read_java_string reads IN FULL up to 16M chars, uniformly per char count) ---
    // 4095 ASCII chars: ONE char BELOW the OLD 4096 cap -- the last length the old
    // guard ACCEPTED.  Pairs with cap4096 / cap4097 to pin the boundary triple
    // (4095 / 4096 / 4097) straddling the removed hard cap.
    public static String cap4095    = repeatChar('x', 4095);
    // Exactly 4096 ASCII chars: LATIN1 byte[] length 4096 -> read in full.
    public static String cap4096    = repeatChar('x', 4096);
    // 4097 ASCII chars: ONE char past the OLD 4096 cap -> read in full (4097 bytes).
    public static String cap4097    = repeatChar('x', 4097);
    // 70000 ASCII chars: a length WAY past the OLD cap and past 65536.  Proves the
    // int32 length is read intact and the full 70000-byte body is decoded.
    public static String cap70000   = repeatChar('x', 70000);
    // 1,000,000 ASCII chars: a MULTI-MEGABYTE LATIN1 String (byte[] length 1e6),
    // far above the old cap yet well under the 16M-char ceiling.  Exercises the
    // full-read path at a size where the body safe_read copies ~1 MB into the
    // sized heap buffer -- the single biggest "read a big String IN FULL" proof.
    // Built OUTSIDE any detour (a static initializer, a pure read on the native
    // side), so it never forces a mid-detour GC.
    public static String cap1M      = repeatChar('x', 1000000);
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
    public static volatile int     jFirstAstralCp0; // 0x10000  (first astral)
    public static volatile int     jFirstAstralCpCount; // 1
    public static volatile int     jMaxAstralCp0;   // 0x10FFFF (max code point)
    public static volatile int     jMaxAstralCpCount;   // 1
    public static volatile int     jLoneHighLen;    // 1
    public static volatile int     jLoneHighCp0;    // 0xD83D (lone surrogate)
    public static volatile int     jLoneLowLen;     // 1
    public static volatile int     jLoneLowCp0;     // 0xDC00 (lone low surrogate)
    public static volatile int     jReversedPairLen;// 2
    public static volatile int     jHighAtEndLen;   // 2 (chars: X, hi surrogate)
    public static volatile int     jLongCjkLen;     // 300
    public static volatile int     jCap4095Len;     // 4095
    public static volatile int     jCap4096Len;     // 4096
    public static volatile int     jCap4097Len;     // 4097
    public static volatile int     jCap70000Len;    // 70000
    public static volatile int     jCap1MLen;       // 1000000
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

    // --- EXPANDED-coverage cross-check witnesses ---------------------------
    public static volatile int     jTwoEmojiLen;        // 4 (2 surrogate pairs)
    public static volatile int     jTwoEmojiCpCount;    // 2
    public static volatile int     jTwoEmojiCp0;        // 0x1F600
    public static volatile int     jTwoEmojiCp1;        // 0x1F601 (codePointAt(2))
    public static volatile int     jHighThenBmpLen;     // 2 (hi surrogate, 'A')
    public static volatile int     jHighThenBmpCp0;     // 0xD83D (lone)
    public static volatile int     jHighThenBmpCp1;     // 'A' == 0x41 (codePointAt(1))
    public static volatile int     jHighThenHighLen;    // 2 (two hi surrogates)
    public static volatile int     jHighThenHighCp0;    // 0xD83D
    public static volatile int     jHighThenHighCp1;    // 0xD83D (codePointAt(1))
    public static volatile int     jEmojiThenNulLen;    // 3 (hi, lo, NUL)
    public static volatile int     jEmojiThenNulCpCount;// 2 (U+1F600, U+0000)
    public static volatile int     jLeadingNulLen;      // 3
    public static volatile int     jLeadingNulCp0;      // 0 (the leading NUL)
    public static volatile int     jTrailingNulLen;     // 3
    public static volatile int     jTrailingNulCp2;     // 0 (the trailing NUL)
    public static volatile int     jAllNulLen;          // 3
    public static volatile int     jLeadingNulUtf16Len; // 2 (NUL, U+65E5)
    public static volatile int     jLeadingNulUtf16Cp0; // 0
    public static volatile int     jLeadingNulUtf16Cp1; // 0x65E5
    public static volatile int     jLast1ByteCp0;       // 0x007F
    public static volatile int     jMidTwoByteCp0;      // 0x0100
    public static volatile int     jFirstTwoByteUtf16Cp0;   // 0x0080
    public static volatile int     jFirstTwoByteUtf16Cp1;   // 0x4E2D
    public static volatile int     jFirstThreeBytePlus1Cp0; // 0x0801
    public static volatile int     jBomCp0;             // 0xFEFF
    public static volatile int     jBomLen;             // 2
    public static volatile int     jCombiningLen;       // 2 ('e', U+0301)
    public static volatile int     jCombiningCp1;       // 0x0301
    public static volatile int     jBulkLatin1HiLen;    // 500
    public static volatile int     jBulkEmojiLen;       // 400 (200 surrogate pairs)
    public static volatile int     jBulkEmojiCpCount;   // 200
    public static volatile int     jCoderTwoEmoji;      // 1 (UTF16) / -1
    public static volatile int     jCoderBulkLatin1Hi;  // 0 (LATIN1) / -1
    public static volatile int     jCoderMidTwoByte;    // 1 (UTF16) / -1
    public static volatile int     jCoderBom;           // 1 (UTF16) / -1

    // --- BATCH-18 cross-check witnesses ---------------------------------------
    public static volatile int     jAstral10001Cp0;     // 0x10001
    public static volatile int     jAstral10001CpCount; // 1
    public static volatile int     jAstral1FFFFCp0;     // 0x1FFFF
    public static volatile int     jAstral20000Cp0;     // 0x20000
    public static volatile int     jAstralFFFFFCp0;     // 0xFFFFF
    public static volatile int     jAstral100000Cp0;    // 0x100000
    public static volatile int     jBmp07FECp0;         // 0x07FE
    public static volatile int     jNoncharFFFECp0;     // 0xFFFE
    public static volatile int     jBmp1000Cp0;         // 0x1000
    public static volatile int     jAsciiPlainLen;      // 2
    public static volatile int     jAsciiInUtf16Len;    // 3
    public static volatile int     jMultiScriptLen;     // 7 (chars; 6 code points)
    public static volatile int     jMultiScriptCpCount; // 6
    public static volatile int     jMultiScriptCpLast;  // 0x1F600
    public static volatile int     jReplInAsciiLen;     // 3
    public static volatile int     jReplInAsciiCp1;     // 0xFFFD
    public static volatile int     jNulThenEmojiLen;    // 3 (NUL, hi, lo)
    public static volatile int     jNulThenEmojiCpCount;// 2 (U+0000, U+1F600)
    public static volatile int     jLowThenPairLen;     // 3 (lo, hi, lo)
    public static volatile int     jLowThenPairCpCount; // 2 (U+DC00 lone, U+1F600)
    public static volatile int     jEmojiCjkFlankLen;   // 4 (CJK, hi, lo, CJK)
    public static volatile int     jEmojiCjkFlankCpCount; // 3
    public static volatile int     jCoderAsciiPlain;    // 0 (LATIN1) / -1
    public static volatile int     jCoderAsciiInUtf16;  // 1 (UTF16) / -1
    public static volatile int     jCoderMultiScript;   // 1 (UTF16) / -1

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
     * Build a fresh String of {@code n} repeats of the astral code point
     * {@code cp} (cp &gt;= 0x10000), each carried as a UTF-16 surrogate pair.  Used
     * for the bulk surrogate-combine subject; owns a private char[] via
     * new String(char[]).  Surrogate split is done by hand to keep the source pure
     * ASCII and Java 8 safe.
     */
    private static String repeatAstral(final int cp, final int n)
    {
        final char hi = (char) (0xD800 + ((cp - 0x10000) >> 10));
        final char lo = (char) (0xDC00 + ((cp - 0x10000) & 0x3FF));
        final char[] buf = new char[n * 2];
        for (int i = 0; i < n; i++)
        {
            buf[i * 2]     = hi;
            buf[i * 2 + 1] = lo;
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
                jFirstAstralCp0 = firstAstral.codePointAt(0);
                jFirstAstralCpCount = firstAstral.codePointCount(0, firstAstral.length());
                jMaxAstralCp0  = maxAstral.codePointAt(0);
                jMaxAstralCpCount = maxAstral.codePointCount(0, maxAstral.length());
                jLoneHighLen   = loneHigh.length();
                jLoneHighCp0   = loneHigh.codePointAt(0);
                jLoneLowLen    = loneLow.length();
                jLoneLowCp0    = loneLow.codePointAt(0);
                jReversedPairLen = reversedPair.length();
                jHighAtEndLen  = highAtEnd.length();
                jLongCjkLen    = longCjk.length();
                jCap4095Len    = cap4095.length();
                jCap4096Len    = cap4096.length();
                jCap4097Len    = cap4097.length();
                jCap70000Len   = cap70000.length();
                jCap1MLen      = cap1M.length();
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

                // --- EXPANDED-coverage witnesses ---------------------------
                jTwoEmojiLen        = twoEmoji.length();
                jTwoEmojiCpCount    = twoEmoji.codePointCount(0, twoEmoji.length());
                jTwoEmojiCp0        = twoEmoji.codePointAt(0);
                jTwoEmojiCp1        = twoEmoji.codePointAt(2);
                jHighThenBmpLen     = highThenBmp.length();
                jHighThenBmpCp0     = highThenBmp.codePointAt(0);
                jHighThenBmpCp1     = highThenBmp.codePointAt(1);
                jHighThenHighLen    = highThenHigh.length();
                jHighThenHighCp0    = highThenHigh.codePointAt(0);
                jHighThenHighCp1    = highThenHigh.codePointAt(1);
                jEmojiThenNulLen    = emojiThenNul.length();
                jEmojiThenNulCpCount = emojiThenNul.codePointCount(0, emojiThenNul.length());
                jLeadingNulLen      = leadingNul.length();
                jLeadingNulCp0      = leadingNul.codePointAt(0);
                jTrailingNulLen     = trailingNul.length();
                jTrailingNulCp2     = trailingNul.codePointAt(2);
                jAllNulLen          = allNul.length();
                jLeadingNulUtf16Len = leadingNulUtf16.length();
                jLeadingNulUtf16Cp0 = leadingNulUtf16.codePointAt(0);
                jLeadingNulUtf16Cp1 = leadingNulUtf16.codePointAt(1);
                jLast1ByteCp0       = last1Byte.codePointAt(0);
                jMidTwoByteCp0      = midTwoByte.codePointAt(0);
                jFirstTwoByteUtf16Cp0 = firstTwoByteUtf16.codePointAt(0);
                jFirstTwoByteUtf16Cp1 = firstTwoByteUtf16.codePointAt(1);
                jFirstThreeBytePlus1Cp0 = firstThreeBytePlus1.codePointAt(0);
                jBomCp0             = bom.codePointAt(0);
                jBomLen             = bom.length();
                jCombiningLen       = combining.length();
                jCombiningCp1       = combining.codePointAt(1);
                jBulkLatin1HiLen    = bulkLatin1Hi.length();
                jBulkEmojiLen       = bulkEmoji.length();
                jBulkEmojiCpCount   = bulkEmoji.codePointCount(0, bulkEmoji.length());
                jCoderTwoEmoji      = coderOf(twoEmoji);
                jCoderBulkLatin1Hi  = coderOf(bulkLatin1Hi);
                jCoderMidTwoByte    = coderOf(midTwoByte);
                jCoderBom           = coderOf(bom);


                // --- BATCH-18 witness assignments --------------------------
                jAstral10001Cp0     = astral10001.codePointAt(0);
                jAstral10001CpCount = astral10001.codePointCount(0, astral10001.length());
                jAstral1FFFFCp0     = astral1FFFF.codePointAt(0);
                jAstral20000Cp0     = astral20000.codePointAt(0);
                jAstralFFFFFCp0     = astralFFFFF.codePointAt(0);
                jAstral100000Cp0    = astral100000.codePointAt(0);
                jBmp07FECp0         = bmp07FE.codePointAt(0);
                jNoncharFFFECp0     = noncharFFFE.codePointAt(0);
                jBmp1000Cp0         = bmp1000.codePointAt(0);
                jAsciiPlainLen      = asciiPlain.length();
                jAsciiInUtf16Len    = asciiInUtf16.length();
                jMultiScriptLen     = multiScript.length();
                jMultiScriptCpCount = multiScript.codePointCount(0, multiScript.length());
                jMultiScriptCpLast  = multiScript.codePointAt(multiScript.length() - 2);
                jReplInAsciiLen     = replInAscii.length();
                jReplInAsciiCp1     = replInAscii.codePointAt(1);
                jNulThenEmojiLen    = nulThenEmoji.length();
                jNulThenEmojiCpCount = nulThenEmoji.codePointCount(0, nulThenEmoji.length());
                jLowThenPairLen     = lowThenPair.length();
                jLowThenPairCpCount = lowThenPair.codePointCount(0, lowThenPair.length());
                jEmojiCjkFlankLen   = emojiCjkFlank.length();
                jEmojiCjkFlankCpCount = emojiCjkFlank.codePointCount(0, emojiCjkFlank.length());
                jCoderAsciiPlain    = coderOf(asciiPlain);
                jCoderAsciiInUtf16  = coderOf(asciiInUtf16);
                jCoderMultiScript   = coderOf(multiScript);

                ReadJavaString.done = true;
            }
        });
    }
}

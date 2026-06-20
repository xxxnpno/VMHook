package vmhook.fixtures;

import vmhook.Harness;

/**
 * Exhaustive fixture for the {@code field_string} feature (String field get AND
 * set through vmhook's zero-JNI field_proxy / read_java_string / store_string).
 *
 * The native module (tests/jvm/modules/field_string.cpp) drives this fixture in
 * two phases around a single go/done handshake:
 *
 *   1. BEFORE raising go, native performs every {@code field_proxy::set(...)}
 *      write it wants to test.  set() builds a fresh java.lang.String and rebinds
 *      the field reference to it (an object-reference store, library bug #30 fixed),
 *      so no Java bytecode needs to run for the write itself.
 *   2. When go is raised, this fixture's probe runs on the Java thread.  It:
 *        - calls a hooked instance method (touchString) so an interpreter hook
 *          fires on a real bytecode dispatch (mirrors the pilot contract), and
 *        - reads every just-rebound field *through Java* and publishes the
 *          observations into the volatile result fields below, so the native
 *          side can prove the writes are visible to Java itself (not just to
 *          vmhook's own read path).
 *
 * GET fields are deliberately constructed to span every decode path of
 * read_java_string on a modern (JDK 9+) compact-string VM.  read_java_string
 * UTF-8-ENCODES every code point (it no longer substitutes '?' for non-ASCII):
 *   - pure ASCII             -> LATIN1 (coder 0), byte-verbatim
 *   - Latin-1 (cp <= 0xFF)   -> LATIN1 (coder 0), each cp UTF-8-encoded
 *                               (0xE9 -> C3 A9, 0x80 -> C2 80, 0xFF -> C3 BF)
 *   - supplementary / CJK    -> UTF-16 (coder 1), decoded to multi-byte UTF-8;
 *                               astral code points combine the surrogate pair
 *                               into one 4-byte sequence
 *   - empty / null           -> the length<=0 and null-oop guard paths
 *   - >4096 chars            -> the length>4096 rejection path (returns "")
 *
 * Every SET-target String is allocated with {@code new String(...)} (or built
 * char-by-char) so it owns a PRIVATE, non-interned backing array.  set() now
 * rebinds the field reference rather than mutating backing in place, so this can
 * no longer corrupt a shared literal; the private backing additionally lets the
 * fixture alias a SET target's original object and prove the rebind left it intact.
 *
 * The class extends {@link FieldStringBase} so the module can also read a String
 * field declared on a SUPERCLASS (inherited get, via vmhook::find_field's super
 * walk).  It is therefore NOT final.
 *
 * Target Java 8 syntax: no var / records / switch-expressions.
 */
public class FieldString extends FieldStringBase
{
    // ----- go/done handshake the native run_probe drives -------------------
    public static volatile boolean go;
    public static volatile boolean done;

    // ================= GET targets (static) ================================
    // ASCII only -> stored LATIN1 (coder 0) on JDK 9+.
    public static String getAscii      = "hello world";
    // Single char, ASCII.
    public static String getOneChar    = "Z";
    // All code points <= 0xFF (Latin-1) -> still LATIN1 coder 0; each cp is
    // UTF-8-encoded by read_java_string (0xE9 -> C3 A9, 0xFF -> C3 BF).
    public static String getLatin1      = makeLatin1();      // "héllo èéÿ"
    // Contains code points > 0xFF -> forced to UTF-16 coder 1 -> multi-byte UTF-8.
    public static String getCjk         = "日本語";          // 日本語
    // Mixed ASCII + a >0xFF char -> whole string promoted to UTF-16 coder 1.
    public static String getMixed       = "A日BéC";             // A 日 B é C
    // Empty string -> length 0 -> read_java_string length<=0 guard.
    public static String getEmpty       = "";
    // Explicit null reference -> field compressed-OOP is 0 -> null-oop guard.
    public static String getNull        = null;
    // Interned literal (identity-shared); reading must not mutate it.
    public static String getInterned    = "INTERNED_LITERAL";
    // Long-String read-in-full targets (robustness bug #29 FIXED: the old hard
    // 4096-byte cap is gone; read_java_string reads IN FULL up to 16M chars,
    // uniformly per decoded char count). ---
    // Exactly 4096 ASCII chars -> LATIN1 byte length 4096 -> read in full.
    public static String getLen4096     = repeat('x', 4096);
    // 4097 ASCII chars -> ONE char past the OLD cap -> read in full (4097 bytes).
    public static String getLen4097     = repeat('y', 4097);
    // 5000 ASCII chars -> well past the OLD cap -> read in full (5000 bytes).
    public static String getLen5000     = repeat('z', 5000);
    // 2048 CJK chars -> UTF-16 byte length 4096 -> 2048*3 UTF-8 bytes.
    public static String getCjk2048     = repeat('日', 2048);
    // 2049 CJK chars -> UTF-16 byte length 4098 (2049 chars) -> read in full on
    // both layouts -> 2049*3 UTF-8 bytes (no more asymmetric UTF-16 cap).
    public static String getCjk2049     = repeat('日', 2049);
    // Embedded NUL: ASCII bytes with a 0x00 in the middle (LATIN1, length 5).
    public static String getEmbeddedNul = makeEmbeddedNul();             // a\0b\0c

    // ----- exhaustive GET extras (the "every possible read" battery) -------
    // Astral emoji (U+1F600) flanked by ASCII: stored UTF-16 (coder 1) as a
    // surrogate PAIR; read_java_string must COMBINE the pair into one 4-byte
    // UTF-8 sequence and not swallow the trailing 'Y'.  Built from code points
    // (no raw-emoji source literal needed) -> 'X' + U+1F600 + 'Y'.
    public static String getEmoji       = makeEmoji();                   // X U+1F600 Y
    // The same astral code point ALONE -> exactly one 4-byte UTF-8 sequence.
    public static String getAstral      = new String(Character.toChars(0x1F600));
    // Max BMP code point U+FFFF -> UTF-16 (coder 1), 3-byte UTF-8 EF BF BF.
    public static String getMaxBmp      = new String(new char[]{ (char) 0xFFFF });
    // U+0080 -> the smallest non-ASCII; STILL LATIN1 (coder 0) but the UTF-8
    // output is the TWO bytes C2 80 (the 1->2 byte encoder boundary on the
    // LATIN1 path).
    public static String getLo80        = new String(new char[]{ (char) 0x0080 });
    // U+0800 -> UTF-16 (coder 1), the FIRST 3-byte UTF-8 code point (E0 A0 80),
    // i.e. the append_utf8 2->3 byte boundary reached via the UTF-16 path.
    public static String getU0800       = new String(new char[]{ (char) 0x0800 });
    // EVERY byte value 0x00..0xFF in one LATIN1 (coder 0) String.  Decodes to
    // 384 UTF-8 bytes: 128 ASCII (1 byte each, incl. a leading interior NUL) +
    // 128 high (2 bytes each, C2 80 .. C3 BF).  The most thorough LATIN1 proof.
    public static String getAll256      = makeAll256();
    // A static FINAL String whose initializer is a COMPILE-TIME CONSTANT.  javac
    // constant-folds every *bytecode read* of it into the using class's constant
    // pool, but the field SLOT still exists with real backing storage holding the
    // value (verified on JDK 21) — so reading it via the field_proxy/raw decode
    // returns the actual String, not "".  Characterizes folded-constant storage.
    public static final String getFinalConst = "FINAL_CONSTANT";
    // A field the PROBE reassigns to a new String between two native reads, to
    // prove a fresh field_proxy read sees the NEW backing (GC-sensitive -> [INFO]).
    public static String getReassign    = freshAscii("before");          // -> "after2"

    // ----- batch-17 GET deepening: "every possible read" the battery lacked ----
    // ASCII control characters (TAB 0x09, LF 0x0A, CR 0x0D) embedded between
    // letters.  All <= 0x7F so the String stores LATIN1 (coder 0) and decodes
    // byte-verbatim — proves read_java_string preserves control bytes (no
    // newline/tab normalisation) and is line-ending agnostic.  7 bytes.
    public static String getControls    = makeControls();                // a\t b\n c\r d

    // COMPACT-STRING CODER BOUNDARY — content identity across both backings.
    // The same shared ASCII prefix "Shared!" is the head of TWO Strings: one that
    // stays LATIN1 (coder 0, all cp <= 0xFF) and one promoted to UTF-16 (coder 1)
    // by a trailing astral scalar.  read_java_string must decode the shared prefix
    // to the IDENTICAL 7 UTF-8 bytes on BOTH layouts (the native side asserts the
    // first 7 decoded bytes are equal), proving the LATIN1 and UTF-16 decode paths
    // agree on common content.  The Latin1 one also carries a trailing 'é' (cp
    // 0x00E9, still <= 0xFF -> stays coder 0).
    public static String getCoderLatin1Prefix = makeCoderLatin1Prefix();  // "Shared!é"
    public static String getCoderUtf16Prefix  = makeCoderUtf16Prefix();   // "Shared!" + U+1F4A9

    // U+07FF -> the LAST 2-byte UTF-8 code point (DF BF); > 0xFF so stored UTF-16
    // (coder 1).  Pairs with getLo80 (C2 80, first 2-byte) and getU0800 (E0 A0 80,
    // first 3-byte) to nail the 2-byte/3-byte encoder boundary from BOTH sides.
    public static String getU07FF       = new String(new char[]{ (char) 0x07FF });
    // U+10000 -> the SMALLEST astral code point (4-byte UTF-8 F0 90 80 80); the
    // surrogate-pair low edge (D800 DC00).  Pairs with getAstral (U+1F600, mid).
    public static String getAstralMin   = new String(Character.toChars(0x10000));
    // U+10FFFF -> the LARGEST valid Unicode code point (4-byte UTF-8 F4 8F BF BF);
    // the surrogate-pair high edge (DBFF DFFF).  Proves the surrogate-combining
    // arithmetic is exact at the very top of the code space.
    public static String getAstralMax   = new String(Character.toChars(0x10FFFF));
    // U+FFFD REPLACEMENT CHARACTER assigned as REAL content (not an error path):
    // a legitimate 3-byte BMP char (EF BF BD) that must round-trip verbatim, never
    // confused with read_java_string's own degrade-to-"" behaviour.
    public static String getReplacement = new String(new char[]{ (char) 0xFFFD });
    // Multi-script BMP: Latin 'A' + Greek alpha (U+03B1) + Hiragana 'i' (U+3044) +
    // Hangul 'ga' (U+AC00).  > 0xFF -> UTF-16 (coder 1); decodes to a mix of 1-,
    // 2- and 3-byte UTF-8 sequences in one string.  Bytes: 41 CE B1 E3 81 84 EA B0 80.
    public static String getMultiScript = makeMultiScript();
    // MODEST-long UTF-16 with REPEATED surrogate pairs: 100 copies of U+1F4A9
    // (PILE OF POO, 4-byte UTF-8 F0 9F 92 A9).  byte[] length 400 (200 UTF-16
    // units), decodes to 100 * 4 = 400 UTF-8 bytes.  Exercises the surrogate-pair
    // combining loop MANY times in one decode (heap-modest, 100 code points).
    public static String getEmojiRun    = makeEmojiRun(100);
    // INHERITED INSTANCE String with NON-ASCII (CJK) content, declared only on
    // FieldStringBase: proves the super-walk field resolution feeds the UTF-16
    // decode path (the existing inherited fields are pure ASCII / LATIN1 only).
    // Value 日本 (U+65E5 U+672C) -> 6 UTF-8 bytes E6 97 A5 E6 9C AC.  (Field lives
    // on the base; see inheritedCjk there.)

    // Java-published facts about the GET targets (native cross-checks these).
    public static volatile int     jAsciiLen;
    public static volatile int     jLatin1Len;
    public static volatile int     jLatin1Cp1;          // codePointAt(1) == 0xE9
    public static volatile int     jCjkLen;
    public static volatile int     jCjkCp0;             // codePointAt(0) == 0x65E5
    public static volatile boolean jNullIsNull;
    public static volatile int     jLen4096Len;
    public static volatile int     jLen4097Len;
    // ...for the exhaustive GET extras.
    public static volatile int     jEmojiCpCount;       // codePointCount == 3 (X,emoji,Y)
    public static volatile int     jEmojiCp1;            // codePointAt(1) == 0x1F600
    public static volatile int     jAstralCp0;          // codePointAt(0) == 0x1F600
    public static volatile int     jMaxBmpCp0;          // == 0xFFFF
    public static volatile int     jLo80Cp0;            // == 0x0080
    public static volatile int     jU0800Cp0;           // == 0x0800
    public static volatile int     jAll256Len;          // == 256
    public static volatile int     jAll256Cp255;        // == 0x00FF
    public static volatile String  jFinalConstValue;    // the folded-constant field, read in Java
    public static volatile String  jInheritedStrValue;  // inherited instance String, read in Java
    public static volatile String  jStaticInheritedValue; // inherited static String, read in Java
    public static volatile String  jInstGetOnlyValue;   // clean instance String, read in Java
    public static volatile String  jReassignAfterValue; // getReassign AFTER the probe reassigns it
    // ...for the batch-17 GET deepening targets.
    public static volatile int     jControlsLen;        // == 7
    public static volatile int     jControlsCp1;        // == 0x09 (TAB)
    public static volatile int     jCoderLatin1Cp7;     // codePointAt(7) == 0x00E9 (trailing é)
    public static volatile int     jCoderUtf16Cp7;      // codePointAt(7) == 0x1F4A9 (trailing astral)
    public static volatile int     jU07FFCp0;           // == 0x07FF
    public static volatile int     jAstralMinCp0;       // == 0x10000
    public static volatile int     jAstralMaxCp0;       // == 0x10FFFF
    public static volatile int     jReplacementCp0;     // == 0xFFFD
    public static volatile int     jMultiScriptLen;     // == 4 (chars)
    public static volatile int     jMultiScriptCp3;     // == 0xAC00 (Hangul ga)
    public static volatile int     jEmojiRunCpCount;    // == 100
    public static volatile int     jEmojiRunCp0;        // == 0x1F4A9
    public static volatile String  jInheritedCjkValue;  // inherited non-ASCII instance String

    // ================= SET targets (static) ================================
    // field_proxy::set(std::string) REBINDS the field to a freshly-built String
    // (library bug #30 fixed); it no longer overwrites the existing backing array
    // in place, so the written value lands EXACTLY (any length) and the previously
    // referenced String object is never mutated.  Targets are still built with
    // new String(char[]) so each starts with a PRIVATE backing — this also lets us
    // hold a SEPARATE reference to the original object (setShorterOriginal below)
    // and prove the rebind did NOT corrupt it.  The lengths no longer need to
    // match the test inputs (the rebind allocates the exact size it needs).
    public static String setAsciiEq    = freshAscii("AAAAA");           // write "world" -> "world"
    public static String setShorter    = freshAscii("world");           // write "hi"    -> "hi"
    public static String setEmptyTgt   = freshAscii("");                // write "ignored"-> "ignored"
    public static String setLatin1Tgt  = newLatin1Blank();              // write "abcde" -> "abcde"
    public static String setOverlong   = freshAscii("abc");             // write "LONGER"-> "LONGER"

    // A SEPARATE alias to setShorter's ORIGINAL String object, captured at class
    // init BEFORE any native write.  After the native set() rebinds setShorter to
    // a new "hi" String, this still points at the original object — which must STILL
    // read "world" (the rebind is an object-reference store, NOT an in-place mutate;
    // proving the old shared object is not corrupted).
    public static final String setShorterOriginal = setShorter;
    public static volatile boolean setShorterOriginalIntact;  // setShorterOriginal.equals("world")

    // Java-published facts AFTER the native writes (read through Java).
    public static volatile String  setAsciiEqValue;
    public static volatile int     setAsciiEqLen;
    public static volatile boolean setAsciiEqMatches;   // equals("world")
    public static volatile String  setShorterValue;
    public static volatile int     setShorterLen;       // == 2 ("hi") after the rebind
    public static volatile String  setEmptyTgtValue;    // == "ignored" after the rebind
    public static volatile int     setEmptyTgtLen;      // == 7
    public static volatile String  setLatin1TgtValue;
    public static volatile boolean setLatin1Matches;    // equals("abcde")
    public static volatile String  setOverlongValue;
    public static volatile int     setOverlongLen;      // == 6 ("LONGER") after the rebind

    // ----- SET-ENCODING round-trip targets ---------------------------------
    // The existing SET targets above all write PURE ASCII.  These exercise the
    // SET path's UTF-8 -> java.lang.String ENCODE for every non-ASCII class.
    // field_proxy::set(std::string) -> store_string() builds the new String via
    // the length-counted UTF-16 path (jni_new_string_utf16_local -> NewString),
    // which is content-exact: interior NULs are kept (counted length, NOT a C
    // string), astral scalars become proper surrogate PAIRS, and the JVM itself
    // picks the LATIN1/UTF16 coder.  Each target starts as a private-backing
    // placeholder; the module writes a known UTF-8 string into it, and the probe
    // republishes Java's view (value / length / code points) so the native side
    // proves the encode is visible to Java itself (the symmetric partner of the
    // GET decode battery).  All start values are built via freshAscii / char[]
    // so they own a PRIVATE backing (the shared-literal landmine never applies).
    // A Latin-1 source (U+00E9 'é') -> Java length 1, codePointAt(0)==0x00E9.
    public static String setLatin1Write  = freshAscii("__lat__");
    public static volatile String  setLatin1WriteValue;
    public static volatile int     setLatin1WriteLen;     // == 1
    public static volatile int     setLatin1WriteCp0;     // == 0x00E9
    // A BMP CJK source (U+65E5 日) -> Java length 1, codePointAt(0)==0x65E5.
    public static String setCjkWrite     = freshAscii("__cjk__");
    public static volatile String  setCjkWriteValue;
    public static volatile int     setCjkWriteLen;        // == 1
    public static volatile int     setCjkWriteCp0;        // == 0x65E5
    // An ASTRAL source (U+1F600) -> Java length 2 (surrogate PAIR!), codePoint
    // COUNT 1, codePointAt(0)==0x1F600.  Proves UTF-8 -> surrogate-pair ENCODE.
    public static String setAstralWrite  = freshAscii("__ast__");
    public static volatile String  setAstralWriteValue;
    public static volatile int     setAstralWriteLen;     // == 2 (two UTF-16 units)
    public static volatile int     setAstralWriteCpCount; // == 1
    public static volatile int     setAstralWriteCp0;     // == 0x1F600
    // An interior-NUL source ("a\0b") -> Java length 3, charAt(1)==0.  Proves
    // the write is length-counted (NewStringUTF would truncate at the NUL).
    public static String setNulWrite     = freshAscii("__nul__");
    public static volatile String  setNulWriteValue;
    public static volatile int     setNulWriteLen;        // == 3
    public static volatile int     setNulWriteCp1;        // == 0 (interior NUL)
    // An EMPTY write into a populated target -> a real empty String (length 0),
    // NOT null and NOT left at the old value.
    public static String setEmptyWrite   = freshAscii("populated");
    public static volatile String  setEmptyWriteValue;
    public static volatile int     setEmptyWriteLen;      // == 0
    public static volatile boolean setEmptyWriteIsNull;   // == false (real "" String)
    public static volatile boolean setEmptyWriteEqualsEmpty; // "".equals(...)
    // Written through field_proxy::set(const char*) — the std::string_view
    // CONVERTIBILITY arm of set(), distinct from the std::string arm above.
    public static String setViaCharPtr   = freshAscii("__ptr__");
    public static volatile boolean setViaCharPtrMatches;  // equals("char-ptr-set")
    // Written through field_proxy::set(std::string_view) — same convertibility arm.
    public static String setViaStringView = freshAscii("__sv__");
    public static volatile boolean setViaStringViewMatches; // equals("sv-set")
    // RE-SET: the module writes "first" then "second" into the SAME field (a
    // second rebind over an already-rebound field).  Java must see "second".
    public static String setReSet        = freshAscii("__pre__");
    public static volatile String  setReSetValue;
    public static volatile int     setReSetLen;           // == 6 ("second")

    // ================= SET target (instance) ===============================
    // Instance String field, mutated through an instance field_proxy.
    // Fresh char[] backing (see SET-target note above) so the write is isolated.
    public String  instAscii = freshAscii("QQQQQ");                      // len 5, write "java!"
    public static volatile String  instAsciiValue;
    public static volatile boolean instAsciiMatches;    // equals("java!")

    // Instance String field written with a NON-ASCII (CJK) value through an
    // instance field_proxy: proves the SET encode path works on an INSTANCE slot
    // too (not just statics).  Source UTF-8 is U+65E5 日 -> Java length 1, cp 0x65E5.
    public String  instCjk = freshAscii("__icjk__");
    public static volatile String  instCjkValue;
    public static volatile int     instCjkLen;          // == 1
    public static volatile int     instCjkCp0;          // == 0x65E5

    // ================= GET target (instance, NEVER written) ================
    // A clean instance String the module READS (does not mutate), proving the
    // String getter decodes an INSTANCE field's compressed OOP through an
    // instance field_proxy.  A plain literal here is fine: nothing writes it, so
    // the shared-backing landmine that applies to SET targets does not apply.
    public String  instGetOnly = "instance-get";

    // A live instance the native side wraps (to reach the instance field).
    public static volatile FieldString self;

    // ================= negative / guard targets ============================
    // A primitive int field.  Native attempts field_proxy::set(std::string) on
    // it; the type guard must REFUSE the write and leave this unchanged.
    public static int notAStringInt = 12345;
    // A NON-String object reference field (java.lang.Object holding an int[]).
    // Native reads its compressed OOP and feeds it to read_java_string, which
    // resolves java/lang/String's `value` offset and treats the bytes there as a
    // backing-array OOP.  For a non-String object that slot is NOT a valid String
    // backing array, so read_java_string must DEGRADE GRACEFULLY (return "" or a
    // bounded best-effort decode) and NEVER crash.  The native side asserts only
    // no-crash + a bounded (short) result via [INFO] — exact bytes are undefined.
    public static Object notAStringObj = new int[]{ 1, 2, 3, 4 };
    // A String the native side reads but never writes; used to prove that
    // reading an interned literal does not corrupt the shared pool.
    public static volatile boolean internedStillIntact;

    // Hookable instance method (mirrors pilot.touch) so a real interpreter hook
    // fires on bytecode dispatch.  Returns the live length of instAscii.
    public int touchString(final int delta)
    {
        return this.instAscii.length() + delta;
    }
    public static volatile int observed;

    // ---------------------- helper constructors ----------------------------
    private static String makeLatin1()
    {
        final char[] c = { 'h', (char) 0x00E9, 'l', 'l', 'o', ' ',
                           (char) 0x00E8, (char) 0x00E9, (char) 0x00FF };
        return new String(c);
    }

    private static String makeEmbeddedNul()
    {
        final char[] c = { 'a', (char) 0x0000, 'b', (char) 0x0000, 'c' };
        return new String(c);
    }

    private static String newLatin1Blank()
    {
        // Five Latin-1 'AA' so coder is 0 and backing length is 5 bytes.
        final char[] c = { 'A', 'A', 'A', 'A', 'A' };
        return new String(c);
    }

    private static String makeControls()
    {
        // a TAB b LF c CR d  -- all ASCII control/print bytes, LATIN1 coder 0.
        final char[] c = { 'a', (char) 0x0009, 'b', (char) 0x000A,
                           'c', (char) 0x000D, 'd' };
        return new String(c);
    }

    private static String makeCoderLatin1Prefix()
    {
        // "Shared!" + 'é' -- all code points <= 0xFF -> LATIN1 (coder 0).  The
        // first 7 chars are the shared ASCII prefix.
        final char[] c = { 'S', 'h', 'a', 'r', 'e', 'd', '!', (char) 0x00E9 };
        return new String(c);
    }

    private static String makeCoderUtf16Prefix()
    {
        // "Shared!" + U+1F4A9 -- the trailing astral scalar forces the WHOLE
        // String to UTF-16 (coder 1), but the shared ASCII prefix must still
        // decode to the IDENTICAL 7 UTF-8 bytes as the LATIN1 sibling above.
        final StringBuilder b = new StringBuilder();
        b.append("Shared!");
        b.appendCodePoint(0x1F4A9);
        return b.toString();
    }

    /**
     * Builds 'A' + Greek alpha (U+03B1) + Hiragana 'i' (U+3044) + Hangul 'ga'
     * (U+AC00).  Contains code points &gt; 0xFF so the String stores UTF-16
     * (coder 1) and decodes to a mix of 1-, 2- and 3-byte UTF-8 sequences.
     */
    private static String makeMultiScript()
    {
        final char[] c = { 'A', (char) 0x03B1, (char) 0x3044, (char) 0xAC00 };
        return new String(c);
    }

    /**
     * Builds {@code count} copies of U+1F4A9 (a 4-byte astral scalar carried as
     * a UTF-16 surrogate pair), to exercise the surrogate-pair combining loop
     * many times in one decode.  Heap-modest: count is small (100).
     */
    private static String makeEmojiRun(final int count)
    {
        final StringBuilder b = new StringBuilder();
        for (int i = 0; i < count; i++)
        {
            b.appendCodePoint(0x1F4A9);
        }
        return b.toString();
    }

    /**
     * Builds "X" + U+1F600 + "Y" from code points (no raw-emoji source literal),
     * so the astral scalar is carried as a UTF-16 surrogate pair flanked by ASCII.
     */
    private static String makeEmoji()
    {
        final StringBuilder b = new StringBuilder();
        b.append('X');
        b.appendCodePoint(0x1F600);
        b.append('Y');
        return b.toString();
    }

    /**
     * Builds a String of every char value 0x0000..0x00FF (256 chars).  All code
     * points are <= 0xFF so the String stores as LATIN1 (coder 0); read_java_string
     * UTF-8-encodes it to 384 bytes.
     */
    private static String makeAll256()
    {
        final char[] c = new char[256];
        for (int i = 0; i < 256; i++)
        {
            c[i] = (char) i;
        }
        return new String(c);
    }

    /**
     * Builds a String from the chars of {@code text} via new String(char[]),
     * guaranteeing a PRIVATE backing array (never shared with an interned
     * literal).  Used for SET targets so each starts with a private object that
     * can be aliased and checked for non-corruption after the field is rebound.
     */
    private static String freshAscii(final String text)
    {
        return new String(text.toCharArray());
    }

    private static String repeat(final char ch, final int count)
    {
        final char[] c = new char[count];
        for (int i = 0; i < count; i++)
        {
            c[i] = ch;
        }
        return new String(c);
    }

    static
    {
        self = new FieldString();

        Harness.register(new Harness.Probe()
        {
            @Override
            public boolean pending()
            {
                return FieldString.go && !FieldString.done;
            }

            @Override
            public void run()
            {
                // Fire the interpreter hook through a real bytecode dispatch.
                FieldString.observed = FieldString.self.touchString(100);

                // --- publish Java-observed facts about the GET targets ------
                jAsciiLen  = getAscii.length();
                jLatin1Len = getLatin1.length();
                jLatin1Cp1 = getLatin1.codePointAt(1);
                jCjkLen    = getCjk.length();
                jCjkCp0    = getCjk.codePointAt(0);
                jNullIsNull = (getNull == null);
                jLen4096Len = getLen4096.length();
                jLen4097Len = getLen4097.length();

                // --- Java-observed facts about the exhaustive GET extras ----
                jEmojiCpCount = getEmoji.codePointCount(0, getEmoji.length());
                jEmojiCp1     = getEmoji.codePointAt(1);     // the astral cp (after 'X')
                jAstralCp0    = getAstral.codePointAt(0);
                jMaxBmpCp0    = getMaxBmp.codePointAt(0);
                jLo80Cp0      = getLo80.codePointAt(0);
                jU0800Cp0     = getU0800.codePointAt(0);
                jAll256Len    = getAll256.length();
                jAll256Cp255  = getAll256.codePointAt(255);
                jFinalConstValue      = getFinalConst;
                jInheritedStrValue    = self.inheritedStr;       // inherited instance String
                jStaticInheritedValue = FieldStringBase.sInheritedStr;
                jInstGetOnlyValue     = self.instGetOnly;        // clean instance String

                // --- Java-observed facts about the batch-17 GET deepening ----
                jControlsLen     = getControls.length();          // 7
                jControlsCp1     = getControls.codePointAt(1);    // 0x09 (TAB)
                jCoderLatin1Cp7  = getCoderLatin1Prefix.codePointAt(7); // 0x00E9
                jCoderUtf16Cp7   = getCoderUtf16Prefix.codePointAt(7);  // 0x1F4A9
                jU07FFCp0        = getU07FF.codePointAt(0);       // 0x07FF
                jAstralMinCp0    = getAstralMin.codePointAt(0);   // 0x10000
                jAstralMaxCp0    = getAstralMax.codePointAt(0);   // 0x10FFFF
                jReplacementCp0  = getReplacement.codePointAt(0); // 0xFFFD
                jMultiScriptLen  = getMultiScript.length();       // 4
                jMultiScriptCp3  = getMultiScript.codePointAt(3); // 0xAC00
                jEmojiRunCpCount = getEmojiRun.codePointCount(0, getEmojiRun.length()); // 100
                jEmojiRunCp0     = getEmojiRun.codePointAt(0);    // 0x1F4A9
                jInheritedCjkValue = self.inheritedCjk;           // inherited non-ASCII

                // --- reassign getReassign to a NEW backing String.  The module
                //     read it as "before" BEFORE the probe; a fresh field_proxy
                //     read AFTER the probe must see this new value.  Built via
                //     freshAscii so it owns a private backing (no shared-literal
                //     surprises) and is a different object than the original.
                getReassign = freshAscii("after2");
                jReassignAfterValue = getReassign;          // Java's view of the new value

                // --- read the post-write SET targets THROUGH JAVA -----------
                setAsciiEqValue   = setAsciiEq;
                setAsciiEqLen     = setAsciiEq.length();
                setAsciiEqMatches = "world".equals(setAsciiEq);

                setShorterValue = setShorter;            // "hi" after the rebind
                setShorterLen   = setShorter.length();    // 2

                // The original String object setShorter pointed at is untouched by
                // the rebind (object-reference store, not in-place mutate).
                setShorterOriginalIntact = "world".equals(setShorterOriginal);

                setEmptyTgtValue = setEmptyTgt;           // "ignored" after the rebind
                setEmptyTgtLen   = setEmptyTgt.length();  // 7

                setLatin1TgtValue = setLatin1Tgt;
                setLatin1Matches  = "abcde".equals(setLatin1Tgt);

                setOverlongValue = setOverlong;           // "LONGER" after the rebind
                setOverlongLen   = setOverlong.length();  // 6

                instAsciiValue   = self.instAscii;
                instAsciiMatches = "java!".equals(self.instAscii);

                // --- SET-ENCODING round-trip readbacks (the native side wrote a
                //     known UTF-8 string into each; Java republishes its view so
                //     the encode path is proven visible to the JVM itself). ----
                setLatin1WriteValue = setLatin1Write;
                setLatin1WriteLen   = setLatin1Write.length();           // 1
                setLatin1WriteCp0   = setLatin1Write.isEmpty() ? -1 : setLatin1Write.codePointAt(0); // 0xE9

                setCjkWriteValue = setCjkWrite;
                setCjkWriteLen   = setCjkWrite.length();                 // 1
                setCjkWriteCp0   = setCjkWrite.isEmpty() ? -1 : setCjkWrite.codePointAt(0); // 0x65E5

                setAstralWriteValue   = setAstralWrite;
                setAstralWriteLen     = setAstralWrite.length();         // 2 (surrogate pair)
                setAstralWriteCpCount = setAstralWrite.codePointCount(0, setAstralWrite.length()); // 1
                setAstralWriteCp0     = setAstralWrite.isEmpty() ? -1 : setAstralWrite.codePointAt(0); // 0x1F600

                setNulWriteValue = setNulWrite;
                setNulWriteLen   = setNulWrite.length();                 // 3
                setNulWriteCp1   = setNulWrite.length() >= 2 ? setNulWrite.codePointAt(1) : -1; // 0

                setEmptyWriteValue       = setEmptyWrite;
                setEmptyWriteLen         = (setEmptyWrite == null) ? -1 : setEmptyWrite.length(); // 0
                setEmptyWriteIsNull      = (setEmptyWrite == null);      // false
                setEmptyWriteEqualsEmpty = "".equals(setEmptyWrite);     // true

                setViaCharPtrMatches    = "char-ptr-set".equals(setViaCharPtr);
                setViaStringViewMatches = "sv-set".equals(setViaStringView);

                setReSetValue = setReSet;                                // "second"
                setReSetLen   = setReSet.length();                       // 6

                instCjkValue = self.instCjk;
                instCjkLen   = self.instCjk.length();                    // 1
                instCjkCp0   = self.instCjk.isEmpty() ? -1 : self.instCjk.codePointAt(0); // 0x65E5

                // The shared literal must be untouched by native reads.
                internedStillIntact = "INTERNED_LITERAL".equals(getInterned)
                        && ("INTERNED_LITERAL" == "INTERNED_LITERAL".intern());

                FieldString.done = true;
            }
        });
    }
}

package vmhook.fixtures;

import vmhook.Harness;

/**
 * Fixture for the {@code method_call_string} feature: it exercises
 * {@code method_proxy::call()} taking and returning java.lang.String across every
 * value category the two decoders can hit -- regular ASCII, empty, single char,
 * whitespace/control, embedded NUL (interior / leading / trailing / only),
 * BMP unicode (Latin-1 + multi-byte + the 1/2- and 2/3-byte boundaries), CJK,
 * Greek, supplementary-plane scalars (one, two, and one spliced between ASCII),
 * the 4096-char backing-array cap, long (300+ char) ASCII, a true {@code null}
 * return, dynamically-built (non-interned) strings, instance vs static dispatch,
 * and a battery of String-argument round-trips (echo each shape back, concat
 * two, and length/charAt-driven results) -- instance AND static.
 *
 * <p>The native module installs a hook on {@link #trigger()}.  The probe calls
 * {@code trigger()} on a real bytecode dispatch (which fires the interpreter
 * hook); inside that detour the native side drives every String method below
 * through {@code method_proxy::call(...).as_string()} and records the decoded
 * {@code std::string}s.  STATIC methods are driven from the native side through
 * the wrapper's {@code static_method(...)} accessor (a null-receiver proxy ->
 * the static JNI call path), NEVER through an instance proxy -- invoking a static
 * method via the instance path mis-binds the receiver as the first declared
 * argument and corrupts the call.</p>
 *
 * <p>EVERY non-ASCII constant is written with backslash-u escapes so the source
 * is pure ASCII and javac decodes it byte-identically under javac 8..26
 * regardless of the build host's file encoding (Windows Cp1252 / Linux+macOS
 * UTF-8) with no {@code -encoding} flag.  Interior NULs are escapes, never raw
 * NUL bytes.  Java 8 syntax only: no var / records / switch-expressions / text
 * blocks / List.of; only java.* + vmhook.Harness.</p>
 */
public final class MethodString
{
    /** Native sets this true to request the action; clears it after. */
    public static volatile boolean go;

    /** The action sets this true when it has run; native polls it. */
    public static volatile boolean done;

    /** Bumped every time {@link #trigger()} actually runs (hook liveness). */
    public static volatile int triggerCount;

    // ---- Java-published cross-check witnesses (populated in the probe AFTER
    //      trigger() runs, on the Java thread) -------------------------------
    // The native module reads these after run_probe to (a) label which physical
    // backing-array coder each returned String used -- LATIN1 (0) vs UTF16 (1) on
    // JDK 9+, or -1 on JDK 8 where there is no `coder` field -- so the test
    // documents that the ASCII/Latin-1 returns are coder 0 and the >0xFF returns
    // are coder 1, and (b) confirm Java's own length for the very-large string.
    public static volatile boolean jHasCoderField;  // true on JDK 9+, false on 8
    public static volatile int     jCoderRegular;    // ASCII "hello world" -> 0 / -1
    public static volatile int     jCoderCafe;       // Latin-1 (U+00E9)    -> 0 / -1
    public static volatile int     jCoderCjk;        // CJK (>0xFF)         -> 1 / -1
    public static volatile int     jCoderEmoji;      // astral (UTF-16)     -> 1 / -1
    public static volatile int     jCoderMaxBmp;     // U+FFFF (>0xFF)      -> 1 / -1
    public static volatile int     jBigLen;          // bigString(70000).length() == 70000

    // ---- Canonical unicode constants (ASCII-safe escaped source) -----------
    // Written purely with backslash-u escapes so javac decodes them identically
    // on every CI host (Windows cmd / Linux / macOS) no matter the file
    // encoding.  The C++ module hard-codes the matching modified-UTF-8 (call_jni)
    // byte expectations and the standard-UTF-8 (call_stub) ones.

    /** "caf" + e-acute (U+00E9). */
    public static final String CAFE = "caf\u00e9";

    /** "r" e-acute "sum" e-acute  (U+00E9 twice). */
    public static final String RESUME = "r\u00e9sum\u00e9";

    /** Latin-1 high run: e-acute e-grave e-circ e-uml. */
    public static final String ACCENTS = "\u00e9\u00e8\u00ea\u00eb";

    /** u-umlaut / n-tilde / euro sign (mix of 2- and 3-byte). */
    public static final String MIXED = "\u00fc\u00f1\u20ac";

    /** CJK: nihongo (U+65E5 U+672C U+8A9E). */
    public static final String CJK = "\u65e5\u672c\u8a9e";

    /** Greek alpha beta (U+03B1 U+03B2). */
    public static final String GREEK = "\u03b1\u03b2";

    /**
     * Supplementary-plane grinning face U+1F600, written as its explicit UTF-16
     * surrogate pair (high U+D83D, low U+DE00) so the source is pure ASCII.
     */
    public static final String EMOJI = "\ud83d\ude00";

    /** Interior NUL: 'a' U+0000 'b' -- must NOT truncate on the JNI path. */
    public static final String INTERIOR_NUL = "a\u0000b";

    /** A SINGLE interior NUL char (U+0000) and nothing else (Java length 1). */
    public static final String NUL_ONLY = "\u0000";

    /** NUL at the START: U+0000 then "tail" (Java length 5). */
    public static final String LEADING_NUL = "\u0000tail";

    /** NUL at the END: "head" then U+0000 (Java length 5). */
    public static final String TRAILING_NUL = "head\u0000";

    /** Latin-1 ceiling char U+00FF (last 2-byte-in-modified-UTF-8 code point). */
    public static final String LATIN1_HI = "\u00ff";

    /** The 2-byte/3-byte UTF-8 boundary: U+07FF (2 bytes) then U+0800 (3 bytes). */
    public static final String BMP_BOUNDARY = "\u07ff\u0800";

    /** The ASCII/2-byte boundary: U+007F (1 byte) then U+0080 (2 bytes). */
    public static final String ASCII_BOUNDARY = "\u007f\u0080";

    /**
     * ASCII with a supplementary-plane emoji spliced in the MIDDLE:
     * "ab" + U+1F600 (D83D DE00) + "cd".  Proves astral handling without
     * leading/trailing ambiguity, and that the surrounding ASCII survives intact
     * on both decode paths.
     */
    public static final String ASTRAL_MID = "ab\ud83d\ude00cd";

    /**
     * TWO supplementary-plane scalars back to back: U+1F600 (grinning, D83D DE00)
     * then U+1F680 (rocket, D83D DE80) -- two surrogate pairs, four code units.
     */
    public static final String TWO_ASTRAL = "\ud83d\ude00\ud83d\ude80";

    /** The highest BMP code point U+FFFF -> 3-byte EF BF BF on BOTH decode paths. */
    public static final String MAX_BMP = "\uffff";

    /** The Unicode replacement character U+FFFD -> 3-byte EF BF BD on both paths. */
    public static final String REPLACEMENT = "\ufffd";

    /**
     * A LONE high surrogate U+D83D with no trailing low surrogate.  A Java String
     * is just a UTF-16 code-unit sequence, so this is a legal (if ill-formed)
     * String.  BOTH vmhook decoders emit the 3-byte CESU encoding of the surrogate
     * code unit (ED A0 BD) -- they do NOT substitute '?' the way
     * String.getBytes(UTF_8) does.  (Cross-checked: writeUTF and read_java_string
     * both yield ED A0 BD.)
     */
    public static final String LONE_HIGH_SURROGATE = "\ud83d";

    /** A LONE low surrogate U+DE00 -> 3-byte CESU ED B8 80 on both paths. */
    public static final String LONE_LOW_SURROGATE = "\ude00";

    /**
     * A REVERSED surrogate pair: low U+DE00 THEN high U+D83D.  Must NOT be combined
     * into an astral scalar (order is wrong), so each is decoded independently to
     * its own 3-byte CESU sequence: ED B8 80 ED A0 BD (6 bytes) on both paths.
     */
    public static final String REVERSED_SURROGATES = "\ude00\ud83d";

    /**
     * Non-breaking space U+00A0 (2-byte) + line separator U+2028 (3-byte) +
     * paragraph separator U+2029 (3-byte): the Unicode-whitespace characters that
     * routinely break JSON/JS string handling.  BMP, so path-independent
     * (C2 A0 E2 80 A8 E2 80 A9, 8 bytes).
     */
    public static final String UNICODE_WS = "\u00a0\u2028\u2029";

    // ---- A field whose String value an accessor method returns ------------
    private String instanceField = "instance-field-value";

    // =======================================================================
    //  Instance String-returning methods
    // =======================================================================

    /** Plain ASCII constant. */
    public String regular()
    {
        return "hello world";
    }

    /** The empty string (length 0) -- distinct from null. */
    public String empty()
    {
        return "";
    }

    /** A single ASCII character. */
    public String single()
    {
        return "X";
    }

    /** Whitespace + control characters (space tab newline cr space). */
    public String whitespace()
    {
        return " \t\n\r ";
    }

    /** Punctuation chars that often trip encoders / JSON serializers. */
    public String punctuation()
    {
        return "\"\\/{}[]:,";
    }

    /** A high pure-ASCII run near the 0x7E boundary. */
    public String asciiHigh()
    {
        return "~}|{`_^]";
    }

    /** Every printable ASCII byte 0x20..0x7E in order (95 chars). */
    public String allAscii()
    {
        StringBuilder sb = new StringBuilder(95);
        for (int c = 0x20; c <= 0x7E; c++)
        {
            sb.append((char) c);
        }
        return sb.toString();
    }

    /** Returns Java {@code null} -- the null-vs-empty boundary. */
    public String returnNull()
    {
        return null;
    }

    /** caf + e-acute (Latin-1 high char). */
    public String cafe()
    {
        return CAFE;
    }

    /** resume with two Latin-1 high chars. */
    public String resume()
    {
        return RESUME;
    }

    /** Four accented Latin-1 high chars. */
    public String accents()
    {
        return ACCENTS;
    }

    /** u-uml / n-tilde / euro (2- and 3-byte multibyte mix). */
    public String mixed()
    {
        return MIXED;
    }

    /** CJK three-character string. */
    public String cjk()
    {
        return CJK;
    }

    /** Greek two-character string. */
    public String greek()
    {
        return GREEK;
    }

    /** Latin-1 ceiling U+00FF. */
    public String latin1Hi()
    {
        return LATIN1_HI;
    }

    /** U+07FF (2-byte) then U+0800 (3-byte) -- the 2/3-byte UTF-8 boundary. */
    public String bmpBoundary()
    {
        return BMP_BOUNDARY;
    }

    /** U+007F (1-byte) then U+0080 (2-byte) -- the ASCII/2-byte boundary. */
    public String asciiBoundary()
    {
        return ASCII_BOUNDARY;
    }

    /** Supplementary-plane emoji (surrogate pair / 4-byte scalar). */
    public String emoji()
    {
        return EMOJI;
    }

    /** "ab" + emoji + "cd": astral scalar spliced between ASCII. */
    public String astralMid()
    {
        return ASTRAL_MID;
    }

    /** Two supplementary-plane scalars back to back. */
    public String twoAstral()
    {
        return TWO_ASTRAL;
    }

    /** The highest BMP code point U+FFFF (3-byte EF BF BF). */
    public String maxBmp()
    {
        return MAX_BMP;
    }

    /** The Unicode replacement char U+FFFD (3-byte EF BF BD). */
    public String replacementChar()
    {
        return REPLACEMENT;
    }

    /** A lone high surrogate U+D83D (3-byte CESU ED A0 BD on both paths). */
    public String loneHighSurrogate()
    {
        return LONE_HIGH_SURROGATE;
    }

    /** A lone low surrogate U+DE00 (3-byte CESU ED B8 80 on both paths). */
    public String loneLowSurrogate()
    {
        return LONE_LOW_SURROGATE;
    }

    /** Reversed surrogate pair (low then high) -- must NOT combine. */
    public String reversedSurrogates()
    {
        return REVERSED_SURROGATES;
    }

    /** U+00A0 / U+2028 / U+2029 -- Unicode whitespace (8 bytes UTF-8). */
    public String unicodeWhitespace()
    {
        return UNICODE_WS;
    }

    /**
     * Every C0 control character U+0001..U+001F followed by U+007F (DEL): 32 chars,
     * all in the ASCII byte range so every byte survives 1:1 on both decode paths.
     * (U+0000 is covered separately by the NUL-family methods.)
     */
    public String controlChars()
    {
        StringBuilder sb = new StringBuilder(32);
        for (int c = 0x01; c <= 0x1F; c++)
        {
            sb.append((char) c);
        }
        sb.append((char) 0x7F);
        return sb.toString();
    }

    /**
     * A String of exactly {@code n} 'A' characters with NO upper clamp (other than
     * a generous 1,048,576 safety ceiling), so the native side can request a
     * String FAR above the 4096-char {@code read_java_string} backing-array cap and
     * above 65,536 code units.  On the call_jni path GetStringUTFChars returns the
     * whole thing (no cap); on the call_stub path read_java_string rejects any
     * length &gt; 4096 and returns "".  Lets the native side prove the >65536 case
     * on whichever decoder is live.
     */
    public String bigString(int n)
    {
        int count = n;
        if (count < 0)
        {
            count = 0;
        }
        if (count > 1048576)
        {
            count = 1048576;
        }
        StringBuilder sb = new StringBuilder(count);
        for (int i = 0; i < count; i++)
        {
            sb.append('A');
        }
        return sb.toString();
    }

    /** Interior-NUL string ('a' U+0000 'b'). */
    public String interiorNul()
    {
        return INTERIOR_NUL;
    }

    /** A single U+0000 char (Java length 1). */
    public String nulOnly()
    {
        return NUL_ONLY;
    }

    /** U+0000 followed by "tail". */
    public String leadingNul()
    {
        return LEADING_NUL;
    }

    /** "head" followed by U+0000. */
    public String trailingNul()
    {
        return TRAILING_NUL;
    }

    /** Returns the value of a String instance field (heap OOP, not a literal). */
    public String fieldValue()
    {
        return this.instanceField;
    }

    /**
     * Dynamically built (NOT interned) string so the returned OOP is a fresh
     * heap object, never a constant-pool entry -- exercises the decode of a
     * non-cached String.  Result is always "dyn-42".
     */
    public String dynamic()
    {
        StringBuilder sb = new StringBuilder();
        sb.append("dyn");
        sb.append('-');
        sb.append(6 * 7);
        return sb.toString();
    }

    /** Substring of a longer string -- yields a fresh String on modern JDKs. */
    public String substringResult()
    {
        return "0123456789abcdef".substring(4, 10); // "456789"
    }

    /** A 300-char ASCII string (the "long" case) built from a repeating run. */
    public String longAscii()
    {
        StringBuilder sb = new StringBuilder(300);
        for (int i = 0; i < 300; i++)
        {
            sb.append((char) ('a' + (i % 26)));
        }
        return sb.toString();
    }

    /** Echoes a native-supplied String argument back (round-trip identity). */
    public String echo(String value)
    {
        return value;
    }

    /** Concatenates two native-supplied String arguments. */
    public String concat(String a, String b)
    {
        return a + b;
    }

    /** length-driven result: returns "len=" + the argument's Java char length. */
    public String lengthOf(String value)
    {
        if (value == null)
        {
            return "len=null";
        }
        return "len=" + value.length();
    }

    /** charAt-driven result: returns the single char at index i as a String. */
    public String charAtOf(String value, int i)
    {
        return String.valueOf(value.charAt(i));
    }

    /**
     * Returns a String whose length is exactly {@code n} (clamped to 0..8192),
     * filled with 'A'.  Lets the native side probe the 4096-char backing-array
     * cap that {@code read_java_string} enforces and that {@code make_java_string}
     * truncates to.
     */
    public String repeatA(int n)
    {
        int count = n;
        if (count < 0)
        {
            count = 0;
        }
        if (count > 8192)
        {
            count = 8192;
        }
        StringBuilder sb = new StringBuilder(count);
        for (int i = 0; i < count; i++)
        {
            sb.append('A');
        }
        return sb.toString();
    }

    // =======================================================================
    //  Static String-returning methods (exercise the FindClass/pool_holder
    //  static dispatch branch instead of GetObjectClass).  These MUST be driven
    //  from the native side through static_method(...) (a null-receiver proxy),
    //  never through an instance proxy.
    // =======================================================================

    public static String staticRegular()
    {
        return "static-hello";
    }

    public static String staticEmpty()
    {
        return "";
    }

    public static String staticNull()
    {
        return null;
    }

    public static String staticUnicode()
    {
        return CAFE;
    }

    public static String staticCjk()
    {
        return CJK;
    }

    public static String staticEmoji()
    {
        return EMOJI;
    }

    public static String staticInteriorNul()
    {
        return INTERIOR_NUL;
    }

    /** Static String-arg echo (round-trip through the static call path). */
    public static String staticEcho(String value)
    {
        return value;
    }

    /** Static two-String-arg concat (round-trip through the static call path). */
    public static String staticConcat(String a, String b)
    {
        return a + b;
    }

    /** Always returns the literal "static-dyn-99" via a builder (non-interned). */
    public static String staticDynamic()
    {
        StringBuilder sb = new StringBuilder();
        sb.append("static-dyn-");
        sb.append(99);
        return sb.toString();
    }

    /**
     * Static counterpart of {@link #bigString(int)}: {@code n} 'A's (clamped to
     * 0..1,048,576) returned through the STATIC call path, so the &gt;65536 case is
     * proven for static dispatch too.
     */
    public static String staticBigString(int n)
    {
        int count = n;
        if (count < 0)
        {
            count = 0;
        }
        if (count > 1048576)
        {
            count = 1048576;
        }
        StringBuilder sb = new StringBuilder(count);
        for (int i = 0; i < count; i++)
        {
            sb.append('A');
        }
        return sb.toString();
    }

    /** Static U+FFFF (max BMP) through the static call path. */
    public static String staticMaxBmp()
    {
        return MAX_BMP;
    }

    /** Static lone high surrogate U+D83D through the static call path. */
    public static String staticLoneSurrogate()
    {
        return LONE_HIGH_SURROGATE;
    }

    /**
     * Reflective {@code String.coder} probe used only to label which physical
     * backing-array coder each returned String used.  Returns -1 when the field is
     * absent (JDK 8, where String.value is a char[] with no coder) or reflection is
     * denied -- the native side only asserts the coder when it is &gt;= 0, so this is
     * a diagnostic, never a hard dependency.
     */
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

    // =======================================================================
    //  Hook trigger -- the native module hooks this no-arg instance method.
    //  Calling it on a real bytecode dispatch fires the interpreter hook, and
    //  the native detour does all the call()-returns-String work from inside
    //  the hook (where current_java_thread is established).
    // =======================================================================

    public void trigger()
    {
        triggerCount++;
    }

    static
    {
        Harness.register(new Harness.Probe()
        {
            public boolean pending()
            {
                return MethodString.go && !MethodString.done;
            }

            public void run()
            {
                MethodString instance = new MethodString();
                // Real bytecode dispatch -> the native hook on trigger() fires,
                // and the detour exercises every String method on this very
                // instance (and the static methods via static_method()).
                instance.trigger();

                // Publish the cross-check witnesses AFTER the detour has captured
                // every observation (the native side reads these once run_probe
                // returns).  coderOf() is -1 on JDK 8 (no coder field), so the
                // native side only asserts a coder it could actually read.
                boolean hasCoder;
                try
                {
                    String.class.getDeclaredField("coder");
                    hasCoder = true;
                }
                catch (final Throwable t)
                {
                    hasCoder = false;
                }
                MethodString.jHasCoderField = hasCoder;
                MethodString.jCoderRegular = coderOf(instance.regular());
                MethodString.jCoderCafe    = coderOf(CAFE);
                MethodString.jCoderCjk      = coderOf(CJK);
                MethodString.jCoderEmoji    = coderOf(EMOJI);
                MethodString.jCoderMaxBmp   = coderOf(MAX_BMP);
                MethodString.jBigLen        = instance.bigString(70000).length();

                MethodString.done = true;
            }
        });
    }
}

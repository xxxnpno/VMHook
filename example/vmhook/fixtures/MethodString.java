package vmhook.fixtures;

import vmhook.Harness;

/**
 * Fixture for the {@code method_call_string} feature: it exercises
 * {@code method_proxy::call()} returning {@code java.lang.String} across every
 * value category the decoder can hit -- regular ASCII, empty, single char,
 * whitespace/control, embedded NUL, BMP unicode (Latin-1 + multi-byte), CJK,
 * a supplementary-plane emoji (surrogate pair), the 4096-char backing-array
 * cap, a true {@code null} return, dynamically-built (non-interned) strings,
 * round-trip echo of a native-supplied argument, and instance vs static
 * dispatch.
 *
 * <p>The native module installs a hook on {@link #trigger()}.  The probe calls
 * {@code trigger()} on a real bytecode dispatch (which fires the interpreter
 * hook); inside that detour the native side drives every String-returning
 * method below through {@code method_proxy::call(...).as_string()} and records
 * the decoded {@code std::string}s.  Every unicode constant is written with
 * {@code \\uXXXX} escapes so the source is pure ASCII and compiles identically
 * under javac 8..25 regardless of the build host's file encoding.</p>
 */
public final class MethodString
{
    /** Native sets this true to request the action; clears it after. */
    public static volatile boolean go;

    /** The action sets this true when it has run; native polls it. */
    public static volatile boolean done;

    /** Bumped every time {@link #trigger()} actually runs (hook liveness). */
    public static volatile int triggerCount;

    // ---- Canonical unicode constants (ASCII-safe \\uXXXX source) -----------
    // Written purely with \\uXXXX escapes so javac decodes them identically on
    // every CI host (Windows cmd / Linux / macOS) no matter the file encoding.
    // The C++ module hard-codes the matching modified-UTF-8 byte expectations.

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

    /** Supplementary-plane emoji (surrogate pair / 4-byte scalar). */
    public String emoji()
    {
        return EMOJI;
    }

    /** Interior-NUL string. */
    public String interiorNul()
    {
        return INTERIOR_NUL;
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
    //  static dispatch branch instead of GetObjectClass)
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

    public static String staticEcho(String value)
    {
        return value;
    }

    /** Always returns the literal "static-dyn-99" via a builder (non-interned). */
    public static String staticDynamic()
    {
        StringBuilder sb = new StringBuilder();
        sb.append("static-dyn-");
        sb.append(99);
        return sb.toString();
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
                // and the detour exercises every String-returning method on
                // this very instance (and the static methods).
                instance.trigger();
                MethodString.done = true;
            }
        });
    }
}

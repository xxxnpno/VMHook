package vmhook.fixtures;

import vmhook.Harness;

/**
 * Fixture for the "enum_singleton" feature (area: enums / object references).
 *
 * Mirrors the legacy example.cpp::test_enum_probe, but exhaustively, on the
 * modular harness.  A Java enum is a perfectly ordinary Java class with:
 *   - a PRIVATE constructor,
 *   - one synthetic {@code public static final <Enum> NAME} field per constant
 *     (the constant's singleton object), and
 *   - a synthetic {@code values()} array.
 * Each constant is therefore a distinct heap object (its own OOP) reachable
 * three ways the native module exercises:
 *   (a) through an INSTANCE field that references it ({@link #favoriteColor}),
 *   (b) through a STATIC field that references it ({@link #staticColor}), and
 *   (c) through the enum class's OWN synthetic static constant fields
 *       (Color.RED / Color.GREEN / Color.BLUE), read via a wrapper registered
 *       for {@code vmhook/fixtures/EnumSingleton$Color}.
 *
 * The nested {@link Color} enum carries an instance field {@code rgb} and an
 * instance method {@code brightness()} so the native side can prove it can read
 * a field declared on the enum body AND dispatch an instance method through an
 * enum singleton, exactly like any other registered class.
 *
 * Robustness: brightness() is computed JAVA-SIDE inside the probe action (real
 * bytecode dispatch) and published into static witnesses, so the module has a
 * thread-gate-independent proof of the documented result even when a native
 * method_proxy::call() has no live JavaThread to invoke the interpreter.  The
 * native side ALSO attempts the call directly (best-effort) and cross-checks.
 *
 * Every singleton's System.identityHashCode is published so the C++ identity /
 * distinctness checks are EXACT (never "non-null and hope").
 *
 * JAVA 8 SYNTAX ONLY: anonymous Harness.Probe (no lambda for the Probe itself),
 * no var / records / switch-expressions.  Compiles under --release 8.
 */
public final class EnumSingleton
{
    /** Native sets this true to request the action; the action clears done first. */
    public static volatile boolean go;

    /** The action sets this true when it has run; native polls it. */
    public static volatile boolean done;

    /**
     * Scenario selector (native sets it BEFORE raising go).  A single scenario
     * suffices: every enum-reference read is side-effect free and happens from
     * native code; the probe exists to fire real Java bytecode (brightness())
     * and to (re)publish identities + the Java-computed brightness witnesses on
     * the Java thread.
     *   0 = compute brightness() Java-side + (re)publish identities, then tick().
     */
    public static volatile int mode;

    /**
     * The enum under test.  Three constants with distinct packed-RGB ints; an
     * instance field {@code rgb}; an instance method {@code brightness()} that
     * sums the three colour channels.  These exact values are mirrored on the
     * native side (RED=0xFF0000, GREEN=0x00FF00, BLUE=0x0000FF).
     */
    public enum Color
    {
        RED   (0xFF0000),
        GREEN (0x00FF00),
        BLUE  (0x0000FF);

        /** Instance field declared on the enum body (native reads this). */
        public final int rgb;

        Color(final int rgb)
        {
            this.rgb = rgb;
        }

        /**
         * Instance method on the enum.  Sums the red, green and blue channels.
         * For GREEN (0x00FF00) this is 0x00 + 0xFF + 0x00 == 0xFF (255) — the
         * documented result the native side asserts.
         */
        public int brightness()
        {
            return ((this.rgb >> 16) & 0xFF)
                 + ((this.rgb >>  8) & 0xFF)
                 + ((this.rgb      ) & 0xFF);
        }
    }

    /**
     * A trivial interface the second enum implements, so the native side can
     * prove it reads a method DECLARED ON AN INTERFACE through an enum singleton
     * (label()), exactly like any other instance method.
     */
    public interface Labeled
    {
        String label();
    }

    /**
     * A second enum whose constants each have their OWN class body (a
     * constant-specific subclass) implementing an ABSTRACT method, and which
     * implements an interface.  javac emits the constants as anonymous
     * subclasses EnumSingleton$Op$1 / EnumSingleton$Op$2, while the constant
     * STATIC fields (PLUS / TIMES) and values()/valueOf still live on the
     * abstract base EnumSingleton$Op.  This exercises:
     *   - an enum with a field (symbol) AND an abstract method body
     *     (apply(int,int)) specialised per constant, and
     *   - an enum implementing an interface (label()).
     * The Java probe computes apply()/label() with real bytecode and publishes
     * witnesses so the native side has a thread-gate-independent proof even when
     * a native method_proxy::call() has no live JavaThread.
     */
    public enum Op implements Labeled
    {
        PLUS("+")
        {
            @Override
            public int apply(final int a, final int b)
            {
                return a + b;
            }
        },
        TIMES("*")
        {
            @Override
            public int apply(final int a, final int b)
            {
                return a * b;
            }
        };

        /** Instance field on the enum body (native reads this). */
        public final String symbol;

        Op(final String symbol)
        {
            this.symbol = symbol;
        }

        /** Constant-specific behaviour (each constant overrides this). */
        public abstract int apply(int a, int b);

        /** Interface method, shared by all constants. */
        @Override
        public String label()
        {
            return "op:" + this.symbol;
        }
    }

    // ---- Deterministic constants the native side mirrors -------------------
    public static final int RED_RGB   = 0xFF0000;
    public static final int GREEN_RGB = 0x00FF00;
    public static final int BLUE_RGB  = 0x0000FF;

    /** brightness() of each constant: sum of the three 8-bit channels. */
    public static final int RED_BRIGHTNESS   = 0xFF;   // 0xFF + 0x00 + 0x00
    public static final int GREEN_BRIGHTNESS = 0xFF;   // 0x00 + 0xFF + 0x00
    public static final int BLUE_BRIGHTNESS  = 0xFF;   // 0x00 + 0x00 + 0xFF

    // ---- The two reference fields the module decodes -----------------------

    /** INSTANCE enum-reference field — resolves to the GREEN singleton. */
    public Color favoriteColor = Color.GREEN;

    /** STATIC enum-reference field — resolves to the BLUE singleton. */
    public static Color staticColor = Color.BLUE;

    // ---- Java-side brightness witnesses (computed via real bytecode) -------
    // Published by the probe action so the native side has a thread-gate-
    // independent proof of brightness() per constant.
    public static volatile int  favoriteBrightnessSeen;   // GREEN.brightness()
    public static volatile int  staticBrightnessSeen;     // BLUE.brightness()
    public static volatile int  redBrightnessSeen;        // RED.brightness()

    // ---- Identity publication (so the C++ distinctness checks are EXACT) ---
    public static volatile int redIdentity;
    public static volatile int greenIdentity;
    public static volatile int blueIdentity;
    public static volatile int favoriteIdentity;          // == greenIdentity
    public static volatile int staticIdentity;            // == blueIdentity

    // ---- Color values()/valueOf witnesses (computed via real bytecode) -----
    public static volatile int     colorValuesLen;        // Color.values().length == 3
    public static volatile boolean valueOfGreenIsGreen;   // Color.valueOf("GREEN") == GREEN
    public static volatile int     valueOfBlueIdentity;   // id of Color.valueOf("BLUE")

    // ---- Per-constant name()/ordinal() witnesses (so name/ordinal reads are
    //      cross-checked against the JVM's own values) ----
    public static volatile int redOrdinal;                // == 0
    public static volatile int greenOrdinal;              // == 1
    public static volatile int blueOrdinal;               // == 2

    // ---- Second-enum (Op) witnesses + identities ---------------------------
    public static volatile int     opValuesLen;           // Op.values().length == 2
    public static volatile int     plusApplySeen;         // PLUS.apply(6,2) == 8
    public static volatile int     timesApplySeen;        // TIMES.apply(6,2) == 12
    public static volatile String  plusLabelSeen;         // PLUS.label() == "op:+"
    public static volatile String  timesLabelSeen;        // TIMES.label() == "op:*"
    public static volatile boolean valueOfPlusIsPlus;     // Op.valueOf("PLUS") == PLUS
    public static volatile int     plusOrdinal;           // == 0
    public static volatile int     timesOrdinal;          // == 1
    public static volatile int     plusIdentity;          // id of Op.PLUS
    public static volatile int     timesIdentity;         // id of Op.TIMES

    /**
     * The single instance the native module wraps for the INSTANCE-field read.
     * Created eagerly so the native side can fetch it through a static field and
     * so the identities published below match the OOPs the module decodes.
     */
    public static final EnumSingleton SINGLETON = new EnumSingleton();

    // ---- Hook-less driver --------------------------------------------------
    /**
     * The probe calls this through normal bytecode dispatch so a real Java frame
     * runs each cycle (mirrors the other fixtures' tick()).  No native hook is
     * installed on it; it simply gives the harness a real dispatch to perform.
     */
    public int tick(final int nonce)
    {
        return nonce + 1;
    }

    static
    {
        Harness.register(new Harness.Probe()
        {
            @Override
            public boolean pending()
            {
                return EnumSingleton.go && !EnumSingleton.done;
            }

            @Override
            public void run()
            {
                // Compute brightness() JAVA-SIDE on each singleton (real
                // bytecode), so the module has a robust witness even if a native
                // method_proxy::call() cannot find a live JavaThread.
                EnumSingleton.favoriteBrightnessSeen = SINGLETON.favoriteColor.brightness();
                EnumSingleton.staticBrightnessSeen   = EnumSingleton.staticColor.brightness();
                EnumSingleton.redBrightnessSeen      = Color.RED.brightness();

                // Publish identities the native side cross-checks against the
                // OOPs it decodes from each enum-reference field / static slot.
                EnumSingleton.redIdentity      = System.identityHashCode(Color.RED);
                EnumSingleton.greenIdentity    = System.identityHashCode(Color.GREEN);
                EnumSingleton.blueIdentity     = System.identityHashCode(Color.BLUE);
                EnumSingleton.favoriteIdentity = System.identityHashCode(SINGLETON.favoriteColor);
                EnumSingleton.staticIdentity   = System.identityHashCode(EnumSingleton.staticColor);

                // Color values()/valueOf + ordinals, via real bytecode dispatch.
                EnumSingleton.colorValuesLen      = Color.values().length;
                EnumSingleton.valueOfGreenIsGreen = Color.valueOf("GREEN") == Color.GREEN;
                EnumSingleton.valueOfBlueIdentity = System.identityHashCode(Color.valueOf("BLUE"));
                EnumSingleton.redOrdinal          = Color.RED.ordinal();
                EnumSingleton.greenOrdinal        = Color.GREEN.ordinal();
                EnumSingleton.blueOrdinal         = Color.BLUE.ordinal();

                // Second enum (Op): constant-specific abstract bodies + interface
                // method + values()/valueOf + ordinals + identities.
                EnumSingleton.opValuesLen      = Op.values().length;
                EnumSingleton.plusApplySeen    = Op.PLUS.apply(6, 2);
                EnumSingleton.timesApplySeen   = Op.TIMES.apply(6, 2);
                EnumSingleton.plusLabelSeen    = Op.PLUS.label();
                EnumSingleton.timesLabelSeen   = Op.TIMES.label();
                EnumSingleton.valueOfPlusIsPlus = Op.valueOf("PLUS") == Op.PLUS;
                EnumSingleton.plusOrdinal      = Op.PLUS.ordinal();
                EnumSingleton.timesOrdinal     = Op.TIMES.ordinal();
                EnumSingleton.plusIdentity     = System.identityHashCode(Op.PLUS);
                EnumSingleton.timesIdentity    = System.identityHashCode(Op.TIMES);

                // Real bytecode dispatch (parity with the other fixtures).
                SINGLETON.tick(7);

                EnumSingleton.done = true;
            }
        });
    }
}

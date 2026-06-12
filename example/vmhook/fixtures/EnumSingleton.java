package vmhook.fixtures;

import java.util.EnumMap;
import java.util.EnumSet;

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

    /**
     * THE enum-singleton idiom: a SINGLE-CONSTANT enum.  Joshua Bloch's
     * "preferred way to implement a singleton" (Effective Java item 3) — the one
     * constant {@code INSTANCE} IS the singleton, guaranteed unique by the JVM's
     * enum machinery (no reflection / serialization can mint a second one).  The
     * native side reads INSTANCE as the enum's own synthetic static, proves
     * values().length == 1, and proves the constant read twice is one OOP.
     */
    public enum Lonely
    {
        INSTANCE;

        /** Instance field on the single-constant enum body (native reads this). */
        public final int tag = 0x515E;   // "SINGLE"-ish sentinel

        /** Instance method on the single-constant enum (native may dispatch it). */
        public int tag()
        {
            return this.tag;
        }
    }

    /**
     * The CLASSIC (pre-enum) singleton idiom for contrast with {@link Lonely}: a
     * final class with a PRIVATE constructor and a {@code private static final}
     * INSTANCE eagerly initialised in a static initializer, exposed through
     * {@code getInstance()}.  Unlike an enum constant this is an ORDINARY object
     * (its klass is {@code EnumSingleton$ClassicSingleton}, not a synthetic enum
     * subclass) — the native side proves the private static slot decodes to a
     * stable OOP and that getInstance() (Java-side) returns that very object.
     */
    public static final class ClassicSingleton
    {
        /** Private static final instance — the canonical classic-singleton slot. */
        private static final ClassicSingleton INSTANCE = new ClassicSingleton();

        /** Payload the native side reads off the singleton instance. */
        private final int magic = 0x5A5A5A5A;

        private ClassicSingleton()
        {
        }

        /** Canonical accessor — always returns the one INSTANCE. */
        public static ClassicSingleton getInstance()
        {
            return INSTANCE;
        }

        public int magic()
        {
            return this.magic;
        }
    }

    // ---- EnumMap / EnumSet keyed on the Color enum -------------------------
    // These are SPECIAL collections (array-backed, keyed by ordinal); the native
    // library has no dedicated wrapper for them, so the module characterises
    // their runtime klass best-effort ([INFO]) and proves their CONTENTS via the
    // Java witnesses published below.  Populated eagerly so the static slots are
    // non-null when the native side reads them.
    public static final EnumMap<Color, String> COLOR_NAMES = new EnumMap<Color, String>(Color.class);
    public static final EnumSet<Color>          WARM_COLORS = EnumSet.of(Color.RED);

    static
    {
        COLOR_NAMES.put(Color.RED, "r");
        COLOR_NAMES.put(Color.GREEN, "g");
        COLOR_NAMES.put(Color.BLUE, "b");
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

    // ---- name() reflection witnesses (cross-check native `name` field reads
    //      against java.lang.Enum.name(), independent of the source literal) ----
    public static volatile String  redNameSeen;           // Color.RED.name()   == "RED"
    public static volatile String  greenNameSeen;         // Color.GREEN.name() == "GREEN"
    public static volatile String  blueNameSeen;          // Color.BLUE.name()  == "BLUE"
    public static volatile String  plusNameSeen;          // Op.PLUS.name()     == "PLUS"

    // ---- Runtime-class witnesses (so the native klass_from_oop characterisation
    //      has a Java cross-check).  A constant WITHOUT a body has the enum klass
    //      itself; a constant WITH a body is an anonymous subclass $1/$2. ----
    public static volatile String  greenClassName;        // GREEN.getClass().getName()
    public static volatile String  plusClassName;         // PLUS.getClass().getName()  (subclass)
    public static volatile String  timesClassName;        // TIMES.getClass().getName() (subclass)
    public static volatile boolean plusIsSubclassOfOp;    // PLUS.getClass() != Op.class
    public static volatile boolean greenIsExactlyColor;   // GREEN.getClass() == Color.class

    // ---- Single-constant enum (Lonely) + classic singleton witnesses --------
    public static volatile int     lonelyValuesLen;       // Lonely.values().length == 1
    public static volatile boolean lonelyInstanceIsSole;  // Lonely.INSTANCE == Lonely.values()[0]
    public static volatile int     lonelyInstanceIdentity;// id of Lonely.INSTANCE
    public static volatile int     lonelyTagSeen;         // Lonely.INSTANCE.tag()
    public static volatile boolean classicSameInstance;   // getInstance() == getInstance()
    public static volatile int     classicInstanceIdentity;// id of ClassicSingleton.getInstance()
    public static volatile int     classicMagicSeen;      // getInstance().magic()

    // ---- EnumMap / EnumSet witnesses (these are special collections; the
    //      native side has no wrapper, so contents are proven Java-side) ------
    public static volatile int     colorNamesSize;        // COLOR_NAMES.size() == 3
    public static volatile String  colorNamesGreen;       // COLOR_NAMES.get(GREEN) == "g"
    public static volatile String  colorNamesClassName;   // COLOR_NAMES.getClass().getName()
    public static volatile int     warmColorsSize;        // WARM_COLORS.size() == 1
    public static volatile boolean warmColorsHasRed;      // WARM_COLORS.contains(RED)
    public static volatile boolean warmColorsHasBlue;     // WARM_COLORS.contains(BLUE) == false
    public static volatile String  warmColorsClassName;   // WARM_COLORS.getClass().getName()

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

                // name() reflection — cross-checks the native `name` field reads
                // against java.lang.Enum.name() (the JVM's own value).
                EnumSingleton.redNameSeen   = Color.RED.name();
                EnumSingleton.greenNameSeen = Color.GREEN.name();
                EnumSingleton.blueNameSeen  = Color.BLUE.name();
                EnumSingleton.plusNameSeen  = Op.PLUS.name();

                // Runtime classes — a body-less constant's class IS the enum
                // class; a constant WITH a body is an anonymous subclass ($1/$2).
                EnumSingleton.greenClassName      = Color.GREEN.getClass().getName();
                EnumSingleton.plusClassName       = Op.PLUS.getClass().getName();
                EnumSingleton.timesClassName      = Op.TIMES.getClass().getName();
                EnumSingleton.plusIsSubclassOfOp  = Op.PLUS.getClass() != Op.class;
                EnumSingleton.greenIsExactlyColor = Color.GREEN.getClass() == Color.class;

                // Single-constant enum (the enum-singleton idiom): exactly one
                // constant, and INSTANCE IS that sole values() element.
                EnumSingleton.lonelyValuesLen        = Lonely.values().length;
                EnumSingleton.lonelyInstanceIsSole   = Lonely.INSTANCE == Lonely.values()[0];
                EnumSingleton.lonelyInstanceIdentity = System.identityHashCode(Lonely.INSTANCE);
                EnumSingleton.lonelyTagSeen          = Lonely.INSTANCE.tag();

                // Classic (pre-enum) singleton: getInstance() is idempotent and
                // identity-stable.
                EnumSingleton.classicSameInstance     = ClassicSingleton.getInstance() == ClassicSingleton.getInstance();
                EnumSingleton.classicInstanceIdentity = System.identityHashCode(ClassicSingleton.getInstance());
                EnumSingleton.classicMagicSeen        = ClassicSingleton.getInstance().magic();

                // EnumMap / EnumSet keyed on Color — contents proven Java-side.
                EnumSingleton.colorNamesSize      = COLOR_NAMES.size();
                EnumSingleton.colorNamesGreen     = COLOR_NAMES.get(Color.GREEN);
                EnumSingleton.colorNamesClassName = COLOR_NAMES.getClass().getName();
                EnumSingleton.warmColorsSize      = WARM_COLORS.size();
                EnumSingleton.warmColorsHasRed    = WARM_COLORS.contains(Color.RED);
                EnumSingleton.warmColorsHasBlue   = WARM_COLORS.contains(Color.BLUE);
                EnumSingleton.warmColorsClassName = WARM_COLORS.getClass().getName();

                // Real bytecode dispatch (parity with the other fixtures).
                SINGLETON.tick(7);

                EnumSingleton.done = true;
            }
        });
    }
}

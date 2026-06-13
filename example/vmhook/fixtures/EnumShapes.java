package vmhook.fixtures;

import java.util.EnumMap;
import java.util.EnumSet;

import vmhook.Harness;

/**
 * Fixture for the "enum_shapes" feature (area: enums / singletons) — the
 * EXHAUSTIVE companion to {@code EnumSingleton}.  Where EnumSingleton proves the
 * field-reference / static-field / wrapper-decode plumbing reaches an enum
 * singleton, this fixture sweeps EVERY enum/singleton SHAPE the native side must
 * read identically — most importantly an enum with MANY constants ({@code >16},
 * the boundary the smaller fixture never crossed), plus the 1- and 2- and
 * 4-constant cardinalities, an enum carrying constructor-initialised state, an
 * enum with constant-specific class bodies, an enum implementing an interface,
 * the classic (pre-enum) private-static-final singleton, and an enum used as an
 * EnumMap key / EnumSet element.
 *
 * A Java enum is an ordinary class with a PRIVATE constructor, one synthetic
 * {@code public static final <Enum> NAME} field per constant (each a distinct
 * heap object / OOP), and a synthetic {@code values()} backing array (the
 * private static final {@code $VALUES} field).  Every property the native module
 * asserts is ALSO computed here Java-side with real bytecode and published into
 * static witnesses, so the native reads have a thread-gate-independent cross
 * check on every JDK (Java 8/11/17/21/24/25).
 *
 * JAVA 8 SYNTAX ONLY: anonymous Harness.Probe (no lambda for the Probe itself),
 * no var / records / switch-expressions.  Compiles under --release 8.
 */
public final class EnumShapes
{
    /** Native sets this true to request the action; the action clears done first. */
    public static volatile boolean go;

    /** The action sets this true when it has run; native polls it. */
    public static volatile boolean done;

    /**
     * Scenario selector (native sets it BEFORE raising go).  One scenario
     * suffices: every enum read is side-effect free; the probe fires real Java
     * bytecode (values()/valueOf/ordinal/name/constant-body dispatch) and
     * publishes the witnesses the native side cross-checks.
     *   0 = compute every witness Java-side, then tick().
     */
    public static volatile int mode;

    // ======================================================================
    //  SHAPE A — a SIMPLE enum (no body, no field): 4 constants.
    //  Exercises name()/ordinal()/values()-order/valueOf on the plainest enum.
    // ======================================================================
    public enum Suit
    {
        CLUBS, DIAMONDS, HEARTS, SPADES;
    }

    // ======================================================================
    //  SHAPE B — a MANY-constant enum (> 16): 26 constants A..Z, each carrying
    //  its 0-based index in an enum-body field.  This is the cardinality the
    //  sibling fixture never reached; it proves $VALUES length / element
    //  identity, name/ordinal, and the body field at indices PAST 16 (where a
    //  byte-indexed or 4-bit-packed assumption would break).
    // ======================================================================
    public enum Letter
    {
        A,  B,  C,  D,  E,  F,  G,  H,  I,  J,  K,  L,  M,
        N,  O,  P,  Q,  R,  S,  T,  U,  V,  W,  X,  Y,  Z;

        /** Enum-body instance field: the constant's 0-based position. */
        public final int index = this.ordinal();
    }

    // ======================================================================
    //  SHAPE C — an enum carrying CONSTRUCTOR-INITIALISED STATE (constants carry
    //  multiple fields of different widths), implementing an INTERFACE.
    // ======================================================================
    public interface Describable
    {
        String describe();
    }

    public enum Planet implements Describable
    {
        MERCURY(3.303e+23, 2.4397e6),
        VENUS  (4.869e+24, 6.0518e6),
        EARTH  (5.976e+24, 6.37814e6);

        /** Enum-body instance fields (different widths) the native side reads. */
        public final double mass;     // kg
        public final double radius;   // m
        public final int    code;     // a small int derived in the ctor

        Planet(final double mass, final double radius)
        {
            this.mass   = mass;
            this.radius = radius;
            this.code   = this.name().length();   // deterministic per-constant int
        }

        /** Interface method shared by all constants. */
        @Override
        public String describe()
        {
            return "planet:" + this.name();
        }
    }

    // ======================================================================
    //  SHAPE D — CONSTANT-SPECIFIC BODIES: each constant overrides an abstract
    //  method (javac emits anonymous subclasses EnumShapes$MathOp$1 / $2), with
    //  a String field, implementing an interface too.
    // ======================================================================
    public enum MathOp implements Describable
    {
        ADD("+")
        {
            @Override
            public int eval(final int a, final int b)
            {
                return a + b;
            }
        },
        SUB("-")
        {
            @Override
            public int eval(final int a, final int b)
            {
                return a - b;
            }
        };

        public final String sym;

        MathOp(final String sym)
        {
            this.sym = sym;
        }

        /** Constant-specific behaviour (each constant overrides this). */
        public abstract int eval(int a, int b);

        @Override
        public String describe()
        {
            return "mathop:" + this.sym;
        }
    }

    // ======================================================================
    //  SHAPE E — the SINGLE-CONSTANT enum (Bloch's enum-singleton idiom).
    // ======================================================================
    public enum Sole
    {
        INSTANCE;

        public final int marker = 0x501E;   // "SOLE"-ish sentinel
        public int marker() { return this.marker; }
    }

    // ======================================================================
    //  SHAPE F — the CLASSIC (pre-enum) singleton: a final class with a PRIVATE
    //  ctor and a private static final INSTANCE behind getInstance().
    // ======================================================================
    public static final class Registry
    {
        private static final Registry INSTANCE = new Registry();

        private final int token = 0x7E57;   // "TEST"-ish sentinel

        private Registry()
        {
        }

        public static Registry getInstance()
        {
            return INSTANCE;
        }

        public int token()
        {
            return this.token;
        }
    }

    // ---- EnumMap / EnumSet keyed on the Suit enum --------------------------
    public static final EnumMap<Suit, Integer> SUIT_RANK = new EnumMap<Suit, Integer>(Suit.class);
    public static final EnumSet<Suit>          REDS      = EnumSet.of(Suit.DIAMONDS, Suit.HEARTS);

    static
    {
        SUIT_RANK.put(Suit.CLUBS,    1);
        SUIT_RANK.put(Suit.DIAMONDS, 2);
        SUIT_RANK.put(Suit.HEARTS,   3);
        SUIT_RANK.put(Suit.SPADES,   4);
    }

    // ---- The holder's own static reference field into a constant ------------
    /** STATIC enum-reference field -> the HEARTS singleton. */
    public static Suit trump = Suit.HEARTS;

    // ---- Java-side witnesses (computed via real bytecode) ------------------

    // Shape A — Suit.
    public static volatile int     suitValuesLen;       // == 4
    public static volatile String  suitName2;           // Suit.values()[2].name() == "HEARTS"
    public static volatile int     heartsOrdinal;       // == 2
    public static volatile boolean valueOfHeartsIsHearts;// Suit.valueOf("HEARTS") == HEARTS
    public static volatile int     heartsIdentity;      // id of Suit.HEARTS
    public static volatile int     trumpIdentity;       // id of trump (== heartsIdentity)

    // Shape B — Letter (the > 16 enum).
    public static volatile int     letterValuesLen;     // == 26
    public static volatile String  letterName0;         // values()[0].name()  == "A"
    public static volatile String  letterName16;        // values()[16].name() == "Q"
    public static volatile String  letterName17;        // values()[17].name() == "R"
    public static volatile String  letterName25;        // values()[25].name() == "Z"
    public static volatile int     letterIndex16;       // Q.index == 16
    public static volatile int     letterIndex25;       // Z.index == 25
    public static volatile int     letterOrdinal16;     // Q.ordinal() == 16
    public static volatile int     letterOrdinal25;     // Z.ordinal() == 25
    public static volatile boolean valueOfQisElem16;    // valueOf("Q") == values()[16]
    public static volatile boolean letterAllOrdinalsContiguous; // ordinals 0..25 in order
    public static volatile int     letterAIdentity;     // id of Letter.A
    public static volatile int     letterQIdentity;     // id of Letter.Q (values()[16])
    public static volatile int     letterZIdentity;     // id of Letter.Z (values()[25])

    // Shape C — Planet.
    public static volatile int     planetValuesLen;     // == 3
    public static volatile double  earthMass;           // EARTH.mass
    public static volatile double  earthRadius;         // EARTH.radius
    public static volatile int     earthCode;           // EARTH.code == "EARTH".length() == 5
    public static volatile int     mercuryCode;         // MERCURY.code == 7
    public static volatile String  earthDescribe;       // EARTH.describe() == "planet:EARTH"
    public static volatile int     earthIdentity;       // id of Planet.EARTH

    // Shape D — MathOp (constant-specific bodies + interface).
    public static volatile int     mathOpValuesLen;     // == 2
    public static volatile int     addEvalSeen;         // ADD.eval(6,2) == 8
    public static volatile int     subEvalSeen;         // SUB.eval(6,2) == 4
    public static volatile String  addSym;              // ADD.sym == "+"
    public static volatile String  addDescribe;         // ADD.describe() == "mathop:+"
    public static volatile boolean valueOfAddIsAdd;     // MathOp.valueOf("ADD") == ADD
    public static volatile String  addClassName;        // ADD.getClass().getName() (subclass $1)
    public static volatile String  subClassName;        // SUB.getClass().getName() (subclass $2)
    public static volatile boolean addIsSubclassOfMathOp;// ADD.getClass() != MathOp.class
    public static volatile int     addIdentity;         // id of MathOp.ADD
    public static volatile int     subIdentity;         // id of MathOp.SUB

    // Shape A runtime-class cross-check (body-LESS constant is exactly the enum).
    public static volatile boolean heartsIsExactlySuit; // HEARTS.getClass() == Suit.class
    public static volatile String  heartsClassName;     // HEARTS.getClass().getName()

    // Shape E — Sole (single-constant enum).
    public static volatile int     soleValuesLen;       // == 1
    public static volatile boolean soleInstanceIsSole;  // INSTANCE == values()[0]
    public static volatile int     soleMarkerSeen;      // INSTANCE.marker()
    public static volatile int     soleIdentity;        // id of Sole.INSTANCE

    // Shape F — Registry (classic singleton).
    public static volatile boolean registrySameInstance;// getInstance() == getInstance()
    public static volatile int     registryTokenSeen;   // getInstance().token()
    public static volatile int     registryIdentity;    // id of Registry.getInstance()

    // EnumMap / EnumSet (special collections; contents proven Java-side).
    public static volatile int     suitRankSize;        // == 4
    public static volatile int     suitRankHearts;      // SUIT_RANK.get(HEARTS) == 3
    public static volatile String  suitRankClassName;   // java.util.EnumMap
    public static volatile int     redsSize;            // == 2
    public static volatile boolean redsHasHearts;       // REDS.contains(HEARTS)
    public static volatile boolean redsHasClubs;        // REDS.contains(CLUBS) == false
    public static volatile String  redsClassName;       // java.util.RegularEnumSet

    /**
     * The single instance the native module wraps for the handshake/tick.
     * Created eagerly so the native side can fetch it through a static field.
     */
    public static final EnumShapes SINGLETON = new EnumShapes();

    /** Real bytecode dispatch each cycle (parity with the other fixtures). */
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
                return EnumShapes.go && !EnumShapes.done;
            }

            @Override
            public void run()
            {
                // ---- Shape A: Suit ----
                EnumShapes.suitValuesLen         = Suit.values().length;
                EnumShapes.suitName2             = Suit.values()[2].name();
                EnumShapes.heartsOrdinal         = Suit.HEARTS.ordinal();
                EnumShapes.valueOfHeartsIsHearts = Suit.valueOf("HEARTS") == Suit.HEARTS;
                EnumShapes.heartsIdentity        = System.identityHashCode(Suit.HEARTS);
                EnumShapes.trumpIdentity         = System.identityHashCode(EnumShapes.trump);
                EnumShapes.heartsIsExactlySuit   = Suit.HEARTS.getClass() == Suit.class;
                EnumShapes.heartsClassName       = Suit.HEARTS.getClass().getName();

                // ---- Shape B: Letter (> 16 constants) ----
                Letter[] letters = Letter.values();
                EnumShapes.letterValuesLen = letters.length;
                EnumShapes.letterName0     = letters[0].name();
                EnumShapes.letterName16    = letters[16].name();
                EnumShapes.letterName17    = letters[17].name();
                EnumShapes.letterName25    = letters[25].name();
                EnumShapes.letterIndex16   = letters[16].index;
                EnumShapes.letterIndex25   = letters[25].index;
                EnumShapes.letterOrdinal16 = letters[16].ordinal();
                EnumShapes.letterOrdinal25 = letters[25].ordinal();
                EnumShapes.valueOfQisElem16 = Letter.valueOf("Q") == letters[16];
                boolean contiguous = true;
                for (int i = 0; i < letters.length; ++i)
                {
                    if (letters[i].ordinal() != i)
                    {
                        contiguous = false;
                        break;
                    }
                }
                EnumShapes.letterAllOrdinalsContiguous = contiguous;
                EnumShapes.letterAIdentity = System.identityHashCode(Letter.A);
                EnumShapes.letterQIdentity = System.identityHashCode(letters[16]);
                EnumShapes.letterZIdentity = System.identityHashCode(letters[25]);

                // ---- Shape C: Planet (state + interface) ----
                EnumShapes.planetValuesLen = Planet.values().length;
                EnumShapes.earthMass       = Planet.EARTH.mass;
                EnumShapes.earthRadius     = Planet.EARTH.radius;
                EnumShapes.earthCode       = Planet.EARTH.code;
                EnumShapes.mercuryCode     = Planet.MERCURY.code;
                EnumShapes.earthDescribe   = Planet.EARTH.describe();
                EnumShapes.earthIdentity   = System.identityHashCode(Planet.EARTH);

                // ---- Shape D: MathOp (constant-specific bodies + interface) ----
                EnumShapes.mathOpValuesLen      = MathOp.values().length;
                EnumShapes.addEvalSeen          = MathOp.ADD.eval(6, 2);
                EnumShapes.subEvalSeen          = MathOp.SUB.eval(6, 2);
                EnumShapes.addSym               = MathOp.ADD.sym;
                EnumShapes.addDescribe          = MathOp.ADD.describe();
                EnumShapes.valueOfAddIsAdd      = MathOp.valueOf("ADD") == MathOp.ADD;
                EnumShapes.addClassName         = MathOp.ADD.getClass().getName();
                EnumShapes.subClassName         = MathOp.SUB.getClass().getName();
                EnumShapes.addIsSubclassOfMathOp = MathOp.ADD.getClass() != MathOp.class;
                EnumShapes.addIdentity          = System.identityHashCode(MathOp.ADD);
                EnumShapes.subIdentity          = System.identityHashCode(MathOp.SUB);

                // ---- Shape E: Sole (single-constant enum) ----
                EnumShapes.soleValuesLen      = Sole.values().length;
                EnumShapes.soleInstanceIsSole = Sole.INSTANCE == Sole.values()[0];
                EnumShapes.soleMarkerSeen     = Sole.INSTANCE.marker();
                EnumShapes.soleIdentity       = System.identityHashCode(Sole.INSTANCE);

                // ---- Shape F: Registry (classic singleton) ----
                EnumShapes.registrySameInstance = Registry.getInstance() == Registry.getInstance();
                EnumShapes.registryTokenSeen    = Registry.getInstance().token();
                EnumShapes.registryIdentity     = System.identityHashCode(Registry.getInstance());

                // ---- EnumMap / EnumSet keyed on Suit ----
                EnumShapes.suitRankSize      = SUIT_RANK.size();
                EnumShapes.suitRankHearts    = SUIT_RANK.get(Suit.HEARTS).intValue();
                EnumShapes.suitRankClassName = SUIT_RANK.getClass().getName();
                EnumShapes.redsSize          = REDS.size();
                EnumShapes.redsHasHearts     = REDS.contains(Suit.HEARTS);
                EnumShapes.redsHasClubs      = REDS.contains(Suit.CLUBS);
                EnumShapes.redsClassName     = REDS.getClass().getName();

                // Real bytecode dispatch (parity with the other fixtures).
                SINGLETON.tick(7);

                EnumShapes.done = true;
            }
        });
    }
}

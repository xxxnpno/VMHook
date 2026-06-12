package vmhook.fixtures;

import vmhook.Harness;

/**
 * Fixture for the method_overload_java_dispatch feature — the JAVA POLYMORPHIC
 * OVERRIDE-dispatch half (area: methods).
 *
 * DISTINCT from MethodOverload (C++-arg-type overload RESOLUTION) and from the
 * `f`/`h` family in this same module's resolution half: THIS fixture is about
 * RUNTIME-TYPE virtual dispatch — calling a method through method_proxy::call()
 * must reach the OVERRIDE belonging to the receiver's RUNTIME class, not the
 * declared/base class's body — i.e. genuine {@code invokevirtual} semantics.
 *
 * It exercises EVERY override shape the native module drives through a hooked
 * call, all folded into ONE top-level fixture so a single Class.forName loads the
 * whole hierarchy (Main.loadFixtures() only forName's the top-level fixture):
 *
 * <pre>
 *   1. 2-LEVEL OVERRIDE        Base.shape()->"base-shape"; Derived overrides ->"derived-shape"
 *   2. 3-LEVEL CHAIN           L0.rank()->10; L1 overrides ->20; L2 (Leaf) overrides ->30;
 *                              PartialMid extends L0 but does NOT override rank() -> inherits 10
 *   3. INTERFACE DEFAULT       Greeter.greet() default ->"default-greet";
 *                              Overrider overrides ->"overrider-greet";
 *                              Inheritor does NOT override -> inherits the default
 *   4. super.method()          Derived.shapeViaSuper() returns "[" + super.shape() + "]"
 *                              -> proves a non-virtual super call reaches Base's body
 *   5. final method            Base.finalTag()->"final-base" is final (no override possible)
 *   6. HOOK-ON-BASE            Base.step(int) is INHERITED (not overridden) by Derived;
 *                              the native module hooks Base.step and drives derived.step(),
 *                              proving the i2i-stub hook shared across the hierarchy fires
 *                              for the subclass call and the detour sees the Derived receiver.
 *                              Counterpart: Base.beat(int) is OVERRIDDEN by Derived, so the
 *                              base hook does NOT fire for derived.beat() (separate Method).
 *   7. OVERLOADED + OVERRIDDEN combo(int)->"+i" / combo(String)->"+s" both overridden in
 *                              Derived -> the (sig, runtime-type) pair selects Derived's body
 *   8. ABSTRACT               AbstractArea.area() abstract; Square->s*s, Circle->314 (impls run)
 * </pre>
 *
 * Distinct concrete instances are published as eager static singletons so the
 * native module can wrap each by its RUNTIME class (and also by its BASE class,
 * to probe whether call() virtual-dispatches a base-typed wrapper).  Each
 * overridable body records its own per-class hit counter + arg/result echo so the
 * module can prove, from Java's OWN observable state, that the intended override
 * ran and no sibling did.
 *
 * The handshake uses a `mode` selector (canonical HookSignature pattern): the
 * native module sets `mode` and clears `done` on the rising edge of `go`, so each
 * probe cycle drives exactly one Java call sequence.  Java 8 syntax only
 * (anonymous Harness.Probe, no var / lambdas / records / switch-expressions).
 */
public final class MethodOverrideDispatch
{
    // ── go / done / mode handshake (native sets mode + clears done first) ──────
    /** Native sets this true to request the action; clears it after. */
    public static volatile boolean go;

    /** The action sets this true when it has run; native polls it. */
    public static volatile boolean done;

    /**
     * Scenario selector.  Set by the native module BEFORE raising {@code go}.
     *   0 = fire the native call site (drive tick once) AND publish every Java-side
     *       virtual-dispatch witness (one shot — every native call() runs in the
     *       detour against the published singletons).
     *   1 = drive derived.step(STEP_ARG) once   — hook-on-base, INHERITED method
     *   2 = drive derived.beat(BEAT_ARG) once    — hook-on-base, OVERRIDDEN method
     */
    public static volatile int mode;

    // ── tick() hook site (native runs every call() inside this detour) ────────
    /** tick() invocation count — handshake proof the main detour fired. */
    public static volatile int tickCount;

    /**
     * The native module hooks this; calling it through real bytecode dispatch is
     * what makes the interpreter hook fire.  Every override read in the module
     * happens from native code against the published singletons, so this body
     * need only exist and bump a counter.
     */
    public int tick(final int nonce)
    {
        tickCount++;
        return nonce + 1;
    }

    // ══════════════════════════════════════════════════════════════════════════
    //  1. TWO-LEVEL OVERRIDE  (Base / Derived)
    // ══════════════════════════════════════════════════════════════════════════

    public static final String BASE_SHAPE    = "base-shape";
    public static final String DERIVED_SHAPE = "derived-shape";

    /** The base class: an overridable shape(), a final method, and two hook-site
     *  methods (one inherited, one overridden by Derived). */
    public static class Base
    {
        // per-class virtual-dispatch recorders
        public static volatile int    baseShapeHits;
        public static volatile int    baseStepHits;
        public static volatile int    baseStepArg;
        public static volatile int    baseBeatHits;

        /** Overridable: Derived replaces this body. */
        public String shape()
        {
            baseShapeHits++;
            return BASE_SHAPE;
        }

        /** FINAL — no subclass may override it; dispatch is always THIS body. */
        public final String finalTag()
        {
            return "final-base";
        }

        /** INHERITED by Derived (NOT overridden) — the hook-on-base i2i-stub is
         *  shared, so hooking THIS Method catches derived.step() too. */
        public int step(final int n)
        {
            baseStepHits++;
            baseStepArg = n;
            return n + 1;
        }

        /** OVERRIDDEN by Derived — Derived has its OWN Method/i2i stub, so a hook
         *  on THIS base Method does NOT fire for derived.beat(). */
        public int beat(final int n)
        {
            baseBeatHits++;
            return n + 100;
        }

        // ── 7. OVERLOADED + OVERRIDDEN: BOTH overloads declared on Base so a
        //      Base-typed wrapper resolves them by name; Derived overrides BOTH.
        //      Base bodies return DISTINCT BASE sentinels (never equal to a Derived
        //      override result) so a base-body mis-dispatch is unambiguously visible.
        public static volatile int    baseComboIntHits;
        public static volatile int    baseComboStrHits;

        /** Base combo(int) -> BASE sentinel (overridden by Derived). */
        public int combo(final int i)
        {
            baseComboIntHits++;
            return -1;   // BASE sentinel — distinct from Derived.combo(int) (i + 1000)
        }

        /** Base combo(String) -> BASE sentinel (overridden by Derived). */
        public String combo(final String s)
        {
            baseComboStrHits++;
            return "base-combo";   // BASE sentinel — distinct from Derived "+s"
        }
    }

    /** The derived class: overrides shape(), beat(), and the combo() overloads;
     *  inherits step() and finalTag(); adds a super.shape() caller. */
    public static class Derived extends Base
    {
        public static volatile int    derivedShapeHits;
        public static volatile int    derivedBeatHits;
        public static volatile int    derivedComboIntHits;
        public static volatile int    derivedComboStrHits;
        public static volatile int    derivedComboIntArg;
        public static volatile String derivedComboStrArg;

        /** Overrides Base.shape() — virtual dispatch must reach THIS body. */
        @Override
        public String shape()
        {
            derivedShapeHits++;
            return DERIVED_SHAPE;
        }

        /** super.shape() — a NON-virtual super call reaches Base.shape() from a
         *  Derived receiver; the result is wrapped so the native side can tell
         *  apart "super reached Base" from "virtual reached Derived". */
        public String shapeViaSuper()
        {
            return "[" + super.shape() + "]";
        }

        /** Overrides Base.beat() — Derived's OWN Method (separate i2i stub). */
        @Override
        public int beat(final int n)
        {
            derivedBeatHits++;
            return n + 200;
        }

        // ── 7. OVERLOADED + OVERRIDDEN: both overloads overridden in Derived ──
        /** combo(int) override -> arg + COMBO_INT_BIAS. */
        @Override
        public int combo(final int i)
        {
            derivedComboIntHits++;
            derivedComboIntArg = i;
            return i + COMBO_INT_BIAS;
        }

        /** combo(String) override -> "+" + s. */
        @Override
        public String combo(final String s)
        {
            derivedComboStrHits++;
            derivedComboStrArg = s;
            return "+" + s;
        }
    }

    // The combo() overloads also exist on Base so a Base-typed wrapper resolves
    // them by name; Derived overrides BOTH.  Base bodies return distinct
    // sentinels so a mis-dispatch (base body instead of override) is observable.
    public static final int    COMBO_INT_ARG    = 7;
    public static final int    COMBO_INT_BIAS   = 1000;       // Derived.combo(int) = i + 1000
    public static final int    COMBO_INT_EXPECT = 1007;       // 7 + 1000
    public static final String COMBO_STR_ARG    = "x";
    public static final String COMBO_STR_EXPECT = "+x";       // Derived.combo(String)

    // ══════════════════════════════════════════════════════════════════════════
    //  2. THREE-LEVEL CHAIN  (L0 -> L1 -> L2) + a partial-override sibling
    // ══════════════════════════════════════════════════════════════════════════

    public static final int RANK_L0 = 10;
    public static final int RANK_L1 = 20;
    public static final int RANK_L2 = 30;

    public static class L0
    {
        public static volatile int l0RankHits;
        public int rank()
        {
            l0RankHits++;
            return RANK_L0;
        }
    }

    public static class L1 extends L0
    {
        public static volatile int l1RankHits;
        @Override
        public int rank()
        {
            l1RankHits++;
            return RANK_L1;
        }
    }

    public static class L2 extends L1
    {
        public static volatile int l2RankHits;
        @Override
        public int rank()
        {
            l2RankHits++;
            return RANK_L2;
        }
    }

    /** Extends L0 but does NOT override rank(): an L0-rooted wrapper around a
     *  PartialMid receiver must dispatch L0's body (10), proving the walk lands
     *  on the nearest ANCESTOR that declares it. */
    public static class PartialMid extends L0
    {
        // intentionally no rank() override
        public int marker()
        {
            return 99;
        }
    }

    // ══════════════════════════════════════════════════════════════════════════
    //  3. INTERFACE DEFAULT METHOD  (Greeter)
    // ══════════════════════════════════════════════════════════════════════════

    public static final String DEFAULT_GREET   = "default-greet";
    public static final String OVERRIDER_GREET = "overrider-greet";

    public interface Greeter
    {
        /** Interface DEFAULT (Java 8+): INHERITED by Inheritor, OVERRIDDEN by Overrider. */
        String greet();   // declared so concrete impls always provide a body; see note
    }

    // NOTE: Greeter.greet() is declared ABSTRACT above (not a default) on purpose:
    // a true Java-8 `default` body reachable only through the interface chain is
    // already exhaustively covered by interface_polymorphism (Animal.defaultGreet
    // INHERITED via the cold-path _transitive_interfaces fallback).  Here the
    // override-vs-inherit contrast is what matters, so DefaultGreeter below carries
    // the concrete default body, Overrider replaces it, and Inheritor extends
    // DefaultGreeter WITHOUT overriding — giving a SUPER-CHAIN-reachable default
    // (HARD on every JDK) rather than re-testing the interface-chain fallback.

    /** Concrete base carrying the "default" greet() body on a real superclass, so
     *  an Inheritor's super walk reaches it on every JDK (no interface-chain
     *  cold-path dependency — that path is interface_polymorphism's job). */
    public static class DefaultGreeter implements Greeter
    {
        public static volatile int defaultGreetHits;
        @Override
        public String greet()
        {
            defaultGreetHits++;
            return DEFAULT_GREET;
        }
    }

    /** Overrides the default greet() body. */
    public static class Overrider extends DefaultGreeter
    {
        public static volatile int overriderGreetHits;
        @Override
        public String greet()
        {
            overriderGreetHits++;
            return OVERRIDER_GREET;
        }
    }

    /** Does NOT override greet() — inherits DefaultGreeter's body via the super walk. */
    public static class Inheritor extends DefaultGreeter
    {
        // intentionally no greet() override
    }

    // ══════════════════════════════════════════════════════════════════════════
    //  8. ABSTRACT METHOD implemented in subclasses
    // ══════════════════════════════════════════════════════════════════════════

    public static final int SQUARE_SIDE   = 5;
    public static final int SQUARE_AREA    = 25;     // 5 * 5
    public static final int CIRCLE_AREA    = 314;    // fixed sentinel

    public abstract static class AbstractArea
    {
        public static volatile int squareAreaHits;
        public static volatile int circleAreaHits;
        /** Abstract: every concrete subclass provides its own body. */
        public abstract int area();
    }

    public static final class Square extends AbstractArea
    {
        private final int side;
        public Square(final int s) { this.side = s; }
        @Override
        public int area()
        {
            squareAreaHits++;
            return this.side * this.side;
        }
    }

    public static final class Circle extends AbstractArea
    {
        @Override
        public int area()
        {
            circleAreaHits++;
            return CIRCLE_AREA;
        }
    }

    // ══════════════════════════════════════════════════════════════════════════
    //  Published instances the native module wraps (eager so klasses are loaded
    //  and the OOPs exist by the time the module runs).
    // ══════════════════════════════════════════════════════════════════════════
    public static final Base           BASE_INSTANCE       = new Base();
    public static final Derived        DERIVED_INSTANCE    = new Derived();
    public static final L0             L0_INSTANCE         = new L0();
    public static final L1             L1_INSTANCE         = new L1();
    public static final L2             L2_INSTANCE         = new L2();
    public static final PartialMid     PARTIAL_MID_INSTANCE = new PartialMid();
    public static final DefaultGreeter DEFAULT_GREETER     = new DefaultGreeter();
    public static final Overrider      OVERRIDER_INSTANCE  = new Overrider();
    public static final Inheritor      INHERITOR_INSTANCE  = new Inheritor();
    public static final AbstractArea   SQUARE_INSTANCE     = new Square(SQUARE_SIDE);
    public static final AbstractArea   CIRCLE_INSTANCE     = new Circle();

    // ── The singleton the native detour drives tick() on ──────────────────────
    public static final MethodOverrideDispatch SINGLETON = new MethodOverrideDispatch();

    // ── hook-on-base scenario arguments ───────────────────────────────────────
    public static final int STEP_ARG = 41;   // derived.step(41) -> 42 (Base body)
    public static final int BEAT_ARG = 8;    // derived.beat(8)  -> 208 (Derived body)

    // ══════════════════════════════════════════════════════════════════════════
    //  Java-side WITNESSES — the GROUND TRUTH.  The probe runs the SAME virtual
    //  dispatches through genuine invokevirtual / invokeinterface bytecode and
    //  publishes the results so the native module can cross-check that the JVM
    //  itself reaches the same overrides.  (Java's own dispatch always runs on the
    //  Java thread, independent of the native call gate, so these are HARD.)
    // ══════════════════════════════════════════════════════════════════════════
    public static volatile String  wBaseShape          = "";
    public static volatile String  wDerivedShape       = "";
    public static volatile String  wShapeViaSuper      = "";
    public static volatile String  wFinalViaBase       = "";
    public static volatile String  wFinalViaDerived    = "";
    public static volatile int     wRankL0             = -1;
    public static volatile int     wRankL1             = -1;
    public static volatile int     wRankL2             = -1;
    public static volatile int     wRankPartialMid     = -1;
    public static volatile String  wDefaultGreet       = "";
    public static volatile String  wOverriderGreet     = "";
    public static volatile String  wInheritorGreet     = "";
    public static volatile int     wComboInt           = -1;
    public static volatile String  wComboStr           = "";
    public static volatile int     wSquareArea         = -1;
    public static volatile int     wCircleArea         = -1;
    public static volatile boolean wAllDistinctSeen    = false;

    private void publishWitnesses()
    {
        // Each call here is a genuine virtual dispatch on the declared static type
        // shown, so the JVM selects the runtime override exactly as the spec says.
        final Base     b = BASE_INSTANCE;
        final Base     d = DERIVED_INSTANCE;          // declared Base, runtime Derived
        wBaseShape       = b.shape();                 // -> base-shape
        wDerivedShape    = d.shape();                 // -> derived-shape (virtual)
        wShapeViaSuper   = DERIVED_INSTANCE.shapeViaSuper(); // -> [base-shape]
        wFinalViaBase    = b.finalTag();              // -> final-base
        wFinalViaDerived = d.finalTag();              // -> final-base (inherited final)

        final L0 l0 = L0_INSTANCE;
        final L0 l1 = L1_INSTANCE;                    // declared L0, runtime L1
        final L0 l2 = L2_INSTANCE;                    // declared L0, runtime L2
        final L0 pm = PARTIAL_MID_INSTANCE;           // declared L0, runtime PartialMid
        wRankL0         = l0.rank();                  // 10
        wRankL1         = l1.rank();                  // 20 (virtual)
        wRankL2         = l2.rank();                  // 30 (virtual)
        wRankPartialMid = pm.rank();                  // 10 (inherited from L0)

        final Greeter g0 = DEFAULT_GREETER;
        final Greeter g1 = OVERRIDER_INSTANCE;
        final Greeter g2 = INHERITOR_INSTANCE;
        wDefaultGreet   = g0.greet();                 // default-greet
        wOverriderGreet = g1.greet();                 // overrider-greet (virtual)
        wInheritorGreet = g2.greet();                 // default-greet (inherited)

        final Base combo = DERIVED_INSTANCE;          // declared Base, runtime Derived
        wComboInt = combo.combo(COMBO_INT_ARG);       // 1007 (Derived override)
        wComboStr = combo.combo(COMBO_STR_ARG);       // +x   (Derived override)

        final AbstractArea sq = SQUARE_INSTANCE;
        final AbstractArea ci = CIRCLE_INSTANCE;
        wSquareArea = sq.area();                      // 25
        wCircleArea = ci.area();                      // 314

        wAllDistinctSeen =
               !wBaseShape.equals(wDerivedShape)
            && wRankL0 != wRankL1 && wRankL1 != wRankL2 && wRankL0 != wRankL2
            && !wDefaultGreet.equals(wOverriderGreet)
            &&  wDefaultGreet.equals(wInheritorGreet)   // inheritor MUST equal default
            &&  wSquareArea != wCircleArea;
    }

    static
    {
        Harness.register(new Harness.Probe()
        {
            @Override
            public boolean pending()
            {
                return MethodOverrideDispatch.go && !MethodOverrideDispatch.done;
            }

            @Override
            public void run()
            {
                switch (MethodOverrideDispatch.mode)
                {
                    case 1:
                        // hook-on-base, INHERITED method: derived.step() runs Base.step
                        // (Derived inherits it) — the native hook on Base.step must fire.
                        DERIVED_INSTANCE.step(STEP_ARG);
                        break;
                    case 2:
                        // hook-on-base, OVERRIDDEN method: derived.beat() runs Derived.beat
                        // (separate Method) — the native hook on Base.beat must NOT fire.
                        DERIVED_INSTANCE.beat(BEAT_ARG);
                        break;
                    default:
                        // mode 0: publish witnesses and fire the native call site.
                        SINGLETON.publishWitnesses();
                        SINGLETON.tick(11);
                        break;
                }
                MethodOverrideDispatch.done = true;
            }
        });
    }
}

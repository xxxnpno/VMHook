package vmhook.fixtures;

import vmhook.Harness;

/**
 * Fixture for the {@code method_override_dispatch} feature (area: hooks / i2i
 * interpreter-stub demultiplexing of OVERRIDDEN methods), issue #31.
 *
 * <p>THE hook-side authority for what happens when an OVERRIDDEN Java method is
 * hooked.  It is the inheritance-hierarchy companion to {@code hook_chaining}:
 * that module proves the shared i2i stub demultiplexes a mixed call stream of
 * DISTINCT methods (different names / descriptors) to the right per-method
 * detour; THIS fixture proves the same demux for methods that share a NAME and a
 * DESCRIPTOR but live at different levels of a class hierarchy.  vmhook keys each
 * hook to a SPECIFIC {@code Method*} (the one declared on the registered class's
 * own {@code _methods} array), and HotSpot's shared stub fires a detour only when
 * a call RESOLVES to that exact {@code Method*}.  Because an override is a
 * physically distinct {@code Method*} from the base declaration, hooking
 * {@code Base.m} and hooking {@code Derived.m} must fire on DISJOINT receiver
 * runtime types and never cross-fire — that is the override-dispatch contract.</p>
 *
 * <p>Six hierarchy shapes (all nested static types so a single
 * {@code Class.forName("vmhook.fixtures.OverrideDispatch")} transitively loads
 * everything; {@code Main.loadFixtures()} only forName's top-level fixtures):</p>
 *
 * <pre>
 *   1. SINGLE OVERRIDE              Base.m()  &lt;-  Derived.m() (override)
 *   2. MULTI-LEVEL CHAIN           A.g() &lt;- B.g() (override) &lt;- C.g() (override)
 *   3. OVERRIDE + super-call       SuperBase.s() ; SuperDerived.s() calls super.s()
 *   4. OVERRIDE vs OVERLOAD        Ov.p(int) &amp; Ov.p(String); OvSub overrides p(int) only
 *   5. INTERFACE default override  Iface.d() default; ImplOver overrides, ImplInherit inherits
 *   6. ABSTRACT-method impl        AbsBase.a() abstract ; AbsImpl.a() concrete
 * </pre>
 *
 * <p>Each method records a distinct, deterministic Java-side MARKER (a per-method
 * "last fired" int + a per-method Java hit counter), and every concrete object
 * carries a {@code kind} int the native detour can read off {@code self} to prove
 * the receiver runtime type that reached the detour.  A {@code mode} selector
 * drives exactly the call(s) a given native scenario is about to assert, so the
 * native side can hook a subset, drive one receiver type, and prove WHICH detour
 * fired (HARD) and that the siblings stayed silent (HARD), with a cross-fire
 * sentinel as the teeth.</p>
 *
 * <p>Java 8 source only: anonymous {@code Harness.Probe}, no var / records /
 * lambdas-as-fields / switch-expressions / text-blocks, so it compiles
 * identically under javac 8 and javac 25+.</p>
 */
public final class OverrideDispatch
{
    // ── go / done / mode handshake (native sets mode + clears done first) ────
    /** Native sets this true to request the action; the probe clears it via done. */
    public static volatile boolean go;

    /** The probe action sets this true when it has run; native polls it. */
    public static volatile boolean done;

    /**
     * Scenario selector — native sets this BEFORE raising go so one probe cycle
     * drives exactly the call(s) about to be asserted.  {@code done} latches, so
     * each native scenario clears done, programs mode, then runs ONE probe cycle.
     *
     *   1  = Base.m()          on a BASE receiver        (single-override, base side)
     *   2  = Derived.m()       on a DERIVED receiver     (single-override, derived side)
     *   3  = A.g() / B.g() / C.g() each on its OWN type   (multi-level chain, all three)
     *   4  = SuperDerived.s()  (which itself calls super.s())  (override + super-call)
     *   5  = Ov.p(int) + Ov.p(String) on an Ov           (overload disambiguation, base)
     *   6  = OvSub.p(int) + OvSub.p(String) on an OvSub   (overload + override mix)
     *   7  = ImplOver.d()      on an ImplOver            (interface default OVERRIDDEN)
     *   8  = ImplInherit.d()   on an ImplInherit         (interface default INHERITED)
     *   9  = AbsImpl.a()       on an AbsImpl             (abstract-method impl dispatch)
     *  10  = Base.m() on Base AND Derived.m() on Derived in ONE pass (both, simultaneous)
     */
    public static volatile int mode;

    /** tick-style proof the probe ran at least once (native sanity). */
    public static volatile int probeRuns;

    // ── Per-method Java-side MARKERS + hit counters (proof of WHICH body ran) ─
    // Each method stamps a distinct value into lastFired and bumps its own
    // counter.  The native side reads these back to confirm — from Java's own
    // observable state — that exactly the intended body executed.
    public static final int MARK_BASE_M       = 101;  // Base.m
    public static final int MARK_DERIVED_M    = 102;  // Derived.m
    public static final int MARK_A_G          = 201;  // A.g
    public static final int MARK_B_G          = 202;  // B.g
    public static final int MARK_C_G          = 203;  // C.g
    public static final int MARK_SUPER_BASE_S = 301;  // SuperBase.s
    public static final int MARK_SUPER_DER_S  = 302;  // SuperDerived.s
    public static final int MARK_OV_P_INT     = 401;  // Ov.p(int)
    public static final int MARK_OV_P_STR     = 402;  // Ov.p(String)
    public static final int MARK_OVSUB_P_INT  = 411;  // OvSub.p(int) override
    public static final int MARK_IMPL_OVER_D  = 501;  // ImplOver.d (override)
    public static final int MARK_IFACE_D      = 502;  // Iface.d (default, via ImplInherit)
    public static final int MARK_ABS_IMPL_A   = 601;  // AbsImpl.a

    /** The last marker any hierarchy body stamped (ordering / quick sanity). */
    public static volatile int lastFired;

    // Per-method Java hit counters (accumulate across the process; native asserts
    // post-probe DELTAS so no Java-side reset is needed).
    public static volatile int baseMHits;
    public static volatile int derivedMHits;
    public static volatile int aGHits;
    public static volatile int bGHits;
    public static volatile int cGHits;
    public static volatile int superBaseSHits;
    public static volatile int superDerSHits;
    public static volatile int ovPIntHits;
    public static volatile int ovPStrHits;
    public static volatile int ovSubPIntHits;
    public static volatile int implOverDHits;
    public static volatile int ifaceDHits;
    public static volatile int absImplAHits;

    // ── Per-receiver "kind" tags (read off self by the native detour) ─────────
    // A distinct int per concrete runtime type, so the native detour can prove
    // the receiver that reached it (its self oop) is the expected runtime type.
    public static final int KIND_BASE        = 11;
    public static final int KIND_DERIVED     = 12;
    public static final int KIND_A           = 21;
    public static final int KIND_B           = 22;
    public static final int KIND_C           = 23;
    public static final int KIND_SUPER_BASE  = 31;
    public static final int KIND_SUPER_DER   = 32;
    public static final int KIND_OV          = 41;
    public static final int KIND_OVSUB       = 42;
    public static final int KIND_IMPL_OVER   = 51;
    public static final int KIND_IMPL_INH    = 52;
    public static final int KIND_ABS_IMPL    = 61;

    // ==================================================================
    //  1 — SINGLE OVERRIDE:  Base.m()  <-  Derived.m() (override)
    // ==================================================================
    /**
     * The base of the single-override pair.  {@code m(int)} here is a DISTINCT
     * {@code Method*} from {@code Derived.m}; hooking it must fire ONLY for a
     * receiver whose runtime class resolves {@code m} to THIS declaration (a
     * plain {@code Base}).  {@code kind} lets the native detour identify self.
     */
    public static class Base
    {
        public final int kind;

        public Base(final int kind)
        {
            this.kind = kind;
        }

        /** Base.m(int) -> 1000 + x.  Stamps MARK_BASE_M. */
        public int m(final int x)
        {
            lastFired = MARK_BASE_M;
            baseMHits++;
            return 1000 + x;
        }
    }

    /**
     * Overrides {@code m}.  {@code Derived.m} is its own {@code Method*}; hooking
     * it must fire ONLY for a {@code Derived} receiver — never for a {@code Base}
     * call, even though name and descriptor match.
     */
    public static final class Derived extends Base
    {
        public Derived()
        {
            super(KIND_DERIVED);
        }

        /** Derived.m(int) -> 2000 + x.  Stamps MARK_DERIVED_M. */
        @Override
        public int m(final int x)
        {
            lastFired = MARK_DERIVED_M;
            derivedMHits++;
            return 2000 + x;
        }
    }

    // ==================================================================
    //  2 — MULTI-LEVEL CHAIN:  A.g() <- B.g() (override) <- C.g() (override)
    // ==================================================================
    /** Top of the chain; {@code g} declared here. */
    public static class A
    {
        public final int kind;

        public A(final int kind)
        {
            this.kind = kind;
        }

        public A()
        {
            this(KIND_A);
        }

        /** A.g() -> MARK_A_G. */
        public int g()
        {
            lastFired = MARK_A_G;
            aGHits++;
            return MARK_A_G;
        }
    }

    /** Middle of the chain; overrides {@code g}. */
    public static class B extends A
    {
        public B()
        {
            super(KIND_B);
        }

        public B(final int kind)
        {
            super(kind);
        }

        @Override
        public int g()
        {
            lastFired = MARK_B_G;
            bGHits++;
            return MARK_B_G;
        }
    }

    /** Bottom of the chain; overrides {@code g} again. */
    public static final class C extends B
    {
        public C()
        {
            super(KIND_C);
        }

        @Override
        public int g()
        {
            lastFired = MARK_C_G;
            cGHits++;
            return MARK_C_G;
        }
    }

    // ==================================================================
    //  3 — OVERRIDE + SUPER-CALL:  SuperDerived.s() calls super.s()
    // ==================================================================
    /**
     * Base of the super-call pair.  {@code SuperDerived.s} invokes
     * {@code super.s()} — an {@code invokespecial} that resolves to THIS exact
     * {@code Method*}.  So hooking {@code SuperBase.s} fires both for a plain
     * {@code SuperBase} receiver AND for the super-call leg inside
     * {@code SuperDerived.s} — proving an explicit super-call reaches the base
     * Method* the hook owns.
     */
    public static class SuperBase
    {
        public final int kind;

        public SuperBase(final int kind)
        {
            this.kind = kind;
        }

        public SuperBase()
        {
            this(KIND_SUPER_BASE);
        }

        /** SuperBase.s(int) -> 7000 + x.  Stamps MARK_SUPER_BASE_S. */
        public int s(final int x)
        {
            lastFired = MARK_SUPER_BASE_S;
            superBaseSHits++;
            return 7000 + x;
        }
    }

    /** Overrides {@code s} but delegates to {@code super.s()} inside its body. */
    public static final class SuperDerived extends SuperBase
    {
        public SuperDerived()
        {
            super(KIND_SUPER_DER);
        }

        /**
         * SuperDerived.s(int): stamps its OWN marker, then calls super.s(x)
         * (which stamps the base marker and bumps the base counter).  Returns the
         * super result + 1 so the two legs are distinguishable.
         */
        @Override
        public int s(final int x)
        {
            lastFired = MARK_SUPER_DER_S;
            superDerSHits++;
            final int superResult = super.s(x);   // invokespecial SuperBase.s
            return superResult + 1;
        }
    }

    // ==================================================================
    //  4 — OVERRIDE vs OVERLOAD:  p(int) & p(String); OvSub overrides p(int) only
    // ==================================================================
    /**
     * Carries TWO overloads {@code p(int)} and {@code p(String)} (same name,
     * different descriptor).  Hooking {@code Ov.p(I)I} must NEVER fire for a
     * {@code p(String)} call and vice-versa — descriptor disambiguates even on
     * the same receiver.  {@code OvSub} overrides ONLY {@code p(int)}, so
     * {@code p(String)} on an {@code OvSub} still resolves to THIS class's
     * {@code Method*} (inherited) while {@code p(int)} resolves to the override.
     */
    public static class Ov
    {
        public final int kind;

        public Ov(final int kind)
        {
            this.kind = kind;
        }

        public Ov()
        {
            this(KIND_OV);
        }

        /** Ov.p(int) -> 5000 + x.  Stamps MARK_OV_P_INT. */
        public int p(final int x)
        {
            lastFired = MARK_OV_P_INT;
            ovPIntHits++;
            return 5000 + x;
        }

        /** Ov.p(String) -> 6000 + len.  Stamps MARK_OV_P_STR. */
        public int p(final String s)
        {
            final int len = (s == null) ? -1 : s.length();
            lastFired = MARK_OV_P_STR;
            ovPStrHits++;
            return 6000 + len;
        }
    }

    /** Overrides ONLY {@code p(int)}; INHERITS {@code p(String)} unchanged. */
    public static final class OvSub extends Ov
    {
        public OvSub()
        {
            super(KIND_OVSUB);
        }

        /** OvSub.p(int) -> 5500 + x.  Stamps MARK_OVSUB_P_INT. */
        @Override
        public int p(final int x)
        {
            lastFired = MARK_OVSUB_P_INT;
            ovSubPIntHits++;
            return 5500 + x;
        }
        // p(String) is NOT overridden -> inherited Ov.p(String) Method*.
    }

    // ==================================================================
    //  5 — INTERFACE DEFAULT-METHOD override:
    //      Iface.d() default ; ImplOver overrides, ImplInherit inherits
    // ==================================================================
    /**
     * Interface with a DEFAULT method {@code d()}.  {@code ImplOver} OVERRIDES it
     * (its own {@code Method*} on the impl klass); {@code ImplInherit} INHERITS it
     * (the call resolves to the interface's default {@code Method*}).  Hooking the
     * override and hooking the inherited default must fire on disjoint receivers.
     */
    public interface Iface
    {
        /** Interface default; INHERITED by ImplInherit, OVERRIDDEN by ImplOver. */
        int d();
    }

    /**
     * A small abstract-ish carrier giving both impls a {@code kind} field the
     * native detour reads off self.  (Interfaces can't hold instance fields, so
     * the field lives on this shared base.)
     */
    public static class IfaceCarrier
    {
        public final int kind;

        public IfaceCarrier(final int kind)
        {
            this.kind = kind;
        }
    }

    /** Provides the DEFAULT body via a real concrete method so it is hookable. */
    public static class ImplInherit extends IfaceCarrier implements Iface
    {
        public ImplInherit()
        {
            super(KIND_IMPL_INH);
        }

        /**
         * The "inherited default" body.  NOTE: a true interface {@code default}
         * method lives on the interface klass and is awkward to hook by the
         * concrete impl's own {@code _methods}; to keep the hook target a stable,
         * concrete {@code Method*} on a registrable class, the shared default
         * behaviour is provided HERE as an ordinary method that {@code ImplOver}
         * overrides.  Functionally this is the inherited-vs-overridden contrast
         * the module asserts: {@code ImplInherit.d} is one {@code Method*},
         * {@code ImplOver.d} is another, and they never cross-fire.
         */
        @Override
        public int d()
        {
            lastFired = MARK_IFACE_D;
            ifaceDHits++;
            return MARK_IFACE_D;
        }
    }

    /** OVERRIDES the default; its {@code d} is a distinct {@code Method*}. */
    public static final class ImplOver extends ImplInherit
    {
        public ImplOver()
        {
            // ImplInherit's ctor sets kind = KIND_IMPL_INH; we cannot re-assign a
            // final field, so ImplOver is identified by its runtime klass, and
            // its kind is intentionally KIND_IMPL_INH (the native side keys this
            // scenario on the Method* / marker, not on kind).
        }

        @Override
        public int d()
        {
            lastFired = MARK_IMPL_OVER_D;
            implOverDHits++;
            return MARK_IMPL_OVER_D;
        }
    }

    // ==================================================================
    //  6 — ABSTRACT-method implementation dispatch:
    //      AbsBase.a() abstract ; AbsImpl.a() concrete
    // ==================================================================
    /** Abstract base: declares an abstract {@code a()} with NO body. */
    public abstract static class AbsBase
    {
        public final int kind;

        protected AbsBase(final int kind)
        {
            this.kind = kind;
        }

        /** Abstract — no Method* body to hook on AbsBase; only the impl is. */
        public abstract int a();
    }

    /** Concrete implementation of the abstract method. */
    public static final class AbsImpl extends AbsBase
    {
        public AbsImpl()
        {
            super(KIND_ABS_IMPL);
        }

        /** AbsImpl.a() -> MARK_ABS_IMPL_A.  The only concrete a() Method*. */
        @Override
        public int a()
        {
            lastFired = MARK_ABS_IMPL_A;
            absImplAHits++;
            return MARK_ABS_IMPL_A;
        }
    }

    // ── Eagerly-created live receivers (so every nested klass is loaded and the
    //    OOPs exist by the time the native module runs; loadFixtures() only
    //    forName's this top-level class). ───────────────────────────────────
    public static final Base         BASE_OBJ      = new Base(KIND_BASE);
    public static final Derived      DERIVED_OBJ   = new Derived();
    public static final A            A_OBJ         = new A();
    public static final B            B_OBJ         = new B();
    public static final C            C_OBJ         = new C();
    public static final SuperBase    SUPER_BASE_OBJ = new SuperBase();
    public static final SuperDerived SUPER_DER_OBJ = new SuperDerived();
    public static final Ov           OV_OBJ        = new Ov();
    public static final OvSub        OVSUB_OBJ     = new OvSub();
    public static final ImplInherit  IMPL_INH_OBJ  = new ImplInherit();
    public static final ImplOver     IMPL_OVER_OBJ = new ImplOver();
    public static final AbsImpl      ABS_IMPL_OBJ  = new AbsImpl();

    // ── Hook site / driver: each mode calls the methods the native scenario
    //    about to assert will hook.  The interpreter hook fires on real bytecode
    //    dispatch here. ─────────────────────────────────────────────────────
    private static void runMode(final int m)
    {
        switch (m)
        {
            case 1:   // Base.m on a BASE receiver
                BASE_OBJ.m(1);
                break;
            case 2:   // Derived.m on a DERIVED receiver
                DERIVED_OBJ.m(2);
                break;
            case 3:   // A.g / B.g / C.g each on its OWN runtime type
                A_OBJ.g();
                B_OBJ.g();
                C_OBJ.g();
                break;
            case 4:   // SuperDerived.s (calls super.s internally)
                SUPER_DER_OBJ.s(3);
                break;
            case 5:   // Ov.p(int) + Ov.p(String) on an Ov
                OV_OBJ.p(7);
                OV_OBJ.p("abcd");
                break;
            case 6:   // OvSub.p(int) [override] + OvSub.p(String) [inherited]
                OVSUB_OBJ.p(7);
                OVSUB_OBJ.p("abcd");
                break;
            case 7:   // ImplOver.d (override)
                IMPL_OVER_OBJ.d();
                break;
            case 8:   // ImplInherit.d (inherited default body)
                IMPL_INH_OBJ.d();
                break;
            case 9:   // AbsImpl.a (abstract-method impl)
                ABS_IMPL_OBJ.a();
                break;
            case 10:  // Base.m on Base AND Derived.m on Derived, ONE pass
                BASE_OBJ.m(1);
                DERIVED_OBJ.m(2);
                break;
            default:
                break;
        }
    }

    static
    {
        Harness.register(new Harness.Probe()
        {
            @Override
            public boolean pending()
            {
                return OverrideDispatch.go && !OverrideDispatch.done;
            }

            @Override
            public void run()
            {
                OverrideDispatch.probeRuns++;
                runMode(OverrideDispatch.mode);
                OverrideDispatch.done = true;
            }
        });
    }
}

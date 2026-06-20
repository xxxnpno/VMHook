package vmhook.fixtures;

import vmhook.Harness;

/**
 * Fixture for the nested_classes feature (area: classes / klass resolution).
 *
 * The live-JVM authority for vmhook's handling of EVERY Java NESTED-class shape,
 * resolved by its javac-generated internal {@code $}-name through {@link
 * vmhook::find_class}, with the headline contract being the synthetic
 * {@code this$N} outer back-reference of a non-static inner class.
 *
 * SHAPES EXERCISED (each force-loaded so find_class can resolve it — see the
 * force-load note below):
 *
 *   STABLE-name shapes (a fixed Java {@code $}-name identifies them):
 *     - STATIC nested class            {@code NestedClasses$Host$StaticNested}
 *         (no synthetic outer reference; an ordinary class merely living in
 *          another class's namespace),
 *     - non-static INNER class         {@code NestedClasses$Host$Inner}
 *         (javac injects a synthetic {@code this$0} back-reference to the
 *          enclosing {@code Host} instance + a synthetic ctor param),
 *     - a SECOND inner class of Host   {@code NestedClasses$Host$SecondInner}
 *         (distinct klass + its OWN {@code this$0} -> the same Host),
 *     - INNER inside INNER             {@code NestedClasses$Host$Inner$InnerInner}
 *         (its synthetic field is named {@code this$1} and points at the
 *          enclosing {@code Inner} instance — depth shows up in the field NAME),
 *     - STATIC nested inside STATIC nested (deeply nested, A$B$C$D)
 *                                      {@code NestedClasses$Host$StaticNested$DeepNested}
 *         (no synthetic outer ref at any level),
 *     - NESTED INTERFACE               {@code NestedClasses$NestedIface},
 *     - NESTED ENUM                    {@code NestedClasses$NestedEnum},
 *     - NESTED ANNOTATION              {@code NestedClasses$NestedAnno},
 *     - GENERIC nested class (erased)  {@code NestedClasses$GenericBox}
 *         (the type parameter vanishes; one declared field of erased type Object).
 *
 *   UNSTABLE-name shapes (the {@code $N} ordinal is assigned by source order and
 *   can shift on a recompile, so NO fixed Java name identifies them — they are
 *   resolved by reading the klass off a published INSTANCE oop, never by name):
 *     - ANONYMOUS class                {@code NestedClasses$1} (a Runnable),
 *     - LOCAL class                    {@code NestedClasses$1LocalCounter}
 *         (declared inside an instance method; carries a {@code this$0}).
 *
 * The enclosing {@code Host} is itself a static nested class of this fixture
 * ({@code NestedClasses$Host}) so it can be force-instantiated without needing a
 * {@code NestedClasses} instance, yet still yields the 3-level internal name
 * {@code NestedClasses$Host} and is the enclosing instance an Inner points back
 * at.  The fixture also keeps a {@code SELF} instance of {@code NestedClasses}
 * itself so the anonymous / local classes (declared in an instance method) get a
 * real {@code this$0} to the fixture instance.
 *
 * Mirrors the legacy {@code test_nested_classes} (vmhook/src/example.cpp) and
 * {@code vmhook.NestedHost} value-for-value so the documented composite holds:
 *
 *     outerField (7)  +  Inner.innerValue (99)  ==  106
 *
 * which the native module asserts both by reading the synthetic {@code this$0}
 * slot of the Inner instance back to the Host (identity proof) and — the robust,
 * JDK-independent proof — by driving {@code Inner.outerPlusInner()} through real
 * bytecode here in mode 1 and publishing the result for the module to check.
 *
 * FORCE-LOAD: every {@code $}-nested klass the native side resolves is loaded
 * here — the STABLE shapes by a force-instantiation (a {@code new}) or a class
 * literal anchor in {@code <clinit>}, the UNSTABLE shapes by holding a live
 * instance — because {@code Main.loadFixtures} only {@code Class.forName}s the
 * TOP-LEVEL fixture, never the {@code $}-nested klasses; without this the nested
 * klasses would not yet be loaded and find_class would miss.  Each instance the
 * native side inspects also publishes its {@code System.identityHashCode} so the
 * C++ checks are exact, never "non-null and hope".
 *
 * Java 8 syntax only (anonymous + local classes, no lambdas / var / records).
 */
public final class NestedClasses
{
    // ── go / done / mode handshake (native sets mode + clears done first) ────
    /** Native sets this true to request the action; clears it after. */
    public static volatile boolean go;

    /** The action sets this true when it has run; native polls it. */
    public static volatile boolean done;

    /**
     * Scenario selector.  The native module sets this BEFORE raising {@code go}.
     *   1 = drive Inner.outerPlusInner() + StaticNested.doubled() through REAL
     *       bytecode and publish the results (the JDK-independent composite
     *       proof) — also fires any interpreter hook on a genuine dispatch.
     *   2 = drive the deeply-nested / inner-in-inner / nested-enum methods
     *       through REAL bytecode and publish their results.
     *   3 = drive the static-in-inner / iface-member / host-enum / anon-of-iface
     *       methods through REAL bytecode and publish their results.
     */
    public static volatile int mode;

    // ── Deterministic constants the native side mirrors (== NestedHost) ──────
    public static final int OUTER_FIELD_INIT   = 7;     // Host.outerField
    public static final int STATIC_NESTED_VAL  = 42;    // StaticNested.value
    public static final int STATIC_NESTED_DBL  = 84;    // == value * 2
    public static final int INNER_VALUE_INIT   = 99;    // Inner.innerValue
    public static final int OUTER_PLUS_INNER   = 106;   // 7 + 99 (documented)

    // ── New deterministic constants for the extra shapes ─────────────────────
    public static final int SECOND_INNER_INIT  = 55;    // SecondInner.secondValue
    public static final int INNER_INNER_INIT   = 11;    // Inner$InnerInner.innerInnerValue
    public static final int DEEP_NESTED_INIT   = 1000;  // StaticNested$DeepNested.deepValue
    public static final int GENERIC_BOX_INIT   = 321;   // GenericBox.boxed (erased Object holding Integer)
    public static final int LOCAL_INIT         = 7777;  // local class field
    public static final int NESTED_ENUM_RANK   = 3;     // NestedEnum.GAMMA.rank() == ordinal()+1
    public static final int ENUM_STATIC_INIT     = 13;  // NestedEnum$EnumStatic.enumStaticValue
    public static final int IFACE_MEMBER_INIT    = 70;  // NestedIface$IfaceMember.ifaceMemberValue
    public static final int HOST_COLOR_CODE      = 11;  // Host$HostColor.GREEN.code() == ordinal()+10

    // ── The outer holder.  STATIC nested so it needs no NestedClasses instance,
    //    yet still produces the 3-level internal name NestedClasses$Host and is
    //    the enclosing instance whose `this$0` an Inner points back at. ────────
    public static class Host
    {
        /** Read by the native side at Host depth-0; the Inner reads it too. */
        public int outerField = OUTER_FIELD_INIT;

        // ---- STATIC nested class: NO synthetic outer reference --------------
        public static class StaticNested
        {
            public int value;

            public StaticNested(final int v)
            {
                this.value = v;
            }

            /** No-arg instance method (interpreter call-gate target). */
            public int doubled()
            {
                return this.value * 2;
            }

            // ---- STATIC nested INSIDE a static nested (deeply nested) --------
            //   Internal name: NestedClasses$Host$StaticNested$DeepNested (4 levels).
            //   No synthetic outer reference at ANY level (both are static).
            public static class DeepNested
            {
                public int deepValue = DEEP_NESTED_INIT;

                public int deepDoubled()
                {
                    return this.deepValue * 2;
                }
            }
        }

        // ---- non-static INNER class: javac injects a synthetic this$0 -------
        public class Inner
        {
            public int innerValue = INNER_VALUE_INIT;

            /**
             * Reads the enclosing Host.outerField THROUGH the synthetic
             * {@code this$0} reference, so a correct read proves the back-link.
             * Returns 7 + 99 == 106.
             */
            public int outerPlusInner()
            {
                return outerField + this.innerValue;
            }

            // ---- INNER inside INNER: the synthetic field is named this$1 -----
            //   and points at the enclosing Inner instance (not the Host).  Its
            //   internal name is NestedClasses$Host$Inner$InnerInner.
            public class InnerInner
            {
                public int innerInnerValue = INNER_INNER_INIT;

                /**
                 * Reads through BOTH synthetic links: this$1 -> Inner, whose
                 * this$0 -> Host.  Returns Host.outerField + Inner.innerValue +
                 * innerInnerValue == 7 + 99 + 11 == 117.
                 */
                public int sumThroughBothOuters()
                {
                    return outerField + innerValue + this.innerInnerValue;
                }
            }

            /** Factory so an InnerInner is built against THIS Inner (wires this$1). */
            public InnerInner newInnerInner()
            {
                return new InnerInner();
            }
        }

        // ---- a SECOND non-static inner class of the SAME outer Host ----------
        //   Distinct klass (NestedClasses$Host$SecondInner) with its OWN this$0
        //   back-reference to the enclosing Host — proves multiple inners of one
        //   outer resolve to distinct klasses, each with an independent this$0.
        public class SecondInner
        {
            public int secondValue = SECOND_INNER_INIT;

            public int outerPlusSecond()
            {
                return outerField + this.secondValue;
            }
        }

        /** Factory so an Inner is built against THIS Host (wires this$0). */
        public Inner newInner()
        {
            return new Inner();
        }

        /** Factory so a SecondInner is built against THIS Host (wires this$0). */
        public SecondInner newSecondInner()
        {
            return new SecondInner();
        }

        // ---- ENUM nested INSIDE the Host class (4-segment enum name) ---------
        //   Internal name: NestedClasses$Host$HostColor.  An enum nested one level
        //   deeper than NestedEnum: its super is still java.lang.Enum, it still
        //   carries the synthetic values()/valueOf()/$VALUES, and a `$`-nested enum
        //   resolves by its multi-segment $-name exactly like any other shape.  A
        //   static nested type (an enum is implicitly static), so NO this$N.
        public enum HostColor
        {
            RED, GREEN, BLUE;

            public int code()
            {
                return this.ordinal() + 10;
            }
        }
    }

    // ── Nested INTERFACE (an InstanceKlass under the hood). ──────────────────
    //   Internal name: NestedClasses$NestedIface.
    public interface NestedIface
    {
        int IFACE_CONST = 17;          // a static final field on the interface

        int ifaceOp(int x);            // abstract method

        default int ifaceDefault()     // default method
        {
            return IFACE_CONST;
        }

        // ---- a NESTED CLASS of an INTERFACE (implicitly static -> no this$0) --
        //   Internal name: NestedClasses$NestedIface$IfaceMember.  A member type
        //   of an interface is implicitly PUBLIC STATIC, so even though it is a
        //   class nested inside an interface it carries NO synthetic outer
        //   reference.  Proves the `$`-name resolution + field/method access work
        //   for a class whose immediate encloser is an interface (not a class).
        class IfaceMember
        {
            public int ifaceMemberValue = IFACE_MEMBER_INIT;

            public int memberPlusConst()
            {
                return this.ifaceMemberValue + IFACE_CONST;
            }
        }
    }

    // ── Nested ENUM (super is java.lang.Enum; ENUM+FINAL access bits). ────────
    //   Internal name: NestedClasses$NestedEnum.
    public enum NestedEnum
    {
        ALPHA, BETA, GAMMA;

        public int rank()
        {
            return this.ordinal() + 1;
        }

        // ---- a STATIC nested class whose immediate encloser is an ENUM -------
        //   Internal name: NestedClasses$NestedEnum$EnumStatic.  A static nested
        //   class can be declared inside an enum (an enum is implicitly a class);
        //   it carries NO synthetic outer reference.  Proves the `$`-name path and
        //   field/method access work when the enclosing type is an enum, not a
        //   plain class — a 3-segment $-name whose middle segment is an enum.
        public static class EnumStatic
        {
            public int enumStaticValue = ENUM_STATIC_INIT;

            public int tripled()
            {
                return this.enumStaticValue * 3;
            }
        }
    }

    // ── Nested ANNOTATION (an interface; INTERFACE+ANNOTATION access bits). ───
    //   Internal name: NestedClasses$NestedAnno.
    public @interface NestedAnno
    {
        String label() default "n";
        int weight() default 0;
    }

    // ── Generic (erased) nested class: the type parameter vanishes. ──────────
    //   Internal name: NestedClasses$GenericBox.  The declared field is of the
    //   erased type java.lang.Object; we box an Integer so the native side reads
    //   a reference field and (Java-side) the unboxed int is published.
    public static final class GenericBox<T>
    {
        public T boxed;

        public GenericBox(final T v)
        {
            this.boxed = v;
        }

        public T get()
        {
            return this.boxed;
        }
    }

    // ── An instance of the fixture itself, so the anonymous + local classes
    //    declared in instance methods below get a real this$0 to a NestedClasses
    //    instance (and so the native side can read it back). ──────────────────
    public static final NestedClasses SELF = new NestedClasses();

    /** A field on the fixture instance the anonymous/local classes read via this$0. */
    public int selfMarker = 4242;

    /**
     * Builds an ANONYMOUS class instance (a Runnable).  javac names it
     * NestedClasses$1 (or another ordinal) and injects a this$0 -> this
     * NestedClasses instance.  Returned as Object so the native side resolves its
     * klass from the live oop, never by the unstable $N name.
     */
    public Object makeAnonymous()
    {
        return new Runnable()
        {
            @Override
            public void run()
            {
                // Touch the enclosing field through this$0 so the synthetic
                // reference is genuinely used (and not elided).
                NestedClasses.this.selfMarker += 0;
            }
        };
    }

    /**
     * Builds a SECOND anonymous class instance, this one implementing the NESTED
     * interface {@link NestedIface} (not Runnable).  javac names it with another
     * unstable ordinal (NestedClasses$2 or similar) and its DIRECT super is
     * java.lang.Object while it implements NestedClasses$NestedIface — so the
     * native side resolves the klass from the live oop and asserts the
     * Outer$&lt;ordinal&gt; shape, distinct from the Runnable anonymous.  Carries a
     * this$0 back to this NestedClasses instance.
     */
    public Object makeAnonymousIface()
    {
        return new NestedIface()
        {
            @Override
            public int ifaceOp(final int x)
            {
                return x + NestedClasses.this.selfMarker;
            }
        };
    }

    /**
     * Builds a LOCAL class instance.  javac names it NestedClasses$1LocalCounter
     * (the $1 ordinal is unstable) and injects a this$0 -> this NestedClasses
     * instance.  Returned as Object for the same reason as the anonymous case.
     */
    public Object makeLocal()
    {
        class LocalCounter
        {
            int localValue = LOCAL_INIT;

            int readMarkerPlusLocal()
            {
                return NestedClasses.this.selfMarker + this.localValue;
            }
        }
        return new LocalCounter();
    }

    // ── Force-instantiated STABLE singletons (so the $-nested klasses are LOADED
    //    and the published identities match exactly the OOPs the module decodes).
    /** The enclosing Host instance; native reads outerField off it. */
    public static final Host host = new Host();

    /** A StaticNested instance; native reads value + calls doubled(). */
    public static final Host.StaticNested staticNested = new Host.StaticNested(STATIC_NESTED_VAL);

    /** An Inner instance bound to {@code host}; native reads innerValue + this$0. */
    public static final Host.Inner innerInst = host.newInner();

    /** A SECOND inner of the same host; native reads its OWN this$0 -> host. */
    public static final Host.SecondInner secondInnerInst = host.newSecondInner();

    /** An InnerInner bound to {@code innerInst}; native reads this$1 -> innerInst. */
    public static final Host.Inner.InnerInner innerInnerInst = innerInst.newInnerInner();

    /** A deeply-nested (4-level, all static) instance. */
    public static final Host.StaticNested.DeepNested deepNestedInst = new Host.StaticNested.DeepNested();

    /** A generic (erased) box holding an Integer. */
    public static final GenericBox<Integer> genericBoxInst = new GenericBox<Integer>(Integer.valueOf(GENERIC_BOX_INIT));

    /** A STATIC nested class declared INSIDE the nested ENUM (no this$N). */
    public static final NestedEnum.EnumStatic enumStaticInst = new NestedEnum.EnumStatic();

    /** A member class of the nested INTERFACE (implicitly static; no this$0). */
    public static final NestedIface.IfaceMember ifaceMemberInst = new NestedIface.IfaceMember();

    /** A live anonymous-class instance (klass resolved from the oop, not by name). */
    public static final Object anonymousInst = SELF.makeAnonymous();

    /** A second anonymous instance implementing the nested interface (this$0 -> SELF). */
    public static final Object anonymousIfaceInst = SELF.makeAnonymousIface();

    /** A live local-class instance (klass resolved from the oop, not by name). */
    public static final Object localInst = SELF.makeLocal();

    // ── Force-load anchors for the no-instance shapes (interface / annotation):
    //    referencing the class literal in <clinit> loads the klass so find_class
    //    can resolve it (the harness loader skips $-nested classes). ───────────
    static final Class<?> ANCHOR_IFACE = NestedIface.class;
    static final Class<?> ANCHOR_ENUM  = NestedEnum.class;
    static final Class<?> ANCHOR_ANNO  = NestedAnno.class;
    static final Class<?> ANCHOR_HOST_COLOR = Host.HostColor.class;

    // ── Identity publication (so the synthetic this$N checks are exact) ───────
    public static volatile int hostIdentity;
    public static volatile int staticNestedIdentity;
    public static volatile int innerIdentity;
    public static volatile int secondInnerIdentity;
    public static volatile int innerInnerIdentity;
    public static volatile int deepNestedIdentity;
    public static volatile int genericBoxIdentity;
    public static volatile int selfIdentity;
    public static volatile int anonymousIdentity;
    public static volatile int anonymousIfaceIdentity;
    public static volatile int localIdentity;
    public static volatile int enumStaticIdentity;
    public static volatile int ifaceMemberIdentity;

    // ── Probe-published composite results (the JDK-independent proofs) ───────
    /** Set by mode 1 to innerInst.outerPlusInner(); native asserts == 106. */
    public static volatile int outerPlusInnerValue;
    /** Set by mode 1 to staticNested.doubled(); native asserts == 84. */
    public static volatile int doubledValue;
    /** Set by mode 1 to secondInnerInst.outerPlusSecond(); native asserts == 62. */
    public static volatile int outerPlusSecondValue;

    /** Set by mode 2 to innerInnerInst.sumThroughBothOuters(); native asserts == 117. */
    public static volatile int innerInnerSumValue;
    /** Set by mode 2 to deepNestedInst.deepDoubled(); native asserts == 2000. */
    public static volatile int deepDoubledValue;
    /** Set by mode 2 to NestedEnum.GAMMA.rank(); native asserts == 3. */
    public static volatile int nestedEnumRankValue;
    /** Set by mode 2 to the local class's readMarkerPlusLocal(); native asserts == 4242+7777. */
    public static volatile int localReadbackValue;
    /** Set by mode 2 to genericBoxInst.get() unboxed; native asserts == 321. */
    public static volatile int genericBoxUnboxedValue;

    /** Set by mode 3 to enumStaticInst.tripled(); native asserts == 39. */
    public static volatile int enumStaticTripledValue;
    /** Set by mode 3 to ifaceMemberInst.memberPlusConst(); native asserts == 87. */
    public static volatile int ifaceMemberPlusConstValue;
    /** Set by mode 3 to Host.HostColor.GREEN.code(); native asserts == 11. */
    public static volatile int hostColorCodeValue;
    /** Set by mode 3 to the anon-iface instance's ifaceOp(8); native asserts == 4250. */
    public static volatile int anonIfaceOpValue;

    static
    {
        // Publish identities once at load (also valid before any probe runs).
        hostIdentity         = System.identityHashCode(host);
        staticNestedIdentity = System.identityHashCode(staticNested);
        innerIdentity        = System.identityHashCode(innerInst);
        secondInnerIdentity  = System.identityHashCode(secondInnerInst);
        innerInnerIdentity   = System.identityHashCode(innerInnerInst);
        deepNestedIdentity   = System.identityHashCode(deepNestedInst);
        genericBoxIdentity   = System.identityHashCode(genericBoxInst);
        selfIdentity         = System.identityHashCode(SELF);
        anonymousIdentity    = System.identityHashCode(anonymousInst);
        anonymousIfaceIdentity = System.identityHashCode(anonymousIfaceInst);
        localIdentity        = System.identityHashCode(localInst);
        enumStaticIdentity   = System.identityHashCode(enumStaticInst);
        ifaceMemberIdentity  = System.identityHashCode(ifaceMemberInst);

        Harness.register(new Harness.Probe()
        {
            @Override
            public boolean pending()
            {
                return NestedClasses.go && !NestedClasses.done;
            }

            @Override
            public void run()
            {
                if (NestedClasses.mode == 1)
                {
                    // Real bytecode dispatch: reads outerField via the synthetic
                    // this$0 (Inner / SecondInner) and value (StaticNested).
                    // These are the authoritative, JDK-independent composite proofs.
                    NestedClasses.outerPlusInnerValue  = NestedClasses.innerInst.outerPlusInner();
                    NestedClasses.doubledValue         = NestedClasses.staticNested.doubled();
                    NestedClasses.outerPlusSecondValue = NestedClasses.secondInnerInst.outerPlusSecond();
                }
                else if (NestedClasses.mode == 2)
                {
                    // Deeper-shape composites through real bytecode.
                    NestedClasses.innerInnerSumValue   = NestedClasses.innerInnerInst.sumThroughBothOuters();
                    NestedClasses.deepDoubledValue     = NestedClasses.deepNestedInst.deepDoubled();
                    NestedClasses.nestedEnumRankValue  = NestedEnum.GAMMA.rank();
                    NestedClasses.genericBoxUnboxedValue = NestedClasses.genericBoxInst.get().intValue();

                    // The local class is package-invisible by name here, but its
                    // method is reachable via its declared instance; reflectively
                    // invoke readMarkerPlusLocal() so the composite is published.
                    try
                    {
                        final java.lang.reflect.Method m =
                            NestedClasses.localInst.getClass().getDeclaredMethod("readMarkerPlusLocal");
                        m.setAccessible(true);
                        NestedClasses.localReadbackValue =
                            ((Integer) m.invoke(NestedClasses.localInst)).intValue();
                    }
                    catch (final Throwable t)
                    {
                        NestedClasses.localReadbackValue = -1;
                    }
                }
                else if (NestedClasses.mode == 3)
                {
                    // Newest-shape composites through real bytecode:
                    //   - a STATIC nested class declared inside the nested ENUM,
                    //   - a member class of the nested INTERFACE,
                    //   - an enum nested inside the Host class,
                    //   - the anonymous class implementing the nested interface.
                    NestedClasses.enumStaticTripledValue =
                        NestedClasses.enumStaticInst.tripled();
                    NestedClasses.ifaceMemberPlusConstValue =
                        NestedClasses.ifaceMemberInst.memberPlusConst();
                    NestedClasses.hostColorCodeValue = Host.HostColor.GREEN.code();
                    NestedClasses.anonIfaceOpValue =
                        ((NestedIface) NestedClasses.anonymousIfaceInst).ifaceOp(8);
                }
                NestedClasses.done = true;
            }
        });
    }
}

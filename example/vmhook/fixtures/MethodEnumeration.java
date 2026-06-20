package vmhook.fixtures;

import vmhook.Harness;

/**
 * Fixture for the method_enumeration feature (area: methods).
 *
 * The native module exercises the four public introspection entry points:
 *   - vmhook::get_class_methods<T>()                 (by registered wrapper type)
 *   - vmhook::get_class_methods("vmhook/fixtures/..")(by internal class name)
 *   - vmhook::find_methods_by_signature<T>(desc)     (all names for a descriptor)
 *   - vmhook::hook_by_signature<T>(desc, detour)     (install on the UNIQUE match;
 *                                                      REFUSE when 2+ methods share)
 *
 * All four walk InstanceKlass::_methods directly (no JNI).  That array holds
 * EVERY method this class DECLARES — including the synthetic constructor
 * {@code <init>} and the static initializer {@code <clinit>} — but NOT methods
 * inherited from java.lang.Object.  This fixture is therefore shaped so that the
 * resulting (name, descriptor) set is known EXACTLY and contains, by design:
 *
 *   descriptor (J)J                 -> exactly ONE method  (idLong)   == hookable
 *   descriptor (I)I                 -> THREE methods (idInt, addInt, sId) == refuse
 *   descriptor ()V                  -> FOUR methods (<init>, <clinit>, noop, tick)
 *                                       == refuse  (the synthetic members collide
 *                                          with the real void no-arg methods)
 *   plus several genuinely-unique descriptors covering reference args, arrays,
 *   the J/D two-slot boundary, primitive vs reference returns, and a static
 *   multi-slot method.
 *
 * The two-methods-sharing-a-descriptor requirement of the contract is satisfied
 * three times over (by (I)I and by ()V), which lets the native side prove the
 * refuse-policy on BOTH an application-only collision (I)I and a
 * synthetic-member collision ()V.
 *
 * The unique-descriptor install/fire target is {@code idLong(long)} -> (J)J.
 * The probe's run() calls idLong on a real bytecode dispatch so the
 * signature-installed interpreter hook fires.
 *
 * Java 8 syntax only (no var / records / switch-expr / text-blocks); java.* +
 * vmhook.Harness only.
 */
public final class MethodEnumeration
{
    /** Native sets this true to request the action; clears it after. */
    public static volatile boolean go;

    /** The action sets this true when it has run; native polls it. */
    public static volatile boolean done;

    /**
     * Scenario selector.  The native module sets this (and clears the latched
     * {@code done}) on the rising edge of {@code go} so a single probe cycle
     * drives exactly the call it is about to assert on.
     *   1 = call idLong(IDLONG_ARG)  (drives the hook_by_signature<(J)J> detour)
     *   2 = call idInt(IDINT_ARG)    (a method whose descriptor (I)I is SHARED;
     *                                  used to confirm a refused signature-hook
     *                                  never installed -> detour must NOT fire)
     *   3..7 = dispatch one Overloads method ONCE each, so its i2i interpreter
     *          entry is RESOLVED/LINKED before the native side installs a
     *          hook_by_signature on it.  hook_by_signature patches the method's
     *          i2i stub; an as-yet-uncalled method still points at the lazy
     *          unresolved-link stub, so the install would fail.  Calling the
     *          target once links it; the install then genuinely succeeds.  See
     *          the Overloads doc below for the per-mode mapping.
     *   8  = link SameNameOverloads.pick(long) -> (J)I (then native scoped_hook).
     *   10 = link + drive Sub.shared(int) -> (I)I override (then scoped_hook).
     *   11 = link + drive ArraySigs.oneD(long[]) -> ([J)J (then scoped_hook fire).
     *   12 = link + drive Tiny.only() -> ()V (then scoped_hook fire).
     */
    public static volatile int mode;

    // ---- Observable results (allow-through proofs) ------------------------

    /** Last value the original idLong() body returned. */
    public static volatile long lastIdLong;

    /** Last value the original idInt() body returned. */
    public static volatile int lastIdInt;

    /** Seed read by the detour to confirm `self` is the right receiver. */
    private int seed = SEED;

    // ---- Constants mirrored on the native side ----------------------------

    public static final int  SEED        = 7;
    public static final long IDLONG_ARG  = 0x0102030405060708L;
    public static final int  IDINT_ARG   = 1234;

    // =======================================================================
    // The declared method set.  Keep this list and the descriptors in lockstep
    // with the native module's EXPECTED table.  Descriptors (verified via
    // `javap -s`) are noted next to each method.
    // =======================================================================

    /** (I)I — SHARES its descriptor with addInt and the static sId. */
    public int idInt(final int x)
    {
        return x;
    }

    /** (I)I — shares (I)I. */
    public int addInt(final int x)
    {
        return x + this.seed;
    }

    /** (J)J — UNIQUE.  This is the hook_by_signature install/fire target. */
    public long idLong(final long x)
    {
        return x;
    }

    /** (Ljava/lang/String;)I — unique (reference argument). */
    public int strLen(final String s)
    {
        return (s == null) ? -1 : s.length();
    }

    /** ([I)I — unique (array argument). */
    public int sumArr(final int[] a)
    {
        int total = 0;
        if (a != null)
        {
            for (int v : a)
            {
                total += v;
            }
        }
        return total;
    }

    /** (IJD)D — unique (int + long + double: spans the J/D two-slot boundary). */
    public double mix(final int a, final long b, final double c)
    {
        return a + b + c;
    }

    /** ()V — SHARES its descriptor with tick, <init> and <clinit>. */
    public void noop()
    {
    }

    /** ()V — shares ()V. */
    public void tick()
    {
        this.seed++;
    }

    /** ()Z — unique (boolean return). */
    public boolean flag()
    {
        return this.seed > 0;
    }

    /** ()Ljava/lang/Object; — unique (reference return). */
    public Object makeObj()
    {
        return new Object();
    }

    /** (I)I — static; shares (I)I with idInt/addInt (descriptor ignores static-ness). */
    public static int sId(final int x)
    {
        return x;
    }

    /** (JD)J — unique (static, multi-slot: long + double). */
    public static long sWide(final long a, final double b)
    {
        return a + (long) b;
    }

    // =======================================================================
    // Nested class for the PER-CLASS, descriptor-UNIQUE scoping proof.
    //
    // Internal name: vmhook/fixtures/MethodEnumeration$Overloads.
    //
    // The point of method_enumeration's signature API is that resolution is
    // SCOPED to one klass: a descriptor that is shared (or absent) on the
    // top-level MethodEnumeration can be UNIQUE here, and hooking by signature
    // on this klass must pick THIS klass's lone match, never reach across to
    // the enclosing class's collisions.  The descriptors below are chosen so:
    //
    //   (II)I  -> UNIQUE here (pickII)        ; ABSENT on MethodEnumeration
    //   (J)I   -> UNIQUE here (pickJI)        ; ABSENT on MethodEnumeration
    //   (IJ)I  -> UNIQUE here (pickIJI)       ; ABSENT on MethodEnumeration
    //   (D)V   -> UNIQUE here (soloDV)        ; ABSENT on MethodEnumeration
    //   ()J    -> UNIQUE here (static sSolo)  ; ABSENT on MethodEnumeration
    //   (I)I   -> UNIQUE here (just idI)      ; 3-WAY SHARED on MethodEnumeration
    //
    // The (I)I row is the sharpest cross-class invariant: the SAME descriptor
    // is unique on this klass but a 3-way collision on the enclosing one, so
    // find_methods_by_signature must answer differently per <T>.
    //
    // hook_by_signature patches a method's i2i interpreter stub, which only
    // exists once the method has been LINKED (called at least once).  So each
    // install-target is dispatched once via a runXxx() driver below (modes
    // 3..7) BEFORE the native side installs on it; the install then succeeds.
    // =======================================================================
    public static final class Overloads
    {
        /** (II)I — UNIQUE on Overloads; (II)I is absent on MethodEnumeration. */
        public int pickII(final int a, final int b)
        {
            return a + b;
        }

        /** (J)I — UNIQUE on Overloads; (J)I is absent on MethodEnumeration. */
        public int pickJI(final long a)
        {
            return (int) a;
        }

        /** (IJ)I — UNIQUE on Overloads (int + long spans two slots). */
        public int pickIJI(final int a, final long b)
        {
            return a + (int) b;
        }

        /** (D)V — UNIQUE on Overloads; ()V here would collide, (D)V does not. */
        public void soloDV(final double d)
        {
            lastSoloDV = d;
        }

        /** ()J — UNIQUE on Overloads; static, no-arg, long return. */
        public static long sSolo()
        {
            lastSSolo = 99L;
            return lastSSolo;
        }

        /**
         * (I)I — UNIQUE on Overloads, but the SAME descriptor is a 3-way
         * collision on the enclosing MethodEnumeration (idInt/addInt/sId).
         */
        public int idI(final int x)
        {
            return x;
        }
    }

    /** Last argument the Overloads.soloDV body saw (allow-through proof). */
    public static volatile double lastSoloDV;

    /** Last value the Overloads.sSolo body returned (allow-through proof). */
    public static volatile long lastSSolo;

    // =======================================================================
    // FEW-METHODS shape.  The whole declared set is { only()V, <init>()V } and
    // nothing else: NO static fields and NO static block, so this klass has NO
    // <clinit> at all.  Contrasts the enclosing MethodEnumeration (a static {}
    // block + static-field initializers GUARANTEE its <clinit>), letting the
    // native side assert get_class_methods reports <clinit> ABSENT here and
    // PRESENT there -- proving the enumeration distinguishes the two, not that
    // <clinit> is universal.  Internal name: ...MethodEnumeration$Tiny.
    // =======================================================================
    public static final class Tiny
    {
        /** ()V -- the single declared user method (drives a scoped_hook fire). */
        public void only()
        {
            lastTinyOnly++;
        }
    }

    /** Counts Tiny.only() dispatches (allow-through proof for mode 12). */
    public static volatile int lastTinyOnly;

    // =======================================================================
    // TRUE SAME-NAME OVERLOADS: one method name, FOUR distinct descriptors.
    // get_class_methods must list all four under the single name `pick`, and
    // find_methods_by_signature must answer per-descriptor (each of the four is
    // UNIQUE here), while a name lookup is descriptor-agnostic.  This is the
    // overload axis the enclosing class lacks (its collisions are across
    // DIFFERENT names sharing a descriptor; here it is ONE name across
    // descriptors).  Internal name: ...MethodEnumeration$SameNameOverloads.
    // =======================================================================
    public static final class SameNameOverloads
    {
        /** pick (I)I */
        public int pick(final int a)
        {
            return a;
        }

        /** pick (J)I -- same name, distinct descriptor. */
        public int pick(final long a)
        {
            return (int) a;
        }

        /** pick (II)I -- same name, distinct descriptor. */
        public int pick(final int a, final int b)
        {
            return a + b;
        }

        /** pick (Ljava/lang/String;)I -- same name, reference arg. */
        public int pick(final String s)
        {
            return (s == null) ? -1 : s.length();
        }
    }

    // =======================================================================
    // INHERITED-vs-DECLARED.  Sub extends Base, OVERRIDES shared(I)I, and adds
    // subOnly()V; Base also declares baseOnly()V.  get_class_methods<Sub> must
    // list ONLY Sub's declared methods (shared + subOnly + <init>) and NEVER the
    // inherited baseOnly -- the documented "declared, not resolved" scope.  The
    // override has the SAME descriptor as the parent method, so javac emits no
    // bridge: Sub.shared appears exactly once.  Internal names:
    // ...MethodEnumeration$Base and ...MethodEnumeration$Sub.
    // =======================================================================
    public static class Base
    {
        /** (I)I -- overridden by Sub. */
        public int shared(final int x)
        {
            return x;
        }

        /** ()V -- declared on Base only; must NOT appear in Sub's enumeration. */
        public void baseOnly()
        {
        }
    }

    public static final class Sub extends Base
    {
        /** (I)I -- override (same descriptor -> no bridge). */
        @Override
        public int shared(final int x)
        {
            lastSubShared = x;
            return x + 1;
        }

        /** ()V -- declared only on Sub. */
        public void subOnly()
        {
        }
    }

    /** Last arg Sub.shared saw (allow-through proof for mode 10). */
    public static volatile int lastSubShared;

    // =======================================================================
    // INTERFACE shape: an abstract method, a default method, and a static
    // method ALL live in _methods.  get_class_methods must enumerate all three;
    // an interface has NO <init>.  hook_by_signature on the ABSTRACT req(I)I is
    // a resolution-only probe on the native side (an abstract method has no body
    // to dispatch, so the native side never drives it -- it only proves the
    // descriptor is uniquely RESOLVED).  Internal name:
    // ...MethodEnumeration$Iface.
    // =======================================================================
    public interface Iface
    {
        /** (I)I -- abstract (no body). */
        int req(int x);

        /** (I)I -- default method (shares (I)I with req and stat). */
        default int def(final int x)
        {
            return x + 1;
        }

        /** (I)I -- static interface method (shares (I)I). */
        static int stat(final int x)
        {
            return x + 2;
        }
    }

    // =======================================================================
    // ABSTRACT CLASS: an abstract method + a concrete method + the synthetic
    // <init>.  abstractOp(I)I is abstract (in _methods, no body); concreteOp(I)I
    // is real.  uniqueAbs(D)D is a genuinely-UNIQUE descriptor on this klass for
    // a no-collision find/refuse contrast.  Internal name:
    // ...MethodEnumeration$AbstractShape.
    // =======================================================================
    public abstract static class AbstractShape
    {
        /** (I)I -- abstract. */
        public abstract int abstractOp(int x);

        /** (I)I -- concrete (shares (I)I with abstractOp on this klass). */
        public int concreteOp(final int x)
        {
            return x;
        }

        /** (D)D -- UNIQUE on AbstractShape. */
        public double uniqueAbs(final double d)
        {
            return d;
        }
    }

    // =======================================================================
    // ARRAY-TYPED SIGNATURES, scoped to their own klass so each is UNIQUE here
    // (the enclosing class only has the single ([I)I).  oneD([J)J is the
    // drive+fire target (a long-array arg + long return through a scoped_hook).
    // Internal name: ...MethodEnumeration$ArraySigs.
    // =======================================================================
    public static final class ArraySigs
    {
        /** ([J)J -- long-array arg, long return (drive+fire target, mode 11). */
        public long oneD(final long[] a)
        {
            long total = 0;
            if (a != null)
            {
                for (long v : a)
                {
                    total += v;
                }
            }
            lastOneD = total;
            return total;
        }

        /** ([[I)I -- 2-D int array. */
        public int twoD(final int[][] a)
        {
            return (a == null) ? -1 : a.length;
        }

        /** ([Ljava/lang/String;)I -- reference array. */
        public int objArr(final String[] a)
        {
            return (a == null) ? -1 : a.length;
        }

        /** ()[I -- array RETURN (the '[' lives in the return slot). */
        public int[] retArr()
        {
            return new int[] { 1, 2, 3 };
        }
    }

    /** Last total ArraySigs.oneD summed (allow-through proof for mode 11). */
    public static volatile long lastOneD;

    // ---- Probe dispatch ---------------------------------------------------

    private static void runIdLong()
    {
        final MethodEnumeration obj = new MethodEnumeration();
        // Real bytecode dispatch through idLong() so a (J)J signature hook fires.
        lastIdLong = obj.idLong(IDLONG_ARG);
    }

    private static void runIdInt()
    {
        final MethodEnumeration obj = new MethodEnumeration();
        // idInt's descriptor (I)I is shared, so hook_by_signature<(I)I> must have
        // REFUSED; calling it proves the detour stays silent (no install).
        lastIdInt = obj.idInt(IDINT_ARG);
    }

    // Each driver dispatches ONE Overloads method exactly once so its i2i
    // interpreter entry is linked; the native side then installs a
    // hook_by_signature on that descriptor and the install succeeds.

    private static void runPickII()
    {
        final Overloads o = new Overloads();
        o.pickII(1, 2);
    }

    private static void runPickJI()
    {
        final Overloads o = new Overloads();
        o.pickJI(3L);
    }

    private static void runPickIJI()
    {
        final Overloads o = new Overloads();
        o.pickIJI(4, 5L);
    }

    private static void runSoloDV()
    {
        final Overloads o = new Overloads();
        o.soloDV(1.5);
    }

    private static void runSSolo()
    {
        Overloads.sSolo();
    }

    // Links SameNameOverloads.pick(long) so a (J)I scoped_hook can install.
    private static void runPickJ()
    {
        final SameNameOverloads o = new SameNameOverloads();
        o.pick(7L);
    }

    // Links + drives Sub.shared(int) (the override) so a (I)I scoped_hook on Sub
    // installs and fires on real bytecode.
    private static void runSubShared()
    {
        final Sub s = new Sub();
        s.shared(SUBSHARED_ARG);
    }

    // Links + drives ArraySigs.oneD(long[]) so a ([J)J scoped_hook installs and
    // fires, proving array-descriptor resolution + install + dispatch.
    private static void runOneD()
    {
        final ArraySigs a = new ArraySigs();
        a.oneD(new long[] { 10L, 20L, 30L });
    }

    // Links + drives Tiny.only() so a ()V scoped_hook on Tiny installs and fires.
    private static void runTinyOnly()
    {
        final Tiny t = new Tiny();
        t.only();
    }

    /** Argument runSubShared passes to Sub.shared (mirrored native-side). */
    public static final int SUBSHARED_ARG = 41;

    static
    {
        Harness.register(new Harness.Probe()
        {
            @Override
            public boolean pending()
            {
                return MethodEnumeration.go && !MethodEnumeration.done;
            }

            @Override
            public void run()
            {
                switch (MethodEnumeration.mode)
                {
                    case 1:
                        runIdLong();
                        break;
                    case 2:
                        runIdInt();
                        break;
                    case 3:
                        runPickII();
                        break;
                    case 4:
                        runPickJI();
                        break;
                    case 5:
                        runPickIJI();
                        break;
                    case 6:
                        runSoloDV();
                        break;
                    case 7:
                        runSSolo();
                        break;
                    case 8:
                        runPickJ();
                        break;
                    case 10:
                        runSubShared();
                        break;
                    case 11:
                        runOneD();
                        break;
                    case 12:
                        runTinyOnly();
                        break;
                    default:
                        break;
                }
                MethodEnumeration.done = true;
            }
        });
    }
}

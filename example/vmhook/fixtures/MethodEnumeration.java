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
                    default:
                        break;
                }
                MethodEnumeration.done = true;
            }
        });
    }
}

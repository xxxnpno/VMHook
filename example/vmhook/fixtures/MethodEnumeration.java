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
    // Nested static class: a same-NAME overload set + a TWO-way descriptor
    // collision, purpose-built for the descriptor-hook resolution axis that the
    // top-level class does NOT exercise (it has no same-name overloads, and its
    // smallest descriptor collision is the 3-way (I)I / 6-way ()V — never the
    // minimal size()==2 boundary that hook_by_signature's "ambiguous" branch
    // turns on).  Resolved by its internal `$`-name; force-loaded via the ANCHOR
    // below (the harness loader only Class.forName's the TOP-LEVEL fixture, so a
    // nested klass must be referenced to be loaded).  None of these methods is
    // ever DISPATCHED by the probe — the native side reads their enumeration and
    // installs (never-firing) signature hooks on the unique descriptors only.
    public static final class Overloads
    {
        // ---- Same-NAME overloads: one name `pick`, FOUR distinct descriptors.
        //      Proves descriptor-uniqueness coexists with name-collision: the
        //      name rotates per obfuscated build, the descriptor does not.
        /** (I)I — pick overload #1. */
        public int pick(final int a)                     { return a; }
        /** (II)I — pick overload #2 (arity discriminator). */
        public int pick(final int a, final int b)        { return a + b; }
        /** (J)I — pick overload #3 (arg-WIDTH discriminator, same arity as #1). */
        public int pick(final long a)                    { return (int) a; }
        /** (IJ)I — pick overload #4 (mixed two-slot). */
        public int pick(final int a, final long b)       { return a + (int) b; }

        // ---- TWO-way descriptor collision: two DIFFERENT names, SAME ()I.
        //      The MINIMAL size()>1 case hook_by_signature must REFUSE.
        /** ()I — collides with beta. */
        public int alpha()                               { return 1; }
        /** ()I — collides with alpha. */
        public int beta()                                { return 2; }

        // ---- Genuinely-unique descriptors (instance + static) for the
        //      accept-on-unique install path; never dispatched, so a leaked
        //      persistent hook is harmless.
        /** (D)V — unique (double arg, void return). */
        public void solo(final double d)                 { }
        /** ()J — unique static (no-arg long return). */
        public static long sSolo()                       { return 0L; }
    }

    // Force-load anchor: referencing Overloads.class loads the nested klass at
    // this class's <clinit> time so find_class("...$Overloads") resolves it.
    static final Class<?> ANCHOR_OVERLOADS = Overloads.class;

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
                    default:
                        break;
                }
                MethodEnumeration.done = true;
            }
        });
    }
}

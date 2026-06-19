package vmhook.fixtures;

import vmhook.Harness;

/**
 * Fixture for the "method_call_return_void" feature: exercises
 * vmhook::method_proxy::call() invoking VOID-returning Java methods, and the
 * value_t::is_void() introspection path that distinguishes "the method returned
 * void / the call failed" from a primitive zero or an empty string.
 *
 * A void method has no return value the native side can inspect, so EVERY void
 * method here records its invocation (and, where applicable, the exact arguments
 * it received) into observable static fields.  The native module then proves two
 * independent things per call:
 *
 *   1. the returned value_t.is_void() is true (the discard contract), AND
 *   2. the Java body actually ran with the right arguments (the side effect the
 *      native side reads back through these static fields).
 *
 * Coverage shape (mirrors the audit finding's scenario list):
 *   - void INSTANCE method      : voidBumpInstance() -> bumps voidInstanceHits,
 *   - void STATIC method        : voidBumpStatic()   -> bumps voidStaticHits
 *                                 (exercises CallStaticVoidMethodA / the static
 *                                 call_stub path where the receiver slot is not
 *                                 consumed),
 *   - void method with PRIMITIVE args : voidPrimArgs(int,long,boolean,double)
 *                                 records each argument verbatim, so the native
 *                                 side proves the args were delivered alongside
 *                                 the void dispatch (mixed slot widths: I, J=2
 *                                 slots, Z, D=2 slots),
 *   - void method with a STRING arg   : voidStringArg(String) records the String,
 *   - void method with an OBJECT arg  : voidObjectArg(Object) records non-null +
 *                                 the object's identityHashCode, so the native
 *                                 side proves a reference arg reached the body,
 *   - NON-CORRUPTION             : after a void call the native side immediately
 *                                 does a value-returning call (retInt /
 *                                 echoIntAfterVoid) and asserts it is still
 *                                 correct — a void dispatch must not poison
 *                                 subsequent calls on the same detour thread,
 *   - CONTRAST                   : retInt() is an int returner whose value_t
 *                                 is_void() must be FALSE (a primitive zero is
 *                                 NOT void).
 *
 * How the native module drives it: it hooks {@link #trigger(int)} (the only
 * context where vmhook::hotspot::current_java_thread is set, which call()
 * requires).  The probe's run() calls trigger() through a real bytecode
 * dispatch; the detour performs every call() below and records observations.
 *
 * Java 8 syntax only (no var / records / switch-expressions / lambdas).
 */
public final class MethodCallVoid
{
    /** Native sets this true to request the action; clears it after. */
    public static volatile boolean go;

    /** The action sets this true when it has run; native polls it. */
    public static volatile boolean done;

    // ── side-effect counters proving a void body actually executed ──────────

    /** Bumped by the instance void method. */
    public static volatile int voidInstanceHits;

    /** Bumped by the static void method. */
    public static volatile int voidStaticHits;

    // ── recorded primitive arguments (prove args delivered to a void body) ──

    /** Set true once voidPrimArgs has run at least once. */
    public static volatile boolean primArgsCalled;

    /** The four arguments the LAST voidPrimArgs(...) invocation received. */
    public static volatile int     primArgInt;
    public static volatile long    primArgLong;
    public static volatile boolean primArgBool;
    public static volatile double  primArgDouble;

    // ── recorded String argument ────────────────────────────────────────────

    /** Set true once voidStringArg has run. */
    public static volatile boolean stringArgCalled;

    /** The String the LAST voidStringArg(String) invocation received (may be
     *  null if the native side passed a null reference). */
    public static volatile String  stringArg;

    /** Length of the received String, or -1 if it was null. */
    public static volatile int     stringArgLen;

    // ── recorded Object argument ────────────────────────────────────────────

    /** Set true once voidObjectArg has run. */
    public static volatile boolean objectArgCalled;

    /** True iff the Object the LAST voidObjectArg(Object) invocation received
     *  was non-null. */
    public static volatile boolean objectArgNonNull;

    /** identityHashCode of the Object the LAST voidObjectArg(Object) received,
     *  or 0 if it was null. */
    public static volatile int     objectArgIdentity;

    // ── identity publication so the object-arg check is exact ───────────────

    /** identityHashCode of SINGLETON (the receiver the native module wraps), so
     *  the native side can pass SINGLETON as the Object arg and prove it is the
     *  very object the body received. */
    public static volatile int     selfIdentity;

    // ── non-corruption breadcrumb ───────────────────────────────────────────

    /** The argument the LAST echoIntAfterVoid(int) received — used to prove a
     *  value-returning call after a void call still delivers its argument. */
    public static volatile int     lastEchoArg;

    // ── repeat-dispatch counter (idempotency / no double-dispatch) ──────────

    /** Bumped by voidRepeat(); the native side calls it a known number of times
     *  and asserts the counter equals exactly that — proving each call() ran the
     *  body exactly once (neither dropped nor doubled). */
    public static volatile int     voidRepeatHits;

    // ── boundary primitive arguments (a SECOND, edge-valued recorder) ───────
    // Distinct from voidPrimArgs so the existing "ran exactly once" assertion on
    // voidPrimArgs is untouched while we also exercise extreme bit patterns.

    public static volatile boolean edgePrimsCalled;
    public static volatile int     edgePrimInt;
    public static volatile long    edgePrimLong;
    public static volatile boolean edgePrimBool;
    public static volatile double  edgePrimDouble;

    // ── narrow / float primitive arguments (byte, short, char, float) ───────
    // The original prim test only covers I/J/Z/D; these are the 1-slot narrow
    // widths whose marshalling (zero/sign-extension into an interpreter slot) is
    // otherwise unproven for a void dispatch.

    public static volatile boolean narrowArgsCalled;
    public static volatile byte    narrowArgByte;
    public static volatile short   narrowArgShort;
    public static volatile char    narrowArgChar;
    public static volatile float   narrowArgFloat;

    // ── degenerate String arguments (empty + null) ──────────────────────────

    /** Length recorded by voidEmptyStringArg (must be 0 for the empty String). */
    public static volatile int     emptyStringLen = -2;
    public static volatile boolean emptyStringCalled;

    /** True iff voidNullStringArg received a genuinely null reference. */
    public static volatile boolean nullStringWasNull;
    public static volatile boolean nullStringCalled;

    // ── null Object argument ─────────────────────────────────────────────────

    /** True iff voidNullObjectArg received a genuinely null reference. */
    public static volatile boolean nullObjectWasNull;
    public static volatile boolean nullObjectCalled;

    // ── many-argument void (exercise more of the 8-slot parameter block) ────

    /** Sum of the six int args voidManyArgs received, and a called flag. */
    public static volatile boolean manyArgsCalled;
    public static volatile int     manyArgsSum;
    public static volatile int     manyArgsLast;

    // ── string arg via the const char* / string_view packer branch ─────────

    /** Recorded by voidStringArgC; proves the const-char-pointer / string_view
     *  arg packing branch (distinct from the std::string branch) reaches a void
     *  body with the exact text. */
    public static volatile String  cstrArg;
    public static volatile int     cstrArgLen = -2;
    public static volatile boolean cstrArgCalled;

    // ── static void with a primitive arg (static slot + arg marshalling) ────

    /** Recorded by the STATIC voidStaticArg(int); proves a static void dispatch
     *  delivers its argument (no receiver slot consumed) — the existing static
     *  test is a no-arg bump only. */
    public static volatile boolean staticArgCalled;
    public static volatile int     staticArgInt;

    // ── throwing void methods (exception propagation across a void call) ─────
    // Each sets a "reached" flag BEFORE it throws, so the native side can prove
    // the Java body actually executed up to the throw point even though the
    // call yields void and the exception is surfaced + cleared by the library.

    /** True once the instance throwing-void body started (set before throw). */
    public static volatile boolean voidThrowReached;
    /** True once the static throwing-void body started (set before throw). */
    public static volatile boolean voidStaticThrowReached;
    /** The arg the throwing-with-arg body saw (recorded before it throws), so
     *  the native side proves args are delivered even on a throwing dispatch. */
    public static volatile boolean voidThrowArgReached;
    public static volatile int     voidThrowArgValue;
    /** Counts how many times the throwing body ran — the native side calls it
     *  twice in a row to prove a throw on call N does not drop or double call N+1
     *  and does not poison the following dispatch. */
    public static volatile int     voidThrowReachedCount;

    // ── exactly-8-slot instance void (receiver + 7 ints fills params[8]) ─────
    // The call_stub fast path packs a fixed intptr_t params[8]; an instance call
    // consumes locals[0] for the receiver, leaving exactly 7 argument slots.
    // Seven ints is the maximum that still fits without the silent >8 drop, so
    // this proves the full block is delivered right at the boundary.
    public static volatile boolean eightSlotCalled;
    public static volatile int     eightSlotSum;
    public static volatile int     eightSlotLast;

    // ── all-wide void (long + double, two 2-slot args back to back) ──────────
    // Both args occupy two interpreter local slots; recording each verbatim
    // proves consecutive wide-slot marshalling for a no-return dispatch.
    public static volatile boolean wideOnlyCalled;
    public static volatile long    wideOnlyLong;
    public static volatile double  wideOnlyDouble;

    // ── mixed reference + primitive + String in one void call ────────────────
    // Exercises object_base, a primitive, and a String marshalled together in a
    // single dispatch; the native side cross-checks all three landed.
    public static volatile boolean mixedRefCalled;
    public static volatile boolean mixedRefObjNonNull;
    public static volatile int     mixedRefObjIdentity;
    public static volatile int     mixedRefInt;
    public static volatile int     mixedRefStrLen = -2;

    // ── the method the native module hooks to obtain a live thread ──────────

    /** Hookable instance method.  The native detour on this method performs
     *  every void call() the test asserts on. */
    public int trigger(final int delta)
    {
        return delta + 1;
    }

    // ── void INSTANCE method (side effect only) ─────────────────────────────
    public void voidBumpInstance()
    {
        voidInstanceHits++;
    }

    // ── void STATIC method (side effect only) ───────────────────────────────
    public static void voidBumpStatic()
    {
        voidStaticHits++;
    }

    // ── void method with PRIMITIVE args (record every arg verbatim) ─────────
    // Mixed slot widths on purpose: int (1 slot), long (2 slots), boolean
    // (1 slot), double (2 slots).  Recording each proves the whole argument
    // block was marshalled correctly even though nothing is returned.
    public void voidPrimArgs(final int i, final long j, final boolean z, final double d)
    {
        primArgInt    = i;
        primArgLong   = j;
        primArgBool   = z;
        primArgDouble = d;
        primArgsCalled = true;
    }

    // ── void method with a STRING arg ───────────────────────────────────────
    public void voidStringArg(final String s)
    {
        stringArg       = s;
        stringArgLen    = (s == null) ? -1 : s.length();
        stringArgCalled = true;
    }

    // ── void method with an OBJECT arg ──────────────────────────────────────
    public void voidObjectArg(final Object o)
    {
        objectArgNonNull  = (o != null);
        objectArgIdentity = (o == null) ? 0 : System.identityHashCode(o);
        objectArgCalled   = true;
    }

    // ── repeat-dispatch: bumps a counter; native calls it a known N times ───
    public void voidRepeat()
    {
        voidRepeatHits++;
    }

    // ── boundary primitive args (a SECOND recorder, edge bit patterns) ──────
    public void voidEdgePrims(final int i, final long j, final boolean z, final double d)
    {
        edgePrimInt    = i;
        edgePrimLong   = j;
        edgePrimBool   = z;
        edgePrimDouble = d;
        edgePrimsCalled = true;
    }

    // ── void with narrow / float args (byte, short, char, float) ────────────
    // 1-slot widths whose extension into the interpreter argument slot is
    // otherwise unproven for a no-return dispatch.
    public void voidNarrowArgs(final byte b, final short s, final char c, final float f)
    {
        narrowArgByte  = b;
        narrowArgShort = s;
        narrowArgChar  = c;
        narrowArgFloat = f;
        narrowArgsCalled = true;
    }

    // ── void with an EMPTY String arg ───────────────────────────────────────
    public void voidEmptyStringArg(final String s)
    {
        emptyStringLen    = (s == null) ? -1 : s.length();
        emptyStringCalled = true;
    }

    // ── void with a NULL String arg ─────────────────────────────────────────
    public void voidNullStringArg(final String s)
    {
        nullStringWasNull = (s == null);
        nullStringCalled  = true;
    }

    // ── void with a NULL Object arg ─────────────────────────────────────────
    public void voidNullObjectArg(final Object o)
    {
        nullObjectWasNull = (o == null);
        nullObjectCalled  = true;
    }

    // ── void with MANY (six) int args — fills more of the parameter block ───
    public void voidManyArgs(final int a, final int b, final int c,
                             final int d, final int e, final int f)
    {
        manyArgsSum    = a + b + c + d + e + f;
        manyArgsLast   = f;
        manyArgsCalled = true;
    }

    // ── void with a String arg, recorded for the const char*/view packer ────
    public void voidStringArgC(final String s)
    {
        cstrArg       = s;
        cstrArgLen    = (s == null) ? -1 : s.length();
        cstrArgCalled = true;
    }

    // ── STATIC void with a primitive arg (no receiver slot + arg delivered) ─
    public static void voidStaticArg(final int v)
    {
        staticArgInt    = v;
        staticArgCalled = true;
    }

    // ── THROWING void (instance): set reached, bump counter, then throw ──────
    // The flag/counter are written BEFORE the throw so the native side can prove
    // the body ran; the library surfaces + clears the exception and returns void.
    public void voidThrows()
    {
        voidThrowReached = true;
        voidThrowReachedCount++;
        throw new RuntimeException("voidThrows-intentional");
    }

    // ── THROWING void (static): static dispatch path + thrown exception ──────
    public static void voidThrowsStatic()
    {
        voidStaticThrowReached = true;
        throw new IllegalStateException("voidThrowsStatic-intentional");
    }

    // ── THROWING void WITH an arg: record the arg, then throw ────────────────
    // Proves the argument is marshalled into the body even though the dispatch
    // ultimately throws (the record happens before the throw).
    public void voidThrowsArg(final int v)
    {
        voidThrowArgReached = true;
        voidThrowArgValue   = v;
        throw new RuntimeException("voidThrowsArg-intentional");
    }

    // ── exactly-8-slot instance void (receiver + 7 int args) ────────────────
    public void voidEightSlots(final int a, final int b, final int c,
                               final int d, final int e, final int f, final int g)
    {
        eightSlotSum    = a + b + c + d + e + f + g;
        eightSlotLast   = g;
        eightSlotCalled = true;
    }

    // ── all-wide void: long + double, two consecutive 2-slot args ───────────
    public void voidWideOnly(final long j, final double d)
    {
        wideOnlyLong   = j;
        wideOnlyDouble = d;
        wideOnlyCalled = true;
    }

    // ── mixed reference + primitive + String in one void dispatch ───────────
    public void voidMixedRef(final Object o, final int n, final String s)
    {
        mixedRefObjNonNull  = (o != null);
        mixedRefObjIdentity = (o == null) ? 0 : System.identityHashCode(o);
        mixedRefInt         = n;
        mixedRefStrLen      = (s == null) ? -1 : s.length();
        mixedRefCalled      = true;
    }

    // ── CONTRAST: an int returner whose value_t.is_void() must be FALSE ──────
    public int retInt()
    {
        return 1337;
    }

    // ── NON-CORRUPTION: a value-returning call performed right after a void
    //    call; records its argument and echoes it back so the native side can
    //    prove the void dispatch did not corrupt this subsequent call. ────────
    public int echoIntAfterVoid(final int v)
    {
        lastEchoArg = v;
        return v;
    }

    static
    {
        Harness.register(new Harness.Probe()
        {
            @Override
            public boolean pending()
            {
                return MethodCallVoid.go && !MethodCallVoid.done;
            }

            @Override
            public void run()
            {
                // Publish the receiver's identity so the native side can pass it
                // as the Object arg and prove byte-for-byte that the body got the
                // exact object.  Use the SAME instance the native module wraps:
                // the hook fires on trigger() with self == SINGLETON.
                MethodCallVoid.selfIdentity = System.identityHashCode(SINGLETON);

                // Driving trigger() through normal bytecode dispatch fires the
                // native interpreter hook; that detour performs every
                // method_proxy::call() the test asserts on.
                SINGLETON.trigger(7);

                MethodCallVoid.done = true;
            }
        });
    }

    /**
     * The single instance the native module wraps and drives.  Created eagerly
     * so the native side reaches the non-static void methods on a stable OOP and
     * so the published identity matches the receiver the detour sees as `self`.
     */
    public static final MethodCallVoid SINGLETON = new MethodCallVoid();
}

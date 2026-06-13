package vmhook.fixtures;

import vmhook.Harness;

/**
 * Fixture for the "method_throwing_call_site" feature: the native module invokes
 * a Java method that THROWS, via vmhook::method_proxy::call(), from inside a
 * detour, and proves the call site COMPLETES and the JVM is left clean and
 * usable.
 *
 * This is the legacy {@code test_throwing_method} scenario carried into the
 * modular harness, but HARDENED: where the legacy Example path merely let a
 * Java-side {@code runProbe} call the throwing method and catch the exception in
 * Java, this fixture instead exposes a throwing method the NATIVE side drives
 * directly through method_proxy::call().  That is the dangerous shape — a Java
 * exception unwinding back into native code — so the contract under test is
 * "no detour access-violation, no suite truncation, thread NOT left in
 * ExceptionOccurred state, JVM healthy afterwards".
 *
 * How the native module drives it (mirrors MethodCallJni / MethodPrimitives):
 *   - The native side hooks {@link #trigger(int)}.  Inside that detour
 *     vmhook::hotspot::current_java_thread is set, which is the ONLY context in
 *     which method_proxy::call() may invoke the interpreter / JNI call gate.
 *   - The probe's run() calls trigger(1) on the shared SINGLETON through normal
 *     bytecode dispatch.  That fires the native interpreter hook; the detour
 *     then calls boom(-1) (which throws), a benign control method, and reads a
 *     field, recording observations into C++ atomics.
 *
 * IMPORTANT — this fixture's probe action does NOT itself call boom().  Only the
 * NATIVE detour calls boom(), so the exception path under test is purely the
 * native call() -> Java-throw -> native unwind one.  The probe body only calls
 * the benign trigger(); if boom() leaked an exception into the detour and that
 * corrupted the thread, the symptom shows up as a missing {@code done} flag
 * (native crash / truncation) or a failed post-throw health check, never as an
 * exception escaping this fixture.
 *
 * Java 8 syntax only (no var / records / switch-expressions / lambdas):
 * the anonymous Harness.Probe below is the Java-8-only probe shape.
 */
public final class ThrowingMethod
{
    /** Native sets this true to request the action; the probe clears it after. */
    public static volatile boolean go;

    /** The probe action sets this true when it has run; native polls it. */
    public static volatile boolean done;

    /** Bumped every time the hooked trigger() runs (handshake sanity). */
    public static volatile int triggerCount;

    /** Bumped every time boom() actually ENTERS its body, so the native side can
     *  prove the throwing method was genuinely dispatched (not silently skipped
     *  because the call gate was unavailable).  Incremented BEFORE the throw. */
    public static volatile int boomEntered;

    /** The argument the LAST boom() invocation received, recorded BEFORE the
     *  throw — lets the native side prove the argument was marshalled into the
     *  throwing call correctly even though the call unwinds. */
    public static volatile int boomLastArg;

    /** Bumped every time the benign control method safeAdd() runs.  The native
     *  side calls safeAdd AFTER boom() threw, so a non-zero value here proves
     *  the JVM/thread is still able to dispatch Java bytecode post-exception. */
    public static volatile int safeAddCalls;

    /** A plain readable instance field.  The native side reads it AFTER the
     *  throwing call to prove field access still works on a clean thread. */
    public int healthField = 0x600DC0DE;

    /** A static readable field, same purpose via the static accessor. */
    public static int staticHealthField = 0x5AFE5AFE;

    // ── the method the native module hooks to obtain a live thread ──────────

    /**
     * Hookable instance method.  The native detour installed on this method
     * performs the throwing method_proxy::call() and the post-throw health
     * checks.  Returns a trivial value so the hook can also observe a normal
     * (non-throwing) return path is intact.
     */
    public int trigger(final int delta)
    {
        triggerCount++;
        return delta + 1;
    }

    // ════════════════════════════════════════════════════════════════════════
    //  The THROWING method under test.
    //
    //  Unconditionally throws for the negative input the native side passes
    //  (boom(-1)); records its entry + argument FIRST so the throw cannot hide
    //  the fact that the body genuinely ran.  An IllegalStateException is an
    //  unchecked RuntimeException, so no `throws` clause is needed and the
    //  descriptor stays a clean (I)I — the native side resolves and calls it
    //  exactly like any other int(int) method.
    // ════════════════════════════════════════════════════════════════════════

    /**
     * Throws IllegalStateException for any negative argument (and for the
     * specific boom(-1) the native side uses).  For a non-negative argument it
     * returns the argument unchanged, so the method has a real non-throwing
     * branch too (the native side does not rely on it, but it keeps boom a
     * normal, JIT-friendly method rather than a guaranteed-throw stub).
     */
    public int boom(final int x)
    {
        boomLastArg = x;
        boomEntered++;
        if (x < 0)
        {
            throw new IllegalStateException("boom:" + x);
        }
        return x;
    }

    /** Static throwing variant, so the native side can also drive the
     *  CallStaticIntMethodA throwing path if it chooses.  Same shape. */
    public static int sBoom(final int x)
    {
        if (x < 0)
        {
            throw new IllegalStateException("sBoom:" + x);
        }
        return x;
    }

    // ── benign control method: the JVM-health witness after the throw ───────

    /**
     * Benign control method the native side calls AFTER boom() threw.  A
     * successful return with the expected value proves the throwing call left
     * the thread in a clean, usable state (no pending exception poisoning the
     * next dispatch).  result = x + 1.
     */
    public int safeAdd(final int x)
    {
        safeAddCalls++;
        return x + 1;
    }

    static
    {
        Harness.register(new Harness.Probe()
        {
            @Override
            public boolean pending()
            {
                return ThrowingMethod.go && !ThrowingMethod.done;
            }

            @Override
            public void run()
            {
                // Drive trigger() through a normal bytecode dispatch -> fires the
                // native interpreter hook; THAT detour performs the throwing
                // method_proxy::call() and the post-throw health checks.  This
                // probe body deliberately never touches boom() itself.
                SINGLETON.trigger(1);
                ThrowingMethod.done = true;
            }
        });
    }

    /**
     * The single instance the native module wraps and drives.  Created eagerly
     * so the native side reaches the instance methods on a stable OOP.
     */
    public static final ThrowingMethod SINGLETON = new ThrowingMethod();
}

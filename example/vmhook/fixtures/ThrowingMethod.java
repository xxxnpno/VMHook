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
 * Java, this fixture instead exposes a FAMILY of throwing methods the NATIVE
 * side drives directly through method_proxy::call().  That is the dangerous
 * shape — a Java exception unwinding back into native code — so the contract
 * under test is "no detour access-violation, no suite truncation, thread NOT
 * left in ExceptionOccurred state, JVM healthy afterwards", proven once per
 * exception KIND.
 *
 * How the native module drives it (mirrors MethodCallJni / MethodPrimitives):
 *   - The native side hooks {@link #trigger(int)}.  Inside that detour
 *     vmhook::hotspot::current_java_thread is set, which is the ONLY context in
 *     which method_proxy::call() may invoke the interpreter / JNI call gate.
 *   - The probe's run() calls trigger(1) on the shared SINGLETON through normal
 *     bytecode dispatch.  That fires the native interpreter hook; the detour
 *     then drives one throwing call() (selected by {@link #scenario}), a benign
 *     control method, and reads a field, recording observations into C++
 *     atomics.  The native side runs MANY probe cycles, one per scenario, so it
 *     can prove the thread recovered cleanly after EACH kind of throw.
 *
 * IMPORTANT — this fixture's probe action does NOT itself call any throwing
 * method.  Only the NATIVE detour calls them, so the exception path under test
 * is purely the native call() -> Java-throw -> native unwind one.  The probe
 * body only calls the benign trigger(); if a throw leaked an exception into the
 * detour and that corrupted the thread, the symptom shows up as a missing
 * {@code done} flag (native crash / truncation) or a failed post-throw health
 * check, never as an exception escaping this fixture.
 *
 * Java 8 syntax only (no var / records / switch-expressions / lambdas):
 * the anonymous Harness.Probe below is the Java-8-only probe shape.  Compiles
 * unchanged under javac 8, 21, and 26 (default source level on each).
 */
public final class ThrowingMethod
{
    /** Native sets this true to request the action; the probe clears it after. */
    public static volatile boolean go;

    /** The probe action sets this true when it has run; native polls it. */
    public static volatile boolean done;

    /**
     * Which throwing method the native detour should drive this cycle.  The
     * native side programs it on the rising edge of {@code go}; the detour reads
     * it (via a field, NOT via this fixture) to pick the scenario.  Mirrors the
     * OnException {@code mode} selector.  It is informational on the Java side —
     * the probe body never branches on it — but lets the native module record
     * exactly which call() it drove per cycle.
     */
    public static volatile int scenario;

    /** Bumped every time the hooked trigger() runs (handshake sanity). */
    public static volatile int triggerCount;

    // ── per-throwing-method witnesses ───────────────────────────────────────
    // Each throwing method records its entry + argument FIRST (BEFORE the throw)
    // so the throw can never hide that the body genuinely ran and received the
    // native side's marshalled argument.

    /** boom(int): RuntimeException (IllegalStateException) — the headline path. */
    public static volatile int boomEntered;
    public static volatile int boomLastArg;

    /** throwChecked(int): a CHECKED exception (java.io.IOException). */
    public static volatile int checkedEntered;
    public static volatile int checkedLastArg;

    /** throwError(int): a java.lang.Error (subclass of Throwable, not Exception). */
    public static volatile int errorEntered;
    public static volatile int errorLastArg;

    /** throwCustom(int): a project-defined exception (the nested BoomException). */
    public static volatile int customEntered;
    public static volatile int customLastArg;

    /** throwNpe(int): a java.lang.NullPointerException raised by a real null deref. */
    public static volatile int npeEntered;
    public static volatile int npeLastArg;

    /** throwAioobe(int): java.lang.ArrayIndexOutOfBoundsException from a real OOB read. */
    public static volatile int aioobeEntered;
    public static volatile int aioobeLastArg;

    /** throwArithmetic(int): java.lang.ArithmeticException from integer divide-by-zero. */
    public static volatile int arithEntered;
    public static volatile int arithLastArg;

    /** throwNested(int): throws via a NESTED Java call (deep()) so the exception
     *  unwinds several Java frames before crossing back into native code. */
    public static volatile int nestedEntered;     // outer frame entered
    public static volatile int nestedDeepEntered; // innermost frame entered
    public static volatile int nestedLastArg;

    /** sBoom(int): a STATIC throwing method (CallStaticIntMethodA throw path). */
    public static volatile int sBoomEntered;
    public static volatile int sBoomLastArg;

    /** throwAfterSuccess(int): records that a PRIOR successful call ran, then
     *  throws — proves "throw after a successful call leaves state clean". */
    public static volatile int afterSuccessEntered;
    public static volatile int afterSuccessLastArg;

    // ── RETURN-DESCRIPTOR-variety witnesses (throwVoid / throwLong / ...) ───
    /** throwVoid(int): (I)V — call() decodes 'V' (monostate) on the unwind. */
    public static volatile int voidEntered;
    public static volatile int voidLastArg;
    /** throwLong(int): (I)J — call() decodes 'J' (64-bit default cell). */
    public static volatile int longRetEntered;
    public static volatile int longRetLastArg;
    /** throwDouble(int): (I)D — call() decodes 'D' (double default cell). */
    public static volatile int doubleRetEntered;
    public static volatile int doubleRetLastArg;
    /** throwBool(int): (I)Z — call() decodes 'Z'. */
    public static volatile int boolRetEntered;
    public static volatile int boolRetLastArg;
    /** throwString(int): (I)Ljava/lang/String; — call() takes the object decode. */
    public static volatile int stringRetEntered;
    public static volatile int stringRetLastArg;

    // ── ARGUMENT-SHAPE-variety witnesses (throwNoArg / throwLongArg / ...) ──
    /** throwNoArg(): ()I — no extra-arg pack. */
    public static volatile int noArgEntered;
    /** throwLongArg(long): (J)I — full 64-bit arg recorded before the throw. */
    public static volatile int  longArgEntered;
    public static volatile long longArgLast;
    /** throwDoubleArg(double): (D)I — arg's raw bits recorded before the throw. */
    public static volatile int  doubleArgEntered;
    public static volatile long doubleArgLastBits;
    /** throwStringArg(String): (Ljava/lang/String;)I — marshalled String proven. */
    public static volatile int    stringArgEntered;
    public static volatile int    stringArgLastLen;
    public static volatile String stringArgLastValue;
    /** throwTwoArgs(int,long): (IJ)I — int then wide long, slot-shift witness. */
    public static volatile int  twoArgsEntered;
    public static volatile int  twoArgsLastA;
    public static volatile long twoArgsLastB;

    // ── extra unwind-shape witnesses (throwDeep3 / throwInFinally) ──────────
    /** throwDeep3(int): unwinds three Java frames before reaching native. */
    public static volatile int deep3Entered;
    public static volatile int deep3MidEntered;
    public static volatile int deep3InnerEntered;
    public static volatile int deep3LastArg;
    /** throwInFinally(int): throw that traverses a finally handler on the way out. */
    public static volatile int finallyEntered;
    public static volatile int finallyRan;
    public static volatile int finallyLastArg;

    /** Bumped every time the benign control method safeAdd() runs.  The native
     *  side calls safeAdd AFTER a throw, so a growing value proves the JVM/thread
     *  is still able to dispatch Java bytecode post-exception. */
    public static volatile int safeAddCalls;

    /** A plain readable instance field.  The native side reads it AFTER the
     *  throwing call to prove field access still works on a clean thread. */
    public int healthField = 0x600DC0DE;

    /** A static readable field, same purpose via the static accessor. */
    public static int staticHealthField = 0x5AFE5AFE;

    // A non-null instance field used to make the NPE path a genuine null deref
    // on a SEPARATE reference, while this one stays valid.
    private final int[] presentArray = new int[] { 11, 22, 33 };

    // Deliberately-null reference the NPE path dereferences.
    private final int[] absentArray = null;

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
    //  The THROWING methods under test.  All keep a clean (I)I descriptor so the
    //  native side resolves and calls each exactly like any other int(int)
    //  method, regardless of whether the thrown type is checked or unchecked.
    //  (A checked exception is wrapped so no `throws` clause is needed and the
    //  descriptor stays (I)I; the WRAPPED throwable is still genuinely a checked
    //  java.io.IOException, observable as such by the native side.)
    // ════════════════════════════════════════════════════════════════════════

    /**
     * Throws IllegalStateException (an unchecked RuntimeException) for any
     * negative argument (and for the boom(-1) the native side uses).  For a
     * non-negative argument it returns the argument unchanged, so the method has
     * a real non-throwing branch too.
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

    /**
     * Raises a CHECKED exception (java.io.IOException).  Wrapped in an unchecked
     * RuntimeException so the descriptor stays (I)I, but the actual pending
     * Throwable the native side observes is the checked IOException cause — i.e.
     * a checked exception genuinely crosses the native boundary.  We throw the
     * RuntimeException whose cause is the IOException; either way a Throwable is
     * pending after the call.
     */
    public int throwChecked(final int x)
    {
        checkedLastArg = x;
        checkedEntered++;
        try
        {
            throw new java.io.IOException("checked-io:" + x);
        }
        catch (final java.io.IOException ioe)
        {
            throw new RuntimeException(ioe);
        }
    }

    /**
     * Throws a java.lang.Error (subclass of Throwable that is NOT an Exception).
     * Errors are the most dangerous to let escape, so the native side must clear
     * one just as cleanly as an ordinary exception.
     */
    public int throwError(final int x)
    {
        errorLastArg = x;
        errorEntered++;
        throw new IllegalAccessError("error:" + x);
    }

    /** Throws the project-defined nested BoomException (custom Throwable type). */
    public int throwCustom(final int x)
    {
        customLastArg = x;
        customEntered++;
        throw new BoomException("custom:" + x);
    }

    /** Raises a genuine java.lang.NullPointerException by dereferencing a null
     *  array reference (NOT an explicit throw), so it is the JVM's own NPE. */
    public int throwNpe(final int x)
    {
        npeLastArg = x;
        npeEntered++;
        // absentArray is null; reading its length is a real null deref -> NPE.
        return absentArray.length + x;
    }

    /** Raises a genuine java.lang.ArrayIndexOutOfBoundsException by reading past
     *  the end of a real array (the JVM's own bounds-check throw). */
    public int throwAioobe(final int x)
    {
        aioobeLastArg = x;
        aioobeEntered++;
        // presentArray has length 3; index 99 is out of bounds -> AIOOBE.
        return presentArray[99] + x;
    }

    /** Raises a genuine java.lang.ArithmeticException via integer / by zero. */
    public int throwArithmetic(final int x)
    {
        arithLastArg = x;
        arithEntered++;
        final int zero = arithEntered - arithEntered; // 0, opaque to the JIT
        return x / zero;
    }

    /** Throws via a NESTED Java call: this frame calls deep(), which throws, so
     *  the exception unwinds an extra Java frame before reaching native code. */
    public int throwNested(final int x)
    {
        nestedLastArg = x;
        nestedEntered++;
        return deep(x); // unwinds deep()'s frame, then this one, into native
    }

    private int deep(final int x)
    {
        nestedDeepEntered++;
        throw new IllegalStateException("nested-deep:" + x);
    }

    /**
     * Static throwing variant (CallStaticIntMethodA throw path).  Records its
     * own witnesses so the static dispatch's throw can be proven independently.
     */
    public static int sBoom(final int x)
    {
        sBoomLastArg = x;
        sBoomEntered++;
        if (x < 0)
        {
            throw new IllegalStateException("sBoom:" + x);
        }
        return x;
    }

    /**
     * Performs a benign side effect (incrementing safeAddCalls) and THEN throws —
     * so the native side can prove that a throw which happens AFTER the method
     * already did real, successful work still leaves the thread clean (the work
     * is committed; only the unwinding return value is lost).
     */
    public int throwAfterSuccess(final int x)
    {
        afterSuccessLastArg = x;
        afterSuccessEntered++;
        safeAddCalls++; // the "successful work" that commits before the throw
        throw new IllegalStateException("after-success:" + x);
    }

    // ════════════════════════════════════════════════════════════════════════
    //  RETURN-DESCRIPTOR variety: throwing methods whose return type is NOT (I)I.
    //  These exercise every ret_char decode branch of method_proxy::call() on the
    //  unwound (exception) path — where result_holder was NEVER written by the
    //  callee, so the decoded value is the dispatcher's zero default cell.  The
    //  return value is therefore [INFO] only on the native side; what is asserted
    //  is the same triad (body ran with our arg, thread clean, JVM healthy).
    // ════════════════════════════════════════════════════════════════════════

    /** Throws after recording its arg; declared VOID so call() decodes 'V' ->
     *  monostate even though the method never returns normally. */
    public void throwVoid(final int x)
    {
        voidEntered++;
        voidLastArg = x;
        throw new IllegalStateException("void:" + x);
    }

    /** Throws after recording its arg; declared LONG so call() takes the 64-bit
     *  'J' decode branch on the unwound path (the default-cell caveat at 64 bits). */
    public long throwLong(final int x)
    {
        longRetEntered++;
        longRetLastArg = x;
        throw new IllegalStateException("long:" + x);
    }

    /** Throws after recording its arg; declared DOUBLE so call() takes the 'D'
     *  decode branch on the unwound path. */
    public double throwDouble(final int x)
    {
        doubleRetEntered++;
        doubleRetLastArg = x;
        throw new IllegalStateException("double:" + x);
    }

    /** Throws after recording its arg; declared BOOLEAN so call() takes the 'Z'
     *  decode branch on the unwound path. */
    public boolean throwBool(final int x)
    {
        boolRetEntered++;
        boolRetLastArg = x;
        throw new IllegalStateException("bool:" + x);
    }

    /** Throws after recording its arg; declared to return a reference
     *  (java.lang.String) so call() takes the object-decode branch on the unwound
     *  path (result_holder is the zero default cell, so the decoded "reference" is
     *  null -> monostate; verified [INFO]-only). */
    public String throwString(final int x)
    {
        stringRetEntered++;
        stringRetLastArg = x;
        throw new IllegalStateException("string:" + x);
    }

    // ════════════════════════════════════════════════════════════════════════
    //  ARGUMENT-SHAPE variety: throwing methods whose parameter list is NOT a
    //  single int.  These exercise the call() pack() branches (no-arg, wide long,
    //  wide double, String reference, multi-slot) on a method that then THROWS, so
    //  the marshalled argument is recorded BEFORE the throw and proven to have
    //  arrived intact across the native->Java boundary that the exception unwinds.
    // ════════════════════════════════════════════════════════════════════════

    /** Zero-arg throwing method: () I.  Proves the no-extra-arg pack path (only
     *  the receiver slot) survives a throw. */
    public int throwNoArg()
    {
        noArgEntered++;
        throw new IllegalStateException("no-arg");
    }

    /** Wide LONG arg then throw: (J)I.  Records the full 64-bit arg first so a
     *  truncated/ shifted wide arg would be caught even though the call throws. */
    public int throwLongArg(final long v)
    {
        longArgEntered++;
        longArgLast = v;
        throw new IllegalStateException("long-arg:" + v);
    }

    /** Wide DOUBLE arg then throw: (D)I.  Records the arg's raw bits via
     *  Double.doubleToRawLongBits so the witness is bit-exact. */
    public int throwDoubleArg(final double v)
    {
        doubleArgEntered++;
        doubleArgLastBits = Double.doubleToRawLongBits(v);
        throw new IllegalStateException("double-arg");
    }

    /** STRING reference arg then throw: (Ljava/lang/String;)I.  Records the
     *  argument's length and a copy so the marshalled java.lang.String is proven
     *  to have reached the body before the unwind. */
    public int throwStringArg(final String s)
    {
        stringArgEntered++;
        stringArgLastLen = (s == null) ? -1 : s.length();
        stringArgLastValue = s;
        throw new IllegalStateException("string-arg");
    }

    /** MULTI-SLOT args then throw: (IJ)I — an int followed by a wide long, so the
     *  long occupies two interpreter local slots after a one-slot int.  Records
     *  both so a slot-shift would be caught. */
    public int throwTwoArgs(final int a, final long b)
    {
        twoArgsEntered++;
        twoArgsLastA = a;
        twoArgsLastB = b;
        throw new IllegalStateException("two-args:" + a + "/" + b);
    }

    // ════════════════════════════════════════════════════════════════════════
    //  Extra UNWIND shapes.
    // ════════════════════════════════════════════════════════════════════════

    /** Throws through THREE Java frames (deeper than throwNested's two) so the
     *  exception unwinds an extra interpreter frame before reaching native code. */
    public int throwDeep3(final int x)
    {
        deep3Entered++;
        deep3LastArg = x;
        return deep3b(x);
    }

    private int deep3b(final int x)
    {
        deep3MidEntered++;
        return deep3c(x);
    }

    private int deep3c(final int x)
    {
        deep3InnerEntered++;
        throw new IllegalStateException("deep3:" + x);
    }

    /** Throws from inside a try whose FINALLY block also runs real work before the
     *  exception propagates — proves a throw that traverses a finally handler on
     *  the way out still leaves the thread clean. */
    public int throwInFinally(final int x)
    {
        finallyEntered++;
        finallyLastArg = x;
        try
        {
            throw new IllegalStateException("finally-try:" + x);
        }
        finally
        {
            finallyRan++; // committed side effect on the unwind path
        }
    }

    // ── benign control method: the JVM-health witness after the throw ───────

    /**
     * Benign control method the native side calls AFTER a throw.  A successful
     * return with the expected value proves the throwing call left the thread in
     * a clean, usable state (no pending exception poisoning the next dispatch).
     * result = x + 1.
     */
    public int safeAdd(final int x)
    {
        safeAddCalls++;
        return x + 1;
    }

    // ── project-defined custom Throwable for the "custom exception" scenario ─
    // A nested static class so it compiles with this fixture and is ignored by
    // Main.loadFixtures() (which skips '$'-containing .class names).  Extends
    // RuntimeException so the descriptor of throwCustom stays (I)I.
    public static final class BoomException extends RuntimeException
    {
        private static final long serialVersionUID = 1L;

        public BoomException(final String message)
        {
            super(message);
        }
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
                // method_proxy::call() (selected by `scenario`) and the
                // post-throw health checks.  This probe body deliberately never
                // touches any throwing method itself.
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

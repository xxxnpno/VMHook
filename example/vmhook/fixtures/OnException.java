package vmhook.fixtures;

import vmhook.Harness;

/**
 * Fixture for the on_exception feature (area: hooks / exception-construction
 * watcher).  Migrates the legacy vmhook.Example throwProbe / test_on_exception.
 *
 * Drives vmhook::on_exception(callback) — the callback that fires whenever a
 * java.lang.Throwable (or any subclass) is constructed, because every public
 * Throwable constructor calls Throwable.fillInStackTrace() (the hooked method)
 * before returning.  This fixture exhaustively exercises EVERY thrown-exception
 * observation path on a LIVE JVM with GENUINE `athrow`s:
 *
 *   - RuntimeException (IllegalStateException) — the unchecked baseline,
 *   - a DIFFERENT RuntimeException (NumberFormatException) for type discrimination,
 *   - a CHECKED exception (java.io.IOException),
 *   - an Error (a custom subclass of java.lang.Error),
 *   - CUSTOM subclasses of RuntimeException / Exception / Error,
 *   - a RE-THROWN exception (same instance thrown twice — constructor runs ONCE),
 *   - a NESTED-call throw (constructed several frames deep, caught at the top),
 *   - JVM-INTERNAL implicit throws (NPE / AIOOBE / ClassCast / ArithmeticException)
 *     versus an EXPLICIT `new` of the same type,
 *   - a throw from a class STATIC INITIALIZER (ExceptionInInitializerError path),
 *   - a throw from a CONSTRUCTOR,
 *   - a throw that is UNCAUGHT at its throw site and caught several frames higher,
 *   - several throws in one cycle (fan-out / exact counting),
 *   - a NO-THROW control cycle (constructs no Throwable, so the callback must not
 *     fire).
 *
 * EVERY throw here is a real `athrow` of a freshly-constructed exception whose
 * public constructor runs fillInStackTrace on THIS Java thread, immediately
 * before the throw — so the interpreter hook (if armed) fires synchronously,
 * exactly like the throw the legacy probe performed.  The ONLY deliberate
 * exceptions to "constructor runs right before athrow" are characterization
 * modes: the RE-THROW mode (mode 9) throws an already-constructed instance a
 * second time, and the JVM-internal implicit modes (11..14) rely on the JVM to
 * build the throwable (which, under -XX:+OmitStackTraceInFastThrow, may reuse a
 * preallocated instance that skips fillInStackTrace).  Each such mode is
 * documented and the native side characterizes it best-effort.
 *
 * Canonical go/done handshake: the native module sets `mode`, raises `go`, the
 * Harness loop runs run() on the Java thread (genuine bytecode dispatch, which
 * is what makes the interpreter hook fire), then the module polls the latched
 * `done`.  Because `done` latches, every throw a single assertion needs happens
 * inside one run() invocation; the module selects the scenario via `mode`.
 *
 * Crucially, run() ALSO records Java-observable witnesses — `throwsObserved`
 * (how many exceptions this cycle constructed + threw + caught) and
 * `lastThrowKind` (a stable tag for the last type thrown).  The native side
 * reads these back to prove the Java throw genuinely happened, INDEPENDENTLY of
 * whether the (possibly silently-dead) on_exception callback fired.  That lets
 * the module distinguish "the callback did not fire" from "the throw never ran".
 *
 * Source level: kept Java-8 compatible (no var / records / switch expressions /
 * text blocks) so the CI javac matrix (8 / 21 / 26) all compile it.
 */
public final class OnException
{
    /** Native sets this true to request the action; clears it after. */
    public static volatile boolean go;

    /** The action sets this true when it has run; native polls it. */
    public static volatile boolean done;

    /**
     * Selects which scenario run() executes.  Set by the native module BEFORE it
     * raises `go` so a single probe cycle drives exactly the throws it asserts on.
     *    1 = throw+catch ONE IllegalStateException     (primary trap trigger)
     *    2 = throw+catch MANY IllegalStateException     (fan-out / per-watcher count)
     *    3 = throw+catch ONE NumberFormatException      (type discrimination)
     *    4 = NO throw at all                            (control: no Throwable built)
     *    5 = throw+catch ONE checked java.io.IOException (checked-exception path)
     *    6 = throw+catch ONE custom Error subclass       (Error path)
     *    7 = throw+catch ONE custom checked Exception     (custom checked subclass)
     *    8 = throw+catch ONE custom RuntimeException       (custom unchecked subclass)
     *    9 = construct ONE ISE, throw + catch, then RE-THROW the SAME instance
     *        and catch again (TWO athrows, but the constructor — and so
     *        fillInStackTrace — runs only ONCE)
     *   10 = throw an ISE from several call frames deep, caught at the top
     *        (nested-call throw; one construction)
     *   11 = trigger an IMPLICIT NullPointerException (deref null)         [JVM-internal]
     *   12 = trigger an IMPLICIT ArrayIndexOutOfBoundsException (bad index) [JVM-internal]
     *   13 = trigger an IMPLICIT ClassCastException (bad cast)              [JVM-internal]
     *   14 = trigger an IMPLICIT ArithmeticException (divide by zero)       [JVM-internal]
     *   15 = throw+catch an EXPLICIT `new NullPointerException()`           (explicit contrast)
     *   16 = force a class whose STATIC INITIALIZER throws (one-shot;
     *        ExceptionInInitializerError wraps the cause)
     *   17 = `new` a class whose CONSTRUCTOR throws (caught)
     *   18 = throw an ISE that is UNCAUGHT at its throw site and caught three
     *        frames higher (caught-vs-uncaught characterization)
     */
    public static volatile int mode;

    // ---- Java-observable witnesses (prove the throw genuinely ran) ---------

    /** How many exceptions run() constructed + threw + caught this cycle. */
    public static volatile int throwsObserved;

    /**
     * Stable tag for the LAST exception kind thrown this cycle, so the native
     * side can confirm the Java action without resolving a class name itself:
     *    0 = none thrown this cycle (mode 4)
     *    1 = IllegalStateException
     *    2 = NumberFormatException
     *    3 = java.io.IOException (checked)
     *    4 = custom Error subclass
     *    5 = custom checked Exception subclass
     *    6 = custom RuntimeException subclass
     *    7 = NullPointerException (implicit OR explicit)
     *    8 = ArrayIndexOutOfBoundsException (implicit)
     *    9 = ClassCastException (implicit)
     *   10 = ArithmeticException (implicit)
     *   11 = ExceptionInInitializerError (static-init throw)
     *   12 = constructor-thrown ISE (mode 17)
     */
    public static volatile int lastThrowKind;

    /** The detail message carried by the last thrown exception (round-trip aid). */
    public static volatile String lastThrowMsg = "";

    /**
     * For mode 9 (re-throw): how many distinct CONSTRUCTIONS happened (always 1),
     * recorded separately from throwsObserved (which counts athrow+catch pairs, 2).
     * Lets the native side assert "two catches but one construction".
     */
    public static volatile int distinctConstructions;

    /**
     * A DEFINITELY-zero value the compiler cannot constant-fold (a literal `7 / 0`
     * is a compile-time error in Java).  Reading this volatile field for the
     * divisor in mode 14 guarantees a deterministic ArithmeticException on every
     * run, with no clock-skew flakiness.  Never written to a non-zero value.
     */
    public static volatile int zeroField = 0;

    // ---- Constants mirrored on the native side ----------------------------

    /** How many ISEs mode 2 throws in one cycle (fan-out counting target). */
    public static final int MANY_THROWS = 4;

    /** The detail message every IllegalStateException carries. */
    public static final String ISE_MESSAGE = "vmhook-on-exception-ISE";

    /** The internal ('/'-separated) name the native callback expects for mode 1/2. */
    public static final String ISE_INTERNAL_NAME = "java/lang/IllegalStateException";

    /** The internal name expected for the mode-3 NumberFormatException. */
    public static final String NFE_INTERNAL_NAME = "java/lang/NumberFormatException";

    /** The internal name expected for the mode-5 checked IOException. */
    public static final String IOE_INTERNAL_NAME = "java/io/IOException";

    /** Internal names of the custom subclasses (mirrored on the native side). */
    public static final String CUSTOM_ERROR_INTERNAL_NAME =
        "vmhook/fixtures/OnException$CustomError";
    public static final String CUSTOM_CHECKED_INTERNAL_NAME =
        "vmhook/fixtures/OnException$CustomChecked";
    public static final String CUSTOM_RUNTIME_INTERNAL_NAME =
        "vmhook/fixtures/OnException$CustomRuntime";

    /** Internal names for the JVM-internal implicit-throw modes. */
    public static final String NPE_INTERNAL_NAME  = "java/lang/NullPointerException";
    public static final String AIOOBE_INTERNAL_NAME =
        "java/lang/ArrayIndexOutOfBoundsException";
    public static final String CCE_INTERNAL_NAME  = "java/lang/ClassCastException";
    public static final String ARITH_INTERNAL_NAME = "java/lang/ArithmeticException";

    /** The nested class whose static initializer throws (mode 16, one-shot). */
    public static final String STATIC_INIT_THROWER =
        "vmhook.fixtures.OnException$StaticInitThrower";

    // ---- Custom Throwable subclasses (each its own klass; '$'-named, so
    //      Main's fixture auto-discovery never force-loads them). ------------

    /** A custom direct subclass of java.lang.Error (mode 6). */
    public static final class CustomError extends Error
    {
        private static final long serialVersionUID = 1L;
        public CustomError(final String message) { super(message); }
    }

    /** A custom CHECKED exception subclass (mode 7). */
    public static final class CustomChecked extends Exception
    {
        private static final long serialVersionUID = 1L;
        public CustomChecked(final String message) { super(message); }
    }

    /** A custom unchecked (RuntimeException) subclass (mode 8). */
    public static final class CustomRuntime extends RuntimeException
    {
        private static final long serialVersionUID = 1L;
        public CustomRuntime(final String message) { super(message); }
    }

    /**
     * A class whose STATIC INITIALIZER throws (mode 16).  Force-loading this with
     * initialize=true runs the &lt;clinit&gt;, whose `throw` constructs an
     * IllegalStateException (fillInStackTrace fires) and propagates as an
     * ExceptionInInitializerError (a second Throwable construction).  ONE-SHOT:
     * once a class fails initialization the JVM marks it erroneous, so a second
     * force of mode 16 yields NoClassDefFoundError, not ExceptionInInitializerError.
     * Never referenced anywhere else, so it is not initialized until the probe.
     */
    public static final class StaticInitThrower
    {
        public static final int BEACON;
        static
        {
            if (System.currentTimeMillis() >= 0L)   // always true; defeats const-fold
            {
                throw new IllegalStateException("vmhook-on-exception-clinit");
            }
            BEACON = 0; // unreachable, but keeps the field definitely-assigned
        }
    }

    /** A class whose CONSTRUCTOR throws (mode 17). */
    public static final class CtorThrower
    {
        public CtorThrower()
        {
            throw new IllegalStateException("vmhook-on-exception-ctor");
        }
    }

    // ---- Throwing actions (each is a genuine construct + athrow + catch) ---

    /**
     * Construct, throw, and catch ONE IllegalStateException.  `throw` is a real
     * athrow of a freshly-built exception, so its constructor's fillInStackTrace
     * runs on this thread right before the throw — the hook (if armed) fires.
     */
    private static void throwOneIse()
    {
        try
        {
            throw new IllegalStateException(ISE_MESSAGE);
        }
        catch (final IllegalStateException ex)
        {
            throwsObserved++;
            lastThrowKind = 1;
            lastThrowMsg = ex.getMessage();
        }
    }

    /** Construct + throw + catch MANY IllegalStateExceptions (distinct athrows). */
    private static void throwManyIse()
    {
        for (int n = 0; n < MANY_THROWS; ++n)
        {
            try
            {
                throw new IllegalStateException(ISE_MESSAGE);
            }
            catch (final IllegalStateException ex)
            {
                throwsObserved++;
                lastThrowKind = 1;
                lastThrowMsg = ex.getMessage();
            }
        }
    }

    /**
     * Construct + throw + catch ONE NumberFormatException.  Built directly (not
     * via Integer.parseInt) so the type is unambiguous and the only Throwable
     * this cycle constructs is the NFE — letting the native side assert the
     * callback reports THIS type and not a stray internal exception.
     */
    private static void throwOneNfe()
    {
        try
        {
            throw new NumberFormatException("vmhook-on-exception-NFE");
        }
        catch (final NumberFormatException ex)
        {
            throwsObserved++;
            lastThrowKind = 2;
            lastThrowMsg = ex.getMessage();
        }
    }

    /** Construct + throw + catch ONE CHECKED java.io.IOException. */
    private static void throwOneChecked()
    {
        try
        {
            throw new java.io.IOException("vmhook-on-exception-IOE");
        }
        catch (final java.io.IOException ex)
        {
            throwsObserved++;
            lastThrowKind = 3;
            lastThrowMsg = ex.getMessage();
        }
    }

    /** Construct + throw + catch ONE custom Error subclass. */
    private static void throwOneCustomError()
    {
        try
        {
            throw new CustomError("vmhook-on-exception-customError");
        }
        catch (final CustomError ex)
        {
            throwsObserved++;
            lastThrowKind = 4;
            lastThrowMsg = ex.getMessage();
        }
    }

    /** Construct + throw + catch ONE custom CHECKED Exception subclass. */
    private static void throwOneCustomChecked()
    {
        try
        {
            throw new CustomChecked("vmhook-on-exception-customChecked");
        }
        catch (final CustomChecked ex)
        {
            throwsObserved++;
            lastThrowKind = 5;
            lastThrowMsg = ex.getMessage();
        }
    }

    /** Construct + throw + catch ONE custom RuntimeException subclass. */
    private static void throwOneCustomRuntime()
    {
        try
        {
            throw new CustomRuntime("vmhook-on-exception-customRuntime");
        }
        catch (final CustomRuntime ex)
        {
            throwsObserved++;
            lastThrowKind = 6;
            lastThrowMsg = ex.getMessage();
        }
    }

    /**
     * Construct ONE ISE, throw + catch it, then RE-THROW the SAME caught instance
     * and catch it again.  There are TWO athrows but only ONE construction, so a
     * construction-path watcher must fire EXACTLY ONCE even though throwsObserved
     * reaches 2.  distinctConstructions records the single construction.
     */
    private static void throwReThrown()
    {
        final IllegalStateException original = new IllegalStateException(ISE_MESSAGE);
        distinctConstructions = 1; // exactly one constructor call above
        try
        {
            throw original;             // first athrow of `original`
        }
        catch (final IllegalStateException first)
        {
            throwsObserved++;
            lastThrowKind = 1;
            lastThrowMsg = first.getMessage();
            try
            {
                throw first;            // SECOND athrow of the SAME instance
            }
            catch (final IllegalStateException second)
            {
                throwsObserved++;
                lastThrowKind = 1;
                lastThrowMsg = second.getMessage();
            }
        }
    }

    // -- Nested-call throw (mode 10): construct several frames deep. ----------
    private static void nestedLevel3()
    {
        throw new IllegalStateException(ISE_MESSAGE);  // constructed 3 frames deep
    }
    private static void nestedLevel2() { nestedLevel3(); }
    private static void nestedLevel1() { nestedLevel2(); }

    private static void throwNested()
    {
        try
        {
            nestedLevel1();
        }
        catch (final IllegalStateException ex)
        {
            throwsObserved++;
            lastThrowKind = 1;
            lastThrowMsg = ex.getMessage();
        }
    }

    // -- JVM-internal implicit throws (modes 11..14). ------------------------
    // Each provokes the JVM to construct the throwable itself.  Under
    // -XX:+OmitStackTraceInFastThrow (default ON) a HOT implicit site may reuse
    // a preallocated instance that skips fillInStackTrace; the native side
    // characterizes these best-effort and always proves the throw ran via the
    // witnesses below.

    @SuppressWarnings("null")
    private static void throwImplicitNpe()
    {
        try
        {
            final int[] nullArray = (System.currentTimeMillis() < 0L) ? new int[1] : null;
            final int len = nullArray.length;   // implicit NPE
            throwsObserved += (len & 0);          // never reached; keeps `len` used
        }
        catch (final NullPointerException ex)
        {
            throwsObserved++;
            lastThrowKind = 7;
            lastThrowMsg = (ex.getMessage() == null) ? "" : ex.getMessage();
        }
    }

    private static void throwImplicitAioobe()
    {
        try
        {
            final int[] one = new int[1];
            final int bad = one[5];               // implicit AIOOBE
            throwsObserved += (bad & 0);
        }
        catch (final ArrayIndexOutOfBoundsException ex)
        {
            throwsObserved++;
            lastThrowKind = 8;
            lastThrowMsg = (ex.getMessage() == null) ? "" : ex.getMessage();
        }
    }

    private static void throwImplicitCce()
    {
        try
        {
            final Object o = "a string";
            final Integer bad = (Integer) o;      // implicit ClassCastException
            throwsObserved += (bad == null ? 0 : 0);
        }
        catch (final ClassCastException ex)
        {
            throwsObserved++;
            lastThrowKind = 9;
            lastThrowMsg = (ex.getMessage() == null) ? "" : ex.getMessage();
        }
    }

    private static void throwImplicitArith()
    {
        try
        {
            final int bad = 7 / zeroField;        // implicit ArithmeticException (/ by zero)
            throwsObserved += (bad & 0);
        }
        catch (final ArithmeticException ex)
        {
            throwsObserved++;
            lastThrowKind = 10;
            lastThrowMsg = (ex.getMessage() == null) ? "" : ex.getMessage();
        }
    }

    /** EXPLICIT `new NullPointerException()` + throw (mode 15): the constructor
     *  ALWAYS runs (no fast-throw preallocation), so the watcher fires reliably —
     *  the dependable contrast to the implicit NPE of mode 11. */
    private static void throwExplicitNpe()
    {
        try
        {
            throw new NullPointerException("vmhook-on-exception-explicitNPE");
        }
        catch (final NullPointerException ex)
        {
            throwsObserved++;
            lastThrowKind = 7;
            lastThrowMsg = ex.getMessage();
        }
    }

    /**
     * Force the static-initializer-throwing class (mode 16).  ONE-SHOT: the first
     * call runs StaticInitThrower's &lt;clinit&gt;, which throws; Class.forName
     * surfaces an ExceptionInInitializerError wrapping the IllegalStateException.
     * A second call would throw NoClassDefFoundError, so the native side runs this
     * mode exactly once.
     */
    private static void throwStaticInit()
    {
        try
        {
            Class.forName(STATIC_INIT_THROWER, true, OnException.class.getClassLoader());
            // Unreachable: the <clinit> always throws.
        }
        catch (final ExceptionInInitializerError err)
        {
            throwsObserved++;
            lastThrowKind = 11;
            lastThrowMsg = (err.getCause() == null) ? "" : err.getCause().getMessage();
        }
        catch (final Throwable t)
        {
            // NoClassDefFoundError (if somehow re-run) or any other surprise:
            // record kind 11 anyway so the native side sees the cycle ran, but
            // leave a distinctive message so a re-run is diagnosable.
            throwsObserved++;
            lastThrowKind = 11;
            lastThrowMsg = "non-EIIE:" + t.getClass().getName();
        }
    }

    /** Construct an object whose CONSTRUCTOR throws (mode 17), caught here. */
    private static void throwFromConstructor()
    {
        try
        {
            final CtorThrower unused = new CtorThrower();
            if (unused != null) { throwsObserved += 0; } // keep `unused` referenced
        }
        catch (final IllegalStateException ex)
        {
            throwsObserved++;
            lastThrowKind = 12;
            lastThrowMsg = ex.getMessage();
        }
    }

    // -- Uncaught-at-throw-site, caught three frames up (mode 18). -----------
    private static void uncaughtInner3() { throw new IllegalStateException(ISE_MESSAGE); }
    private static void uncaughtInner2() { uncaughtInner3(); }  // does NOT catch
    private static void uncaughtInner1() { uncaughtInner2(); }  // does NOT catch

    private static void throwUncaughtThenHandled()
    {
        try
        {
            uncaughtInner1();   // throw is uncaught across 3 frames, caught here
        }
        catch (final IllegalStateException ex)
        {
            throwsObserved++;
            lastThrowKind = 1;
            lastThrowMsg = ex.getMessage();
        }
    }

    private static void runScenario()
    {
        // Reset witnesses at the start of every cycle so each drive() reads only
        // what THIS cycle produced.
        throwsObserved = 0;
        lastThrowKind = 0;
        lastThrowMsg = "";
        distinctConstructions = 0;
        switch (mode)
        {
            case 1:  throwOneIse();             break;
            case 2:  throwManyIse();            break;
            case 3:  throwOneNfe();             break;
            case 4:
                // Deliberately construct NO Throwable: the control cycle.  The
                // native callback (if it were firing) must observe nothing new.
                break;
            case 5:  throwOneChecked();         break;
            case 6:  throwOneCustomError();     break;
            case 7:  throwOneCustomChecked();   break;
            case 8:  throwOneCustomRuntime();   break;
            case 9:  throwReThrown();           break;
            case 10: throwNested();             break;
            case 11: throwImplicitNpe();        break;
            case 12: throwImplicitAioobe();     break;
            case 13: throwImplicitCce();        break;
            case 14: throwImplicitArith();      break;
            case 15: throwExplicitNpe();        break;
            case 16: throwStaticInit();         break;
            case 17: throwFromConstructor();    break;
            case 18: throwUncaughtThenHandled(); break;
            default:                            break;
        }
    }

    static
    {
        Harness.register(new Harness.Probe()
        {
            @Override
            public boolean pending()
            {
                return OnException.go && !OnException.done;
            }

            @Override
            public void run()
            {
                runScenario();
                OnException.done = true;
            }
        });
    }
}

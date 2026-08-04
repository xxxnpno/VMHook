package vmhook.fixtures;

import vmhook.Harness;

/**
 * Fixture for the RESTORED pure-VM method invocation path (area: methods /
 * invocation capability).
 *
 * vmhook::method_proxy::call() dispatches through HotSpot's own call stub,
 * StubRoutines::_call_stub_entry.  That entry has never been published in
 * VMStructs on ANY JDK, so the header's single lookup returned null on every
 * version and call() was dead everywhere; it is now DERIVED from
 * StubRoutines::_call_stub_return_address through four validated tiers.  The
 * native module vmhook/tests/jvm/modules/invocation_capability.cpp guards that
 * derivation and every call shape the previous implementation got wrong.
 *
 * Why a hook + handshake?  call() needs a live JavaThread in _thread_in_Java.
 * The test-suite worker is a detached native thread that is neither, so the
 * module installs a scoped_hook on {@link #trigger()} and performs every
 * invocation from inside that detour -- the supported production path.
 *
 * mode selector (native sets `mode` + clears `done` on the rising edge of go):
 *   1 = round-trip phase.  Warm each callee once from Java (so the native side
 *       never invokes a never-dispatched, unlinked method), then dispatch
 *       trigger() so the detour can drive every shape: object argument +
 *       object return, a 2-slot long, a 2-slot double, an int widened into a J
 *       parameter, a void return, a throwing callee, and a native callee
 *       (java.lang.System.currentTimeMillis) whose JNIHandleBlock watermark
 *       must be restored.
 *   2 = GC phase.  Dispatch trigger() again so the detour can drive
 *       System.gc() THROUGH a synthetic entry frame: the collector walks the
 *       JavaCallWrapper the call built, which the old `link = -1` argument
 *       corrupted (a GC reading ((JavaCallWrapper*)-1)->_anchor took the
 *       process down at address 0x1f).
 *
 * Java 8 syntax only (anonymous Probe class; no var/lambda/switch-expr).
 * ASCII only -- javac on JDK 8 / Windows reads these sources as Cp1252.
 */
public final class InvokeCapability
{
    // -- go / done handshake driven by the native module via run_probe --------
    /** Native raises this to request the probe action; clears it afterwards. */
    public static volatile boolean go;

    /** The probe action sets this true once it has run; native polls it. */
    public static volatile boolean done;

    /** Scenario selector; native sets it on the rising edge of go (1 or 2). */
    public static volatile int mode;

    /** Set true by trigger() so the native scoped_hook has a real dispatch. */
    public static volatile boolean triggerRan;

    /** Incremented by the Java-side warm-up so the native side can see it ran. */
    public static volatile int warmRounds;

    // -- side effects the native module reads back to prove a body RAN -------
    /** Bumped by {@link #bump()} -- the only observable of a void return. */
    public static volatile int voidHits;

    /** Bumped by {@link #boom()} BEFORE it throws (the arg crossed the gate). */
    public static volatile int boomCalls;

    /** Bumped by {@link #echo(String)}. */
    public static volatile int echoCalls;

    /** Bumped by {@link #pair(InvokeCapability)}. */
    public static volatile int pairCalls;

    /** Per-instance payload; lets the native side prove object identity. */
    public int seed;

    /** ()V -- leaves seed at 0. */
    public InvokeCapability()
    {
    }

    /** (I)V -- the ctor vmhook::make_unique drives to build a peer object. */
    public InvokeCapability(final int value)
    {
        this.seed = value;
    }

    /** Hookable instance method; the detour rides a real bytecode dispatch. */
    public void trigger()
    {
        triggerRan = true;
    }

    /**
     * OBJECT argument + OBJECT return: hands the peer straight back, so the
     * native side can compare the returned oop against the one it passed in.
     */
    public InvokeCapability pair(final InvokeCapability peer)
    {
        pairCalls++;
        return peer;
    }

    /**
     * STRING (object) argument + STRING (object) return.  Uses concat rather
     * than `+` so no invokedynamic StringConcatFactory bootstrap runs inside
     * the synthetic entry frame.
     */
    public String echo(final String value)
    {
        echoCalls++;
        return value.concat("-echoed");
    }

    /** 2-slot LONG argument + long return (value lives in the HIGH slot). */
    public long addLong(final long value)
    {
        return value + 1L;
    }

    /**
     * The target for "an int argument widened into a J parameter".  XORs with
     * a constant that touches all eight bytes, so a truncated / mis-slotted
     * argument cannot accidentally produce the expected answer.
     */
    public long widenLong(final long value)
    {
        return value ^ 0x0102030405060708L;
    }

    /** 2-slot DOUBLE argument + double return (value lives in the HIGH slot). */
    public double halve(final double value)
    {
        return value / 2.0;
    }

    /** VOID return -- observable only through {@link #voidHits}. */
    public void bump()
    {
        voidHits++;
    }

    /** A callee that THROWS; the native side asserts it is surfaced + cleared. */
    public int boom()
    {
        boomCalls++;
        throw new IllegalStateException("invoke-capability-boom");
    }

    /**
     * Dispatches every callee once from Java so none of them is a
     * never-dispatched (lazy-link-stub) method when the native side invokes
     * it.  Cheap, and it keeps the native assertions about the RESULT rather
     * than about linkage.
     */
    private static void warm()
    {
        final InvokeCapability probe = new InvokeCapability(1);
        final InvokeCapability peer = new InvokeCapability(2);
        if (probe.pair(peer) != peer)
        {
            throw new IllegalStateException("unreachable");
        }
        if (!probe.echo("w").equals("w-echoed"))
        {
            throw new IllegalStateException("unreachable");
        }
        if (probe.addLong(1L) != 2L)
        {
            throw new IllegalStateException("unreachable");
        }
        if (probe.widenLong(0L) != 0x0102030405060708L)
        {
            throw new IllegalStateException("unreachable");
        }
        if (probe.halve(4.0) != 2.0)
        {
            throw new IllegalStateException("unreachable");
        }
        probe.bump();
        try
        {
            probe.boom();
        }
        catch (final IllegalStateException expected)
        {
            // expected -- the warm-up only needs the body to have been linked
        }
        if (System.currentTimeMillis() == 0L)
        {
            throw new IllegalStateException("unreachable");
        }
        // Undo the warm-up's side effects so the native side reads counters
        // that reflect ONLY its own invocations.
        voidHits = 0;
        boomCalls = 0;
        echoCalls = 0;
        pairCalls = 0;
        warmRounds++;
    }

    // -- Probe self-registration ---------------------------------------------
    static
    {
        Harness.register(new Harness.Probe()
        {
            @Override
            public boolean pending()
            {
                return InvokeCapability.go && !InvokeCapability.done;
            }

            @Override
            public void run()
            {
                if (InvokeCapability.mode == 1)
                {
                    InvokeCapability.warm();
                }
                // Real bytecode dispatch the native scoped_hook rides; the
                // detour performs the invocations while a JavaThread is live.
                new InvokeCapability().trigger();
                InvokeCapability.done = true;
            }
        });
    }
}

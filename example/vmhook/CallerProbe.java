package vmhook;

/**
 * Call-chain probe target.  Verifies that vmhook can, from inside a hook
 * on {@link #innerStep(int)}, walk the saved-rbp chain on the interpreter
 * stack and identify {@link #outerStep(int)} as the calling method.
 *
 * The two methods are deliberately small so HotSpot keeps them in the
 * interpreter for long enough to observe the relationship; the suite uses
 * -XX:+UseSerialGC implicitly via the default-VM choice for stability
 * (no aggressive deopt cycles).
 */
public class CallerProbe
{
    // Probe-coordination fields read by the C++ side.
    public static volatile boolean probeRequested = false;
    public static volatile boolean probeDone      = false;
    public static volatile int     observedSum    = 0;

    /** Outer method — calls innerStep so the latter has a known caller. */
    public int outerStep(final int x)
    {
        return this.innerStep(x + 1) * 10;
    }

    /** Inner method — vmhook hooks this one and asks for the caller info. */
    public int innerStep(final int x)
    {
        return x * 2;
    }
}

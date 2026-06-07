package vmhook;

/**
 * Field-change probe target.  The Java side runs a tight loop that
 * increments {@link #counter} once per millisecond while the C++ side
 * has a field-watcher registered on it.  The watcher should observe
 * monotonically increasing values.
 */
public class TickerProbe
{
    public static volatile boolean probeRequested = false;
    public static volatile boolean probeDone      = false;

    /** Counter that the C++ field watcher observes. */
    public static volatile int counter = 0;

    /** Bumps the counter once.  Called by Main on every probe iteration. */
    public static void tick()
    {
        counter++;
    }
}

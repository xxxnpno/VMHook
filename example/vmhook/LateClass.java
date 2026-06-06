package vmhook;

/**
 * Class-load probe target.  This class exists in the build output but is
 * never referenced by the rest of the harness, so HotSpot does not load
 * it during startup.  The probe deliberately triggers loading via
 * {@code Class.forName("vmhook.LateClass")} so the C++ side's class-load
 * watcher sees a new klass appear in the ClassLoaderDataGraph.
 */
public class LateClass
{
    public static int beacon = 0xC0FFEE;

    public int instanceValue;

    public LateClass(final int v)
    {
        this.instanceValue = v;
    }
}

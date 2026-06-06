package vmhook.fixtures;

import vmhook.Harness;

/**
 * Fixture for the register_class feature: vmhook::register_class&lt;T&gt;(name) and the
 * type-&gt;class map / wrapper-factory machinery (vmhook.hpp:1440 type_to_class_map,
 * 1462 g_type_factory_map, 6915-6952 register_class&lt;T&gt;).
 *
 * register_class is almost entirely native-side: it associates a C++ wrapper type
 * with an internal JVM class name, verifies the class is loaded (find_class), and
 * stores a factory lambda used to build the wrapper from a decoded OOP.  The
 * native module proves, all from C++:
 *
 *   - a registered wrapper -&gt; static_field / get_class_methods / find_class
 *     resolve (the type_to_class_map -&gt; find_class path),
 *   - an UNREGISTERED wrapper type -&gt; every accessor returns nullopt/empty
 *     (type_to_class_map miss), with NO crash,
 *   - re-registering the SAME name to the SAME / a DIFFERENT wrapper type
 *     (idempotent vs last-wins -- characterised),
 *   - two DIFFERENT wrappers bound to two DIFFERENT classes both resolve,
 *   - a BOGUS class name -&gt; register_class returns false and leaves the type
 *     unregistered (accessors return nullopt; no crash),
 *   - the FACTORY map is exercised by the ONE Java-coordinated angle below.
 *
 * The single Java interaction anchors a LIVE instance: the native module installs
 * a scoped_hook on {@link #anchor(int)} whose detour receives the receiver as a
 * std::unique_ptr&lt;wrapper&gt; -- the ONLY code path that goes through the registered
 * FACTORY (vmhook.hpp:7488-7508 extract_frame_arg builds the wrapper via
 * g_type_factory_map[class]).  Reading the anchored instance's own field back
 * inside the detour proves a made/decoded object decodes to the REGISTERED wrapper
 * type (correct klass, correct field offsets).
 *
 * JAVA 8 SOURCE (compiles under javac 8 AND 25): no var/records/switch-expr/
 * text-blocks/sealed/List.of/Stream.toList/post-8 API.
 */
public final class RegisterClassFix
{
    // -- go / done handshake driven by the native module via run_probe ------
    public static volatile boolean go;
    public static volatile boolean done;

    // A distinctive instance field the detour reads back THROUGH the wrapper the
    // factory built -- proves the decoded oop is wrapped as the registered type
    // and the wrapper's field offsets resolve against the right klass.
    public int marker = 0x5AFE7A11;          // 1526182929

    // A static field the native side resolves via static_field(...) to prove the
    // registered wrapper -> static accessor path.
    public static int classToken = 0x1357BD13; // 323158291

    // Witness: how many times the anchored method ran (Java-visible), so the
    // native side can correlate fire-count with real dispatch.
    public static volatile int anchorCalls;

    // The exact argument the probe feeds anchor(), echoed for an arg-decode check.
    public static final int ANCHOR_ARG = 0x0CA75;  // 51829

    // Held so the probe always dispatches on a known, stable receiver whose
    // `marker` the native detour validates.
    public static final RegisterClassFix INSTANCE = new RegisterClassFix();

    /**
     * The hookable instance method the native module anchors.  Returns
     * marker + delta so the original-body allow-through is observable, and bumps
     * a Java-visible call counter.  Real invokevirtual bytecode dispatch fires
     * the interpreter hook, whose detour decodes `this` via the registered
     * wrapper FACTORY.
     */
    public int anchor(final int delta)
    {
        anchorCalls = anchorCalls + 1;
        return this.marker + delta;
    }

    static
    {
        Harness.register(new Harness.Probe()
        {
            @Override
            public boolean pending()
            {
                return RegisterClassFix.go && !RegisterClassFix.done;
            }

            @Override
            public void run()
            {
                // Real bytecode dispatch on the held instance: fires the
                // interpreter hook so the detour runs on the Java thread and
                // decodes `this` through the registered factory.
                RegisterClassFix.INSTANCE.anchor(ANCHOR_ARG);
                RegisterClassFix.done = true;
            }
        });
    }
}

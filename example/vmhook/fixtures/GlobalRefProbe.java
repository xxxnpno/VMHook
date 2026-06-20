package vmhook.fixtures;

import vmhook.Harness;

/**
 * Fixture for the vmhook::jni::global_ref feature (area: jni / GC lifetime).
 *
 * global_ref is the move-only RAII pin that keeps a Java object alive across a
 * relocating garbage collection: it promotes a raw decoded OOP to a JNI global
 * reference (NewGlobalRef) and its .oop() re-derives the object's CURRENT
 * (post-relocation) heap address out of the handle slot on every call.  The
 * native module proves the pin survives an explicit System.gc() and that the
 * sentinel field stays readable THROUGH .oop() afterwards.
 *
 * Why a hook + handshake?  Every JNI-touching step the module performs
 * (vmhook::make_unique to allocate the object, the NewGlobalRef the pin's
 * constructor issues, the DeleteGlobalRef its reset()/destructor issues) needs
 * a live JavaThread with an attached JNIEnv.  The test-suite worker runs on a
 * detached native thread that has none, so the module installs a scoped_hook on
 * {@link #trigger()} and does all of that work from inside the detour — exactly
 * the shape the sibling MakeUnique fixture uses.  The GC itself must run as real
 * Java bytecode, so the probe drives System.gc() here (mode 2) rather than from
 * native.
 *
 * mode selector (native sets `mode` + clears `done` on the rising edge of go):
 *   1 = build/pin phase.  Just dispatch trigger() so the native detour can
 *       make_unique a GlobalRefProbe, pin it, run the move-only / null-empty
 *       checks, read the sentinel through .oop() once, then drop its wrapper.
 *       The surviving pin lives in a native file-scope global_ref.
 *   2 = survive-GC phase.  Force System.gc() several times (a relocating
 *       young/full collection may move the still-pinned object), THEN dispatch
 *       trigger() again so the native detour — now running post-GC on a live
 *       JavaThread — re-reads BOTH the primitive sentinel AND the embedded `tag`
 *       object reference through the SAME pin's .oop(), confirms the pin's raw
 *       handle is byte-identical pre/post-GC (a relocating collector moves the
 *       slot contents, never the handle), and finally releases it.  The handle
 *       check holds across every collector; the live-oop re-reads are gated
 *       native-side on the post-GC address being safely re-derivable.
 *
 * Java 8 syntax only (anonymous Probe class; no var/lambda/switch-expr).
 */
public final class GlobalRefProbe
{
    // ── go / done handshake driven by the native module via run_probe ──────────
    /** Native raises this to request the probe action; clears it afterwards. */
    public static volatile boolean go;

    /** The probe action sets this true once it has run; native polls it. */
    public static volatile boolean done;

    /** Scenario selector; native sets it on the rising edge of go (1 or 2). */
    public static volatile int mode;

    /** How many times the survive-GC phase has driven System.gc(). */
    public static volatile int gcRounds;

    /** Set true by trigger() so the native scoped_hook has a real dispatch. */
    public static volatile boolean triggerRan;

    // ── The pinned object's payload ─────────────────────────────────────────────
    /**
     * Sentinel the native side reads back THROUGH global_ref.oop() before and
     * after GC.  Stamped by the (I)V constructor so make_unique's NewObjectA
     * path runs a real <init> and the value is genuinely Java-initialised, not
     * default-zeroed.
     */
    public int sentinel;

    /**
     * A reference-typed payload the native side ALSO reads back through
     * global_ref.oop() — proving the pin tracks the object well enough to chase
     * an embedded object field (not just a primitive).  Stamped by the (I)V
     * constructor to a fixed, sentinel-independent constant so the native module
     * can assert it byte-for-byte via read_java_string(.oop()-derived field).
     * The ()V path leaves it null, which the native null-payload check relies on.
     *
     * Read back TWICE by the native module: once pre-GC in phase 1 (proving the
     * fresh pin chases an object reference) and again post-GC in phase 2 (proving
     * the embedded OBJECT reference — not merely the primitive sentinel — survives
     * a relocating collection through the SAME global ref).  Both reads compare
     * against {@link #TAG_VALUE}; the post-GC read is gated native-side on the oop
     * being safely re-derivable (a relocating G1 may leave it non-dereferenceable,
     * which the native module records as informational rather than asserting).
     */
    public String tag;

    /** The exact bytes the native module expects {@link #tag} to hold after the (I)V ctor. */
    public static final String TAG_VALUE = "pinned-tag";

    /** ()V — leaves sentinel at 0 and tag at null (the no-arg / default-payload path). */
    public GlobalRefProbe()
    {
    }

    /** (I)V — stamps the sentinel AND the reference-typed tag the native module pins and verifies. */
    public GlobalRefProbe(final int value)
    {
        this.sentinel = value;
        this.tag = TAG_VALUE;
    }

    // ── Probe self-registration ─────────────────────────────────────────────────
    static
    {
        Harness.register(new Harness.Probe()
        {
            @Override
            public boolean pending()
            {
                return GlobalRefProbe.go && !GlobalRefProbe.done;
            }

            @Override
            public void run()
            {
                if (GlobalRefProbe.mode == 2)
                {
                    // Force a handful of collections so a relocating collector
                    // (serial / G1, as the CI uses) has every chance to MOVE the
                    // still-pinned object before the native side re-reads it.
                    // Allocate churn between gc()s to dirty the young gen.
                    for (int round = 0; round < 4; round++)
                    {
                        final byte[] churn = new byte[1 << 16];
                        if (churn.length == 0)
                        {
                            throw new IllegalStateException("unreachable");
                        }
                        System.gc();
                        GlobalRefProbe.gcRounds++;
                    }
                }
                // Real bytecode dispatch the native scoped_hook rides; the detour
                // does the build/pin (mode 1) or the post-GC read + release
                // (mode 2) while a JavaThread is guaranteed live.
                new GlobalRefProbe().trigger();
                GlobalRefProbe.done = true;
            }
        });
    }

    /**
     * Hookable instance method.  The native module hooks this; its detour runs
     * the global_ref work while a JavaThread (and an attached JNIEnv) is live.
     */
    public void trigger()
    {
        triggerRan = true;
    }
}

package vmhook.fixtures;

import java.lang.reflect.Modifier;

import vmhook.Harness;

/**
 * Fixture for the klass_introspection feature (area: classes / klass shape) —
 * the SHAPES companion to {@link KlassIntrospection}.
 *
 * {@code KlassIntrospection} already pins the staple klass-metadata reads on the
 * common, javac-stable shapes (normal / interface / abstract / final / enum /
 * annotation / static-nested / 3-level chain / generic-bridge / 1-D & 2-D array /
 * java.lang.Object).  This fixture deliberately covers the shapes that one does
 * NOT have a fixed {@code $}-name for, or that need a witness the other fixture
 * never publishes, so the native module {@code klass_introspection_shapes} can
 * make "every Klass metadata field read through the library" EXHAUSTIVE:
 *
 *   NAME-FORM shapes (binary {@code .}-name vs internal {@code /}-name, with the
 *   runtime name PUBLISHED by Java so the native side resolves it dynamically and
 *   never hardcodes an unstable ordinal):
 *     - ANONYMOUS class   ({@code KlassIntrospectionShapes$1}, a Runnable),
 *     - LOCAL class       ({@code KlassIntrospectionShapes$1LocalShape}),
 *     - LAMBDA class      (a hidden/synthetic class on modern JDKs; its name is
 *                          published — the native side resolves it best-effort and
 *                          records reachability when the class is not in the
 *                          ClassLoaderDataGraph walk, e.g. a JDK15+ hidden class),
 *     - non-static INNER  ({@code KlassIntrospectionShapes$InnerShape}, with a
 *                          synthetic {@code this$0}).
 *
 *   KIND / FLAG shapes the other fixture does not carry:
 *     - a class implementing SEVERAL interfaces ({@code MultiImpl} implements
 *       {@code IA}, {@code IB}, {@code IC}) — the declared-interface LIST,
 *     - a SYNTHETIC member witness (the generic bridge {@code compareTo} on
 *       {@code CmpShape} is ACC_SYNTHETIC; published so the native side can pin
 *       the ACC_SYNTHETIC bit it reads on a method against the Java witness),
 *     - boxed-PRIMITIVE wrappers ({@code Integer} / {@code Long} / {@code Boolean}
 *       / {@code Character}) — bootstrap-loaded; the native side reads their name
 *       (binary form), super, and final/non-abstract/non-interface flags and
 *       cross-checks the super against the Java witness,
 *     - a RECORD supertype ({@code java.lang.Record}, JDK16+) — there is NO
 *       {@code record} in this source (it must compile on JDK 8), so the record
 *       SHAPE is exercised through {@code java.lang.Record} itself: the probe
 *       force-loads it when the running JDK has records, and the native side
 *       resolves {@code java/lang/Record} and pins its super == java.lang.Object.
 *
 *   ARRAY shapes the other fixture does not carry:
 *     - a 1-D REFERENCE array     ({@code String[]}  ->  {@code [Ljava/lang/String;}),
 *     - a 3-D PRIMITIVE array     ({@code int[][][]} ->  {@code [[[I}),
 *     - a 1-D array of the FIXTURE type (an app-loaded element klass) so the
 *       element-klass derivation crosses the bootstrap/app loader boundary.
 *   For every array the COMPONENT (element) type and the DIMENSION are derived by
 *   the native side from the name and corroborated by a Java witness, and the
 *   element klass is resolved via find_class on the stripped descriptor.
 *
 *   LOADER shapes:
 *     - bootstrap-loaded klasses ({@code java.lang.Object}, the boxes) vs
 *       app-loaded klasses (this fixture + its nested types).  The probe publishes
 *       each class's loader name (or "bootstrap" for a null loader) so the native
 *       side can assert the boundary.
 *
 * Ground truth: the probe computes, on the Java thread (real bytecode), each
 * shape's {@code Class.getModifiers()} / derived booleans / super name / loader
 * name / runtime class name and stores them in static witnesses the native module
 * cross-checks its direct klass reads against.  Class literals + force
 * instantiations in {@code <clinit>} also LOAD the nested/inner shapes (the
 * harness loader only {@code forName}s top-level fixtures), so find_class can
 * resolve them.
 *
 * JAVA 8 SYNTAX ONLY: anonymous + local classes (no lambda for the Probe), a
 * single lambda only via {@code Runnable} assignment (legal since Java 8), no var
 * / records / switch-expressions / text-blocks.  java.* + vmhook.Harness only.
 */
public final class KlassIntrospectionShapes
{
    /** Native sets this true to request the action; the action clears done. */
    public static volatile boolean go;

    /** The action sets this true when it has run; native polls it. */
    public static volatile boolean done;

    /** Scenario selector.  0 = (re)publish every witness, then tick(). */
    public static volatile int mode;

    // =======================================================================
    //  KIND / FLAG SHAPES
    // =======================================================================

    /** Three independent interfaces a single class can implement together. */
    public interface IA { int aOp(); }
    public interface IB { int bOp(); }
    public interface IC { int cOp(); }

    /** A class implementing SEVERAL interfaces — the declared-interface LIST. */
    public static final class MultiImpl implements IA, IB, IC
    {
        @Override public int aOp() { return 1; }
        @Override public int bOp() { return 2; }
        @Override public int cOp() { return 3; }
    }

    /** Generic-bridge class: javac emits a SYNTHETIC bridge
     *  compareTo(Ljava/lang/Object;)I beside the typed one — used to pin the
     *  ACC_SYNTHETIC bit the native side reads on a method. */
    public static final class CmpShape implements Comparable<CmpShape>
    {
        public int key;

        @Override
        public int compareTo(final CmpShape other)
        {
            return Integer.compare(this.key, other.key);
        }
    }

    /** Non-static INNER class (javac injects a synthetic this$0 -> outer). */
    public class InnerShape
    {
        public int innerField = 7;

        public int innerMethod()
        {
            return this.innerField;
        }
    }

    // =======================================================================
    //  WITNESS STORAGE (published by the probe via real bytecode)
    // =======================================================================

    // ---- runtime class NAMES (binary '.'-form, exactly Class.getName()) -----
    // The native side converts '.'->'/' to get the internal form it resolves
    // through find_class, and converts the resolved klass's internal name back
    // to '.'-form to assert the binary-name derivation.
    public static volatile String anonBinaryName;     // ...$1
    public static volatile String localBinaryName;    // ...$1LocalShape
    public static volatile String lambdaBinaryName;   // hidden/synthetic
    public static volatile String innerBinaryName;    // ...$InnerShape
    public static volatile String multiImplBinaryName;// ...$MultiImpl
    public static volatile String cmpBinaryName;      // ...$CmpShape

    // NOTE on array names: Class.getName() returns the descriptor in '.'-form
    // (e.g. "[Ljava.lang.String;"), but HotSpot's Klass::get_name() symbol uses
    // '/'-form ("[Ljava/lang/String;").  To avoid that dot/slash mismatch the
    // native side asserts array names against the '/'-form descriptor literal
    // directly and cross-checks only the COMPONENT name (below) against Java.

    // ---- array component / dimension witnesses ------------------------------
    public static volatile String strArrayComponentName;  // java.lang.String
    public static volatile int    int3DArrayDim;          // 3
    public static volatile String int3DArrayLeafComponent;// int  (Class.getName())
    public static volatile String selfArrayComponentName; // vmhook.fixtures.KlassIntrospectionShapes

    // ---- super NAMES (internal '/'-form) ------------------------------------
    public static volatile String integerSuperName;   // java/lang/Number
    public static volatile String longSuperName;      // java/lang/Number
    public static volatile String booleanSuperName;   // java/lang/Object
    public static volatile String characterSuperName; // java/lang/Object
    public static volatile String multiImplSuperName; // java/lang/Object
    public static volatile String innerSuperName;     // java/lang/Object
    public static volatile String recordSuperName;    // java/lang/Object   (JDK16+)

    // ---- modifiers / derived booleans ---------------------------------------
    public static volatile int     integerMods;
    public static volatile int     booleanMods;
    public static volatile int     multiImplMods;
    public static volatile int     innerMods;
    public static volatile int     cmpMods;
    public static volatile boolean integerIsFinal;
    public static volatile boolean integerIsAbstract;
    public static volatile boolean integerIsInterface;
    public static volatile boolean strArrayIsArray;
    public static volatile boolean int3DIsArray;
    public static volatile boolean selfArrayIsArray;
    public static volatile boolean innerIsMemberAndNotStatic;

    // ---- declared-interface witnesses ---------------------------------------
    public static volatile int     multiImplInterfaceCount;   // 3
    public static volatile boolean multiImplImplementsIA;
    public static volatile boolean multiImplImplementsIB;
    public static volatile boolean multiImplImplementsIC;

    // ---- synthetic-bridge witness -------------------------------------------
    public static volatile boolean cmpBridgeIsSynthetic;      // the Object-bridge
    public static volatile boolean cmpTypedIsNotSynthetic;    // the typed compareTo

    // ---- loader witnesses (null loader -> "bootstrap") ----------------------
    public static volatile String objectLoaderName;   // bootstrap
    public static volatile String integerLoaderName;  // bootstrap
    public static volatile String selfLoaderName;     // app loader (non-empty)
    public static volatile String multiImplLoaderName;// app loader (== selfLoaderName)
    public static volatile boolean objectLoaderIsBootstrap;
    public static volatile boolean selfLoaderIsApp;

    // ---- record availability ------------------------------------------------
    public static volatile boolean recordSupported;   // JDK16+ has java.lang.Record

    // ---- sanity tick --------------------------------------------------------
    public static volatile int     tickWitness;

    // =======================================================================
    //  LIVE INSTANCES (force-load the unstable-name shapes so find_class can
    //  resolve them; held so the klasses stay loaded).
    // =======================================================================
    public static final KlassIntrospectionShapes SELF = new KlassIntrospectionShapes();

    public int selfMarker = 1234;

    /** Anonymous class instance (a Runnable); name is unstable -> published. */
    public Object makeAnonymous()
    {
        return new Runnable()
        {
            @Override
            public void run()
            {
                KlassIntrospectionShapes.this.selfMarker += 0;
            }
        };
    }

    /** Local class instance; name is unstable -> published. */
    public Object makeLocal()
    {
        class LocalShape
        {
            int localValue = 99;

            int readMarkerPlusLocal()
            {
                return KlassIntrospectionShapes.this.selfMarker + this.localValue;
            }
        }
        return new LocalShape();
    }

    /** Lambda instance.  On Java 8 this is a synthetic {@code $$Lambda$} class;
     *  on JDK 15+ it is a hidden class.  Its name is published; reachability via
     *  find_class is best-effort (hidden classes are not in the CLD walk). */
    public Runnable makeLambda()
    {
        return () -> this.selfMarker += 0;
    }

    public static final Object   anonInst   = SELF.makeAnonymous();
    public static final Object   localInst  = SELF.makeLocal();
    public static final Runnable lambdaInst = SELF.makeLambda();
    public static final InnerShape innerInst = SELF.new InnerShape();
    public static final MultiImpl  multiImplInst = new MultiImpl();
    public static final CmpShape   cmpInst       = new CmpShape();

    // Array instances (so the array klasses are loaded and reachable).
    public static final String[]  strArrayInst = new String[2];
    public static final int[][][]  int3DArrayInst = new int[1][1][1];
    public static final KlassIntrospectionShapes[] selfArrayInst =
        new KlassIntrospectionShapes[1];

    // Force-load anchors for the kind/flag shapes that have no instance touch.
    static final Class<?> ANCHOR_IA  = IA.class;
    static final Class<?> ANCHOR_IB  = IB.class;
    static final Class<?> ANCHOR_IC  = IC.class;

    /** Real bytecode dispatch each probe cycle (parity with other fixtures). */
    public int tick(final int nonce)
    {
        return nonce + 1;
    }

    /** Internal '/'-name of c's superclass, or "" when c has no superclass. */
    private static String superInternalName(final Class<?> c)
    {
        final Class<?> s = c.getSuperclass();
        return (s == null) ? "" : s.getName().replace('.', '/');
    }

    /** Loader display name, or "bootstrap" for the null (bootstrap) loader. */
    private static String loaderName(final Class<?> c)
    {
        final ClassLoader cl = c.getClassLoader();
        return (cl == null) ? "bootstrap" : cl.toString();
    }

    /** Whether the named method on c is ACC_SYNTHETIC, matching the given param. */
    private static boolean syntheticCompareTo(final Class<?> c, final boolean wantObjectParam)
    {
        final java.lang.reflect.Method[] ms = c.getDeclaredMethods();
        for (int i = 0; i < ms.length; i++)
        {
            if (!ms[i].getName().equals("compareTo"))
            {
                continue;
            }
            final Class<?>[] ps = ms[i].getParameterTypes();
            final boolean isObjectParam = (ps.length == 1 && ps[0] == Object.class);
            if (isObjectParam == wantObjectParam)
            {
                return ms[i].isSynthetic();
            }
        }
        return false;
    }

    static
    {
        Harness.register(new Harness.Probe()
        {
            @Override
            public boolean pending()
            {
                return KlassIntrospectionShapes.go && !KlassIntrospectionShapes.done;
            }

            @Override
            public void run()
            {
                // ---- runtime class names (binary form) ----------------------
                anonBinaryName      = anonInst.getClass().getName();
                localBinaryName     = localInst.getClass().getName();
                lambdaBinaryName    = lambdaInst.getClass().getName();
                innerBinaryName     = InnerShape.class.getName();
                multiImplBinaryName = MultiImpl.class.getName();
                cmpBinaryName       = CmpShape.class.getName();

                // ---- array component/dimension (component name in '.'-form) --
                strArrayComponentName = String[].class.getComponentType().getName();
                selfArrayComponentName =
                    KlassIntrospectionShapes[].class.getComponentType().getName();
                {
                    int dim = 0;
                    Class<?> c = int[][][].class;
                    while (c.isArray())
                    {
                        dim++;
                        c = c.getComponentType();
                    }
                    int3DArrayDim = dim;                 // 3
                    int3DArrayLeafComponent = c.getName(); // int
                }

                // ---- super names --------------------------------------------
                integerSuperName   = superInternalName(Integer.class);
                longSuperName      = superInternalName(Long.class);
                booleanSuperName   = superInternalName(Boolean.class);
                characterSuperName = superInternalName(Character.class);
                multiImplSuperName = superInternalName(MultiImpl.class);
                innerSuperName     = superInternalName(InnerShape.class);

                // ---- modifiers / derived booleans ---------------------------
                integerMods   = Integer.class.getModifiers();
                booleanMods   = Boolean.class.getModifiers();
                multiImplMods = MultiImpl.class.getModifiers();
                innerMods     = InnerShape.class.getModifiers();
                cmpMods       = CmpShape.class.getModifiers();

                integerIsFinal     = Modifier.isFinal(Integer.class.getModifiers());
                integerIsAbstract  = Modifier.isAbstract(Integer.class.getModifiers());
                integerIsInterface = Integer.class.isInterface();
                strArrayIsArray    = String[].class.isArray();
                int3DIsArray       = int[][][].class.isArray();
                selfArrayIsArray   = KlassIntrospectionShapes[].class.isArray();
                innerIsMemberAndNotStatic =
                    InnerShape.class.isMemberClass()
                    && !Modifier.isStatic(InnerShape.class.getModifiers());

                // ---- declared interfaces ------------------------------------
                final Class<?>[] mi = MultiImpl.class.getInterfaces();
                multiImplInterfaceCount = mi.length;
                boolean ia = false;
                boolean ib = false;
                boolean ic = false;
                for (int i = 0; i < mi.length; i++)
                {
                    if (mi[i] == IA.class) { ia = true; }
                    if (mi[i] == IB.class) { ib = true; }
                    if (mi[i] == IC.class) { ic = true; }
                }
                multiImplImplementsIA = ia;
                multiImplImplementsIB = ib;
                multiImplImplementsIC = ic;

                // ---- synthetic-bridge witness -------------------------------
                cmpBridgeIsSynthetic   = syntheticCompareTo(CmpShape.class, true);
                cmpTypedIsNotSynthetic = !syntheticCompareTo(CmpShape.class, false);

                // ---- loaders ------------------------------------------------
                objectLoaderName   = loaderName(Object.class);
                integerLoaderName  = loaderName(Integer.class);
                selfLoaderName     = loaderName(KlassIntrospectionShapes.class);
                multiImplLoaderName = loaderName(MultiImpl.class);
                objectLoaderIsBootstrap = (Object.class.getClassLoader() == null);
                selfLoaderIsApp =
                    (KlassIntrospectionShapes.class.getClassLoader() != null);

                // ---- record supertype (JDK16+) ------------------------------
                boolean haveRecord = false;
                String recSuper = "";
                try
                {
                    final Class<?> rec = Class.forName("java.lang.Record");
                    haveRecord = true;
                    recSuper = superInternalName(rec);   // java/lang/Object
                }
                catch (final Throwable t)
                {
                    haveRecord = false;
                }
                recordSupported = haveRecord;
                recordSuperName = recSuper;

                // ---- real bytecode dispatch (parity) ------------------------
                tickWitness = new KlassIntrospectionShapes().tick(41);

                KlassIntrospectionShapes.done = true;
            }
        });
    }
}

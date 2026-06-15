package vmhook.fixtures;

import vmhook.Harness;

/**
 * Fixture for the instanceklass_methods_walk feature (area: methods / klass).
 *
 * The live-JVM authority for the PRIMITIVE under hook<T>() /
 * get_class_methods() / find_methods_by_signature<T>() / deoptimize_methods_if()
 * / static_method()->call() overload re-selection: the raw walk of
 * {@code InstanceKlass::_methods} (an {@code Array<Method*>}) into the class's
 * DECLARED (name, JVM-descriptor) set -- no JNI, no JVMTI.  The library reads
 * the array LENGTH at offset 0 (clamped to [0,65535]) and the DATA at offset 8,
 * decodes each {@code Method*} via {@code get_name()}/{@code get_signature()} ->
 * {@code Symbol::to_string()}, and skips any slot failing {@code is_valid_pointer}.
 *
 * Unlike find_methods_by_signature's fixture (which is descriptor-shaped), THIS
 * fixture is shaped to exercise the WALK's OWN edge behaviours across method
 * SHAPES and across CLASS SHAPES, with the EXACT declared (name, descriptor) set
 * of each type known and JDK-stable (verified with `javap -s -p`).  Every method
 * body returns a constant / trivial value -- behaviour is irrelevant; only the
 * declared shape matters.
 *
 * --------------------------------------------------------------------------
 * TOP-LEVEL CLASS  vmhook.fixtures.MethodsWalk  (internal: vmhook/fixtures/MethodsWalk)
 *   The "many methods + every method shape" anchor.  Its DECLARED user methods
 *   (verified by javap -s -p, JDK-stable across 8..26) are EXACTLY:
 *
 *     STATIC vs INSTANCE:
 *       int    si(int)                       (I)I        static
 *       int    ii(int)                       (I)I        instance   (shares (I)I)
 *     NATIVE (declared; never linked/called -- only its (name,desc) is walked):
 *       int    nat(int)                      (I)I        native     (shares (I)I)
 *     FINAL / SYNCHRONIZED modifiers (do NOT change the descriptor):
 *       int    fin(int)                      (I)I        final      (shares (I)I)
 *       void   syncM()                       ()V         synchronized
 *       double strictM(double)               (D)D
 *     VARARGS (compiles to an ARRAY arg -- the descriptor is the array form):
 *       int    varargs(int[])                ([I)I       (Java: int... )
 *     OVERLOADS (same name, distinct descriptors):
 *       int    ov(int)                       (I)I
 *       long   ov(long)                      (J)J
 *       java.lang.String ov(java.lang.String)
 *                                            (Ljava/lang/String;)Ljava/lang/String;
 *     GENERIC-ERASED (type param erases to its bound / Object in the descriptor):
 *       java.lang.Object gen(java.lang.Object)
 *                                            (Ljava/lang/Object;)Ljava/lang/Object;
 *       java.lang.Comparable genB(java.lang.Comparable)
 *                                            (Ljava/lang/Comparable;)Ljava/lang/Comparable;
 *     EVERY PRIMITIVE in ONE descriptor (boolean,byte,char,short,int,long,float,double):
 *       void   allPrims(boolean,byte,char,short,int,long,float,double)
 *                                            (ZBCSIJFD)V
 *     DEEPLY-NESTED / MULTI-DIM reference + array descriptor:
 *       void   deep([[[Ljava/lang/String;)   ([[[Ljava/lang/String;)V
 *     LONG NAME (84 ASCII chars, well under the 0x1000 symbol clamp):
 *       void   m_<80 x 'x'>()                ()V         (name length probed exactly)
 *     UNICODE NAME (legal Java identifier, BMP letters -> modified-UTF-8 bytes):
 *       void   méthodé()           ()V         ("méthodé")
 *       void   名前()                ()V         ("名前", Japanese)
 *     SYNTHETIC the walk MUST include:
 *       <init>  ()V    (the no-arg constructor -- a real _methods entry)
 *       <clinit> ()V   (the static initializer -- exists: static field inits + block)
 *
 *   These named methods are present on EVERY JDK.  javac MAY additionally emit a
 *   bridge method for the generic override in Sub (see below) and (JDK 8 only) an
 *   access$NNN synthetic; those are characterised [INFO] by the native module,
 *   never hard-asserted, so the synthetic delta cannot break CI.
 *
 * --------------------------------------------------------------------------
 * NESTED TYPES (each force-loaded; resolved by its internal `$`-name):
 *
 *   MethodsWalk$Empty           -- a class with NO user-declared methods.  Its
 *                                  _methods array therefore holds ONLY the
 *                                  synthetic <init> ()V (every class has one).
 *                                  Pins "minimal but non-empty" enumeration.
 *
 *   MethodsWalk$Marker          -- a MARKER INTERFACE with ZERO methods.  Its
 *                                  _methods array is EMPTY (interfaces get no
 *                                  <init>).  Pins "empty enumeration" -- and is,
 *                                  by design, indistinguishable from an
 *                                  unregistered wrapper (flaw #6, documented).
 *
 *   MethodsWalk$Iface           -- an interface with an ABSTRACT method, a
 *                                  DEFAULT method, and a STATIC interface method.
 *                                  ABSTRACT methods STILL have _methods entries,
 *                                  so all three MUST be enumerated:
 *                                    int  absOp(int)   (I)I   abstract
 *                                    int  defOp(int)   (I)I   default
 *                                    int  staticOp(int)(I)I   static
 *
 *   MethodsWalk$Abstract        -- an ABSTRACT class with one abstract + one
 *                                  concrete method + a constructor:
 *                                    <init>           ()V
 *                                    int  shape(int)  (I)I   abstract
 *                                    int  concrete(int)(I)I
 *
 *   MethodsWalk$Base / $Mid / $Sub -- a 3-LEVEL hierarchy proving the walk lists
 *                                  ONLY a class's OWN declared methods, never
 *                                  inherited ones:
 *                                    Base:  baseOnly()  ()V ; <init> ()V
 *                                    Mid extends Base:  midOnly() ()V ; <init> ()V
 *                                    Sub extends Mid:   subOnly() ()V ; <init> ()V ;
 *                                                       compareTo(Sub) -- a generic
 *                                                       override that yields a BRIDGE
 *                                                       compareTo(Object) [INFO].
 *                                  Sub additionally declares a STATIC method
 *                                    int subStatic(int) (I)I   so the native module
 *                                  can contrast the bare walk (Sub-only) with the
 *                                  super-walking static_method()->call() resolver,
 *                                  which sees the INHERITED static baseStatic too:
 *                                    Base: static int baseStatic(int) (I)I
 *
 *   MethodsWalk$Vals (enum)     -- an ENUM.  The compiler-synthesised
 *                                    values()         ()[Lvmhook/fixtures/MethodsWalk$Vals;
 *                                    valueOf(String)  (Ljava/lang/String;)Lvmhook/fixtures/MethodsWalk$Vals;
 *                                  are JDK-stable (8..26) and hard-asserted PRESENT;
 *                                  plus a user method  int rank() (I removed) ()I.
 *
 *   MethodsWalk$Anno (@interface) -- an ANNOTATION type (an interface under the
 *                                  hood).  Its annotation ELEMENTS are abstract
 *                                  methods in _methods:
 *                                    String label()  ()Ljava/lang/String;
 *                                    int    weight() ()I
 *
 *   MethodsWalk$Many            -- a class declaring MANY methods (m00..m49, each
 *                                  ()V) so the walk enumerates a large array; the
 *                                  native module asserts all 50 are present and the
 *                                  count is stable across two reads.
 *
 *   MethodsWalk$Inner           -- a non-static INNER class (carries a synthetic
 *                                  this$0 FIELD, not a method) declaring one user
 *                                  method  int innerOp() ()I ; <init> takes the
 *                                  synthetic outer param (its descriptor varies, so
 *                                  the module hard-asserts innerOp only, not <init>).
 *
 * --------------------------------------------------------------------------
 * PROBE: drives a UNIQUE-named method (idLong (J)J) through real bytecode so the
 * native module can install a scoped hook on it and prove the inline hook<T>()
 * walk (vmhook.hpp:8049) resolves the SAME Method* the collector enumerates
 * (flaw #3 -- substrate vs inline-clone agreement).  Also touches a static of
 * MethodsWalk$Many to force its <clinit> AFTER the first enumeration, so the
 * module can prove the declared set does not grow from class init.
 *
 * Java 8 source compatibility only: no var / records / switch-expressions /
 * text-blocks / sealed / lambdas / List.of; only java.* (none past Java 8) +
 * vmhook.Harness.
 */
public class MethodsWalk
{
    // ── go / done / mode handshake (native sets mode + clears done first) ────
    public static volatile boolean go;
    public static volatile boolean done;
    public static volatile int     mode;

    // ── deterministic constants the native side mirrors ──────────────────────
    public static final int    SEED        = 7;
    public static final long   IDLONG_ARG  = 0x0102030405060708L;

    // Witness written by the probe (mode 1) through real bytecode dispatch.
    public static volatile long lastIdLong;
    // Witness that Many.<clinit> ran (mode 2 touches a Many static).
    public static volatile int  manyTouched;

    // A static field + a static block => the class genuinely has a <clinit>.
    private static int staticInitMarker = SEED;
    static
    {
        staticInitMarker += 0; // keep the static block non-trivial
    }

    // Instance field so <init> has a body (does not affect _methods set).
    @SuppressWarnings("unused")
    private int seed = SEED;

    // =======================================================================
    //  STATIC vs INSTANCE vs NATIVE vs modifiers (all share (I)I).
    // =======================================================================
    public static int si(final int x)            { return x + 1; }
    public int ii(final int x)                   { return x + 2; }
    public native int nat(int x);                // declared native; never linked
    public final int fin(final int x)            { return x + 3; }
    public synchronized void syncM()             { /* no-op */ }
    public double strictM(final double x)        { return x; }

    // VARARGS -> ([I)I (the array form is the descriptor).
    public int varargs(final int... xs)          { return (xs == null) ? 0 : xs.length; }

    // OVERLOADS (same name, distinct descriptors).
    public int ov(final int x)                   { return x; }
    public long ov(final long x)                 { return x; }
    public String ov(final String x)             { return (x == null) ? "" : x; }

    // The UNIQUE (J)J method the probe dispatches + the module hooks.
    public long idLong(final long x)             { return x; }

    // GENERIC-ERASED arg/return (erases to Object / the bound in the descriptor).
    public <T> Object gen(final T x)             { return x; }
    public <T extends Comparable<T>> Comparable<T> genB(final T x) { return x; }

    // EVERY PRIMITIVE in one descriptor: (ZBCSIJFD)V.
    public void allPrims(final boolean a, final byte b, final char c, final short d,
                         final int e, final long f, final float g, final double h)
    {
        // no-op
    }

    // DEEPLY-NESTED multi-dim reference array arg: ([[[Ljava/lang/String;)V.
    public void deep(final String[][][] s)       { /* no-op */ }

    // LONG NAME (the identifier itself is 82 chars: 'm' + '_' + 80x 'x').
    public void m_xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx()
    {
        // no-op
    }

    // UNICODE NAMES (legal Java identifiers; bytes are modified UTF-8 in Symbol).
    public void méthodé()                        { /* "méthodé" */ }
    public void 名前()                            { /* "名前" (Japanese) */ }

    // =======================================================================
    //  NESTED TYPES (force-loaded below; resolved by internal `$`-name).
    // =======================================================================

    /** A class with NO user methods -> _methods holds ONLY the synthetic <init>. */
    public static class Empty
    {
    }

    /** A MARKER interface with ZERO methods -> _methods is EMPTY (no <init>). */
    public interface Marker
    {
    }

    /** Interface with abstract + default + static methods (all in _methods). */
    public interface Iface
    {
        int absOp(int x);                       // abstract
        default int defOp(final int x)          // default
        {
            return x + 1;
        }
        static int staticOp(final int x)        // static interface method
        {
            return x + 2;
        }
    }

    /** Abstract class: <init> + abstract + concrete. */
    public abstract static class Abstract
    {
        public abstract int shape(int x);
        public int concrete(final int x)        { return x; }
    }

    // ---- 3-level hierarchy: Base <- Mid <- Sub (walk lists OWN methods only) --

    /** Root: one user method + a STATIC the call-resolver sees inherited. */
    public static class Base
    {
        public void baseOnly()                  { /* no-op */ }
        public static int baseStatic(final int x) { return x + 10; }
    }

    /** Middle: one user method; inherits Base's. */
    public static class Mid extends Base
    {
        public void midOnly()                   { /* no-op */ }
    }

    /**
     * Leaf: one user method, one STATIC, and a GENERIC override (compareTo(Sub))
     * that makes javac emit a synthetic BRIDGE compareTo(Object) [INFO].
     */
    public static class Sub extends Mid implements Comparable<Sub>
    {
        public void subOnly()                   { /* no-op */ }
        public static int subStatic(final int x) { return x + 20; }

        @Override
        public int compareTo(final Sub other)   { return 0; }
    }

    /** An ENUM: synthetic values()/valueOf(String) + a user method. */
    public enum Vals
    {
        ALPHA, BETA, GAMMA;

        public int rank()                       { return ordinal() + 1; }
    }

    /** An ANNOTATION type: its elements are abstract methods in _methods. */
    public @interface Anno
    {
        String label() default "n";
        int    weight() default 0;
    }

    /** A class declaring MANY methods (m00..m49, each ()V). */
    public static class Many
    {
        // An INITIALIZED static field so Many genuinely has a <clinit> the module
        // can force later (an uninitialized static would NOT generate one).
        public static volatile int touched = 0;

        public void m00() { } public void m01() { } public void m02() { }
        public void m03() { } public void m04() { } public void m05() { }
        public void m06() { } public void m07() { } public void m08() { }
        public void m09() { } public void m10() { } public void m11() { }
        public void m12() { } public void m13() { } public void m14() { }
        public void m15() { } public void m16() { } public void m17() { }
        public void m18() { } public void m19() { } public void m20() { }
        public void m21() { } public void m22() { } public void m23() { }
        public void m24() { } public void m25() { } public void m26() { }
        public void m27() { } public void m28() { } public void m29() { }
        public void m30() { } public void m31() { } public void m32() { }
        public void m33() { } public void m34() { } public void m35() { }
        public void m36() { } public void m37() { } public void m38() { }
        public void m39() { } public void m40() { } public void m41() { }
        public void m42() { } public void m43() { } public void m44() { }
        public void m45() { } public void m46() { } public void m47() { }
        public void m48() { } public void m49() { }

        public static void touch()              { touched += 1; }
    }

    /** A non-static INNER class (synthetic this$0 FIELD), one user method. */
    public class Inner
    {
        public int innerOp()                    { return MethodsWalk.this.seed; }
    }

    /** Factory so Inner is force-instantiable (wires this$0). */
    public Inner newInner()                     { return new Inner(); }

    // ── Force-load anchors so find_class resolves every `$`-nested klass.  The
    //    harness loader only Class.forName's the TOP-LEVEL fixture, so each
    //    nested klass must be referenced here to be loaded. ────────────────────
    static final Class<?> ANCHOR_EMPTY    = Empty.class;
    static final Class<?> ANCHOR_MARKER   = Marker.class;
    static final Class<?> ANCHOR_IFACE    = Iface.class;
    static final Class<?> ANCHOR_ABSTRACT = Abstract.class;
    static final Class<?> ANCHOR_BASE     = Base.class;
    static final Class<?> ANCHOR_MID      = Mid.class;
    static final Class<?> ANCHOR_SUB      = Sub.class;
    static final Class<?> ANCHOR_VALS     = Vals.class;
    static final Class<?> ANCHOR_ANNO     = Anno.class;
    static final Class<?> ANCHOR_MANY     = Many.class;
    static final Class<?> ANCHOR_INNER    = Inner.class;

    // Force the top-level fixture itself to be instantiable for the probe.
    public static final MethodsWalk SELF = new MethodsWalk();

    static
    {
        Harness.register(new Harness.Probe()
        {
            @Override
            public boolean pending()
            {
                return MethodsWalk.go && !MethodsWalk.done;
            }

            @Override
            public void run()
            {
                if (MethodsWalk.mode == 1)
                {
                    // Real (J)J bytecode dispatch -> fires any installed hook.
                    MethodsWalk.lastIdLong = MethodsWalk.SELF.idLong(MethodsWalk.IDLONG_ARG);
                }
                else if (MethodsWalk.mode == 2)
                {
                    // Force Many.<clinit> + a method dispatch AFTER first enum.
                    MethodsWalk.Many.touch();
                    MethodsWalk.manyTouched = MethodsWalk.Many.touched;
                }
                MethodsWalk.done = true;
            }
        });
    }
}

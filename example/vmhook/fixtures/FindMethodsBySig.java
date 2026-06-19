package vmhook.fixtures;

import vmhook.Harness;

/**
 * Fixture for the find_methods_by_signature feature (area: methods).
 *
 * The ONE thing under test:
 *
 *     vmhook::find_methods_by_signature<W>(descriptor)
 *         -> std::vector<std::string>   // names of EVERY declared method on W's
 *                                       // klass whose exact JVM descriptor == descriptor
 *
 * find_methods_by_signature is the obfuscated-build selector: the method NAME
 * rotates between builds, the JVM DESCRIPTOR is stable, so callers look a method
 * up by descriptor and get back ALL matching names (so a non-unique descriptor is
 * detectable instead of silently taking the first).  It is a thin filter over
 * get_class_methods<W>(), which walks InstanceKlass::_methods DIRECTLY (no JNI).
 * That array holds every method this class DECLARES -- including the synthetic
 * constructor {@code <init>} and the static initializer {@code <clinit>} -- but
 * NOT methods inherited from java.lang.Object.  The match is EXACT string
 * equality on the descriptor (no normalization, no validation).
 *
 * This fixture is shaped so the resulting (name -> descriptor) map is known
 * EXACTLY and exercises every discrimination axis the matcher must respect:
 *
 *   ARG-TYPE / WIDTH:
 *     f(int)        (I)I        | f(long)      (J)J
 *     sFn(short)    (S)S        | bFn(byte)    (B)B
 *     cFn(char)     (C)C        | zFn(boolean) (Z)Z
 *     ffn(float)    (F)F        | dfn(double)  (D)D
 *   ARITY:
 *     f()           ()V         | g(int,int)   (II)I   | g()          ()J
 *   RETURN-TYPE discrimination (same arg list, different return):
 *     f(int)        (I)I        VS  fL(int)     (I)J
 *     f()           ()V         VS  retI()      ()I     VS  g()          ()J
 *   REFERENCE vs primitive return, and String vs Object:
 *     f(String)     (Ljava/lang/String;)Ljava/lang/String;
 *     makeObj()     ()Ljava/lang/Object;
 *   ARRAYS (1-D primitive, 2-D primitive, 1-D reference):
 *     arr(int[])      ([I)[I
 *     arr2(int[][])   ([[I)[[I
 *     arrStr(String[])([Ljava/lang/String;)[Ljava/lang/String;
 *   MULTI-SLOT (long/double occupy two stack slots):
 *     mix(int,long,double)  (IJD)D
 *     wideVoid(long,double) (JD)V    -> two wide args, VOID return
 *   MANY-ARG (all four slot-width classes + a reference + an array, two ways):
 *     many(int,long,double,String,int[],boolean) (IJDLjava/lang/String;[IZ)V  (void)
 *     manyR(int,int,int,int,int,int)             (IIIIII)I                    (int)
 *   REFERENCE arg -> REFERENCE return of a DIFFERENT type (String in, Object out):
 *     boxStr(String)  (Ljava/lang/String;)Ljava/lang/Object;
 *   NO-ARG REFERENCE return that collides (by descriptor) with an INHERITED
 *   java.lang.Object method, proving find returns the DECLARED one only:
 *     name()          ()Ljava/lang/String;   -> { name }, NOT inherited toString()
 *   THREE-WAY shared descriptor -- proves find returns ALL N matches at N=3, and
 *   that two instance + one static method co-enumerate on one descriptor:
 *     tri1(double) / tri2(double) (instance) + tri3(double) (static)  (D)I
 *   STATIC methods are found exactly like instance methods (the descriptor walk
 *   ignores JVM_ACC_STATIC):
 *     sf(int)       (I)I        -> SHARES (I)I with instance f(int)
 *     sf(String)    (Ljava/lang/String;)Ljava/lang/String; -> SHARES with f(String)
 *     sUnique(long,long)  (JJ)J -> a genuinely-unique static descriptor
 *     tri3(double)  (D)I        -> SHARES (D)I with instance tri1/tri2
 *
 * So, by design and verified against `javap -s`:
 *   (I)I                                       -> { f , sf }    (instance + static)
 *   (J)J                                       -> { f }
 *   (Ljava/lang/String;)Ljava/lang/String;     -> { f , sf }    (instance + static)
 *   ()V                                        -> contains { f , uniqueVoid , <init> }
 *   (II)I                                      -> { g }
 *   ([I)[I                                     -> { arr }
 *   (I)J                                       -> { fL }         (return-type proof)
 *   (D)D / (F)F / (S)S / (B)B / (C)C / (Z)Z    -> each { its one method }
 *   ([[I)[[I / ([L..String;)[L..String;        -> { arr2 } / { arrStr }
 *   (IJD)D                                     -> { mix }
 *   (JD)V                                      -> { wideVoid }
 *   (IJDLjava/lang/String;[IZ)V                -> { many }
 *   (IIIIII)I                                  -> { manyR }
 *   (JJ)J                                      -> { sUnique }
 *   ()I                                        -> { retI }
 *   ()J                                        -> { g }
 *   ()Ljava/lang/Object;                       -> { makeObj }
 *   (Ljava/lang/String;)Ljava/lang/Object;     -> { boxStr }
 *   ()Ljava/lang/String;                       -> { name }   (NOT inherited toString)
 *   (D)I                                       -> { tri1 , tri2 , tri3 }  (3-way set)
 *
 * NOTE on (I)I: the task brief sketched "(I)I -> {f}", but it ALSO asked for a
 * static int sf(int) AND for the String descriptor to be { f , sf }.  A static
 * int sf(int) IS (I)I, so (I)I genuinely contains BOTH f and sf.  Asserting the
 * TRUE set { f , sf } is the stronger test: it simultaneously proves static
 * methods are enumerated, that ALL matches are returned (not just the first),
 * and -- via the (I)J -> {fL} check -- that return type still discriminates.
 *
 * The probe's run() additionally dispatches several of these methods through
 * REAL bytecode (invokevirtual / invokestatic), so the native side can re-run
 * find_methods_by_signature AFTER live dispatch + JIT and prove the _methods
 * enumeration is stable (calling/compiling a method does not perturb it).
 *
 * Java 8 source compatibility only: no var / records / switch-expressions /
 * text-blocks / sealed / List.of; only java.* (none past Java 8) + vmhook.Harness.
 * Every method body returns a constant / trivial value -- behaviour is irrelevant;
 * only the declared (name, descriptor) shape matters.
 */
public final class FindMethodsBySig
{
    /** Native sets this true to request the probe action; cleared after. */
    public static volatile boolean go;

    /** The probe action sets this true when it has run; native polls it. */
    public static volatile boolean done;

    // ---- Witnesses written by the probe through real bytecode -------------
    public static volatile int     wFInt;     // result of f(7)
    public static volatile long    wFLong;    // result of f(11L)
    public static volatile int     wGII;      // result of g(3,4)
    public static volatile int     wArrLen;   // arr(new int[]{...}).length
    public static volatile int     wSfInt;    // sf(9)  (static dispatch)
    public static volatile long    wSUnique;  // sUnique(2L,3L) (static dispatch)

    // =======================================================================
    //  Instance methods.  Declaration order is intentionally scrambled vs
    //  HotSpot's name-symbol sort order; the enumeration must not depend on it.
    //  Bodies return constants -- only the descriptor matters.
    // =======================================================================

    /** (I)I -- SHARES its descriptor with the static sf(int). */
    public int f(final int x)
    {
        return x + 1;
    }

    /** (J)J -- unique. */
    public long f(final long x)
    {
        return x + 1L;
    }

    /** (Ljava/lang/String;)Ljava/lang/String; -- SHARES with static sf(String). */
    public String f(final String x)
    {
        return (x == null) ? "" : x;
    }

    /** ()V -- SHARES ()V with uniqueVoid, <init> and <clinit>. */
    public void f()
    {
        // no-op
    }

    /** (II)I -- the two-int-arg discriminator (arity vs (I)I). */
    public int g(final int a, final int b)
    {
        return a + b;
    }

    /** ()J -- no-arg long return (return-type discrimination vs ()V and ()I). */
    public long g()
    {
        return 42L;
    }

    /** ()V -- a deliberately-unique NAME on the shared ()V descriptor. */
    public void uniqueVoid()
    {
        // no-op
    }

    /** ([I)[I -- 1-D primitive array in and out. */
    public int[] arr(final int[] a)
    {
        return (a == null) ? new int[0] : a;
    }

    /** (I)J -- same arg list as f(int) but a DIFFERENT return type. */
    public long fL(final int x)
    {
        return (long) x;
    }

    /** ()I -- no-arg int return (return-type discrimination vs ()V and ()J). */
    public int retI()
    {
        return 5;
    }

    /** (S)S -- short width. */
    public short sFn(final short x)
    {
        return x;
    }

    /** (B)B -- byte width. */
    public byte bFn(final byte x)
    {
        return x;
    }

    /** (C)C -- char width. */
    public char cFn(final char x)
    {
        return x;
    }

    /** (Z)Z -- boolean. */
    public boolean zFn(final boolean x)
    {
        return x;
    }

    /** (F)F -- float. */
    public float ffn(final float x)
    {
        return x;
    }

    /** (D)D -- double. */
    public double dfn(final double x)
    {
        return x;
    }

    /** (IJD)D -- int + long + double spans the two-slot boundary. */
    public double mix(final int a, final long b, final double c)
    {
        return a + b + c;
    }

    /** ()Ljava/lang/Object; -- reference return distinct from String. */
    public Object makeObj()
    {
        return new Object();
    }

    /** ([[I)[[I -- 2-D primitive array. */
    public int[][] arr2(final int[][] a)
    {
        return (a == null) ? new int[0][] : a;
    }

    /** ([Ljava/lang/String;)[Ljava/lang/String; -- 1-D reference array. */
    public String[] arrStr(final String[] a)
    {
        return (a == null) ? new String[0] : a;
    }

    /** (JD)V -- two WIDE args (long+double, four slots) with a VOID return. */
    public void wideVoid(final long a, final double b)
    {
        // no-op
    }

    /**
     * (IJDLjava/lang/String;[IZ)V -- a MANY-arg, VOID-return method spanning every
     * slot-width class: 1-slot int, 2-slot long, 2-slot double, a reference, a
     * reference array, and a 1-slot boolean.
     */
    public void many(final int a, final long b, final double c,
                     final String d, final int[] e, final boolean f)
    {
        // no-op
    }

    /** (IIIIII)I -- a MANY-arg method, all single-slot ints, with an INT return. */
    public int manyR(final int a, final int b, final int c,
                     final int d, final int e, final int f)
    {
        return a + b + c + d + e + f;
    }

    /**
     * (Ljava/lang/String;)Ljava/lang/Object; -- a REFERENCE arg returning a
     * DIFFERENT reference type (String in, Object out), distinct from the
     * String->String set { f, sf }.
     */
    public Object boxStr(final String s)
    {
        return (s == null) ? new Object() : s;
    }

    /**
     * ()Ljava/lang/String; -- a no-arg String return.  java.lang.Object does NOT
     * declare a no-arg String method, but Object#toString() carries this exact
     * descriptor; because find walks DECLARED methods only, ()Ljava/lang/String;
     * resolves to { name } and never to the inherited toString.
     */
    public String name()
    {
        return "FindMethodsBySig";
    }

    /** (D)I -- member of the THREE-WAY (D)I set { tri1, tri2, tri3 }. */
    public int tri1(final double x)
    {
        return (int) x;
    }

    /** (D)I -- member of the THREE-WAY (D)I set { tri1, tri2, tri3 }. */
    public int tri2(final double x)
    {
        return (int) x + 1;
    }

    // =======================================================================
    //  Array element-type and dimension coverage.  A descriptor's array part is
    //  just a run of '[' followed by the element descriptor, so element TYPE and
    //  dimension COUNT both discriminate.  These give find a wide spread of
    //  array shapes that the (int[])-only fixture above did not exercise.
    // =======================================================================

    /** ([J)[J -- a WIDE (long) element array in and out. */
    public long[] arrJ(final long[] a)
    {
        return (a == null) ? new long[0] : a;
    }

    /** ([D)[D -- a WIDE (double) element array in and out. */
    public double[] arrD(final double[] a)
    {
        return (a == null) ? new double[0] : a;
    }

    /** ([Z)[Z -- a boolean element array (single-slot, distinct tag Z). */
    public boolean[] arrZ(final boolean[] a)
    {
        return (a == null) ? new boolean[0] : a;
    }

    /** ([B)[B -- a byte element array. */
    public byte[] arrB(final byte[] a)
    {
        return (a == null) ? new byte[0] : a;
    }

    /** ([[[I)[[[I -- a THREE-dimensional int array (dimension-count discriminator). */
    public int[][][] arr3(final int[][][] a)
    {
        return (a == null) ? new int[0][][] : a;
    }

    /**
     * ([I)I -- a primitive-array ARG with a SCALAR return.  This is also the
     * descriptor a {@code varargs} int... parameter compiles to: varargs is pure
     * syntactic sugar over an array, so the descriptor carries NO vararg marker.
     * Distinct from ([I)[I {arr} (array return) and from (I)I {f, sf} (scalar arg).
     */
    public int sumArr(final int... xs)
    {
        int s = 0;
        if (xs != null)
        {
            for (int i = 0; i < xs.length; i++)
            {
                s += xs[i];
            }
        }
        return s;
    }

    // =======================================================================
    //  Reference-type coverage beyond String: an Object<->Object identity and a
    //  TWO-reference-arg method.  Proves the reference tag L...; discriminates by
    //  the exact internal class name, and that consecutive reference args are
    //  parsed as separate slots.
    // =======================================================================

    /** (Ljava/lang/Object;)Ljava/lang/Object; -- Object in, Object out (NOT String). */
    public Object idObj(final Object o)
    {
        return o;
    }

    /** (Ljava/lang/String;Ljava/lang/Object;)V -- TWO reference args, VOID return. */
    public void twoRef(final String s, final Object o)
    {
        // no-op
    }

    // =======================================================================
    //  Static methods.  The descriptor walk ignores JVM_ACC_STATIC, so these
    //  appear in the SAME result set as the instance methods of equal descriptor.
    // =======================================================================

    /** (I)I -- shares (I)I with instance f(int). */
    public static int sf(final int x)
    {
        return x * 2;
    }

    /** (Ljava/lang/String;)Ljava/lang/String; -- shares with instance f(String). */
    public static String sf(final String x)
    {
        return (x == null) ? "" : x;
    }

    /** (JJ)J -- a genuinely-unique static descriptor (two longs, four slots). */
    public static long sUnique(final long a, final long b)
    {
        return a + b;
    }

    /**
     * ([J)[J -- a STATIC long-array method that SHARES its descriptor with the
     * instance arrJ(long[]).  Proves a SHARED ARRAY descriptor returns the full
     * set { arrJ, sArrJ } (static + instance) exactly like the scalar (I)I case.
     */
    public static long[] sArrJ(final long[] a)
    {
        return (a == null) ? new long[0] : a;
    }

    /**
     * ([I)I -- a STATIC varargs partner sharing the descriptor of the instance
     * sumArr(int...).  ([I)I therefore returns the full set { sumArr, sVararg }:
     * a second proof that varargs compiles to a plain array descriptor AND that a
     * static varargs co-enumerates with an instance one.
     */
    public static int sVararg(final int... xs)
    {
        int s = 0;
        if (xs != null)
        {
            for (int i = 0; i < xs.length; i++)
            {
                s += xs[i];
            }
        }
        return s;
    }

    /**
     * (D)I -- the STATIC member of the three-way (D)I set { tri1, tri2, tri3 },
     * proving find returns ALL matches at multiplicity 3 and that static and
     * instance methods co-enumerate on one descriptor.
     */
    public static int tri3(final double x)
    {
        return (int) x + 2;
    }

    // ---- Probe dispatch ---------------------------------------------------

    /**
     * Drives REAL bytecode through a representative spread of the methods so the
     * native side can re-enumerate AFTER live dispatch/JIT and prove stability.
     * Uses both invokevirtual (on a fresh instance) and invokestatic.
     */
    private static void driveDispatch()
    {
        final FindMethodsBySig obj = new FindMethodsBySig();
        wFInt   = obj.f(7);                       // invokevirtual (I)I
        wFLong  = obj.f(11L);                     // invokevirtual (J)J
        wGII    = obj.g(3, 4);                    // invokevirtual (II)I
        wArrLen = obj.arr(new int[] { 1, 2, 3 }).length; // invokevirtual ([I)[I
        obj.f();                                  // invokevirtual ()V
        obj.uniqueVoid();                         // invokevirtual ()V
        wSfInt   = FindMethodsBySig.sf(9);        // invokestatic   (I)I
        wSUnique = FindMethodsBySig.sUnique(2L, 3L); // invokestatic (JJ)J
    }

    static
    {
        Harness.register(new Harness.Probe()
        {
            @Override
            public boolean pending()
            {
                return FindMethodsBySig.go && !FindMethodsBySig.done;
            }

            @Override
            public void run()
            {
                driveDispatch();
                FindMethodsBySig.done = true;
            }
        });
    }
}

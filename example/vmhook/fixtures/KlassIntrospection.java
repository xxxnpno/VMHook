package vmhook.fixtures;

import java.lang.annotation.Retention;
import java.lang.annotation.RetentionPolicy;
import java.lang.reflect.Modifier;

import vmhook.Harness;

/**
 * Fixture for the klass_introspection feature (area: classes / klass shape).
 *
 * The native module reads a loaded HotSpot Klass / InstanceKlass directly (no
 * JNI, no JVMTI) and queries EVERY reachable klass-introspection accessor:
 *
 *   - vmhook::find_class("vmhook/fixtures/..")            -> hotspot::klass*
 *   - klass::get_name()       (internal '/'-name symbol)
 *   - klass::get_super()      (super-klass*; null ONLY for java.lang.Object)
 *   - klass::get_instance_size()  (Klass::_layout_helper based; 0 for a
 *                                  non-instantiable klass: interface / abstract*
 *                                  / array — see note below)
 *   - klass::get_methods_count()  (InstanceKlass::_methods length)
 *   - klass::get_interfaces_ptr() (InstanceKlass transitive interfaces)
 *   - klass::find_field("x")      (declared field metadata)
 *   - vmhook::get_class_methods<T>() / (name) and find_methods_by_signature<T>
 *   - the class-file access flags (Klass::_access_flags) read through a guarded,
 *     cached-offset os::safe_read in the module (the SAME pattern the library
 *     uses internally in klass_is_interface_like) — tested bit by bit
 *     (PUBLIC / FINAL / ABSTRACT / INTERFACE / ENUM) and CROSS-CHECKED against
 *     java.lang.reflect.Modifier published here.
 *
 * This fixture deliberately exhibits EVERY class shape the module sweeps, each
 * with a javac-STABLE internal name so a fixed name resolves it through
 * find_class:
 *
 *   - a NORMAL public class          (this top-level KlassIntrospection)
 *   - a generic (erased) class       (Box$<T>  -> KlassIntrospection$Box)
 *   - an INTERFACE                   (KlassIntrospection$Iface)
 *   - an ABSTRACT class              (KlassIntrospection$AbstractBase)
 *   - a FINAL class                  (KlassIntrospection$FinalLeaf)
 *   - an ENUM                        (KlassIntrospection$Suit)
 *   - an ANNOTATION type             (KlassIntrospection$Marker)
 *   - a NESTED (static) class        (KlassIntrospection$Nested)
 *   - an INNER (non-static) class    (KlassIntrospection$Inner)
 *   - a 3-level inheritance chain    (Base -> Mid -> Leaf) for the declared-only
 *                                    + _super-walk semantics
 *   - a class with MANY fields/methods (KlassIntrospection$Wide)
 *   - a generic-bridge class         (KlassIntrospection$Cmp implements
 *                                    Comparable<Cmp>; javac emits a synthetic
 *                                    bridge compareTo(Ljava/lang/Object;)I)
 *   - array klasses are queried by the module via find_class("[I") etc. — no
 *     Java type to declare for those.
 *
 * NOTE on get_instance_size() for an ABSTRACT class: an abstract class still has
 * a positive layout_helper (HotSpot computes a real instance size for it; the
 * abstract-ness only forbids `new`).  So get_instance_size() > 0 for an abstract
 * class and does NOT distinguish abstract from concrete — only the access-flag
 * ABSTRACT bit does.  An INTERFACE and an ARRAY klass have a non-positive
 * layout_helper, so get_instance_size() == 0 there.  These exact relationships
 * are what the module pins.
 *
 * Ground-truth publication: the probe action computes, on the Java thread (real
 * bytecode), each shape's Class.getModifiers() and a couple of derived booleans
 * (isInterface / isArray / isEnum / isAnnotation), its superclass name, declared
 * method count, and declared field count, and stores them in static witnesses
 * the native module cross-checks its direct klass reads against.  Class literals
 * in the probe also FORCE the nested/inner/annotation/array types to load (the
 * harness's loadFixtures only forName's top-level fixtures, never the $-nested
 * ones), so find_class can resolve them.
 *
 * JAVA 8 SYNTAX ONLY: anonymous Harness.Probe (no lambda for the Probe), no var /
 * records / switch-expressions / text-blocks.  java.* + vmhook.Harness only.
 */
public final class KlassIntrospection
{
    /** Native sets this true to request the action; the action clears done. */
    public static volatile boolean go;

    /** The action sets this true when it has run; native polls it. */
    public static volatile boolean done;

    /**
     * Scenario selector.  Only one scenario is needed — every klass read happens
     * from native code; the probe exists to (re)publish the reflection witnesses
     * on the Java thread and to perform a real bytecode dispatch.
     *   0 = (re)publish Modifier / super-name / count witnesses, then tick().
     */
    public static volatile int mode;

    // =======================================================================
    //  CLASS SHAPES (each with a javac-stable internal name)
    // =======================================================================

    /** INTERFACE shape: abstract + default + static methods all live here. */
    public interface Iface
    {
        /** abstract — lives in _methods. */
        int abstractOp(int x);

        /** default — concrete body, still declared by the interface. */
        default int defaultOp(int x)
        {
            return x + 1;
        }

        /** static interface method. */
        static int staticOp(int x)
        {
            return x * 2;
        }
    }

    /** ABSTRACT class shape (positive layout_helper; ABSTRACT access bit set). */
    public abstract static class AbstractBase
    {
        public int baseField;

        public abstract int mustImplement();

        public int concreteHelper()
        {
            return this.baseField + 1;
        }
    }

    /** FINAL class shape (FINAL access bit set; extends the abstract base). */
    public static final class FinalLeaf extends AbstractBase
    {
        public int leafField;

        @Override
        public int mustImplement()
        {
            return this.leafField;
        }
    }

    /** ENUM shape (ENUM + FINAL access bits; super is java.lang.Enum). */
    public enum Suit
    {
        CLUBS, DIAMONDS, HEARTS, SPADES;

        public int rank()
        {
            return this.ordinal() + 1;
        }
    }

    /** ANNOTATION shape (INTERFACE + ANNOTATION access bits). */
    @Retention(RetentionPolicy.RUNTIME)
    public @interface Marker
    {
        String value() default "";
        int count() default 0;
    }

    /** Static NESTED class (ordinary class; no synthetic outer reference). */
    public static class Nested
    {
        public int nestedField = 10;

        public int nestedMethod()
        {
            return this.nestedField;
        }
    }

    /** Non-static INNER class (javac injects a synthetic this$0 -> outer). */
    public class Inner
    {
        public int innerField = 20;

        public int innerMethod()
        {
            return this.innerField;
        }
    }

    /** Generic (erased) class: the type parameter vanishes in the klass. */
    public static class Box<T>
    {
        private T value;

        public T get()
        {
            return this.value;
        }

        public void set(final T v)
        {
            this.value = v;
        }
    }

    /** Generic-bridge class: javac emits a synthetic bridge
     *  compareTo(Ljava/lang/Object;)I beside compareTo(LKlassIntrospection$Cmp;)I. */
    public static final class Cmp implements Comparable<Cmp>
    {
        public int key;

        @Override
        public int compareTo(final Cmp other)
        {
            return Integer.compare(this.key, other.key);
        }
    }

    // ---- 3-level inheritance chain (declared-only + _super-walk proof) ------

    public static class Base
    {
        public int baseOnly;

        public int fromBase()
        {
            return 1;
        }

        public int sharedName()
        {
            return 10;
        }
    }

    public static class Mid extends Base
    {
        public int midOnly;

        public int fromMid()
        {
            return 2;
        }

        @Override
        public int sharedName()
        {
            return 20;
        }
    }

    public static final class Leaf extends Mid
    {
        public int leafOnly;

        public int fromLeaf()
        {
            return 3;
        }

        @Override
        public int sharedName()
        {
            return 30;
        }
    }

    // ---- "many fields / methods" shape -------------------------------------

    /** Eight declared fields + several declared methods + a constructor. */
    public static class Wide
    {
        public int    f0;
        public long   f1;
        public double f2;
        public float  f3;
        public short  f4;
        public byte   f5;
        public char   f6;
        public boolean f7;

        public int    m0() { return this.f0; }
        public long   m1() { return this.f1; }
        public double m2() { return this.f2; }
        public void   m3() { this.f0++; }
        public int    m4(final int a, final int b) { return a + b; }
    }

    // =======================================================================
    //  GROUND-TRUTH WITNESSES (published by the probe via real bytecode)
    // =======================================================================
    //
    //  *_mods  = Class.getModifiers()  (the java.lang.reflect.Modifier bits)
    //  The native side reads Klass::_access_flags directly and the low 16 bits
    //  must agree with these on the shared bits (PUBLIC/FINAL/ABSTRACT/
    //  INTERFACE/ENUM).  Modifier exposes more than _access_flags carries for
    //  some synthetic cases, so the native checks mask to the bits it tests.

    public static volatile int     selfMods;          // KlassIntrospection
    public static volatile int     ifaceMods;         // Iface
    public static volatile int     abstractMods;      // AbstractBase
    public static volatile int     finalMods;         // FinalLeaf
    public static volatile int     enumMods;          // Suit
    public static volatile int     markerMods;        // Marker
    public static volatile int     nestedMods;        // Nested
    public static volatile int     innerMods;         // Inner
    public static volatile int     boxMods;           // Box
    public static volatile int     baseMods;          // Base
    public static volatile int     intArrayMods;      // int[].class
    public static volatile int     strArray2DMods;    // String[][].class
    public static volatile int     objectMods;        // java.lang.Object

    // Derived booleans (so the native side never has to reimplement Modifier).
    public static volatile boolean ifaceIsInterface;     // true
    public static volatile boolean abstractIsAbstract;   // true
    public static volatile boolean finalIsFinal;         // true
    public static volatile boolean enumIsEnum;           // true
    public static volatile boolean markerIsAnnotation;   // true
    public static volatile boolean markerIsInterface;    // true (annotations are interfaces)
    public static volatile boolean intArrayIsArray;      // true
    public static volatile boolean strArray2DIsArray;    // true
    public static volatile boolean nestedIsInterface;    // false
    public static volatile boolean nestedIsAbstract;     // false

    // Extra array-shape witnesses (every primitive array + a 1-D / 3-D reference
    // array): isArray + final + abstract bits (the JLS stamps array classes
    // public(component) final abstract) so the native side can cross-check that
    // its resolved klass really is an array klass of the expected element type.
    public static volatile boolean longArrayIsArray;     // true  ([J)
    public static volatile boolean doubleArrayIsArray;   // true  ([D)
    public static volatile boolean floatArrayIsArray;    // true  ([F)
    public static volatile boolean shortArrayIsArray;    // true  ([S)
    public static volatile boolean byteArrayIsArray;     // true  ([B)
    public static volatile boolean charArrayIsArray;     // true  ([C)
    public static volatile boolean boolArrayIsArray;     // true  ([Z)
    public static volatile boolean strArray1DIsArray;    // true  ([Ljava/lang/String;)
    public static volatile boolean intArray3DIsArray;    // true  ([[[I)
    public static volatile boolean intArrayIsFinal;      // true  (array classes are final)
    public static volatile boolean intArrayIsAbstract;   // true  (array classes are abstract)
    // Array classes implement BOTH Cloneable and Serializable (JLS 4.10.3); the
    // native get_interfaces_ptr walk on an array klass should see exactly these.
    public static volatile int     intArrayInterfaceCount;   // 2 (Cloneable, Serializable)

    // Superclass NAMES (internal '/'-form) for the _super cross-check.
    public static volatile String  finalSuperName;       // .../AbstractBase
    public static volatile String  enumSuperName;        // java/lang/Enum
    public static volatile String  midSuperName;         // .../Base
    public static volatile String  leafSuperName;        // .../Mid
    public static volatile String  objectSuperName;      // null -> "" (Object has no super)
    public static volatile String  intArraySuperName;    // java/lang/Object
    public static volatile boolean objectSuperIsNull;    // true

    // Declared method / field counts (Class.getDeclaredMethods/Fields length).
    // NOTE: these are NOT identical to InstanceKlass::_methods length (which also
    // counts <init>/<clinit> and bridges and excludes nothing), so the native
    // side uses them only as a LOWER-BOUND / membership cross-check, never ==.
    public static volatile int     wideDeclaredFields;   // 8
    public static volatile int     wideDeclaredMethods;  // 5 (m0..m4), excl. <init>
    public static volatile int     leafDeclaredMethods;  // fromLeaf + sharedName
    public static volatile int     midDeclaredMethods;   // fromMid + sharedName
    public static volatile int     baseDeclaredMethods;  // fromBase + sharedName

    // Bridge witness: Cmp has TWO compareTo methods after erasure (the typed one
    // + the synthetic bridge taking Object).  Published so the native count of
    // declared compareTo methods can be cross-checked.
    public static volatile int     cmpCompareToCount;    // 2

    // Interface witnesses.
    public static volatile boolean finalImplementsNothingDirect;  // FinalLeaf has no direct iface
    public static volatile int     cmpInterfaceCount;             // 1 (Comparable)
    public static volatile int     nestedInterfaceCount;          // 0 (Nested implements nothing)
    public static volatile int     baseInterfaceCount;            // 0 (Base implements nothing)

    // Enum-shape witnesses: the constant count (so the native synthetic-field
    // walk for $VALUES + the constants can be sanity-checked) and the declared
    // field count (4 constants + the synthetic $VALUES = 5).
    public static volatile int     suitConstantCount;    // 4 (CLUBS..SPADES)
    public static volatile int     suitDeclaredFields;   // 5 (4 constants + $VALUES)

    // Inner-class witness: a non-static inner class carries a synthetic this$0
    // field referencing the enclosing instance — so it declares MORE fields than
    // its source shows.  Published so the native find_field("this$0") (synthetic)
    // probe can be corroborated without hardcoding javac's synthetic name rules.
    public static volatile int     innerDeclaredFields;  // 2 (innerField + this$0)

    // Box (generic, erased) witness: the declared method count is unaffected by
    // erasure (get + set), and the field is a single erased Object reference.
    public static volatile int     boxDeclaredMethods;   // 2 (get, set)

    // Identity/sanity witness so the probe's dispatch is observable.
    public static volatile int     tickWitness;

    // Force-load anchors: referencing these class literals in <clinit> ensures
    // the nested/inner/annotation types are LOADED (the harness loader skips
    // $-nested classes), so find_class can resolve them from native code.
    static final Class<?> ANCHOR_IFACE    = Iface.class;
    static final Class<?> ANCHOR_ABSTRACT = AbstractBase.class;
    static final Class<?> ANCHOR_FINAL    = FinalLeaf.class;
    static final Class<?> ANCHOR_ENUM     = Suit.class;
    static final Class<?> ANCHOR_MARKER   = Marker.class;
    static final Class<?> ANCHOR_NESTED   = Nested.class;
    static final Class<?> ANCHOR_INNER    = Inner.class;
    static final Class<?> ANCHOR_BOX      = Box.class;
    static final Class<?> ANCHOR_CMP      = Cmp.class;
    static final Class<?> ANCHOR_BASE     = Base.class;
    static final Class<?> ANCHOR_MID      = Mid.class;
    static final Class<?> ANCHOR_LEAF     = Leaf.class;
    static final Class<?> ANCHOR_WIDE     = Wide.class;

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

    /** Count declared methods named `name` on class c (erasure bridge counting). */
    private static int countDeclaredNamed(final Class<?> c, final String name)
    {
        int n = 0;
        final java.lang.reflect.Method[] ms = c.getDeclaredMethods();
        for (int i = 0; i < ms.length; i++)
        {
            if (ms[i].getName().equals(name))
            {
                n++;
            }
        }
        return n;
    }

    static
    {
        Harness.register(new Harness.Probe()
        {
            @Override
            public boolean pending()
            {
                return KlassIntrospection.go && !KlassIntrospection.done;
            }

            @Override
            public void run()
            {
                // ---- access-flag witnesses (Class.getModifiers) -------------
                selfMods       = KlassIntrospection.class.getModifiers();
                ifaceMods      = Iface.class.getModifiers();
                abstractMods   = AbstractBase.class.getModifiers();
                finalMods      = FinalLeaf.class.getModifiers();
                enumMods       = Suit.class.getModifiers();
                markerMods     = Marker.class.getModifiers();
                nestedMods     = Nested.class.getModifiers();
                innerMods      = Inner.class.getModifiers();
                boxMods        = Box.class.getModifiers();
                baseMods       = Base.class.getModifiers();
                intArrayMods   = int[].class.getModifiers();
                strArray2DMods = String[][].class.getModifiers();
                objectMods     = Object.class.getModifiers();

                // ---- derived booleans ---------------------------------------
                ifaceIsInterface   = Iface.class.isInterface();
                abstractIsAbstract = Modifier.isAbstract(AbstractBase.class.getModifiers());
                finalIsFinal       = Modifier.isFinal(FinalLeaf.class.getModifiers());
                enumIsEnum         = Suit.class.isEnum();
                markerIsAnnotation = Marker.class.isAnnotation();
                markerIsInterface  = Marker.class.isInterface();
                intArrayIsArray    = int[].class.isArray();
                strArray2DIsArray  = String[][].class.isArray();
                nestedIsInterface  = Nested.class.isInterface();
                nestedIsAbstract   = Modifier.isAbstract(Nested.class.getModifiers());

                // ---- extra array-shape witnesses ----------------------------
                longArrayIsArray   = long[].class.isArray();
                doubleArrayIsArray = double[].class.isArray();
                floatArrayIsArray  = float[].class.isArray();
                shortArrayIsArray  = short[].class.isArray();
                byteArrayIsArray   = byte[].class.isArray();
                charArrayIsArray   = char[].class.isArray();
                boolArrayIsArray   = boolean[].class.isArray();
                strArray1DIsArray  = String[].class.isArray();
                intArray3DIsArray  = int[][][].class.isArray();
                intArrayIsFinal    = Modifier.isFinal(int[].class.getModifiers());
                intArrayIsAbstract = Modifier.isAbstract(int[].class.getModifiers());
                intArrayInterfaceCount = int[].class.getInterfaces().length;

                // ---- superclass names ---------------------------------------
                finalSuperName    = superInternalName(FinalLeaf.class);
                enumSuperName     = superInternalName(Suit.class);
                midSuperName      = superInternalName(Mid.class);
                leafSuperName     = superInternalName(Leaf.class);
                objectSuperName   = superInternalName(Object.class);   // "" (null super)
                intArraySuperName = superInternalName(int[].class);    // java/lang/Object
                objectSuperIsNull = (Object.class.getSuperclass() == null);

                // ---- declared counts ----------------------------------------
                wideDeclaredFields  = Wide.class.getDeclaredFields().length;
                wideDeclaredMethods = Wide.class.getDeclaredMethods().length;
                leafDeclaredMethods = Leaf.class.getDeclaredMethods().length;
                midDeclaredMethods  = Mid.class.getDeclaredMethods().length;
                baseDeclaredMethods = Base.class.getDeclaredMethods().length;

                // ---- erasure / interface witnesses --------------------------
                cmpCompareToCount = countDeclaredNamed(Cmp.class, "compareTo");
                cmpInterfaceCount = Cmp.class.getInterfaces().length;
                finalImplementsNothingDirect = (FinalLeaf.class.getInterfaces().length == 0);
                nestedInterfaceCount = Nested.class.getInterfaces().length;
                baseInterfaceCount   = Base.class.getInterfaces().length;

                // ---- enum / inner / box shape witnesses ---------------------
                suitConstantCount   = Suit.class.getEnumConstants().length;
                suitDeclaredFields  = Suit.class.getDeclaredFields().length;
                innerDeclaredFields = Inner.class.getDeclaredFields().length;
                boxDeclaredMethods  = Box.class.getDeclaredMethods().length;

                // ---- real bytecode dispatch (parity) ------------------------
                tickWitness = new KlassIntrospection().tick(41);

                KlassIntrospection.done = true;
            }
        });
    }
}

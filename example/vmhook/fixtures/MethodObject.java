package vmhook.fixtures;

import vmhook.Harness;

/**
 * Fixture for the method_call_object feature (area: methods).
 *
 * Exercises the ONE thing the method-vs-field parity path promises:
 *
 *     std::unique_ptr&lt;wrapper&gt; obj = proxy-&gt;get_method("foo")-&gt;call(args...);
 *
 * i.e. method_proxy::call() that returns a Java reference type ('L' / '[') and
 * whose value_t implicitly converts to a std::unique_ptr&lt;wrapper&gt;.  The native
 * module asserts the full contract on EVERY object-return SHAPE:
 *
 *   - a NON-NULL object return yields a USABLE wrapper: the native side reads a
 *     field through it (Child.tag / Child.label) AND calls a method through it
 *     (Child.getTag()), so it proves the decoded OOP is a real, walkable heap
 *     object — not a truncated handle or a garbage pointer,
 *   - a NULL object return yields a NULL unique_ptr (monostate -&gt; nullptr), on
 *     the SAME method that can also return non-null (maybeChild(false)) and on a
 *     method that is unconditionally null (nullChild()),
 *   - a FRESHLY-ALLOCATED object each call yields DISTINCT identities across
 *     calls (makeChild() new's a Child every time -&gt; two calls, two OOPs),
 *   - method-vs-field PARITY: the SAME Child reachable via the `child` field
 *     (field_proxy -&gt; unique_ptr) and via getChild() (method_proxy -&gt; unique_ptr)
 *     decode to the SAME heap object — identityHashCode is published so the
 *     native side can cross-check the field read against the method return,
 *   - SELF identity: self() returns `this`, so the returned wrapper's instance
 *     must equal the receiver's instance,
 *   - POLYMORPHIC return: makeAnimal() is declared to return the base type
 *     Animal but actually returns a Dog subclass — the decoded wrapper must see
 *     the RUNTIME klass (Dog), and a virtual method (speak()) dispatches to the
 *     override; getAnimalSound() returns the Dog's String so the native side can
 *     confirm the override fired,
 *   - BOXED return: boxedInt() returns Integer.valueOf(N) typed as Object — a
 *     bootstrap-class reference; the wrapper is usable and intValue() dispatches
 *     through it,
 *   - STATIC object returns: staticMakeChild() / staticNullChild() drive the
 *     static-call path of method_proxy::call() returning an object,
 *   - ARRAY reference returns: childArray() returns Child[] ('[L' descriptor),
 *     intArray() returns int[] ('[I' descriptor), objectArray() returns Object[]
 *     ('[Ljava/lang/Object;') — the array-reference branches of the value_t; the
 *     native side decodes the array oop and walks its length + elements,
 *   - CHAINED call: getChild() returns a Child, and Child.makeSibling() returns
 *     ANOTHER Child — so the unique_ptr&lt;wrapper&gt; from the first call is itself
 *     used as the receiver of a second object-returning call,
 *   - a String-returning method (childLabel()) — the eager-decode reference
 *     return that lands in the std::string variant alternative, NOT the uint32
 *     OOP alternative; included so the module proves the value_t routes String
 *     vs Object to different alternatives.
 *
 * Every object the native side inspects is published with a deterministic field
 * value AND its System.identityHashCode so the C++ checks are exact, never
 * "non-null and hope".  The fixture follows the canonical go/done handshake; a
 * `mode` selector lets one probe cycle drive exactly the call the module is
 * about to assert on (because `done` latches, every observed call must happen
 * inside a single run()).
 */
public final class MethodObject
{
    /** Native sets this true to request the action; clears it after. */
    public static volatile boolean go;

    /** The action sets this true when it has run; native polls it. */
    public static volatile boolean done;

    /**
     * Selects which scenario run() executes.  The native module sets this
     * BEFORE raising `go` so a single probe cycle drives exactly the calls the
     * module is about to assert on.
     *   0  = trigger only: call tick() once (the native hook lives on tick();
     *        the detour does all the object-returning calls on `self`).
     */
    public static volatile int mode;

    // ── The child object the wrappers walk ─────────────────────────────────
    /**
     * Small standalone reference type the native side wraps.  Has a primitive
     * field (tag), a String field (label), and methods (getTag, makeSibling) so
     * the native side can prove a method-returned wrapper is fully usable (field
     * read + method call through it) AND chain a second object-returning call
     * off the returned wrapper.
     */
    public static final class Child
    {
        public int tag;
        public String label;

        public Child(final int tag, final String label)
        {
            this.tag = tag;
            this.label = label;
        }

        public int getTag()
        {
            return this.tag;
        }

        public String getLabel()
        {
            return this.label;
        }

        /**
         * Returns ANOTHER freshly-allocated Child — the chained-call target.
         * The native side calls this THROUGH a method-returned Child wrapper,
         * so a unique_ptr&lt;Child&gt; from one call() is the receiver of the next.
         */
        public Child makeSibling()
        {
            return new Child(SIBLING_TAG, SIBLING_LABEL);
        }

        /**
         * Object-returning method THROUGH a method-returned wrapper that returns
         * null — the chained-call whose SECOND link is a null reference return.
         * Proves a null reference return through a method-decoded receiver still
         * yields a null unique_ptr (the chained null path).
         */
        public Child makeNullSibling()
        {
            return null;
        }

        /**
         * Identity round-trip: returns `this`.  The native side wraps a Child,
         * calls self() through the wrapper, and the returned wrapper must decode
         * to the SAME Child OOP (self-identity through a method-returned wrapper).
         */
        public Child self()
        {
            return this;
        }
    }

    // ── Polymorphic return hierarchy ────────────────────────────────────────
    /** Base type a method is DECLARED to return. */
    public static class Animal
    {
        public String speak()
        {
            return "generic";
        }
    }

    /** First-level subclass.  NOT final any more — Puppy extends it, so the
     *  native side can probe a TWO-level-deep polymorphic return as well. */
    public static class Dog extends Animal
    {
        public int breedId = DOG_BREED_ID;

        @Override
        public String speak()
        {
            return DOG_SOUND;
        }
    }

    /**
     * Two-level-deep subclass (Animal -&gt; Dog -&gt; Puppy).  makePuppy() is
     * DECLARED to return the base Animal but returns a Puppy; the decoded
     * wrapper must see the concrete runtime klass (Puppy), inherit Dog's
     * breedId field, AND dispatch speak() to the Puppy override — proving the
     * runtime-type decode is depth-independent.
     */
    public static final class Puppy extends Dog
    {
        @Override
        public String speak()
        {
            return PUPPY_SOUND;
        }
    }

    // ── Interface-typed return ──────────────────────────────────────────────
    /** Interface a method is DECLARED to return. */
    public interface Named
    {
        String name();
    }

    /** Concrete implementor actually returned — proves the wrapper decodes the
     *  runtime IMPL class behind an interface-typed return. */
    public static final class NamedThing implements Named
    {
        public int code = NAMED_CODE;

        @Override
        public String name()
        {
            return NAMED_NAME;
        }
    }

    // ── Deterministic constants the native side mirrors ────────────────────
    /** tag of the Child returned by makeChild() / held by getChild(). */
    public static final int CHILD_TAG = 0x5EED;          // 24301

    /** label of that Child. */
    public static final String CHILD_LABEL = "child-of-method";

    /** tag of the Child returned by maybeChild(true). */
    public static final int MAYBE_TAG = 0x1234;

    /** tag of the Child returned by staticMakeChild(). */
    public static final int STATIC_TAG = 0x7AC0;          // 31424

    /** label of the static Child. */
    public static final String STATIC_LABEL = "static-child";

    /** tag of each FRESH Child new'd by staticFreshChild() (distinct-each-call
     *  on the static dispatch path). */
    public static final int STATIC_FRESH_TAG = 0x7AC1;    // 31425

    /** tag/label of the chained Child returned by Child.makeSibling(). */
    public static final int SIBLING_TAG = 0x51B;          // 1307
    public static final String SIBLING_LABEL = "sibling-of-child";

    /** breedId of the polymorphic Dog, and the sound its speak() override returns. */
    public static final int DOG_BREED_ID = 0x0D06;        // 3334
    public static final String DOG_SOUND = "woof";

    /** Sound the two-level-deep Puppy override returns (proves depth-independent
     *  runtime dispatch through a method-decoded wrapper). */
    public static final String PUPPY_SOUND = "yip";

    /** Published field + name() result of the interface-typed NamedThing return. */
    public static final int NAMED_CODE = 0x4A3D;          // 18989
    public static final String NAMED_NAME = "named-impl";

    /** Elements of stringArray(), in order. */
    public static final String STR_ARRAY_0 = "s0";
    public static final String STR_ARRAY_1 = "s1";
    public static final String STR_ARRAY_2 = "s2";

    /** value boxed by boxedInt(). */
    public static final int BOXED_INT_VALUE = 0x07E5;     // 2021

    /** tags of the three Child elements in childArray(), in order. */
    public static final int ARRAY_TAG_0 = 100;
    public static final int ARRAY_TAG_1 = 200;
    public static final int ARRAY_TAG_2 = 300;

    /** Length of childArray() / objectArray(). */
    public static final int ARRAY_LEN = 3;

    /** Elements of intArray(), in order, and its length. */
    public static final int INT_ARRAY_0 = 11;
    public static final int INT_ARRAY_1 = 22;
    public static final int INT_ARRAY_2 = 33;
    public static final int INT_ARRAY_3 = 44;
    public static final int INT_ARRAY_LEN = 4;

    /** Return value of childLabel() — a String, the eager-decode reference. */
    public static final String LABEL_STRING = "label-via-method";

    // ── VARIED-ARG-SIGNATURE object-return constants (batch-14 deepening) ───
    // Each arg-driven object-returning method below folds its argument(s) into
    // the returned Child's tag so the native decode is non-vacuous (the wrong
    // arg width / lost arg would yield the wrong tag, not merely "non-null").

    /** Base tag the wide-long arg method adds its (int)arg to. */
    public static final int LONG_ARG_BASE = 0x6000;
    /** The wide long passed by the native side to childForLong(long). */
    public static final long LONG_ARG_VALUE = 0x0000_0001_0000_002AL; // hi+lo halves both set
    /** Expected tag of childForLong(LONG_ARG_VALUE): BASE + (int)(v + (v>>>32)). */
    public static final int LONG_ARG_TAG = LONG_ARG_BASE + (int) (LONG_ARG_VALUE + (LONG_ARG_VALUE >>> 32));

    /** Base tag the wide-double arg method adds its rounded arg to. */
    public static final int DOUBLE_ARG_BASE = 0x6100;
    /** The wide double passed to childForDouble(double). */
    public static final double DOUBLE_ARG_VALUE = 1234.5;
    /** Expected tag of childForDouble(DOUBLE_ARG_VALUE): BASE + (int)arg. */
    public static final int DOUBLE_ARG_TAG = DOUBLE_ARG_BASE + (int) DOUBLE_ARG_VALUE;

    /** The String arg passed to childForString(String) and echoed in the label. */
    public static final String STRING_ARG_VALUE = "arg-string";
    /** Tag of childForString(STRING_ARG_VALUE) — encodes the arg length. */
    public static final int STRING_ARG_TAG = 0x6200 + STRING_ARG_VALUE.length();

    /** The float arg passed to childForFloat(float). */
    public static final float FLOAT_ARG_VALUE = 2.5f;
    /** Tag of childForFloat(FLOAT_ARG_VALUE): BASE + (int)arg. */
    public static final int FLOAT_ARG_BASE = 0x6300;
    public static final int FLOAT_ARG_TAG = FLOAT_ARG_BASE + (int) FLOAT_ARG_VALUE;

    /** The boolean arg passed to childForBool(boolean) (true branch). */
    public static final int BOOL_ARG_TRUE_TAG  = 0x6401;
    public static final int BOOL_ARG_FALSE_TAG = 0x6400;

    /** Args + expected tag of the MULTI-arg method childForMany(int,long,String):
     *  tag = i + (int)l + s.length(). */
    public static final int    MANY_ARG_INT    = 7;
    public static final long   MANY_ARG_LONG   = 0x0000_0000_0000_1000L;
    public static final String MANY_ARG_STRING = "many";
    public static final int    MANY_ARG_TAG    =
        MANY_ARG_INT + (int) MANY_ARG_LONG + MANY_ARG_STRING.length();

    /** value boxed by boxedLong() (a wide boxed primitive return). */
    public static final long BOXED_LONG_VALUE = 0x0000_0007_0000_0003L;
    /** value boxed by boxedBool() — Boolean.TRUE. */
    public static final boolean BOXED_BOOL_VALUE = true;
    /** value boxed by boxedDouble(). */
    public static final double BOXED_DOUBLE_VALUE = 6.25;

    // ── Stored instance state ──────────────────────────────────────────────
    /**
     * The Child instance getChild() returns and the `child` field exposes.
     * Published identity (below) lets the native side prove the method return
     * and the field read decode to the SAME heap object.
     */
    public Child child = new Child(CHILD_TAG, CHILD_LABEL);

    // ── Identity publication (so native checks are exact) ──────────────────
    /** identityHashCode of `this` (the receiver), for the self() parity check. */
    public static volatile int selfIdentity;

    /** identityHashCode of `child`, for the method-vs-field parity check. */
    public static volatile int childIdentity;

    /** identityHashCode of the Child staticMakeChild() returns. */
    public static volatile int staticChildIdentity;

    /** identityHashCode of the Animal (a Dog) makeAnimal() returns. */
    public static volatile int animalIdentity;

    /** identityHashCode of the two-level-deep Puppy makePuppy() returns. */
    public static volatile int puppyIdentity;

    /** identityHashCode of the interface-typed NamedThing makeNamed() returns. */
    public static volatile int namedIdentity;

    /** The singleton static Child, so its identity is stable across calls. */
    private static final Child STATIC_CHILD = new Child(STATIC_TAG, STATIC_LABEL);

    /** The singleton Dog makeAnimal() returns, so its identity is stable. */
    private static final Dog ANIMAL = new Dog();

    /** The singleton two-level-deep Puppy makePuppy() returns. */
    private static final Puppy PUPPY = new Puppy();

    /** The singleton interface-typed implementor makeNamed() returns. */
    private static final NamedThing NAMED = new NamedThing();

    /** The fixed Child[] childArray() returns, so element identities are stable. */
    private final Child[] childArray =
    {
        new Child(ARRAY_TAG_0, "a0"),
        new Child(ARRAY_TAG_1, "a1"),
        new Child(ARRAY_TAG_2, "a2"),
    };

    /** The fixed int[] intArray() returns. */
    private final int[] intArray = { INT_ARRAY_0, INT_ARRAY_1, INT_ARRAY_2, INT_ARRAY_3 };

    /** The fixed Object[] objectArray() returns (holds the same kind of Children). */
    private final Object[] objectArray = new Object[]
    {
        new Child(ARRAY_TAG_0, "o0"),
        new Child(ARRAY_TAG_1, "o1"),
        new Child(ARRAY_TAG_2, "o2"),
    };

    /** The fixed String[] stringArray() returns ('[Ljava/lang/String;'). */
    private final String[] stringArray = { STR_ARRAY_0, STR_ARRAY_1, STR_ARRAY_2 };

    // ── Object-returning probe targets ─────────────────────────────────────

    /** Hook site.  The native module hooks this; the detour drives every call
     *  below on `self` so they run through method_proxy::call() on a live OOP. */
    public int tick(final int nonce)
    {
        return nonce + 1;
    }

    /** Returns a freshly-allocated non-null Child with a known tag/label.
     *  Each call new's a DISTINCT object so the native side can assert two
     *  calls yield two different identities. */
    public Child makeChild()
    {
        return new Child(CHILD_TAG, CHILD_LABEL);
    }

    /** Returns the stored `child` field (same object the field read sees). */
    public Child getChild()
    {
        return this.child;
    }

    /** Returns `this` — identity / self parity probe. */
    public MethodObject self()
    {
        return this;
    }

    /** Returns a non-null Child when present==true, else null (same method, both paths). */
    public Child maybeChild(final boolean present)
    {
        return present ? new Child(MAYBE_TAG, "maybe") : null;
    }

    /** Always returns null (the unconditional null-reference return). */
    public Child nullChild()
    {
        return null;
    }

    /** Static object-returning call: a stable non-null Child. */
    public static Child staticMakeChild()
    {
        return STATIC_CHILD;
    }

    /** Static null-returning object call. */
    public static Child staticNullChild()
    {
        return null;
    }

    /**
     * Polymorphic return: declared Animal, actually a Dog.  The native wrapper
     * registered for Dog must decode the RUNTIME type and dispatch speak() to
     * the Dog override.  Returns the stable ANIMAL singleton (identity published).
     */
    public Animal makeAnimal()
    {
        return ANIMAL;
    }

    /** The polymorphic Dog's overridden speak() result, for cross-checking the
     *  virtual dispatch the native side performs through its decoded wrapper. */
    public String getAnimalSound()
    {
        return ANIMAL.speak();
    }

    /** Boxed-type Object return: Integer.valueOf(N) typed as Object. */
    public Object boxedInt()
    {
        return Integer.valueOf(BOXED_INT_VALUE);
    }

    /** Array reference return ('[L' descriptor) — Child[]. */
    public Child[] childArray()
    {
        return this.childArray;
    }

    /** Primitive array reference return ('[I' descriptor) — int[]. */
    public int[] intArray()
    {
        return this.intArray;
    }

    /** Object array reference return ('[Ljava/lang/Object;' descriptor). */
    public Object[] objectArray()
    {
        return this.objectArray;
    }

    /** String reference return — lands in the std::string variant alternative. */
    public String childLabel()
    {
        return LABEL_STRING;
    }

    /** String array reference return ('[Ljava/lang/String;') — each element is a
     *  java.lang.String the native side decodes via read_java_string. */
    public String[] stringArray()
    {
        return this.stringArray;
    }

    /**
     * Object ARGUMENT + object return: identity echo.  The native side passes a
     * method-returned Child wrapper back IN as the argument; this returns the
     * very same object, so the decoded return OOP must equal the argument OOP
     * (round-trips a unique_ptr&lt;wrapper&gt; arg through an object-returning call).
     */
    public Child echoChild(final Child c)
    {
        return c;
    }

    /**
     * Arg-SELECTED object return: returns the childArray element at `idx`.  The
     * native side drives idx 0/1/2 and asserts each decodes to a DISTINCT object
     * with the matching published tag — an int arg selecting which object the
     * call() returns.  Out-of-range idx returns null (the arg-driven null path).
     */
    public Child pickChild(final int idx)
    {
        return (idx >= 0 && idx < this.childArray.length) ? this.childArray[idx] : null;
    }

    /** Two-level-deep polymorphic return: declared Animal, runtime Puppy. */
    public Animal makePuppy()
    {
        return PUPPY;
    }

    /** The Puppy override's speak() result — ground truth for the depth-2 dispatch. */
    public String getPuppySound()
    {
        return PUPPY.speak();
    }

    /** Interface-typed return: declared Named, runtime NamedThing. */
    public Named makeNamed()
    {
        return NAMED;
    }

    /** The interface impl's name() result — ground truth for the interface dispatch. */
    public String getNamedName()
    {
        return NAMED.name();
    }

    /** A SECOND method that returns the SAME singleton Child as staticMakeChild()
     *  — lets the native side prove two DIFFERENT methods returning one object
     *  decode to the SAME OOP (cross-method identity). */
    public Child sameStaticChild()
    {
        return STATIC_CHILD;
    }

    /**
     * STATIC method that new's a FRESH Child every call — the static-call
     * companion to makeChild()'s distinct-each-call probe.  Two static calls
     * must decode to two DISTINCT non-null instances, proving fresh-allocation
     * identity holds on the STATIC dispatch path, not only the instance one.
     */
    public static Child staticFreshChild()
    {
        return new Child(STATIC_FRESH_TAG, "static-fresh");
    }

    /**
     * Subtype-as-arg identity echo: declared to take AND return the base Animal,
     * but the native side passes a Dog wrapper IN.  The returned OOP must equal
     * the argument OOP, and its runtime klass is the concrete Dog — a widening
     * reference arg (Dog -&gt; Animal) round-tripped through an object-returning
     * call() whose decode is still driven by the runtime oop.
     */
    public Animal echoAnimal(final Animal a)
    {
        return a;
    }

    // ── VARIED-ARG-SIGNATURE object-returning methods (batch-14 deepening) ──
    // Each folds its argument(s) into the returned Child's tag so a mis-packed,
    // truncated, or dropped argument produces a WRONG tag the native side
    // catches — never a vacuous "non-null and hope".

    /**
     * WIDE long arg -&gt; object return.  A 64-bit argument occupies TWO
     * interpreter slots; the tag folds in both the low and high halves so a
     * narrowed (32-bit) pack would yield the wrong tag.
     */
    public Child childForLong(final long v)
    {
        return new Child(LONG_ARG_BASE + (int) (v + (v >>> 32)), "long-arg");
    }

    /** WIDE double arg -&gt; object return (two slots; tag folds the rounded arg). */
    public Child childForDouble(final double v)
    {
        return new Child(DOUBLE_ARG_BASE + (int) v, "double-arg");
    }

    /**
     * String (reference) arg -&gt; object return.  The arg is echoed into the
     * returned Child's label and its length into the tag, so a lost or wrong
     * String arg is detectable on BOTH the tag and the label.
     */
    public Child childForString(final String s)
    {
        return new Child(0x6200 + (s == null ? -1 : s.length()), s);
    }

    /** float arg -&gt; object return (single slot; tag folds the rounded arg). */
    public Child childForFloat(final float v)
    {
        return new Child(FLOAT_ARG_BASE + (int) v, "float-arg");
    }

    /** boolean arg -&gt; object return (the arg selects the tag). */
    public Child childForBool(final boolean b)
    {
        return new Child(b ? BOOL_ARG_TRUE_TAG : BOOL_ARG_FALSE_TAG, "bool-arg");
    }

    /**
     * MULTI-arg mixed-width signature -&gt; object return: int + wide long +
     * String.  The tag folds all three, so any single mis-packed argument
     * (wrong slot count for the long, dropped String) yields the wrong tag.
     */
    public Child childForMany(final int i, final long l, final String s)
    {
        return new Child(i + (int) l + (s == null ? 0 : s.length()), "many-arg");
    }

    /**
     * java.lang.Object base return: declared Object, runtime is the stored
     * `child`.  The native side decodes it through a GENERIC base wrapper and
     * proves identity against the field child — the return type is the root
     * reference type, not a Child, yet the OOP is the same heap object.
     */
    public Object childAsObject()
    {
        return this.child;
    }

    /**
     * Object ARG (declared Object) returned unchanged — identity echo on the
     * ROOT reference type.  The native side passes the stored child back in as
     * an Object and the returned OOP must equal it (arg-unchanged identity for
     * a non-Child declared type).
     */
    public Object echoObject(final Object o)
    {
        return o;
    }

    /**
     * String ARG returned unchanged — identity echo whose return lands in the
     * std::string alternative, NOT the OOP alternative.  Proves an argument can
     * be returned unchanged AND routed to the eager-String path.
     */
    public String echoString(final String s)
    {
        return s;
    }

    /** Wide boxed-primitive return: Long.valueOf(N) typed as Object. */
    public Object boxedLong()
    {
        return Long.valueOf(BOXED_LONG_VALUE);
    }

    /** Boxed boolean return: Boolean.valueOf(true) typed as Object. */
    public Object boxedBool()
    {
        return Boolean.valueOf(BOXED_BOOL_VALUE);
    }

    /** Boxed double return: Double.valueOf(N) typed as Object. */
    public Object boxedDouble()
    {
        return Double.valueOf(BOXED_DOUBLE_VALUE);
    }

    /**
     * Arg-driven null on a TYPED (String) arg: returns the stored child when
     * the arg is non-null, else null.  Drives the null branch from a reference
     * argument (distinct from pickChild's int-out-of-range null).
     */
    public Child childWhenPresent(final String key)
    {
        return (key != null) ? this.child : null;
    }

    static
    {
        Harness.register(new Harness.Probe()
        {
            @Override
            public boolean pending()
            {
                return MethodObject.go && !MethodObject.done;
            }

            @Override
            public void run()
            {
                // Publish identities the native side cross-checks.  Use the
                // SAME instance the native module wrapped: the module's hook
                // fires on tick() with `self` == this instance, so identity of
                // `this` and `this.child` here are exactly what native decodes.
                final MethodObject self = SINGLETON;
                MethodObject.selfIdentity = System.identityHashCode(self);
                MethodObject.childIdentity = System.identityHashCode(self.child);
                MethodObject.staticChildIdentity = System.identityHashCode(STATIC_CHILD);
                MethodObject.animalIdentity = System.identityHashCode(ANIMAL);
                MethodObject.puppyIdentity = System.identityHashCode(PUPPY);
                MethodObject.namedIdentity = System.identityHashCode(NAMED);

                // Calling tick() through normal bytecode dispatch is what makes
                // the native interpreter hook fire; the detour then performs the
                // object-returning calls on `self` via method_proxy::call().
                self.tick(7);

                MethodObject.done = true;
            }
        });
    }

    /**
     * The single instance the native module wraps and drives.  Created eagerly
     * so the native side can fetch it via a static field (the standard "get an
     * instance to reach non-static methods" pattern) and so the identities
     * published above match the OOP the detour sees as `self`.
     */
    public static final MethodObject SINGLETON = new MethodObject();
}

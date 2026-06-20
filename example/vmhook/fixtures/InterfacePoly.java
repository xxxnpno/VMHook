package vmhook.fixtures;

import vmhook.Harness;

/**
 * Fixture for the {@code interface_polymorphism} feature (area: fields + methods).
 *
 * Mirrors the legacy {@code test_interface_and_polymorphism} from
 * {@code vmhook/src/example.cpp} (the Animal / Dog / Pet shapes) as a
 * self-contained modular fixture, but folds the interfaces, the concrete
 * implementations, the abstract base, and the holder all INTO this one top-level
 * fixture class so a single {@code Class.forName("vmhook.fixtures.InterfacePoly")}
 * (the only thing {@code Main.loadFixtures()} does for a fixture) transitively
 * loads everything the native module touches.
 *
 * <p>The shape under test is the canonical interface-polymorphism case: a field
 * whose STATIC (declared) type is an interface (or an abstract base) but whose
 * RUNTIME type is a concrete subclass.  This fixture covers the FULL input
 * space:</p>
 *
 * <pre>
 *     interface Animal  { String speak(); default String defaultGreet(); }
 *     interface Named   { String who(); }                       // 2nd interface
 *     class Dog    implements Animal        { speak() -> "...woof";  inherits defaultGreet() }
 *     class Cat    implements Animal        { speak() -> "...meow";  inherits defaultGreet() }  // 2nd impl
 *     class Snake  implements Animal        { speak() -> "...hiss";  OVERRIDES defaultGreet() } // override
 *     class Robot  implements Animal, Named { speak() -> "...beep";  who() -> id }              // 2 ifaces
 *     abstract class AbstractPet { abstract sound(); String describe() {...} }                  // abstract base
 *     class Hamster extends AbstractPet     { sound() -> "...squeak" }                          // concrete sub
 *
 *     // EXHAUSTIVE itable / interface-dispatch shapes:
 *     interface LoudAnimal extends Animal   { default shout() }                                 // iface EXTENDS iface
 *     class Wolf   implements LoudAnimal    { speak() -> "...howl"; inherits shout()+greet() }  // super-iface impl
 *     interface DiamondTop { default tag() }                                                    // diamond apex
 *     interface DiamondLeft extends DiamondTop {}  interface DiamondRight extends DiamondTop {} // diamond arms
 *     class Diamond implements DiamondLeft, DiamondRight { inherits ONE tag() }                 // diamond impl
 *     interface Greeter { default hello() }  interface Welcomer { default hello() }             // same-sig defaults
 *     class Concierge implements Greeter, Welcomer { OVERRIDES hello() }                        // required override
 *     class Watcher implements Animal       { speak() -> "...woof" }                            // non-final base
 *     class GuardDog extends Watcher        { (Animal only via superclass) }                    // iface via super
 *     interface Toolish { use(); static brand() }                                               // STATIC iface method
 *     class Gadget implements Toolish       { use() -> "...in use" }                            // static NOT inherited
 *     interface Box<T> { T get() }                                                              // generic iface
 *     class StringBox implements Box<String>{ String get() -> "boxed:..."; + Object get BRIDGE }// erasure/bridge
 *
 *     holder InterfacePoly {
 *         Animal      pet       = new Dog(...);   // declared Animal, IS a Dog
 *         Animal      pet2      = new Cat(...);   //                  IS a Cat
 *         Animal      pet3      = new Snake(...); //                  IS a Snake (default override)
 *         Animal      robotPet  = new Robot(...); //                  IS a Robot (multi-iface)
 *         AbstractPet absPet    = new Hamster();  // declared abstract, IS a Hamster
 *         Dog         petAsDog  = (Dog) pet;      // concrete-typed alias to the SAME Dog object
 *         LoudAnimal  loud      = new Wolf(...);  // declared super-iface, IS a Wolf
 *         DiamondLeft diamond   = new Diamond(...);// declared diamond arm, IS a Diamond
 *         Greeter     concierge = new Concierge();// declared Greeter, IS a Concierge (same-sig)
 *         Animal      guard     = new GuardDog(); // declared Animal (via superclass), IS a GuardDog
 *         Watcher     guardAsWatcher = (Watcher) guard; // base-CLASS alias to the SAME object
 *         Toolish     gadget    = new Gadget(...);// declared Toolish, IS a Gadget
 *         Box<String> box       = new StringBox();// declared generic iface, IS a StringBox (bridge)
 *     }
 * </pre>
 *
 * Because the nested types are declared inside this class, javac emits them as
 * {@code vmhook/fixtures/InterfacePoly$Animal}, {@code ...$Dog}, {@code ...$Cat},
 * {@code ...$Snake}, {@code ...$Robot}, {@code ...$Named}, {@code ...$AbstractPet}
 * and {@code ...$Hamster} -- the exact internal names the C++ module asserts the
 * runtime klasses resolve to.
 *
 * <p>What the native module proves on a live JVM (Java 8..25 x MSVC/Clang/GCC),
 * all from native code against the published {@link #SINGLETON}:</p>
 * <ul>
 *   <li>reading any interface-typed field yields a wrapper whose decoded OOP's
 *       RUNTIME klass is the CONCRETE implementor (internal name ends with the
 *       impl's leaf), i.e. vmhook sees the concrete type, not the declared
 *       interface;</li>
 *   <li>the overridden {@code speak()} dispatched through each concrete-type
 *       wrapper reaches THAT impl's override and returns its impl-specific String
 *       ("woof" / "meow" / "hiss" / "beep") -- real virtual dispatch, one slot
 *       per impl;</li>
 *   <li>a class implementing TWO interfaces ({@code Robot} : {@code Animal},
 *       {@code Named}) dispatches both interface-declared methods;</li>
 *   <li>an ABSTRACT base ({@code AbstractPet}) with a concrete subclass
 *       ({@code Hamster}): the abstract-typed field's runtime klass is the
 *       concrete sub, the abstract {@code sound()} dispatches to the override, and
 *       -- the contrast with interfaces -- the concrete method {@code describe()}
 *       declared on the abstract base IS reachable through the subclass wrapper
 *       because the abstract base is a real {@code _super};</li>
 *   <li>reading the slot as the DECLARED interface type and as the CONCRETE type
 *       yields the SAME decoded OOP (the field decode is type-agnostic);</li>
 *   <li>the interface DEFAULT method {@code defaultGreet()} INHERITED (Dog/Cat)
 *       vs OVERRIDDEN (Snake): the override lands on Snake's own klass and IS
 *       reachable through the Snake wrapper's superclass walk; the INHERITED form
 *       lives only on the {@code Animal} interface, and vmhook's
 *       {@code object::get_method} reaches it through the concrete wrapper by
 *       falling back to the IMPLEMENTED-INTERFACE chain after the superclass walk
 *       (HotSpot's own {@code InstanceKlass::_transitive_interfaces}).  That
 *       fallback is a cold-path {@code os::safe_read} of the interface arrays, so
 *       the native module treats reaching the inherited default as best-effort:
 *       when it resolves, the call body + a byte-for-byte Java cross-check are
 *       asserted HARD; otherwise it records [INFO] rather than failing.</li>
 * </ul>
 *
 * <p>The Java-side probe runs the SAME polymorphism observations across every
 * impl and publishes per-impl witnesses, so the native side can confirm the JVM
 * itself agrees. Canonical go/done handshake with a {@code mode}/done-reset
 * selector drives exactly one dispatch. Java 8 syntax only (anonymous
 * {@code Harness.Probe}, no lambdas / var / switch-expressions).</p>
 */
public final class InterfacePoly
{
    // ── go / done / mode handshake (native sets mode + clears done first) ───
    /** Native sets this true to request the action; clears it after. */
    public static volatile boolean go;

    /** The action sets this true when it has run; native polls it. */
    public static volatile boolean done;

    /**
     * Scenario selector. The native module sets this BEFORE raising {@code go}.
     *   0 = publish the Java-side polymorphism witnesses and call {@code tick()}
     *       once (the bytecode dispatch that fires the native interpreter hook).
     * One scenario suffices: every native observation is a side-effect-free read
     * against the published SINGLETON, so the probe exists only to fire the hook
     * and to (re)publish witnesses on the Java thread.
     */
    public static volatile int mode;

    // ── The nested interface (emitted as InterfacePoly$Animal) ─────────────
    /**
     * The declared (static) type of the holder's {@code pet}/{@code pet2}/
     * {@code pet3}/{@code robotPet} fields. Declares an abstract {@code speak()}
     * every implementation must override, plus a DEFAULT method
     * {@code defaultGreet()} that {@code Dog}/{@code Cat} INHERIT and {@code Snake}
     * OVERRIDES -- the probe target for the interface-default-method limitation.
     */
    public interface Animal
    {
        /**
         * An INTERFACE CONSTANT: every field declared in an interface is
         * implicitly {@code public static final}, so this is a compile-time
         * constant living on the {@code Animal} interface's own mirror (NOT on
         * any implementor). The native side reads it through {@code static_field}
         * keyed on the interface klass, proving a constant declared on an
         * interface is reachable exactly like a class static. ASCII so it lands
         * in the LATIN1 / char-array branch of read_java_string on every JDK.
         */
        String KINGDOM = "animalia";

        /** A primitive interface constant (int), read as a static off the iface. */
        int LEGS_DEFAULT = 4;

        /** Abstract: every implementation overrides this. */
        String speak();

        /**
         * Interface DEFAULT method (Java 8+). INHERITED unchanged by Dog and Cat,
         * OVERRIDDEN by Snake. The inherited form is reachable only by walking the
         * interface chain; vmhook's object::get_method walks the SUPERCLASS chain
         * first and then falls back to the IMPLEMENTED-INTERFACE chain
         * (InstanceKlass::_transitive_interfaces), so the inherited default IS
         * reachable through a concrete wrapper (Dog/Cat). That fallback is a
         * cold-path os::safe_read, so the native module asserts reaching it
         * best-effort (HARD when it resolves, [INFO] otherwise, never a failure).
         * The OVERRIDDEN form lives on Snake's own klass, so it is reached by the
         * superclass walk before the interface fallback even runs.
         */
        default String defaultGreet()
        {
            return "interface-default-greet:" + this.speak();
        }
    }

    // ── A SECOND interface (emitted as InterfacePoly$Named) ────────────────
    /**
     * A second interface so a class can implement MORE THAN ONE. Declares one
     * abstract method {@code who()}; {@code Robot} implements both this and
     * {@code Animal}.
     */
    public interface Named
    {
        /** A SECOND interface's own constant (proves per-interface constants). */
        String REALM = "named-realm";

        /** Abstract: returns the implementor's identity string. */
        String who();
    }

    // ── Concrete implementation #1 (emitted as InterfacePoly$Dog) ──────────
    /**
     * Concrete {@code Animal}. Overrides {@code speak()} (the virtual-dispatch
     * target), carries Dog-specific fields the native side reads, adds a Dog-only
     * method {@code fetch()}, and INHERITS {@code defaultGreet()} unchanged.
     */
    public static final class Dog implements Animal
    {
        public String name;
        public int    age;
        public String breed;

        public Dog(final String name, final int age, final String breed)
        {
            this.name  = name;
            this.age   = age;
            this.breed = breed;
        }

        /** Overrides Animal.speak(); the string the native side matches on. */
        @Override
        public String speak()
        {
            return this.name + " says woof";
        }

        /** Dog-only method (not on the Animal interface). */
        public String fetch()
        {
            return this.name + " fetches the " + this.breed;
        }
    }

    // ── Concrete implementation #2 (emitted as InterfacePoly$Cat) ──────────
    /**
     * A SECOND concrete {@code Animal}. Its {@code speak()} returns an
     * impl-specific string ("meow"), proving virtual dispatch selects each impl's
     * own body. INHERITS {@code defaultGreet()} unchanged (same as Dog).
     */
    public static final class Cat implements Animal
    {
        public String name;
        public boolean indoor;

        public Cat(final String name, final boolean indoor)
        {
            this.name   = name;
            this.indoor = indoor;
        }

        @Override
        public String speak()
        {
            return this.name + " says meow";
        }
    }

    // ── Concrete implementation #3 (emitted as InterfacePoly$Snake) ────────
    /**
     * A third concrete {@code Animal} that OVERRIDES the interface default
     * {@code defaultGreet()}. The override lives on this klass directly, so a
     * superclass walk from Snake finds it (unlike the inherited form on Dog/Cat).
     */
    public static final class Snake implements Animal
    {
        public String name;

        public Snake(final String name)
        {
            this.name = name;
        }

        @Override
        public String speak()
        {
            return this.name + " says hiss";
        }

        /** OVERRIDES the interface default -> reachable via Snake's super walk. */
        @Override
        public String defaultGreet()
        {
            return "snake-greet:" + this.name;
        }
    }

    // ── Concrete implementation of TWO interfaces (InterfacePoly$Robot) ────
    /**
     * Implements BOTH {@code Animal} and {@code Named}. Proves a multi-interface
     * class dispatches each interface-declared method to its own override.
     */
    public static final class Robot implements Animal, Named
    {
        public String id;

        public Robot(final String id)
        {
            this.id = id;
        }

        @Override
        public String speak()
        {
            return this.id + " says beep";
        }

        @Override
        public String who()
        {
            return "robot:" + this.id;
        }
    }

    // ── An ABSTRACT base (emitted as InterfacePoly$AbstractPet) ────────────
    /**
     * An abstract base class (NOT an interface): declares an abstract
     * {@code sound()} subclasses must override, plus a CONCRETE method
     * {@code describe()} with a body that lives on this klass. The contrast with
     * the interface case: an abstract base IS a real superclass, so vmhook's
     * superclass-chain walk DOES reach {@code describe()} through a subclass
     * wrapper -- whereas an interface default method it does not.
     */
    public abstract static class AbstractPet
    {
        public int legs;

        protected AbstractPet(final int legs)
        {
            this.legs = legs;
        }

        /** Abstract: every concrete subclass overrides this. */
        public abstract String sound();

        /** CONCRETE method on the abstract base -> on the _super chain. */
        public String describe()
        {
            return "a pet with " + this.legs + " legs that goes " + this.sound();
        }
    }

    // ── Concrete subclass of the abstract base (InterfacePoly$Hamster) ─────
    /**
     * Concrete {@code AbstractPet}. Overrides {@code sound()}; INHERITS the
     * concrete {@code describe()} from the abstract base (reachable via the super
     * walk through this subclass).
     */
    public static final class Hamster extends AbstractPet
    {
        public String name;

        public Hamster(final String name)
        {
            super(4);
            this.name = name;
        }

        @Override
        public String sound()
        {
            return this.name + " says squeak";
        }
    }

    // ── A SUPER-interface: interface EXTENDS interface (InterfacePoly$LoudAnimal)
    /**
     * An interface that EXTENDS {@code Animal}, adding its own DEFAULT method
     * {@code shout()}.  A class implementing {@code LoudAnimal} transitively
     * implements {@code Animal} too, so vmhook's {@code _transitive_interfaces}
     * walk must surface BOTH {@code shout()} (declared here) and the inherited
     * {@code defaultGreet()} (declared on the super-interface {@code Animal})
     * through a concrete wrapper, even though they live several interface-
     * inheritance hops up.  The abstract {@code speak()} (from {@code Animal}) is
     * reached by the superclass walk via the implementor's own override.
     */
    public interface LoudAnimal extends Animal
    {
        /** DEFAULT on the SUB-interface; reachable via the transitive set. */
        default String shout()
        {
            return "LOUD:" + this.speak();
        }
    }

    /**
     * Concrete implementor of the SUPER-interface {@code LoudAnimal} (and hence,
     * transitively, of {@code Animal}).  Overrides only the abstract
     * {@code speak()}; INHERITS {@code shout()} (from {@code LoudAnimal}) and
     * {@code defaultGreet()} (from the grandparent {@code Animal}) — both must be
     * reached through this wrapper by the implemented-interface fallback walking
     * the transitive interface set.
     */
    public static final class Wolf implements LoudAnimal
    {
        public String name;

        public Wolf(final String name)
        {
            this.name = name;
        }

        @Override
        public String speak()
        {
            return this.name + " says howl";
        }
    }

    // ── A DIAMOND of interfaces sharing ONE inherited default ──────────────
    /**
     * The apex of a diamond: a single DEFAULT method {@code tag()}.  Both
     * {@code DiamondLeft} and {@code DiamondRight} extend this, and
     * {@code Diamond} implements BOTH — so {@code tag()} is reachable through
     * {@code Diamond} along two interface paths but is a SINGLE, unambiguous
     * default (Java allows this; no override is required because both paths lead
     * to the same declaration).  The native side proves the transitive walk
     * resolves to the one real body despite the apex appearing via two paths.
     */
    public interface DiamondTop
    {
        /** The one default shared by both diamond arms. */
        default String tag()
        {
            return "diamond-top-tag";
        }
    }

    /** Left arm of the diamond — extends the apex, adds nothing. */
    public interface DiamondLeft extends DiamondTop
    {
    }

    /** Right arm of the diamond — extends the apex, adds nothing. */
    public interface DiamondRight extends DiamondTop
    {
    }

    /**
     * Implements BOTH diamond arms.  {@code tag()} is inherited (unambiguously)
     * from the shared apex {@code DiamondTop}; the concrete {@code mark()} is its
     * own.  Reaching {@code tag()} through this wrapper exercises the transitive
     * walk over a diamond (the apex appears via two paths, one default body).
     */
    public static final class Diamond implements DiamondLeft, DiamondRight
    {
        public String label;

        public Diamond(final String label)
        {
            this.label = label;
        }

        /** Own concrete method (superclass walk, depth 0). */
        public String mark()
        {
            return "mark:" + this.label;
        }
    }

    // ── TWO interfaces with the SAME-SIGNATURE default + an OVERRIDE ────────
    /**
     * Interface supplying a same-named/same-signature default {@code hello()} as
     * {@code Welcomer}.  A class implementing BOTH must override {@code hello()}
     * (javac rejects the inherited ambiguity otherwise) — so the override lands on
     * the implementor's own klass and the SUPERCLASS walk reaches it first, before
     * the interface fallback's "first in transitive order" tie-break ever runs.
     * This pins the documented precedence: an own/overridden method always wins.
     */
    public interface Greeter
    {
        default String hello()
        {
            return "greeter-hello";
        }
    }

    /** Second interface with the SAME {@code hello()} default signature. */
    public interface Welcomer
    {
        default String hello()
        {
            return "welcomer-hello";
        }
    }

    /**
     * Implements two interfaces that each declare {@code hello()} with the SAME
     * signature.  MUST override {@code hello()} (Java diamond-conflict rule); the
     * override is on this klass, so the superclass walk binds it and the native
     * call dispatches THIS body — never either interface default.  Proves the
     * same-signature-across-interfaces case resolves to the implementor's choice.
     */
    public static final class Concierge implements Greeter, Welcomer
    {
        public String desk;

        public Concierge(final String desk)
        {
            this.desk = desk;
        }

        /** Required override resolving the Greeter/Welcomer ambiguity. */
        @Override
        public String hello()
        {
            return "concierge-hello:" + this.desk;
        }
    }

    // ── An interface inherited from a SUPERCLASS (InterfacePoly$GuardDog) ───
    /**
     * A NON-final concrete class that implements {@code Animal} directly and
     * overrides {@code speak()}.  Exists purely as a real superclass for
     * {@code GuardDog} so the interface ends up on the subclass only
     * TRANSITIVELY (through this superclass), and so the subclass is reachable as
     * a base-CLASS reference (vtable) as well as an {@code Animal} INTERFACE
     * reference (itable).  Carries a name field reused by the inherited
     * {@code speak()} body.
     */
    public static class Watcher implements Animal
    {
        public String name;

        public Watcher(final String name)
        {
            this.name = name;
        }

        @Override
        public String speak()
        {
            return this.name + " says woof";
        }
    }

    /**
     * Extends the concrete {@code Watcher} (which implements {@code Animal}) and
     * adds NOTHING to the interface set itself — so {@code GuardDog} implements
     * {@code Animal} only TRANSITIVELY, through its superclass.  vmhook's
     * {@code _transitive_interfaces} read must still surface {@code Animal} here,
     * so the inherited interface default {@code defaultGreet()} is reachable
     * through a {@code GuardDog} wrapper even though {@code GuardDog} declares no
     * interface directly.  {@code speak()} resolves via the superclass walk
     * (Watcher's override, inherited unchanged).  This same object is the
     * vtable-vs-itable witness: reachable as a {@code Watcher} base-CLASS ref
     * (virtual / vtable) and as an {@code Animal} INTERFACE ref (itable /
     * invokeinterface), dispatching the identical {@code speak()} body either way.
     */
    public static final class GuardDog extends Watcher
    {
        public boolean onDuty;

        public GuardDog(final String name, final boolean onDuty)
        {
            super(name);
            this.onDuty = onDuty;
        }
    }

    // ── A STATIC interface method (Java 8+, NOT a default) ─────────────────
    /**
     * Declares an abstract {@code use()} AND a {@code static} interface method
     * {@code brand()} (legal since Java 8 — note: a PRIVATE interface method is
     * deliberately NOT used here because it is JDK9+ and would break the javac-8
     * fixture build).  The native side characterises that vmhook's interface-
     * default fallback EXCLUDES the static method (it is not inherited by
     * implementors) while the abstract {@code use()} override resolves via the
     * superclass walk, and that the static method is still invocable directly off
     * the interface's own klass via static_method().
     */
    public interface Toolish
    {
        /** Abstract: implementors override. */
        String use();

        /** STATIC interface method (Java 8+): NOT inherited by implementors. */
        static String brand()
        {
            return "toolish-brand";
        }
    }

    /**
     * Concrete {@code Toolish}.  Overrides {@code use()} (superclass walk); does
     * NOT — and cannot — inherit the static {@code brand()}.  The native side
     * asserts the static method is invisible to the implementor's interface-
     * fallback lookup yet callable on the {@code Toolish} klass itself.
     */
    public static final class Gadget implements Toolish
    {
        public String model;

        public Gadget(final String model)
        {
            this.model = model;
        }

        @Override
        public String use()
        {
            return this.model + " in use";
        }
    }

    // ── A GENERIC interface + covariant impl -> javac BRIDGE method ────────
    /**
     * A generic interface whose single method erases to {@code Object get()} in
     * the interface's own bytecode.  A class implementing {@code Box<String>} and
     * declaring {@code String get()} makes javac emit a SYNTHETIC BRIDGE method
     * {@code Object get()} on the IMPLEMENTOR (delegating to the real
     * {@code String get()}), so the implementor's {@code _methods} array carries
     * TWO {@code get} entries with different descriptors.  The native side reads
     * both via get_class_methods and proves dispatch reaches the real (String)
     * body.
     *
     * @param <T> the boxed element type.
     */
    public interface Box<T>
    {
        /** Erases to {@code ()Ljava/lang/Object;} in {@code Box}'s own bytecode. */
        T get();
    }

    /**
     * Implements {@code Box<String>} with a covariant {@code String get()}.  javac
     * emits a synthetic bridge {@code Object get()} alongside it on THIS klass, so
     * the runtime klass exposes both {@code ()Ljava/lang/String;} (real) and
     * {@code ()Ljava/lang/Object;} (bridge) — the erasure witness.
     */
    public static final class StringBox implements Box<String>
    {
        public String value;

        public StringBox(final String value)
        {
            this.value = value;
        }

        /** Covariant override of {@code Box.get()}; the real body. */
        @Override
        public String get()
        {
            return "boxed:" + this.value;
        }
    }

    // ── Deterministic constants the native side mirrors ────────────────────
    public static final String PET_NAME    = "Rex";
    public static final int    PET_AGE     = 5;
    public static final String PET_BREED   = "labrador";
    public static final String CAT_NAME    = "Whiskers";
    public static final String SNAKE_NAME  = "Slither";
    public static final String ROBOT_ID    = "R2";
    public static final String HAMSTER_NAME = "Nibbles";
    public static final String WOLF_NAME    = "Fang";
    public static final String DIAMOND_LABEL = "D1";
    public static final String CONCIERGE_DESK = "front";
    public static final String GUARD_NAME   = "Bruno";
    public static final String GADGET_MODEL = "X1";
    public static final String BOX_VALUE    = "cargo";

    // ── The holder fields: declared interface/abstract, runtime concrete ───
    /**
     * The headline shape: declared type is the interface {@code Animal}, the
     * runtime object is a concrete {@code Dog}. Eagerly initialised so the Dog
     * klass is loaded (and the OOP exists) by the time the native module runs --
     * {@code Main.loadFixtures()} only forName's the top-level fixture, so the
     * nested impls would otherwise stay unloaded.
     */
    public Animal pet = new Dog(PET_NAME, PET_AGE, PET_BREED);

    /** A SECOND impl behind the SAME declared interface type ({@code Cat}). */
    public Animal pet2 = new Cat(CAT_NAME, true);

    /** A THIRD impl that OVERRIDES the interface default ({@code Snake}). */
    public Animal pet3 = new Snake(SNAKE_NAME);

    /** A multi-interface impl behind the {@code Animal} declared type. */
    public Animal robotPet = new Robot(ROBOT_ID);

    /** An abstract-typed field whose runtime object is a concrete subclass. */
    public AbstractPet absPet = new Hamster(HAMSTER_NAME);

    /**
     * A field that holds the SAME object as {@code pet} (shared-ref angle):
     * proves reading the slot through either declared type yields the same
     * decoded OOP. Declared as the concrete type here purely to vary the declared
     * signature; the native side compares decoded identities, not types.
     */
    public Dog petAsDog = (Dog) this.pet;

    /**
     * Declared as the SUPER-interface {@code LoudAnimal}; runtime object is a
     * {@code Wolf}.  Exercises the interface-extends-interface case: reading it
     * as the concrete {@code Wolf} sees the transitive defaults {@code shout()}
     * (sub-interface) and {@code defaultGreet()} (grandparent {@code Animal}).
     */
    public LoudAnimal loud = new Wolf(WOLF_NAME);

    /** Declared as a diamond ARM; runtime object implements both arms. */
    public DiamondLeft diamond = new Diamond(DIAMOND_LABEL);

    /** Same-signature-across-two-interfaces impl (overrides the conflict). */
    public Greeter concierge = new Concierge(CONCIERGE_DESK);

    /**
     * Declared as the {@code Animal} interface but holding a {@code GuardDog}
     * (extends {@code Watcher}, implements {@code Animal} only via its
     * superclass).  The interface is reached transitively-through-a-superclass.
     */
    public Animal guard = new GuardDog(GUARD_NAME, true);

    /** Base-CLASS-typed alias to the SAME GuardDog object (vtable-side read). */
    public Watcher guardAsWatcher = (Watcher) this.guard;

    /** Declared as the {@code Toolish} interface; runtime object is a Gadget. */
    public Toolish gadget = new Gadget(GADGET_MODEL);

    /** Declared as the generic interface {@code Box}; runtime is a StringBox. */
    public Box<String> box = new StringBox(BOX_VALUE);

    // ── Java-side witnesses (so native checks the JVM agrees) ──────────────
    /** True after the probe confirms, Java-side, that pet instanceof Dog and speak() contains "woof". */
    public static volatile boolean petIsDogSeen;

    /** The Dog's speak() result, published so native can compare byte-for-byte. */
    public static volatile String petSpeakSeen = "";

    /** Cat's speak() result (impl #2), published for the native cross-check. */
    public static volatile String catSpeakSeen = "";

    /** Snake's speak() result (impl #3), published for the native cross-check. */
    public static volatile String snakeSpeakSeen = "";

    /** Snake's OVERRIDDEN defaultGreet() result, published for cross-check. */
    public static volatile String snakeGreetSeen = "";

    /**
     * Dog's INHERITED defaultGreet() result (the interface DEFAULT method Dog does
     * NOT override), published so native can cross-check that the implemented-
     * interface fallback in object::get_method, reached through the concrete Dog
     * wrapper, dispatches the SAME default body the JVM does.  Embeds Dog's speak()
     * ("...woof") via the default's body.
     */
    public static volatile String dogGreetSeen = "";

    /** Robot's speak() result (multi-iface, Animal side), published for cross-check. */
    public static volatile String robotSpeakSeen = "";

    /** Robot's who() result (multi-iface, Named side), published for cross-check. */
    public static volatile String robotWhoSeen = "";

    /** Hamster's sound() result (abstract-base override), published for cross-check. */
    public static volatile String hamsterSoundSeen = "";

    /** Hamster's INHERITED describe() result (concrete method on abstract base). */
    public static volatile String hamsterDescribeSeen = "";

    /** Wolf's INHERITED shout() (default on the SUPER-interface LoudAnimal). */
    public static volatile String wolfShoutSeen = "";

    /** Wolf's speak() (abstract on grandparent Animal, overridden on Wolf). */
    public static volatile String wolfSpeakSeen = "";

    /** Wolf's INHERITED defaultGreet() (default on grandparent Animal). */
    public static volatile String wolfGreetSeen = "";

    /** Diamond's INHERITED tag() (single default at the diamond apex). */
    public static volatile String diamondTagSeen = "";

    /** Concierge's OVERRIDDEN hello() (resolves the two-interface conflict). */
    public static volatile String conciergeHelloSeen = "";

    /** GuardDog's speak() via the Dog (CLASS / vtable) reference. */
    public static volatile String guardSpeakViaClassSeen = "";

    /** GuardDog's speak() via the Animal (INTERFACE / itable) reference. */
    public static volatile String guardSpeakViaInterfaceSeen = "";

    /** GuardDog's INHERITED defaultGreet() (Animal iface via the superclass). */
    public static volatile String guardGreetSeen = "";

    /** Gadget's use() (abstract Toolish method, overridden). */
    public static volatile String gadgetUseSeen = "";

    /** Toolish.brand() (STATIC interface method, invoked directly). */
    public static volatile String toolishBrandSeen = "";

    /** StringBox.get() dispatched through the concrete (covariant) reference. */
    public static volatile String boxGetSeen = "";

    /**
     * The {@code Animal.KINGDOM} interface constant as the JVM resolves it
     * (Java-side {@code getstatic} off the interface), published so native can
     * cross-check the value it read off the interface mirror byte-for-byte.
     */
    public static volatile String animalKingdomSeen = "";

    /** The {@code Named.REALM} interface constant (second interface) for cross-check. */
    public static volatile String namedRealmSeen = "";

    /** True after the probe confirms all four Animal impls dispatch distinctly. */
    public static volatile boolean allImplsDistinctSeen;

    /** System.identityHashCode of the pet object, so native identity checks are exact. */
    public static volatile int petIdentity;

    // ── Hook site ──────────────────────────────────────────────────────────
    /**
     * The native module hooks this; calling it through real bytecode dispatch is
     * what makes the interpreter hook fire. All the polymorphism reads in the
     * module happen from native code against the published SINGLETON, so the
     * detour body itself need do nothing but exist.
     */
    public int tick(final int nonce)
    {
        return nonce + 1;
    }

    static
    {
        Harness.register(new Harness.Probe()
        {
            @Override
            public boolean pending()
            {
                return InterfacePoly.go && !InterfacePoly.done;
            }

            @Override
            public void run()
            {
                final InterfacePoly s = SINGLETON;

                // Java-side polymorphism observations: the runtime type IS a Dog,
                // and the overridden speak() reaches Dog's body ("...woof").
                final boolean isDog = (s.pet instanceof Dog);
                final String  spoke = s.pet.speak();
                InterfacePoly.petSpeakSeen = spoke;
                InterfacePoly.petIsDogSeen = isDog && spoke.contains("woof");
                InterfacePoly.petIdentity  = System.identityHashCode(s.pet);

                // The other impls behind the SAME declared interface type: each
                // speak() dispatches to its own override (impl-specific result).
                InterfacePoly.catSpeakSeen   = s.pet2.speak();
                InterfacePoly.snakeSpeakSeen = s.pet3.speak();
                InterfacePoly.snakeGreetSeen = s.pet3.defaultGreet();   // OVERRIDDEN default
                InterfacePoly.dogGreetSeen   = ((Dog) s.pet).defaultGreet(); // INHERITED default (Dog)

                // Multi-interface impl: dispatch through BOTH declared interfaces.
                InterfacePoly.robotSpeakSeen = s.robotPet.speak();      // Animal side
                InterfacePoly.robotWhoSeen   = ((Named) s.robotPet).who(); // Named side

                // Abstract base + concrete sub: the abstract override and the
                // INHERITED concrete method declared on the abstract base.
                InterfacePoly.hamsterSoundSeen    = s.absPet.sound();
                InterfacePoly.hamsterDescribeSeen = s.absPet.describe();

                // Super-interface (interface extends interface): shout() lives on
                // LoudAnimal, defaultGreet() on the grandparent Animal, speak() is
                // Wolf's override — all dispatched through the LoudAnimal ref.
                InterfacePoly.wolfShoutSeen = s.loud.shout();
                InterfacePoly.wolfSpeakSeen = s.loud.speak();
                InterfacePoly.wolfGreetSeen = s.loud.defaultGreet();

                // Diamond: the single inherited apex default reached via two arms.
                InterfacePoly.diamondTagSeen = s.diamond.tag();

                // Same-signature-across-two-interfaces: the required override wins.
                InterfacePoly.conciergeHelloSeen = s.concierge.hello();

                // Interface inherited from a SUPERCLASS + vtable-vs-itable: the
                // SAME GuardDog object reached as a Dog CLASS ref (vtable) and as
                // an Animal INTERFACE ref (itable) dispatches the identical body.
                InterfacePoly.guardSpeakViaClassSeen     = s.guardAsWatcher.speak(); // vtable
                InterfacePoly.guardSpeakViaInterfaceSeen = s.guard.speak();          // itable
                InterfacePoly.guardGreetSeen             = s.guard.defaultGreet();

                // Static interface method (NOT inherited) vs the overridden abstract.
                InterfacePoly.gadgetUseSeen    = s.gadget.use();
                InterfacePoly.toolishBrandSeen = Toolish.brand();

                // Generic interface + covariant impl -> bridge method; dispatch
                // through the declared Box<String> ref reaches the real body.
                InterfacePoly.boxGetSeen = s.box.get();

                // Interface CONSTANTS (implicitly public static final): the JVM's
                // own getstatic off each interface, published for the native
                // cross-check against the value read off the interface mirror.
                InterfacePoly.animalKingdomSeen = Animal.KINGDOM;
                InterfacePoly.namedRealmSeen    = Named.REALM;

                // All four Animal impls produce mutually distinct speak() results.
                InterfacePoly.allImplsDistinctSeen =
                       !spoke.equals(InterfacePoly.catSpeakSeen)
                    && !spoke.equals(InterfacePoly.snakeSpeakSeen)
                    && !spoke.equals(InterfacePoly.robotSpeakSeen)
                    && !InterfacePoly.catSpeakSeen.equals(InterfacePoly.snakeSpeakSeen)
                    && !InterfacePoly.catSpeakSeen.equals(InterfacePoly.robotSpeakSeen)
                    && !InterfacePoly.snakeSpeakSeen.equals(InterfacePoly.robotSpeakSeen);

                // Real bytecode dispatch -> native interpreter hook fires.
                s.tick(7);

                InterfacePoly.done = true;
            }
        });
    }

    /**
     * The single instance the native module wraps and drives. Created eagerly so
     * the native side can fetch it through a static field and so the published
     * identity matches exactly the OOP the module decodes from the pet slot.
     */
    public static final InterfacePoly SINGLETON = new InterfacePoly();
}

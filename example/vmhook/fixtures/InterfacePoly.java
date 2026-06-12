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
 *     holder InterfacePoly {
 *         Animal      pet       = new Dog(...);   // declared Animal, IS a Dog
 *         Animal      pet2      = new Cat(...);   //                  IS a Cat
 *         Animal      pet3      = new Snake(...); //                  IS a Snake (default override)
 *         Animal      robotPet  = new Robot(...); //                  IS a Robot (multi-iface)
 *         AbstractPet absPet    = new Hamster();  // declared abstract, IS a Hamster
 *         Dog         petAsDog  = (Dog) pet;      // concrete-typed alias to the SAME Dog object
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
 *       reachable through the Snake wrapper's superclass walk, while the inherited
 *       form lives only on the interface and characterises vmhook's
 *       superclass-only walk (it walks the superclass chain, not the interface
 *       chain), so the module records [INFO] rather than failing.</li>
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
        /** Abstract: every implementation overrides this. */
        String speak();

        /**
         * Interface DEFAULT method (Java 8+). INHERITED unchanged by Dog and Cat,
         * OVERRIDDEN by Snake. The inherited form is reachable only by walking the
         * interface chain; vmhook's object::get_method walks the SUPERCLASS chain
         * but not the interface chain, so the native module treats reaching the
         * INHERITED form through a concrete wrapper as a best-effort [INFO], never
         * a failure. The OVERRIDDEN form lives on Snake's own klass, so it IS
         * reachable through the Snake wrapper's superclass walk.
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

    // ── Deterministic constants the native side mirrors ────────────────────
    public static final String PET_NAME    = "Rex";
    public static final int    PET_AGE     = 5;
    public static final String PET_BREED   = "labrador";
    public static final String CAT_NAME    = "Whiskers";
    public static final String SNAKE_NAME  = "Slither";
    public static final String ROBOT_ID    = "R2";
    public static final String HAMSTER_NAME = "Nibbles";

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

    /** Robot's speak() result (multi-iface, Animal side), published for cross-check. */
    public static volatile String robotSpeakSeen = "";

    /** Robot's who() result (multi-iface, Named side), published for cross-check. */
    public static volatile String robotWhoSeen = "";

    /** Hamster's sound() result (abstract-base override), published for cross-check. */
    public static volatile String hamsterSoundSeen = "";

    /** Hamster's INHERITED describe() result (concrete method on abstract base). */
    public static volatile String hamsterDescribeSeen = "";

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

                // Multi-interface impl: dispatch through BOTH declared interfaces.
                InterfacePoly.robotSpeakSeen = s.robotPet.speak();      // Animal side
                InterfacePoly.robotWhoSeen   = ((Named) s.robotPet).who(); // Named side

                // Abstract base + concrete sub: the abstract override and the
                // INHERITED concrete method declared on the abstract base.
                InterfacePoly.hamsterSoundSeen    = s.absPet.sound();
                InterfacePoly.hamsterDescribeSeen = s.absPet.describe();

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

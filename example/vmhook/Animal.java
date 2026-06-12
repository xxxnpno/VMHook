package vmhook;

// Interface probe target.  Verifies that vmhook can:
//   - Resolve methods declared on an interface (including default
//     methods, Java 8+).
//   - Resolve static methods declared on an interface (Java 8+).
//   - Walk the interface chain when looking up methods on a concrete
//     implementation class.
public interface Animal
{
    // Abstract method — every implementation must override this.
    String speak();

    // Default method, available on every implementation without an
    // override.  Tests interface-default-method lookup through the
    // implemented-interface fallback in vmhook::object::get_method (which
    // runs after the superclass walk misses, since a default lives on the
    // interface, not on the concrete class or its superclasses).
    default String greet()
    {
        return "Hello, " + this.speak();
    }

    // Static method on an interface.  Not inherited by implementations,
    // but vmhook can still call it via the registered interface class.
    static int kingdomCount()
    {
        return 6;  // animalia, plantae, fungi, protista, monera, archaea
    }
}

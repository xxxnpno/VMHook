package vmhook;

// Concrete implementation of Animal.  Used to verify:
//   - Method override resolution: speak() is declared on Animal but
//     dispatched on Dog.
//   - Inherited default method greet() still works on a Dog instance.
//   - vmhook can read fields declared on the concrete class.
public class Dog implements Animal
{
    public String name;
    public int    age;

    public Dog(final String name, final int age)
    {
        this.name = name;
        this.age  = age;
    }

    @Override
    public String speak()
    {
        return this.name + " says woof";
    }

    // Concrete-class overload of greet() — keeps the default but adds
    // dog-specific behaviour.  vmhook should still find greet() via
    // the interface superclass chain.
    public String wag()
    {
        return this.name + " wags its tail";
    }
}

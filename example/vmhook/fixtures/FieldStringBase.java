package vmhook.fixtures;

/**
 * Superclass of the field_string fixture (area: fields).
 *
 *     FieldStringBase   <-  FieldString
 *
 * It exists solely so the native {@code field_string} module can prove that a
 * String-typed field DECLARED ON A SUPERCLASS is read correctly through the
 * child wrapper (vmhook::find_field walks Klass::get_super(), and the String
 * getter then decodes the inherited slot's compressed OOP exactly like an own
 * field).  Two angles:
 *
 *   - {@code inheritedStr}        an INSTANCE String declared only here, read
 *                                 through a FieldString instance wrapper (super
 *                                 walk depth 1, compressed-OOP decode).
 *   - {@code sInheritedStr}       a STATIC String declared only here, read
 *                                 through the child's static_field() (the same
 *                                 super walk on the class mirror).
 *
 * Both values are pure ASCII so they store as LATIN1 (coder 0) and decode
 * byte-verbatim on a JDK 9+ compact-string VM (and identically on JDK 8 char[]).
 *
 * Package-private top-level class (its public-named file matches the type); it
 * carries no Harness.Probe — Main.loadFixtures() merely loads it, and loading
 * FieldString forces it to load anyway (a subclass requires its super first).
 * Target Java 8 syntax: no var / records.
 */
class FieldStringBase
{
    // Inherited INSTANCE String, declared ONLY on the base.  The child's
    // instance field_proxy resolves it via the super walk (depth 1).
    public String inheritedStr = "base-inherited";

    // Inherited STATIC String, declared ONLY on the base.  Resolved via the
    // same super walk on the java.lang.Class mirror through the child wrapper.
    public static String sInheritedStr = "base-static-inherited";

    // Touch the static so javac never warns it unused and it is guaranteed to
    // be present (and class-initialized) in the layout.
    String describe()
    {
        return this.inheritedStr + "/" + FieldStringBase.sInheritedStr;
    }
}

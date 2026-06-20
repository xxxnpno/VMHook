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

    // Inherited INSTANCE String with NON-ASCII (CJK) content, declared ONLY on
    // the base.  Proves the super-walk field resolution feeds the UTF-16 decode
    // path (the other inherited fields are pure ASCII / LATIN1).  Value 日本
    // (U+65E5 U+672C); built from a char[] so it owns a private backing.
    public String inheritedCjk = new String(new char[]{ (char) 0x65E5, (char) 0x672C });

    // Inherited STATIC String, declared ONLY on the base.  Resolved via the
    // same super walk on the java.lang.Class mirror through the child wrapper.
    public static String sInheritedStr = "base-static-inherited";

    // Inherited STATIC String with NON-ASCII (CJK) content, declared ONLY on the
    // base.  Proves the super walk on the class mirror feeds the UTF-16 decode
    // path (sInheritedStr above is pure ASCII).  Value 語 (U+8A9E, the third kanji
    // of 日本語) -> 3 UTF-8 bytes E8 AA 9E; built from a char[] -> private backing.
    public static String sInheritedCjk = new String(new char[]{ (char) 0x8A9E });

    // Inherited writable INSTANCE String, declared ONLY on the base.  The child's
    // instance field_proxy::set() rebinds it via the super walk, proving SET (not
    // just GET) resolves an inherited slot.  Built from a char[] -> private
    // backing so the rebind cannot disturb a shared literal.
    public String inheritedWritable = new String("base-writable".toCharArray());

    // Touch the static so javac never warns it unused and it is guaranteed to
    // be present (and class-initialized) in the layout.
    String describe()
    {
        return this.inheritedStr + "/" + FieldStringBase.sInheritedStr
                + "/" + this.inheritedCjk + "/" + FieldStringBase.sInheritedCjk
                + "/" + this.inheritedWritable;
    }
}

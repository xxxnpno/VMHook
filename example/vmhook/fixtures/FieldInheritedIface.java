package vmhook.fixtures;

/**
 * Interface in the field_inherited hierarchy (area: fields).
 *
 *     FieldInheritedIface (interface)
 *        ^  implements
 *     FieldInheritedBase   <-  FieldInheritedMid   <-  FieldInherited
 *
 * Declares ONE interface constant ({@code IFACE_CONST}).  In Java every interface
 * field is implicitly {@code public static final}, so this single declaration is
 * the canonical "interface static final inherited by an implementor" shape the
 * native module characterizes.
 *
 * The point this fixture pins is a deliberate, documented LIMITATION of
 * vmhook::find_field: its super-chain walk follows ONLY {@code Klass::_super}
 * (the superclass link), it does NOT consult {@code _transitive_interfaces}.
 * (Contrast vmhook's METHOD lookup, which DOES fall back to the implemented-
 * interface chain for default methods -- see InterfacePoly.)  Consequently the
 * native module proves:
 *   - looked up THROUGH an implementor wrapper (FieldInherited / Mid / Base),
 *     {@code find_field("IFACE_CONST")} walks Base -> Mid -> ... -> Object along
 *     {@code _super} and returns nullopt -- the interface is never on that chain;
 *   - looked up THROUGH the interface's OWN klass (its declaring class), the very
 *     same field resolves as a static at walk depth 0, with the mirrored value.
 *
 * Package-private top-level type (the public-named file matches the type); it
 * carries no Harness.Probe, so Main.loadFixtures() merely loads it.  Java 8.
 */
interface FieldInheritedIface
{
    // Implicitly public static final.  An interface constant lives on the
    // interface's OWN java.lang.Class mirror; it is NOT copied onto an
    // implementor's InstanceKlass field metadata, which is exactly why the
    // _super-only walk cannot reach it from an implementor.
    int IFACE_CONST = 0x1FACE123;
}

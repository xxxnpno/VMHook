package vmhook.fixtures;

/**
 * Interface implemented by {@link MethodExplicitSig}, used by the
 * method_explicit_signature test module to exercise the SECOND-CHANCE
 * interface-DEFAULT-method fallback inside the instance
 * {@code get_method(name, signature)} overload.
 *
 * The superclass-chain walk follows {@code Klass::_super} only and therefore
 * cannot see a method declared on an implemented interface.  An interface
 * DEFAULT method (Java 8+) lives exactly there — on the interface, never copied
 * onto the implementor's own {@code InstanceKlass._methods} array — so resolving
 * {@code get_method("ifaceDefault", "(I)I")} on a MethodExplicitSig instance is
 * the ONLY way the library reaches it, and only via
 * {@code find_interface_default_method}.
 *
 * Two overloads share the name {@code ifaceDefault} so the explicit descriptor
 * (not just the name) is what disambiguates inside the interface walk:
 *   ifaceDefault(I)I    returns arg + 3   (records ifaceDefaultIntSeen)
 *   ifaceDefault(J)J    returns arg + 30  (records ifaceDefaultLongSeen)
 *
 * {@code ifaceAbstract(I)I} is a NON-default (abstract) interface method: the
 * default-method gate inside the fallback must SKIP it (it has no body on the
 * interface), and the concrete override on MethodExplicitSig is the one found by
 * the ordinary superclass/own-method walk.  This characterizes that the
 * interface fallback returns DEFAULTS only, never abstract declarations.
 *
 * Side-effect tallies live on {@link MethodExplicitSigCounters} (interface
 * fields are implicitly final, so a default method cannot mutate one directly).
 *
 * Java 8 syntax only (default methods are Java 8).
 */
interface MethodExplicitSigIface
{
    /** ifaceDefault(I)I — a genuine DEFAULT (has a body). */
    default int ifaceDefault(final int a)
    {
        MethodExplicitSigCounters.ifaceDefaultIntSeen = a;
        return a + 3;
    }

    /** ifaceDefault(J)J — same name, different descriptor; also a DEFAULT. */
    default long ifaceDefault(final long a)
    {
        MethodExplicitSigCounters.ifaceDefaultLongSeen = a;
        return a + 30L;
    }

    /** ifaceAbstract(I)I — ABSTRACT (no body): the default-only gate must skip it. */
    int ifaceAbstract(int a);
}

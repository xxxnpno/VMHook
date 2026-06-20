package vmhook.fixtures;

/**
 * Superclass of {@link MethodExplicitSig}, used by the method_explicit_signature
 * test module to exercise the HIERARCHY WALK inside both explicit-signature
 * {@code get_method(name, signature)} overloads (instance + static-by-type_index):
 * the two {@code base(...)} overloads live here, NOT on the leaf class, so a
 * lookup that matched only the leaf class's own _methods array would miss them.
 *
 * This is a package-private (non-public) top-level class on purpose: the harness
 * fixture loader ({@code Main.loadFixtures}) will {@code Class.forName} it because
 * it sits in vmhook/fixtures, which is harmless — it registers no probe and has
 * no static state.  It must only compile and be a valid superclass.
 *
 * Java 8 syntax only.
 */
class MethodExplicitSigBase
{
    /** base(I)I executed; observable proof the inherited overload ran. */
    public static volatile int baseIntSeen;
    /** base(II)I executed; observable proof. */
    public static volatile int baseIntIntSeen;
    /** base(JI)J executed; records the wide+int mixed-slot args. */
    public static volatile long baseLongIntSeen;
    /** cov() base declaration executed (only when NOT overridden away). */
    public static volatile int covBaseSeen;

    /** base(I)I — distinct descriptor from base(II)I. */
    public int base(final int a)
    {
        baseIntSeen = a;
        return a + 7;
    }

    /** base(II)I — same name, different descriptor. */
    public int base(final int a, final int b)
    {
        baseIntIntSeen = a * 1000 + b;
        return a - b;
    }

    /**
     * base(JI)J — INHERITED wide-mixed-arg overload.  Descriptor (JI)J: a long
     * (two interpreter slots) followed by an int (one slot), returning a long.
     * Exercises the hierarchy walk picking a multi-slot wide-arg overload by its
     * exact descriptor, distinct from base(I)I / base(II)I.  Returns a*1000 + b.
     */
    public long base(final long a, final int b)
    {
        baseLongIntSeen = a * 1000L + b;
        return a * 1000L + b;
    }

    /**
     * cov() — declared to return CharSequence on the base.  MethodExplicitSig
     * overrides this COVARIANTLY to return String (a narrower reference type).
     * The covariant override makes javac emit a SYNTHETIC BRIDGE method on the
     * leaf class with the SUPERTYPE descriptor ()Ljava/lang/CharSequence; plus
     * the real override ()Ljava/lang/String;.  So the leaf _methods array holds
     * TWO cov() entries differing only by return descriptor — the exact-signature
     * lookup must pick each by its full descriptor.  Records covBaseSeen when the
     * UN-overridden base body runs (it should not, since the leaf overrides it).
     */
    public CharSequence cov()
    {
        covBaseSeen++;
        return "base-cov";
    }
}

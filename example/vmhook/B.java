package vmhook;

/**
 * Inheritance probe target.  Verifies that vmhook can:
 *   - Read fields declared directly on this class ({@link #bInt}, {@link #bString}).
 *   - Read fields inherited from {@link A} (e.g. {@code protectedInt}) by
 *     walking the super chain.
 *   - Call methods inherited from {@link A} (e.g. {@code protectedAdd}) by
 *     walking the same chain.
 *   - Distinguish own-class fields from inherited fields when both are
 *     accessed through a B instance.
 */
public class B extends A
{
    // ── Own fields ─────────────────────────────────────────────────────────
    public int    bInt    = 42;
    public String bString = "from_B";

    // ── Own methods ────────────────────────────────────────────────────────
    /** Returns the sum of {@link #bInt} and the inherited protectedInt. */
    public int combinedSum()
    {
        return this.bInt + this.protectedInt;
    }
}

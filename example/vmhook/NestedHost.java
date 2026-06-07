package vmhook;

// Nested-class probe target.  Verifies that vmhook can resolve:
//   - Static nested classes (vmhook.NestedHost$StaticNested)
//   - Inner (non-static) classes (vmhook.NestedHost$Inner) which carry
//     a synthetic outer reference (`this$0`).
//
// Anonymous and local classes are deliberately *not* exercised here:
// their javac-generated names (e.g. NestedHost$1) are unstable across
// recompiles, so vmhook can't rely on a fixed Java class name to
// identify them.  Anonymous-class hooking is exercised separately
// via a hook on a method that happens to use a lambda.
public class NestedHost
{
    public int outerField = 7;

    public static class StaticNested
    {
        public int value;
        public StaticNested(final int v) { this.value = v; }

        public int doubled() { return this.value * 2; }
    }

    public class Inner
    {
        public int innerValue = 99;

        public int outerPlusInner()
        {
            // Reads outerField through the synthetic outer reference.
            return outerField + this.innerValue;
        }
    }

    public Inner newInner()
    {
        return new Inner();
    }
}

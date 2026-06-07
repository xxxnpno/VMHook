package vmhook;

import java.lang.management.ManagementFactory;
import java.util.function.BooleanSupplier;

public class Main
{
    // this boolean must be called by vmhook.dll
    static boolean stopJVM = false;

    public static void main(final String[] args) throws InterruptedException, ClassNotFoundException
    {
        final String runtimeName = ManagementFactory.getRuntimeMXBean().getName();

        final String pidString = runtimeName.contains("@") ? runtimeName.substring(0, runtimeName.indexOf('@')) : runtimeName;

        System.out.println("[INFO] JVM PID : " + pidString);

        System.out.println("[INFO] JVM : " + System.getProperty("java.vm.name") + " " + System.getProperty("java.version"));

        System.out.println("Waiting for vmhook.dll injection...");

        // Eagerly load every probe class so vmhook's klass walker sees
        // them.  HotSpot lazy-loads classes; without these explicit
        // Class.forName calls the C++ side would see "class not found"
        // until something Java-side actually instantiated them.
        Class.forName("vmhook.Example");
        Class.forName("vmhook.A");
        Class.forName("vmhook.B");
        Class.forName("vmhook.Color");
        Class.forName("vmhook.Animal");
        Class.forName("vmhook.Dog");
        Class.forName("vmhook.NestedHost");
        Class.forName("vmhook.NestedHost$StaticNested");
        Class.forName("vmhook.NestedHost$Inner");
        Class.forName("vmhook.CallerProbe");
        Class.forName("vmhook.TickerProbe");
        // LateClass is deliberately NOT loaded here — the class-load probe
        // triggers it later so the C++ on_class_loaded watcher observes it.

        // Modular harness: load every vmhook.fixtures.* class so its static
        // initializer registers its Harness.Probe.  Conflict-free — each
        // feature drops one fixture class and it is discovered here.
        loadFixtures();

        // we let vmhook.dll do its things in the jvm
        // vmhook has to get_field("stopJVM")->set(true) to stop the JVM
        while(!stopJVM)
        {
            runProbe(() -> Example.hookProbeRequested && !Example.hookProbeDone,
                () -> Example.instance.nonStaticCallMe(77),
                () -> Example.hookProbeDone = true);

            runProbe(() -> Example.forceReturnProbeRequested && !Example.forceReturnProbeDone,
                () -> Example.forceReturnProbeValue = Example.instance.nonStaticReturnMe(77),
                () -> Example.forceReturnProbeDone = true);

            runProbe(() -> Example.cancelProbeRequested && !Example.cancelProbeDone,
                () -> Example.instance.nonStaticCancelMe(9),
                () -> Example.cancelProbeDone = true);

            runProbe(() -> Example.staticForceReturnProbeRequested && !Example.staticForceReturnProbeDone,
                () -> Example.staticForceReturnProbeValue = Example.staticReturnMe(77),
                () -> Example.staticForceReturnProbeDone = true);

            runProbe(() -> Example.makeUniqueProbeRequested && !Example.makeUniqueProbeDone,
                () ->
                {
                    final Object tlabRefill = new Object();
                    Example.instance.nonStaticCallMe(88);
                    if (tlabRefill == null)
                    {
                        throw new IllegalStateException("unreachable");
                    }
                },
                () -> Example.makeUniqueProbeDone = true);

            runProbe(() -> Example.listProbeRequested && !Example.listProbeDone,
                () -> Example.listProbeSize = Example.instance.listOfAs.size(),
                () -> Example.listProbeDone = true);

            // Mirror probes for the new container fixtures.  Each one reports
            // the Java-side size; the C++ harness then reads the container
            // through the matching wrapper and asserts the sizes match.
            runProbe(() -> Example.linkedListProbeRequested && !Example.linkedListProbeDone,
                () -> Example.linkedListProbeSize = Example.instance.linkedListOfAs.size(),
                () -> Example.linkedListProbeDone = true);

            runProbe(() -> Example.setProbeRequested && !Example.setProbeDone,
                () -> Example.setProbeSize = Example.instance.setOfAs.size(),
                () -> Example.setProbeDone = true);

            runProbe(() -> Example.mapProbeRequested && !Example.mapProbeDone,
                () -> Example.mapProbeSize = Example.instance.mapOfAs.size(),
                () -> Example.mapProbeDone = true);

            runProbe(() -> Example.hashMapProbeRequested && !Example.hashMapProbeDone,
                () -> Example.hashMapProbeSize = Example.instance.hashMapOfAs.size(),
                () -> Example.hashMapProbeDone = true);

            runProbe(() -> Example.treeMapProbeRequested && !Example.treeMapProbeDone,
                () -> Example.treeMapProbeSize = Example.instance.treeMapOfAs.size(),
                () -> Example.treeMapProbeDone = true);

            runProbe(() -> Example.polyProbeRequested && !Example.polyProbeDone,
                () ->
                {
                    Example.polyProbeInheritedField  = (Example.instance.bInstance.protectedInt == 1337);
                    Example.polyProbeInheritedMethod = (Example.instance.bInstance.protectedAdd(3) == 1340);
                    Example.polyProbeOwnField        = (Example.instance.bInstance.bInt == 42);
                },
                () -> Example.polyProbeDone = true);

            runProbe(() -> Example.methodCallReturnProbeRequested && !Example.methodCallReturnProbeDone,
                () -> Example.instance.nonStaticCallMe(99),
                () -> Example.methodCallReturnProbeDone = true);

            runProbe(() -> Example.argMutationProbeRequested && !Example.argMutationProbeDone,
                () -> Example.instance.nonStaticArgMutationMe(7),
                () -> Example.argMutationProbeDone = true);

            runProbe(() -> Example.stringArgMutationProbeRequested && !Example.stringArgMutationProbeDone,
                () -> Example.instance.nonStaticStringArgMutationMe("before"),
                () -> Example.stringArgMutationProbeDone = true);

            // ── New probes ──────────────────────────────────────────────────

            // Enum probe: call brightness() on the favorite-color singleton
            // so the C++ side can verify that vmhook resolves enum
            // instance methods.
            runProbe(() -> Example.enumProbeRequested && !Example.enumProbeDone,
                () -> Example.enumProbeBrightness = Example.instance.favoriteColor.brightness(),
                () -> Example.enumProbeDone = true);

            // Interface probe: call the static method on the Animal
            // interface and the default greet() method on the Dog
            // implementation, so the C++ side can verify both lookup paths.
            runProbe(() -> Example.interfaceProbeRequested && !Example.interfaceProbeDone,
                () ->
                {
                    Example.interfaceProbeKingdoms = Animal.kingdomCount();
                    // Just exercise the default method so vmhook can find it
                    // via the interface superclass walk on the Dog wrapper.
                    Example.instance.animal.greet();
                },
                () -> Example.interfaceProbeDone = true);

            // Nested-class probe: read a field through the synthetic outer
            // reference of an Inner instance.
            runProbe(() -> Example.nestedProbeRequested && !Example.nestedProbeDone,
                () -> Example.nestedProbeValue = Example.instance.innerInst.outerPlusInner(),
                () -> Example.nestedProbeDone = true);

            // Throwing-method probe: call the throwing method with a
            // negative input; observe that the IllegalStateException is
            // raised and caught here.  The C++ side hooks the method and
            // verifies it can read the parameters.
            runProbe(() -> Example.throwProbeRequested && !Example.throwProbeDone,
                () ->
                {
                    try
                    {
                        Example.instance.throwsCheckedException(-1);
                    }
                    catch (final IllegalStateException ex)
                    {
                        Example.throwProbeExceptionSeen = true;
                    }
                },
                () -> Example.throwProbeDone = true);

            // Overload probe: call each of the three overload(...) variants.
            runProbe(() -> Example.overloadProbeRequested && !Example.overloadProbeDone,
                () ->
                {
                    Example.overloadProbeIntResult  = Example.instance.overload(13);
                    Example.overloadProbeStrResult  = Example.instance.overload("foo");
                    Example.overloadProbeDualResult = Example.instance.overload(2, 3);
                },
                () -> Example.overloadProbeDone = true);

            // Return-types probe: exercise every primitive return type so
            // the C++ side can verify method_proxy::call() handles each.
            runProbe(() -> Example.returnTypesProbeRequested && !Example.returnTypesProbeDone,
                () ->
                {
                    int accum = 0;
                    accum += Example.instance.returnsBool()  ? 1 : 0;
                    accum += Example.instance.returnsByte();
                    accum += Example.instance.returnsShort();
                    accum += Example.instance.returnsInt() >>> 24;
                    accum += (int)(Example.instance.returnsLong() >>> 56);
                    accum += (int) Example.instance.returnsFloat();
                    accum += (int) Example.instance.returnsDouble();
                    accum += Example.instance.returnsChar();
                    if (Example.instance.returnsNull() == null)
                    {
                        accum += 999;
                    }
                    Example.returnTypesProbeAccum = accum;
                },
                () -> Example.returnTypesProbeDone = true);

            // Caller-info probe: outerStep(7) calls innerStep(8); innerStep is
            // hooked on the C++ side and asks return_value::caller() for the
            // outerStep frame.
            runProbe(() -> CallerProbe.probeRequested && !CallerProbe.probeDone,
                () ->
                {
                    final CallerProbe probe = new CallerProbe();
                    CallerProbe.observedSum = probe.outerStep(7);
                },
                () -> CallerProbe.probeDone = true);

            // Field-watcher probe: bump TickerProbe.counter several times so
            // the C++ watch_static_field watcher observes the transitions.
            runProbe(() -> TickerProbe.probeRequested && !TickerProbe.probeDone,
                () ->
                {
                    for (int i = 0; i < 25; i++)
                    {
                        TickerProbe.tick();
                        try { Thread.sleep(2); } catch (final InterruptedException ie) { /* ignore */ }
                    }
                },
                () -> TickerProbe.probeDone = true);

            // Class-load probe: trigger loading of LateClass so the C++
            // on_class_loaded watcher observes a brand-new klass.
            runProbe(() -> Example.classLoadProbeRequested && !Example.classLoadProbeDone,
                () ->
                {
                    try
                    {
                        Class.forName("vmhook.LateClass");
                    }
                    catch (final ClassNotFoundException ex)
                    {
                        // Print to stderr; the C++ test will still fail
                        // because the class never appears.
                        System.err.println("LateClass not on classpath: " + ex);
                    }
                },
                () -> Example.classLoadProbeDone = true);

            // Edge-value probe: confirm the boundary primitive fields are
            // still readable through Java itself (sanity check that nothing
            // upstream corrupted them).  The C++ side ALSO reads them and
            // compares directly.
            runProbe(() -> Example.edgeProbeRequested && !Example.edgeProbeDone,
                () ->
                {
                    boolean all =
                           Example.intMinValue  == Integer.MIN_VALUE
                        && Example.intMaxValue  == Integer.MAX_VALUE
                        && Example.longMinValue == Long.MIN_VALUE
                        && Example.longMaxValue == Long.MAX_VALUE
                        && Float.isNaN(Example.floatNaN)
                        && Example.floatPosInf  == Float.POSITIVE_INFINITY
                        && Example.floatNegInf  == Float.NEGATIVE_INFINITY
                        && Double.isNaN(Example.doubleNaN)
                        && Example.doublePosInf == Double.POSITIVE_INFINITY
                        && Example.doubleNegInf == Double.NEGATIVE_INFINITY
                        && Example.nullString   == null
                        && Example.emptyString.isEmpty()
                        && Example.finalInt     == 0xC0FFEE;
                    Example.edgeProbeAllSeen = all;
                },
                () -> Example.edgeProbeDone = true);

            // Modular harness: run any pending feature-fixture probe.
            Harness.tickAll();

            Thread.sleep(1);
        }

        // Phase 3: Java-side verification that C++ setter effects are visible.
        System.out.println("[INFO] Running Java-side setter verification...");

        if (!Example.verifySetterEffects())
        {
            System.out.println("[JAVA FAIL] One or more setter verifications failed.");
            System.exit(1);
        }

        System.out.println("[JAVA PASS] All setter verifications passed.");
    }

    private static void runProbe(final BooleanSupplier shouldRun, final Runnable action, final Runnable markDone)
    {
        if (!shouldRun.getAsBoolean())
        {
            return;
        }

        action.run();
        markDone.run();
    }

    /**
     * Discover and load every compiled vmhook.fixtures.* class so its static
     * initializer registers its Harness.Probe.  Scans each classpath directory
     * entry for a vmhook/fixtures sub-directory and Class.forName's each
     * top-level .class file there.  Robust to the classpath being any directory
     * (CI uses "out"; local runs use a temp dir).
     */
    private static void loadFixtures()
    {
        final String classpath = System.getProperty("java.class.path", "");
        int loaded = 0;
        for (final String entry : classpath.split(java.io.File.pathSeparator))
        {
            final java.io.File dir = new java.io.File(entry, "vmhook/fixtures");
            if (!dir.isDirectory())
            {
                continue;
            }
            final java.io.File[] files = dir.listFiles(
                (d, name) -> name.endsWith(".class") && !name.contains("$"));
            if (files == null)
            {
                continue;
            }
            for (final java.io.File file : files)
            {
                final String simple = file.getName().substring(0, file.getName().length() - ".class".length());
                final String fqcn = "vmhook.fixtures." + simple;
                try
                {
                    Class.forName(fqcn);
                    System.out.println("[INFO] loaded fixture " + fqcn);
                    loaded++;
                }
                catch (final Throwable t)
                {
                    System.err.println("[WARN] fixture load failed " + fqcn + ": " + t);
                }
            }
        }
        System.out.println("[INFO] loaded " + loaded + " fixture(s).");
    }
}

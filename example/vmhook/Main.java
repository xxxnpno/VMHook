package vmhook;

import java.lang.management.ManagementFactory;

public class Main
{
    // this boolean must be called by vmhook.dll
    static boolean stopJVM = false;

    public static void main(final String[] args) throws InterruptedException
    {
        final String runtimeName = ManagementFactory.getRuntimeMXBean().getName();

        final String pidString = runtimeName.contains("@") ? runtimeName.substring(0, runtimeName.indexOf('@')) : runtimeName;

        System.out.println("[INFO] JVM PID : " + pidString);

        System.out.println("[INFO] JVM : " + System.getProperty("java.vm.name") + " " + System.getProperty("java.version"));

        System.out.println("Waiting for vmhook.dll injection...");

        // Modular harness: load every vmhook.fixtures.* class so its static
        // initializer registers its Harness.Probe.  Conflict-free — each
        // feature drops one fixture class and it is discovered here.  (Rework D
        // retired the legacy top-level probe classes + their eager Class.forName
        // loads; every feature now owns a self-contained fixture.)
        loadFixtures();

        // vmhook.dll drives the modular harness on its own worker thread; it
        // sets get_field("stopJVM")->set(true) once the C++ run_all() suite
        // finishes.  Each tick services any fixture probe the native side
        // requested via Harness.
        while (!stopJVM)
        {
            Harness.tickAll();

            Thread.sleep(1);
        }
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

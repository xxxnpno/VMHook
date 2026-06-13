package vmhook.fixtures;

import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.atomic.AtomicInteger;

import vmhook.Harness;

/**
 * Fixture for the for_each_thread feature (area: threads / HotSpot thread list).
 *
 * vmhook::for_each_thread() walks the JVM's live JavaThread list (Path 1: the
 * classic intrusive Threads::_thread_list; Path 2: the JDK 10+ SMR ThreadsList
 * snapshot) and hands the native visitor a thread_info per live Java thread.
 * The thread_info carries only {JavaThread*, state, os_thread_id} -- there is no
 * thread NAME, so the module cannot match the spawned thread by name.  Instead
 * this fixture lets the native side prove enumeration TRACKS a newly-created
 * Java thread by an exact LIVE-COUNT / POINTER-IDENTITY DELTA: enumerate a
 * baseline, start an extra named daemon thread (or a BATCH of them) that parks
 * itself alive, enumerate again (brand-new live (ptr,tid) identities must
 * appear), then release the thread(s) and enumerate again (those identities must
 * leave).  This is JDK-portable across Path 1 and Path 2 and needs no native
 * OS-TID-from-Java trick.
 *
 * Coordination is the standard go/done handshake plus a `mode` selector.  Extra
 * volatile flags bridge the spawned threads' lifecycle to the native side:
 *   - `threadUp`  : the SINGLE worker (mode 1) sets this true once it is running
 *                   and parked (so the native side can WAIT for the worker to
 *                   become a live JavaThread before re-enumerating).  Native
 *                   reads it directly via static_field("threadUp")->get() -- a
 *                   plain heap read, no bytecode dispatch, so it works off the
 *                   Java thread.
 *   - `stop`      : native sets this true to ask the parked worker(s) to exit;
 *                   each worker observes it and returns, after which the JVM
 *                   reclaims its JavaThread and the live count drops back.  The
 *                   SAME stop flag releases both the single worker and the batch.
 *   - `upCount`   : how many BATCH workers (mode 3) are currently running and
 *                   parked.  Native polls it to wait for the batch to attach
 *                   (rises to workerCount) and to confirm the batch has drained
 *                   (falls to 0) after `stop`.  A plain int heap read.
 *   - `workerCount`: native sets the desired BATCH size before raising go for
 *                   mode 3.  Clamped to [0, MAX_BATCH] so a wild value can never
 *                   spawn a runaway number of threads.
 *
 * mode selector (native sets `mode` + clears `done` on the rising edge of go):
 *   1 = startWorker(): create + start ONE named daemon worker, then return
 *       (done=true) WITHOUT joining -- the worker stays parked & alive so the
 *       native side can enumerate it.  Idempotent: a second mode-1 with the
 *       worker already up is a no-op.
 *   2 = (no-op tick) -- a plain done=true so the native side has a cheap probe
 *       cycle available if it needs one; the actual worker release is driven by
 *       the `stop` flag, which native can set directly (no probe required).
 *   3 = startWorkers(): create + start `workerCount` (clamped) named daemon
 *       workers, each of which parks itself alive exactly like the single worker
 *       and increments/decrements `upCount` around its parked lifetime.  Returns
 *       without joining.  Idempotent-ish: a second mode-3 only tops the pool back
 *       up to `workerCount` live workers, never exceeding it.
 *   4 = joinWorkers(): set `stop`, then JOIN every spawned worker (single +
 *       batch) with a bounded per-thread timeout and clear the pools, so no
 *       spawned thread can leak into a downstream test module.  Bounded so it can
 *       never wedge the Harness tick loop even if a worker is stuck.  Native
 *       drives this as the unconditional cleanup step.
 *
 * Every worker is a DAEMON so it can never wedge JVM shutdown even if the native
 * side fails before setting `stop`; each also self-times-out after a generous
 * bound so a crashed/aborted native run cannot leak a busy thread.
 *
 * Java 8 syntax only (anonymous Runnable + anonymous Probe; no var/lambda).
 */
public final class ForEachThread
{
    /** Native sets this true to request the action; clears it after. */
    public static volatile boolean go;

    /** The action sets this true when it has run; native polls it. */
    public static volatile boolean done;

    /** Scenario selector; native sets it before raising go. */
    public static volatile int mode;

    /** The exact name the single spawned worker thread is given. */
    public static final String WORKER_NAME = "vmhook-fet-probe";

    /** Name prefix for the batch workers (each gets PREFIX + index). */
    public static final String BATCH_WORKER_PREFIX = "vmhook-fet-batch-";

    /** Hard upper bound on the batch size, mirroring native's sane-count intent. */
    public static final int MAX_BATCH = 64;

    /** Set true by the single worker once it is running and parked. */
    public static volatile boolean threadUp;

    /** Native sets this true to ask the parked worker(s) to exit. */
    public static volatile boolean stop;

    /** Desired BATCH size; native sets it before raising go for mode 3. */
    public static volatile int workerCount;

    /** How many BATCH workers are currently running and parked. */
    public static volatile int upCount;

    /** The single live worker (null until started). */
    private static volatile Thread worker;

    /** The batch workers (empty until mode 3). */
    private static final List<Thread> batch = new ArrayList<Thread>();

    /** Live count of running batch workers, bridged to {@code upCount}. */
    private static final AtomicInteger liveBatch = new AtomicInteger(0);

    /** Generous self-timeout (ns) so an aborted native run cannot leak a worker. */
    private static final long SELF_TIMEOUT_NANOS = 30_000_000_000L; // 30s

    /**
     * Creates and starts the named daemon worker if it is not already running.
     * Returns immediately; the worker parks itself alive until {@code stop} is
     * set (or a self-timeout fires).  Called on the Harness tick thread.
     */
    private static void startWorker()
    {
        if (worker != null && worker.isAlive())
        {
            // Already up -- mode 1 is idempotent.
            return;
        }
        stop = false;
        threadUp = false;

        final Thread t = new Thread(new Runnable()
        {
            @Override
            public void run()
            {
                // Announce we are alive and parked so the native side can
                // re-enumerate and see the count rise by one.
                ForEachThread.threadUp = true;

                parkUntilStop();

                ForEachThread.threadUp = false;
            }
        }, WORKER_NAME);

        t.setDaemon(true);
        worker = t;
        t.start();
    }

    /**
     * Tops the batch pool up to {@code workerCount} (clamped to [0, MAX_BATCH])
     * live daemon workers.  Each worker parks itself alive until {@code stop}
     * (or a self-timeout) and maintains {@code upCount} around its lifetime, so
     * the native side can wait for the whole batch to attach and later drain.
     * Returns immediately without joining.  Called on the Harness tick thread.
     */
    private static void startWorkers()
    {
        int want = ForEachThread.workerCount;
        if (want < 0)
        {
            want = 0;
        }
        if (want > MAX_BATCH)
        {
            want = MAX_BATCH;
        }

        stop = false;

        synchronized (batch)
        {
            // Drop any references to workers that have already exited so a
            // repeated mode-3 tops the pool back up rather than over-spawning.
            final List<Thread> stillAlive = new ArrayList<Thread>();
            for (int i = 0; i < batch.size(); i++)
            {
                final Thread existing = batch.get(i);
                if (existing != null && existing.isAlive())
                {
                    stillAlive.add(existing);
                }
            }
            batch.clear();
            batch.addAll(stillAlive);

            for (int i = batch.size(); i < want; i++)
            {
                final Thread t = new Thread(new Runnable()
                {
                    @Override
                    public void run()
                    {
                        ForEachThread.liveBatch.incrementAndGet();
                        ForEachThread.upCount = ForEachThread.liveBatch.get();
                        try
                        {
                            parkUntilStop();
                        }
                        finally
                        {
                            ForEachThread.liveBatch.decrementAndGet();
                            ForEachThread.upCount = ForEachThread.liveBatch.get();
                        }
                    }
                }, BATCH_WORKER_PREFIX + i);

                t.setDaemon(true);
                batch.add(t);
                t.start();
            }
        }
    }

    /**
     * Sets {@code stop} and JOINs every spawned worker (single + batch) with a
     * bounded per-thread timeout, then clears the pools.  Bounded so it can never
     * wedge the Harness tick loop even if a worker is stuck; daemon status means
     * any worker that somehow outlives the join still cannot wedge JVM shutdown.
     * Called on the Harness tick thread as the native side's cleanup step.
     */
    private static void joinWorkers()
    {
        stop = true;

        // OVERALL wall-clock budget for the whole join, independent of N, so this
        // can never block the Harness tick thread longer than the native probe's
        // own ~5 s wait (the probe runs THIS method on the tick thread).  Because
        // stop is already set, every live worker returns from its 5 ms sleep loop
        // almost immediately, so in practice the joins complete in a few ms; the
        // budget only bounds a pathological wedged worker (which daemon status
        // already makes harmless to JVM shutdown anyway).
        final long joinDeadlineNanos = System.nanoTime() + 3_000_000_000L; // 3s

        final Thread single = worker;
        if (single != null)
        {
            joinBounded(single, joinDeadlineNanos);
            worker = null;
        }

        final List<Thread> toJoin;
        synchronized (batch)
        {
            toJoin = new ArrayList<Thread>(batch);
            batch.clear();
        }
        for (int i = 0; i < toJoin.size(); i++)
        {
            final Thread t = toJoin.get(i);
            if (t == null)
            {
                continue;
            }
            if (!joinBounded(t, joinDeadlineNanos))
            {
                // Budget exhausted or interrupted — stop joining the rest; they
                // are daemons with a self-timeout and cannot wedge shutdown.
                break;
            }
        }
    }

    /**
     * Joins {@code t} but never past {@code deadlineNanos} (and never less than a
     * tiny floor so a live worker that is mid-sleep still gets a chance to exit).
     * Returns false if the deadline was reached or the join was interrupted.
     */
    private static boolean joinBounded(final Thread t, final long deadlineNanos)
    {
        final long remainingNanos = deadlineNanos - System.nanoTime();
        if (remainingNanos <= 0L)
        {
            return false;
        }
        long remainingMillis = remainingNanos / 1_000_000L;
        if (remainingMillis < 20L)
        {
            remainingMillis = 20L; // floor: one or two sleep ticks
        }
        try
        {
            t.join(remainingMillis);
            return !t.isAlive();
        }
        catch (final InterruptedException ignored)
        {
            Thread.currentThread().interrupt();
            return false;
        }
    }

    /**
     * Parks the calling worker alive until {@code stop} is set or the generous
     * self-timeout fires.  Shared by the single worker and every batch worker.
     */
    private static void parkUntilStop()
    {
        final long deadlineNanos = System.nanoTime() + SELF_TIMEOUT_NANOS;
        while (!ForEachThread.stop && System.nanoTime() < deadlineNanos)
        {
            try
            {
                Thread.sleep(5);
            }
            catch (final InterruptedException ignored)
            {
                // Treat an interrupt as a stop request.
                break;
            }
        }
    }

    static
    {
        Harness.register(new Harness.Probe()
        {
            @Override
            public boolean pending()
            {
                return ForEachThread.go && !ForEachThread.done;
            }

            @Override
            public void run()
            {
                switch (ForEachThread.mode)
                {
                    case 1:
                        startWorker();
                        break;
                    case 3:
                        startWorkers();
                        break;
                    case 4:
                        joinWorkers();
                        break;
                    case 2:
                    default:
                        // Plain tick: nothing to do but acknowledge.
                        break;
                }
                ForEachThread.done = true;
            }
        });
    }
}

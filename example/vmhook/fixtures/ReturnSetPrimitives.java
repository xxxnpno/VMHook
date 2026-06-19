package vmhook.fixtures;

import vmhook.Harness;

/**
 * Fixture for the return_value::set(value) force-return feature (area: hooks).
 *
 * Each {@code orig*} method has a hard-coded "original" return value that is
 * deliberately DIFFERENT from any value the native side forces, so a passing
 * test proves the trampoline skipped the original body and delivered the
 * native return slot to the Java caller.  The probe action calls every method
 * once through normal bytecode dispatch (which is what makes the interpreter
 * hook fire) and stores what the caller *observed* into a per-type field.  The
 * native module re-arms its hooks with new boundary values each round and
 * re-runs the probe, so one dumb fixture covers an unbounded number of forced
 * values.
 *
 * Coverage spans both INSTANCE methods (implicit 'this' in slot 0) and STATIC
 * methods (no 'this'), every primitive return type
 * (boolean/byte/short/int/long/float/double/char), and the two control fields
 * (`sawException`, `callCount`) the native side asserts on for the
 * exception-safety and once-per-call angles.
 *
 * Shape matches Pilot.java exactly: a go/done handshake plus a static-block
 * self-registration of a Harness.Probe.
 */
public final class ReturnSetPrimitives
{
    /** Native sets this true to request the action; clears it after. */
    public static volatile boolean go;

    /** The action sets this true when it has run; native polls it. */
    public static volatile boolean done;

    // ---- Observed return values (what the Java caller actually received) ----
    // One field per primitive, for BOTH instance and static dispatch paths.
    // Pre-seeded to a sentinel that no test ever forces, so a hook that never
    // fired is distinguishable from a hook that forced the sentinel.
    public static volatile boolean obsBool;
    public static volatile byte    obsByte    = (byte)   0x5A;
    public static volatile short   obsShort   = (short)  0x5AA5;
    public static volatile int     obsInt     =          0x5A5A5A5A;
    public static volatile long    obsLong    = 0x5A5A5A5A5A5A5A5AL;
    public static volatile float   obsFloat   = 1234.5678f;
    public static volatile double  obsDouble  = 9876.54321;
    public static volatile char    obsChar    = (char)   0x5A5A;

    public static volatile boolean obsStaticBool;
    public static volatile byte    obsStaticByte   = (byte)  0x5A;
    public static volatile short   obsStaticShort  = (short) 0x5AA5;
    public static volatile int     obsStaticInt    =         0x5A5A5A5A;
    public static volatile long    obsStaticLong   = 0x5A5A5A5A5A5A5A5AL;
    public static volatile float   obsStaticFloat  = 1234.5678f;
    public static volatile double  obsStaticDouble = 9876.54321;
    public static volatile char    obsStaticChar   = (char)  0x5A5A;

    // ---- Sub-int returns captured WIDENED to int -----------------------------
    // A byte/short/char method returns an int-on-stack value (the ireturn
    // family); the JVM masks the forced 64-bit slot to the declared width on the
    // caller side.  Assigning that masked narrow return into an int local makes
    // the caller-side widening visible at full 32-bit resolution: byte/short
    // SIGN-extend, char ZERO-extends.  These are captured by a SECOND dispatch of
    // the same orig* method (so the byte/short/char hooks fire twice per probe).
    public static volatile int obsByteAsInt        = 0x5A5A5A5A;
    public static volatile int obsShortAsInt       = 0x5A5A5A5A;
    public static volatile int obsCharAsInt        = 0x5A5A5A5A;
    public static volatile int obsStaticByteAsInt  = 0x5A5A5A5A;
    public static volatile int obsStaticShortAsInt = 0x5A5A5A5A;
    public static volatile int obsStaticCharAsInt  = 0x5A5A5A5A;

    // ---- Control observations for the error / lifecycle angles ----
    /** Set true by the action if any orig* call threw (it must never throw). */
    public static volatile boolean sawException;
    /** How many times the per-round action body ran (sanity for run_probe). */
    public static volatile int     roundCount;
    /**
     * Bytewise echo of obsInt as seen through a SECOND read after the call,
     * used to prove the value is stable (not a transient register artifact).
     */
    public static volatile int     obsIntReadback;

    // The single instance the instance-method hooks dispatch through.
    public static final ReturnSetPrimitives INSTANCE = new ReturnSetPrimitives();

    // ---- Original-return methods (instance) ----
    // Each returns a fixed value the native side never forces.
    public boolean origBool()   { return false; }
    public byte    origByte()   { return (byte) 11; }
    public short   origShort()  { return (short) 111; }
    public int     origInt()    { return 1111; }
    public long    origLong()   { return 1111L; }
    public float   origFloat()  { return 11.5f; }
    public double  origDouble() { return 11.25; }
    public char    origChar()   { return 'A'; }

    // ---- Original-return methods (static) ----
    public static boolean origStaticBool()   { return false; }
    public static byte    origStaticByte()   { return (byte) 22; }
    public static short   origStaticShort()  { return (short) 222; }
    public static int     origStaticInt()    { return 2222; }
    public static long    origStaticLong()   { return 2222L; }
    public static float   origStaticFloat()  { return 22.5f; }
    public static double  origStaticDouble() { return 22.25; }
    public static char    origStaticChar()   { return 'B'; }

    static
    {
        Harness.register(new Harness.Probe()
        {
            @Override
            public boolean pending()
            {
                return ReturnSetPrimitives.go && !ReturnSetPrimitives.done;
            }

            @Override
            public void run()
            {
                final ReturnSetPrimitives self = ReturnSetPrimitives.INSTANCE;
                boolean threw = false;
                try
                {
                    // INSTANCE dispatch — implicit 'this' occupies local slot 0.
                    ReturnSetPrimitives.obsBool   = self.origBool();
                    ReturnSetPrimitives.obsByte   = self.origByte();
                    ReturnSetPrimitives.obsShort  = self.origShort();
                    ReturnSetPrimitives.obsInt    = self.origInt();
                    ReturnSetPrimitives.obsIntReadback = self.origInt();
                    ReturnSetPrimitives.obsLong   = self.origLong();
                    ReturnSetPrimitives.obsFloat  = self.origFloat();
                    ReturnSetPrimitives.obsDouble = self.origDouble();
                    ReturnSetPrimitives.obsChar   = self.origChar();

                    // STATIC dispatch — parameters (none) begin at slot 0.
                    ReturnSetPrimitives.obsStaticBool   = origStaticBool();
                    ReturnSetPrimitives.obsStaticByte   = origStaticByte();
                    ReturnSetPrimitives.obsStaticShort  = origStaticShort();
                    ReturnSetPrimitives.obsStaticInt    = origStaticInt();
                    ReturnSetPrimitives.obsStaticLong   = origStaticLong();
                    ReturnSetPrimitives.obsStaticFloat  = origStaticFloat();
                    ReturnSetPrimitives.obsStaticDouble = origStaticDouble();
                    ReturnSetPrimitives.obsStaticChar   = origStaticChar();

                    // Sub-int returns WIDENED to int — a SECOND dispatch of each
                    // byte/short/char method, assigned into an int local so the
                    // JVM's caller-side mask-and-widen is visible at 32 bits
                    // (byte/short sign-extend; char zero-extends).
                    ReturnSetPrimitives.obsByteAsInt        = self.origByte();
                    ReturnSetPrimitives.obsShortAsInt       = self.origShort();
                    ReturnSetPrimitives.obsCharAsInt        = self.origChar();
                    ReturnSetPrimitives.obsStaticByteAsInt  = origStaticByte();
                    ReturnSetPrimitives.obsStaticShortAsInt = origStaticShort();
                    ReturnSetPrimitives.obsStaticCharAsInt  = origStaticChar();
                }
                catch (final Throwable t)
                {
                    threw = true;
                }
                ReturnSetPrimitives.sawException = threw;
                ReturnSetPrimitives.roundCount = ReturnSetPrimitives.roundCount + 1;
                ReturnSetPrimitives.done = true;
            }
        });
    }
}

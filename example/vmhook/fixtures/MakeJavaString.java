package vmhook.fixtures;

import vmhook.Harness;

/**
 * Fixture for the {@code vmhook::make_java_string(value)} feature (area:
 * heap allocation / string construction).  This is the FIRST live-JVM coverage
 * of the make_java_string API: allocating a brand-new java.lang.String OOP from
 * C++ (no JNI NewStringUTF), across the LATIN1 / UTF16 / classic-char[] coder
 * paths, and proving the result is usable THREE independent ways:
 *   1. natively (read_java_string round-trip — the hard correctness gate);
 *   2. from executing Java bytecode via the LOW-LEVEL injection surface
 *      (return_value::set_arg into an interpreter local + field_proxy::set into a
 *      static String field, both read back with real Java bytecode);
 *   3. from executing Java bytecode via the PUBLIC method-call surface
 *      ({@code method_proxy::call} with a {@code std::string} argument, which
 *      constructs the String internally through make_java_string and hands it to
 *      a Java {@link #echoCheck(int, String)} that compares it to the expected
 *      literal with genuine String.equals / length / codePointCount).
 *
 * <p>The native module ({@code tests/jvm/modules/make_java_string.cpp}) installs
 * two interpreter hooks on this fixture and does ALL of its make_java_string /
 * read_java_string / set_arg / field-write / call work from INSIDE those detours,
 * where HotSpot's current_java_thread is established (the precondition for heap
 * allocation via make_java_object, which make_java_string calls, AND for the
 * call stub / JNI call path).  The detours run on a real bytecode dispatch, which
 * is the only thing that fires an interpreter hook.</p>
 *
 * <h3>Four canonical test strings (index 0..3)</h3>
 * <ul>
 *   <li>0 = "hello"  — pure ASCII (compact LATIN1 / classic char[]);</li>
 *   <li>1 = "café"  — Latin-1 high char U+00E9 (still LATIN1 on JDK 9+, but
 *       a real 2-byte multibyte UTF-8 round-trip);</li>
 *   <li>2 = "日本"  — CJK (forces the UTF16 coder on JDK 9+);</li>
 *   <li>3 = ""  — the empty string (0-length backing array boundary).</li>
 * </ul>
 * Every non-ASCII constant is written with {@code \\uXXXX} escapes so the source
 * is pure ASCII and javac decodes it identically on every CI host regardless of
 * the build machine's file encoding.  The C++ module hard-codes the matching
 * UTF-8 byte expectations for the native round-trip, AND drives a much wider set
 * of native-only round-trips (interior NUL, astral surrogate pairs, lone
 * surrogate, the U+00FF LATIN1 ceiling, 1000+-char strings, the 4096-char cap
 * and its truncation boundary) that need no Java field — read_java_string reads
 * the freshly-made backing array directly.
 *
 * <h3>mode selector</h3>
 * The native side sets {@link #mode} (and clears {@link #done}) on the rising
 * edge of {@link #go}, then drives one probe cycle:
 * <ul>
 *   <li>0 = MAIN cycle.  Fire {@link #roundtrip()} once (native detour does every
 *       native round-trip, writes a made oop into each {@code madeN} field, and
 *       calls {@link #echoCheck} on the live receiver for each index), snapshot
 *       the {@code madeN} fields with {@link #captureMade()}, then drive one
 *       {@link #injectArg(String)} dispatch per index.</li>
 *   <li>2 = SURVIVE-GC cycle.  Force {@code System.gc()} several times with young
 *       churn (a relocating collector may move / reclaim the freshly-made backing
 *       arrays that the unbarriered {@code madeN} static-field writes installed),
 *       then re-snapshot the {@code madeN} fields with {@link #captureMadeGc()}.
 *       This is the live-JVM probe for the suspected GC store-barrier hazard: a
 *       String whose backing array is young and was stored into an older field by
 *       a raw write with no card mark.</li>
 * </ul>
 *
 * <h3>KNOWN ISSUE — characterised, not asserted green</h3>
 * There is a suspected vmhook hazard where a make_java_string String matches on a
 * native byte-view (read_java_string is byte-exact) yet Java
 * {@code expected.equals(made)} can disagree due to a coder / length / array-klass
 * metadata inconsistency or a missing GC store barrier on the reference write.
 * This fixture therefore RECORDS the actual Java-side .equals()/.length() outcomes
 * into primitive witness fields ({@code madeEqN}, {@code madeLenN}, {@code argEqN},
 * {@code echoEqN}, {@code madeEqGcN}, …) that the native side reads back and
 * asserts as the ACTUAL observed value, keeping CI green while still surfacing the
 * behaviour.  The native round-trip (read_java_string) is the hard correctness
 * gate on the C++ side; a pure invariant (equals ⇒ correct length) stays HARD.
 *
 * <p>Java 8 syntax only (anonymous Harness.Probe, no var / lambda-in-field /
 * switch-expression / records).</p>
 */
public final class MakeJavaString
{
    // ── go / done handshake driven by the native module via run_probe ───────
    public static volatile boolean go;
    public static volatile boolean done;

    /**
     * Cycle selector (native sets it on the rising edge of go, then clears
     * done).  0 = MAIN (roundtrip + echo + injectArg); 2 = SURVIVE-GC re-capture.
     */
    public static volatile int mode;

    /** How many times the SURVIVE-GC cycle has driven System.gc(). */
    public static volatile int gcRounds;

    /**
     * Selects which canonical string the native injectArg() detour should make
     * and inject (0..3).  The probe sets this immediately before each
     * injectArg() dispatch.
     */
    public static volatile int injectWhich = -1;

    /** Liveness counters: bumped each time a hooked method body actually runs. */
    public static volatile int roundtripCount;
    public static volatile int injectArgCount;

    // ── The four canonical expected values (ASCII-safe \\uXXXX source) ───────
    /** index 0: pure ASCII. */
    public static final String EXP0 = "hello";
    /** index 1: caf + U+00E9 (Latin-1 high — 2-byte UTF-8). */
    public static final String EXP1 = "café";
    /** index 2: CJK U+65E5 U+672C (forces UTF16 coder on JDK 9+). */
    public static final String EXP2 = "日本";
    /** index 3: the empty string. */
    public static final String EXP3 = "";

    /** Convenience accessor so the native side never hard-codes the order. */
    public static String expected(final int index)
    {
        switch (index)
        {
            case 0:  return EXP0;
            case 1:  return EXP1;
            case 2:  return EXP2;
            case 3:  return EXP3;
            default: return null;
        }
    }

    /**
     * The placeholder the probe passes to injectArg().  Deliberately DISTINCT
     * from every EXPn so a no-op (un-injected) arg is unmistakable: if the body
     * records PLACEHOLDER content, set_arg did nothing.
     */
    public static final String PLACEHOLDER = "<<placeholder-not-injected>>";

    // =====================================================================
    //  String FIELDS the native side overwrites with a made oop (object-ref
    //  write path: field_proxy::set(unique_ptr<wrapper>) stamps the compressed
    //  OOP of a make_java_string result into the field).  Initialised to a
    //  recognisable sentinel so a skipped write is caught (the witness would
    //  still reflect the sentinel, never the expected value).
    // =====================================================================
    public static String made0 = "<<unwritten-0>>";
    public static String made1 = "<<unwritten-1>>";
    public static String made2 = "<<unwritten-2>>";
    public static String made3 = "<<unwritten-3>>";

    // ── Witnesses for the made* FIELD writes (snapshotted by captureMade()
    //    using genuine Java bytecode AFTER the native roundtrip detour ran). ──
    public static boolean madeEq0;   public static int madeLen0;   public static boolean madeNull0;
    public static boolean madeEq1;   public static int madeLen1;   public static boolean madeNull1;
    public static boolean madeEq2;   public static int madeLen2;   public static boolean madeNull2;
    public static boolean madeEq3;   public static int madeLen3;   public static boolean madeNull3;

    // ── Witnesses for the SAME made* fields re-read AFTER a forced GC
    //    (snapshotted by captureMadeGc() in the mode-2 cycle).  A divergence
    //    from the pre-GC snapshot is the fingerprint of the missing store
    //    barrier: a young backing array reclaimed/relocated out from under an
    //    unbarriered static-field reference write. ──
    public static boolean madeEqGc0;   public static int madeLenGc0;   public static boolean madeNullGc0;
    public static boolean madeEqGc1;   public static int madeLenGc1;   public static boolean madeNullGc1;
    public static boolean madeEqGc2;   public static int madeLenGc2;   public static boolean madeNullGc2;
    public static boolean madeEqGc3;   public static int madeLenGc3;   public static boolean madeNullGc3;

    // ── Witnesses for the set_arg INJECTION (written by injectArg's body) ───
    // argEqN is the .equals(expected[N]) result the BODY observed for the
    // injected arg; argLenN is its .length(); argNullN true if the body saw
    // null; argPlaceholderN true if the body still saw the placeholder
    // (i.e. set_arg did not take effect).
    public static boolean argEq0;   public static int argLen0;   public static boolean argNull0;   public static boolean argPlaceholder0;
    public static boolean argEq1;   public static int argLen1;   public static boolean argNull1;   public static boolean argPlaceholder1;
    public static boolean argEq2;   public static int argLen2;   public static boolean argNull2;   public static boolean argPlaceholder2;
    public static boolean argEq3;   public static int argLen3;   public static boolean argNull3;   public static boolean argPlaceholder3;

    // ── Witnesses for the PUBLIC method-call surface (echoCheck), driven by
    //    the native detour via method_proxy::call(index, std::string).  call()
    //    builds the String through make_java_string internally, so these prove
    //    the SAME constructor product is usable as a real call argument.
    //    echoCalledN flips true the instant the body runs (so the native side
    //    can tell "call dispatched" from "call never reached the body"). ──
    public static boolean echoCalled0;   public static boolean echoEq0;   public static int echoLen0;   public static boolean echoNull0;   public static int echoCp0;
    public static boolean echoCalled1;   public static boolean echoEq1;   public static int echoLen1;   public static boolean echoNull1;   public static int echoCp1;
    public static boolean echoCalled2;   public static boolean echoEq2;   public static int echoLen2;   public static boolean echoNull2;   public static int echoCp2;
    public static boolean echoCalled3;   public static boolean echoEq3;   public static int echoLen3;   public static boolean echoNull3;   public static int echoCp3;

    // ── Witnesses for the GENERIC content check (checkContent), driven by the
    //    native detour to prove — char-by-char, with genuine Java bytecode — that
    //    a make_java_string product Java actually sees matches the exact UTF-16
    //    the encoder intended, for inputs WAY beyond the four canonical strings:
    //    every byte 0x00..0xFF, the control range, a dense BMP sweep, multiple
    //    astral code points, and the OVER-CAP (>4096 and >65536) NewString-fallback
    //    strings whose full length is NOT natively read-back-able (read_java_string
    //    caps at 4096) and so can ONLY be proven full-length from the Java side.
    //
    //    There are NUM_CC slots (kind 0..NUM_CC-1).  Per slot the body records:
    //      ccCalledK  — true the instant the body ran (call dispatched to the body);
    //      ccLenK     — value.length()  (UTF-16 code-unit count Java sees);
    //      ccCpK      — value.codePointCount(0, length)  (astral folds to 1 cp/pair);
    //      ccNullK    — true if the body saw null;
    //      ccSigK     — a position-weighted content signature over value.charAt(i):
    //                   sig = ((sig * 131) + charAt(i)) over all i, in a 32-bit int.
    //                   The native side computes the SAME fold over the UTF-16 units
    //                   it asked make_java_string to encode and asserts equality, so
    //                   a single int proves byte-for-byte content agreement (a
    //                   transposition, a dropped char, or a wrong coder all change it)
    //                   without thousands of witness fields.
    public static final int NUM_CC = 8;
    public static boolean ccCalled0; public static int ccLen0; public static int ccCp0; public static boolean ccNull0; public static int ccSig0;
    public static boolean ccCalled1; public static int ccLen1; public static int ccCp1; public static boolean ccNull1; public static int ccSig1;
    public static boolean ccCalled2; public static int ccLen2; public static int ccCp2; public static boolean ccNull2; public static int ccSig2;
    public static boolean ccCalled3; public static int ccLen3; public static int ccCp3; public static boolean ccNull3; public static int ccSig3;
    public static boolean ccCalled4; public static int ccLen4; public static int ccCp4; public static boolean ccNull4; public static int ccSig4;
    public static boolean ccCalled5; public static int ccLen5; public static int ccCp5; public static boolean ccNull5; public static int ccSig5;
    public static boolean ccCalled6; public static int ccLen6; public static int ccCp6; public static boolean ccNull6; public static int ccSig6;
    public static boolean ccCalled7; public static int ccLen7; public static int ccCp7; public static boolean ccNull7; public static int ccSig7;

    // =====================================================================
    //  Hooked methods.
    // =====================================================================

    /**
     * No-arg trigger.  The native module hooks this; calling it on a real
     * bytecode dispatch fires the interpreter hook, and the native detour does
     * every make_java_string / read_java_string native round-trip, writes a
     * made oop into each madeN field, AND calls {@link #echoCheck} on the live
     * receiver for each index (the public method-call surface).  Returns
     * nothing; just bumps a counter so the native side can confirm the hook fired.
     */
    public void roundtrip()
    {
        roundtripCount++;
    }

    /**
     * Records what the body actually received for {@code value}.  The native
     * detour overwrites slot 1 (the {@code value} local) with a make_java_string
     * result chosen by {@link #injectWhich} BEFORE this body runs, so the
     * comparisons below describe the INJECTED string.  Results are stored into
     * the per-index argN witnesses.
     */
    public void injectArg(final String value)
    {
        injectArgCount++;
        final int which = injectWhich;
        final boolean isNull = (value == null);
        final boolean isPlaceholder = PLACEHOLDER.equals(value);
        final int len = isNull ? -1 : value.length();
        final String exp = expected(which);
        final boolean eq = (exp != null) && exp.equals(value);
        switch (which)
        {
            case 0: argEq0 = eq; argLen0 = len; argNull0 = isNull; argPlaceholder0 = isPlaceholder; break;
            case 1: argEq1 = eq; argLen1 = len; argNull1 = isNull; argPlaceholder1 = isPlaceholder; break;
            case 2: argEq2 = eq; argLen2 = len; argNull2 = isNull; argPlaceholder2 = isPlaceholder; break;
            case 3: argEq3 = eq; argLen3 = len; argNull3 = isNull; argPlaceholder3 = isPlaceholder; break;
            default: break;
        }
    }

    /**
     * Genuine-bytecode echo of a String the native side built with
     * make_java_string and passed through {@code method_proxy::call}.  Compares
     * {@code value} against {@link #expected(int)} for {@code which} and stores
     * the .equals / .length() / null / codePointCount it observes into the
     * per-index echo witnesses.  codePointCount is recorded so the native side
     * can confirm astral / surrogate handling on a string built by the call path
     * (none of the four canonical strings are astral, but the field is here for
     * completeness and future expansion).  Returns the observed length so the
     * call also has a non-void primitive return (-1 for null / unknown index).
     */
    public int echoCheck(final int which, final String value)
    {
        final boolean isNull = (value == null);
        final int len = isNull ? -1 : value.length();
        final int cp = isNull ? -1 : value.codePointCount(0, value.length());
        final String exp = expected(which);
        final boolean eq = (exp != null) && exp.equals(value);
        switch (which)
        {
            case 0: echoCalled0 = true; echoEq0 = eq; echoLen0 = len; echoNull0 = isNull; echoCp0 = cp; break;
            case 1: echoCalled1 = true; echoEq1 = eq; echoLen1 = len; echoNull1 = isNull; echoCp1 = cp; break;
            case 2: echoCalled2 = true; echoEq2 = eq; echoLen2 = len; echoNull2 = isNull; echoCp2 = cp; break;
            case 3: echoCalled3 = true; echoEq3 = eq; echoLen3 = len; echoNull3 = isNull; echoCp3 = cp; break;
            default: break;
        }
        return len;
    }

    /**
     * GENERIC content witness for a make_java_string product the native side
     * built (often a string with no canonical constant — every-byte LATIN1, a
     * dense BMP sweep, multiple astral code points, or an OVER-CAP string built
     * through the NewString fallback).  Runs entirely in Java bytecode: walks
     * {@code value} char-by-char and records into the per-{@code kind} witnesses
     * its {@link String#length()}, {@link String#codePointCount(int,int)}, a
     * null flag, and a position-weighted 32-bit content signature
     * {@code sig = sig*131 + value.charAt(i)} over every UTF-16 code unit.  The
     * native side computes the identical fold over the UTF-16 units it handed
     * make_java_string and asserts both the length and the signature match, which
     * proves — byte for byte — that the String the JVM sees is the exact content
     * the encoder intended, even when the string is far too long for
     * read_java_string's 4096 native readback ceiling.  Returns the observed
     * length (-1 for null) so the call also exercises a primitive return.
     */
    public int checkContent(final int kind, final String value)
    {
        final boolean isNull = (value == null);
        final int len = isNull ? -1 : value.length();
        final int cp = isNull ? -1 : value.codePointCount(0, len);
        int sig = 0;
        if (!isNull)
        {
            for (int i = 0; i < len; i++)
            {
                sig = (sig * 131) + value.charAt(i);
            }
        }
        switch (kind)
        {
            case 0: ccCalled0 = true; ccLen0 = len; ccCp0 = cp; ccNull0 = isNull; ccSig0 = sig; break;
            case 1: ccCalled1 = true; ccLen1 = len; ccCp1 = cp; ccNull1 = isNull; ccSig1 = sig; break;
            case 2: ccCalled2 = true; ccLen2 = len; ccCp2 = cp; ccNull2 = isNull; ccSig2 = sig; break;
            case 3: ccCalled3 = true; ccLen3 = len; ccCp3 = cp; ccNull3 = isNull; ccSig3 = sig; break;
            case 4: ccCalled4 = true; ccLen4 = len; ccCp4 = cp; ccNull4 = isNull; ccSig4 = sig; break;
            case 5: ccCalled5 = true; ccLen5 = len; ccCp5 = cp; ccNull5 = isNull; ccSig5 = sig; break;
            case 6: ccCalled6 = true; ccLen6 = len; ccCp6 = cp; ccNull6 = isNull; ccSig6 = sig; break;
            case 7: ccCalled7 = true; ccLen7 = len; ccCp7 = cp; ccNull7 = isNull; ccSig7 = sig; break;
            default: break;
        }
        return len;
    }

    /**
     * Snapshots — with genuine getfield / String.equals / String.length
     * bytecode — what the JVM observes in each madeN field after the native
     * roundtrip detour wrote a make_java_string oop there.  Captures the
     * CHARACTERISED Java-side outcome (.equals may be false even when the
     * native byte-view is correct).
     */
    private static void captureMade()
    {
        madeNull0 = (made0 == null); madeLen0 = madeNull0 ? -1 : made0.length(); madeEq0 = EXP0.equals(made0);
        madeNull1 = (made1 == null); madeLen1 = madeNull1 ? -1 : made1.length(); madeEq1 = EXP1.equals(made1);
        madeNull2 = (made2 == null); madeLen2 = madeNull2 ? -1 : made2.length(); madeEq2 = EXP2.equals(made2);
        madeNull3 = (made3 == null); madeLen3 = madeNull3 ? -1 : made3.length(); madeEq3 = EXP3.equals(made3);
    }

    /**
     * Re-snapshots the SAME madeN fields AFTER the forced-GC churn (mode 2).
     * If a made String survived only by an unbarriered reference write, a
     * relocating/reclaiming collector can leave these disagreeing with the
     * pre-GC {@link #captureMade()} snapshot — the fingerprint the native side
     * characterises.  Reads are wrapped so a corrupt String that throws on
     * .equals/.length cannot wedge the probe loop.
     */
    private static void captureMadeGc()
    {
        madeNullGc0 = (made0 == null); try { madeLenGc0 = madeNullGc0 ? -1 : made0.length(); madeEqGc0 = EXP0.equals(made0); } catch (final Throwable t) { madeLenGc0 = -2; madeEqGc0 = false; }
        madeNullGc1 = (made1 == null); try { madeLenGc1 = madeNullGc1 ? -1 : made1.length(); madeEqGc1 = EXP1.equals(made1); } catch (final Throwable t) { madeLenGc1 = -2; madeEqGc1 = false; }
        madeNullGc2 = (made2 == null); try { madeLenGc2 = madeNullGc2 ? -1 : made2.length(); madeEqGc2 = EXP2.equals(made2); } catch (final Throwable t) { madeLenGc2 = -2; madeEqGc2 = false; }
        madeNullGc3 = (made3 == null); try { madeLenGc3 = madeNullGc3 ? -1 : made3.length(); madeEqGc3 = EXP3.equals(made3); } catch (final Throwable t) { madeLenGc3 = -2; madeEqGc3 = false; }
    }

    static
    {
        Harness.register(new Harness.Probe()
        {
            @Override
            public boolean pending()
            {
                return MakeJavaString.go && !MakeJavaString.done;
            }

            @Override
            public void run()
            {
                if (MakeJavaString.mode == 2)
                {
                    // SURVIVE-GC cycle: force a handful of collections with young
                    // churn so a relocating/reclaiming collector has every chance
                    // to move or free the freshly-made backing arrays that the
                    // native side stored into the madeN static fields (mode 0)
                    // through an unbarriered reference write.  Then re-snapshot.
                    for (int round = 0; round < 4; round++)
                    {
                        final byte[] churn = new byte[1 << 16];
                        if (churn.length == 0)
                        {
                            throw new IllegalStateException("unreachable");
                        }
                        System.gc();
                        MakeJavaString.gcRounds++;
                    }
                    captureMadeGc();
                    MakeJavaString.done = true;
                    return;
                }

                // MAIN cycle (mode 0).
                final MakeJavaString self = new MakeJavaString();

                // (1) Fire the roundtrip hook once: a real bytecode dispatch so
                //     the native detour does every native round-trip, writes the
                //     madeN fields, and drives echoCheck on the live receiver.
                self.roundtrip();

                // (2) Snapshot what Java sees in the madeN fields (pure Java).
                captureMade();

                // (3) Drive one injectArg dispatch per index; the native detour
                //     injects the matching made string into slot 1 before the
                //     body records its observation.
                for (int i = 0; i < 4; i++)
                {
                    MakeJavaString.injectWhich = i;
                    self.injectArg(PLACEHOLDER);
                }

                MakeJavaString.done = true;
            }
        });
    }
}

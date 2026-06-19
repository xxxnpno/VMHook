package vmhook.fixtures;

import vmhook.Harness;

/**
 * Fixture for {@code return_value::set_arg(index, <wrapper-or-null>)} — the
 * OBJECT-WRAPPER and NULL-reference argument-injection branches of set_arg
 * (area: return_value / argument mutation).
 *
 * <p>The companion native module ({@code tests/jvm/modules/return_set_wrapper_null.cpp})
 * installs an interpreter hook on each method below.  Inside the hook it calls
 * {@code retval.set_arg(slot, <wrapper-or-null>)} to overwrite a <em>reference</em>
 * argument slot <em>before</em> the original body runs, exercising:</p>
 * <ul>
 *   <li>{@code is_unique_object_ptr} branch — inject a {@code unique_ptr<wrapper>}
 *       built from a live OOP (a published donor or a {@code make_unique}-allocated
 *       object),</li>
 *   <li>the same branch with an <em>empty</em> {@code unique_ptr} — writes a null
 *       OOP (the body must see Java {@code null}),</li>
 *   <li>the {@code object_base}-by-value branch — inject a wrapper passed by value,</li>
 *   <li>cross-type injection — a wrapper of an UNRELATED Java class injected into a
 *       differently-typed slot (no klass check; characterized natively only).</li>
 * </ul>
 *
 * <p><b>CRASH-SAFETY CONTRACT (this is why the methods are split the way they are).</b>
 * A live JDK-21 / Windows reproduction proved that {@code set_arg}'s {@code store_oop}
 * helper corrupts an interpreter local slot whenever the slot being overwritten
 * previously held a value whose bits are {@code <= 0xFFFFFFFF} — most importantly a
 * Java {@code null} (bits {@code == 0}).  In that case it re-encodes the injected
 * object as a 32-bit <em>narrow</em> (compressed) OOP and stores that, but a HotSpot
 * interpreter object slot must hold a full 64-bit <em>uncompressed</em> OOP.  The very
 * next {@code aload}/{@code getfield}/native call against that slot dereferences the
 * 4-byte narrow value as a raw pointer and the JVM dies with an
 * EXCEPTION_ACCESS_VIOLATION (observed faulting on {@code System.identityHashCode}
 * inside {@code takeObject}, caller {@code Harness.tickAll} C2-compiled).  See the
 * native module's REPORT block for the exact {@code vmhook.hpp} patch.</p>
 *
 * <p>Therefore the fixture is split into two method <em>families</em>:</p>
 * <ul>
 *   <li><b>allow-through families</b> ({@code takeObject}, {@code takeObjectStatic},
 *       {@code takeByVal}, {@code takeFresh}, {@code takeTwoObjects},
 *       {@code takeMixedObject}, {@code takeString} object inject, the null-inject
 *       methods): the original argument the probe passes into the to-be-overwritten
 *       slot is a NON-NULL object of the EXACT declared parameter type (a
 *       {@code SENTINEL Donor}, or a real {@code String} for {@code takeString}).
 *       The overwritten slot therefore already holds a wide 64-bit OOP, so
 *       {@code store_oop} takes its safe raw-store branch and writes a wide OOP — the
 *       body can safely dereference the injected, type-matching object (field read +
 *       identity).  Null injection writes a literal {@code null} pointer (always safe).</li>
 *   <li><b>cancel-only families</b> ({@code takeOverNull}, {@code takeWrongType}):
 *       the native side injects over a genuinely-{@code null} original slot (the
 *       bug-triggering case) or injects a wrong-klass oop, then {@code cancel()}s the
 *       method so its body NEVER runs.  The native module characterizes the slot with
 *       raw, {@code is_valid_pointer}-gated reads — it never lets Java dereference a
 *       possibly-narrow / wrong-typed slot.  The bodies below still publish a
 *       null-flag so a (hypothetical) accidental allow-through is observable, but they
 *       perform NO field read / method call / identity on the injected reference.</li>
 * </ul>
 *
 * <p>Slot model (HotSpot x64 interpreter; every local is an 8-byte slot):
 * instance method slot 0 = {@code this}, slot 1 = first arg, slot 2 = second arg;
 * static method slot 0 = first arg; {@code takeMixedObject(int, Donor)}: this=0,
 * n=1, x=2 (the object follows a primitive — proves the slot index, not the
 * argument ordinal, is what set_arg targets).</p>
 *
 * <p>The whole suite runs in a single {@code go}/{@code done} handshake: the probe's
 * {@code run()} calls every method once (one real bytecode dispatch each, the only
 * thing that makes an interpreter hook fire), then raises {@code done}.  The native
 * module installs all of its scoped hooks up front and fires the probe once; it then
 * re-fires for the {@code takeString} null scenario.</p>
 *
 * <p>Target Java 8 syntax only (no var / records / switch-expressions).</p>
 */
public final class ReturnSetWrapperNull
{
    /** Native raises this to request the probe action; lowers it afterwards. */
    public static volatile boolean go;

    /** The probe action raises this when it has run; native polls it. */
    public static volatile boolean done;

    /** Selects which probe variant run() executes (round 1 vs the string round). */
    public static volatile int mode;

    // A singleton instance the probe uses for the instance-method dispatches.
    public static final ReturnSetWrapperNull instance = new ReturnSetWrapperNull();

    // ── The reference type the wrappers walk ──────────────────────────────────
    /**
     * Small standalone reference type the native side wraps and injects.  Has a
     * primitive field (tag) and a method (getTag) so the native side can prove a
     * wrapper-injected argument is the real, dispatch-capable object it expects
     * (field read + identity), not a truncated/garbage pointer.
     */
    public static final class Donor
    {
        public int tag;

        public Donor(final int tag)
        {
            this.tag = tag;
        }

        public int getTag()
        {
            return this.tag;
        }

        // ── RECEIVER-SWAP probe ───────────────────────────────────────────────
        // An instance method on Donor: slot 0 is the Donor receiver (`this`).
        // The native side hooks this and injects a DIFFERENT Donor over slot 0,
        // so the body publishes the REPLACEMENT receiver's tag/identity — proving
        // set_arg(0, wrapper) swaps the receiver of an instance method, not just a
        // parameter.  The original receiver passed by the probe has RECV_ORIG_TAG;
        // a successful swap makes recvTag == RECV_TAG instead.
        public void publishReceiver()
        {
            ReturnSetWrapperNull.recvWasNull  = false;
            ReturnSetWrapperNull.recvTag      = this.tag;
            ReturnSetWrapperNull.recvIdentity = System.identityHashCode(this);
        }
    }

    /**
     * An UNRELATED reference type, used for the cross-type-injection
     * characterization: a Decoy wrapper injected into a Donor-typed slot is
     * silently accepted by set_arg (no klass-match check).  Distinct field layout
     * from Donor so a mis-typed read would be observably different — but the native
     * side NEVER lets Java read it AS a Donor (that getfield would be UB); the
     * cross-type acceptance is characterized natively (identity / oop compare only).
     */
    public static final class Decoy
    {
        public int poison;
        public long extra;

        public Decoy(final int poison)
        {
            this.poison = poison;
            this.extra = 0xABCDEF0123456789L;
        }
    }

    // ── Deterministic constants the native side mirrors ───────────────────────
    /** tag of the published DONOR injected through the unique_ptr branch. */
    public static final int DONOR_TAG = 0x0D04;          // 3332

    /** tag of the published DONOR injected through the by-value branch. */
    public static final int BYVAL_DONOR_TAG = 0x0BABE;   // 48318

    /** tag the native make_unique-allocated Donor is constructed with. */
    public static final int FRESH_DONOR_TAG = 0x7E57;    // 32343

    /** poison value of the published DECOY used for cross-type injection. */
    public static final int DECOY_POISON = 0x6A11;       // 27153

    /** tag of the SENTINEL Donor the probe passes as the original (overwritten) arg. */
    public static final int SENTINEL_TAG = 0x5E11;       // 24081

    /** content of the published donor String injected into takeString. */
    public static final String STRING_DONOR = "injected-object-string";

    /** tag of a SECOND published donor (distinct from DONOR) for multi/double inject. */
    public static final int DONOR2_TAG = 0x0D2D2;        // 53970

    /** tag of the make_unique fresh donor injected onto a STATIC slot. */
    public static final int FRESH_STATIC_TAG = 0x57A7;   // 22439

    /** tag of the replacement RECEIVER donor injected over slot 0 (receiver swap). */
    public static final int RECV_TAG = 0x4EC0;           // 20160

    /** tag of the ORIGINAL receiver donor the probe builds for the receiver-swap method. */
    public static final int RECV_ORIG_TAG = 0x015A1;     // 5537

    // ── Published donor objects (stable identities the native side reads) ─────
    /** Donor the native side reads into a unique_ptr<wrapper> and injects. */
    public static final Donor DONOR = new Donor(DONOR_TAG);

    /** Donor injected through the object_base-by-value set_arg branch. */
    public static final Donor BYVAL_DONOR = new Donor(BYVAL_DONOR_TAG);

    /** Decoy injected (wrong type) into a Donor-typed slot. */
    public static final Decoy DECOY = new Decoy(DECOY_POISON);

    /** A String donor the native side injects into takeString as a real object. */
    public static final String STRING_DONOR_REF = STRING_DONOR;

    /**
     * A NON-NULL sentinel Donor passed as the ORIGINAL argument into every
     * allow-through slot, so the overwritten slot already holds a wide OOP and
     * store_oop writes the injected object's wide OOP (its safe raw-store branch).
     * Its tag (SENTINEL_TAG) is distinct from every injected tag, so a no-op hook
     * leaves a recognisably different observation than a successful injection.
     */
    public static final Donor SENTINEL = new Donor(SENTINEL_TAG);

    /** A SECOND donor for double-injection (last-write-wins) and both-slots inject. */
    public static final Donor DONOR2 = new Donor(DONOR2_TAG);

    /** The replacement receiver injected over an instance method's slot 0. */
    public static final Donor RECV_DONOR = new Donor(RECV_TAG);

    /** identityHashCode of DONOR, published so native checks identity exactly. */
    public static volatile int donorIdentity;
    /** identityHashCode of BYVAL_DONOR. */
    public static volatile int byvalDonorIdentity;
    /** identityHashCode of DECOY. */
    public static volatile int decoyIdentity;
    /** identityHashCode of DONOR2. */
    public static volatile int donor2Identity;
    /** identityHashCode of RECV_DONOR (the replacement receiver). */
    public static volatile int recvDonorIdentity;

    // ─────────────────────────────────────────────────────────────────────────
    // ALLOW-THROUGH FAMILY — original arg is a non-null SENTINEL Donor (wide OOP
    // in the slot), so the injected object's wide OOP is what store_oop writes and
    // the body can safely read it (field + identity).  These cover the "body
    // observes the injected, type-matching object" promise.
    // ─────────────────────────────────────────────────────────────────────────

    // ── takeObject(Donor): instance, slot 1.  unique_ptr injection. ───────────
    public static volatile boolean objWasNull   = true;
    public static volatile int     objIdentity  = 0;
    public static volatile int     objTag       = -1;
    public void takeObject(final Donor value)
    {
        objWasNull  = (value == null);
        objIdentity = (value == null) ? 0 : System.identityHashCode(value);
        objTag      = (value == null) ? -1 : value.getTag();
    }

    // ── takeObjectStatic(Donor): static, slot 0.  unique_ptr injection. ───────
    public static volatile boolean sObjWasNull  = true;
    public static volatile int     sObjIdentity = 0;
    public static volatile int     sObjTag      = -1;
    public static void takeObjectStatic(final Donor value)
    {
        sObjWasNull  = (value == null);
        sObjIdentity = (value == null) ? 0 : System.identityHashCode(value);
        sObjTag      = (value == null) ? -1 : value.getTag();
    }

    // ── takeByVal(Donor): instance, slot 1.  object_base-by-value branch. ─────
    public static volatile boolean byvalWasNull  = true;
    public static volatile int     byvalIdentity = 0;
    public static volatile int     byvalTag      = -1;
    public void takeByVal(final Donor value)
    {
        byvalWasNull  = (value == null);
        byvalIdentity = (value == null) ? 0 : System.identityHashCode(value);
        byvalTag      = (value == null) ? -1 : value.getTag();
    }

    // ── takeFresh(Donor): instance, slot 1.  make_unique-allocated injection. ─
    public static volatile boolean freshWasNull = true;
    public static volatile int     freshTag     = -1;
    public void takeFresh(final Donor value)
    {
        freshWasNull = (value == null);
        freshTag     = (value == null) ? -1 : value.getTag();
    }

    // ── takeTwoObjects(Donor a, Donor b): instance, slots 1 and 2. ────────────
    // Native injects the published DONOR at slot 2 (b) only; a (a non-null Donor)
    // must survive as the original argument the probe passed.  Proves object
    // injection targets the requested slot among consecutive single-slot refs.
    public static volatile int twoFirstTag  = -1;   // a's tag (untouched original)
    public static volatile int twoSecondTag = -1;   // b's tag (injected DONOR)
    public static volatile boolean twoFirstWasNull  = true;
    public static volatile boolean twoSecondWasNull = true;
    public void takeTwoObjects(final Donor a, final Donor b)
    {
        twoFirstWasNull  = (a == null);
        twoSecondWasNull = (b == null);
        twoFirstTag  = (a == null) ? -1 : a.getTag();
        twoSecondTag = (b == null) ? -1 : b.getTag();
    }

    // ── takeMixedObject(int n, Donor x): instance.  this=0, n=1, x=2. ─────────
    // An object arg that FOLLOWS a primitive int.  Native injects the DONOR at
    // slot 2 (x) and leaves n (slot 1) alone — proving the slot index, not the
    // argument ordinal, is what set_arg targets.
    public static volatile int     mixedN       = -1;   // n's value (untouched)
    public static volatile int     mixedObjTag  = -1;   // x's tag (injected DONOR)
    public static volatile boolean mixedObjWasNull = true;
    public void takeMixedObject(final int n, final Donor x)
    {
        mixedN          = n;
        mixedObjWasNull = (x == null);
        mixedObjTag     = (x == null) ? -1 : x.getTag();
    }

    // ── takeString(String): instance, slot 1.  real Java String object inject. ─
    // Two scenarios reuse this single method across re-fired probes (the native
    // side resets `done` between them): (1) inject a published String object over
    // the non-null "before" String the probe passes, (2) inject explicit null.
    // The original arg is the non-null "before" String, so the overwritten slot is
    // already a wide OOP (store_oop raw-store branch) — safe.
    public static volatile boolean strWasNull = true;
    public static volatile int     strLen     = -2;
    public static volatile String  strSeen    = "<unset>";
    public void takeString(final String value)
    {
        strWasNull = (value == null);
        strLen     = (value == null) ? -1 : value.length();
        strSeen    = value;
    }

    // ── takeObjectNull(Donor): instance, slot 1.  empty-unique_ptr -> null. ───
    // Null injection writes a literal null pointer (always safe), so the body may
    // observe Java null directly.  The probe passes DONOR; the injection overwrites
    // it with null.
    public static volatile boolean nullObjWasNull = false;   // body must flip true
    public static volatile int     nullObjTag     = -2;
    public void takeObjectNull(final Donor value)
    {
        nullObjWasNull = (value == null);
        nullObjTag     = (value == null) ? -1 : value.getTag();
    }

    // ── takeObjectNullStatic(Donor): static, slot 0.  null injection. ─────────
    public static volatile boolean sNullObjWasNull = false;
    public static void takeObjectNullStatic(final Donor value)
    {
        sNullObjWasNull = (value == null);
    }

    // ── takeNullOriginal(Donor): instance, slot 1.  OBJECT over a NULL slot. ──
    // POST-FIX scenario: store_oop now ALWAYS writes a wide 64-bit oop (the old
    // narrow-over-null corruption is gone — vmhook.hpp ~9895-9912).  The probe
    // passes null, so set_arg overwrites a genuinely-null slot with the DONOR's
    // wide oop; the body can now SAFELY dereference it (field read + identity).
    // This is the allow-through twin of the (still cancel-only, native-read)
    // takeOverNull characterization, proving the fix landed end-to-end.
    public static volatile boolean nullOrigWasNull = true;
    public static volatile int     nullOrigTag     = -1;
    public static volatile int     nullOrigIdentity = 0;
    public void takeNullOriginal(final Donor value)
    {
        nullOrigWasNull  = (value == null);
        nullOrigTag      = (value == null) ? -1 : value.getTag();
        nullOrigIdentity = (value == null) ? 0 : System.identityHashCode(value);
    }

    // ── takeReplace(Donor): instance, slot 1.  DOUBLE inject (last wins). ─────
    // Native calls set_arg(1, DONOR) then set_arg(1, DONOR2) in the same detour.
    // The body must observe DONOR2 (the last write), proving repeated set_arg on
    // one slot is last-write-wins, not first-write-sticky.
    public static volatile boolean replaceWasNull = true;
    public static volatile int     replaceTag     = -1;
    public void takeReplace(final Donor value)
    {
        replaceWasNull = (value == null);
        replaceTag     = (value == null) ? -1 : value.getTag();
    }

    // ── takeThenNull(Donor): instance, slot 1.  object THEN empty uptr. ───────
    // Native injects DONOR then an empty unique_ptr into the same slot; the body
    // must observe Java null (the null write supersedes the object write).
    public static volatile boolean thenNullWasNull = false;
    public void takeThenNull(final Donor value)
    {
        thenNullWasNull = (value == null);
    }

    // ── takeTwoFirst(Donor a, Donor b): instance.  inject slot 1, slot 2 lives. ─
    // Mirror of takeTwoObjects but the injection targets the FIRST ref slot (1);
    // the SECOND (slot 2, b) must survive as the original the probe passed.
    public static volatile int     twoFirstOnlyA = -1;   // injected DONOR's tag
    public static volatile int     twoFirstOnlyB = -1;   // b's surviving tag (22)
    public static volatile boolean twoFirstOnlyAWasNull = true;
    public static volatile boolean twoFirstOnlyBWasNull = true;
    public void takeTwoFirst(final Donor a, final Donor b)
    {
        twoFirstOnlyAWasNull = (a == null);
        twoFirstOnlyBWasNull = (b == null);
        twoFirstOnlyA = (a == null) ? -1 : a.getTag();
        twoFirstOnlyB = (b == null) ? -1 : b.getTag();
    }

    // ── takeTwoBoth(Donor a, Donor b): instance.  inject BOTH slots, one detour. ─
    // Native injects DONOR into slot 1 and DONOR2 into slot 2 in a single detour;
    // the body must observe DONOR in a and DONOR2 in b, proving multiple object
    // slots can be rewritten in one dispatch and each lands in its own slot.
    public static volatile int     bothA = -1;
    public static volatile int     bothB = -1;
    public static volatile boolean bothAWasNull = true;
    public static volatile boolean bothBWasNull = true;
    public void takeTwoBoth(final Donor a, final Donor b)
    {
        bothAWasNull = (a == null);
        bothBWasNull = (b == null);
        bothA = (a == null) ? -1 : a.getTag();
        bothB = (b == null) ? -1 : b.getTag();
    }

    // ── takeMixedNull(int n, Donor x): instance.  this=0, n=1, x=2. null@2. ───
    // Null injection into the object slot that FOLLOWS a primitive; n (slot 1)
    // must survive.  Proves null injection targets the slot index past a primitive.
    public static volatile int     mixedNullN       = -1;
    public static volatile boolean mixedNullXWasNull = false;
    public void takeMixedNull(final int n, final Donor x)
    {
        mixedNullN        = n;
        mixedNullXWasNull = (x == null);
    }

    // ── takeFreshStatic(Donor): static, slot 0.  make_unique on a STATIC slot. ─
    // A natively-allocated (make_unique) object injected onto a static method's
    // slot 0 — fresh-object injection on the static branch (the instance fresh
    // case is takeFresh).  Probe passes a non-null SENTINEL so the slot is wide.
    public static volatile boolean freshStaticWasNull = true;
    public static volatile int     freshStaticTag     = -1;
    public static void takeFreshStatic(final Donor value)
    {
        freshStaticWasNull = (value == null);
        freshStaticTag     = (value == null) ? -1 : value.getTag();
    }

    // ── takeByValString(String): instance, slot 1.  object_base BY VALUE. ─────
    // Exercises the object_base-by-value set_arg branch with a String wrapper
    // (a NON-Donor wrapper type) — the by-value branch is type-agnostic.  Native
    // injects the published String donor by value over the non-null "before".
    public static volatile boolean byvalStrWasNull = true;
    public static volatile int     byvalStrLen     = -2;
    public static volatile String  byvalStrSeen    = "<unset>";
    public void takeByValString(final String value)
    {
        byvalStrWasNull = (value == null);
        byvalStrLen     = (value == null) ? -1 : value.length();
        byvalStrSeen    = value;
    }

    // ── takeByValNull(Donor): instance, slot 1.  by-value branch, NULL instance. ─
    // The object_base-by-value branch calls store_oop(value.get_instance()); a
    // wrapper built over a null oop injects Java null.  Probe passes DONOR; the
    // injection overwrites it with null (literal-null write is always safe).
    public static volatile boolean byvalNullWasNull = false;
    public void takeByValNull(final Donor value)
    {
        byvalNullWasNull = (value == null);
    }

    // ── Receiver-swap witnesses (published by Donor.publishReceiver above). ────
    // Slot 0 of an instance Donor method is the receiver; the native side swaps
    // it for RECV_DONOR, so recvTag flips from RECV_ORIG_TAG to RECV_TAG.
    public static volatile boolean recvWasNull  = true;
    public static volatile int     recvTag      = -1;
    public static volatile int     recvIdentity = 0;

    // ─────────────────────────────────────────────────────────────────────────
    // CANCEL-ONLY FAMILY — the body must NEVER dereference the injected reference.
    // The native side cancel()s these so the bodies below DO NOT RUN at all; they
    // only publish a null-flag (no field read / method call / identity on `value`)
    // so that an accidental allow-through regression is still observable instead of
    // an outright crash.  The injected-slot state is characterized NATIVELY.
    // ─────────────────────────────────────────────────────────────────────────

    // ── takeOverNull(Donor): instance, slot 1.  THE BUG-TRIGGERING CASE. ──────
    // The probe passes null, so set_arg overwrites a null slot: store_oop encodes
    // the injected DONOR as a NARROW oop and stores it (the corruption).  If this
    // body ran and touched `value`, the JVM would AV.  The native side therefore
    // cancel()s it and reads the slot raw (gated) to characterize the injection.
    public static volatile boolean overNullBodyRan = false;
    public void takeOverNull(final Donor value)
    {
        // Reaching here at all is a cancel-failure; publish it WITHOUT touching
        // `value` (no deref — a narrow-oop slot would crash on any use).
        overNullBodyRan = true;
    }

    // ── takeWrongType(Donor): instance, slot 1.  cross-type characterization. ─
    // Native injects a DECOY (unrelated class) into this Donor-typed slot, then
    // cancel()s the method.  A `getfield Donor.tag` / `invokevirtual Donor.getTag`
    // on a Decoy oop is UNDEFINED behaviour, so the body never runs; the cross-type
    // acceptance is characterized natively (oop identity compare only).
    public static volatile boolean wrongBodyRan = false;
    public void takeWrongType(final Donor value)
    {
        wrongBodyRan = true;   // cancel-failure witness; never touches `value`.
    }

    // ── takeWrongByVal(Donor): instance, slot 1.  cross-type via BY-VALUE branch. ─
    // The Decoy is injected through the object_base-by-value set_arg branch (not the
    // unique_ptr branch takeWrongType uses).  Same no-klass-check acceptance; the
    // native side characterizes the slot and cancel()s — body never derefs.
    public static volatile boolean wrongByValBodyRan = false;
    public void takeWrongByVal(final Donor value)
    {
        wrongByValBodyRan = true;   // cancel-failure witness; never touches `value`.
    }

    // ── takeOobObject(Donor): instance, slot 1.  out-of-range index rejection. ─
    // Native calls set_arg with NEGATIVE and ABOVE-max-locals indices (both must
    // return false and leave the slot untouched), then allows the body through to
    // PROVE the original SENTINEL survived — no wild write corrupted the slot.
    public static volatile boolean oobWasNull = true;
    public static volatile int     oobTag     = -1;
    public void takeOobObject(final Donor value)
    {
        oobWasNull = (value == null);
        oobTag     = (value == null) ? -1 : value.getTag();
    }

    // Tiny no-arg dispatch so even a fully-failed feature still flips probeTicks
    // (lets the native side tell "probe ran" apart from "observation failed").
    public static volatile int probeTicks = 0;
    public void tick() { probeTicks++; }

    static
    {
        Harness.register(new Harness.Probe()
        {
            @Override
            public boolean pending()
            {
                return ReturnSetWrapperNull.go && !ReturnSetWrapperNull.done;
            }

            @Override
            public void run()
            {
                final ReturnSetWrapperNull self = ReturnSetWrapperNull.instance;

                // Publish donor identities the native side cross-checks.
                ReturnSetWrapperNull.donorIdentity      = System.identityHashCode(DONOR);
                ReturnSetWrapperNull.byvalDonorIdentity = System.identityHashCode(BYVAL_DONOR);
                ReturnSetWrapperNull.decoyIdentity      = System.identityHashCode(DECOY);
                ReturnSetWrapperNull.donor2Identity     = System.identityHashCode(DONOR2);
                ReturnSetWrapperNull.recvDonorIdentity  = System.identityHashCode(RECV_DONOR);

                self.tick();

                if (ReturnSetWrapperNull.mode == 1)
                {
                    // ── ROUND 2: takeString NULL injection only. ──────────────
                    // The native side armed a single hook that injects null into
                    // the String slot; pass a non-null "before" so the body's
                    // null observation can only come from the injection.
                    self.takeString("before");
                    ReturnSetWrapperNull.done = true;
                    return;
                }

                // ── ROUND 1: every object/null injection. ─────────────────────
                // One real bytecode dispatch per method -> the matching hook fires
                // and injects the reference BEFORE the body publishes.
                //
                // ALLOW-THROUGH family: original arg is the non-null SENTINEL Donor
                // (wide OOP in the slot) so the injection is crash-safe to read.
                self.takeObject(SENTINEL);                          // -> DONOR (unique_ptr, slot 1)
                ReturnSetWrapperNull.takeObjectStatic(SENTINEL);    // -> DONOR (slot 0)
                self.takeByVal(SENTINEL);                           // -> BYVAL_DONOR (by value)
                self.takeFresh(SENTINEL);                           // -> make_unique Donor
                self.takeTwoObjects(new Donor(11), SENTINEL);       // -> DONOR into slot 2
                self.takeMixedObject(4242, SENTINEL);               // -> DONOR into slot 2
                self.takeString("before");                          // -> String donor (slot 1)

                // NULL injection (writes literal null — safe): probe passes DONOR,
                // injection overwrites with null, body observes Java null.
                self.takeObjectNull(DONOR);                         // -> null (empty uptr, slot 1)
                ReturnSetWrapperNull.takeObjectNullStatic(DONOR);   // -> null (slot 0)

                // POST-FIX allow-through additions (store_oop now always wide):
                self.takeNullOriginal(null);                        // -> DONOR over a NULL slot, body reads it
                self.takeReplace(SENTINEL);                         // -> DONOR then DONOR2 (last wins)
                self.takeThenNull(SENTINEL);                        // -> DONOR then null (null wins)
                self.takeTwoFirst(SENTINEL, new Donor(22));         // -> DONOR into slot 1; slot 2 (22) lives
                self.takeTwoBoth(SENTINEL, SENTINEL);               // -> DONOR@1 + DONOR2@2 in one detour
                self.takeMixedNull(7373, DONOR);                    // -> null into slot 2; n (slot 1) lives
                ReturnSetWrapperNull.takeFreshStatic(SENTINEL);     // -> make_unique Donor (static slot 0)
                self.takeByValString("before");                    // -> String donor BY VALUE (object_base)
                self.takeByValNull(DONOR);                          // -> null via by-value (null instance)
                self.takeOobObject(SENTINEL);                       // -> out-of-range rejected; SENTINEL lives

                // Receiver swap: slot 0 of an instance Donor method is `this`.
                new Donor(RECV_ORIG_TAG).publishReceiver();         // -> receiver swapped to RECV_DONOR

                // CANCEL-ONLY family: native injects then cancel()s, so the body
                // never derefs.  Probe passes null (the bug-triggering original
                // for takeOverNull) / SENTINEL (irrelevant — cancelled either way).
                self.takeOverNull(null);                            // -> DONOR over a NULL slot, cancelled
                self.takeWrongType(SENTINEL);                       // -> DECOY (wrong type), cancelled
                self.takeWrongByVal(SENTINEL);                      // -> DECOY by value (wrong type), cancelled

                ReturnSetWrapperNull.done = true;
            }
        });
    }
}

package vmhook.fixtures;

import vmhook.Harness;

/**
 * Fixture for the field_object_ref feature (area: fields).
 *
 * Exercises the ONE promise of the object-reference field path:
 *
 *     std::unique_ptr&lt;wrapper&gt; obj = holder-&gt;get_field("ref")-&gt;get();
 *
 * i.e. field_proxy::get() on a field whose JVM descriptor starts with 'L'
 * yields a compressed OOP that value_t::cast_for_variant decodes into a
 * std::unique_ptr&lt;wrapper&gt;.  Unlike the method-return twin (method_proxy::call,
 * which truncates/free's a JNI handle on JDK 21+), the FIELD path reads a real
 * compressed OOP straight from the object slot, so the "non-null ref -&gt; usable
 * wrapper" contract holds on EVERY JDK.  That makes this fixture the canonical,
 * JDK-independent proof of the decode pipeline.
 *
 * The native module asserts, on a live JVM, the WHOLE object-reference input
 * space.  Every reference shape a field can hold is represented here:
 *
 *   - NON-NULL instance ref field   -&gt; usable wrapper (read its int / String /
 *     nested-ref fields, AND call a method through it),
 *   - NON-NULL static ref field     -&gt; usable wrapper (the mirror+offset path),
 *   - NULL ref field (instance+static) -&gt; null unique_ptr (the most important
 *     invariant: a null slot must never fabricate a wrapper),
 *   - FINAL object field            -&gt; decodes identically to a non-final one,
 *   - VOLATILE object field         -&gt; decodes correctly (plain slot read),
 *   - SELF ref (a field holding `this`) -&gt; decoded instance == the receiver,
 *   - OTHER-INSTANCE ref            -&gt; a field holding a DIFFERENT instance of
 *     this same class decodes to a distinct, independently-usable wrapper,
 *   - STRING ref                    -&gt; a java.lang.String-typed field decodes
 *     (its slot holds a real String OOP; read back as std::string),
 *   - BOXED ref                     -&gt; a java.lang.Integer-typed field decodes
 *     to a usable wrapper (intValue() dispatched through it),
 *   - INTERFACE-typed field         -&gt; declared an interface, holds a concrete
 *     impl; the decoded oop's RUNTIME klass is the impl and a method dispatches,
 *   - OBJECT-typed field            -&gt; declared java.lang.Object, holds various
 *     runtime types (a Ref, a String); the decoded oop's runtime klass is the
 *     concrete type and (for the Ref case) it reads back as a usable Ref,
 *   - IDENTITY: the decoded OOP, re-encoded, round-trips; the field read and a
 *     direct decode_oop of the same slot agree; and two DIFFERENTLY-DECLARED
 *     fields holding the SAME object (ref / objAsRef) decode to the SAME oop,
 *   - SHARED ref: two reference-typed fields that hold the SAME object decode to
 *     the SAME heap address,
 *   - WRONG-WRAPPER-TYPE read (flaw A): reading a Ref-typed field through a
 *     wrapper registered for an UNRELATED class (Decoy) is NOT rejected — the
 *     fixture lays Decoy out so its offsets differ, proving the silent
 *     mis-decode,
 *   - ARRAY-vs-object (flaw B, now fixed): a '[' (Ref[]) field decoded as a
 *     single unique_ptr is rejected; the array is walked element-wise instead,
 *   - PRIMITIVE get_compressed_oop (flaw C, now fixed): get_compressed_oop() on
 *     an 'I' field is guarded and returns 0.
 *
 * Every object the native side inspects carries deterministic field values AND
 * its System.identityHashCode, so the C++ checks are exact (never "non-null and
 * hope").  Canonical go/done handshake; `mode` selector + done-reset let one
 * probe cycle drive exactly the dispatch the module asserts on.
 */
public final class FieldObjectRef
{
    /** Native sets this true to request the action; clears it after. */
    public static volatile boolean go;

    /** The action sets this true when it has run; native polls it. */
    public static volatile boolean done;

    /**
     * Selects which scenario run() executes.  The native module sets this
     * BEFORE raising `go`.  Only one trivial scenario is needed because every
     * object-reference read is side-effect free and happens from native code;
     * the probe exists solely to fire the interpreter hook on tick() and to
     * (re)publish identities on the Java thread.
     *   0 = wire self/nested/other + publish identities + call tick() once
     *       (drives the native interpreter hook).
     */
    public static volatile int mode;

    // ── A tiny interface + impl for the interface-typed-field angle ─────────
    /**
     * Declared (static) type of the `tag` field.  Its runtime value is a
     * TagImpl, so the native side proves the decoded oop's RUNTIME klass is the
     * concrete impl (never the interface) and that a method dispatches through a
     * concrete wrapper.
     */
    public interface Tag
    {
        int tagValue();
    }

    /** Concrete implementation behind the interface-typed `tag` field. */
    public static final class TagImpl implements Tag
    {
        public int slot;

        public TagImpl(final int slot)
        {
            this.slot = slot;
        }

        @Override
        public int tagValue()
        {
            return this.slot;
        }
    }

    /**
     * A SUBCLASS of Ref used for the polymorphic / inherited angle.  A field
     * declared Ref but holding a SubRef proves the base-typed wrapper reads the
     * inherited int slot AND that the OVERRIDDEN virtual compute() dispatches to
     * SubRef's body through a base-typed (Ref) wrapper.  Its runtime klass is
     * SubRef even when read through a Ref wrapper (IS-A: SubRef's super chain
     * contains Ref, so the klass-match guard accepts the read).
     */
    public static final class SubRef extends Ref
    {
        public int extra;

        public SubRef(final int val, final String label, final int extra)
        {
            super(val, label);
            this.extra = extra;
        }

        /** Overrides Ref.compute() so virtual dispatch through a Ref wrapper
         *  lands HERE, not in Ref. */
        @Override
        public int compute()
        {
            return this.val * 3 + this.extra;
        }
    }

    // ── The reference type the wrappers walk ───────────────────────────────
    /**
     * The object a Holder field points at.  Has a primitive int (val), a String
     * (label), a nested self-reference capability, and a method (compute) so the
     * native side can prove a field-decoded wrapper is fully usable: read a
     * primitive, read a String, AND dispatch a real virtual call through it.
     */
    public static class Ref
    {
        public int val;
        public String label;
        public Ref next;        // nested object reference (a Ref-typed field on a Ref)

        public Ref(final int val, final String label)
        {
            this.val = val;
            this.label = label;
            this.next = null;
        }

        /** Method dispatched through a field-decoded wrapper (usability proof). */
        public int compute()
        {
            return this.val * 2 + 1;
        }
    }

    /**
     * An UNRELATED reference type used for the wrong-wrapper-type angle.  Its
     * field layout deliberately differs from Ref: the first declared int sits at
     * a name ("poison") that Ref does not have, and its value is distinctive.
     * When native reads a Ref-typed slot through a Decoy wrapper, the library
     * does NOT reject it (no klass-match check), and Decoy.get_field("poison")
     * reads at refOop + Decoy's poison-offset — i.e. garbage relative to Ref —
     * which the module shows differs from the correct Ref read.
     */
    public static final class Decoy
    {
        public long pad0;       // shift subsequent fields so offsets differ from Ref
        public long pad1;
        public int poison;      // a field name Ref does NOT declare
        public int poison2;

        public Decoy()
        {
            this.pad0 = 0x1111_1111_1111_1111L;
            this.pad1 = 0x2222_2222_2222_2222L;
            this.poison = 0xDEAD;
            this.poison2 = 0xBEEF;
        }
    }

    // ── Deterministic constants the native side mirrors ────────────────────
    public static final int    REF_VAL          = 0x0BADF00D >>> 8;   // 0x000BADF0 -> positive
    public static final String REF_LABEL        = "ref-of-field";
    public static final int    STATIC_REF_VAL   = 0x5151;
    public static final String STATIC_REF_LABEL = "static-ref";
    public static final int    NESTED_REF_VAL   = 0x2222;
    public static final String NESTED_REF_LABEL = "nested-ref";
    public static final int    FINAL_REF_VAL    = 0x3333;
    public static final int    VOLATILE_REF_VAL = 0x4444;
    public static final int    ARRAY_ELEM0_VAL  = 700;
    public static final int    ARRAY_ELEM1_VAL  = 800;
    public static final int    ARRAY_LEN        = 2;
    public static final int    OTHER_REF_VAL    = 0x6363;   // the `other` instance's ref.val
    public static final String STR_REF_VALUE    = "string-ref-field";
    public static final int    BOXED_INT_VALUE  = 0x07E5;   // 2021
    public static final int    TAG_SLOT_VALUE   = 0x0539;   // 1337
    public static final int    POLY_REF_VAL     = 0x7070;   // SubRef.val
    public static final int    POLY_REF_EXTRA   = 0x000A;   // SubRef.extra
    public static final String POLY_REF_LABEL   = "poly-ref";
    public static final int    WRITABLE_REF_VAL = 0x1357;   // writableRef seed value
    public static final int    SET_TARGET_VAL   = 0x2468;   // the ref the SET test writes
    public static final String WRITABLE_STR_SEED = "writable-seed";
    public static final String SET_STR_VALUE     = "set-via-native";

    // ── Instance reference fields (read through an INSTANCE field_proxy) ────

    /** Non-null instance object reference: the primary usable-wrapper target. */
    public Ref ref = makeRef(REF_VAL, REF_LABEL);

    /** A second field pointing at the SAME object as `ref` (shared-ref angle). */
    public Ref refAlias = this.ref;

    /** Null object reference: must decode to a null unique_ptr. */
    public Ref nullRef = null;

    /** FINAL object reference: final is compile-time; the slot is a plain oop. */
    public final Ref finalRef = makeRef(FINAL_REF_VAL, "final-ref");

    /** VOLATILE object reference: a plain slot read must still decode correctly. */
    public volatile Ref volatileRef = makeRef(VOLATILE_REF_VAL, "volatile-ref");

    /** A field that holds `this` (self-reference identity angle); wired in run(). */
    public FieldObjectRef self;

    /**
     * A field holding a DIFFERENT instance of THIS class (other-instance angle);
     * wired in run().  Its own `ref.val` is OTHER_REF_VAL so the native side can
     * tell it apart from the SINGLETON's ref.  Left null at construction so a
     * field initialiser `new FieldObjectRef()` cannot recurse (the ctor never
     * touches `other`).
     */
    public FieldObjectRef other;

    /** A java.lang.String-typed object reference (the String-ref angle). */
    public String strRef = STR_REF_VALUE;

    /** A boxed java.lang.Integer-typed object reference (the boxed-type angle). */
    public Integer boxedInt = Integer.valueOf(BOXED_INT_VALUE);

    /** An INTERFACE-typed field holding a concrete impl (the interface angle). */
    public Tag tag = new TagImpl(TAG_SLOT_VALUE);

    /** A field declared java.lang.Object holding a Ref at runtime (Object angle). */
    public Object objAsRef = this.ref;

    /** A field declared java.lang.Object holding a String at runtime. */
    public Object objAsString = STR_REF_VALUE;

    /**
     * POLYMORPHIC / inherited angle: declared Ref, holds a SubRef.  Read through
     * a Ref wrapper, the inherited int slot reads back POLY_REF_VAL and the
     * OVERRIDDEN compute() dispatches to SubRef's body (val*3+extra), proving
     * virtual dispatch through a base-typed wrapper and that the klass-match
     * guard accepts a subclass-through-base read (SubRef IS-A Ref).
     */
    public Ref polyRef = new SubRef(POLY_REF_VAL, POLY_REF_LABEL, POLY_REF_EXTRA);

    // ── NULL reference shapes across every declared type (null-slot invariant) ─
    /** Null java.lang.Object reference. */
    public Object nullObj = null;
    /** Null INTERFACE-typed reference. */
    public Tag nullTag = null;
    /** Null object-ARRAY reference. */
    public Ref[] nullArray = null;
    /** Null boxed java.lang.Integer reference. */
    public Integer nullBoxed = null;
    /** Null java.lang.String reference. */
    public String nullStr = null;

    // ── Mutable scratch slots for the object-reference SET/GET round-trip ──────
    /**
     * A mutable Ref slot the native side writes (object-reference store) and
     * reads back, then RESTORES.  Seeded non-null so the set test can prove a
     * non-null -&gt; null -&gt; non-null round-trip.  Distinct from every other
     * field so a stray write never corrupts another scenario.
     */
    public Ref writableRef = makeRef(WRITABLE_REF_VAL, "writable-ref");

    /**
     * A SECOND strong reference to writableRef's original seed object.  The
     * native SET test overwrites the writableRef slot (to setTarget, then null)
     * before restoring it; this field keeps the original seed object permanently
     * GC-reachable so the native restore (re-storing the seed's decoded oop) can
     * never reference a collected object.  Initialised right after writableRef.
     */
    public Ref writableRefKeepAlive = this.writableRef;

    /**
     * A pre-built Ref the native SET test rebinds writableRef to; reading
     * writableRef back must then see SET_TARGET_VAL.  Kept reachable as its own
     * field so it is a live GC root across the native store.
     */
    public Ref setTarget = makeRef(SET_TARGET_VAL, "set-target");

    /**
     * A mutable String slot the native side rebinds via set(std::string) and
     * reads back, then RESTORES — the String-field write (object-reference
     * rebind) round-trip.
     */
    public String writableStr = WRITABLE_STR_SEED;

    /** An object-ARRAY field ('[' descriptor) — the signature-shape angle. */
    public Ref[] refArray =
    {
        makeRef(ARRAY_ELEM0_VAL, "a0"),
        makeRef(ARRAY_ELEM1_VAL, "a1"),
    };

    /**
     * A plain (non-final, mutable) primitive int instance field with an ordinary
     * object slot.  Target for the get_compressed_oop()-on-a-primitive flaw: it
     * has a real 4-byte 'I' slot (no ConstantValue inlining), so reading its
     * compressed-oop returns exactly these 4 bytes.
     */
    public int primitiveInt = PRIMITIVE_INT_VALUE;

    /** The exact value of primitiveInt (native mirrors it). */
    public static final int PRIMITIVE_INT_VALUE = 0x04D2;   // 1234

    // ── Static reference fields (read through the mirror+offset path) ───────

    /** Non-null STATIC object reference. */
    public static Ref staticRef = makeRef(STATIC_REF_VAL, STATIC_REF_LABEL);

    /** Null STATIC object reference. */
    public static Ref staticNullRef = null;

    // ── Identity publication (so native checks are exact) ──────────────────
    public static volatile int refIdentity;
    public static volatile int refAliasIdentity;
    public static volatile int finalRefIdentity;
    public static volatile int volatileRefIdentity;
    public static volatile int selfIdentity;
    public static volatile int otherIdentity;
    public static volatile int staticRefIdentity;
    public static volatile int nestedRefIdentity;
    public static volatile int refArrayIdentity;
    public static volatile int refArrayElem0Identity;
    public static volatile int strRefIdentity;
    public static volatile int boxedIntIdentity;
    public static volatile int tagIdentity;
    public static volatile int objAsRefIdentity;
    public static volatile int objAsStringIdentity;
    public static volatile int polyRefIdentity;

    /** Helper so the field initialisers and run() build Refs identically. */
    private static Ref makeRef(final int val, final String label)
    {
        return new Ref(val, label);
    }

    /**
     * Builds the "other" instance and gives its `ref` a distinguishing value so
     * the native side can confirm it is a genuinely different object.  Safe: the
     * default ctor never wires `other`, so this does not recurse.
     */
    private static FieldObjectRef makeOther()
    {
        final FieldObjectRef o = new FieldObjectRef();
        o.ref = makeRef(OTHER_REF_VAL, "other-ref");
        return o;
    }

    // ── Hook site ──────────────────────────────────────────────────────────
    /**
     * The native module hooks this; calling it through real bytecode dispatch is
     * what makes the interpreter hook fire.  All the object-reference reads in
     * the module happen from native code against the published SINGLETON, so the
     * detour body itself need do nothing but exist.
     */
    public int tick(final int nonce)
    {
        return nonce + 1;
    }

    static
    {
        Harness.register(new Harness.Probe()
        {
            @Override
            public boolean pending()
            {
                return FieldObjectRef.go && !FieldObjectRef.done;
            }

            @Override
            public void run()
            {
                final FieldObjectRef s = SINGLETON;

                // Wire the nested + self + other references on the published
                // instance.
                if (s.ref.next == null)
                {
                    s.ref.next = makeRef(NESTED_REF_VAL, NESTED_REF_LABEL);
                }
                s.self = s;
                if (s.other == null)
                {
                    s.other = makeOther();
                }

                // Publish identities the native side cross-checks against the
                // OOPs it decodes from each field slot.
                FieldObjectRef.refIdentity            = System.identityHashCode(s.ref);
                FieldObjectRef.refAliasIdentity       = System.identityHashCode(s.refAlias);
                FieldObjectRef.finalRefIdentity       = System.identityHashCode(s.finalRef);
                FieldObjectRef.volatileRefIdentity    = System.identityHashCode(s.volatileRef);
                FieldObjectRef.selfIdentity           = System.identityHashCode(s.self);
                FieldObjectRef.otherIdentity          = System.identityHashCode(s.other);
                FieldObjectRef.staticRefIdentity      = System.identityHashCode(FieldObjectRef.staticRef);
                FieldObjectRef.nestedRefIdentity      = System.identityHashCode(s.ref.next);
                FieldObjectRef.refArrayIdentity       = System.identityHashCode(s.refArray);
                FieldObjectRef.refArrayElem0Identity  = System.identityHashCode(s.refArray[0]);
                FieldObjectRef.strRefIdentity         = System.identityHashCode(s.strRef);
                FieldObjectRef.boxedIntIdentity       = System.identityHashCode(s.boxedInt);
                FieldObjectRef.tagIdentity            = System.identityHashCode(s.tag);
                FieldObjectRef.objAsRefIdentity       = System.identityHashCode(s.objAsRef);
                FieldObjectRef.objAsStringIdentity    = System.identityHashCode(s.objAsString);
                FieldObjectRef.polyRefIdentity        = System.identityHashCode(s.polyRef);

                // Real bytecode dispatch -> native interpreter hook fires.
                s.tick(7);

                FieldObjectRef.done = true;
            }
        });
    }

    /**
     * The single instance the native module wraps and drives.  Created eagerly
     * so the native side can fetch it through a static field and so the
     * identities published above match exactly the OOPs the module decodes.
     */
    public static final FieldObjectRef SINGLETON = new FieldObjectRef();
}

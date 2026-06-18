---
slug: method_call_wide_args
title: Method Call Wide Args
category: method
status: seeded
risk: medium
java_versions: [8, 11, 17, 21, 24, 25, 26]
tags: [status/seeded, risk/medium, category/method, tag/method, tag/call, tag/wide-args, tag/two-slot, tag/long, tag/double, tag/overload, tag/call-stub, tag/call-jni]
---

# Method Call Wide Args

> **Category:** [[categories/method|Method proxies (resolve / call / dispatch)]]  ·  **Status:** `seeded`  ·  **Risk:** `medium`  ·  **Specialist:** `.claude/agents/method_call_wide_args-specialist.md`

## Description

`method_proxy::call(args...)` passing `long`/`double` arguments correctly — each
occupies TWO interpreter local slots, every other primitive (and an object
reference) occupies one. Defends against the bug class where a wide arg either
(1) TRUNCATES to 32 bits or (2) SHIFTS / corrupts the FOLLOWING parameter slot.
On the call-stub fast path the `pack` lambda zero-inits an `intptr_t v`, memcpys
the arg, and stores ONE intptr_t word per C++ argument (so a long/double fills
all 8 bytes of one word, a narrow arg's high 4 stay clean), counting `param_idx`
ONE per C++ arg (NOT per interpreter slot). On the call_jni path (taken on every
CI JDK) args go through `write_jni_arg_to_slot` (`value.j` for a 64-bit integral,
`value.d` for a double) and `Call(Static)?<T>MethodA` expands each jvalue into
the two interpreter locals internally. Overload selection (`int64_t`->"J",
`double`->"D") is re-run by both paths; an explicit `get_method(name,"(J)I")`
pins the exact overload.

## Depends on

- [[features/method_call_primitives|method_call_primitives]]
- [[features/jni_arg_packing|jni_arg_packing]]
- [[features/method_overload|method_overload]]

## Related

- [[features/method_call_jni_fallback|method_call_jni_fallback]]
- [[features/method_call_object|method_call_object]]
- [[features/method_call_return_void|method_call_return_void]]
- [[features/method_call_string|method_call_string]]
- [[features/method_return_types|method_return_types]]
- [[features/method_static|method_static]]

## Implementation anchors

- `method_proxy::call (pack lambda + result decode)` — `vmhook/ext/vmhook/vmhook.hpp:16871-17050` — params[8] zero-init, generic-primitive memcpy into one intptr_t/arg, receiver in params[0] for instance; J->int64, D/F memcpy decode
- `method_proxy::call_jni (wide-arg packing)` — `vmhook/ext/vmhook/vmhook.hpp:16233-16870` — write_jni_arg_to_slot: value.j=0 clear then value.j for 64-bit integral / value.d for double; Call(Static)?<T>MethodA does the two-slot expansion
- `method_proxy::resolve_compatible_method / argument_matches_descriptor` — `vmhook/ext/vmhook/vmhook.hpp:17351-17600` — int64_t->J, double->D, int32_t->I, float->F, char16_t/uint16_t->C; signature_pinned forces the exact overload verbatim
- `detail::write_jni_arg_to_slot` — `vmhook/ext/vmhook/vmhook.hpp:13165-13200` — the union-cell packer the call_jni wide path uses (value.j=0 full clear before the narrow write)

## Tests

- `tests/jvm/modules/method_call_wide_args.cpp`

## Known bugs

- **[medium]** The call-stub fast path's wide-arg slot accounting is unproven and suspect: call() passes ONE intptr_t word per C++ argument and sets the stub's size_of_parameters to param_idx (the C++ ARGUMENT count, NOT the JVM SLOT count). A Java (JJ)J body has interpreter parameter_size = 5 slots (this + 2 + 2) but vmhook hands the stub only 3 words for call(a,b) on an instance. Whether the call stub re-expands wide values or copies words verbatim determines if this is correct or a latent mis-read. NOT exercised on CI (the call stub is absent there).

## Notes

call() checks find_call_stub_entry(); the call_jni path is the one actually taken
on every CI JDK (the stub VMStruct is absent), and on that path the two-slot
expansion is the JVM's job (JNI Call*MethodA), not vmhook's — so the wide-arg
correctness CI proves is the call_jni path. argument_matches_descriptor drives the
overload pick: call((int64_t)x) on a name-only proxy with both an int and a long
overload picks long; double maps to D ONLY. The fixture exercises wide args in
leading / middle / trailing positions, long+double mixed, two longs, two doubles,
an all-wide four-arg frame, and the minimal narrow-after-wide witness — on both
paths, instance AND static. Java-8-only fixture; call() runs inside the
trigger(int) detour.

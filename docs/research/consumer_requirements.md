# vmhook consumer requirements — the npnoqol evidence base

**Status:** research input for the handle/reference redesign. Read-only audit, 2026-08-04.
**Consumer:** `C:\repos\cpp\npnoqol` — Lunar Client 1.8.9 mod/anticheat DLL, C++23/MSVC, ~14,410 LOC
across 108 source files under `npnoqol/src`, vendoring a diverged v0.5.0+10-local-patches copy of
vmhook at `npnoqol/ext/vmhook/vmhook.hpp` (17,162 lines).
**Upstream:** `C:\repos\cpp\vmhook\vmhook\ext\vmhook\vmhook.hpp` — v0.5.3, 19,824 lines,
HEAD `27db40e` + one uncommitted addition (`vmhook::is_instance_of`, lines 17137–17179).
**Target runtime:** HotSpot G1, Zulu JDK 17.0.3 / 17.0.18 / 25.0.1. Not Java 8. Not ZGC/Shenandoah.

> **The owner's framing.** vmhook must be effortless to use. *It is not the user's job to manage
> global refs and oops.* Section 3 is the deliverable that matters; sections 1, 2, 4, 5 exist to
> constrain and justify it.

---

## 0. The one-sentence finding

**vmhook hands the consumer *addresses* and asks the consumer to keep them fresh** — and every
single runtime pathology npnoqol has suffered in its entire history is an address that went stale
between the read and the use. The 46 `jni::global_ref` sites, the 67 `.oop()` re-reads, the 39 raw
`void*` oop carriers and the ~2,000 words of load-bearing comments explaining *why each individual
line is ordered the way it is* are not a consumer style problem. They are the consumer paying, by
hand and in every function, for a lifetime model the library declined to own.

The single most damning artefact in the codebase (`flag_manager.cpp:632–652`) reads 16 primitive
fields off one entity and needs **17 address re-reads and 17 null checks** to do it.

---

## 1. Complete call-site inventory

### 1.0 Namespace-qualified frequency (whole of `npnoqol/src`)

| Symbol | Hits |
|---|---|
| `vmhook::return_value` | 65 |
| `vmhook::object` | 54 |
| `vmhook::jni::` (all of it = `global_ref`, `local_frame`, `is_instance_of`) | 52 |
| `vmhook::oop_t` | 43 |
| `vmhook::register_class` | 32 |
| `vmhook::hook` | 13 |
| `vmhook::make_unique` | 10 |
| `vmhook::find_class` | 3 |
| `vmhook::get_class_methods` | 2 |
| `vmhook::shutdown_hooks` | 1 |
| `vmhook::reanchor_classes_via_oop` | 1 |
| `vmhook::detail` | 1 (a comment about `function_traits`) |

Unqualified member surface: `get_instance()` **95**, `.oop()` **80** (67 real calls + 13 comments),
`get_method` **44**, `->call(...)` **41**, `get_field` **28**, `->get()` **25**,
`is_instance_of` **5**, `caller()` **4**, `to_vector` **3**, `to_entries` **1**,
`to_pinned_vector` **1**, `to_pinned_values` **1**, `set_arg` **1**.

**That is the entire API npnoqol uses.** Twelve namespace-scope entry points and eleven member
functions. vmhook's public surface is far larger (see §4.5); nothing else is touched.

---

### 1.1 Object / wrapper model — `object<T>`, `register_class`, `find_class`

**26 wrapper classes**, all in `npnoqol/src/sdk/**`, all of the shape:

```cpp
// C:\repos\cpp\npnoqol\npnoqol\src\sdk\net\minecraft\client\minecraft.hpp:13
class minecraft final : public vmhook::object<sdk::minecraft>
{
public:
    explicit minecraft(const vmhook::oop_t instance) noexcept
        : vmhook::object<sdk::minecraft>{ instance } {}

    static auto get_minecraft() noexcept -> std::unique_ptr<sdk::minecraft>
    { return get_field(mapping::minecraft::theMinecraft)->get(); }

    auto get_the_player() const noexcept -> std::unique_ptr<sdk::entity_player_sp>
    { return get_field(mapping::minecraft::thePlayer)->get(); }
};
```

- **`register_class<T>(name)` — 30 calls** (26 unconditional + 3 Lunar-only in
  `sdk::register_classes()`, `sdk/register_classes.hpp:33–72`, called once at DLL init; +1 eager
  registration of `sdk::minecraft` inside `mapping::can_inject()` because that wrapper *is* the
  client-fingerprinting probe). 32 wrapper files live under `src/sdk`.
  Class names come from `mapping::<x>::clazz` (`sdk/mapping.hpp`, 733 lines), runtime-selected
  between MCP / OBF / SRG (`mapping::mapping_mode`, `mapping.hpp:15–21`) by parsing an embedded
  JSON blob (`sdk/mapping_json.hpp`, 272 lines) and `_strdup`-ing the winning name into an
  `inline const char*`. A gate check enforces registry completeness (`scripts/gate.mjs` check 4).
  **Three distinct name-resolution strategies coexist:** (a) JSON-mapped per mode; (b) hardcoded
  `constexpr` for never-obfuscated library classes (`java.util.ArrayList`, `com.mojang.authlib.*`,
  `net.kyori.adventure.*`); (c) **descriptor-driven runtime discovery** for Lunar mixin methods
  whose names embed a per-build hash (`resolve_method_by_signature<T>`, below).
- **`find_class(name)` — 3 sites**, all in `sdk/mapping.cpp:10,17,40`, used purely as a
  *predicate* ("does this class exist? then we are on Lunar / MCP / OBF").
- **`get_class_methods<T>()` — 2 sites**, `hypixel_module.cpp:707` and `:725`. Used to resolve a
  method **by JVM descriptor** because Lunar's mixin/obfuscated names differ per build:
  ```cpp
  // hypixel_module.cpp:703
  template<class wrapper_type>
  auto resolve_method_by_signature(const std::string& signature) noexcept -> std::string
  {
      for (const auto& [name, candidate] : vmhook::get_class_methods<wrapper_type>())
          if (candidate == signature) return name;
      return std::string{};
  }
  ```
- **`reanchor_classes_via_oop(anchor_oop, {names...})` — 1 site**, `hypixel_module.cpp:62`.
  Lunar's Genesis classloader holds a *second* copy of `net.kyori.adventure.*` and
  `com.mojang.authlib.*`; the whole SDK must be re-anchored onto it, once, using a live
  `EntityPlayerSP` oop as the loader anchor.
- **`get_field(name)->get()` — 25 sites.** Always dereferenced **unchecked** (`->` on an
  `std::optional<field_proxy>`). A misspelled or renamed field is UB, not an error.
- **`get_method(name)->call(...)` / `get_method(name, sig)->call(...)` — 41 sites.** Same
  unchecked-optional pattern. Explicit signature is used 5 times, always to pin a *generic-erased*
  descriptor (`"(Ljava/lang/Object;)Z"`, `"(Ljava/lang/Object;Ljava/lang/Object;)Z"`,
  `"({i_chat_component})V"`). By return type: void/discarded-`Z` 8 · int 3 · float 2 · bool 4 ·
  `std::string` 8 · wrapper `unique_ptr` 13 · raw `oop_t` 2 · `vector<wrapper>` 1.
  By args: none 27 · int 2 · String 5 · object 4 · int+object 1 · String+object 1.
  ⚠ **Explicit signatures are a liability on Lunar** — `scoreboard.hpp:23–39` documents a
  composed `"(IL…ScoreObjective;)V"` that never byte-matched, so the lookup returned `nullopt`
  and the call **silently no-op'd**; the fix was to drop back to name-only resolution.
- **Exactly one field WRITE exists in the entire codebase** —
  `abstract_client_player::set_player_info` (`abstract_client_player.hpp:118–125`), an object-ref
  store into a live entity. It is the single most GC-dangerous operation in the mod and the origin
  of pathology P1/P2 (§5).
- **No `operator bool` exists on `object_base`**, so every wrapper null-test is written
  `x && x->get_instance()` — **~46 occurrences**, 33 of them in `hypixel_module.cpp` alone.

### 1.2 Lifetime — `global_ref`, `local_frame`, raw oop storage

**46 `jni::global_ref` code sites**, concentrated 35 : 4 : 3 : 4 across
`hypixel_module.cpp` : `flag_manager.cpp` : `feature.hpp` : the SDK.

| Kind | Count | Where |
|---|---|---|
| **Long-lived member / static** (survives ticks, crosses threads) | 3 declarations | `hypixel_module.hpp:59` (`nametag_snapshot = unordered_map<int32_t, global_ref>`), `hypixel_module.cpp:84` (`skin_entry::network_player_info`), `:636` (`tab_snapshot::player_list`) |
| **Containers of pins** (per-tick batches) | 6 declarations + 6 loop bindings | `world.hpp:29`, `net_handler_play_client.hpp:44` (return types); `hypixel_module.cpp:821,906,1205,1013`; `flag_manager.cpp:532,561` |
| **Function-scope pins** held across a whole function | 13 | `hypixel_module.cpp:144,152,184,211,385,441,881,896,1012,1170,1232,1429,1489` |
| **Short local pins** (pin → re-read once → use → drop) | 8 | `feature.hpp:92,99,132`; `hypixel_module.cpp:292,359,1054`; `item_stack.hpp:56` |

**`jni::local_frame` — 1 site**, `flag_manager.cpp:476`, `local_frame frame{ 256 }` bracketing one
anticheat observation tick. Exists solely because vmhook leaked one JNI local ref per missed
`DeleteLocalRef` per tick until the worker's local-ref table overflowed and the anticheat silently
died (see §5, P8). Note: **only one of the JNI-heavy lanes has it**;
`hypixel_module::on_run_tick` (~80 players × ~8 calls per tick) has none.

**Raw `void*` / `oop_t` holding a Java oop — 39 sites.** Three of them escape a stack frame:

| Site | What | Exposure |
|---|---|---|
| `hypixel_module.cpp:635` | `std::unordered_map<vmhook::oop_t, std::string> lines` — **raw address as a map key**, unpinned | read from the **render thread** at `:656`, ≥25 ms after it was written |
| `hypixel_module.cpp:190` | `resolve_skin_network_player_info(...) -> void*` — returns a raw oop out of a `global_ref` **and out of the mutex** | consumed at `:972` and `:1388`, with JNI in between |
| `hypixel_module.cpp:969` | `vmhook::oop_t row_oop` — becomes both an `ArrayList` element and a map key | crosses `list->add()` (a safepoint) |

The other 36 are the safe short-lived form `void* const live_x{ ref.oop() };` immediately followed
by a re-wrap. **There is no `void*` member anywhere** — the discipline is real, it is just enormous.

### 1.3 Collections

Only **6 collection call sites in the entire codebase**, and they exercise four different APIs:

```cpp
// world.hpp:23      — field value_t::to_vector
return get_field(mapping::world::playerEntities)->get().to_vector<sdk::entity_player>();
// world.hpp:32      — field value_t::to_pinned_vector   [npnoqol-local CHANGE SET 9]
return get_field(mapping::world::playerEntities)->get().to_pinned_vector();
// net_handler_play_client.hpp:28 — field value_t::to_entries
auto entries{ get_field(...playerInfoMap)->get().to_entries<sdk::uuid, sdk::network_player_info>() };
// net_handler_play_client.hpp:47 — field value_t::to_pinned_values [CHANGE SET 9]
return get_field(...playerInfoMap)->get().to_pinned_values();
// scoreboard.hpp:56 — METHOD value_t::to_vector
return get_method(...getScoreObjectives)->call().to_vector<sdk::score_objective>();
```

`vmhook::list` / `set` / `map` / `collection` are **never named directly** — always reached through
`field_proxy::value_t`. There is no iteration, no `add`/`put`/`remove` through the collection
wrappers; mutation goes through raw Java method calls (`array_list::add`, `property_map::put`).

### 1.4 Hooks

**12 install sites, 10 distinct Java targets.** All installed once, never uninstalled except by
`shutdown_hooks()` (1 site, `feature_manager.cpp:44`). **`scoped_hook` / `hook_handle` are never
used.**

| Target | Site | Detour | Purpose |
|---|---|---|---|
| `EntityPlayerSP.sendChatMessage` | `feature_manager.cpp:13` | `send_chat_message_hook` | command interception (`cancel()`) |
| `GuiNewChat.printChatMessage` | `:14` | `print_chat_message_hook` | chat parsing + `set_arg` rewrite |
| `Minecraft.loadWorld` | `:15` | `load_world_hook` | invalidate caches |
| `Minecraft.runTick` | `:16` | `run_tick_hook` | the tick pump → posts to workers |
| `World.rayTraceBlocks` (explicit sig) | `:28` | `ray_trace_blocks_hook` | camera no-clip |
| `EntityRenderer.orientCamera` | `:30` | `orient_camera_hook` | camera no-clip |
| `EntityLivingBase.bridge$getDisplayNameComponent` (Lunar) | `:34` | `bridge_…_hook` | nametag override |
| `EntityPlayer.getDisplayName` (vanilla) | `:38` | `get_display_name_hook` | nametag override |
| `NetHandlerPlayClient.*()Ljava/util/Collection;` (by descriptor) | `hypixel_module.cpp:748` | `tab_player_info_map_detour` | replace the tab row source |
| `GuiPlayerTabOverlay.*(…)Ljava/lang/String;` ×3 variants | `:767,768,796` | `tab_name_*_detour` | replace the tab line text |

Callback shape is always
`void(vmhook::return_value&, const std::unique_ptr<sdk::W>& self, Args...)`, with `Args` arriving as
`std::unique_ptr<sdk::X>&` for objects, `const std::string&` for Strings, and plain scalars for
primitives. One detour takes a raw `vmhook::oop_t` for a Lunar redirect receiver it does not use
(`hypixel_module.cpp:666`). Detours must **not** be `noexcept` (`function_traits` only specialises
plain function pointers — noted in a comment at `hypixel_module.cpp:641`).

> **There is no post-hook.** `hook<T>()` patches the method's i2i entry point, so every detour runs
> *before* the Java body, and `set()`/`cancel()` suppress the body entirely. "Run the original, then
> observe the result" is not expressible. npnoqol works around this by pre-computing everything on a
> worker thread into a snapshot the detour merely looks up — which is why §2.4 and §2.8 exist at all.
> A post/around hook mode would delete a large fraction of the snapshot machinery.

**`return_value` — 9 real uses:** `set` ×5 (`hypixel_module.cpp:658` *a `std::string`*, `:697` and
`:1573` *an oop*, `camera_no_clip.cpp:34` *typed null*), `cancel` ×2, `set_arg` ×1
(`chat_manager.cpp:29`), `caller` ×1 (`camera_no_clip.cpp:18`).
**`stack_trace()` — 0 uses.**

```cpp
// camera_no_clip.cpp:18 — the only caller() use
const vmhook::return_value::caller_info caller{ return_value.caller() };
if (!caller.valid()) return;
if (caller.class_name  != mapping::entity_renderer::clazz)        return;
if (caller.method_name != mapping::entity_renderer::orientCamera) return;
return_value.set<sdk::moving_object_position>(nullptr);
```

### 1.5 Construction

Two things must not be conflated:

- **`vmhook::make_unique<T>(...)` — 7 real Java allocations.** `chat_component_text`
  (`feature.hpp:87`, `hypixel_module.cpp:1445`, `chat_manager.cpp:29`), `game_profile` (`:139`,
  takes a `unique_ptr<sdk::uuid>` **object** arg), `property` (`:147`, 3 strings),
  `network_player_info` (`:178`, takes a `unique_ptr<sdk::game_profile>` **object** arg),
  `array_list` (`array_list.hpp:23`, no args).
- **`std::make_unique<sdk::T>(oop)` — 17 pure C++ wrapper rebinds.** No allocation, no safepoint.
  These exist *only* to satisfy APIs that take `const unique_ptr<T>&` after an address re-read.
  **All 17 vanish under §3's `ref<T>`** — they are pure ceremony.

No Java array is ever constructed. Object-producing *calls* that also mint Java objects:
`uuid::from_string`, `adventure_component::text`, `legacy_component_serializer::deserialize`.

**`construct()` — 1 wrapper implements it** (`chat_component_text.hpp:16`,
`get_method("<init>")->call(text)`), and it is **currently dead code**: the vendored v0.5.0
`make_unique` runs `<init>` itself via `NewObjectA`, so nothing calls `construct()`. It becomes
live the instant the header moves to v0.5.3. The other four `make_unique`d wrappers (`array_list`,
`game_profile`, `property`, `network_player_info`) do **not** have it — a silent-zeroed-object bug
the moment the header moves. npnoqol's own gate already encodes this as a conditional check
(`scripts/gate.mjs:486–497`), skipped while the pinned header is pre-v0.5.3.

**`make_java_string` / `make_java_array` — 0 uses.** Strings are always passed as `std::string` to
`->call(...)` and let the library marshal them.

### 1.6 Classification

**`vmhook::jni::is_instance_of(oop, "net/minecraft/item/ItemXxx")` — 5 sites**, all inside one
function, `sdk/net/minecraft/item/item_stack.hpp:47–83`. This is a `[npnoqol-local CHANGE SET 8]`
addition, not an upstream API. Its whole job is a five-way item taxonomy for the anticheat
(`sdk::item_kind` = none/other/sword/block/bow/consumable).

⚠ **The five class names are hardcoded MCP strings, deliberately bypassing the mapping system**
(`item_stack.hpp:11–12`: *"no obf/srg names — MCP class names are ground truth and Lunar runs MCP"*).
On an OBF or SRG client `classify()` silently returns `item_kind::other` for everything — the anticheat
degrades to useless without a single error. It is the only place in the SDK that ignores `mapping::`.
**A `ref<T>::is<U>()` form keyed on a `register_class`'d wrapper (§3, Pattern 8) fixes this for free**,
because wrapper class names *are* mapping-resolved.

There is **no other classification** anywhere: no `klass_from_oop`, no class-name-from-oop, no
`for_each_instance`, no `for_each_loaded_class`, no `for_each_thread`.

### 1.7 Everything else npnoqol touches

`shutdown_hooks()` ×1 · `vmhook::oop_t` as a wrapper ctor parameter type (26 wrappers) and as a
map key type (1) · `vmhook::exception` — caught by `catch (...)` around hook installs, never by
type · `VMHOOK_DEBUG_LOGS` referenced in a comment (release builds compile logging out).

**Never used, at all:** `scoped_hook`, `hook_handle`, `watch_handle`, `watch_static_field`,
`on_class_loaded`, `on_exception`, `for_each_instance`, `for_each_loaded_class`, `for_each_thread`,
`deoptimize_*`, `hook_by_signature`, `verify_hooks`, `set_auto_repair_enabled`, `find_field`,
`get_field`/`set_field` free functions, `array_length`, `get_array_element`, `set_array_element`,
`read_java_string`, `write_java_string`, `set_str_field`, `set_prim_array`, `stack_trace`,
`static_field`, `static_method`, `find_methods_by_signature`, `log_class_methods`,
`override_class_lookup`, `evict_class_lookup`, `find_class_via_oop`, `vmhook::pin`.

> **Design signal.** A serious 14 kLOC consumer uses **23 API entry points**. The redesign should
> optimise those 23 ruthlessly and treat the rest as advanced/introspection surface.

---

## 2. The pain map

### 2.1 The mechanism, precisely

Every read/call in vmhook is a potential G1 safepoint. `object_base` caches a bare decoded oop
(`oop_type_t instance` — upstream line 16433). Every accessor that returns an object returns a
**bare, unrooted** oop wrapped in a `unique_ptr` whose destructor does nothing GC-relevant. So the
consumer's obligation, in full, is:

1. Never let a wrapper live across a call.
2. Pin anything that must live across a call — but pinning is *itself* a call, so pin **first** and
   in the right order.
3. After every call, discard the wrapper and rebuild it from the pin's fresh address.
4. Null-check the fresh address (it can vanish).
5. Do all of the above on the correct thread, which must already be attached, which only happens
   as a side effect of some *other* call.

Rules 1–4 are why `.oop()` is called 67 times and 36 `void* const live_x{ … }` locals exist.
Rule 5 is why `chat::add_chat_message` has an eight-line comment explaining its statement order.

### 2.2 Worst offender #1 — 17 re-reads to read 16 fields

`C:\repos\cpp\npnoqol\npnoqol\src\feature\impl\flag\flag_manager.cpp:632–652`

```cpp
// Re-read the pinned oop before EVERY field read (each is a GC safepoint).
player_sample current{};
oop = ref.oop(); if (!oop) { continue; } current.x        = sdk::entity_player{ oop }.get_pos_x();
oop = ref.oop(); if (!oop) { continue; } current.y        = sdk::entity_player{ oop }.get_pos_y();
oop = ref.oop(); if (!oop) { continue; } current.z        = sdk::entity_player{ oop }.get_pos_z();
oop = ref.oop(); if (!oop) { continue; } current.yaw      = sdk::entity_player{ oop }.get_rotation_yaw();
oop = ref.oop(); if (!oop) { continue; } current.pitch    = sdk::entity_player{ oop }.get_rotation_pitch();
oop = ref.oop(); if (!oop) { continue; } current.yaw_head = sdk::entity_player{ oop }.get_rotation_yaw_head();
oop = ref.oop(); if (!oop) { continue; } current.body_yaw = sdk::entity_player{ oop }.get_render_yaw_offset();
oop = ref.oop(); if (!oop) { continue; } current.on_ground= sdk::entity_player{ oop }.get_on_ground();
// ... 8 more identical lines ...
oop = ref.oop(); if (!oop) { continue; } current.held     = sdk::entity_player{ oop }.get_held_item_kind();
```

**Why:** each `get_pos_x()` is a field read that transitions into the VM and can safepoint; the
next line's receiver would be stale. The consumer is manually inlining the library's missing
revalidation. **This is acceptance test A.**

### 2.3 Worst offender #2 — construct-and-publish, pin-per-intermediate

`C:\repos\cpp\npnoqol\npnoqol\src\feature\impl\module\hypixel_module.cpp:122–185`
(abbreviated; the comment is preserved verbatim because it *is* the requirement)

```cpp
auto build_network_player_info(const mojang_api::profile_textures& textures) noexcept
    -> vmhook::jni::global_ref
{
    const std::unique_ptr<sdk::uuid> id{ sdk::uuid::from_string(textures.uuid_dashed) };
    if (!id || !id->get_instance()) { return {}; }

    // vmhook::make_unique / get_properties return UNROOTED oops (they delete the sole
    // local ref), so every intermediate here is relocatable/reclaimable by the NEXT JNI
    // safepoint below (another make_unique, get_properties, put). Holding a bare wrapper
    // across those and then using it as a put-value or ctor-arg stores a DANGLING
    // GameProfile/Property into the live PropertyMap / NetworkPlayerInfo — CLASS A heap
    // corruption a later GC walk trips on (hs_err_pid14484). Pin each the instant it is
    // created and rebuild from the fresh .oop() immediately before each downstream use.
    const std::unique_ptr<sdk::game_profile> profile{ vmhook::make_unique<sdk::game_profile>(id, textures.name) };
    if (!profile || !profile->get_instance()) { return {}; }
    const vmhook::jni::global_ref profile_ref{ profile->get_instance() };

    const std::unique_ptr<sdk::property> texture_property{ vmhook::make_unique<sdk::property>(
        std::string{ "textures" }, textures.value, textures.signature) };
    if (!texture_property || !texture_property->get_instance()) { return {}; }
    const vmhook::jni::global_ref texture_ref{ texture_property->get_instance() };

    void* const profile_oop{ profile_ref.oop() };
    if (!profile_oop) { return {}; }
    const std::unique_ptr<sdk::property_map> properties{ sdk::game_profile{ profile_oop }.get_properties() };
    if (!properties || !properties->get_instance()) { return {}; }
    void* const texture_oop{ texture_ref.oop() };
    if (!texture_oop) { return {}; }
    properties->put("textures", std::make_unique<sdk::property>(texture_oop));

    // put() above was a safepoint — rebuild the profile ctor-arg from its fresh address.
    void* const profile_oop_for_npi{ profile_ref.oop() };
    if (!profile_oop_for_npi) { return {}; }
    const std::unique_ptr<sdk::network_player_info> info{ vmhook::make_unique<sdk::network_player_info>(
        std::make_unique<sdk::game_profile>(profile_oop_for_npi)) };
    if (!info || !info->get_instance()) { return {}; }

    return vmhook::jni::global_ref{ info->get_instance() };
}
```

64 lines to express `new NetworkPlayerInfo(new GameProfile(uuid, name))` with one property put.
**Note two bugs the discipline itself missed**: `id` is never pinned though it is a ctor arg, and
`properties` (a `PropertyMap` returned by a call) is used as a receiver unpinned.
**This is acceptance test B.**

### 2.4 Worst offender #3 — publish a container of Java objects to another thread

`hypixel_module.cpp:880–1004`, `update_tab_list`. 125 lines whose *entire* structure is lifetime
bookkeeping. The load-bearing fragment:

```cpp
// hypixel_module.cpp:894
// Pin the ArrayList: every list->add() below is a GC safepoint that can relocate the list
// itself, so a wrapper built once up-front would hand a stale receiver to the next add().
vmhook::jni::global_ref list_ref{ list->get_instance() };
...
// hypixel_module.cpp:947
// Re-read the pinned (GC-updated) NPI address after the get_game_profile / get_name
// safepoints above, so both the list element we add() and the lines-map key are the
// entry's *current* location — a stale address written into the live ArrayList would
// dangle and corrupt the heap when the render thread iterates it.
void* const live_info_oop{ pinned.oop() };
...
// hypixel_module.cpp:978
if (void* const list_oop{ list_ref.oop() })
{
    const std::unique_ptr<sdk::array_list> live_list{ std::make_unique<sdk::array_list>(list_oop) };
    const std::unique_ptr<sdk::network_player_info> row_info{ std::make_unique<sdk::network_player_info>(row_oop) };
    live_list->add(row_info);
}
```

**This is acceptance test C.**

### 2.5 Re-resolution from a root — the exact recipe

There is **no caching of any root**. The recipe is: *re-read the static field, every time.*

```cpp
// C:\repos\cpp\npnoqol\npnoqol\src\feature\feature.hpp:45
// [rank12] The Minecraft singleton lives as a raw, GC-untracked OOP. Caching one
// wrapper at boot and dereferencing it on worker threads reads theWorld/thePlayer
// off a base G1 may have relocated across a safepoint -> stale-base AV / silent heap
// corruption. Resolve theMinecraft — a STATIC field GC keeps current — FRESH on every
// access instead: operator-> hands back a throwaway wrapper built from the live
// singleton oop, so each field read runs off an up-to-date base.
struct minecraft_accessor final
{
    auto operator->() const noexcept -> std::unique_ptr<sdk::minecraft>
    { return sdk::minecraft::get_minecraft(); }

    explicit operator bool() const noexcept
    { return static_cast<bool>(sdk::minecraft::get_minecraft()); }
};
static constexpr minecraft_accessor minecraft{};
```

**Every `feature::minecraft->x()` is a static-field read plus a heap allocation.** Call counts:
`get_minecraft()` 6 sites, `get_the_player()` 18, `get_the_world()` 6, `get_send_queue()` 3.
`on_run_tick` re-reads `theWorld` **four times in one tick** (`:1139, :1215, :1236, :1492`) and
`thePlayer` **three times** (`:1173, :1192, :1513`); `flag_manager`'s observation tick re-reads
`theMinecraft` **four times** (`:501, :518, :534, :563`). Each re-resolve carries a comment
explaining which intervening call forced it, e.g.:

```cpp
// hypixel_module.cpp:1481
// Re-resolve the world here instead of reusing the one captured at the top of the
// tick. The per-player loop above performs JNI (the barriered skin swap), and each
// JNI call is a GC safepoint that can relocate the long-held raw `world` OOP. A
// stale world hands get_scoreboard() a moved receiver, whose garbage class then
// AVs inside JNI GetMethodID during clear_display_slot().
```

Other re-resolution roots: a **static method re-call** every tick
(`legacy_component_serializer::legacy_section()`, `hypixel_module.cpp:1178`); a one-shot
**classloader re-anchor** (`reanchor_classes_via_oop`, `:62`); a **descriptor-based method
re-resolve** (`resolve_method_by_signature<T>`, `:703`); and **generation-counter invalidation**
(`hypixel_mode::game_generation`, checked in every deferred lambda at `:262, :1285, :1311, :1346,
:1362`). `for_each_instance` is never used — there is always a static root to walk from.

### 2.6 Cross-thread handoff

Five lanes: `worker` (world-load), `tick_worker` (observation/snapshot), `observation_worker` (the
*only* lane allowed to touch the JVM for chat), `fetch_pool{6}` (HTTP only), plus `flag_manager`'s
private `tick_worker`. All are plain `std::thread` draining a `deque<function<void()>>`
(`util/worker_thread.cpp`, `util/worker_pool.cpp`). **Neither class knows anything about the JVM —
no attach, no detach, no oop.** The rule is convention, enforced by a comment:

```cpp
// C:\repos\cpp\npnoqol\npnoqol\src\util\worker_pool.hpp:26
// Post ONLY work that stays out of the JVM. These are plain native threads and
// several run at once, so JNI from them would mean concurrent calls from freshly
// created unattached threads ... HTTP, JSON and pure C++ only.
```

What crosses to a pool thread: `std::string` and a `uint64_t` generation counter. Nothing else.
The anticheat's entire flag pipeline consumes a **pure-C++ POD** — `player_sample`
(`feature/impl/flag/player_sample.hpp`: 15 primitives + a `std::string` + an enum). All 12 flag
implementations are oop-free. *This is the pattern that already works, and the new API should make
it the default.*

Three oop handoffs *do* cross threads, all through `atomic<shared_ptr<const T>>` + pins:
`nametags` (tick_worker → **render thread**), `tab_lines` (tick_worker → **render thread**),
`skin_registry` (tick_worker-owned, cleared from `worker` — worked around by deferring the clear via
`pending_skin_clear`, `hypixel_module.hpp:63–69`).

Thread attachment is implicit, load-bearing and undiscoverable:

```cpp
// C:\repos\cpp\npnoqol\npnoqol\src\feature\feature.hpp:78
// Build the component FIRST: make_unique does the JNI NewObjectA — which both attaches this
// worker thread to the JVM (so the global_refs below can be created; otherwise the very
// FIRST chat on a fresh worker ... was silently dropped because the pin ran before anything
// attached the thread) AND is the only allocation / GC safepoint here.
```

### 2.7 Validity checking — the consumer doing the library's job

| Guard | Count | Note |
|---|---|---|
| `!x \|\| !x->get_instance()` | ~46 | the prologue of nearly every function |
| `void* const live{ ref.oop() }; if (!live) …` | ~30 | post-pin re-validation |
| `if (x && x->get_instance())` | ~12 | inline-init form |
| `zoo::feature::sanity_check()` — a *live JNI liveness probe*, 5 field reads | 3 sites | `feature.cpp:46–58` |
| `world_ready` atomic — a hand-cached `sanity_check` | 1 | `module_manager.cpp:19–25` |
| `local_frame frame{ 256 }` | 1 | `flag_manager.cpp:476` |
| `is_readable_pointer` | 0 direct calls | pushed into the library by CHANGE SET 2/5 — *proof this works* |

```cpp
// C:\repos\cpp\npnoqol\npnoqol\src\feature\impl\module\module_manager.cpp:19
// Published by the tick_worker ... and read cheaply by the render-thread hooks below. Those
// hooks fire per rendered player / per ray-trace / per frame, so calling sanity_check() there
// paid ~5 live JNI field reads (get_minecraft x3 + thePlayer + theWorld) every time just to
// gate on "is the game ready".
std::atomic<bool> world_ready{ false };
```

### 2.8 Caches of Java objects

`skin_registry` (nick → `global_ref` NPI, **never evicted**, hard cap 512) ·
`nametags` (entity id → `global_ref` Component, replaced per tick) ·
`tab_snapshot::lines` (**raw oop** → string, replaced per tick, the one raw-address key) ·
`tab_snapshot::player_list` (`global_ref` ArrayList).

The identity decision is explicit and correct — and it is a **library gap**:

```cpp
// C:\repos\cpp\npnoqol\npnoqol\src\feature\impl\module\hypixel_module.hpp:55
// ... Keyed by entity id rather
// than the raw OOP so a GC relocation / address reuse can't return the
// wrong component.
```
```cpp
// C:\repos\cpp\npnoqol\npnoqol\src\sdk\net\minecraft\entity\player\entity_player.hpp:22
// Server-assigned entity id (inherited from Entity).  Stable for the
// entity's lifetime and unaffected by GC relocation, unlike the raw OOP —
// use it as a snapshot key rather than get_instance().
```

The consumer had to find a *domain-specific* stable identity because the library offers none.

### 2.9 Pain summary — what the library leaves on the floor

1. **No revalidating reference.** `global_ref` + `.oop()` + re-wrap, hand-written ~48 times.
2. **Object returns are unrooted from birth.** Four sites forget to pin
   (`hypixel_module.cpp:125` `id`, `:159` `properties`, `:395` `stack_oop`, `:1176` the anchor).
3. **No local-ref frame management** (pre-JNI-free builds). One lane has it; the heaviest doesn't.
4. **No cheap liveness probe** → a hand-cached `world_ready` atomic.
5. **No cross-thread-safe handle** → `pending_skin_clear` exists only to move a destructor.
6. **No root abstraction** → 4 `theWorld` reads and 3 `thePlayer` reads per tick.
7. **No stable object identity** → domain-specific keys, and one raw-address key with a known race.
8. **No `operator bool`** → ~46 `x && x->get_instance()`.
9. **Unchecked `optional` deref** on every `get_field`/`get_method` → a renamed field is UB.
10. **Implicit thread attach** → statement-order-dependent silent failures.

---

## 3. The dream API  ⭐ *(the centrepiece)*

### 3.0 Design axioms

| # | Axiom | Kills |
|---|---|---|
| **A1** | **The consumer never sees an address.** No `void*`, no `oop_t`, no `.oop()`, no `get_instance()` in user code. | §2.2, §2.4, 39 raw sites |
| **A2** | **A reference revalidates on every dereference, inside the library.** The user cannot forget, because there is nothing to remember. | §2.2, P2–P7 |
| **A3** | **Rooting is automatic and total.** Every object the library hands out is already rooted; there is no unrooted intermediate to lose. | §2.3, P5, P6 |
| **A4** | **Containers of Java objects are rooted during the walk, or they do not exist.** The `to_vector`-then-pin shape is unexpressible. | P7 (CHANGE SET 9) |
| **A5** | **Identity is stable, hashable and relocation-proof** — usable as a map key. | §2.8 |
| **A6** | **Liveness is a first-class, cheap, checkable state**, distinct from "null". | §2.7 |
| **A7** | **Handles are thread-agnostic**: creatable, copyable and *destructible* on any thread; acquisition attaches or fails loudly. | §2.6, P10 |
| **A8** | **Lookup failure is a value, not UB.** No unchecked `optional` deref. | §2.9 item 9 |
| **A9** | **The pin *mechanism* is a policy, invisible in the API.** JNI global ref, OopStorage slot, or detour-scoped borrow — the consumer code is byte-identical. | unblocks design while A-vs-B (§4.6) is open |

A9 is the one that unblocks the project: **the ergonomic API can be designed and agreed now**,
because none of the 12 patterns below name a pin mechanism.

### 3.1 The type vocabulary

```cpp
namespace vmhook
{
    // A rooted, revalidating, copyable reference to a live Java object.
    // Replaces: oop_t, std::unique_ptr<object<T>>, jni::global_ref — all three.
    template<class T> class ref;

    // A non-owning reference valid only for the enclosing scope (hook detour args,
    // for_each visitors). Cheap: no root allocated. Promote with .pin().
    template<class T> class borrowed;

    // A reference that does not keep the object alive; observes collection.
    template<class T> class weak_ref;

    // A rooted container. Built pin-during-walk; A4 makes the unsafe shape impossible.
    template<class T> class ref_vector;             // ~ std::vector<ref<T>>
    template<class K, class V> class ref_map_view;  // ~ std::vector<std::pair<ref<K>, ref<V>>>

    // A revalidating handle onto a VM root (static field / static method / singleton).
    template<class T> class root;

    // Stable, relocation-proof identity. Hashable, comparable, printable.
    class object_id;
}
```

`ref<T>::operator->` returns an internal `access<T>` proxy that (a) re-reads the root slot,
(b) materialises a `T` bound to the *current* address, (c) lives exactly as long as the full
expression. That is the whole of A2, and it is invisible.

```cpp
template<class T>
class ref
{
public:
    ref() noexcept;                                    // empty
    ref(const ref&);                                   // COPYABLE (unlike global_ref)
    ref(ref&&) noexcept;
    auto operator=(const ref&) -> ref&;
    auto operator=(ref&&) noexcept -> ref&;
    ~ref();                                            // safe on ANY thread (A7)

    explicit operator bool() const noexcept;           // rooted AND alive
    auto alive() const noexcept -> bool;               // false after collection/world teardown
    auto id()    const noexcept -> object_id;          // A5

    auto operator->() const noexcept -> detail::access<T>;   // A2 — revalidates
    auto operator*()  const noexcept -> detail::access<T>;

    template<class U> auto is()   const noexcept -> bool;    // instanceof, class-chain + interfaces
    template<class U> auto as()   const noexcept -> ref<U>;  // empty ref if not an instance
    auto class_name() const -> std::string;

    auto weak() const noexcept -> weak_ref<T>;
};

template<class T> auto operator==(const ref<T>&, const ref<T>&) noexcept -> bool;  // by identity
// std::hash<vmhook::ref<T>> specialised on object_id
```

**Wrapper authoring is unchanged in spirit, simplified in fact:**

```cpp
class entity_player : public vmhook::object<entity_player>
{
public:
    using vmhook::object<entity_player>::object;              // no hand-written ctor

    auto pos_x()     const -> double        { return field<double>("posX"); }
    auto name()      const -> std::string   { return call<std::string>("getName"); }
    auto inventory() const -> vmhook::ref<inventory_player> { return field<vmhook::ref<inventory_player>>("inventory"); }
};
```

`field<R>(name)` / `call<R>(name, args...)` are the two accessors. `R` may be a primitive, `bool`,
`std::string`, `void`, `ref<W>`, `ref_vector<W>`, `ref_map_view<K,V>`, or `std::optional<R>` when
the caller wants to distinguish "absent" from "default" (A8). A missing field/method is reported
once via the diagnostic channel and yields a value-initialised `R`; an
`try_field<R>` / `try_call<R>` pair returning `std::expected<R, vmhook::error>` covers the strict
case.

---

### Pattern 1 — read N fields off one object *(acceptance test A)*

**Before** — `flag_manager.cpp:632–652`, 17 re-reads / 17 null checks for 16 fields:

```cpp
player_sample current{};
oop = ref.oop(); if (!oop) { continue; } current.x    = sdk::entity_player{ oop }.get_pos_x();
oop = ref.oop(); if (!oop) { continue; } current.y    = sdk::entity_player{ oop }.get_pos_y();
oop = ref.oop(); if (!oop) { continue; } current.yaw  = sdk::entity_player{ oop }.get_rotation_yaw();
// ... 13 more identical lines ...
oop = ref.oop(); if (!oop) { continue; } current.held = sdk::entity_player{ oop }.get_held_item_kind();
```

**After** — every `->` revalidates internally (A2):

```cpp
if (!player) { continue; }                          // A6: one liveness check, or none
player_sample current{
    .name      = player->name(),
    .x         = player->pos_x(),
    .y         = player->pos_y(),
    .z         = player->pos_z(),
    .yaw       = player->rotation_yaw(),
    .pitch     = player->rotation_pitch(),
    .yaw_head  = player->rotation_yaw_head(),
    .on_ground = player->on_ground(),
    .sprinting = player->is_sprinting(),
    .held      = player->held_item_kind(),
};
```

Optional optimisation for hot loops — one revalidation for a whole block, with the library
asserting no safepoint escapes:

```cpp
player.read([&](const sdk::entity_player& e) noexcept {
    current.x = e.pos_x(); current.y = e.pos_y(); current.z = e.pos_z();
});   // ref<T>::read(F&&) — revalidate once, bind, invoke; F must not call Java
```

---

### Pattern 2 — cache the local player across ticks

**Before** — impossible to cache; re-walked from the singleton on every access, allocating:

```cpp
struct minecraft_accessor final {
    auto operator->() const noexcept -> std::unique_ptr<sdk::minecraft>
    { return sdk::minecraft::get_minecraft(); }
};
static constexpr minecraft_accessor minecraft{};
// ... and then, per tick:
const std::unique_ptr<sdk::minecraft> mc{ sdk::minecraft::get_minecraft() };
if (!mc || !mc->get_instance()) { return; }
const std::unique_ptr<sdk::entity_player_sp> local{ mc->get_the_player() };
if (!local || !local->get_instance()) { return; }
```

**After** — a `root<T>` is a revalidating handle onto a VM root; a `ref<T>` is cacheable:

```cpp
// once, at init:
inline const vmhook::root<sdk::minecraft> mc{ vmhook::root_kind::static_field,
                                              "net/minecraft/client/Minecraft", "theMinecraft" };

// per tick — mc-> re-reads the static field; no allocation, no wrapper churn:
vmhook::ref<sdk::entity_player_sp> local{ mc->the_player() };
if (!local) { return; }

// and, unlike today, this is legal — the ref stays valid across ticks and threads:
class my_module { vmhook::ref<sdk::entity_player_sp> cached_local_; };
```

`root<T>` also covers static-method roots
(`root_kind::static_method`, e.g. `LegacyComponentSerializer.legacySection()`), which npnoqol
re-invokes every tick at `hypixel_module.cpp:1178`.

---

### Pattern 3 — iterate the entity list each tick *(replaces `to_pinned_vector`)*

**Before** — two parallel APIs, one of which is a landmine, plus a manual re-pin loop that still
exists in the nametag path (`hypixel_module.cpp:1218–1226`):

```cpp
auto get_player_entities()        -> std::vector<std::unique_ptr<sdk::entity_player>>;  // UNSAFE
auto get_player_entities_pinned() -> std::vector<vmhook::jni::global_ref>;              // CHANGE SET 9
// caller:
const std::vector<vmhook::jni::global_ref> pinned{ world->get_player_entities_pinned() };
for (const vmhook::jni::global_ref& ref : pinned) {
    void* oop{ ref.oop() }; if (!oop) { continue; }
    const std::int32_t id{ sdk::entity_player{ oop }.get_entity_id() };
    oop = ref.oop(); if (!oop || sdk::entity_player{ oop }.is_dead()) { continue; }
    // ...
}
```

**After** — one API, rooted during the walk by construction (A4); the unsafe shape does not exist:

```cpp
auto players() const -> vmhook::ref_vector<sdk::entity_player>
{ return field<vmhook::ref_vector<sdk::entity_player>>("playerEntities"); }

// caller:
for (const vmhook::ref<sdk::entity_player>& p : world->players())
{
    if (!p || p->is_dead()) { continue; }
    observe(p->entity_id(), p);
}
```

`ref_vector<T>` exposes `begin/end/size/empty/operator[]`, is movable, and its elements are ordinary
`ref<T>`s that can be copied out into a cache.

---

### Pattern 4 — iterate a Java Map *(replaces `to_entries` / `to_pinned_values`)*

**Before** — two APIs again, plus a hand-rolled null filter (`net_handler_play_client.hpp:22–48`):

```cpp
auto entries{ get_field(...playerInfoMap)->get().to_entries<sdk::uuid, sdk::network_player_info>() };
for (auto& [uuid, info] : entries) { if (info) result.push_back(std::move(info)); }
// ...and separately:
return get_field(...playerInfoMap)->get().to_pinned_values();   // keys dropped
```

**After**:

```cpp
auto player_info() const -> vmhook::ref_map_view<sdk::uuid, sdk::network_player_info>
{ return field<vmhook::ref_map_view<sdk::uuid, sdk::network_player_info>>("playerInfoMap"); }

for (const auto& [id, info] : net_handler->player_info())
{
    if (!info) { continue; }
    use(info->game_profile()->name());
}
// values-only, when the key is not wanted:
for (const vmhook::ref<sdk::network_player_info>& info : net_handler->player_info().values()) { ... }
```

---

### Pattern 5 — read a nested field/method chain

**Before** — `hypixel_module.cpp:911–946`, four wrappers, two pins, three address re-reads:

```cpp
void* const info_oop{ pinned.oop() }; if (!info_oop) { continue; }
const std::unique_ptr<sdk::network_player_info> info{ std::make_unique<sdk::network_player_info>(info_oop) };
const std::unique_ptr<sdk::game_profile> profile{ info->get_game_profile() };
if (!profile || !profile->get_instance()) { continue; }
const vmhook::jni::global_ref profile_ref{ profile->get_instance() };
void* const name_profile{ profile_ref.oop() }; if (!name_profile) { continue; }
const std::string name{ sdk::game_profile{ name_profile }.get_name() };
if (name.empty()) { continue; }
```

**After** — each `->` returns a rooted `ref` (A3); the chain is safe *because* it is a chain:

```cpp
const std::string name{ info->game_profile()->name() };
if (name.empty()) { continue; }
```

For the deep case, a null-short-circuiting chain avoids the pyramid of `if`s:

```cpp
const std::string kit{ player.chain()
    .then(&sdk::entity_player::inventory)
    .then([](auto& inv) { return inv.armor_item(3); })
    .then(&sdk::item_stack::display_name)
    .value_or("") };
```

---

### Pattern 6 — construct a Java object graph *(acceptance test B)*

**Before** — 64 lines, 2 pins, 3 address re-reads, 6 null checks (§2.3).

**After** — arguments are `ref`s or values; the library roots every intermediate for the duration of
the call (A3), so no dangling ctor arg is expressible:

```cpp
auto build_network_player_info(const mojang_api::profile_textures& t)
    -> vmhook::ref<sdk::network_player_info>
{
    vmhook::ref<sdk::uuid> id{ sdk::uuid::from_string(t.uuid_dashed) };
    if (!id) { return {}; }

    vmhook::ref<sdk::game_profile> profile{ vmhook::create<sdk::game_profile>(id, t.name) };
    vmhook::ref<sdk::property>     texture{ vmhook::create<sdk::property>("textures", t.value, t.signature) };
    if (!profile || !texture) { return {}; }

    profile->properties()->put("textures", texture);
    return vmhook::create<sdk::network_player_info>(profile);
}
```

`vmhook::create<T>(args...)` allocates **and runs the Java `<init>`** matching the argument types,
returning a rooted `ref<T>` (empty on failure). It replaces both `make_unique<T>` and the duck-typed
`construct()` convention. Overload selection uses the arg types; an explicit descriptor overload
`vmhook::create<T>(vmhook::descriptor{"(Ljava/util/UUID;Ljava/lang/String;)V"}, id, name)` covers
generic erasure.

**64 lines → 11.**

---

### Pattern 7 — call a Java method with an object argument

**Before** — `array_list.hpp:27–32` plus the caller at `hypixel_module.cpp:978–985`:

```cpp
template<class W> auto add(const std::unique_ptr<W>& item) const noexcept -> void
{ get_method("add", "(Ljava/lang/Object;)Z")->call(item); }
// caller must rebuild BOTH receiver and argument from fresh addresses:
if (void* const list_oop{ list_ref.oop() }) {
    const std::unique_ptr<sdk::array_list> live_list{ std::make_unique<sdk::array_list>(list_oop) };
    const std::unique_ptr<sdk::network_player_info> row_info{ std::make_unique<sdk::network_player_info>(row_oop) };
    live_list->add(row_info);
}
```

**After** — receiver and args are both `ref`s, both revalidated at dispatch by the library:

```cpp
template<class W> auto add(const vmhook::ref<W>& item) const -> bool
{ return call<bool>(vmhook::descriptor{"add", "(Ljava/lang/Object;)Z"}, item); }
// caller:
list->add(row_info);
```

---

### Pattern 8 — classify an ItemStack *(replaces `is_instance_of` + manual pin)*

**Before** — `item_stack.hpp:47–83`, a pin, six `.oop()` re-reads, five stringly-typed probes:

```cpp
const vmhook::oop_t item_oop{ this->get_item() };
if (!item_oop) { return sdk::item_kind::none; }
const vmhook::jni::global_ref item_ref{ item_oop };
if (!item_ref.oop()) { return sdk::item_kind::none; }
if (vmhook::jni::is_instance_of(item_ref.oop(), "net/minecraft/item/ItemSword")) return sdk::item_kind::sword;
if (vmhook::jni::is_instance_of(item_ref.oop(), "net/minecraft/item/ItemBlock")) return sdk::item_kind::block;
// ... 3 more
```

**After** — `is<U>()` on a rooted ref; the class handle is resolved once and cached by the library:

```cpp
auto classify() const -> sdk::item_kind
{
    const vmhook::ref<sdk::item> item{ this->item() };
    if (!item)                       { return sdk::item_kind::none; }
    if (item.is<sdk::item_sword>())  { return sdk::item_kind::sword; }
    if (item.is<sdk::item_block>())  { return sdk::item_kind::block; }
    if (item.is<sdk::item_bow>())    { return sdk::item_kind::bow; }
    if (item.is<sdk::item_food>()
     || item.is<sdk::item_potion>()) { return sdk::item_kind::consumable; }
    return sdk::item_kind::other;
}
```

`item.is<U>()` requires only that `U` was `register_class`'d. A string form
`item.instance_of("net/minecraft/item/ItemSword")` stays available for classes with no wrapper.
**Must support interfaces**, which the uncommitted upstream `is_instance_of` does not (§4.1 gap 7).

---

### Pattern 9 — hook detour: args, receiver, return value

**Before**:

```cpp
auto tab_name_lunar_detour(vmhook::return_value& return_value,
    const std::unique_ptr<sdk::gui_player_tab_overlay>& /*this*/,
    vmhook::oop_t /*redirected receiver*/,                              // ← raw oop in user code
    const std::unique_ptr<sdk::network_player_info>& info) -> void
{
    if (!info || !info->get_instance()) { return; }
    const auto snapshot{ tab_lines.load(std::memory_order_acquire) };
    if (!snapshot) { return; }
    if (const auto it{ snapshot->lines.find(info->get_instance()) }; it != snapshot->lines.end())
        return_value.set(it->second);                        // ← std::string: REMOVED in v0.5.3
}
```

**After** — `borrowed<T>` for detour-scoped args (zero root cost), and a `set` that knows Java types:

```cpp
void tab_name_detour(vmhook::hook_context& ctx,
                     vmhook::borrowed<sdk::gui_player_tab_overlay> self,
                     vmhook::borrowed<sdk::gui_player_tab_overlay> redirect_receiver,
                     vmhook::borrowed<sdk::network_player_info>    info)
{
    if (!info) { return; }
    const auto snapshot{ tab_lines.load(std::memory_order_acquire) };
    if (!snapshot) { return; }
    if (const auto it{ snapshot->lines.find(info.id()) }; it != snapshot->lines.end())
        ctx.result.set(it->second);                          // std::string → library builds the String
}
```

`hook_context` carries `result` (the old `return_value`), `args`, `caller()`, `stack_trace()`.
`ctx.result.set` overloads: primitives, `bool`, `std::string`/`string_view` (**builds a Java
String**), `const ref<T>&` / `const borrowed<T>&` (revalidates at set time), and
`ctx.result.set_null<T>()`. `ctx.arg(1) = "text"` replaces `set_arg`. Note `info.id()` as a map
key — A5 removes the raw-address key at `hypixel_module.cpp:635` and its documented ≤25 ms race.

---

### Pattern 10 — publish a container of Java objects to another thread *(acceptance test C)*

**Before** — `update_tab_list`, 125 lines (§2.4), plus a manual "call `set()` before the snapshot
`shared_ptr` drops to zero or a concurrent swap `DeleteGlobalRef`s the oop" rule at
`hypixel_module.cpp:1569–1573`.

**After** — `ref<T>` is copyable and refcounted; a snapshot is just data:

```cpp
struct tab_snapshot
{
    std::unordered_map<vmhook::object_id, std::string> lines;   // A5: stable, relocation-proof key
    vmhook::ref<sdk::array_list>                       player_list;
};

// worker thread
auto list{ vmhook::create<sdk::array_list>() };
for (const auto& info : net_handler->player_info().values())
{
    if (!info) { continue; }
    const std::string line{ format_row(info) };
    list->add(info);
    fresh->lines.emplace(info.id(), line);
}
fresh->player_list = list;                       // a copy; both refs root the same object
tab_lines.store(std::move(fresh), std::memory_order_release);

// render thread — no ordering rule, no lifetime rule
void tab_list_detour(vmhook::hook_context& ctx, vmhook::borrowed<sdk::net_handler_play_client>)
{
    if (const auto snap{ tab_lines.load(std::memory_order_acquire) }; snap && snap->player_list)
        ctx.result.set(snap->player_list);
}
```

The "set it before the snapshot dies" hazard disappears because `set(const ref<T>&)` revalidates
against a root the caller demonstrably still holds.

---

### Pattern 11 — write an object into a Java field (the skin swap)

**Before** — `hypixel_module.cpp:1386–1405`, the site that produced the G1 write-barrier heap
corruption (P1/P2) and needed a caller-side re-read *and* a library-side fix:

```cpp
if (void* const npi_oop{ resolve_skin_network_player_info(name) })
    if (void* const live_player_oop{ pinned.oop() })      // re-read, or the WRITE corrupts the heap
    {
        sdk::abstract_client_player client_player{ live_player_oop };
        const std::unique_ptr<sdk::network_player_info> info{ std::make_unique<sdk::network_player_info>(npi_oop) };
        client_player.set_player_info(info);
    }
```

**After** — receiver and value are refs; the barriered store is the only store the library has:

```cpp
if (const vmhook::ref<sdk::network_player_info> skin{ skin_for(name) })
    player->set_player_info(skin);
```

with the wrapper written as
`auto set_player_info(const vmhook::ref<sdk::network_player_info>& v) -> void { set_field("playerInfo", v); }`.
**Requirement: `set_field` for reference types must be barriered, always, with no raw-`memcpy`
fallback** — the fallback is what CHANGE SET 1 existed to remove.

---

### Pattern 12 — cache Java objects in a long-lived, cross-thread registry

**Before** — `skin_registry`: a `global_ref` member, a mutex, a raw `void*` escape hatch, a
deferred-clear flag to avoid cross-thread `DeleteGlobalRef`, and a never-evict policy because
eviction timing is unanalysable:

```cpp
struct skin_entry { mojang_api::profile_textures textures{}; vmhook::jni::global_ref network_player_info{};
                    bool has_textures{}; bool npi_built{}; };
std::mutex skin_mutex{}; std::unordered_map<std::string, skin_entry> skin_registry{};
auto resolve_skin_network_player_info(const std::string& nick) noexcept -> void*;   // raw oop escapes
inline static std::atomic<bool> pending_skin_clear{ false };                        // defer the destructor
```

**After** — a `ref<T>` member is an ordinary C++ value; destruction is thread-agnostic (A7):

```cpp
struct skin_entry
{
    mojang_api::profile_textures            textures{};
    vmhook::ref<sdk::network_player_info>   npi{};        // rooted, copyable, any-thread destructible
};
std::mutex skin_mutex{};
std::unordered_map<std::string, skin_entry> skin_registry{};

auto skin_for(const std::string& nick) -> vmhook::ref<sdk::network_player_info>
{
    std::lock_guard lock{ skin_mutex };
    auto& e{ skin_registry[normalize(nick)] };
    if (!e.npi) { e.npi = build_network_player_info(e.textures); }
    return e.npi;                                          // a copy, not an address
}
// world change:  skin_registry.clear();     // safe from any thread; pending_skin_clear deleted
```

Where the mod *wants* eviction on world change rather than pinning forever, `weak_ref<T>` gives
"observe but do not retain", and `ref<T>::alive()` reports collection explicitly (A6) instead of
returning a plausible-looking dead address.

---

### 3.13 Ergonomic requirements that are not patterns

| Req | Statement | Evidence |
|---|---|---|
| **E1** | `object<T>` gets `explicit operator bool` and `operator==`; `get_instance()` becomes internal. | ~46 `x && x->get_instance()` |
| **E2** | Field/method lookup failure is a *value*, never an unchecked `optional` deref; a missing name is logged once per name, not per call. | 25 + 41 unchecked `->` |
| **E3** | Symmetric extraction: `to_vector`/`to_entries` must work identically on field results, method results and collection wrappers. | `scoreboard.hpp:56` is a method result; upstream lacks it (§4.1 gap 8) |
| **E4** | A cheap `vmhook::vm_ready()` (no Java call) replaces the 5-field-read `sanity_check()`. | `module_manager.cpp:19–25` |
| **E5** | Thread attach is implicit in handle acquisition and **never silently no-ops**; a failed attach is observable. | `feature.hpp:78` |
| **E6** | Local-ref management is entirely internal (A3 implies it). No consumer-visible `local_frame`. | `flag_manager.cpp:476` is the only one — and the heavier lane has none |
| **E7** | `register_class<T>` should also register `T`'s Java superclass relationship so `ref<Base>` ⇄ `ref<Derived>` conversion and `is<U>()` work without a second lookup. | `world_client : world`, `abstract_client_player : entity_player`, `entity_player : entity` |
| **E8** | Method resolution by **descriptor** must be first-class (`find_methods_by_signature` exists but is unused because it is not reachable from the wrapper). | `resolve_method_by_signature<T>` hand-rolled at `hypixel_module.cpp:703` |
| **E9** | Keep `object<T>` inheritance for wrappers — the SDK's 32 wrapper classes and their inheritance chains are the part of the model that *works*. Do not replace it. | §1.1 |
| **E10** | Every name-taking API must have a **type-taking** sibling that routes through `register_class` (so `mapping::`-resolved names apply): `is<U>()` beside `instance_of(name)`, `create<T>()` beside a descriptor form. Raw-string APIs are the escape hatch, not the default. | `item_stack::classify` hardcodes MCP names and dies silently on OBF/SRG |
| **E11** | Consider a **post/around** hook mode. Its absence is the root cause of npnoqol's entire snapshot architecture (gap 17): three `atomic<shared_ptr<const T>>` snapshots, a deferred-clear flag, and a documented "call `set()` before the snapshot dies" ordering rule. | §1.4, §2.4, §2.8 |

### 3.14 What section 3 deliberately does not decide

- **Which root mechanism.** A9. `ref<T>` is a handle onto a root slot; whether that slot is a JNI
  global ref, an OopStorage slot, or a detour-scoped borrow-only degraded mode is a policy chosen
  at build/runtime. The 12 patterns above are unchanged by the choice. In a borrow-only build,
  `ref<T>` construction outside a detour would fail loudly (`alive() == false`) rather than lie.
- **Refcounting strategy** for copyable `ref<T>` (one root per ref vs. an intrusive
  `shared_ptr`-style control block over a single root). The latter is cheaper when snapshots are
  copied — npnoqol copies refs into per-tick snapshots ~80×/tick.
- **`object_id` derivation.** Options: the root slot address (stable while rooted, not comparable
  across independent pins of the same object), `System.identityHashCode` (correct, costs a call),
  or a library-side identity map. npnoqol needs *map-key* semantics only, so slot-based identity
  with an `operator==` that compares *current addresses* is likely sufficient.

---

## 4. Migration blockers

### 4.1 The 8-item gap table, verified against the current upstream header

Line numbers are `C:\repos\cpp\vmhook\vmhook\ext\vmhook\vmhook.hpp` as of `27db40e` + working tree.

| # | API | v0.5.3 status (verified) | npnoqol sites | Break |
|---|---|---|---|---|
| 1 | `return_value::set(std::string)` | **STILL MISSING.** `set` is `template<value_type>` at **1386–1387** with `static_assert(std::is_trivially_copyable_v<value_type>)` at **1392–1395**. No String overload despite `make_java_string` existing at **13100**. npnoqol compiles today only because its vendored copy carries an **undocumented local `set(std::string_view)` overload** (`ext/vmhook/vmhook.hpp:1224`) that does `NewStringUTF` → decode → `memcpy` into the slot — an 11th, unlisted CHANGE SET. | 1 — `hypixel_module.cpp:658` | **compile** |
| 2 | `return_value::set(unique_ptr<T>)` | Removed; the typed-null form `set<W>(nullptr)` **survives** at **1435–1437**. | 1 — `camera_no_clip.cpp:34` uses the *surviving* form | **none** |
| 3 | `make_unique` runs Java `<init>` | **CONFIRMED REMOVED.** `make_unique` at **12619–12621** does TLAB alloc (`make_java_object`, **12910**) then calls a duck-typed `result->construct(args...)` iff present (**12675–12681**), else logs a warning (**12684**). | 4 wrappers lack `construct()`: `array_list`, `game_profile`, `property`, `network_player_info`. `chat_component_text` has it. | **SILENT** (zeroed object) |
| 4 | `jni::global_ref` real GC pin | **CONFIRMED NO-OP.** Class at **19725–19796**: ctor stores the raw oop (**19730–19733**), `~global_ref() noexcept = default;` (**19735**), `oop()` returns the stored pointer (**19768–19771**). ⚠ **The class doc at 19702–19724 still claims real GC-root semantics** ("*the constructor promotes it to a JNI global reference*", "*oop() re-derives the live address every call*") — directly contradicting the honest note at 19756–19767. | 46 | **SILENT UB** — *the crux* |
| 5 | `jni::local_frame` | **CONFIRMED ABSENT.** `namespace jni` (**19700**) contains `global_ref` and nothing else; `vmhook::pin` free functions sit outside it at **19803/19814**. | 1 — `flag_manager.cpp:476` | **compile** |
| 6 | `method_proxy::call` JNI fallback | **CONFIRMED REMOVED.** `call_jni` survives only in comments (14923, 14948, 15084, 15149, 15225, 15288, 15341, 15407, 15900). `call()` at **15052** requires `StubRoutines::_call_stub_entry` (**15068**) and logs + fails when absent (**15075**). npnoqol's own gate records the field result: *"`_call_stub_entry` absent from every tested `jvm.dll`, so `method_proxy::call()` is a silent no-op on every JDK tested"* and *"all 21 Windows JVM cells red"* (`scripts/gate.mjs:66`; `vmhook-vendoring.md`). | all 41 calls | **RUNTIME-FATAL** |
| 7 | `is_instance_of` | **PARTIALLY FIXED.** Added uncommitted at **17137–17179** as `vmhook::is_instance_of(void*, std::string_view)` — walks `klass_from_oop` → `get_super()` comparing `symbol::to_string()`. **But npnoqol calls `vmhook::jni::is_instance_of` — a different namespace → still a compile break.** And its doc admits *interfaces are not matched* (secondary-super array not walked). | 5 — `item_stack.hpp:62,66,70,74,78` | **compile** + semantic gap |
| 8 | `to_vector` / `to_pinned_vector` / `to_pinned_values` on method/field results | **PARTIALLY PRESENT.** `field_proxy::value_t::to_vector` (decl **14067**, def **18350**) and `::to_entries` (decl **14081**, def **18412**) exist. `collection::to_vector` **17393**, `map::to_entries` **17687**. **`method_proxy::value_t` has NEITHER** (class at **14868**; members are only `operator target_type` 14909, `is_void` 14989, `is_string` 14997, `as_string` 15015). **`to_pinned_*` do not exist at all.** | `scoreboard.hpp:56` (method `to_vector`), `world.hpp:32`, `net_handler_play_client.hpp:47` (`to_pinned_*`) | **compile** ×3 |

**Verdict: 0 of 8 items are fully closed.** Item 7 is half-done (wrong namespace, no interfaces);
item 2 was never a problem.

### 4.2 New gaps found by diffing npnoqol's calls against upstream

| # | Gap | Detail | Break |
|---|---|---|---|
| **9** | **`method_proxy::value_t` has no `signature` member**, unlike `field_proxy::value_t` (**13497**). | Any consumer code that inspects a returned value's Java type has nowhere to look. | latent |
| **10** | **`vmhook::jni::is_instance_of` → `vmhook::is_instance_of`** is a *namespace* delta, not just an addition. | 5 call sites need requalifying. | **compile** |
| **11** | **`object_base` has no `operator bool`** (whole class 15978–16900). | 46 sites keep the `x && x->get_instance()` idiom; a redesign that adds it is a pure win with zero breakage. | ergonomic |
| **12** | **`field_proxy::value_t` has no `std::monostate`** (variant at 13486–13496) while `method_proxy::value_t` does (14870–14882). | A null reference field is indistinguishable from compressed-oop `0`. Asymmetric with the method path. | latent |
| **13** | **No public class-name-from-oop.** `oop_klass()`, `get_field_by_oop_klass()`, `get_method_by_oop_klass()` are **protected** on `oop_reflective_base` (17213/17221/17262). | A consumer must reach into `vmhook::hotspot::` to name an object's class — exactly what npnoqol's `item_stack::classify` needed and got via a local patch. | design |
| **14** | **`field_proxy` in v0.5.3 carries no barrier context.** The vendored copy's CHANGE SET 1 added a second ctor `(barrier_instance_oop, barrier_field_name)` and routed instance object-ref writes through `SetObjectField`. Upstream's `field_proxy::set` (14280) has no such path. | The one object-field write npnoqol performs (`abstract_client_player::set_player_info`) would revert to the raw `memcpy` that caused the P1 heap corruption. In a JNI-free build the barrier must be emitted some other way — **this is an open library problem, not a migration chore.** | **SILENT heap corruption** |
| **15** | **`return_value::caller()` hardening is npnoqol-local.** CHANGE SETS 2 and 5 gate the `Method*` and transitive `ConstMethod*` derefs with `is_readable_pointer` / safe-read. Upstream `caller()` is at 1510/10232 — needs verification that the same guards landed. | `camera_no_clip` re-enabled its `rayTraceBlocks` hook *only because* of those guards; without them, 1642 caught AVs and a 79 MB log in one capture. | **AV storm** |
| **16** | **`hypixel_module.cpp:1218–1226` still uses the unsafe `to_vector`-then-pin shape** that CHANGE SET 9 was written to eliminate (the nametag path was never migrated to `_pinned`). | Live CLASS-A exposure in the shipped mod today. A4 makes this shape unexpressible. | **latent CLASS-A** |
| **17** | **No post/around hook mode.** `hook<T>()` patches the i2i entry, so a detour can only run *before* the body and either let it run or replace the return. | Forces the entire worker-thread-snapshot architecture (§2.4, §2.8, three `atomic<shared_ptr>` snapshots, `pending_skin_clear`) because the mod cannot observe a real return value. | design |
| **18** | **Classification bypasses the mapping layer.** `is_instance_of` takes a raw class-name string, so consumers hardcode names; `item_stack::classify` degrades silently to `other` on OBF/SRG clients. | An `is<U>()` keyed on a registered wrapper type resolves through `mapping::` for free. | **SILENT** on non-MCP clients |

### 4.3 Exact compile-breaking signature deltas

```
- vmhook::jni::local_frame{ 256 }
+ (delete — library-internal)                                    flag_manager.cpp:476

- vmhook::jni::is_instance_of(void*, std::string_view) -> bool
+ vmhook::is_instance_of(void*, std::string_view) -> bool         item_stack.hpp:62,66,70,74,78
  (and: no interface support; no ref/wrapper overload)

- return_value::set(const std::string&)
+ (absent; requires set<void*>(make_java_string(s)))              hypixel_module.cpp:658

- field_proxy::value_t::to_pinned_vector() -> std::vector<jni::global_ref>
+ (absent)                                                        world.hpp:32
- field_proxy::value_t::to_pinned_values() -> std::vector<jni::global_ref>
+ (absent)                                                        net_handler_play_client.hpp:47

- method_proxy::value_t::to_vector<T>() -> std::vector<std::unique_ptr<T>>
+ (absent — only field_proxy::value_t has it)                     scoreboard.hpp:56

- vmhook::make_unique<T>(args...)  // ran Java <init>
+ vmhook::make_unique<T>(args...)  // requires T::construct(args...)
                                    array_list.hpp, game_profile.hpp, property.hpp,
                                    network_player_info.hpp  (4 wrappers to add)
```

### 4.4 Non-compiling but fatal

- **`method_proxy::call()` on Zulu 17/25.** If `StubRoutines::_call_stub_entry` is not exposed, all
  41 call sites become silent no-ops. This must be measured on the actual target before any header
  swap; a fallback dispatch path (JNI or i2i trampoline) is a hard requirement, not an option.
- **`global_ref` no-op.** Swapping the header without a real pin turns 46 correct pins into 46
  silent lies. This is worse than the compile breaks because nothing reports it.

### 4.5 API surface npnoqol never touches

For redesign scoping: `scoped_hook`, `hook_handle`, `watch_handle`, `watch_static_field`,
`on_class_loaded`, `on_exception`, `for_each_instance`, `for_each_loaded_class`, `for_each_thread`,
`deoptimize_*`, `hook_by_signature`, `verify_hooks`, `find_field`, free `get_field`/`set_field`,
`array_length`/`get_array_element`/`set_array_element`, `read_java_string`/`write_java_string`,
`set_str_field`/`set_prim_array`/`set_str_array`/`set_bool_array`, `stack_trace`, `static_field`,
`static_method`, `find_methods_by_signature`, `log_class_methods`, `override_class_lookup`,
`evict_class_lookup`, `find_class_via_oop`, `vmhook::pin`, `vmhook::exception` (by type).

Also note: `type_to_class_map` (1678), `g_type_factory_map` (1703), `g_field_cache` (12699) and
their mutexes are `inline` globals in the `vmhook::` namespace — consumer-reachable mutable state
that should be encapsulated.

### 4.6 The unresolved decision that gates everything

`vmhook-migration.md` records the blocking research result: a **safe, 100 %-JNI-free relocation pin
is not achievable on Zulu 17/25**. A dump of Lunar's Zulu 17 `jvm.dll` (116,224 exports) found none
of `OopStorage::allocate`, `JNIHandles::make_global`, `global_handles`; production Zulu ships no
debug symbols; hand-replicating `OopStorage::allocate` lockless "races the GC → heap corruption"
and was refused. The options on the table are **A** (one `NewGlobalRef` per pin — safe, recommended)
and **B** (pin-free: do all oop work inside detours on JavaThreads — a large rewrite that collides
with `hook-design-rule.md`'s "Java-thread hooks do minimal work").

**Axiom A9 exists so this decision does not block the API.** Both A and B implement the same
`ref<T>` / `borrowed<T>` surface; B simply makes `ref<T>` acquisition outside a detour fail loudly
instead of silently.

---

## 5. Behavioural traps the new design must structurally prevent

Sources: `.claude/knowledge/crash-audit.md`, `crash-hunt-2026-07-21.md`, `crash-hunt-2026-07-22.md`,
`camera-noclip-caller-av.md`, and `ext/vmhook/VMHOOK_CHANGES.txt` (CHANGE SETS 1–10).

The systemic cause, in the audit's own words: vmhook *"represents every live Java object as a raw,
GC-untracked OOP at every layer"*, and *"`is_readable_pointer` only no-ops an **unmapped** read; it
can't save a readable-remapped receiver and does **nothing** for a WRITE. Reads AV on garbage
(class B/C); **WRITES store a dangling OOP into a live field/collection with the G1 barrier marking
it live** (class A) — the fault lands later elsewhere."* Class A presents as
`EXCEPTION_ACCESS_VIOLATION reading -1` on a GC thread, no Java frames, the hs_err reporter itself
crashing.

| # | Pathology | Symptom | Root cause | Consumer fix | Prevented by §3? |
|---|---|---|---|---|---|
| **P1** | **G1 write-barrier AV** | AV reading -1 in `GuiPlayerTabOverlay$PlayerComparator.compare`, in chat send, on a GC thread | `field_proxy::set()` stored a compressed oop with raw `memcpy`, skipping SATB + card-table barriers; G1 reclaimed a live `NetworkPlayerInfo` | CHANGE SET 1 — route through `SetObjectField` | **Partial → must be made total.** A3/Pattern 11 require *all* reference stores to be barriered with **no raw fallback**. Gap 14: upstream has no barrier path at all. |
| **P2** | **Stale receiver on the barriered write** | client thread, JIT'd frame, dangling oop read as -1 | the write's receiver was captured before intervening safepoints; *"a caller re-reading `pinned.oop()` could not fix this — **the window is INSIDE the helper**"* | CHANGE SET 1 addendum: promote receiver *and* value to real local refs inside the helper | **Yes.** A2: the library revalidates at the point of use; no caller discipline can substitute. |
| **P3** | **Stale receiver on instance dispatch** | `jni_get_method_id` AV reading -1; `Call*Method` reading `0x28` off null; `jni_get_object_class` reading `0xFFFF…` | `call_jni` built the receiver for both `GetObjectClass` and the call from a fake stack handle | 3 commits placing `is_readable_pointer` guards (188c052 → a3aba6a → d9e4855), then CHANGE SET 7 (real local refs) | **Yes** — and it also removes the entire heuristic-guard-placement class of bug. |
| **P4** | **Stale object *arguments*** | CLASS A: `ArrayList.add` / `Map.put` / `addChatMessage` store a dangling element | object args marshalled from raw oops after the receiver/method-id safepoints | CHANGE SET 4 | **Yes.** Pattern 7: args are refs, revalidated at dispatch. |
| **P5** | **Stale *constructor* arguments** — `hs_err_pid14484` | **GC Thread#2**, AV on compressed oop `0x…24ffffb9`, `jvm.dll+0xcb86`, no Java frames | `jni_make_unique` passed ctor args as fake handles across `GetMethodID` + `NewObjectA` (which can trigger a young GC) → a dangling `gameProfile` stored inside a fresh NPI, later published to the tab list | CHANGE SET 6 + app-side pin-every-intermediate | **Yes.** Pattern 6. *Also kills the sibling bug*: `jni_arg_cleanup` decided "is this a ref?" by pointer-range heuristic over a `union`, so `int 42` → `DeleteLocalRef(0x2A)` → JVM-fatal. Typed refs make it a type fact. |
| **P6** | **Unrooted object RETURNS** | diffuse — the origin of every stale read | `call_jni` object-returns delete the sole local ref and hand back a bare pointer; `make_unique` likewise | **NEVER LANDED.** Held at every audit as *"the ultimate cure"*. Mitigated by ~12 app-side pins. | **Yes — this *is* the handle model.** A3. Every app-side pin in three crash hunts exists only because of P6. |
| **P7** | **CLASS-A pin-loop race** (ranked #1, 2026-07-22) | CLASS-A GC-thread fatal | `to_vector`/`to_entries` decode all N elements to raw pointers; the caller pins them one by one, and `NewGlobalRef`'s VM entry **is a safepoint** — so each pin roots a *stale* sibling **into the GC root set**. *"No purely app-side fix closes it."* The recommended "pin everything" doctrine was itself the crash driver. | CHANGE SET 9 — `to_pinned_vector` / `to_pinned_values` | **Yes, structurally.** A4: `ref_vector<T>` is the only container API, built pin-during-walk, so the shape is unexpressible. ⚠ Gap 16: npnoqol *still* has one unmigrated site. |
| **P8** | **JNI local-ref leak** | **no crash** — the anticheat *"stops after a few minutes"*, `/acdebug` goes silent | no frame management; one missed `DeleteLocalRef` per site per tick overflows the worker's local-ref table, after which JNI returns null | CHANGE SET 10 + caching the tab snapshot to cut the rate 20× | **Yes.** A3/E6: RAII ownership cannot be leaked by a missed manual delete. (In a JNI-free build it vanishes for a different reason.) |
| **P9** | **`caller()` AV storm on compiled frames** | **1642 caught AVs** on one method in one capture; **79 MB** log; camera no-clip dead post-JIT | `caller()` reads `[caller_rbp − 24]` as a `Method*` — valid only for *interpreter* frames. On a C2 frame it is a spilled register; the arithmetic validity heuristic passes it; the raw deref hardware-faults. *"A hardware AV is an SEH exception, so the enclosing `try/catch(const std::exception&)` never catches it."* | CHANGE SETS 2 + 5 (`is_readable_pointer` + safe-read metadata walk) | **No — orthogonal.** Separate requirement: **validate the frame kind (return PC in the interpreter code cache) before decoding it**, and route every VM-metadata deref through safe-read. Gap 15. |
| **P10** | **Worker/attach rules** | first chat on a fresh worker silently dropped; cross-thread `DeleteGlobalRef` UAF | (a) pool threads must never touch the JVM; (b) a pin on an *unattached* thread is a silent no-op; (c) freeing a root from the wrong thread races a writer | convention + `pending_skin_clear` deferral + statement reordering | **Partial.** A7/E5: acquisition attaches or fails loudly; destruction is thread-agnostic. Lane ownership stays an application concern. |
| **P11** | **World-load data race** | JVM-fatal AV via use-after-free | `on_load_world` cleared `history` on `worker` while `tick_worker` iterated it. *"The 'touched only on tick_worker, so no lock' invariant was simply **false**."* | post the reset to `tick_worker` | **No.** Plain C++ container race — included as the reminder that not every crash here is a vmhook problem. |

**Score: 7 of 11 fully prevented by the §3 model, 2 partially (P1 needs the barrier requirement
made explicit; P10 needs the attach contract), 2 out of scope (P9 frame-kind validation, P11
application threading).**

### 5.1 The ten local CHANGE SETs as de-facto library requirements

`ext/vmhook/VMHOOK_CHANGES.txt`. Each is a behaviour the consumer needed badly enough to fork the
library for. **All ten must be satisfied — by the new model or by an equivalent — before npnoqol
can un-fork.**

1. **Barriered object-field writes** (+ receiver/value rooted *inside* the helper). → Pattern 11, gap 14.
2. **Crash-safe `caller()`** — `is_readable_pointer`-gated `Method*` deref. → gap 15.
3. **Reject a dangling receiver before dispatch** — explicitly *"a partial net"*. → obsoleted by A2.
4. **Rooted object arguments** in calls. → Pattern 7.
5. **Safe-read `caller()` metadata walk.** → gap 15.
6. **Rooted constructor arguments** + explicit `is_local_ref` flags (no union heuristics). → Pattern 6.
7. **Rooted instance receiver** on dispatch. → A2.
8. **`is_instance_of`.** → Pattern 8 (needs interfaces + a wrapper overload).
9. **Pin-during-walk collection accessors.** → A4 / Pattern 3–4.
10. **`local_frame` RAII.** → A3 / E6 (internalised).
11. **(unlisted, undocumented)** `return_value::set(std::string_view)` at `ext/vmhook/vmhook.hpp:1224`
    — `NewStringUTF` → decode → write the slot. Not in `VMHOOK_CHANGES.txt`; found only by diffing.
    → Pattern 9, gap 1. *A reminder that the fork's divergence is larger than its own changelog.*

### 5.2 Two process lessons worth encoding

- *"'it compiles' and 'the vendor's CI is green' are different claims, and for a vendored dependency
  only the second one matters."* (`decisions.md`, after the v0.5.0→v0.5.3 sync was reverted with
  **all 21 Windows JVM cells red**.)
- **npnoqol has no test suite and cannot run outside Minecraft.** Java names are runtime strings, so
  a wrong name compiles perfectly and silently no-ops. Any redesign that shifts errors from
  compile-time to runtime-silent is a regression *for this consumer specifically*. E2 (lookup
  failure as an observable value) is therefore not a nicety.

---

## 6. Acceptance tests for the new API

Each is an existing npnoqol function; the new API is accepted when the rewrite is correct, obviously
correct, and materially shorter.

| ID | Function | Today | Target |
|---|---|---|---|
| **A** | `flag_manager::on_minecraft_tick` field-read block, `flag_manager.cpp:632–652` | 21 lines, 17 address re-reads, 17 null checks | ≤ 12 lines, **0** address re-reads |
| **B** | `build_network_player_info`, `hypixel_module.cpp:122–185` | 64 lines, 2 pins, 3 re-reads, 6 null checks, 2 latent bugs | ≤ 12 lines, 0 pins |
| **C** | `update_tab_list`, `hypixel_module.cpp:880–1004` | 125 lines, ~40 of them lifetime bookkeeping | ≤ 60 lines, 0 raw addresses |
| **D** | `item_stack::classify`, `item_stack.hpp:47–83` | 37 lines, 1 pin, 6 `.oop()` | ≤ 14 lines, 0 pins |
| **E** | `chat::add_chat_message`, `feature.hpp:75–109` | 35 lines + an 8-line comment on statement order | ≤ 8 lines, order-independent |
| **F** | `hypixel_module::on_run_tick` root re-resolution | `theWorld` ×4, `thePlayer` ×3 per tick | ×1 each, cached in a `ref` |
| **G** | `feature::minecraft_accessor`, `feature.hpp:52–66` | a custom `operator->` returning `unique_ptr`, allocating per access | `vmhook::root<sdk::minecraft>` |
| **H** | Whole-codebase grep | `void*` 39 · `.oop()` 67 · `get_instance()` 95 · `global_ref` 46 | **0 · 0 · 0 · 0** |

---

## 7. Open questions for the API designer

1. **Refcount granularity** for copyable `ref<T>` — one root per copy, or a shared control block?
   npnoqol copies ~80 refs per tick into snapshots.
2. **`object_id` derivation** — root-slot address, `identityHashCode`, or a library identity map?
   Only map-key semantics are actually needed (§2.8).
3. **`borrowed<T>` enforcement** — can escape be prevented at compile time (a lifetime-bound token
   parameter), or is it documentation + a debug-build assert?
4. **`ref_vector<T>` element ordering vs. rooting cost** — npnoqol walks ~80 entities and ~80 tab
   entries per tick; that is ~160 roots per tick at 20 Hz = 3,200 root ops/s. Is that acceptable
   under mechanism A (one `NewGlobalRef` each)? If not, a **batch-root** primitive
   (`root_all(span<oop>)` taken once at a single safepoint) is required.
5. **Degraded mode semantics** — under mechanism B (no pins), does `ref<T>` outside a detour
   throw, return empty, or compile-error? A9 says it must not silently lie.
6. **Barrier emission without JNI.** Gap 14 is unsolved: `SetObjectField` was the barrier. A
   JNI-free build needs either a direct SATB-enqueue + card-mark implementation or an explicit
   "reference stores are unsupported" contract. npnoqol performs exactly one such store, but it is
   the one that produced the worst crash in the project's history.

---

*Prepared from a read-only audit of both trees. No files outside*
`C:\repos\cpp\vmhook\audit\research\consumer_requirements.md` *were created or modified.*

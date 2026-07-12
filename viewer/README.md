# vmhook viewer

A C++23 / ImGui (Win32 + DirectX 11) desktop tool that turns the header-only
`vmhook` library into an interactive JVM inspector — plus an **MCP server** so
Claude/other AIs can drive it (see [`mcp/`](mcp/README.md)).

**Highlights**

- Modern dark UI (Segoe UI + Font Awesome icons, custom theme) with a resizable
  IDE-style split. **Borderless** window with in-app min/close controls.
- **DPI-aware and adaptive** — crisp on high-DPI displays, and every size is
  font-relative so the whole UI scales with `Ctrl +/-` zoom and the display DPI.
- Auto-refreshing JVM list showing each JVM's **command line** (not just `java.exe`).
- Class list with instant search (Ctrl+F), sortable columns, method/field counts,
  and smooth scrolling over tens of thousands of classes (clipped).
- **Class-kind badges** — every class is tagged `[interface]` / `[enum]` /
  `[abstract]` / `[annotation]` / `[record]` / `[class]` (+ `[final]` / `[nested]`)
  from its real class-file access flags, and the class list is **colour-coded by
  kind**. A **kind filter** narrows the list to one kind at a time.
- Per-class **methods & fields** with **access modifiers** colour-coded by
  visibility (public/private/protected), pretty-printed signatures
  (`(int, double) : double`, toggle to raw descriptors), per-pane filters, and
  right-click copy.
- **Navigate the class graph like a browser:** superclass shown as `extends X`,
  and every field **and method** type that resolves to a loaded class is a
  **clickable link** — in the per-class view *and* the global member search — so
  you can jump in, then **Back / Forward** (◀ ▶ buttons or `Alt+←` / `Alt+→`).
- **Search members across every class** — switch the search scope to
  **Methods** / **Fields** to find a member by name anywhere in the JVM.
- **Show inherited members** — one toggle folds in methods & fields from the whole
  superclass chain (inherited rows dimmed, hover shows the declaring class); it
  also drives **Copy all**.
- **Live heap inspection** — pick a class and hit **Live instances** to scan the
  heap for its live objects, shown master/detail: a clickable instance list on the
  left, the selected object's **Field / Value** table on the right — own *and*
  inherited fields, colour-coded by type (strings, refs, booleans, null), live-
  updating, sortable and filterable. Ref values are clickable (jump to the class).
- **Edit & freeze fields** — every instance/static field row has inline actions:
  ✏ **edit** the value (primitive text, a new `String`, `null`, a `0x` oop, or a
  saved object), and 🔒 **freeze** it — a payload thread then re-writes it ~50×/s so
  it holds against the running program until you unlock it. A **frozen: N**
  indicator in the status bar lists every freeze with one-click / bulk unfreeze.
- **Object clipboard** — 📌 grab any object (a reference field, an instance, a call
  result) into a **Saved objects** strip, then **drag a chip onto a field or a
  method argument** to place it. Repoint references and pass objects around live.
- **Array inspector** — click any array-typed value (`int[]`, `String[]`, …) to open
  its elements in a table; object elements are clickable / grabbable into the clipboard.
- **Static fields window** — a class's live static values, editable/freezable like
  instance fields, plus a **static-method call** panel.
- **Call methods & construct objects** — invoke any method on an instance (or a
  static), or a **constructor** to make a new object; fill arguments from primitives,
  `#text` new Strings, `@null`, or saved objects, and see the result inline (which you
  can grab). Works on every JDK — the call runs inside a hook detour on a real Java
  thread and dispatches through JNI.
- **Live class-load tracking** — turn on **Auto** and new classes appear the moment
  `ClassLoader.defineClass` defines them (an event-driven `on_class_loaded` hook,
  no full re-scan). **Rescan** does a full re-list + diff (also catches unloads).
- **Sort the class list** by name, package, **age** (when a class first appeared),
  kind, or member count — ascending or descending.
- **Ctrl +/−/0** scales the UI font for accessibility / dense listings.
- One-click **Copy all** (Java-like listing) / **Export .txt**.
- Re-attach works (the payload serves every attach; no restart needed).

It:

1. Lists every running **HotSpot JVM** on the machine (any process with
   `jvm.dll` loaded — `java.exe`, `javaw.exe`, modded Minecraft, embedded VMs…).
2. Injects the **`vmhook_payload.dll`** into the JVM you pick.
3. That DLL uses `vmhook` — **pure-VM, no JNI / no JVMTI** — to walk every loaded
   Java class and enumerate each class's declared **methods** and **fields**, then
   streams the whole surface back over a named pipe.
4. The viewer shows it in an MCPMappingViewer-style layout: a searchable class
   list up top, methods and fields below. Everything is discovered **dynamically
   at runtime** — there are no mappings.

## Layout

```
┌ JVM  [com.example.demo.ExampleApp - 21544  ▾]  [Attach]  Done ───────────┐
│  PID   Process     Path                                                   │
│  10720 javaw.exe   C:\...\javaw.exe                                       │
├──────────────────────────────────────────────────────────────────────────┤
│  [Search classes]                     12994 classes loaded                │
│  Package                                              Class                │
│  net/minecraft/client                                Minecraft            │
│  net/minecraft/client/gui/toasts                     ToastGui             │
├────────────────────────────────┬─────────────────────────────────────────┤
│ Methods (42)                    │ Fields (73)                             │
│  Name          Descriptor       │  Name         Type          Static     │
│  getInstance   ()Lbdl;          │  instance     Lbdl;         yes        │
└────────────────────────────────┴─────────────────────────────────────────┘
```

## Build (MSVC)

Requires Visual Studio 2022 (or Build Tools) with the C++ workload and the
Windows SDK. ImGui is fetched into `third_party/imgui` (git-ignored):

```bat
:: from the repo root
git clone --depth 1 https://github.com/ocornut/imgui viewer/third_party/imgui
cmake -S viewer -B build/viewer -G "Visual Studio 17 2022" -A x64
cmake --build build/viewer --config Release
```

Outputs `vmhook_viewer.exe` and `vmhook_payload.dll` side by side in
`build/viewer/bin/Release/`. Run the exe from that directory (it loads the
payload DLL next to itself). The MSVC runtime is static-linked, so the payload
is self-contained when injected.

## Notes

- **Same bitness:** inject a 64-bit payload into a 64-bit JVM (the CMake targets
  build x64). vmhook's runtime hooking is x86_64-only.
- **Permissions:** attaching to some JVMs needs the same/higher privilege; run
  the viewer elevated if `OpenProcess` fails.
- **Pipe:** the viewer creates `\\.\pipe\vmhook_viewer` before injecting; the
  payload connects to it and streams `C`/`M`/`F` records terminated by `DONE`.

## How it maps onto vmhook

| Viewer step            | vmhook API                                   |
|------------------------|----------------------------------------------|
| list classes            | `vmhook::for_each_loaded_class`              |
| a class's methods        | `vmhook::detail::collect_klass_methods`      |
| a class's fields         | `klass::collect_fields()` (added for this)   |
| live instances of a class| `vmhook::for_each_instance_of` (added) + `vmhook::get_field<T>` |
| a field's value          | `vmhook::get_field<T>` / `read_java_string` / `decode_oop_pointer` |
| a class's static values  | `klass::get_java_mirror()` + `vmhook::get_field<T>` (mirror-relative offset) |
| write / freeze a field   | `vmhook::set_field<T>` (freeze = a payload thread re-writing it ~50×/s) |
| call a method            | JNI `Call*MethodA` run inside a `vmhook::hook` detour on a real JavaThread |
| new-class notifications  | `vmhook::on_class_loaded` (deopted `ClassLoader.defineClass` detour) |

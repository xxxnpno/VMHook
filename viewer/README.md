# vmhook viewer

A C++23 / ImGui (Win32 + DirectX 11) desktop tool that turns the header-only
`vmhook` library into an interactive JVM inspector — plus an **MCP server** so
Claude/other AIs can drive it (see [`mcp/`](mcp/README.md)).

**Highlights**

- Modern dark UI (Segoe UI font, custom theme) with a resizable IDE-style split.
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
- **Navigate the class graph like a browser:** superclass shown as `extends X`
  and every field whose type is a loaded class is a **clickable link** — jump in,
  then **Back / Forward** (◀ ▶ buttons or `Alt+←` / `Alt+→`).
- **Show inherited members** — one toggle folds in methods & fields from the whole
  superclass chain (inherited rows dimmed, hover shows the declaring class).
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
┌ Running JVMs  [Refresh] [Attach & Enumerate]  Status: Done ──────────────┐
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
cmake -S viewer -B build-viewer -G "Visual Studio 17 2022" -A x64
cmake --build build-viewer --config Release
```

Outputs `vmhook_viewer.exe` and `vmhook_payload.dll` side by side in
`build-viewer/bin/Release/`. Run the exe from that directory (it loads the
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
| list classes           | `vmhook::for_each_loaded_class`              |
| a class's methods       | `vmhook::detail::collect_klass_methods`      |
| a class's fields        | `klass::collect_fields()` (added for this)   |

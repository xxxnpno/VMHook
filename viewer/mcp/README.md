# vmhook MCP server

Lets Claude (or any [MCP](https://modelcontextprotocol.io) client) drive the
vmhook viewer: discover running JVMs, inject the vmhook payload, and inspect any
JVM's classes / methods / fields — all discovered live, no mappings.

## Pieces

- **`vmhook_cli.exe`** — a JSON CLI (built by `viewer/CMakeLists.txt`) that does
  the actual injection + enumeration and caches the result per pid.
- **`vmhook_mcp.py`** — a dependency-free (stdlib only) stdio MCP server that
  wraps the CLI.

## Tools

| Tool | Args | Result |
|------|------|--------|
| `list_jvms`     | –                        | `[{pid, image, cmdline}, …]` |
| `enumerate_jvm` | `pid`                    | injects + enumerates + caches; `{pid, classes, methods, fields}` |
| `list_classes`  | `pid`, `filter?`         | `[ "java/util/HashMap", … ]` (from cache) |
| `get_class`     | `pid`, `class_name`      | `{name, super, kind, access, methods:[{name,descriptor,signature,modifiers,access}], fields:[{name,descriptor,type,modifiers,access,static}]}` |
| `get_instances` | `pid`, `class_name`, `cap?` | `{pid, class, instances:[{address, fields:{name:value, …}}]}` — live heap objects of the class + their field values (own + inherited); injects if needed, so no prior `enumerate_jvm` required |
| `get_statics`   | `pid`, `class_name`      | `{pid, class, statics:{name:value, …}}` — the class's STATIC field values, read live from its `java.lang.Class` mirror (class-level state the instance tools don't show) |
| `search_members`| `pid`, `query`, `scope?` | `[{class, kind, name, descriptor, signature\|type, static?}, …]` — every method/field whose name matches, across all classes (from cache); `scope` ∈ `all`/`methods`/`fields`. Capped at 5000 hits |
| `set_instance_field` | `pid`, `class_name`, `address`, `field`, `value` | **writes** one field on the live instance at `address` (from `get_instances`); `{pid, class, field, value, ok:true}` with the re-read value, or `{error}` |
| `set_static_field`   | `pid`, `class_name`, `field`, `value` | **writes** a static field on the class mirror; same result shape |

`kind` is one of `class` / `interface` / `enum` / `abstract` / `annotation` /
`record` (from the class-file access flags); `access` is the raw flag word.

Typical flow: `list_jvms` → pick a pid → `enumerate_jvm(pid)` → then
`list_classes` / `get_class` / `search_members` as much as you like (they read
the cache; re-run `enumerate_jvm` to refresh — re-attach is supported).
`get_instances` / `get_statics` read the **live heap / class mirror** directly
(not the cache), so they reflect the current object state on every call.

### Reading *and* writing

The `set_*` tools **mutate the running JVM** — the same pure-VM path the reader
uses, in reverse. Values are parsed by the field's declared type:

- **primitives** — `int`/`long`/`short`/`byte`/`char`/`float`/`double`/`boolean`
  are parsed from the string (`"42"`, `"3.14"`, `"true"`, `"0x1F"`) — fully reliable;
- **reference** fields — take `null`, or a raw `0x<oop>` heap address to repoint them;
- **String** fields — also accept the literal text, but building a *new* String is
  best-effort: it needs a VM allocation from the helper thread and may be refused
  (you get a clear `{"error": …}`); `null` and repointing always work.

The write is re-read and echoed back, so `value` in the result is what actually
landed. Example — freeze a worker's byte field and bump a static counter:

```
$ vmhook_cli set-instance 15352 com/example/demo/ExampleApp$Worker 0x60F44C718 b8 7
{"pid":15352,"class":"…$Worker","field":"b8","value":"7","ok":true}

$ vmhook_cli set-static 15352 com/example/demo/ExampleApp tickCounter 999
{"pid":15352,"class":"…","field":"tickCounter","value":"999","ok":true}
```

(Fields the target program keeps rewriting each loop — like a live tick counter —
will show your value on the echo, then be overwritten by the app on its next tick;
that's the program, not the write, at work.)

### Example — live heap + statics

The same commands the MCP tools shell out to (JSON on one line, wrapped here):

```
$ vmhook_cli list
[{"pid":15352,"image":"java.exe","cmdline":"…com.example.demo.ExampleApp"}]

$ vmhook_cli instances 15352 com/example/demo/ExampleApp$Worker 2
{"pid":15352,"class":"…$Worker","instances":[
  {"address":"0x60F44D7F8","fields":{"seq":"0","team":"\"team-0\"","id":"0",
   "ticks":"325","active":"false","inner":"<com/example/demo/ExampleApp$Inner>"}}, … ]}

$ vmhook_cli statics 15352 com/example/demo/ExampleApp
{"pid":15352,"class":"…","statics":{"APP_NAME":"\"vmhook demo\"","ratio":"1.500000",
  "tickCounter":"188","defaultColor":"<…$Color>","scores":"<java/util/HashMap>"}}
```

A `<internal/name>` value is an object reference; a `"…"` value is a String; the
rest are primitives.

## Setup

1. Build the viewer (which also builds `vmhook_cli.exe` + `vmhook_payload.dll`):
   ```bat
   cmake -S viewer -B build/viewer -G "Visual Studio 17 2022" -A x64
   cmake --build build/viewer --config Release
   ```
2. Register the server with your MCP client, pointing `VMHOOK_CLI` at the built
   exe (the payload DLL must sit next to it — it does after the build):
   ```json
   {
     "mcpServers": {
       "vmhook": {
         "command": "python",
         "args": ["C:\\repos\\cpp\\vmhook\\viewer\\mcp\\vmhook_mcp.py"],
         "env": { "VMHOOK_CLI": "C:\\repos\\cpp\\vmhook\\build\\viewer\\bin\\Release\\vmhook_cli.exe" }
       }
     }
   }
   ```
   (If `VMHOOK_CLI` is unset, the server looks for `build/viewer/bin/Release/vmhook_cli.exe`
   relative to the repo.)

## Notes

- x64 only (x64 payload ↔ x64 JVM). Some JVMs need matching/elevated privilege to
  inject — run the MCP client elevated if `enumerate_jvm` reports an OpenProcess error.
- The transport is line-delimited JSON-RPC 2.0 over stdio (`initialize`,
  `tools/list`, `tools/call`), protocol `2024-11-05`.

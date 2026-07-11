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

`kind` is one of `class` / `interface` / `enum` / `abstract` / `annotation` /
`record` (from the class-file access flags); `access` is the raw flag word.

Typical flow: `list_jvms` → pick a pid → `enumerate_jvm(pid)` → then
`list_classes` / `get_class` as much as you like (they read the cache; re-run
`enumerate_jvm` to refresh — re-attach is supported).

## Setup

1. Build the viewer (which also builds `vmhook_cli.exe` + `vmhook_payload.dll`):
   ```bat
   cmake -S viewer -B build-viewer -G "Visual Studio 17 2022" -A x64
   cmake --build build-viewer --config Release
   ```
2. Register the server with your MCP client, pointing `VMHOOK_CLI` at the built
   exe (the payload DLL must sit next to it — it does after the build):
   ```json
   {
     "mcpServers": {
       "vmhook": {
         "command": "python",
         "args": ["C:\\repos\\cpp\\vmhook\\viewer\\mcp\\vmhook_mcp.py"],
         "env": { "VMHOOK_CLI": "C:\\repos\\cpp\\vmhook\\build-viewer\\bin\\Release\\vmhook_cli.exe" }
       }
     }
   }
   ```
   (If `VMHOOK_CLI` is unset, the server looks for `build-viewer/bin/Release/vmhook_cli.exe`
   relative to the repo.)

## Notes

- x64 only (x64 payload ↔ x64 JVM). Some JVMs need matching/elevated privilege to
  inject — run the MCP client elevated if `enumerate_jvm` reports an OpenProcess error.
- The transport is line-delimited JSON-RPC 2.0 over stdio (`initialize`,
  `tools/list`, `tools/call`), protocol `2024-11-05`.

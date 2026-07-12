#!/usr/bin/env python3
"""
vmhook MCP server.

Exposes the vmhook viewer to Claude (or any MCP client) as tools: list running
JVMs, inject the vmhook payload + enumerate a JVM's classes/methods/fields, and
query that surface. It is a thin stdio JSON-RPC (MCP) wrapper around the
`vmhook_cli.exe` helper, with zero third-party dependencies (Python stdlib only).

Register it, e.g. in an MCP client config:

    {
      "mcpServers": {
        "vmhook": {
          "command": "python",
          "args": ["C:\\\\repos\\\\cpp\\\\vmhook\\\\viewer\\\\mcp\\\\vmhook_mcp.py"],
          "env": { "VMHOOK_CLI": "C:\\\\repos\\\\cpp\\\\vmhook\\\\build\\\\viewer\\\\bin\\\\Release\\\\vmhook_cli.exe" }
        }
      }
    }
"""

import json
import os
import shutil
import subprocess
import sys


def find_cli() -> str:
    env = os.environ.get("VMHOOK_CLI")
    if env and os.path.exists(env):
        return env
    here = os.path.dirname(os.path.abspath(__file__))
    for rel in (
        os.path.join(here, "..", "..", "build", "viewer", "bin", "Release", "vmhook_cli.exe"),
        os.path.join(here, "..", "..", "build", "viewer", "bin", "vmhook_cli.exe"),
        os.path.join(here, "vmhook_cli.exe"),
    ):
        if os.path.exists(rel):
            return os.path.abspath(rel)
    return shutil.which("vmhook_cli") or "vmhook_cli.exe"


CLI = find_cli()


def run_cli(args) -> str:
    try:
        r = subprocess.run([CLI, *args], capture_output=True, text=True, timeout=120)
        out = (r.stdout or "").strip()
        return out if out else json.dumps({"error": (r.stderr or "no output").strip()})
    except FileNotFoundError:
        return json.dumps({"error": f"vmhook_cli not found at '{CLI}'. Set the VMHOOK_CLI env var."})
    except Exception as exc:  # noqa: BLE001
        return json.dumps({"error": str(exc)})


TOOLS = [
    {
        "name": "list_jvms",
        "description": "List every running HotSpot JVM on this machine (pid, image name, command line). Use the pid with the other tools.",
        "inputSchema": {"type": "object", "properties": {}},
    },
    {
        "name": "enumerate_jvm",
        "description": (
            "Inject the vmhook payload into a JVM (by pid) and enumerate every loaded Java class with its "
            "methods and fields, using vmhook's pure-VM introspection (no JNI/JVMTI). Caches the result and "
            "returns counts. Call this once before list_classes / get_class for that pid."
        ),
        "inputSchema": {
            "type": "object",
            "properties": {"pid": {"type": "integer", "description": "JVM process id from list_jvms"}},
            "required": ["pid"],
        },
    },
    {
        "name": "list_classes",
        "description": "List loaded class internal names ('java/util/HashMap') for a JVM from its cached enumeration, optionally filtered by a case-insensitive substring.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "pid": {"type": "integer"},
                "filter": {"type": "string", "description": "optional case-insensitive substring"},
            },
            "required": ["pid"],
        },
    },
    {
        "name": "get_class",
        "description": (
            "Get one class (by internal '/'-separated name) from a JVM's cached enumeration: its "
            "superclass, 'kind' (class/interface/enum/abstract/annotation/record), raw class access "
            "flags, and every declared method and field. Each member has the raw JVM descriptor, a "
            "pretty signature/type, human-readable modifiers, and the numeric access flags."
        ),
        "inputSchema": {
            "type": "object",
            "properties": {
                "pid": {"type": "integer"},
                "class_name": {"type": "string", "description": "internal name, e.g. 'com/example/demo/ExampleApp'"},
            },
            "required": ["pid", "class_name"],
        },
    },
    {
        "name": "get_instances",
        "description": (
            "Scan a JVM's live heap for instances of one class (by internal '/'-separated name) and "
            "return each instance's address plus its field values (own + inherited), read live from the "
            "heap. Matches the exact class only (subclasses have their own class). A value like "
            "'<java/util/ArrayList>' is a reference field; '\"...\"' is a String. Injects the payload if "
            "needed, so no prior enumerate_jvm is required."
        ),
        "inputSchema": {
            "type": "object",
            "properties": {
                "pid": {"type": "integer"},
                "class_name": {"type": "string", "description": "internal name, e.g. 'com/example/demo/ExampleApp$Worker'"},
                "cap": {"type": "integer", "description": "max instances to scan (default 1000)"},
            },
            "required": ["pid", "class_name"],
        },
    },
    {
        "name": "get_statics",
        "description": (
            "Read a class's STATIC field values live from its java.lang.Class mirror (by internal "
            "'/'-separated name): the class-level state the instance tools don't show. Returns "
            "{pid, class, statics:{name:value, …}} where a '<internal/name>' value is a reference field "
            "and '\"...\"' is a String. Injects the payload if needed."
        ),
        "inputSchema": {
            "type": "object",
            "properties": {
                "pid": {"type": "integer"},
                "class_name": {"type": "string", "description": "internal name, e.g. 'com/example/demo/ExampleApp'"},
            },
            "required": ["pid", "class_name"],
        },
    },
    {
        "name": "search_members",
        "description": (
            "Search a JVM's cached enumeration for methods and/or fields whose name contains a "
            "case-insensitive substring, across every loaded class. Returns a list of "
            "{class, kind, name, descriptor, signature|type, static?}. Run enumerate_jvm first. "
            "Use this to locate a member (e.g. every 'update' method or 'health' field) without "
            "scanning class by class. Capped at 5000 hits."
        ),
        "inputSchema": {
            "type": "object",
            "properties": {
                "pid": {"type": "integer"},
                "query": {"type": "string", "description": "case-insensitive substring of the member name"},
                "scope": {"type": "string", "enum": ["all", "methods", "fields"], "description": "what to search (default 'all')"},
            },
            "required": ["pid", "query"],
        },
    },
    {
        "name": "watch_class_loads",
        "description": (
            "Arm vmhook's on_class_loaded hook (a detour on java.lang.ClassLoader.defineClass, "
            "deoptimised so it fires even when defineClass is JIT-compiled), wait `seconds`, then "
            "return every class DEFINED AT RUNTIME during that window: {pid, armed, seconds, loaded:[…]}. "
            "Event-driven (zero polling). Catches application / agent / custom-loader classes — bootstrap "
            "java.*/sun.* classes bypass the Java defineClass path and are NOT reported. Injects the "
            "payload if needed."
        ),
        "inputSchema": {
            "type": "object",
            "properties": {
                "pid": {"type": "integer"},
                "seconds": {"type": "integer", "description": "how long to watch (default 5)"},
            },
            "required": ["pid"],
        },
    },
    {
        "name": "set_instance_field",
        "description": (
            "WRITE a field on ONE live heap instance (mutates the running JVM). Identify the object by "
            "the heap 'address' from get_instances, then give the field name and new value. Primitives "
            "(int/long/float/double/boolean/char/byte/short) are parsed from the string and always work; "
            "reference fields take 'null' or a 0x<oop> address; a String field also accepts literal text, "
            "though building a NEW String is best-effort (the VM may refuse the allocation — you get "
            "{error}; 'null' always works). Returns {pid, class, field, value, ok:true} with the re-read "
            "value, or {error}. Injects the payload if needed."
        ),
        "inputSchema": {
            "type": "object",
            "properties": {
                "pid": {"type": "integer"},
                "class_name": {"type": "string", "description": "internal name of the instance's class, e.g. 'com/example/demo/ExampleApp$Worker'"},
                "address": {"type": "string", "description": "heap address of the instance (the 'address' from get_instances, e.g. '0x60F44D7F8')"},
                "field": {"type": "string", "description": "declared or inherited instance field name"},
                "value": {"type": "string", "description": "new value (primitive literal, String text, 'null', or 0x<oop>)"},
            },
            "required": ["pid", "class_name", "address", "field", "value"],
        },
    },
    {
        "name": "set_static_field",
        "description": (
            "WRITE a STATIC field on a class (mutates the running JVM's class-level state, on the "
            "java.lang.Class mirror). Value parsing matches set_instance_field: primitives always work, "
            "reference fields take 'null' or a 0x<oop> address, and a new String is built by allocating on "
            "an attached JavaThread. Returns {pid, class, field, value, ok:true} with the re-read value, or "
            "{error}. Injects the payload if needed."
        ),
        "inputSchema": {
            "type": "object",
            "properties": {
                "pid": {"type": "integer"},
                "class_name": {"type": "string", "description": "internal name, e.g. 'com/example/demo/ExampleApp'"},
                "field": {"type": "string", "description": "static field name"},
                "value": {"type": "string", "description": "new value (primitive literal, String text, 'null', or 0x<oop>)"},
            },
            "required": ["pid", "class_name", "field", "value"],
        },
    },
    {
        "name": "get_array",
        "description": (
            "Read the ELEMENTS of a Java array at a heap address. Give the array's '0x' address (from a "
            "call_method result whose 'refClass' is an array type like '[J', or a reference field's oop) and "
            "its element descriptor: a primitive ('I','J','F','D','Z','B','C','S') or 'Ljava/lang/String;' / "
            "'L<class>;' for an object array (the array's own descriptor with one leading '[' removed). "
            "Returns {address, length, elements:[...]} — the first `max` (default 4096) elements formatted "
            "like field values (a '<class>' element is an object, '\"...\"' a String). Injects if needed."
        ),
        "inputSchema": {
            "type": "object",
            "properties": {
                "pid": {"type": "integer"},
                "address": {"type": "string", "description": "the array's heap address, e.g. '0x60F4DE5A8'"},
                "element_type": {"type": "string", "description": "element descriptor, e.g. 'J', 'I', 'Ljava/lang/String;'"},
                "max": {"type": "integer", "description": "max elements to read (default 4096)"},
            },
            "required": ["pid", "address", "element_type"],
        },
    },
    {
        "name": "set_array_element",
        "description": (
            "WRITE one element of a Java array (mutates the running JVM). Give the array's '0x' address, "
            "its element descriptor (see get_array), the index, and the new value: a primitive literal, "
            "or for an object array 'null' / a 0x<oop> address / (for a String[]) the literal text. Returns "
            "{address, index, value, ok:true} with the re-read element, or {error}. Injects if needed."
        ),
        "inputSchema": {
            "type": "object",
            "properties": {
                "pid": {"type": "integer"},
                "address": {"type": "string"},
                "element_type": {"type": "string", "description": "element descriptor, e.g. 'J', 'Ljava/lang/String;'"},
                "index": {"type": "integer"},
                "value": {"type": "string"},
            },
            "required": ["pid", "address", "element_type", "index", "value"],
        },
    },
    {
        "name": "call_method",
        "description": (
            "INVOKE a Java method in the running JVM and return its result. For an instance method, give "
            "the receiver's heap 'address' from get_instances; for a static method, pass '-' as address. "
            "The descriptor is the raw JVM signature (e.g. '(ID)D', '()Ljava/lang/String;'). Each argument "
            "is a tagged token: a bare literal for a primitive ('42', '3.14', 'true', 'A'); '@null' for a "
            "null reference; '@0x<oop>' for an existing object (an address from get_instances / a reference "
            "field / a previous call result); or '#<text>' to allocate a new java.lang.String. Returns "
            "{result, kind, ref, refClass, ok:true} where 'kind' is void/bool/int/long/float/double/char/"
            "string/ref and — for an object/String result — 'ref' is the returned object's 0x address "
            "(reuse it as a @0x argument or with set_*_field). Runs the call inside a real Java thread "
            "context, so it works on every JDK. Injects the payload if needed."
        ),
        "inputSchema": {
            "type": "object",
            "properties": {
                "pid": {"type": "integer"},
                "class_name": {"type": "string", "description": "internal name of the class declaring the method, e.g. 'com/example/demo/ExampleApp$Worker'"},
                "address": {"type": "string", "description": "receiver heap address for an instance method, or '-' for a static method"},
                "method": {"type": "string", "description": "method name, e.g. 'compute'"},
                "descriptor": {"type": "string", "description": "raw JVM method descriptor, e.g. '(ID)D'"},
                "args": {"type": "array", "items": {"type": "string"}, "description": "tagged argument tokens (see the description); omit or [] for a no-arg method"},
            },
            "required": ["pid", "class_name", "address", "method", "descriptor"],
        },
    },
    {
        "name": "freeze_field",
        "description": (
            "FREEZE a field at a value: a background thread in the payload re-writes it ~50x/second so it "
            "holds against the program's own writes, until unfrozen (like a memory trainer). scope 'I' is "
            "an instance field (give the heap 'address'); 'S' is a static field (address ignored, pass '-'). "
            "The value follows set_*_field rules. Returns {pid, class, field, value, frozen:true}. NOTE: an "
            "instance freeze pins a raw heap address, so a relocating GC can leave it stale (best-effort); "
            "static freezes re-resolve the mirror each write. Unfreeze with unfreeze_field. Injects the "
            "payload if needed."
        ),
        "inputSchema": {
            "type": "object",
            "properties": {
                "pid": {"type": "integer"},
                "scope": {"type": "string", "enum": ["I", "S"], "description": "'I' instance field, 'S' static field"},
                "class_name": {"type": "string", "description": "internal name of the class"},
                "address": {"type": "string", "description": "instance heap address (scope 'I'), or '-' for a static field"},
                "field": {"type": "string", "description": "field name"},
                "value": {"type": "string", "description": "value to hold (primitive literal, String text, 'null', or 0x<oop>)"},
            },
            "required": ["pid", "scope", "class_name", "address", "field", "value"],
        },
    },
    {
        "name": "unfreeze_field",
        "description": (
            "Release a freeze created by freeze_field. Pass the exact key '<I|S>|Class|addr|field' "
            "(the internal class name; addr only for an instance freeze — e.g. "
            "'I|com/example/demo/ExampleApp$Worker|0x60F451660|ticks' or 'S|com/example/demo/ExampleApp||tickCounter'), "
            "or '*' to release every freeze. Returns {pid, unfroze, ok:true}."
        ),
        "inputSchema": {
            "type": "object",
            "properties": {
                "pid": {"type": "integer"},
                "key": {"type": "string", "description": "'<I|S>|Class|addr|field' or '*' for all"},
            },
            "required": ["pid", "key"],
        },
    },
]


def call_tool(name: str, args: dict) -> str:
    if name == "list_jvms":
        return run_cli(["list"])
    if name == "enumerate_jvm":
        return run_cli(["enumerate", str(args["pid"])])
    if name == "list_classes":
        cli_args = ["classes", str(args["pid"])]
        if args.get("filter"):
            cli_args.append(str(args["filter"]))
        return run_cli(cli_args)
    if name == "get_class":
        return run_cli(["class", str(args["pid"]), str(args["class_name"])])
    if name == "get_instances":
        cli_args = ["instances", str(args["pid"]), str(args["class_name"])]
        if args.get("cap"):
            cli_args.append(str(args["cap"]))
        return run_cli(cli_args)
    if name == "get_statics":
        return run_cli(["statics", str(args["pid"]), str(args["class_name"])])
    if name == "search_members":
        cli_args = ["search", str(args["pid"]), str(args["query"])]
        if args.get("scope"):
            cli_args.append(str(args["scope"]))
        return run_cli(cli_args)
    if name == "watch_class_loads":
        cli_args = ["watch", str(args["pid"])]
        if args.get("seconds"):
            cli_args.append(str(args["seconds"]))
        return run_cli(cli_args)
    if name == "set_instance_field":
        return run_cli(["set-instance", str(args["pid"]), str(args["class_name"]),
                        str(args["address"]), str(args["field"]), str(args["value"])])
    if name == "set_static_field":
        return run_cli(["set-static", str(args["pid"]), str(args["class_name"]),
                        str(args["field"]), str(args["value"])])
    if name == "get_array":
        cli_args = ["array", str(args["pid"]), str(args["address"]), str(args["element_type"])]
        if args.get("max"):
            cli_args.append(str(args["max"]))
        return run_cli(cli_args)
    if name == "set_array_element":
        return run_cli(["array-set", str(args["pid"]), str(args["address"]), str(args["element_type"]),
                        str(args["index"]), str(args["value"])])
    if name == "call_method":
        cli_args = ["call", str(args["pid"]), str(args["class_name"]), str(args["address"]),
                    str(args["method"]), str(args["descriptor"])]
        for a in args.get("args", []) or []:
            cli_args.append(str(a))
        return run_cli(cli_args)
    if name == "freeze_field":
        return run_cli(["freeze", str(args["pid"]), str(args["scope"]), str(args["class_name"]),
                        str(args["address"]), str(args["field"]), str(args["value"])])
    if name == "unfreeze_field":
        return run_cli(["unfreeze", str(args["pid"]), str(args["key"])])
    return json.dumps({"error": f"unknown tool: {name}"})


def handle(msg: dict):
    mid = msg.get("id")
    method = msg.get("method")

    if method == "initialize":
        return {
            "jsonrpc": "2.0", "id": mid,
            "result": {
                "protocolVersion": "2024-11-05",
                "capabilities": {"tools": {}},
                "serverInfo": {"name": "vmhook", "version": "0.1.0"},
            },
        }
    if method in ("notifications/initialized", "initialized"):
        return None
    if method == "ping":
        return {"jsonrpc": "2.0", "id": mid, "result": {}}
    if method == "tools/list":
        return {"jsonrpc": "2.0", "id": mid, "result": {"tools": TOOLS}}
    if method == "tools/call":
        params = msg.get("params", {}) or {}
        name = params.get("name", "")
        args = params.get("arguments", {}) or {}
        try:
            text = call_tool(name, args)
        except KeyError as ke:
            text = json.dumps({"error": f"missing argument: {ke}"})
        except Exception as exc:  # never let one bad tool call kill the server
            text = json.dumps({"error": f"{type(exc).__name__}: {exc}"})
        return {"jsonrpc": "2.0", "id": mid, "result": {"content": [{"type": "text", "text": text}]}}

    if mid is not None:
        return {"jsonrpc": "2.0", "id": mid, "error": {"code": -32601, "message": f"method not found: {method}"}}
    return None


def main() -> None:
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            msg = json.loads(line)
        except json.JSONDecodeError:
            continue
        try:
            resp = handle(msg)
        except Exception as exc:  # a malformed request must not kill the loop
            mid = msg.get("id") if isinstance(msg, dict) else None
            resp = ({"jsonrpc": "2.0", "id": mid, "error": {"code": -32603, "message": str(exc)}}
                    if mid is not None else None)
        if resp is not None:
            sys.stdout.write(json.dumps(resp) + "\n")
            sys.stdout.flush()


if __name__ == "__main__":
    main()

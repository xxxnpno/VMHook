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
          "env": { "VMHOOK_CLI": "C:\\\\repos\\\\cpp\\\\vmhook\\\\build-viewer\\\\bin\\\\Release\\\\vmhook_cli.exe" }
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
        os.path.join(here, "..", "..", "build-viewer", "bin", "Release", "vmhook_cli.exe"),
        os.path.join(here, "..", "..", "build-viewer", "bin", "vmhook_cli.exe"),
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

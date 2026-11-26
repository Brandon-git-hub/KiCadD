"""End-to-end test of kicadd-mcp with the official MCP Python SDK, plus raw HTTP edge cases.

usage: python -I mcp_e2e.py <kicadd-mcp binary> <daemon socket> <http port>

Starts kicadd-mcp itself (HTTP on 127.0.0.1:<port>, a second instance with a token on
<port>+1, and one over stdio). The daemon (kicad-cli api-server) must already be running.
"""

import asyncio
import json
import os
import re
import subprocess
import sys
import time
import urllib.error
import urllib.request

from mcp import ClientSession, StdioServerParameters
from mcp.client.client import Client
from mcp.client.stdio import stdio_client

FAILS = []


def check(cond, label):
    print(("  ok   " if cond else "  FAIL ") + label)
    if not cond:
        FAILS.append(label)


def text_of(result):
    return "".join(c.text for c in result.content if getattr(c, "type", "") == "text")


def raw(url, method="POST", body=None, headers=None):
    req = urllib.request.Request(url, method=method, data=None if body is None else json.dumps(body).encode(),
                                 headers={"Content-Type": "application/json",
                                          "Accept": "application/json, text/event-stream", **(headers or {})})
    try:
        with urllib.request.urlopen(req, timeout=30) as r:
            return r.status, dict(r.headers), r.read().decode()
    except urllib.error.HTTPError as e:
        return e.code, dict(e.headers), e.read().decode()


async def exercise(call, label):
    """Run the tool sequence through `call(name, args) -> CallToolResult`."""
    t0 = time.perf_counter()
    r = await call("board_summary", {})
    check(not r.is_error and "footprints=" in text_of(r), f"{label}: board_summary -> {text_of(r).strip()[:70]}")

    r = await call("query_area", {"ref": "U78", "radius": 2, "kinds": ["footprint", "pad"]})
    txt = text_of(r)
    check(not r.is_error and "PAD U78." in txt, f"{label}: query_area U78 r=2mm ({len(txt)} chars)")

    r = await call("query_net", {"net": "/SoM_IO/UART3_DEBUG.TX"})
    check(not r.is_error and "items" in text_of(r), f"{label}: query_net ({len(text_of(r))} chars)")

    timings = []
    for dx in (0.5, -0.5):
        s = time.perf_counter()
        r = await call("move_footprints", {"refs": ["U78"], "dx": dx})
        timings.append((time.perf_counter() - s) * 1000)
        check(not r.is_error and "U78 @" in text_of(r) and "drc:" in text_of(r),
              f"{label}: move_footprints U78 dx={dx} ({timings[-1]:.1f} ms end-to-end)")
    print("       " + text_of(r).replace("\n", "\n       ").rstrip())

    r = await call("move_footprints", {"refs": ["C276"], "dx": 0.3})
    check("excl. deferred nets: GND" in text_of(r), f"{label}: large-net deferral is reported")
    await call("move_footprints", {"refs": ["C276"], "dx": -0.3})

    r = await call("run_drc", {"local": True, "refs": ["U78", "NOPE"]})
    check(not r.is_error and "not found: NOPE" in text_of(r), f"{label}: run_drc local reports unknown refs")

    r = await call("board_summary", {})
    check(not r.is_error and "\nratsnest: " in text_of(r) and "unconnected=" in text_of(r),
          f"{label}: board_summary's ratsnest -> {text_of(r).strip().splitlines()[-1]}")

    r = await call("move_footprints", {"refs": ["U78"]})
    check(r.is_error, f"{label}: move without delta is a tool error (isError)")

    r = await call("query_area", {"ref": "U78", "radius": 1, "kinds": ["bogus"]})
    check(r.is_error and "kinds[0]: one of" in text_of(r) and "bogus" in text_of(r), f"{label}: bad enum -> isError with message")
    print(f"       total {label} sequence: {(time.perf_counter() - t0) * 1000:.0f} ms")


async def test_http_sdk(url):
    print("\n[HTTP, official SDK Client, mode=auto (discover probe -> legacy fallback)]")
    async with Client(url) as client:
        tools = await client.list_tools()
        props = {t.name: t.input_schema.get("properties", {}) for t in tools.tools}
        descr = lambda tool, arg: props.get(tool, {}).get(arg, {}).get("description", "")
        check(all(descr(t, "nets").startswith("Net names") for t in ("route_short", "fanout", "teardrops", "delete_tracks", "zones"))
              and descr("zones", "layers").startswith("Layer names") and descr("zones", "ids").startswith("Zone ids")
              and any(descr(t, "refs").startswith("Reference designators") for t in props),
              "nets / layers / ids described as what they are (not reference designators)")
        names = sorted(t.name for t in tools.tools)
        check(len(names) == 60 and all(n in names for n in ("board_transform", "pcb_text", "export", "project", "undo", "launch_kicad", "close_kicad", "simulate", "tool_help", "pdn", "reflection", "impedance", "sch_annotate", "silk_tidy", "board_diff", "sch_render", "stitch_vias", "teardrops", "board_render", "lcsc_match", "board_outline", "board_setup", "route", "route_short", "fanout", "sch_set_fields", "pcb_set_fields", "set_rules", "zones", "batch")) and all(n in names for n in ("delete_tracks", "sch_move", "sch_wire", "run_erc", "lib_register", "audit", "lcsc_search", "lib_create_symbol", "lib_create_footprint")) and "lcsc_fetch" in names and "sch_connect" in names and "auto_place" in names and "sch_add_sheet" in names, f"tools/list -> {names}")
        check(all(t.input_schema.get("type") == "object" for t in tools.tools), "every inputSchema is an object")
        await exercise(client.call_tool, "http")


async def test_stdio_sdk(binary, sock):
    print("\n[stdio, official SDK ClientSession]")
    params = StdioServerParameters(command=binary, args=["--socket", sock, "--stdio"])
    with open(os.devnull, "w") as devnull:
        async with stdio_client(params, errlog=devnull) as (read, write):
            async with ClientSession(read, write) as session:
                init = await session.initialize()
                check(init.protocol_version == "2025-11-25", f"negotiated protocol {init.protocol_version}")
                check(init.server_info.name == "kicadd", "serverInfo.name == kicadd")
                out = subprocess.run([binary, "--version"], capture_output=True, text=True, timeout=10).stdout.strip()
                ver = out.removeprefix("kicadd-mcp ")
                check(out.startswith("kicadd-mcp ") and ver and init.server_info.version.startswith(ver + "+t"),
                      f"--version '{out}' is serverInfo.version's release ({init.server_info.version})")
                await exercise(session.call_tool, "stdio")
                await resources_prompts(session, init)


async def resources_prompts(session, init):
    """phase 5 item 2: the skill and the tools' descriptions as resources, three prompts"""
    print("\n[resources and prompts]")
    caps = init.capabilities
    check(caps.resources is not None and caps.prompts is not None, "resources and prompts announced")
    res = (await session.list_resources()).resources
    uris = [str(r.uri) for r in res]
    sections = [u for u in uris if u.startswith("kicadd://skill/")]
    check("kicadd://skill" in uris and len(sections) >= 15 and "kicadd://skill/review-a-design" in sections
          and {"kicadd://tools", "kicadd://board", "kicadd://schematic"} <= set(uris),
          f"{len(uris)} resources: the skill, {len(sections)} of its sections, the tools, the board, the schematic")
    whole = (await session.read_resource("kicadd://skill")).contents[0].text
    one = (await session.read_resource("kicadd://skill/review-a-design")).contents[0].text
    check(whole.startswith("# Working with KiCadD") and "---" not in whole[:5] and len(whole) > 30000,
          f"the skill without its front matter ({len(whole)} chars)")
    check(one.startswith("## Review a design") and one in whole and len(one) < len(whole) / 3,
          f"one section alone ({len(one)} chars)")
    tools_md = (await session.read_resource("kicadd://tools")).contents[0].text
    check("## board_render" in tools_md and "## export" in tools_md and "## Old names" not in tools_md, "every tool's full description")
    tmpl = [t.uri_template for t in (await session.list_resource_templates()).resource_templates]
    check("kicadd://tool/{name}" in tmpl and "kicadd://skill/{section}" in tmpl, "templates: a tool, a section")
    one_tool = (await session.read_resource("kicadd://tool/auto_place")).contents[0].text
    check("auto_place" in one_tool and "back_decaps" in one_tool, "kicadd://tool/auto_place: its full description")
    board = (await session.read_resource("kicadd://board")).contents[0].text
    check(board.startswith("file=") and "footprints=" in board, "kicadd://board: " + board.strip()[:60])
    try:
        await session.read_resource("kicadd://nope")
        check(False, "an unknown resource is an error")
    except Exception as e:
        check("no resource" in str(e), "an unknown resource is an error")
    prompts = {p.name: p for p in (await session.list_prompts()).prompts}
    check(set(prompts) == {"review_design", "new_board", "prepare_fab"}, f"prompts: {sorted(prompts)}")
    r = await session.get_prompt("review_design", {"focus": "USB"})
    text = r.messages[0].content.text
    check("kicadd://skill/review-a-design" in text and text.endswith("Focus on: USB"), "review_design with a focus")
    r = await session.get_prompt("new_board", {"spec": "a 2-layer USB-C to UART board, 30 x 20 mm"})
    text = r.messages[0].content.text
    named = re.findall(r"kicadd://skill/[a-z0-9-]+", text)
    for other, args in (("review_design", {}), ("prepare_fab", {})):
        named += re.findall(r"kicadd://skill/[a-z0-9-]+", (await session.get_prompt(other, args)).messages[0].content.text)
    check(len(named) >= 11 and all(u in sections for u in named), f"the prompts name {len(named)} skill sections, all real")
    try:
        await session.get_prompt("new_board", {})
        check(False, "new_board without spec is refused")
    except Exception as e:
        check("required" in str(e), "new_board without spec is refused")


def test_http_raw(url, token_url):
    print("\n[HTTP edge cases, raw]")
    init = {"jsonrpc": "2.0", "id": 1, "method": "initialize",
            "params": {"protocolVersion": "2025-06-18", "capabilities": {},
                       "clientInfo": {"name": "raw", "version": "0"}}}
    st, hd, body = raw(url, body=init)
    sid = {k.lower(): v for k, v in hd.items()}.get("mcp-session-id")
    check(st == 200 and json.loads(body)["result"]["protocolVersion"] == "2025-06-18" and sid,
          "initialize echoes a supported older version and sets Mcp-Session-Id")
    st, _, body = raw(url, body={**init, "params": {**init["params"], "protocolVersion": "1999-01-01"}})
    check(json.loads(body)["result"]["protocolVersion"] == "2025-11-25", "unknown version -> server's latest")
    st, _, body = raw(url, body={"jsonrpc": "2.0", "method": "notifications/initialized"})
    check(st == 202 and body == "", "notification -> 202, empty body")
    st, _, body = raw(url, body={"jsonrpc": "2.0", "id": 2, "method": "server/discover"})
    check(json.loads(body)["error"]["code"] == -32601, "server/discover -> -32601 (legacy server)")
    st, _, body = raw(url, body={"jsonrpc": "2.0", "id": 3, "method": "tools/call",
                                 "params": {"name": "nope", "arguments": {}}})
    check(json.loads(body)["error"]["code"] == -32602, "unknown tool -> -32602")
    st, _, _ = raw(url, method="GET")
    check(st == 405, "GET -> 405 (no server-initiated stream)")
    st, _, _ = raw(url, body=init, headers={"Origin": "http://evil.example"})
    check(st == 403, "foreign Origin -> 403")
    st, _, _ = raw(url, body=init, headers={"Origin": "http://localhost:3000"})
    check(st == 200, "localhost Origin -> 200")
    st, _, body = raw(url.replace("/mcp", "/other"), body=init)
    check(st == 404, "other path -> 404")
    req = urllib.request.Request(url, method="POST", data=b"{not json", headers={"Content-Type": "application/json"})
    try:
        urllib.request.urlopen(req, timeout=10)
        st = 200
    except urllib.error.HTTPError as e:
        st, body = e.code, e.read().decode()
    check(st == 400 and json.loads(body)["error"]["code"] == -32700, "malformed JSON -> 400 / -32700")

    st, _, _ = raw(token_url, body=init)
    check(st == 401, "token server: no Authorization -> 401")
    st, _, _ = raw(token_url, body=init, headers={"Authorization": "Bearer wrong"})
    check(st == 401, "token server: wrong token -> 401")
    st, _, _ = raw(token_url, body=init, headers={"Authorization": "Bearer s3cret"})
    check(st == 200, "token server: right token -> 200")


def main():
    binary, sock, port = sys.argv[1], sys.argv[2], int(sys.argv[3])
    url = f"http://127.0.0.1:{port}/mcp"
    token_url = f"http://127.0.0.1:{port + 1}/mcp"
    procs = [
        subprocess.Popen([binary, "--socket", sock, "--http", str(port)], stderr=subprocess.DEVNULL),
        subprocess.Popen([binary, "--socket", sock, "--http", str(port + 1), "--token", "s3cret"],
                         stderr=subprocess.DEVNULL),
    ]
    try:
        time.sleep(0.5)
        refused = subprocess.run([binary, "--socket", sock, "--http", f"0.0.0.0:{port + 2}"],
                                 capture_output=True, text=True, timeout=10)
        print("[startup]")
        check(refused.returncode == 2 and "without --token" in refused.stderr,
              "non-loopback bind without --token is refused")
        test_http_raw(url, token_url)
        asyncio.run(test_http_sdk(url))
        asyncio.run(test_stdio_sdk(binary, sock))
    finally:
        for p in procs:
            p.terminate()
    print("\nRESULT:", "PASS" if not FAILS else f"FAIL ({len(FAILS)})")
    sys.exit(1 if FAILS else 0)


if __name__ == "__main__":
    main()

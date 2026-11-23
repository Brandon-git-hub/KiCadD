"""End-to-end test of the 2D view channel: two browsers (WebSocket clients) + an MCP agent.

usage: python -I web_e2e.py <base-url, e.g. http://127.0.0.1:8765> [SOCKET KICADD_MCP]

With SOCKET and KICADD_MCP: an edit from another kicadd-mcp (not this view's) reaches the
browsers through KiCad's change notifications ("<socket>.ev"), well before the 1 s fallback poll.

Checks snapshot delivery, browser->server moves, fan-out of deltas to the other browser,
agent (MCP) edits reaching browsers, latency, and WebSocket origin/path checks.
"""

import asyncio
import subprocess
import json
import sys
import time
import urllib.request

import websockets

FAILS = []


def check(cond, label):
    print(("  ok   " if cond else "  FAIL ") + label)
    if not cond:
        FAILS.append(label)


async def recv_until(ws, pred, timeout=10.0):
    end = time.perf_counter() + timeout
    while True:
        left = end - time.perf_counter()
        if left <= 0:
            return None
        try:
            msg = json.loads(await asyncio.wait_for(ws.recv(), left))
        except asyncio.TimeoutError:
            return None
        if pred(msg):
            return msg


def mcp_call(base, name, args):
    body = json.dumps({"jsonrpc": "2.0", "id": 1, "method": "tools/call",
                       "params": {"name": name, "arguments": args}}).encode()
    req = urllib.request.Request(base + "/mcp", data=body, headers={"Content-Type": "application/json"})
    return json.load(urllib.request.urlopen(req))["result"]["content"][0]["text"]


def fp_by_ref(msg, ref):
    return next((f for f in msg.get("fp", []) if f["ref"] == ref), None)


async def main(base):
    ws_url = base.replace("http", "ws", 1) + "/ws"

    print("[page]")
    html = urllib.request.urlopen(base + "/").read().decode()
    check("<canvas" in html and "KiCadD" in html, f"GET / serves the embedded view ({len(html)} bytes)")

    print("[snapshot]")
    t0 = time.perf_counter()
    a = await websockets.connect(ws_url, max_size=None)
    snap_a = await recv_until(a, lambda m: m["t"] == "snapshot")
    dt = (time.perf_counter() - t0) * 1000
    size = len(json.dumps(snap_a)) if snap_a else 0
    check(snap_a and len(snap_a["fp"]) > 1000 and len(snap_a["trk"]) > 20000,
          f"browser A snapshot: {len(snap_a['fp'])} fp, {len(snap_a['trk'])} tracks, "
          f"{size / 1e6:.2f} MB JSON, {dt:.0f} ms")
    b = await websockets.connect(ws_url, max_size=None)
    snap_b = await recv_until(b, lambda m: m["t"] == "snapshot")
    check(snap_b and snap_b["rev"] >= snap_a["rev"], "browser B snapshot")

    u78 = fp_by_ref(snap_a, "U78")
    x0 = u78["x"]

    print("[status]")
    st = await recv_until(b, lambda m: m["t"] == "status" and m["viewers"] == 2, 3)
    check(st and st["push"], f"status: KiCad pushes its changes, 2 viewers ({st})")

    if len(sys.argv) > 3:
        print("[another agent edits: KiCad's notification, not the poll]")
        sock, mcp = sys.argv[2], sys.argv[3]
        lat = []
        for dx in (0.5, -0.5):
            def other():
                init = {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {
                    "protocolVersion": "2025-06-18", "capabilities": {}, "clientInfo": {"name": "other", "version": "1"}}}
                call = {"jsonrpc": "2.0", "id": 2, "method": "tools/call",
                        "params": {"name": "move_footprints", "arguments": {"refs": ["U78"], "dx": dx}}}
                p = subprocess.Popen([mcp, "--socket", sock, "--stdio"], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
                p.stdin.write(json.dumps(init) + "\n"); p.stdin.flush(); p.stdout.readline()
                p.stdin.write(json.dumps(call) + "\n"); p.stdin.flush(); p.stdout.readline()
                done = time.perf_counter()
                p.stdin.close(); p.wait(10)
                return done
            want = x0 + (500 if dx > 0 else 0)
            done = await asyncio.to_thread(other)
            d = await recv_until(a, lambda m: m["t"] == "delta" and fp_by_ref(m, "U78") and fp_by_ref(m, "U78")["x"] == want, 5)
            lat.append((time.perf_counter() - done) * 1000)
            check(d is not None, f"the other agent's move of U78 reaches browser A")
        # the reply may come before or after the delta (the view's poll is on another thread):
        # well under the 1 s fallback either way
        check(max(lat) < 400, "... at once: " + ", ".join(f"{v:.0f} ms" for v in lat) + " after its reply")

    print("[browser A moves U78 by +0.5 mm]")
    t0 = time.perf_counter()
    await a.send(json.dumps({"t": "move", "refs": ["U78"], "dx": 0.5, "dy": 0, "rot": 0}))
    moved = await recv_until(a, lambda m: m["t"] == "moved")
    t_ack = (time.perf_counter() - t0) * 1000
    check(moved and moved["ok"], f"A gets 'moved' ack in {t_ack:.1f} ms (server {moved['ms']} ms), "
                                 f"drc={len(moved['drc'])}, pending={moved['pending']}")
    delta_b = await recv_until(b, lambda m: m["t"] == "delta" and fp_by_ref(m, "U78"))
    t_b = (time.perf_counter() - t0) * 1000
    f = fp_by_ref(delta_b, "U78") if delta_b else None
    check(f and f["x"] == x0 + 500, f"B receives the delta in {t_b:.1f} ms; U78.x {x0} -> {f and f['x']} µm")
    # One footprint: pads, courtyard and its silk / fab graphics (stroke text is most of it).
    check(delta_b and len(json.dumps(delta_b)) < 16000, f"delta is small ({len(json.dumps(delta_b))} bytes)")
    delta_a = await recv_until(a, lambda m: m["t"] == "delta" and fp_by_ref(m, "U78"))
    check(delta_a is not None, "A also receives the authoritative delta")

    print("[agent moves U78 back via MCP]")
    t0 = time.perf_counter()
    text = await asyncio.to_thread(mcp_call, base, "move_footprints", {"refs": ["U78"], "dx": -0.5})
    t_mcp = (time.perf_counter() - t0) * 1000
    d_a = await recv_until(a, lambda m: m["t"] == "delta" and fp_by_ref(m, "U78"))
    t_push = (time.perf_counter() - t0) * 1000
    check(d_a and fp_by_ref(d_a, "U78")["x"] == x0,
          f"MCP move ({t_mcp:.1f} ms) reaches browser A in {t_push:.1f} ms; U78 back at {x0}")
    d_b = await recv_until(b, lambda m: m["t"] == "delta" and fp_by_ref(m, "U78"))
    check(d_b is not None, "... and browser B")
    check("U78 @" in text, "MCP tool output unchanged")

    print("[agent activity and pause from the browser]")
    cp = await recv_until(b, lambda m: m["t"] == "copilot" and any("U78" in e[2] for e in m["events"]), 5)
    check(cp and not cp["paused"] and cp["edits"] >= 1, f"browser B sees the agent's edit of U78 ({cp and cp['events'][-1]})")
    check(cp and cp["last_type"] not in ("GetHistory", "GetRatsnest", "GetChanges", ""),
          f"the agent's last request is its own, not the view's reads ({cp and cp['last_type']})")
    await b.send(json.dumps({"t": "pause", "paused": True}))
    cp = await recv_until(a, lambda m: m["t"] == "copilot" and m["paused"], 5)
    check(cp and "a web viewer paused" in cp["events"][-1][2], "B pauses the agent: browser A is told (" + (cp["events"][-1][2] if cp else "-") + ")")
    text = await asyncio.to_thread(mcp_call, base, "move_footprints", {"refs": ["U78"], "dx": 0.5})
    check("paused" in text, "the agent's move is refused: " + text.strip()[:70])
    cp = await recv_until(a, lambda m: m["t"] == "copilot" and m["events"][-1][1] == 1, 5)
    check(cp is not None, "... and the refusal shows in the browsers")
    await a.send(json.dumps({"t": "pause", "paused": False}))
    cp = await recv_until(b, lambda m: m["t"] == "copilot" and not m["paused"], 5)
    check(cp is not None, "A resumes it")
    text = await asyncio.to_thread(mcp_call, base, "board_summary", {})
    check("paused" not in text, "board_summary: no pause note")

    print("[rotation + DRC payload]")
    await a.send(json.dumps({"t": "move", "refs": ["C216"], "dx": -1.0, "dy": 0}))
    m = await recv_until(a, lambda m: m["t"] == "moved")
    check(m and len(m["drc"]) > 0 and {"type", "a", "b", "x", "y"} <= set(m["drc"][0]),
          f"violating move returns {len(m['drc'])} structured DRC items")
    await a.send(json.dumps({"t": "move", "refs": ["C216"], "dx": 1.0, "dy": 0}))
    m = await recv_until(a, lambda m: m["t"] == "moved")
    check(m and not m["drc"], "moving back: DRC clean")
    async def drain(ws, quiet=0.3):
        """Consume everything queued; return the last C216 rotation seen in a delta."""
        last = None
        while True:
            m = await recv_until(ws, lambda m: True, timeout=quiet)
            if m is None:
                return last
            f = fp_by_ref(m, "C216") if m["t"] == "delta" else None
            if f:
                last = f["r"]

    r0 = await drain(b)
    await drain(a)
    await a.send(json.dumps({"t": "move", "refs": ["C216"], "rot": 90}))
    await recv_until(a, lambda m: m["t"] == "moved")
    r1 = await drain(b)
    await a.send(json.dumps({"t": "move", "refs": ["C216"], "rot": -90}))
    await recv_until(a, lambda m: m["t"] == "moved")
    r2 = await drain(b)
    check(r1 is not None and r2 is not None and r1 != r2 and r2 == r0,
          f"rotation propagates to B ({r0} -> {r1} -> {r2})")
    await drain(a)

    print("[errors]")
    await a.send(json.dumps({"t": "move", "refs": ["NOPE"], "dx": 1}))
    m = await recv_until(a, lambda m: m["t"] == "moved")
    check(m and not m["ok"], "unknown ref -> moved.ok=false")
    await a.send("not json")
    m = await recv_until(a, lambda m: m["t"] == "error")
    check(m is not None, "malformed message -> error frame, connection stays open")
    await a.send(json.dumps({"t": "resync"}))
    m = await recv_until(a, lambda m: m["t"] == "snapshot")
    check(m is not None, "resync -> fresh snapshot")

    try:
        bad = await websockets.connect(ws_url, origin="http://evil.example", max_size=None)
        await bad.close()
        check(False, "foreign Origin rejected")
    except Exception as e:
        check("403" in str(e), f"foreign Origin rejected ({e.__class__.__name__}: 403)")
    try:
        bad = await websockets.connect(base.replace("http", "ws", 1) + "/other")
        await bad.close()
        check(False, "upgrade on other path rejected")
    except Exception as e:
        check("404" in str(e), "upgrade on other path -> 404")

    await a.close()
    await b.close()
    print("\nRESULT:", "PASS" if not FAILS else f"FAIL ({len(FAILS)})")
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main(sys.argv[1].rstrip("/"))))

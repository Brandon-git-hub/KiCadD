"""Undo / redo / save end to end: browser (WebSocket) + agent (MCP), then reload from disk.

usage: python -I history_e2e.py <base-url> <restart-cmd...>

The daemon must be serving a disposable copy of a board: this test saves it. After saving it
runs <restart-cmd> (which must restart daemon+MCP on the same port from the saved file) and
checks the saved positions.
"""

import asyncio
import json
import subprocess
import sys
import time
import urllib.request

import websockets

FAILS = []


def check(cond, label):
    print(("  ok   " if cond else "  FAIL ") + label)
    if not cond:
        FAILS.append(label)


def mcp(base, name, args=None):
    body = json.dumps({"jsonrpc": "2.0", "id": 1, "method": "tools/call",
                       "params": {"name": name, "arguments": args or {}}}).encode()
    req = urllib.request.Request(base + "/mcp", data=body, headers={"Content-Type": "application/json"})
    r = json.load(urllib.request.urlopen(req))["result"]
    return r["content"][0]["text"], r["isError"]


class View:
    """Minimal browser stand-in: applies snapshot/deltas, remembers the latest history."""

    def __init__(self, ws):
        self.ws, self.fp, self.rev, self.history, self.inbox = ws, {}, 0, None, []

    async def pump(self, until, timeout=10):
        end = time.perf_counter() + timeout
        while time.perf_counter() < end:
            try:
                m = json.loads(await asyncio.wait_for(self.ws.recv(), end - time.perf_counter()))
            except asyncio.TimeoutError:
                break
            if m["t"] == "snapshot":
                self.fp = {f["ref"]: f for f in m["fp"]}
                self.rev = m["rev"]
            elif m["t"] == "delta" and m["rev"] > self.rev:
                for f in m["fp"]:
                    self.fp[f["ref"]] = f
                self.rev = m["rev"]
            elif m["t"] == "history":
                self.history = m
            self.inbox.append(m)
            if until(m):
                return m
        return None

    async def settle(self):
        await self.pump(lambda m: False, timeout=0.4)

    def pos(self, ref):
        f = self.fp[ref]
        return (f["x"], f["y"], f["r"])


async def main(base, restart):
    url = base.replace("http", "ws", 1) + "/ws"
    a = View(await websockets.connect(url, max_size=None))
    await a.pump(lambda m: m["t"] == "history")
    p0 = a.pos("U78")
    print(f"[start] U78 at {p0}, history {a.history}")
    check(a.history and a.history["undo"] == 0 and not a.history["modified"], "fresh board: nothing to undo, saved")

    print("[edits]")
    await a.ws.send(json.dumps({"t": "move", "refs": ["U78"], "dx": 0.5, "dy": -0.25}))
    await a.pump(lambda m: m["t"] == "moved")
    await a.ws.send(json.dumps({"t": "move", "refs": ["U78"], "rot": 90}))
    await a.pump(lambda m: m["t"] == "moved")
    await a.settle()
    p2 = a.pos("U78")
    check(a.history["undo"] == 2 and a.history["modified"], f"two edits recorded; next undo = '{a.history['next_undo']}'")

    print("[undo / redo from the browser]")
    await a.ws.send(json.dumps({"t": "undo"}))
    m = await a.pump(lambda m: m["t"] == "moved")
    await a.settle()
    check(m["ok"] and m["op"] == "undo" and "rotate" in m["label"], f"undo -> '{m['label']}'")
    check(a.pos("U78")[2] == p0[2] and a.pos("U78")[:2] != p0[:2], f"rotation undone, move kept: {a.pos('U78')}")
    await a.ws.send(json.dumps({"t": "undo"}))
    await a.pump(lambda m: m["t"] == "moved")
    await a.settle()
    check(a.pos("U78") == p0, f"second undo restores the exact original position {a.pos('U78')}")
    check(a.history["undo"] == 0 and a.history["redo"] == 2, "history: 0 undo / 2 redo")
    await a.ws.send(json.dumps({"t": "undo"}))
    m = await a.pump(lambda m: m["t"] == "moved")
    check(not m["ok"] and "nothing" in m["error"], "undo with empty history -> ok=false, nothing to undo")

    await a.ws.send(json.dumps({"t": "redo"}))
    await a.pump(lambda m: m["t"] == "moved")
    await a.settle()
    check(a.pos("U78")[:2] == (p0[0] + 500, p0[1] - 250), f"redo re-applies the move: {a.pos('U78')}")

    print("[agent uses MCP undo/redo; the browser follows]")
    text, err = await asyncio.to_thread(mcp, base, "undo")
    await a.settle()
    check(not err and text.startswith("undid: move U78") and "history:" in text, f"MCP undo: {text.splitlines()[0]}")
    check(a.pos("U78") == p0, "browser sees the agent's undo")
    text, err = await asyncio.to_thread(mcp, base, "redo")
    await a.settle()
    check(not err and text.startswith("redid:"), "MCP redo")
    check(a.history["undo"] == 1 and a.history["redo"] == 1, f"browser history updated by MCP edits: {a.history['undo']}/{a.history['redo']}")

    print("[a new edit clears redo]")
    text, err = await asyncio.to_thread(mcp, base, "move_footprints", {"refs": ["C216"], "dx": 0.1})
    await a.settle()
    check(a.history["redo"] == 0 and a.history["undo"] == 2, "redo cleared after a new edit")
    expect_u78, expect_c216 = a.pos("U78"), a.pos("C216")

    print("[save + reload from disk]")
    await a.ws.send(json.dumps({"t": "save"}))
    m = await a.pump(lambda m: m["t"] == "saved")
    await a.settle()
    check(m and m["ok"], f"save -> {m}")
    check(not a.history["modified"], "history shows saved")
    await a.ws.close()

    subprocess.run(restart, check=True)
    b = View(await websockets.connect(url, max_size=None))
    await b.pump(lambda m: m["t"] == "history", timeout=60)
    check(b.pos("U78") == expect_u78 and b.pos("C216") == expect_c216,
          f"after reload from the saved file: U78 {b.pos('U78')}, C216 {b.pos('C216')}")
    check(b.history["undo"] == 0 and not b.history["modified"], "reloaded board: fresh history, not modified")
    await b.ws.close()

    print("\nRESULT:", "PASS" if not FAILS else f"FAIL ({len(FAILS)})")
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main(sys.argv[1].rstrip("/"), sys.argv[2:])))

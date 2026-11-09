"""M2 desktop co-pilot end to end: the real pcbnew GUI under Xvfb, an agent through kicadd-mcp.

usage: python3 -I desktop_e2e.py --build BUILD_DIR --demos DEMOS_DIR --work DIR [--display :55]
                                 [--keep]

Needs Xvfb and xdotool (ImageMagick `import` for screenshots, optional). pcbnew runs from the
build tree with its own config dir and TMPDIR, so the API socket is WORK/tmp/kicad/api.sock and
kicadd-mcp finds it with no --socket (as Claude Desktop / Cursor would launch it).

The agent and the user share one board, one undo stack and one canvas:
  1. pcbnew starts its API server by itself (no preference to turn on)
  2. agent move -> the editor's board changes, status bar names it; user Ctrl+Z takes it back
  3. agent undo after the user has changed the part again -> left alone, reported
  4. user in an interactive tool (router) -> agent edits are refused as busy, not applied
  5. agent delete -> user Ctrl+Z restores it, Ctrl+Y removes it again (one owner: the undo list)
  6. the editor closes cleanly (no double free of the deleted footprint)
Screenshots go to WORK/shot-*.png.
"""

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import threading
import time

FAILS = []


def check(cond, label):
    print(("  ok   " if cond else "  FAIL ") + label, flush=True)
    if not cond:
        FAILS.append(label)


class Mcp:
    def __init__(self, cmd, env):
        self.p = subprocess.Popen(cmd, env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.PIPE, text=True, encoding="utf-8")
        self.err = []
        threading.Thread(target=lambda: self.err.extend(self.p.stderr), daemon=True).start()
        self.rid = 0
        self.rpc("initialize", {"protocolVersion": "2025-06-18", "capabilities": {},
                                "clientInfo": {"name": "desktop-e2e", "version": "1"}})
        self.p.stdin.write(json.dumps({"jsonrpc": "2.0", "method": "notifications/initialized"}) + "\n")
        self.p.stdin.flush()

    def rpc(self, method, params, timeout=60):
        self.rid += 1
        self.p.stdin.write(json.dumps({"jsonrpc": "2.0", "id": self.rid, "method": method,
                                       "params": params}) + "\n")
        self.p.stdin.flush()
        box = []
        t = threading.Thread(target=lambda: box.append(self.p.stdout.readline()), daemon=True)
        t.start()
        t.join(timeout)
        if not box or not box[0]:
            raise RuntimeError(f"no reply to {method}; stderr: {''.join(self.err)[-800:]}")
        return json.loads(box[0])

    def tool(self, name, args=None, timeout=60):
        r = self.rpc("tools/call", {"name": name, "arguments": args or {}}, timeout).get("result", {})
        return bool(r.get("isError")), "".join(c.get("text", "") for c in r.get("content", []))

    def close(self):
        try:
            self.p.stdin.close()
            self.p.wait(10)
        except Exception:
            self.p.kill()


class Desktop:
    def __init__(self, display, env):
        self.env = dict(env, DISPLAY=display)

    def x(self, *args, check_rc=False):
        r = subprocess.run(["xdotool", *args], env=self.env, capture_output=True, text=True)
        return r.stdout.strip()

    def titles(self):
        ids = self.x("search", "--onlyvisible", "--name", "").split()
        return {self.x("getwindowname", i): i for i in ids}

    def window(self, part):
        return next((i for t, i in self.titles().items() if part in t), None)

    def keys(self, win, *keys):
        self.x("windowfocus", "--sync", win)     # no window manager on Xvfb: focus, not activate
        for k in keys:
            self.x("key", "--clearmodifiers", k)
            time.sleep(0.4)

    def shot(self, path):
        if shutil.which("import"):
            subprocess.run(["import", "-window", "root", path], env=self.env, capture_output=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", required=True)
    ap.add_argument("--demos", required=True)
    ap.add_argument("--work", required=True)
    ap.add_argument("--display", default=":55")
    ap.add_argument("--keep", action="store_true", help="leave pcbnew running at the end")
    a = ap.parse_args()

    for tool in ("Xvfb", "xdotool"):
        if not shutil.which(tool):
            sys.exit(f"{tool} not found")

    work = os.path.abspath(a.work)
    shutil.rmtree(work, ignore_errors=True)
    cfg, tmp, board_dir = (os.path.join(work, d) for d in ("cfg", "tmp", "board"))
    os.makedirs(os.path.join(cfg, "10.99"))
    os.makedirs(tmp)
    shutil.copytree(os.path.join(a.demos, "pic_programmer"), board_dir)
    for name, head in (("fp-lib-table", "fp_lib_table"), ("sym-lib-table", "sym_lib_table"),
                       ("design-block-lib-table", "design_block_lib_table")):
        with open(os.path.join(cfg, "10.99", name), "w") as f:
            f.write(f"({head}\n  (version 7)\n)\n")

    env = dict(os.environ, KICAD_CONFIG_HOME=cfg, TMPDIR=tmp, KICAD_RUN_FROM_BUILD_DIR="1",
               LIBGL_ALWAYS_SOFTWARE="1")
    env.pop("KICADD_SOCK", None)
    sock = os.path.join(tmp, "kicad", "api.sock")
    xvfb = subprocess.Popen(["Xvfb", a.display, "-screen", "0", "1600x1000x24"],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(1)
    d = Desktop(a.display, env)
    log = open(os.path.join(work, "pcbnew.log"), "w")
    gui = subprocess.Popen([os.path.join(a.build, "pcbnew", "pcbnew"),
                            os.path.join(board_dir, "pic_programmer.kicad_pcb")],
                           env=d.env, stdout=log, stderr=subprocess.STDOUT)
    mcp = None

    try:
        print("[1] editor starts its API server")
        t0 = time.time()
        main_win = None
        while time.time() - t0 < 120 and gui.poll() is None:
            if (w := d.window("KiCad Setup")):          # first-run wizard: cancel, then "Yes"
                d.keys(w, "Escape")
                time.sleep(1)
                if (c := d.window("Confirmation")):
                    d.keys(c, "Return")
            if (main_win := d.window("pic_programmer")):
                break
            time.sleep(1)
        check(main_win is not None, f"pcbnew opened the board ({time.time() - t0:.0f} s)")
        check(os.path.exists(sock), f"API socket at {os.path.relpath(sock, work)} with no preference set")
        if not main_win:
            return 1
        time.sleep(3)
        d.shot(os.path.join(work, "shot-0-start.png"))

        mcp = Mcp([os.path.join(a.build, "kicadd-mcp"), "--stdio"], env)   # default socket

        def pos(ref):
            # Position of pad 1 (query_area centred on a footprint lists its pads, not itself);
            # None when the footprint is not on the board.
            err, text = mcp.tool("query_area", {"ref": ref, "radius": 3, "kinds": ["pad"]})
            m = re.search(rf"PAD {ref}\.1 .*?@(-?[\d.]+),(-?[\d.]+)", text)
            return (float(m.group(1)), float(m.group(2))) if m and not err else None

        def near(p, q):
            return p and q and abs(p[0] - q[0]) < 0.01 and abs(p[1] - q[1]) < 0.01

        err, text = mcp.tool("board_summary")
        check(not err and "footprints=63" in text, "agent reads the editor's board: " + text.splitlines()[0])

        print("[2] agent move, user Ctrl+Z")
        u3 = pos("U3")
        err, text = mcp.tool("move_footprints", {"refs": ["U3"], "dx": 0, "dy": -10})
        moved = pos("U3")
        check(not err and near(moved, (u3[0], u3[1] - 10)), f"agent moved U3 {u3} -> {moved}")
        time.sleep(1)
        d.shot(os.path.join(work, "shot-1-agent-move.png"))
        d.keys(main_win, "ctrl+z")
        time.sleep(1)
        check(near(pos("U3"), u3), "user Ctrl+Z put U3 back (one undo stack)")
        d.shot(os.path.join(work, "shot-2-user-undo.png"))

        print("[3] agent undo does not overwrite the user")
        err, text = mcp.tool("undo")
        check("changed since" in text and near(pos("U3"), u3),
              "agent undo left U3 alone: " + " ".join(text.split())[:120])

        print("[4] user in the router: agent edits wait")
        d.x("mousemove", "--sync", "700", "400")
        d.keys(main_win, "x")
        time.sleep(1)
        t1 = time.time()
        err, text = mcp.tool("move_footprints", {"refs": ["U3"], "dx": 5, "dy": 0}, timeout=60)
        check(err and "busy" in text.lower() and near(pos("U3"), u3),
              f"move refused while routing after {time.time() - t1:.0f} s: {text.strip()[:110]}")
        d.keys(main_win, "Escape", "Escape")
        time.sleep(1)
        err, text = mcp.tool("move_footprints", {"refs": ["U3"], "dx": 5, "dy": 0})
        check(not err and near(pos("U3"), (u3[0] + 5, u3[1])), "after the user leaves the router it applies")

        print("[5] agent delete, user Ctrl+Z / Ctrl+Y")
        err, text = mcp.tool("delete_footprints", {"refs": ["C1"]})
        check(not err and pos("C1") is None, "agent deleted C1")
        d.keys(main_win, "ctrl+z")
        time.sleep(1)
        check(pos("C1") is not None, "user Ctrl+Z restored C1")
        d.keys(main_win, "ctrl+y")
        time.sleep(1)
        check(pos("C1") is None, "user Ctrl+Y removed it again")
        err, text = mcp.tool("undo")
        check(pos("C1") is not None, "agent undo (from its own copy) restored C1")
        d.shot(os.path.join(work, "shot-3-end.png"))

        mcp.close()
        mcp = None
        if a.keep:
            print(f"pcbnew left running on {a.display}")
            return 1 if FAILS else 0

        print("[6] editor closes cleanly")
        d.keys(main_win, "ctrl+q")
        time.sleep(2)
        for t, w in d.titles().items():                 # "save changes?" -> don't save
            if t and "pic_programmer" not in t and t != "pcbnew":
                d.keys(w, "alt+d")
        try:
            rc = gui.wait(30)
        except subprocess.TimeoutExpired:
            rc = None
        check(rc == 0, f"pcbnew exit code {rc}")
    except Exception as e:
        check(False, f"exception: {e}")
    finally:
        if mcp:
            mcp.close()
        if not a.keep:
            if gui.poll() is None:
                gui.kill()
            xvfb.terminate()
        log.close()

    if FAILS:
        print("pcbnew log tail:\n" + open(os.path.join(work, "pcbnew.log"), errors="replace").read()[-2000:])
    print("\nRESULT:", "PASS" if not FAILS else f"FAIL ({len(FAILS)})")
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())

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
     (5b, with KICADD_LCSC_CACHE: an LCSC part placed and taken back, its symbol saved;
      5c: the user's Place > Add LCSC Part... with the part on the cursor, placed, undone)
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


class SkipRest(Exception):
    pass


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
    ap.add_argument("--timeout", type=float, default=120, help="seconds to wait for the editor (raise under valgrind)")
    ap.add_argument("--upto", type=int, default=5, help="run steps 1..N, then close (bisecting)")
    ap.add_argument("--wrap", default="", help="prefix pcbnew's command, e.g. \"gdb -q -batch -ex run -ex bt --args\"")
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
    gui = subprocess.Popen(a.wrap.split() + [os.path.join(a.build, "pcbnew", "pcbnew"),
                            os.path.join(board_dir, "pic_programmer.kicad_pcb")],
                           env=d.env, stdout=log, stderr=subprocess.STDOUT)
    mcp = None

    try:
        print("[1] editor starts its API server")
        t0 = time.time()
        main_win = None
        while time.time() - t0 < a.timeout and gui.poll() is None:
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
            raise SkipRest()
        time.sleep(3)
        d.shot(os.path.join(work, "shot-0-start.png"))
        if a.upto < 1:
            raise SkipRest()

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
        r = mcp.rpc("tools/call", {"name": "board_render", "arguments": {"refs": ["U3"], "size": 500}}).get("result", {})
        img = [c for c in r.get("content", []) if c.get("type") == "image"]
        check(not r.get("isError") and img and len(img[0].get("data", "")) > 1000,
              "board_render in the editor: a PNG of U3 (" + "".join(c.get("text", "") for c in r.get("content", [])).splitlines()[0][:60] + ")")

        if a.upto < 2:
            raise SkipRest()
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

        if a.upto < 3:
            raise SkipRest()
        print("[3] agent undo does not overwrite the user")
        err, text = mcp.tool("undo")
        check("already undone (in the editor)" in text and near(pos("U3"), u3),
              "agent undo left U3 alone: " + " ".join(text.split())[:120])

        if a.upto < 4:
            raise SkipRest()
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

        if a.upto < 5:
            raise SkipRest()
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
        print("[5a] pcb_set_fields in the editor: a field added, removed, undone, Ctrl+Z")
        err, text = mcp.tool("pcb_set_fields", {"parts": {"C1": {"MPN": "GRM188R71C104KA01", "Value": "100n"}}})
        check(not err and "set fields of 1" in text, "a new field on C1: " + text.splitlines()[0])
        err, text = mcp.tool("pcb_set_fields", {"refs": ["C1"], "set": {"MPN": None}})
        check(not err, "removed again")
        err, text = mcp.tool("undo")
        check(not err and "set fields" in text, "agent undo: the field back")
        for _ in range(3):
            d.keys(main_win, "ctrl+z")
            time.sleep(1)
        err, text = mcp.tool("board_summary")
        check(not err, "three Ctrl+Z over it: the editor runs (" + text.splitlines()[0][:50] + ")")
        d.shot(os.path.join(work, "shot-3-end.png"))

        print("[5d] agent routes in the editor, user Ctrl+Z, then the user's own router still works")
        err, text = mcp.tool("delete_tracks", {"refs": ["R9"]})
        ripped = int(re.search(r"unconnected=(\d+)", text).group(1))
        err, text = mcp.tool("route", {"from": "R9.2", "to": "D8.2"})
        check(not err and "routed on" in text and f"unconnected={ripped - 1}" in text,
              "route in the editor: " + text.splitlines()[0][:100])
        d.keys(main_win, "ctrl+z")
        time.sleep(1)
        err, text = mcp.tool("query_net", {"net": "Net-(D8-A)"})
        check(not err and "TRK" not in text, "user Ctrl+Z took the route back")
        # the editor's interactive router (its own PNS instance) after the agent's
        d.x("mousemove", "--sync", "700", "400")
        d.keys(main_win, "x")
        time.sleep(1)
        d.x("mousemove", "--sync", "760", "430")
        time.sleep(0.5)
        d.keys(main_win, "Escape", "Escape")
        time.sleep(1)
        err, text = mcp.tool("route", {"from": "R9.2", "to": "D8.2"})
        check(not err and "routed on" in text, "the user's router ran, the agent routes again")
        err, text = mcp.tool("undo")
        check(not err and "route" in text, "agent undo of the route")

        print("[5e] board setup in the editor: 4 layers, a plane on In1.Cu, an outline; undo, Ctrl+Z")
        err, text = mcp.tool("board_setup", {"copper_layers": 4})
        check(not err and "4 copper layers" in text, "board_setup 4 layers in the editor")
        # not refilled: the editor's Ctrl+Z would take back the fill first (its own undo step)
        err, text = mcp.tool("zones", {"create": {"net": "GND", "layers": ["In1.Cu"]}, "refill": False})
        check(not err and text.startswith("new zone GND on In1.Cu"), "a GND plane on In1.Cu")
        d.keys(main_win, "ctrl+z")
        time.sleep(1)
        err, z = mcp.tool("zones", {"layers": ["In1.Cu"]})
        check(not err and z.startswith("0 copper zone"), "user Ctrl+Z took the plane away")
        err, text = mcp.tool("undo")
        check(not err and "already" in text, "agent undo of the plane: already undone in the editor: " + text.splitlines()[0][:80])
        err, text = mcp.tool("undo")
        check(not err and text.startswith("undid: board setup"), "agent undo of the setup")
        err, text = mcp.tool("board_setup")
        check(text.startswith("2 copper layers"), "2 layers again")
        err, text = mcp.tool("board_outline", {"keep_old": True, "holes": [[110, 75, 3.2]]})
        check(not err and "H1 @110,75" in text, "a mounting hole added in the editor: " + text.splitlines()[0][:80])
        err, text = mcp.tool("undo")
        err, q = mcp.tool("query_area", {"x": 110, "y": 75, "radius": 1, "kinds": ["footprint"]})
        check(not err and "undid" in text and "FP H1" not in q, "agent undo of the hole")

        cache = os.environ.get("KICADD_LCSC_CACHE", "")
        if cache and os.path.exists(os.path.join(cache, "C25804.json")):
            print("[5b] agent places an LCSC part (from the cache), user Ctrl+Z")
            err, text = mcp.tool("add_footprint", {"lcsc": "C25804", "ref": "R?", "x": 150, "y": 60,
                                                   "nets": {"1": "GND"}})
            ref = re.search(r"(R\d+) added", text)
            check(not err and ref and pos(ref.group(1)) is not None,
                  "add_footprint lcsc=C25804 in the editor: " + " ".join(text.split())[:100])
            table = open(os.path.join(board_dir, "fp-lib-table"), encoding="utf-8").read()
            check('(name "LCSC")' in table and os.path.exists(os.path.join(board_dir, "lcsc.pretty", "R0603.kicad_mod")),
                  "project library LCSC written and registered")
            d.keys(main_win, "ctrl+z")
            time.sleep(1)
            check(ref and pos(ref.group(1)) is None, "user Ctrl+Z removed it")
            # M4b: the symbol goes through the eeschema kiface, loaded into the editor's process
            err, text = mcp.tool("lcsc_fetch", {"lcsc": "C25804"})
            stable = os.path.join(board_dir, "sym-lib-table")
            check(not err and "symbol LCSC:0603WAF1002T5E" in text
                  and os.path.exists(os.path.join(board_dir, "lcsc.kicad_sym"))
                  and os.path.exists(stable) and '(name "LCSC")' in open(stable, encoding="utf-8").read(),
                  "lcsc_fetch in the editor saved the symbol too: "
                  + " ".join(l for l in text.splitlines() if "symbol" in l or "not saved" in l)[:110])

            # M4d: the user's own entry, Place > Add LCSC Part... (second item of the Place menu)
            print("[5c] user: Place > Add LCSC Part..., C8734, click to place, Ctrl+Z")
            def count():
                err, text = mcp.tool("board_summary")
                m = re.search(r"footprints=(\d+)", text)
                return int(m.group(1)) if m else -1
            before = count()
            def on_board(r):      # LQFP-48: pad 1 is 5 mm from the centre, past pos()'s 3 mm
                err, text = mcp.tool("query_area", {"ref": r, "radius": 8, "kinds": ["pad"]})
                return not err and f"PAD {r}.1 " in text
            # the tool numbers it after the highest U on the board (the symbol's prefix)
            ref = next(f"U{n}" for n in range(1, 100) if not on_board(f"U{n}"))
            d.keys(main_win, "alt+p")
            d.x("key", "Down")
            d.x("key", "Return")
            dlg = None
            for _ in range(20):
                dlg = d.window("Add LCSC Part")
                if dlg:
                    break
                time.sleep(0.3)
            check(dlg is not None, "Add LCSC Part dialog opened from the Place menu")
            if dlg:
                d.x("windowfocus", "--sync", dlg)
                d.x("type", "--delay", "30", "C8734")
                d.x("key", "Return")
                time.sleep(3)
                d.x("mousemove", "600", "400")
                time.sleep(0.3)
                d.x("mousemove", "620", "420")
                time.sleep(1)
                d.shot(os.path.join(work, "shot-4-lcsc-part.png"))
                d.x("click", "1")
                time.sleep(1)
                d.x("key", "Escape")
                time.sleep(0.5)
                check(count() == before + 1 and on_board(ref),
                      f"user placed C8734 as {ref} (footprints {before} -> {count()})")
                d.keys(main_win, "ctrl+z")
                time.sleep(1)
                check(count() == before and not on_board(ref), "user Ctrl+Z took the LCSC part back")

    except SkipRest:
        pass
    except Exception as e:
        check(False, f"exception: {e}")
    try:
        if mcp:
            mcp.close()
        mcp = None
        if a.keep:
            print(f"pcbnew left running on {a.display}")
            return 1 if FAILS else 0

        if not main_win:
            raise RuntimeError("pcbnew did not open the board")
        print("[6] editor closes cleanly")
        d.keys(main_win, "ctrl+q")
        time.sleep(2)
        for t, w in d.titles().items():                 # "save changes?" -> don't save
            if t and "pic_programmer" not in t and t != "pcbnew":
                d.keys(w, "alt+d")
        try:
            rc = gui.wait(max(30, a.timeout / 2))
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

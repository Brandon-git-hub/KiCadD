"""Desktop pcbnew (Xvfb), routed pic_programmer: auto_place runs in the background.
 1. while it runs: board_summary answers fast (the editor's thread is free); an agent edit waits for it
    (refused as busy; kicadd-mcp retries busy for up to 10 s) and lands after the placement
 2. it finishes and is applied (one undo entry)
 3. a run during which the board changes (refill_zones, upstream API) moves nothing
 4. the agent's edit undone in the editor (Ctrl+Z): the agent's undo says so, moves nothing
 5. agent add, agent undo, then Ctrl+Z in the editor: the footprint comes back (the editor's
    undo still owns it), the editor keeps running

usage: python3 -I desktop_autoplace_e2e.py --build BUILD_DIR --demos DEMOS_DIR --work DIR [--display :58]

Needs Xvfb and xdotool. """
import os, shutil, subprocess, sys, time, threading
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from m5_e2e import Mcp
import argparse
ap = argparse.ArgumentParser(); ap.add_argument("--build", required=True); ap.add_argument("--demos", required=True)
ap.add_argument("--work", required=True); ap.add_argument("--display", default=":58")
a = ap.parse_args()
build, work, disp = os.path.abspath(a.build), os.path.abspath(a.work), a.display
shutil.rmtree(work, ignore_errors=True)
cfg, tmp, bd = (os.path.join(work, d) for d in ("cfg", "tmp", "board"))
os.makedirs(os.path.join(cfg, "10.99")); os.makedirs(tmp)
shutil.copytree(os.path.join(a.demos, "pic_programmer"), bd)
for name, head in (("fp-lib-table", "fp_lib_table"), ("sym-lib-table", "sym_lib_table"), ("design-block-lib-table", "design_block_lib_table")):
    open(os.path.join(cfg, "10.99", name), "w").write(f"({head}\n  (version 7)\n)\n")
env = dict(os.environ, KICAD_CONFIG_HOME=cfg, TMPDIR=tmp, KICAD_RUN_FROM_BUILD_DIR="1", LIBGL_ALWAYS_SOFTWARE="1", DISPLAY=disp)
env.pop("KICADD_SOCK", None)
sock = os.path.join(tmp, "kicad", "api.sock")
xv = subprocess.Popen(["Xvfb", disp, "-screen", "0", "1600x1000x24"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
time.sleep(1)
gui = subprocess.Popen([os.path.join(build, "pcbnew", "pcbnew"), os.path.join(bd, "pic_programmer.kicad_pcb")], env=env,
                       stdout=open(os.path.join(work, "pcbnew.log"), "w"), stderr=subprocess.STDOUT)
fails = []
def check(c, label):
    print(("  ok   " if c else "  FAIL ") + label); c or fails.append(label)
def xs(*a):
    return subprocess.run(["xdotool", *a], env=env, capture_output=True, text=True).stdout.split()
try:
    t0 = time.time()
    while time.time() - t0 < 120:
        if (w := xs("search", "--name", "KiCad Setup")):
            subprocess.run(["xdotool", "key", "--window", w[0], "Escape"], env=env); time.sleep(1)
            if (c := xs("search", "--name", "Confirmation")): subprocess.run(["xdotool", "key", "--window", c[0], "Return"], env=env)
        if os.path.exists(sock) and xs("search", "--name", "pic_programmer"): break
        time.sleep(1)
    time.sleep(4)
    mk = lambda: Mcp([os.path.join(build, "kicadd-mcp"), "--socket", sock, "--stdio"], env)
    agent, other = mk(), mk()

    def run_bg(args, box):
        t = time.time(); box["r"] = agent.tool("auto_place", args); box["dt"] = time.time() - t

    print("[1] while auto_place runs")
    box = {}; th = threading.Thread(target=run_bg, args=({"all": True, "force": True}, box)); th.start()
    time.sleep(2.0)
    lat = []
    for _ in range(5):
        t = time.time(); err, s = other.tool("board_summary"); lat.append(time.time() - t); time.sleep(0.3)
    check(max(lat) < 1.0 and th.is_alive(), f"board_summary answers while it runs (max {max(lat)*1000:.0f} ms)")
    tm = time.time(); err, m = other.tool("move_footprints", {"refs": ["R1"], "dx": 1, "dy": 0}); tm_end = time.time()
    th.join()
    print(f"       move returned after {tm_end - tm:.1f} s: {m.splitlines()[0][:60]!r}")
    check(not th.is_alive(), "an agent edit meanwhile waits for auto_place")
    err, t = box["r"]
    print(f"[2] finished in {box['dt']:.1f} s")
    check(not err and "placed 62 part(s)" in t and "undo: \"auto-place 62 parts\"" in t, "applied: " + t.splitlines()[0][-60:])
    err, s = other.tool("undo"); print("       undo 1:", s.splitlines()[0][:80])
    check(not err and "move" in s.lower(), "first undo: the move (it came after the placement)")
    err, s = other.tool("undo"); print("       undo 2:", s.splitlines()[0][:80])
    check(not err and "auto-place" in s, "second undo: the placement")

    print("[3] the board changes meanwhile")
    box = {}; th = threading.Thread(target=run_bg, args=({"all": True, "force": True}, box)); th.start()
    time.sleep(1.5)
    err, z = other.tool("refill_zones"); print("       refill_zones:", z.splitlines()[0][:80] if z else z)
    th.join()
    err, t = box["r"]
    check(err and "board changed" in t, f"nothing moved: {t[:100]!r}")

    def editor_keys(k):
        w = xs("search", "--onlyvisible", "--name", "pic_programmer")
        subprocess.run(["xdotool", "windowfocus", "--sync", w[0]], env=env)
        subprocess.run(["xdotool", "mousemove", "--sync", "800", "500"], env=env)
        subprocess.run(["xdotool", "key", "--clearmodifiers", k], env=env); time.sleep(1.5)

    def fp_count():
        err, s = other.tool("board_summary")
        return int(s.split("footprints=")[1].split()[0])

    print("[4] the agent's move undone in the editor")
    err, m = other.tool("move_footprints", {"refs": ["R1"], "dx": 2, "dy": 0})
    pos = m.splitlines()[0]
    editor_keys("ctrl+z")
    err, q = other.tool("query_area", {"x": 0, "y": 0, "radius": 1000, "kinds": ["footprint"]})
    err, u = other.tool("undo"); print("       undo:", u.splitlines()[0][:110])
    check(not err and "already undone" in u, "the agent's undo: already undone in the editor")

    print("[5] agent add, agent undo, editor Ctrl+Z")
    n0 = fp_count()
    err, t = other.tool("add_footprint", {"like": "C1", "ref": "C?", "x": 120, "y": 60})
    check(not err and fp_count() == n0 + 1, "added")
    err, u = other.tool("undo"); check(not err and fp_count() == n0, "agent undo removes it")
    editor_keys("ctrl+z")
    check(gui.poll() is None, "editor still running after Ctrl+Z")
    check(fp_count() == n0 + 1, "Ctrl+Z put the footprint back")
    editor_keys("ctrl+z")
    check(gui.poll() is None and fp_count() == n0, "Ctrl+Z again takes the add back")
    agent.close(); other.close()
finally:
    gui.terminate()
    try: gui.wait(20)
    except Exception: gui.kill()
    xv.terminate()
print("\nRESULT:", "PASS" if not fails else f"FAIL ({len(fails)})")
sys.exit(1 if fails else 0)

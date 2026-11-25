"""Desktop Co-pilot panel (roadmap phase 4 item 1): the PCB Editor under Xvfb, an agent through kicadd-mcp.

usage: python3 -I copilot_e2e.py --build BUILD --kicad KICAD_SRC --work DIR [--display :96]

Needs Xvfb and xdotool (ImageMagick `import` for the screenshots, optional). The editor is
started with launch_kicad mode=desktop (own config dir and TMPDIR), then:
  edit      the agent moves R1: it goes through, board_summary has no pause note
  pause     the user presses Ctrl+Alt+P (View > Pause Agent Edits): the agent's move is refused
            at once (no ~10 s busy retry) saying the user paused it; nothing moved; reads
            (board_summary, query_area) work and board_summary says edits are paused
  resume    Ctrl+Alt+P again: the move goes through
  hidden    KICADD_COPILOT=0 is not set: the panel is docked (screenshot WORK/copilot.png)
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from m5_e2e import FAILS, binaries, check  # noqa: E402
from desktop_e2e import Desktop, Mcp  # noqa: E402


def pos(text, ref):
    m = re.search(r"FP " + re.escape(ref) + r"\b[^@\n]*@\(?(-?[\d.]+), ?(-?[\d.]+)", text)
    return (float(m.group(1)), float(m.group(2))) if m else None


def main():
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", required=True)
    ap.add_argument("--kicad", required=True)
    ap.add_argument("--work", required=True)
    ap.add_argument("--display", default=":96")
    a = ap.parse_args()
    for tool in ("Xvfb", "xdotool"):
        if not shutil.which(tool):
            sys.exit(f"{tool} not found")

    work = os.path.abspath(a.work)
    shutil.rmtree(work, ignore_errors=True)
    cfg, tmp, pic = (os.path.join(work, d) for d in ("cfg", "tmp", "pic"))
    os.makedirs(os.path.join(cfg, "10.99"))
    os.makedirs(tmp)
    shutil.copytree(os.path.join(a.kicad, "demos", "pic_programmer"), pic)
    for name, head in (("fp-lib-table", "fp_lib_table"), ("sym-lib-table", "sym_lib_table"),
                       ("design-block-lib-table", "design_block_lib_table")):
        with open(os.path.join(cfg, "10.99", name), "w") as f:
            f.write(f"({head}\n  (version 7)\n)\n")
    _, kicadd_mcp, env = binaries(os.path.abspath(a.build))
    env = dict(env, KICAD_CONFIG_HOME=cfg, TMPDIR=tmp, DISPLAY=a.display, LIBGL_ALWAYS_SOFTWARE="1")
    env.pop("KICADD_COPILOT", None)
    xvfb = subprocess.Popen(["Xvfb", a.display, "-screen", "0", "1600x1000x24"],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(1)
    d = Desktop(a.display, env)
    stop = threading.Event()

    def wizard():   # the first-run wizard: cancel, then "Yes"
        while not stop.is_set():
            if (w := d.window("KiCad Setup")):
                d.keys(w, "Escape")
                time.sleep(1)
                if (c := d.window("Confirmation")):
                    d.keys(c, "Return")
            time.sleep(0.5)

    threading.Thread(target=wizard, daemon=True).start()
    m = Mcp([kicadd_mcp, "--socket", os.path.join(work, "d.sock"), "--stdio"], env)
    pid = 0
    try:
        err, t = m.tool("launch_kicad", {"mode": "desktop", "path": pic}, timeout=180)
        pid = int(re.search(r"pid (\d+)", t).group(1)) if "pid " in t else 0
        check(not err, "the PCB editor up (" + t.splitlines()[0][:70] + ")")
        if err:
            raise RuntimeError(t)
        win = None
        for _ in range(40):
            # a project opens in the project window with both editors: the PCB Editor's
            if (win := d.window("PCB Editor")):
                break
            time.sleep(0.5)
        check(win is not None, "its window")
        stop.set()

        def canvas_keys(*keys):
            # keys go to the canvas once it has the focus (in the project window's process the
            # frame alone does not pass them on): click an empty spot of it first, as a user would
            geo = d.x("getwindowgeometry", win)
            g = re.search(r"Position: (\d+),(\d+).*?Geometry: (\d+)x(\d+)", geo, re.S)
            x, y, w, h = (int(v) for v in g.groups()) if g else (0, 0, 1280, 720)
            d.x("windowfocus", "--sync", win)
            d.x("mousemove", "--sync", str(x + int(w * 0.60)), str(y + int(h * 0.20)))
            d.x("click", "1")
            time.sleep(0.5)
            for k in keys:
                d.x("key", "--clearmodifiers", k)
                time.sleep(0.4)

        print("[edit]")
        err, t = m.tool("move_footprints", {"refs": ["R1"], "dx": 1})
        check(not err, "the agent's move goes through")
        r1 = pos(t, "R1")
        err, t = m.tool("board_summary")
        check(not err and "paused" not in t, "board_summary: no pause note")

        print("[pause]")
        canvas_keys("ctrl+alt+p")
        time.sleep(1)
        t0 = time.time()
        err, t = m.tool("move_footprints", {"refs": ["R1"], "dx": 1})
        took = time.time() - t0
        check(err and "paused" in t and "user" in t, "the agent's move refused: " + t.strip()[:90])
        check(took < 3, f"at once, not retried ({took:.1f} s)")
        err, t = m.tool("query_area", {"ref": "R1", "radius": 0.5})
        check(not err and (pos(t, "R1") is None or r1 is None or abs(pos(t, "R1")[0] - r1[0]) < 0.01),
              "reads work; R1 did not move")
        err, t = m.tool("board_summary")
        check(not err and "the user paused the agent's edits" in t, "board_summary says edits are paused")
        d.shot(os.path.join(work, "copilot.png"))

        print("[resume]")
        canvas_keys("ctrl+alt+p")
        time.sleep(1)
        err, t = m.tool("move_footprints", {"refs": ["R1"], "dx": 1})
        check(not err, "resumed: the move goes through")
        err, t = m.tool("board_summary")
        check(not err and "paused" not in t, "board_summary: no pause note again")
        d.shot(os.path.join(work, "copilot-resumed.png"))

        print("[focus]")
        m.tool("move_footprints", {"refs": ["R2"], "dx": 1})
        geo = d.x("getwindowgeometry", win)
        g = re.search(r"Position: (\d+),(\d+).*?Geometry: (\d+)x(\d+)", geo, re.S)
        x, y, w, h = (int(v) for v in g.groups()) if g else (0, 0, 1280, 720)
        for n, ref in ((1, "R2"), (2, "R1")):    # twice: the second click finds the panel as the first left it
            err, t = m.tool("query_area", {"ref": ref, "radius": 0.5})
            before = pos(t, ref)
            d.x("windowfocus", "--sync", win)
            d.x("mousemove", "--sync", str(x + int(w * 0.90)), str(y + int(h * 0.87)))   # the Co-pilot list
            d.x("click", "1")
            time.sleep(1)
            d.x("key", "--clearmodifiers", "ctrl+z")
            time.sleep(1.5)
            err, t = m.tool("query_area", {"ref": ref, "radius": 0.5})
            after = pos(t, ref)
            check(before and after and abs(after[0] - before[0] + 1) < 0.01,
                  f"click {n} in the panel, then Ctrl+Z: the editor undoes the agent's move of {ref} ({before} -> {after})")

        err, t = m.tool("close_kicad", {"discard": True})
        check(not err, "closed")
    finally:
        stop.set()
        try:
            m.tool("close_kicad", {"discard": True})
        except Exception:
            pass
        m.close()
        if pid:
            try:
                os.kill(pid, 9)
            except OSError:
                pass
        xvfb.terminate()

    print("RESULT: " + ("FAIL (%d); first: %s" % (len(FAILS), FAILS[0]) if FAILS else "PASS"))
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())

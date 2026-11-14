"""Desktop co-pilot, schematic side: the real KiCad project manager under Xvfb, the Schematic
Editor and the PCB Editor open, an agent through kicadd-mcp.

usage: python3 -I desktop_sch_e2e.py --build BUILD_DIR --demos DEMOS_DIR --work DIR [--display :56]
                                     [--keep]

Needs Xvfb and xdotool (ImageMagick `import` for screenshots, optional). Runs from the build tree
with its own config dir and TMPDIR, so the API socket is WORK/tmp/kicad/api.sock and kicadd-mcp
finds it with no --socket. The project manager is one process: both editors' handlers are on
its one API server.

  1. project manager, Ctrl+E: the Schematic Editor registers the agent's schematic tools
  2. agent reads the editor's schematic, adds a part and connects it: the editor's schematic
     changes (title marked modified, status bar names the edit)
  3. user Ctrl+Z takes the connection back, then the part (one undo list); the agent adds a
     sheet with a part on it, two Ctrl+Z take both away
  4. user in an interactive tool (drawing a wire): agent edits refused as busy, applied after
  5. Ctrl+P, agent sync_pcb: the PCB Editor's board gets the new part from the open Schematic
     Editor (unsaved), and the schematic file on disk is untouched
  6. Ctrl+Q closes everything cleanly (don't save)
Screenshots go to WORK/shot-*.png.
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from desktop_e2e import Desktop, Mcp, SkipRest, check, FAILS  # noqa: E402

FOOTPRINT = "footprints:SolderJumper-2_P1.3mm_Open_TrianglePad1.0x1.5mm"   # in the project's own library


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", required=True)
    ap.add_argument("--demos", required=True)
    ap.add_argument("--work", required=True)
    ap.add_argument("--display", default=":56")
    ap.add_argument("--keep", action="store_true", help="leave KiCad running at the end")
    ap.add_argument("--timeout", type=float, default=120)
    a = ap.parse_args()

    for tool in ("Xvfb", "xdotool"):
        if not shutil.which(tool):
            sys.exit(f"{tool} not found")

    work = os.path.abspath(a.work)
    shutil.rmtree(work, ignore_errors=True)
    cfg, tmp, proj = (os.path.join(work, d) for d in ("cfg", "tmp", "proj"))
    os.makedirs(os.path.join(cfg, "10.99"))
    os.makedirs(tmp)
    shutil.copytree(os.path.join(a.demos, "pic_programmer"), proj)
    for name, head in (("fp-lib-table", "fp_lib_table"), ("sym-lib-table", "sym_lib_table"),
                       ("design-block-lib-table", "design_block_lib_table")):
        with open(os.path.join(cfg, "10.99", name), "w") as f:
            f.write(f"({head}\n  (version 7)\n)\n")
    sch_file = os.path.join(proj, "pic_programmer.kicad_sch")
    sch_before = open(sch_file, "rb").read()

    env = dict(os.environ, KICAD_CONFIG_HOME=cfg, TMPDIR=tmp, KICAD_RUN_FROM_BUILD_DIR="1",
               LIBGL_ALWAYS_SOFTWARE="1")
    env.pop("KICADD_SOCK", None)
    sock = os.path.join(tmp, "kicad", "api.sock")
    xvfb = subprocess.Popen(["Xvfb", a.display, "-screen", "0", "1600x1000x24"],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(1)
    d = Desktop(a.display, env)
    log = open(os.path.join(work, "kicad.log"), "w")
    gui = subprocess.Popen([os.path.join(a.build, "kicad", "kicad"), os.path.join(proj, "pic_programmer.kicad_pro")],
                           env=d.env, stdout=log, stderr=subprocess.STDOUT)
    mcp = None

    def canvas_keys(win, *keys):
        # keys go to the canvas once it has the focus: click an empty spot near its lower right
        # corner first (as a user would; after an undo the focus is elsewhere)
        d.x("windowfocus", "--sync", win)
        geo = d.x("getwindowgeometry", win)
        m = re.search(r"Position: (\d+),(\d+).*?Geometry: (\d+)x(\d+)", geo, re.S)
        x, y, w, h = (int(v) for v in m.groups()) if m else (0, 0, 1280, 720)
        d.x("mousemove", "--sync", str(x + int(w * 0.93)), str(y + int(h * 0.85)))
        d.x("click", "1")
        time.sleep(0.5)
        for k in keys:
            d.x("key", "--clearmodifiers", k)
            time.sleep(0.4)

    def wait_window(part, timeout):
        t0 = time.time()
        while time.time() - t0 < timeout and gui.poll() is None:
            if (w := d.window("KiCad Setup")):          # first-run wizard: cancel, then "Yes"
                d.keys(w, "Escape")
                time.sleep(1)
                if (c := d.window("Confirmation")):
                    d.keys(c, "Return")
            if (w := d.window(part)):
                return w
            time.sleep(1)
        return None

    try:
        print("[1] project manager, Ctrl+E: the Schematic Editor")
        manager = wait_window("pic_programmer — KiCad", a.timeout)
        check(manager is not None, "project manager open")
        if not manager:
            raise SkipRest()
        d.keys(manager, "ctrl+e")
        sch_win = wait_window("Schematic Editor", a.timeout)
        check(sch_win is not None, "Schematic Editor open")
        check(os.path.exists(sock), f"API socket at {os.path.relpath(sock, work)} with no preference set")
        if not sch_win:
            raise SkipRest()
        time.sleep(3)

        mcp = Mcp([os.path.join(a.build, "kicadd-mcp"), "--stdio"], env)

        print("[2] agent edits the editor's schematic")
        err, t = mcp.tool("sch_summary")
        check(not err and " 69 parts" in t, "agent reads the editor's schematic: " + t.splitlines()[0][-60:])
        err, t = mcp.tool("sch_add_part", {"lib_id": "pic_programmer:R", "value": "4k7", "footprint": FOOTPRINT,
                                           "x": 120, "y": 200})
        m = re.match(r"added (R\d+) ", t)
        ref = m.group(1) if m else None
        check(not err and ref, "agent added " + t.splitlines()[0][:80])
        err, t = mcp.tool("sch_connect", {"nets": {"VPP": [f"{ref}.1"], "AGENT_PULLUP": [f"{ref}.2"]}})
        check(not err and t.startswith("2 pin(s) connected"), "agent connected it: " + t[:70])
        err, t = mcp.tool("sch_part", {"ref": ref})
        check("VPP" in t and "AGENT_PULLUP" in t, "its pins on VPP and AGENT_PULLUP: " + " ".join(t.split())[:100])
        time.sleep(1)
        titles = d.titles()
        check(any(t.startswith("*") and "Schematic Editor" in t for t in titles),
              f"the editor shows the schematic modified: {[t for t in titles if 'Schematic' in t]}")
        d.shot(os.path.join(work, "shot-1-agent-edit.png"))

        r = mcp.rpc("tools/call", {"name": "sch_render", "arguments": {"refs": [ref], "size": 600}}).get("result", {})
        img = [c for c in r.get("content", []) if c.get("type") == "image"]
        check(not r.get("isError") and img and len(img[0].get("data", "")) > 1000,
              "sch_render in the editor: a PNG of " + ref)

        print("[3] user Ctrl+Z")
        canvas_keys(sch_win, "ctrl+z")
        time.sleep(1)
        err, t = mcp.tool("sch_part", {"ref": ref})
        check("AGENT_PULLUP" not in t, "first Ctrl+Z takes the connection back")
        canvas_keys(sch_win, "ctrl+z")
        time.sleep(1)
        err, t = mcp.tool("sch_part", {"ref": ref})
        check(err and "no part" in t.lower(), f"second Ctrl+Z takes {ref} away: {t[:60]}")
        d.shot(os.path.join(work, "shot-2-user-undo.png"))

        print("[3b] agent adds a sheet with a part, user Ctrl+Z twice")
        err, t = mcp.tool("sch_add_sheet", {"name": "Agent", "x": 30, "y": 190})
        check(not err and "SHEET Agent /Agent/" in t, "agent added a sheet: " + t.splitlines()[0][:70])
        err, t = mcp.tool("sch_add_part", {"lib_id": "pic_programmer:R", "value": "1k", "sheet": "Agent"})
        m2 = re.match(r"added (R\d+) ", t)
        check(not err and m2 and "on /Agent/" in t.splitlines()[0], "a part on it: " + t.splitlines()[0][:70])
        err, t = mcp.tool("sch_summary")
        check(" 70 parts" in t and "3 sheet(s)" in t, "70 parts, 3 sheets: " + t.splitlines()[0][-45:])
        d.shot(os.path.join(work, "shot-2b-sheet.png"))
        canvas_keys(sch_win, "ctrl+z")
        time.sleep(1)
        canvas_keys(sch_win, "ctrl+z")
        time.sleep(1)
        err, t = mcp.tool("sch_summary")
        check(" 69 parts" in t and "2 sheet(s)" in t and "SHEET Agent" not in t,
              "user Ctrl+Z twice: part and sheet gone: " + t.splitlines()[0][-45:])

        print("[3c] power symbol, sch_move, sch_wire, run_erc in the editor; Ctrl+Z")
        err, t = mcp.tool("sch_add_part", {"lib_id": "pic_programmer:R", "value": "1k", "footprint": FOOTPRINT,
                                           "x": 140, "y": 200})
        m3 = re.match(r"added (R\d+) ", t)
        r3 = m3.group(1) if m3 else None
        err, t = mcp.tool("sch_connect", {"nets": {"GND": [f"{r3}.2"], "AGENT_N": [f"{r3}.1"]}, "power": ["GND"]})
        check(not err and t.startswith("2 pin(s) connected"), "connected with a GND power symbol: " + t[:50])
        err, n1 = mcp.tool("sch_netlist", {"net": "GND"})
        check(f"{r3}.2" in n1, f"{r3}.2 on GND")
        err, t = mcp.tool("sch_move", {"ref": r3, "dx": 10.16, "rotation": 90})
        err2, n2 = mcp.tool("sch_netlist", {"net": "GND"})
        check(not err and f"{r3}.2" in n2 and "@" in t, "moved and turned, still on GND: " + t.splitlines()[0][:70])
        err, t = mcp.tool("sch_wire", {"points": [[152.4, 190.5], [165.1, 190.5]]})
        check(not err and "1 segment" in t, "a wire: " + t.splitlines()[0][:70])
        err, t = mcp.tool("run_erc")
        check(not err and t.startswith("ERC:") and "footprint_link_issues" not in t, "run_erc: " + t.splitlines()[0][:90])
        for _ in range(4):
            canvas_keys(sch_win, "ctrl+z")
            time.sleep(1)
        err, t = mcp.tool("sch_summary")
        err2, n3 = mcp.tool("sch_netlist", {"net": "GND"})
        check(" 69 parts" in t and f"{r3}." not in n3, "four Ctrl+Z: wire, move, connection, part gone: " + t.splitlines()[0][-30:])

        print("[3d] sch_undo with the user's Ctrl+Z in between")
        err, t = mcp.tool("sch_add_part", {"lib_id": "pic_programmer:R", "value": "2k2", "footprint": FOOTPRINT,
                                           "x": 160, "y": 200})
        m4 = re.match(r"added (R\d+) ", t)
        r4 = m4.group(1) if m4 else None
        mcp.tool("sch_connect", {"nets": {"AGENT_U": [f"{r4}.1"]}})
        canvas_keys(sch_win, "ctrl+z")
        time.sleep(1)
        err, t = mcp.tool("sch_undo")
        check(not err and "already undone (in the editor)" in t, "the connect the user undid: said so: " + t.splitlines()[0][:80])
        err, t = mcp.tool("sch_undo")
        err2, p = mcp.tool("sch_part", {"ref": r4})
        check(not err and t.startswith(f"undid: add {r4}") and err2, f"sch_undo takes {r4} away: " + t.splitlines()[0][:60])
        canvas_keys(sch_win, "ctrl+z")
        time.sleep(1)
        err, p = mcp.tool("sch_part", {"ref": r4})
        check(not err, f"Ctrl+Z undoes the agent's undo: {r4} back")
        canvas_keys(sch_win, "ctrl+z")
        time.sleep(1)
        err, t = mcp.tool("sch_summary")
        check(not err and " 69 parts" in t, "Ctrl+Z again: the add undone, the editor runs: " + t.splitlines()[0][-30:])
        err, t = mcp.tool("sch_redo")
        check(not err and "add" in t, "sch_redo afterwards answers: " + t.splitlines()[0][:70])
        canvas_keys(sch_win, "ctrl+z")
        time.sleep(1)

        print("[4] user drawing a wire: agent edits wait")
        canvas_keys(sch_win, "w")
        time.sleep(1)
        err, t = mcp.tool("sch_add_part", {"lib_id": "pic_programmer:R", "value": "4k7", "footprint": FOOTPRINT,
                                           "x": 120, "y": 200})
        check(err and "busy" in t.lower(), "add refused while the user draws: " + t.strip()[:90])
        err, s = mcp.tool("sch_summary")
        check(" 69 parts" in s, "nothing was added")
        canvas_keys(sch_win, "Escape", "Escape")
        time.sleep(1)
        err, t = mcp.tool("sch_add_part", {"lib_id": "pic_programmer:R", "value": "4k7", "footprint": FOOTPRINT,
                                           "x": 120, "y": 200})
        m = re.match(r"added (R\d+) ", t)
        ref = m.group(1) if m else None
        check(not err and ref, "after the user leaves the tool it applies: " + t.splitlines()[0][:60])
        mcp.tool("sch_connect", {"nets": {"VPP": [f"{ref}.1"], "AGENT_PULLUP": [f"{ref}.2"]}})

        print("[5] PCB Editor, agent sync_pcb from the open Schematic Editor")
        d.keys(manager, "ctrl+p")
        pcb_win = wait_window("PCB Editor", a.timeout)
        check(pcb_win is not None, "PCB Editor open")
        if not pcb_win:
            raise SkipRest()
        time.sleep(3)
        err, t = mcp.tool("board_summary")
        check(not err and "footprints=63" in t, "board as saved: " + t.splitlines()[0][:70])
        err, t = mcp.tool("sync_pcb")
        check(not err and f"added: {ref}" in t and "from the open Schematic Editor" in t,
              "sync_pcb: " + " ".join(t.split())[:400])
        err, q = mcp.tool("query_net", {"net": "/AGENT_PULLUP"})
        check(f"PAD {ref}.2" in q, f"the board has {ref} on AGENT_PULLUP")
        check(open(sch_file, "rb").read() == sch_before, "the schematic file on disk is untouched (not saved)")
        d.shot(os.path.join(work, "shot-3-sync.png"))

        print("[6] Ctrl+Q closes everything (don't save)")
        d.keys(manager, "ctrl+q")
        for _ in range(6):
            time.sleep(2)
            if gui.poll() is not None:
                break
            for title, w in d.titles().items():          # "save changes?" -> don't save
                if title and "KiCad" not in title and "Editor" not in title and title != "kicad":
                    d.keys(w, "alt+d")
        try:
            rc = gui.wait(max(30, a.timeout / 2))
        except subprocess.TimeoutExpired:
            rc = None
        check(rc == 0, f"KiCad exit code {rc}")
    except SkipRest:
        pass
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
        print("kicad log tail:\n" + open(os.path.join(work, "kicad.log"), errors="replace").read()[-2000:])
    print("\nRESULT:", "PASS" if not FAILS else f"FAIL ({len(FAILS)})")
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())

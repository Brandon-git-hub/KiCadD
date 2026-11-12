"""Headless, routed pic_programmer: auto_place keeps a better current placement, delete_tracks
(refs / all) with undo / redo, auto_place after ripping up, and the refill hint.

usage: python3 -I routed_e2e.py --build BUILD_DIR --demos DEMOS_DIR --work DIR """
import os, re, shutil, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from m5_e2e import Mcp, Server
import argparse
ap = argparse.ArgumentParser(); ap.add_argument("--build", required=True); ap.add_argument("--demos", required=True)
ap.add_argument("--work", required=True)
a = ap.parse_args()
build, work = os.path.abspath(a.build), os.path.abspath(a.work)
shutil.rmtree(work, ignore_errors=True)
shutil.copytree(os.path.join(a.demos, "pic_programmer"), os.path.join(work, "p"))
sock = os.path.join(work, "s.sock"); env = dict(os.environ, KICAD_RUN_FROM_BUILD_DIR="1")
srv = Server(build, sock, os.path.join(work, "p", "pic_programmer.kicad_pcb"), env, os.path.join(work, "server.log"))
assert srv.start()
fails = []


def check(c, label):
    print(("  ok   " if c else "  FAIL ") + label); c or fails.append(label)


def counts(m):
    err, s = m.tool("board_summary")
    tr = int(re.search(r"tracks=(\d+)", s).group(1))
    vi = int(re.search(r"vias=(\d+)", s).group(1))
    return tr, vi, s


def unconnected(s):
    m = re.search(r"unconnected=(\d+)", s)
    return int(m.group(1)) if m else -1


try:
    m = Mcp([os.path.join(build, "kicadd-mcp"), "--socket", sock, "--stdio"], env)
    tr0, vi0, s = counts(m)
    print("board:", s.splitlines()[0][:100], f"| tracks {tr0} vias {vi0}")

    print("[1] auto_place on the routed, hand-placed board")
    err, t = m.tool("auto_place", {"all": True})
    print("      ", t.splitlines()[0][:150])
    check(not err and t.startswith("kept the current placement"), "the current placement is kept (the new one is longer)")
    check("force=true" in t, "says force applies it anyway")
    tr, vi, _ = counts(m)
    check((tr, vi) == (tr0, vi0), "nothing changed")

    print("[2] delete_tracks refs=[U5], undo, redo, undo")
    err, t = m.tool("delete_tracks", {"refs": ["U5"]})
    print("      ", " | ".join(t.splitlines()[:3]))
    n = int(re.search(r"(\d+) track", t).group(1)) if re.search(r"(\d+) track", t) else 0
    tr1, vi1, _ = counts(m)
    check(not err and n > 0 and tr1 + vi1 == tr0 + vi0 - n, f"removed {n}, board {tr0 + vi0} -> {tr1 + vi1}")
    check("refill_zones" in t, "refill hint")
    u1 = unconnected(t)
    err, t = m.tool("undo"); print("       undo:", t.splitlines()[0][:80])
    check(not err and t.startswith("undid") and counts(m)[:2] == (tr0, vi0), "undo puts them back")
    u_back = unconnected(t)
    check(u_back < u1, f"unconnected {u1} -> {u_back}")
    err, t = m.tool("redo"); print("       redo:", t.splitlines()[0][:80])
    check(not err and counts(m)[:2] == (tr1, vi1), "redo removes them again")
    err, t = m.tool("undo")
    check(not err and counts(m)[:2] == (tr0, vi0), "undo again")

    print("[3] delete_tracks all, then auto_place")
    err, t = m.tool("delete_tracks", {"all": True})
    tr2, vi2, _ = counts(m)
    print("      ", t.splitlines()[0][:80], f"| left: {tr2} tracks {vi2} vias")
    check(not err and tr2 + vi2 < tr0 + vi0, "routing removed (locked ones may stay)")
    err, t = m.tool("auto_place", {"all": True, "time_limit": 5})
    print("      ", t.splitlines()[0][:150])
    check(not err and re.match(r"placed \d+ part", t) is not None or t.startswith("kept"), "auto_place runs on the ripped-up board")
    if t.startswith("placed"):
        check("refill_zones" in t, "refill hint after moving parts")
        err, u = m.tool("undo"); check(not err and "auto-place" in u, "undo the placement")
    err, u = m.tool("undo"); check(not err and "track" in u and counts(m)[:2] == (tr0, vi0), "undo the rip-up: routing back")

    print("[4] force")
    err, t = m.tool("auto_place", {"all": True, "force": True, "time_limit": 3})
    print("      ", t.splitlines()[0][:100])
    check(not err and t.startswith("placed"), "force applies it")
    check("their tracks behind" in t and "refill_zones" in t, "says the tracks are left behind, refill")
    check("in all; detail=true" in t and len(t.split(":")[1].split("...")[0].split()) == 15,
          "auto_place lists 15 refs, then a count")

    print("[5] brief output: undo / move of many parts lists five, detail=true all")
    err, u = m.tool("undo")
    moved = re.findall(r"^\S+ @", u, re.M)
    check(not err and len(moved) == 5 and re.search(r"^\.\.\. \d+ more part\(s\) moved \(detail=true", u, re.M),
          f"undo: {len(moved)} positions, then a count")
    check(re.search(r"^drc: ", u, re.M) is not None, "the DRC line is still there")
    err, u = m.tool("redo", {"detail": True})
    check(not err and len(re.findall(r"^\S+ @", u, re.M)) > 50, "redo detail=true: every part")
    refs = sorted(set(re.findall(r'\(property "Reference" "([^"#]+)"', open(os.path.join(work, "p", "pic_programmer.kicad_pcb")).read())))
    err, t = m.tool("move_footprints", {"refs": refs, "dx": 1})
    check(not err and len(re.findall(r"^\S+ @", t, re.M)) == 5 and "more part(s) moved" in t, "move_footprints: five, then a count")
    drc_lines = [l for l in t.splitlines()[t.splitlines().index(next(l for l in t.splitlines() if l.startswith("drc:"))) + 1:]]
    check(len(drc_lines) <= 11, f"local DRC cut to 10 lines + a count ({len(drc_lines)})")
    err, t = m.tool("move_footprints", {"refs": refs[:3], "dx": 1})
    check(len(re.findall(r"^\S+ @", t, re.M)) == 3 and "more part" not in t, "three parts: all three listed")

    print("[6] get_rules / set_rules")
    err, r0 = m.tool("get_rules")
    print("      ", " | ".join(r0.splitlines()[:3])[:160])
    check(not err and "Default:" in r0 and "constraints:" in r0, "get_rules answers")
    err, t = m.tool("set_rules", {"classes": {"Power": {"track_width": 0.6, "clearance": 0.3}},
                                  "assign": {"VCC": "Power", "GND": "Power"}})
    print("      ", " | ".join(t.splitlines()[:2])[:200])
    check(not err and t.startswith("set rules: 1 net class(es), 2 assignment(s)"), "a Power class for VCC and GND")
    check(re.search(r"Power: clearance 0\.3, track 0\.6 \[2 net\(s\)\]", t) is not None, "Power has its 2 nets: " + next((l for l in t.splitlines() if "Power:" in l), ""))
    check(re.search(r"DRC errors \d+ -> \d+", t) is not None, "DRC before / after reported")
    err, t = m.tool("set_rules", {"constraints": {"min_track_width": 5}, "drc": True})
    m_ = re.search(r"DRC errors (\d+) -> (\d+)", t)
    check(not err and m_ and int(m_.group(2)) > int(m_.group(1)), "a 5 mm minimum track: more DRC errors: " + (m_.group(0) if m_ else t[:80]))
    err, t = m.tool("set_rules", {"classes": {"Default": {"clearance": None}}, "drc": False})
    check(err and "Default class needs a clearance" in t, "Default keeps its values")
    err, t = m.tool("set_rules", {"assign": {"X": "Nope"}, "drc": False})
    check(err and "no net class Nope" in t, "an assignment to a missing class is refused")
    m.tool("undo"); m.tool("undo")
    err, r1 = m.tool("get_rules")
    check(r1 == r0, "two undo: the rules as before")
    err, t = m.tool("redo")
    check(not err and "set rules" in t, "redo: " + t.splitlines()[0])
    err, t = m.tool("save_board")
    pro = open(os.path.join(work, "p", "pic_programmer.kicad_pro")).read()
    check('"Power"' in pro and '"VCC"' in pro, "save_board: the class and its patterns in the project file")

    print("[7] zones")
    err, z0 = m.tool("zones")
    print("      ", " | ".join(z0.splitlines()[:3])[:180])
    n = int(re.match(r"(\d+) copper zone", z0).group(1)) if re.match(r"(\d+) copper zone", z0) else 0
    check(not err and n > 0, f"{n} zone(s) listed")
    net = re.search(r"^  \S+ (\S+) ", z0, re.M).group(1)
    err, t = m.tool("zones", {"nets": [net], "clearance": 0.6, "connection": "solid", "priority": 3})
    print("      ", " | ".join(t.splitlines()[:3])[:180])
    check(not err and t.startswith("zone settings of") and "refilled" in t.lower(), "changed and refilled")
    check("clearance 0.6" in t and "pads solid" in t and "priority 3" in t, "the new settings listed")
    err, t = m.tool("zones", {"clearance": 1})
    check(err and "which zones" in t, "a change needs a choice of zones")
    err, t = m.tool("undo")
    err, z1 = m.tool("zones")
    check(not err and z1 == z0, "undo: the zones as before")
    err, t = m.tool("redo")
    check(not err and "zone settings" in t, "redo: " + t.splitlines()[0])

    print("[8] batch")
    err, t = m.tool("batch", {"steps": [{"tool": "move_footprints", "args": {"refs": ["U5"], "dx": 1}}, {"tool": "undo"}],
                              "repeat": 5})
    print("      ", t.splitlines()[0])
    check(not err and t.startswith("batch: 10 of 10 call(s) run, 0 error(s)") and len(t.splitlines()) == 11,
          "5 x (move, undo): one line per call")
    check("[5.2] undo: undid: move U5" in t, "each step's first line: " + t.splitlines()[-1][:60])
    err, t = m.tool("batch", {"steps": [{"tool": "move_footprints", "args": {"refs": ["NOPE"], "dx": 1}},
                                        {"tool": "board_summary"}]})
    check(err and "1 of 2 call(s) run, 1 error(s)" in t and "stopped at the error" in t, "stops at the first error")
    err, t = m.tool("batch", {"steps": [{"tool": "move_footprints", "args": {"refs": ["NOPE"], "dx": 1}},
                                        {"tool": "board_summary"}], "stop_on_error": False, "show": "last"})
    check(not err and "2 of 2" in t and "--- last step in full:" in t and "footprints=" in t, "stop_on_error=false, show=last")
    err, t = m.tool("batch", {"steps": [{"tool": "nope"}]})
    check(err and "no tool nope" in t, "an unknown tool is refused before anything runs")
    m.close()
finally:
    srv.stop()
print("\nRESULT:", "PASS" if not fails else f"FAIL ({len(fails)})")
sys.exit(1 if fails else 0)

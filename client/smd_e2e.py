"""The alpha.12 trial path (BUILD: a build tree, or an installed folder like the Windows zip's bin/) on an SMD board (headless): KiCad's stickhub demo (94 parts, all SMD
but one, routed, GND / +5V / +1V8 zones, metric chip footprints 1005 / 1608 / 2012).

usage: python3 -I smd_e2e.py --build BUILD --demos DEMOS_DIR --work DIR --index PARTS_DB

PARTS_DB: an unpacked parts index (ci/build_parts_index.py's parts.db). Offline: lcsc_match runs
with live=false (no stock check).

  * lcsc_match on the schematic's passives (metric footprint names), dry run then apply, sch_undo
  * fanout: GND pads freed of their tracks, fanned out to the zone on the other side; undo
  * audit with currents / voltages, set_rules current, silk_tidy, board_diff, all undone
"""

import argparse
import os
import re
import shutil
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from m5_e2e import FAILS, Mcp, Server, binaries, check  # noqa: E402

ap = argparse.ArgumentParser()
ap.add_argument("--build", required=True)
ap.add_argument("--demos", required=True)
ap.add_argument("--work", required=True)
ap.add_argument("--index", required=True)
a = ap.parse_args()

build, work = os.path.abspath(a.build), os.path.abspath(a.work)
shutil.rmtree(work, ignore_errors=True)
shutil.copytree(os.path.join(a.demos, "stickhub"), os.path.join(work, "p"))
pcb = os.path.join(work, "p", "StickHub.kicad_pcb")
sock = os.path.join(work, "s.sock")
kicad_cli, kicadd_mcp, env = binaries(build)
env.update(KICADD_LCSC_OFFLINE="1", KICADD_PARTS_DB=os.path.abspath(a.index))

srv = Server(build, sock, pcb, env, os.path.join(work, "server.log"))
assert srv.start(), "api-server did not start"
try:
    m = Mcp([kicadd_mcp, "--socket", sock, "--stdio"], env)

    print("[the board]")
    err, t = m.tool("board_summary")
    print("      ", t.splitlines()[0][:150])
    check(not err and "footprints=94" in t, "stickhub: 94 footprints")
    err, t = m.tool("board_diff")
    check(not err and t.startswith("no changes"), "as saved")

    print("[lcsc_match]")
    err, t = m.tool("lcsc_match", {"live": False})
    print("      ", " | ".join(t.splitlines()[:3])[:300])
    groups = re.search(r"(\d+) matched", t)
    check(not err and groups and int(groups.group(1)) >= 5, "metric chip footprints (1005 / 1608 / 2012) matched: " + t.splitlines()[0][:100])
    check(re.search(r"\(1uF 10V, 1005_C\)[^\n]*0402", t) is not None and "not a chip package" not in t.split("1005_C")[1][:60],
          "1005_C read as 0402")
    err, t = m.tool("lcsc_match", {"live": False, "apply": True})
    check(not err and "sch_undo" in t, "applied: " + t.splitlines()[0][:100])
    err, t = m.tool("sch_undo")
    check(not err and t.startswith("undid: set fields"), "sch_undo: " + t.splitlines()[0][:80])

    print("[fanout]")
    err, t = m.tool("delete_tracks", {"nets": ["GND"]})
    check(not err, "GND tracks removed: " + t.splitlines()[0][:80])
    err, t = m.tool("fanout", {"nets": ["GND"], "dry_run": True})
    print("      ", t.splitlines()[0][:150])
    n = re.search(r"(\d+) pad\(s\)", t)
    check(not err and n and int(n.group(1)) > 0, "fanout dry run: GND pads to fan out")
    err, t = m.tool("fanout", {"nets": ["GND"]})
    check(not err and "via" in t, "fanout: " + t.splitlines()[0][:120])
    err, t = m.tool("run_drc", {"types": ["clearance", "shorting_items", "hole_clearance"]})
    print("      ", " | ".join(t.splitlines()[:6])[:400])
    err, t = m.tool("undo")
    check(not err and "all zones refilled" in t and "drc: clean" not in t, "undo of fanout refills the zones: " + t.splitlines()[0])
    err, t = m.tool("undo")
    check(not err and "all zones refilled" in t, "undo of delete_tracks refills the zones")
    err, t = m.tool("run_drc")
    check(not err and "0 errors" in t.splitlines()[0], "after the undos the full DRC is clean (the fills match the tracks): "
          + t.splitlines()[0])

    print("[audit, set_rules, silk_tidy]")
    err, t = m.tool("audit", {"currents": {"+5V": 2}, "voltages": {"+5V": 5}})
    print("      ", t.splitlines()[0][:200])
    check(not err and "board:" in t and re.search(r"\+5V \(2 A\)|current:", t) is not None or "current:" not in t,
          "audit: the board half answers")
    err, t = m.tool("set_rules", {"classes": {"Power": {"current": 2}}, "assign": {"+5V": "Power"}, "drc": False})
    check(not err and "Power: 2 A -> " in t, "set_rules current: " + (t.splitlines()[1] if len(t.splitlines()) > 1 else t)[:100])
    check("assign +5V -> Power: 1 net(s)" in t, "set_rules says what each pattern matched")
    m.tool("undo")
    # a pattern that misses the sheet-path nets ("/U2D-") says so, with a pattern that catches them
    err, t = m.tool("set_rules", {"classes": {"USB": {"track_width": 0.2}}, "assign": {"U?D-": "USB"}, "drc": False})
    miss = re.search(r"assign U\?D- -> USB: 0 net\(s\); (\d+) more only under a sheet path.*\"\*U\?D-\"", t)
    check(not err and miss and int(miss.group(1)) >= 6, "set_rules: sheet-path nets missed by a pattern named: "
          + next((l for l in t.splitlines() if l.startswith("assign")), t)[:160])
    m.tool("undo")
    err, t = m.tool("set_rules", {"classes": [{"name": "USB"}]})
    check(err and "classes is an object" in t, "set_rules: a list for classes refused, not ignored")
    err, t = m.tool("silk_tidy", {"dry_run": True})
    print("      ", t.splitlines()[0][:150])
    check(not err and t.startswith("would move 0 text(s)"), "silk_tidy dry run: nothing to move (glyphs judged, not boxes)")
    err, t = m.tool("audit")
    check(not err and "\nsilk: clean" in t, "audit agrees: silk clean")

    print("[board_diff]")
    err, t = m.tool("board_diff")
    print("      ", t.splitlines()[0][:150])
    check(not err and t.startswith("no changes against"), "after the undos: as saved")
    m.close()
finally:
    srv.stop()

print("\nRESULT:", "PASS" if not FAILS else f"FAIL ({len(FAILS)})")
sys.exit(1 if FAILS else 0)

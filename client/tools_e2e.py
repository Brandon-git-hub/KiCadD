"""Tools no other test covers well (headless): open_board, flush_ratsnest.

usage: python3 -I tools_e2e.py --build BUILD --demos DEMOS_DIR --work DIR

  * flush_ratsnest: stickhub's GND (a large net) has its ratsnest deferred while its tracks are
    deleted; flush rebuilds it and gives the full unrouted count; after undo it is 0 again
  * tool descriptions: tools/list sends a sentence or two per tool (each under 200 characters,
    parameters under 61, the whole list under 40k characters); tool_help gives a tool's full
    text; a tool's first error points at tool_help, the second does not; skills/kicadd/TOOLS.md
    is what kicadd-mcp --tool-docs prints (regenerate it when a description changes)
  * found by the blind design test (2026-11-22): delete_tracks nets + region delete only that net
    inside the box, and say per net what went; query_area ref= gives the part itself first;
    board_render layers ["back"] is the view from below (its silkscreen), front + back refused;
    run_drc's unconnected lines give a bottom-side pad's own layer (B.Cu), not the marker's
  * placement / routing for agents: move_footprints parts {ref: {x, y, rotation, side}} (absolute,
    one undo); auto_place spacing / edge 0 taken (not the defaults), its "no room" said per side;
    route to "zone" (the net's pour: here +5V's on B.Cu, a via at the end), the length against the
    end-to-end straight line and against the waypoints (project_e2e routes to pours on a clean board)
  * open_board: errors (no path, relative, a missing file, not a board: the old board comes
    back); unsaved changes refused unless discard=true; another project by .kicad_pcb and by
    .kicad_pro, with its schematic and rules; the agent history starts over; the same file
    again reloads it
"""

import argparse
import json
import subprocess
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
a = ap.parse_args()

build, work = os.path.abspath(a.build), os.path.abspath(a.work)
shutil.rmtree(work, ignore_errors=True)
shutil.copytree(os.path.join(a.demos, "stickhub"), os.path.join(work, "sh"))
shutil.copytree(os.path.join(a.demos, "pic_programmer"), os.path.join(work, "pic"))
stickhub = os.path.join(work, "sh", "StickHub.kicad_pcb")
pic = os.path.join(work, "pic", "pic_programmer.kicad_pcb")
pic_bytes = open(pic, "rb").read()
sock = os.path.join(work, "s.sock")
kicad_cli, kicadd_mcp, env = binaries(build)

srv = Server(build, sock, stickhub, env, os.path.join(work, "server.log"))
assert srv.start(), "api-server did not start"
try:
    m = Mcp([kicadd_mcp, "--socket", sock, "--stdio"], env)

    print("[flush_ratsnest]")
    err, t = m.tool("delete_tracks", {"nets": ["GND"]})
    check(not err and "excl. deferred nets: GND" in t, "GND's ratsnest deferred after delete_tracks")
    err, t = m.tool("flush_ratsnest")
    n = re.search(r"rebuilt (\d+) deferred nets; unconnected=(\d+)", t)
    print("      ", t.strip())
    check(not err and n and int(n.group(1)) >= 1 and int(n.group(2)) > 0, "flush: GND rebuilt, its pads unrouted")
    err, t = m.tool("flush_ratsnest")
    check(not err and t.startswith("rebuilt 0 deferred nets"), "flush again: nothing deferred")
    m.tool("undo")
    err, t = m.tool("flush_ratsnest")
    check(not err and t.strip().endswith("unconnected=0"), "after undo: routed again, unconnected=0")

    print("[blind-test fixes]")
    err, t = m.tool("delete_tracks", {"nets": ["GND"], "region": [145, 95, 150, 100]})
    n = re.search(r"(\d+) track\(s\)/via\(s\) deleted", t)
    check(not err and n and 0 < int(n.group(1)) < 30 and "note: deleted per net: GND " in t and "; " not in t.split("per net: ")[1].split("\n")[0],
          "delete_tracks nets + region: GND in the box only, listed per net (" + (n.group(1) if n else "?") + ")")
    m.tool("undo")
    err, t = m.tool("query_area", {"ref": "U1", "radius": 0.5})
    check(not err and t.splitlines()[1].startswith("FP U1 B.Cu @"), "query_area ref=U1: U1 itself first (B.Cu, its position)")
    err, t = m.tool("board_render", {"layers": ["back"]})
    check(not err and "B.Silkscreen" in t and "F.Silkscreen" not in t, "board_render layers [back]: the bottom's silkscreen")
    err, t = m.tool("board_render", {"nets": ["/U*D+"], "size": 400})
    check(not err and "not found" not in t, "board_render nets with a pattern (/U*D+): " + t.splitlines()[0][:70])
    err, t = m.tool("board_render", {"layers": ["front", "back"]})
    check(err and "one view at a time" in t, "front and back together: refused")
    m.tool("delete_tracks", {"nets": ["/U2D-"]})
    err, t = m.tool("run_drc", {"limit": 5})
    check(re.search(r"E unconnected_items U1\.\d+\[/U2D-\] x J3\.2\[/U2D-\] B\.Cu / F\.Cu", t),
          "run_drc: U1 (bottom) to J3 (top) unconnected: B.Cu / F.Cu, not the marker's F.Cu")
    m.tool("undo")

    print("[placement / routing for agents]")
    err, t = m.tool("move_footprints", {"parts": {"C21": {"x": 150, "y": 110, "rotation": 90, "side": "top"}}})
    err2, t2 = m.tool("query_area", {"ref": "C21", "radius": 0.1, "kinds": ["footprint"]})
    check(not err and t2.splitlines()[1].startswith("FP C21 F.Cu @150,110 r90"), "move_footprints parts: C21 to (150, 110), 90 deg, top: " + t2.splitlines()[1][:60])
    err, t = m.tool("move_footprints", {"parts": {"C21": {"x": 150, "y": 110, "rotation": 90, "side": "top"}}})
    check("nothing to do" in t, "the same place again: nothing to do")
    m.tool("undo")
    # a client with an old copy of the schema sends objects / booleans as strings: taken as meant
    err, t = m.tool("move_footprints", {"parts": '{"C21": {"x": 150, "y": 110}}'})
    check(not err and "C21" in t, "parts as a JSON string (a stale client schema): taken: " + t.splitlines()[0][:60])
    m.tool("undo")
    err, t = m.tool("auto_place", {"refs": ["C21"], "measure_only": "true", "silk_room": "false"})
    check(not err, "booleans as strings: taken: " + t.splitlines()[0][:60])
    err, t = m.tool("move_footprints", {"parts": {"C21": {"x": 1}}, "dx": 1})
    check(err and "not both" in t, "parts and dx together: refused")
    err, t = m.tool("auto_place", {"refs": ["U1"], "spacing": 0, "edge": 0, "measure_only": True})
    check(not err, "auto_place spacing 0 / edge 0 taken: " + t.splitlines()[0][:80])
    err, t = m.tool("auto_place", {"refs": ["C21"], "force": True, "rules": {"region": {"C21": [140, 80, 140.3, 80.3]}}})
    check("on the top," in t and "on the bottom" in t and "each side" in t, "no room: the area per side")
    m.tool("undo")
    err, t = m.tool("auto_place", {"refs": ["C21"], "force": True, "silk_room": False, "spacing": 0.1})
    check(not err and "placed 1 part" in t, "auto_place silk_room=false (courtyards only): C21 placed: " + t.splitlines()[0][:70])
    m.tool("undo")
    err, t = m.tool("route", {"from": "J3.1", "to": "zone", "dry_run": True})
    check("pour B.Cu" in t and ("would route" in t or "other spot(s) in the pour" in t),
          "route to zone: J3.1 tries the +5V pour on B.Cu (under U1 there is little room: " + t.strip()[-60:] + ")")
    err, t = m.tool("route", {"from": "R7.2", "to": "zone", "dry_run": True})
    check(err and ("no filled pour" in t or "already in its pour" in t), "route to zone of a net without one / in it: said: " + t.strip()[:90])

    print("[route_short vias, audit sections]")
    m.tool("delete_tracks", {"nets": ["/U2D-"]})
    err, t = m.tool("audit", {"sections": ["drc", "unrouted"]})
    secs = [ln for ln in t.splitlines() if not ln.startswith("  ")]
    check(not err and secs[1:] == ["drc:", "unrouted:"] and re.search(r"unrouted:\n  \d+ connection\(s\) on \d+ net\(s\): /U2D- ", t),
          "audit sections drc + unrouted: only those, the unrouted per net: " + " | ".join(secs))
    err, t = m.tool("audit", {"sections": ["nope"]})
    check(err and "no section nope" in t, "an unknown section: refused with the names")
    m.tool("undo")

    print("[tool descriptions]")
    tools = m.rpc("tools/list", {})["result"]["tools"]
    longest = max(tools, key=lambda x: len(x["description"]))
    check(all(len(x["description"]) < 200 for x in tools), f"each description under 200 characters (longest {longest['name']} {len(longest['description'])})")
    size = len(json.dumps(tools, ensure_ascii=False))
    check(size < 40000, f"tools/list {size} characters (under 40k; 67.7k before the briefs)")
    params = []

    def walk(name, s):
        if isinstance(s, dict):
            if isinstance(s.get("description"), str) and len(s["description"]) > 60:
                params.append(name)
            for v in s.values():
                walk(name, v)
    for x in tools:
        walk(x["name"], x["inputSchema"])
    check(not params, "parameter descriptions one line (60 characters): " + ", ".join(params))
    err, t = m.tool("tool_help", {"name": "pdn"})
    check(not err and "mounting" in t and "Arguments: " in t and len(t) > 600, "tool_help pdn: the full description")
    err, t = m.tool("tool_help", {"name": "nope"})
    check(err and "no tool nope" in t, "tool_help of no tool: the names")
    err, t = m.tool("query_net", {"net": "NO_SUCH_NET"})
    err2, t2 = m.tool("query_net", {"net": "NO_SUCH_NET"})
    check(err and 'tool_help {"name": "query_net"}' in t and "tool_help" not in t2, "the first error points at tool_help, the next not")
    docs = subprocess.run([kicadd_mcp, "--tool-docs"], env=env, capture_output=True, text=True, encoding="utf-8").stdout
    have = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "skills", "kicadd", "TOOLS.md"), encoding="utf-8").read()
    check(docs.replace("\r\n", "\n") == have.replace("\r\n", "\n") and "## pdn" in docs,
          "skills/kicadd/TOOLS.md is up to date (kicadd-mcp --tool-docs > skills/kicadd/TOOLS.md)")

    print("[open_board]")
    err, t = m.tool("open_board", {})
    check(err and "path" in t, "no path: refused")
    err, t = m.tool("open_board", {"path": os.path.join(work, "nope.kicad_pcb")})
    check(err and "no such board" in t, "a missing file: refused: " + t.strip()[:100])
    err, t = m.tool("open_board", {"path": "pic/pic_programmer.kicad_pcb"})
    check(err and "absolute" in t, "a relative path: refused")
    err, t = m.tool("open_board", {"path": pic})
    check(err and "unsaved changes" in t and "StickHub.kicad_pcb" in t, "unsaved changes (the flush / undo above): refused")
    check("the same as the saved file" in t, "and board_diff finds nothing: said so (undone back to it)")
    junk = os.path.join(work, "junk.kicad_pcb")
    open(junk, "w").write("this is not a board\n")
    err, t = m.tool("open_board", {"path": junk, "discard": True})
    print("      ", t.strip()[:200])
    check(err and "could not open" in t and "StickHub.kicad_pcb is open again" in t, "not a board: refused, stickhub reopened")
    err, t = m.tool("board_summary")
    check(not err and "footprints=94" in t, "stickhub is open")
    err, t = m.tool("board_diff")
    check(not err and t.startswith("no changes against"), "as saved (reopened from disk)")

    err, t = m.tool("move_footprints", {"refs": ["U1"], "dx": 1})
    check(not err, "an unsaved edit on stickhub")
    err, t = m.tool("open_board", {"path": pic})
    print("      ", t.strip().replace("\n", " | ")[:200])
    check(err and re.search(r"board_diff: \d+ change\(s\) since saved", t) and "U1" in t,
          "a real change: refused, with what board_diff finds (U1)")
    err, t = m.tool("open_board", {"path": pic, "discard": True})
    print("      ", t.splitlines()[0][:150])
    check(not err and "footprints=63" in t and "unsaved changes of StickHub.kicad_pcb were dropped" in t,
          "pic_programmer opened by .kicad_pcb, the drop said")
    err, t = m.tool("undo")
    check(not err and t.startswith("nothing to undo"), "a fresh history: " + t.splitlines()[0])
    err, t = m.tool("board_diff")
    check(not err and t.startswith("no changes against") and "pic_programmer.kicad_pcb" in t, "board_diff: against its own file")
    err, t = m.tool("sch_netlist")
    check(not err and "VPP" in t, "its schematic opened with it (sch_netlist)")
    err, t = m.tool("get_rules")
    check(not err and "POWER" in t, "its project's net classes (POWER)")
    m.tool("move_footprints", {"refs": ["R1"], "dx": 1})

    err, t = m.tool("open_board", {"path": os.path.join(work, "sh", "StickHub.kicad_pro"), "discard": True})
    check(not err and "footprints=94" in t, "stickhub opened by .kicad_pro")
    err, t = m.tool("board_diff")
    check(not err and t.startswith("no changes against"), "stickhub as saved: the unsaved U1 move went with the old board")
    check(open(pic, "rb").read() == pic_bytes, "pic_programmer's file untouched by the unsaved R1 move")
    err, t = m.tool("get_rules")
    check(not err and "POWER" not in t, "stickhub's rules, not pic_programmer's")
    m.tool("move_footprints", {"refs": ["U1"], "dx": 1})
    err, t = m.tool("open_board", {"path": stickhub, "discard": True})
    err, t2 = m.tool("board_diff")
    check(not err and t2.startswith("no changes against"), "the same file again: reloaded from disk")
    m.close()
finally:
    srv.stop()

print("\nRESULT:", "PASS" if not FAILS else f"FAIL ({len(FAILS)})")
sys.exit(1 if FAILS else 0)

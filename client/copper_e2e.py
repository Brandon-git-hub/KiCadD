"""Stitching vias and teardrops end to end (headless), on pic_programmer (GND poured on B.Cu).

usage: python3 -I copper_e2e.py --build BUILD --demos DEMOS_DIR --work DIR

  * stitch_vias refuses a net poured on one layer only, and says how to pour the other
  * with a GND pour on F.Cu too: dry run adds nothing; the grid vias, then DRC is clean, they are
    GND, none in a courtyard; spacing / area narrow it; undo takes them all back
  * teardrops on vias and through-hole pads: teardrop zones appear, DRC clean; undo / redo;
    remove takes them off; refs alone are those parts' pads
"""
import argparse, os, re, shutil, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from m5_e2e import Mcp, Server
ap = argparse.ArgumentParser(); ap.add_argument("--build", required=True); ap.add_argument("--demos", required=True)
ap.add_argument("--work", required=True)
a = ap.parse_args()
build, work = os.path.abspath(a.build), os.path.abspath(a.work)
shutil.rmtree(work, ignore_errors=True)
shutil.copytree(os.path.join(a.demos, "pic_programmer"), os.path.join(work, "p"))
pcb = os.path.join(work, "p", "pic_programmer.kicad_pcb")
sock = os.path.join(work, "s.sock"); env = dict(os.environ, KICAD_RUN_FROM_BUILD_DIR="1")
fails = []


def check(c, label):
    print(("  ok   " if c else "  FAIL ") + label); c or fails.append(label)


def summary(m):
    err, t = m.tool("board_summary")
    return {k: int(v) for k, v in re.findall(r"\b(vias|zones|tracks)=(\d+)", t)}


def drc_errors(m):
    err, t = m.tool("run_drc", {"limit": 3})
    return int(re.search(r"(\d+) errors", t).group(1)), t


srv = Server(build, sock, pcb, env, os.path.join(work, "server.log")); assert srv.start()
try:
    m = Mcp([os.path.join(build, "kicadd-mcp"), "--socket", sock, "--stdio"], env)
    base = summary(m)
    e0, _ = drc_errors(m)
    print("[stitch_vias]", base, "DRC errors", e0)
    err, t = m.tool("stitch_vias")
    check(err and "only on B.Cu" in t and "zones create" in t, "GND poured on B.Cu only: refused, with the way out")
    err, t = m.tool("zones", {"create": {"net": "GND", "layers": ["F.Cu"]}})
    check(not err, "a GND pour on F.Cu too")
    err, t = m.tool("stitch_vias", {"dry_run": True})
    n = int(re.search(r"would add (\d+) stitching", t).group(1)) if not err else 0
    check(not err and n > 50 and summary(m)["vias"] == base["vias"], f"dry run: would add {n}, added none")
    err, t = m.tool("stitch_vias")
    print("      ", " | ".join(t.splitlines()[:3]))
    added = int(re.search(r"added (\d+) stitching", t).group(1)) if not err else 0
    check(not err and added == n and "joining F.Cu B.Cu" in t and "all zones refilled" in t, f"added {added}, zones refilled")
    check(summary(m)["vias"] == base["vias"] + added, "the board has them")
    e1, d = drc_errors(m)
    check(e1 <= e0, f"DRC errors {e0} -> {e1}" + ("" if e1 <= e0 else ": " + d[:300]))
    err, q = m.tool("query_area", {"x": 140, "y": 95, "radius": 3})
    err, t = m.tool("query_net", {"net": "GND"})
    check(not err, "query_net GND")
    err, t = m.tool("stitch_vias", {"dry_run": True})
    check(not err and re.search(r"would add 0 stitching", t) is not None, "again: nothing left to add (the grid is done)")
    err, t = m.tool("undo")
    check(not err and "stitch" in t and summary(m)["vias"] == base["vias"], "undo: " + t.splitlines()[0])
    err, t = m.tool("stitch_vias", {"spacing": 10, "area": [80, 40, 140, 80], "islands": False})
    k = int(re.search(r"added (\d+) stitching", t).group(1)) if not err else 0
    check(not err and 0 < k < added / 4, f"spacing 10 mm in a 60x40 mm area: {k} via(s)")
    m.tool("undo")
    err, t = m.tool("stitch_vias", {"net": "NOPE"})
    check(err and "no net NOPE" in t, "an unknown net is refused")

    print("[teardrops]")
    err, t = m.tool("teardrops", {"dry_run": True})
    check(not err and "would switch teardrops on for" in t and summary(m)["zones"] == base["zones"] + 1, "dry run: " + t.splitlines()[0])
    err, t = m.tool("teardrops")
    print("      ", t.splitlines()[0])
    tear = re.search(r"0 -> (\d+) teardrop", t)
    td = int(tear.group(1)) if tear else 0
    check(not err and td > 50 and summary(m)["zones"] == base["zones"] + 1 + td, f"{td} teardrop zones on the board")
    e2, d = drc_errors(m)
    check(e2 <= e0, f"DRC errors {e0} -> {e2}" + ("" if e2 <= e0 else ": " + d[:300]))
    err, t = m.tool("teardrops")
    check(not err and "for 0 pad(s)" in t and "were already" in t, "again: all already on")
    err, t = m.tool("undo")
    check(not err and summary(m)["zones"] == base["zones"] + 1, "undo: the teardrops are gone: " + t.splitlines()[0])
    err, t = m.tool("redo")
    check(not err and summary(m)["zones"] == base["zones"] + 1 + td, "redo: back")
    m.tool("move_footprints", {"refs": ["R16"], "dx": 1, "dy": 0})
    check(summary(m)["zones"] >= base["zones"] + td - 4, "a moved part keeps its teardrops (made again)")
    m.tool("undo")
    err, t = m.tool("teardrops", {"remove": True})
    check(not err and re.search(r"-> 0 teardrop", t) is not None and summary(m)["zones"] == base["zones"] + 1, "remove: " + t.splitlines()[0])
    err, t = m.tool("teardrops", {"refs": ["R16", "R15"]})
    check(not err and "for 4 pad(s)" in t, "refs only: the 4 pads of R15 R16: " + t.splitlines()[0])
    err, t = m.tool("teardrops", {"refs": ["R999"]})
    check(err and "not found" in t, "an unknown part is refused")
    m.close()
finally:
    srv.stop()
print("\nRESULT:", "PASS" if not fails else f"FAIL ({len(fails)})")
sys.exit(1 if fails else 0)

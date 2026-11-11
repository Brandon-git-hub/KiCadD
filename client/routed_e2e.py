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
    m.close()
finally:
    srv.stop()
print("\nRESULT:", "PASS" if not fails else f"FAIL ({len(fails)})")
sys.exit(1 if fails else 0)

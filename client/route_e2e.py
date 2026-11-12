"""route / route_short / fanout end to end (headless).

usage: python3 -I route_e2e.py --build BUILD --kicad KICAD_SRC --work DIR

pic_programmer (2 layers, through-hole), with a keepout added across the straight line R9.2 -> D8.2:
  route        R9's connections ripped up and routed back from the ratsnest; dry run changes
               nothing; quality numbers in the reply; a via ([x, y]) and a waypoint are where they
               were asked; the keepout is gone round; a waypoint inside another net's pad fails
               and names it; undo takes a route back
  route_short  every track ripped up, then routed shortest first: dry run first, then for real as
               one edit; after a refill, DRC has nothing but unrouted connections; one undo
  fanout       through-hole pads are skipped, a net without a zone on the other side is warned
StickHub (2 layers, SMD, GND on both sides):
  fanout       GND ripped up, fanned out again: vias inside the other side's GND, DRC with
               nothing but unrouted connections, one undo
"""

import argparse
import os
import re
import shutil
import sys
import uuid

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from m5_e2e import FAILS, Mcp, Server, check  # noqa: E402

KEEPOUT = (150.6, 76.2, 152.3, 78.8)   # across R9.2 (149.225, 77.47) -> D8.2 (153.67, 77.47)


def keepout_block(x0, y0, x1, y1):
    return f'''	(zone
		(net 0)
		(net_name "")
		(layers "F.Cu" "B.Cu")
		(uuid "{uuid.uuid4()}")
		(name "KO_TEST")
		(hatch edge 0.5)
		(connect_pads
			(clearance 0)
		)
		(min_thickness 0.25)
		(filled_areas_thickness no)
		(keepout
			(tracks not_allowed)
			(vias not_allowed)
			(pads allowed)
			(copperpour allowed)
			(footprints allowed)
		)
		(fill
			(thermal_gap 0.5)
			(thermal_bridge_width 0.5)
		)
		(polygon
			(pts
				(xy {x0} {y0}) (xy {x1} {y0}) (xy {x1} {y1}) (xy {x0} {y1})
			)
		)
	)
'''


def drc_kinds(text):
    m = re.search(r"errors: (.*)", text)
    if not m or m.group(1).strip() == "none":
        return {}
    return {k: int(n) for k, n in re.findall(r"(\w+) (\d+)", m.group(1))}


def tracks(mcp):
    err, t = mcp.tool("board_summary")
    return int(re.search(r"tracks=(\d+)", t).group(1)), int(re.search(r"vias=(\d+)", t).group(1))


def unconnected(text):
    m = re.search(r"unconnected=(\d+)", text)
    return int(m.group(1)) if m else -1


def session(a, work, demo, prepare=None):
    proj = os.path.join(work, demo)
    shutil.copytree(os.path.join(a.kicad, "demos", demo), proj)
    pcb = [os.path.join(proj, f) for f in os.listdir(proj) if f.endswith(".kicad_pcb")][0]
    if prepare:
        text = open(pcb, encoding="utf-8").read()
        open(pcb, "w", encoding="utf-8").write(prepare(text))
    sock = os.path.join(work, demo[:6] + ".sock")
    env = dict(os.environ, KICAD_RUN_FROM_BUILD_DIR="1")
    server = Server(a.build, sock, pcb, env, os.path.join(work, demo + ".log"))
    check(server.start(), f"api-server up on {demo}")
    return server, Mcp([os.path.join(a.build, "kicadd-mcp"), "--socket", sock, "--stdio"], env)


def pic(a, work):
    def add_keepout(text):
        i = text.rindex("\n)")
        return text[:i + 1] + keepout_block(*KEEPOUT) + text[i + 1:]

    server, mcp = session(a, work, "pic_programmer", add_keepout)
    try:
        print("[route]")
        err, t = mcp.tool("delete_tracks", {"refs": ["R9"]})
        base = unconnected(t)
        check(not err and base == 2, f"R9 ripped up: {base} unrouted")
        n0 = tracks(mcp)

        err, t = mcp.tool("route", {"from": "R9.1", "dry_run": True})
        check(not err and "would route" in t and tracks(mcp) == n0, "dry run: " + t.split("\n")[0][:110])

        err, t = mcp.tool("route", {"from": "R9.1"})
        m = re.search(r"routed on (\S+), ([\d.]+) mm \(([\d.]+)x the straight line\), (\d+) segment\(s\), (\d+) via\(s\), "
                      r"width ([\d.]+), min gap ([\d.]+) to (.+)", t)
        check(not err and m and unconnected(t) == base - 1, "routed from the ratsnest: " + (t.split("\n")[0][:150]))
        check(m and float(m.group(3)) >= 1.0 and float(m.group(7)) >= 0.25 - 1e-6,
              f"quality: {m.group(3) if m else '?'}x, min gap {m.group(7) if m else '?'} mm (clearance 0.25)")

        err, t = mcp.tool("undo")
        check(not err and tracks(mcp) == n0, "undo takes the route back")

        err, t = mcp.tool("route", {"from": "R9.1", "via": [132, 74]})
        check(not err and "1 via(s)" in t, "via where asked: " + t.split("\n")[0][:120])
        err, q = mcp.tool("query_area", {"x": 132, "y": 74, "radius": 0.05, "kinds": ["via"]})
        check(re.search(r"VIA \S+ \[Net-\(Q2-C\)\] @132,74", q), "the via is at 132,74")
        mcp.tool("undo")

        # through a waypoint above the keepout: the route passes it
        err, t = mcp.tool("route", {"from": "R9.2", "to": "D8.2", "waypoints": [[151.45, 74.3]]})
        check(not err and "routed" in t, "waypoint: " + t.split("\n")[0][:120])
        err, q = mcp.tool("query_area", {"x": 151.45, "y": 74.3, "radius": 0.05, "kinds": ["track"]})
        check("[Net-(D8-A)]" in q, "a track of the net ends at the waypoint")
        mcp.tool("undo")

        # straight across is the keepout: the route goes round it
        err, t = mcp.tool("route", {"from": "R9.2", "to": "D8.2"})
        m = re.search(r"([\d.]+)x the straight line", t)
        check(not err and m and float(m.group(1)) > 1.05, "the keepout is gone round: " + t.split("\n")[0][:120])
        err, q = mcp.tool("query_area", {"x": (KEEPOUT[0] + KEEPOUT[2]) / 2, "y": (KEEPOUT[1] + KEEPOUT[3]) / 2,
                                         "radius": 1.2, "kinds": ["track"]})
        inside = []
        for tm in re.finditer(r"TRK \S+ \[Net-\(D8-A\)\] \S+ ([-\d.]+),([-\d.]+)>([-\d.]+),([-\d.]+)", q):
            x0, y0, x1, y1 = (float(v) for v in tm.groups())
            for i in range(11):
                px, py = x0 + (x1 - x0) * i / 10, y0 + (y1 - y0) * i / 10
                if KEEPOUT[0] < px < KEEPOUT[2] and KEEPOUT[1] < py < KEEPOUT[3]:
                    inside.append((px, py))
        check(not inside, f"no new track inside the keepout ({len(inside)} points)")
        mcp.tool("undo")

        # a waypoint in the middle of another net's pad
        err, t = mcp.tool("route", {"from": "R9.2", "to": "D8.2", "waypoints": [[140.081, 70.993]]})
        check(err and "not routed" in t and "in the way" in t and "R11.1" in t, "blocked, says by what: " + t.strip()[:170])
        err, t = mcp.tool("route", {"from": "R9.1", "to": "D8.2"})
        check(err and "different nets" in t, "different nets refused")
        mcp.tool("undo")   # the rip-up

        print("[route_short]")
        err, q = mcp.tool("query_area", {"x": 150, "y": 90, "radius": 500, "kinds": ["footprint"], "limit": 500})
        refs = sorted({m.group(1) for m in re.finditer(r"^FP (\S+)", q, re.M)})
        err, t = mcp.tool("delete_tracks", {"all": True})
        err, t = mcp.tool("run_drc")
        before = drc_kinds(t).get("unconnected_items", 0)
        err, t = mcp.tool("route_short", {"refs": refs, "dry_run": True})
        m = re.search(r"dry run: (\d+) connection\(s\), shortest first, (\d+) would be routed", t)
        check(not err and m and tracks(mcp) == (0, 0), "dry run: " + t.split("\n")[0])
        err, t = mcp.tool("route_short", {"refs": refs})
        m2 = re.search(r"(\d+) connection\(s\), shortest first, (\d+) routed, (\d+) not", t)
        check(not err and m2 and m and m2.group(2) == m.group(2) and int(m2.group(2)) >= 0.9 * int(m2.group(1)),
              "routed: " + t.split("\n")[0])
        check(all("in the way" in ln or "no unrouted" in ln for ln in t.split("not routed:\n")[-1].split("\n")[:3]
                  if ln.startswith("  ") and "not routed" in ln), "the failures say what is in the way")
        mcp.tool("refill_zones")
        err, t = mcp.tool("run_drc")
        kinds = drc_kinds(t)
        other = {k: n for k, n in kinds.items() if k != "unconnected_items"}
        check(not other and kinds.get("unconnected_items", 0) < before, f"DRC after a refill: {kinds}")
        err, t = mcp.tool("undo")
        check(not err and tracks(mcp) == (0, 0), "one undo takes all of it back")
        mcp.tool("undo")   # the rip-up

        print("[fanout, through-hole]")
        err, t = mcp.tool("fanout", {"nets": ["GND", "VCC"]})
        check(re.search(r"\d+ through-hole pad\(s\) skipped", t), "through-hole pads skipped: " + t.split("\n")[1][:60])
        check("warning: no copper zone of VCC" in t, "VCC has no zone to reach: warned")
    finally:
        mcp.close()
        server.stop()


def stickhub(a, work):
    server, mcp = session(a, work, "stickhub")
    try:
        print("[fanout, SMD]")
        err, t = mcp.tool("run_drc")
        base = drc_kinds(t)
        err, t = mcp.tool("fanout", {"nets": ["GND"], "dry_run": True})
        check("already joined" in t, "the routed board: " + t.split("\n")[-2 if t.endswith("\n") else -1][:80])
        mcp.tool("delete_tracks", {"nets": ["GND"]})
        mcp.tool("refill_zones")
        n0 = tracks(mcp)
        err, t = mcp.tool("fanout", {"nets": ["GND"], "dry_run": True})
        check(not err and tracks(mcp) == n0, "dry run: " + t.split("\n")[0])
        err, t = mcp.tool("fanout", {"nets": ["GND"]})
        m = re.search(r"(\d+) pad\(s\), (\d+) routed", t)
        check(not err and m and int(m.group(2)) >= 40 and "refilled" in t, "fanned out: " + t.split("\n")[0])
        err, t = mcp.tool("run_drc")
        kinds = drc_kinds(t)
        other = {k: n for k, n in kinds.items() if k != "unconnected_items" and n > base.get(k, 0)}
        check(not other, f"DRC: nothing new but unrouted connections ({kinds})")
        err, t = mcp.tool("undo")
        check(not err and tracks(mcp) == n0, "one undo takes the fanout back")
    finally:
        mcp.close()
        server.stop()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", required=True)
    ap.add_argument("--kicad", required=True)
    ap.add_argument("--work", required=True)
    a = ap.parse_args()

    work = os.path.abspath(a.work)
    if os.path.exists(work):
        shutil.rmtree(work)
    os.makedirs(work)
    pic(a, work)
    stickhub(a, work)

    print("\nRESULT:", "PASS" if not FAILS else f"FAIL ({len(FAILS)})")
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())

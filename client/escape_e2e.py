"""Fine-pitch escape on a 4-layer board (headless): beta.13's fixes from blind run S2 (RP2040,
QFN-56 0.4 mm, In1 GND / In2 +3V3 planes). General, no S2 board: a blank project, a generated
QFN-56 7x7 mm 0.4 mm pitch, EP 3.2 mm, JLC04161H-7628.

usage: python3 -I escape_e2e.py --build BUILD --kicad KICAD_SRC --work DIR

  vias into planes  route to=zone with a via waypoint [x, y, "In2.Cu"] in the QFN's pad ring:
                    one via there, into the pour (S2: a second via and a 2x detour); via [x, y]
                    (the other side) the same, the pour's layer found; from a point with the via
                    on it: only the via; query_area via_net gives spots in the pad ring, marked *;
                    stitch_vias on +3V3 (poured on In2.Cu only): refused without area, with an
                    area adds its via
  auto_place        a ring of 2 pitches + a track (0.95 mm) past U1's pads kept free of courtyards
                    (escape_room=0: closer); back_decaps: the caps may go under U1, a 2-pad part
                    that is no capacitor (SW1, at U1's RUN pin) stays on top (S2's SW2)
  width at pitch    a 0.4 mm class out of a 0.2 mm pad at 0.4 mm pitch: route necks to 0.3 mm
                    (gap = the clearance, accepted) and widens; a width asked is kept; route_short
                    necks into the pad, fanout's stubs out of it
  starved thermal   run_drc says both ways out (the pour solid, or a track / via as a spoke);
                    a track into the pour fixes one, the pour solid the rest
"""

import argparse
import os
import re
import shutil
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from m5_e2e import FAILS, Mcp, Server, binaries, check, make_project  # noqa: E402

QFN = "kicadd:QFN-56_7x7mm_P0.4mm_EP3.2mm"


def first(t):
    return (t or "").splitlines()[0] if t else ""


def vias_of(t):
    m = re.search(r"(\d+) via\(s\)", t or "")
    return int(m.group(1)) if m else -1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", required=True)
    ap.add_argument("--kicad", required=True)
    ap.add_argument("--work", required=True)
    a = ap.parse_args()

    work = os.path.abspath(a.work)
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(work)
    proj = make_project(a.kicad, work)
    pcb = os.path.join(proj, "blank.kicad_pcb")
    # two spokes wanted (KiCad's default; the blank project asks one): for the starved thermal
    for f in os.listdir(proj):
        if f.endswith(".kicad_pro"):
            t = open(os.path.join(proj, f), encoding="utf-8").read()
            open(os.path.join(proj, f), "w", encoding="utf-8").write(t.replace('"min_resolved_spokes": 1', '"min_resolved_spokes": 2'))
    sock = os.path.join(work, "s.sock")
    kicad_cli, kicadd_mcp, env = binaries(a.build)
    server = Server(a.build, sock, pcb, env, os.path.join(work, "server.log"))
    check(server.start(), "api-server up with the blank board")
    mcp = Mcp([kicadd_mcp, "--socket", sock, "--stdio"], env)
    try:
        err, t = mcp.tool("board_setup", {"stackup": "JLC04161H-7628"})
        check(not err and "4 layers" in t, "4 layers, JLC04161H-7628")
        err, t = mcp.tool("set_rules", {"preset": "jlcpcb_4layer", "classes": {"Default": {
            "clearance": 0.15, "track_width": 0.15, "via_diameter": 0.6, "via_drill": 0.3}}})
        check(not err, "JLCPCB 4-layer rules: " + re.sub(r"\s+", " ", t)[:100])
        err, t = mcp.tool("lib_create_footprint", {"kind": "qfn", "pins": 56, "pitch": 0.4, "body": 7, "ep": 3.2})
        check(not err and QFN in t, "a QFN-56 0.4 mm: " + first(t)[:80])
        # U1 at (130, 120): pin 1 at (126.57, 117.4), pins 1-14 down the left side 0.4 mm apart,
        # 15-28 along the bottom; the EP 128.4..131.6
        nets = {"1": "+3V3", "10": "+3V3", "22": "+3V3", "23": "+1V1", "57": "GND"}
        nets.update({str(p): f"SIG{p}" for p in (2, 3, 4, 5, 11, 12, 13, 14, 15, 16, 17, 18)})
        err, t = mcp.tool("add_footprint", {"footprint": QFN, "ref": "U1", "x": 130, "y": 120, "nets": nets})
        check(not err and t.startswith("U1 added"), "U1 placed")
        mcp.tool("zones", {"create": {"net": "GND", "layers": ["In1.Cu"]}})
        mcp.tool("zones", {"create": {"net": "+3V3", "layers": ["In2.Cu"]}})
        err, t = mcp.tool("refill_zones")
        check(not err, "In1.Cu GND and In2.Cu +3V3 planes poured")

        print("[vias into planes]")
        # a via waypoint to In2.Cu in the pad ring: the route ends in it
        err, t = mcp.tool("route", {"from": "U1.1", "to": "zone", "waypoints": [[127.7, 117.4, "In2.Cu"]], "width": 0.15})
        print("      ", first(t)[:220])
        check(not err and "-> via into the pour In2.Cu @127.7,117.4" in first(t) and vias_of(t) == 1
              and "routed on F.Cu -> In2.Cu," in t,
              "to zone with a via waypoint: one via there into In2.Cu, no second via past it")
        err, t = mcp.tool("route", {"from": "U1.10", "to": "zone", "via": [127.7, 121], "width": 0.15})
        print("      ", first(t)[:220])
        check(not err and "-> via into the pour In2.Cu @127.7,121" in first(t) and vias_of(t) == 1,
              "via [x, y]: the pour's layer (In2.Cu) found, one via")
        # from a point with the via on it (a stub's end): only the via
        err, t = mcp.tool("route", {"from": "U1.22", "to": [130.2, 125.5], "width": 0.2})
        check(not err and "routed" in first(t), "a stub out of U1.22")
        err, t = mcp.tool("route", {"from": [130.2, 125.5], "to": "zone", "via": [130.2, 125.5]})
        print("      ", first(t)[:220])
        check(not err and "via into the pour In2.Cu @130.2,125.5" in first(t) and vias_of(t) == 1
              and "is on the route's start" not in t, "from a point with the via on it: just the via")
        err, t = mcp.tool("query_net", {"net": "+3V3"})
        check(len(re.findall(r"^VIA ", t, re.M)) == 3, "three +3V3 vias, one per pin")

        # via spots in the pad ring (between the pads and the EP): allowed, marked
        err, t = mcp.tool("query_area", {"x": 127.7, "y": 119.4, "radius": 0.5, "via_net": "+3V3"})
        print("      ", " ".join(t.splitlines())[:220])
        check(not err and re.search(r"@127\.\d+,1(19|20)\.\d+~[\d.]+\*", t) and "inside an exposed-pad IC's pad ring" in t,
              "query_area via_net: spots in the QFN's pad ring, marked *")

        # stitch_vias on a net poured on one layer only
        err, t = mcp.tool("stitch_vias", {"net": "+3V3"})
        check(err and "only on In2.Cu" in t and "area" in t, "stitch_vias +3V3, In2.Cu only, no area: refused, said why")
        err, t = mcp.tool("stitch_vias", {"net": "+3V3", "area": [140, 130, 144, 134], "max": 1})
        print("      ", " ".join(t.splitlines())[:220])
        check(not err and re.search(r"\b1 via", t) and "poured on In2.Cu only" in t, "with an area: its via, the note")

        print("[auto_place escape room]")
        # two decaps and a 2-pad "button" on U1's pins, off the board; U1 fixed, the earlier tracks gone
        mcp.tool("delete_tracks", {"all": True})
        mcp.tool("lib_create_footprint", {"kind": "chip", "size": "0402", "prefix": "C"})
        mcp.tool("lib_create_footprint", {"kind": "chip", "size": "0603", "prefix": "R"})
        for ref, fp, x, n1 in (("C1", "kicadd:C_0402", 90, "+3V3"), ("C2", "kicadd:C_0402", 92, "+1V1"),
                               ("SW1", "kicadd:R_0603", 95, "RUN")):
            err, t = mcp.tool("add_footprint", {"footprint": fp, "ref": ref, "x": x, "y": 90, "nets": {"1": n1, "2": "GND"}})
            check(not err, f"{ref} added off the board")

        def placed():
            """{ref: (side, gap mm from U1's pad box to its courtyard box)}"""
            out = {}
            for ref in ("C1", "C2", "SW1"):
                _, q = mcp.tool("query_area", {"ref": ref, "radius": 0.1})
                m = re.search(r"FP " + ref + r" (\S+) @([\d.]+),([\d.]+)(?: r(-?\d+))? \(body ([\d.]+)x([\d.]+)", q or "")
                if not m:
                    continue
                x, y, w, h = float(m.group(2)), float(m.group(3)), float(m.group(5)), float(m.group(6))
                # U1's pads: 130 +/- 3.805, 120 +/- 3.805
                dx = max(126.195 - (x + w / 2), (x - w / 2) - 133.805, 0)
                dy = max(116.195 - (y + h / 2), (y - h / 2) - 123.805, 0)
                out[ref] = (m.group(1), max(dx, dy) if not (dx and dy) else (dx * dx + dy * dy) ** 0.5)
            return out

        err, t = mcp.tool("auto_place", {"fixed": ["U1"], "escape_room": 0})
        p0 = placed()
        check(not err and len(p0) == 3 and "escape room" not in t, f"escape_room=0: placed {p0}")
        mcp.tool("undo")
        err, t = mcp.tool("auto_place", {"fixed": ["U1"]})
        p1 = placed()
        print("      ", " | ".join(ln.strip() for ln in t.splitlines() if "escape" in ln or "placed" in ln)[:260])
        check(not err and "escape room kept free past the pads of U1 0.95 mm (0.40 mm pitch)" in t,
              "the default: U1's ring 2 x 0.4 pitch + the 0.15 track")
        check(len(p1) == 3 and all(g >= 0.93 for _, g in p1.values()) and min(g for _, g in p1.values()) > min(g for _, g in p0.values()),
              f"no courtyard in the ring: {p1} (without it {p0})")
        mcp.tool("undo")

        # back_decaps: the capacitors may go under U1; the 2-pad button tied to U1's RUN pin stays on top
        err, t = mcp.tool("auto_place", {"fixed": ["U1"], "back_decaps": True})
        p2 = placed()
        check(not err and p2.get("SW1", ("?",))[0] == "F.Cu", f"back_decaps: SW1 (not a capacitor) on top: {p2.get('SW1')}")
        check(any(v[0] == "B.Cu" for k, v in p2.items() if k.startswith("C")), f"a decap under U1: {p2}")

        print("[width at pitch]")
        # the Power class's 0.4 mm does not leave a 0.2 mm pad 0.4 mm from the next (0.15 clearance):
        # 0.2 + 2 x (0.2 - 0.15) = 0.3 mm does, its gap exactly the clearance
        mcp.tool("delete_footprints", {"refs": ["C1", "C2", "SW1"]})
        mcp.tool("delete_tracks", {"all": True})
        mcp.tool("add_footprint", {"footprint": "kicadd:R_0603", "ref": "R5", "x": 131, "y": 128, "nets": {"1": "+1V1", "2": "X5"}})
        err, t = mcp.tool("set_rules", {"classes": {"Power": {"track_width": 0.4, "clearance": 0.15}},
                                        "assign": {"+3V3": "Power", "+1V1": "Power"}})
        check(not err, "a Power class, 0.4 mm, on +3V3 / +1V1")
        err, t = mcp.tool("route", {"from": "U1.22", "to": [130.2, 127], "dry_run": True})
        print("      ", first(t)[:240])
        check(not err and "width 0.4 (netclass 'Power" in t and "necked to 0.30 mm out of U1.22" in t
              and "min gap 0.15 to pad U1.2" in t, "route: 0.4 mm past a 0.3 mm neck out of U1.22, its gap the clearance")
        err, t = mcp.tool("route", {"from": "U1.22", "to": [130.2, 127], "width": 0.4, "dry_run": True})
        check("not routed" in t and "tight at width 0.4" in t and "necked" not in t, "a width asked is kept (and said tight)")
        err, t = mcp.tool("route_short", {"refs": ["R5"], "dry_run": True, "detail": True})
        print("      ", " ".join(ln.strip() for ln in t.splitlines() if "R5.1" in ln)[:240])
        check(not err and "1 would be routed" in t and "necked to 0.30 mm into U1.23" in t, "route_short: necked into U1.23")
        err, t = mcp.tool("fanout", {"nets": ["+3V3"], "refs": ["U1"], "dry_run": True, "detail": True})
        check(not err and re.search(r"(\d+) would be routed, 0 not", t) and t.count("necked to 0.30 mm out of U1.") == 3,
              "fanout: the three +3V3 pins' stubs necked: " + first(t)[:80])

        print("[starved thermal]")
        # a GND pad at the end of a thin F.Cu strip of pour: one spoke of the two asked
        mcp.tool("add_footprint", {"footprint": "kicadd:R_0603", "ref": "R7", "x": 150, "y": 135, "nets": {"1": "GND", "2": "Y7"}})
        mcp.tool("add_footprint", {"footprint": "kicadd:R_0603", "ref": "R9", "x": 140, "y": 135, "nets": {"1": "Q9", "2": "GND"}})
        mcp.tool("zones", {"create": {"net": "GND", "layers": ["F.Cu"], "rect": [138, 134.3, 149.9, 135.7]}})
        mcp.tool("refill_zones")
        err, t = mcp.tool("run_drc", {"types": ["starved_thermal"]})
        adv = next((ln for ln in t.splitlines() if ln.startswith("starved_thermal (")), "")
        print("      ", adv[:240])
        check(not err and "starved_thermal 2" in t and '"connection": "solid"' in adv and '"to": [x, y]' in adv
              and '"to": "zone", "waypoints"' in adv, "run_drc: the starved thermals, with both ways out")
        # a spoke by hand: a track from R7.1 into the strip
        err, t = mcp.tool("route", {"from": "R7.1", "to": [147.8, 135], "width": 0.3})
        mcp.tool("refill_zones")
        err, t = mcp.tool("run_drc", {"types": ["starved_thermal"]})
        check("starved_thermal 1" in t and "R7.1" not in t, "a track into the pour: R7.1 no longer starved")
        err, t = mcp.tool("zones", {"nets": ["GND"], "layers": ["F.Cu"], "connection": "solid"})
        err, t = mcp.tool("run_drc", {"types": ["starved_thermal"]})
        check("starved_thermal" not in t.split("filter")[0], "the pour solid: none left")
    finally:
        mcp.close()
        server.stop()

    print("\nRESULT:", "PASS" if not FAILS else f"FAIL ({len(FAILS)})")
    sys.exit(1 if FAILS else 0)


if __name__ == "__main__":
    main()

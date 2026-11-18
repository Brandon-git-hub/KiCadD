"""Controlled impedance from the stackup (headless): the impedance tool and set_rules impedance /
diff_impedance.

usage: python3 -I impedance_e2e.py --build BUILD --kicad KICAD_SRC --work DIR

Blank project (m5_e2e's), no parts:
  2 layers        50 ohm on F.Cu is a wide microstrip over B.Cu (the whole board between),
                  B.Cu not poured is said
  JLC04161H-7628  board_setup stackup: F.Cu / B.Cu microstrip over In1 / In2 (0.2104 mm 7628),
                  the same width on both; In1 / In2 striplines narrower; a width's impedance
                  back again; a 90 ohm pair at a gap, its gap at a width; the source note
  set_rules       RF {impedance: 50} -> the F.Cu width, the note gives the other layers';
                  USB {diff_impedance: 90, diff_pair_gap: 0.15}; a pair with no gap takes the
                  clearance; impedance_layer In1.Cu; refusals; undo puts the class back
  planes          a GND zone on In1.Cu: F.Cu's plane is no longer said to be missing
"""

import argparse
import os
import re
import shutil
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from m5_e2e import FAILS, Mcp, Server, binaries, check, make_project  # noqa: E402


def widths(t):
    """{layer: (width, gap, z)} from the impedance tool's lines."""
    out = {}
    for ln in t.splitlines():
        m = re.match(r"(\S+\.Cu): width ([\d.]+) mm(?:, gap ([\d.]+) mm)? -> ([\d.]+) ohm", ln)
        if m:
            out[m.group(1)] = (float(m.group(2)), float(m.group(3) or 0), float(m.group(4)))
    return out


def rule_class(rules, name):
    m = re.search(r"^\s*" + re.escape(name) + r": (.*)$", rules, re.M)
    return m.group(1) if m else ""


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
    proj = make_project(a.kicad, work)
    pcb = os.path.join(proj, "blank.kicad_pcb")
    sock = os.path.join(work, "s.sock")
    kicad_cli, kicadd_mcp, env = binaries(a.build)
    server = Server(a.build, sock, pcb, env, os.path.join(work, "server.log"))
    check(server.start(), "api-server up with the blank board")
    mcp = Mcp([kicadd_mcp, "--socket", sock, "--stdio"], env)
    try:
        # a GND net for the planes: a part on it
        err, t = mcp.tool("lib_create_footprint", {"kind": "chip", "size": "0805"})
        name = re.search(r"made (kicadd:\S+?):", t or "")
        err, t = mcp.tool("add_footprint", {"footprint": name.group(1) if name else "kicadd:R_0805", "ref": "R1",
                                            "x": 20, "y": 15, "nets": {"1": "GND", "2": "VCC"}})
        check(not err and t.startswith("R1"), "R1 on GND / VCC")

        print("[2 layers]")
        err, t = mcp.tool("impedance", {"z": 50})
        print("      ", " | ".join(t.splitlines()[:3])[:260])
        w = widths(t)
        check(not err and set(w) == {"F.Cu", "B.Cu"}, "both layers answered")
        check(2.0 < w.get("F.Cu", (0,))[0] < 3.5, f"50 ohm over 1.5 mm of FR4 is wide: {w.get('F.Cu')}")
        check("microstrip over B.Cu" in t and "B.Cu has no copper zone yet" in t, "the plane is B.Cu, not poured yet")

        print("[JLC04161H-7628]")
        err, t = mcp.tool("board_setup", {"stackup": "JLC04161H-7628"})
        check(not err and t.startswith("board setup"), "the fab's stackup: " + t.splitlines()[0][:80])
        err, t = mcp.tool("impedance", {"z": 50})
        print("      ", "\n        ".join(t.splitlines()))
        w = widths(t)
        check(not err and list(w) == ["F.Cu", "In1.Cu", "In2.Cu", "B.Cu"], "four layers, top to bottom")
        f = w.get("F.Cu", (0, 0, 0))
        check(0.33 < f[0] < 0.42 and abs(f[2] - 50) < 0.1, f"F.Cu 50 ohm over 0.2104 mm 7628: {f[0]} mm")
        check(w.get("B.Cu", (0,))[0] == f[0], "B.Cu the same (the stackup is symmetric)")
        check(0 < w.get("In1.Cu", (1,))[0] < f[0], f"In1.Cu (a stripline) narrower: {w.get('In1.Cu')}")
        check("F.Cu: width 0.3" in t and "(microstrip over In1.Cu, 0.2104 mm Er 4.4, copper 0.035 mm, mask" in t, "F.Cu's cross-section")
        check("stripline between F.Cu (0.2104 mm) and In2.Cu (1.065 mm)" in t, "In1.Cu's cross-section")
        check("impedance stackup" in t, "the note: the fab's stackup's dielectrics")

        err, t = mcp.tool("impedance", {"width": f[0], "layers": ["F.Cu"]})
        z = widths(t).get("F.Cu", (0, 0, 0))[2]
        check(not err and abs(z - 50) < 0.5, f"that width back: {z} ohm")

        err, t = mcp.tool("impedance", {"z_diff": 90, "gap": 0.2, "layers": ["F.Cu"]})
        p = widths(t).get("F.Cu", (0, 0, 0))
        check(not err and 0.1 < p[0] < f[0] and p[1] == 0.2 and abs(p[2] - 90) < 0.1,
              f"a 90 ohm pair at 0.2 mm: {p[0]} mm wide")
        err, t = mcp.tool("impedance", {"z_diff": 90, "width": p[0], "layers": ["F.Cu"]})
        q = widths(t).get("F.Cu", (0, 0, 0))
        check(not err and abs(q[1] - 0.2) < 0.003, f"its gap at that width: {q[1]} mm")
        err, t = mcp.tool("impedance", {"width": p[0], "gap": 0.2, "layers": ["F.Cu"]})
        check(not err and abs(widths(t).get("F.Cu", (0, 0, 0))[2] - 90) < 0.5, "the pair back: ~90 ohm")

        err, t = mcp.tool("impedance", {"z": 50, "z_diff": 90})
        check(err and "one at a time" in t, "z and z_diff together: refused")
        err, t = mcp.tool("impedance", {})
        check(err and "z_diff" in t, "nothing asked: refused")
        err, t = mcp.tool("impedance", {"z": 50, "layers": ["In3.Cu"]})
        check(err and "not a copper layer" in t, "a layer the board has not: refused")
        for z in (300, 1000):
            err, t = mcp.tool("impedance", {"z": z, "layers": ["F.Cu"]})
            check(not err and "F.Cu: no width gives that impedance" in t, f"{z} ohm, out of reach, is said: " + t.splitlines()[0][:90])

        print("[set_rules]")
        err, t = mcp.tool("set_rules", {"classes": {"RF": {"impedance": 50},
                                                    "USB": {"diff_impedance": 90, "diff_pair_gap": 0.15}}})
        print("      ", "\n        ".join(ln for ln in t.splitlines() if "ohm" in ln or "note" in ln)[:900])
        check(not err and f"{f[0]:.3f} mm tracks for 50 ohm" in t, "RF: the F.Cu width for 50 ohm")
        check(re.search(r"on other layers it takes In1\.Cu [\d.]+, In2\.Cu [\d.]+ mm", t) is not None,
              "the note gives the inner layers' widths")
        check("In1.Cu has no copper zone yet" in t, "and that In1.Cu is not poured")
        m = re.search(r"pairs ([\d.]+) mm wide, 0\.150 mm apart for 90 ohm", t)
        check(m is not None and "the width at the gap given" in t, "USB: the width at the 0.15 mm gap")
        err, r = mcp.tool("get_rules")
        rf, usb = rule_class(r, "RF"), rule_class(r, "USB")
        check(f"track {f[0]:.3f}".rstrip("0") in rf or f"track {f[0]:g}" in rf, "get_rules: RF's track width: " + rf[:80])
        check(m and f"diff pair {float(m.group(1)):g} gap 0.15" in usb, "get_rules: USB's pair: " + usb[:100])

        err, t = mcp.tool("undo")
        check(not err and t.startswith("undid: set rules"), "undo: " + t.splitlines()[0])
        err, r = mcp.tool("get_rules")
        check(not rule_class(r, "RF") and not rule_class(r, "USB"), "the classes are gone")

        cl = re.search(r"Default: clearance ([\d.]+)", r)
        err, t = mcp.tool("set_rules", {"classes": {"USB": {"diff_impedance": 90}}})
        check(not err and cl and "the width at the class's clearance as the gap" in t
              and f"mm wide, {float(cl.group(1)):.3f} mm apart" in t,
              f"no gap given: the clearance ({cl and cl.group(1)} mm) is the gap")
        err, t = mcp.tool("set_rules", {"classes": {"USB": {"diff_impedance": 90, "diff_pair_width": 0.2}}})
        check(not err and "the gap at the width given" in t and "pairs 0.200 mm wide" in t, "a width given: the gap")
        err, t = mcp.tool("set_rules", {"classes": {"SL": {"impedance": 50, "impedance_layer": "In1.Cu"}}})
        sl = widths(mcp.tool("impedance", {"z": 50, "layers": ["In1.Cu"]})[1]).get("In1.Cu", (0,))[0]
        check(not err and f"{sl:.3f} mm tracks for 50 ohm" in t and "keep these nets on In1.Cu" in t,
              f"impedance_layer In1.Cu: {sl} mm")

        err, t = mcp.tool("set_rules", {"classes": {"X": {"impedance": 50, "track_width": 0.3}}})
        check(err and "not two of them" in t, "impedance with a track width: refused")
        err, t = mcp.tool("set_rules", {"classes": {"X": {"diff_impedance": 90, "diff_pair_width": 0.2, "diff_pair_gap": 0.2}}})
        check(err and "not both" in t, "diff_impedance with width and gap: refused")
        err, t = mcp.tool("set_rules", {"classes": {"X": {"impedance": 50, "impedance_layer": "User.1"}}})
        check(err and "not a copper layer" in t, "impedance_layer not copper: refused")
        err, t = mcp.tool("set_rules", {"classes": {"X": {"impedance": -5}}})
        check(err and "above 0" in t, "a negative impedance: refused")

        print("[planes]")
        err, t = mcp.tool("zones", {"create": {"net": "GND", "layers": ["In1.Cu"]}})
        check(not err and t.startswith("new zone"), "a GND plane on In1.Cu")
        err, t = mcp.tool("impedance", {"z": 50, "layers": ["F.Cu", "In1.Cu"]})
        lines = {ln.split(":")[0]: ln for ln in t.splitlines()}
        check("no copper zone" not in lines.get("F.Cu", "x"), "F.Cu's plane is poured now")
        check("In2.Cu has no copper zone yet" in lines.get("In1.Cu", ""), "In1.Cu still lacks In2.Cu")
    finally:
        mcp.close()
        server.stop()

    print("\nRESULT:", "PASS" if not FAILS else f"FAIL ({len(FAILS)})")
    sys.exit(1 if FAILS else 0)


if __name__ == "__main__":
    main()

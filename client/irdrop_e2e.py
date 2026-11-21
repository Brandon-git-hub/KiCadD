"""DC IR drop in audit (headless), on the demos as they are, and board_render marks. Made for an
agent: without limits a header line (rows with ir_limit / ir_nets), no verdicts; with currents
{net: A} the drops in mV, with ir_drop {glob: mV} / via_current {glob: A} warnings. Measures only.

usage: python3 -I irdrop_e2e.py --build BUILD --demos DEMOS_DIR --work DIR

  cm5_minima  6 rails, sources guessed (an inductor for M2_3V3, the CM5 module's "+3.3v_(Output)"
              pins, the USB-C connector for +5V); +1V8 against a hand calculation (one 0.96 mm,
              0.15 mm track: 3.14 mOhm); a track ending along another (a T) connects; linear in
              the current; ir_drop / via_current judge only rails with a current; ir_loads /
              ir_source; ir_mesh_mm; GND by ir_nets from the connector; deleted tracks leave the load
              without copper; bad arguments refused; board_render marks (far apart: both in the picture)
  vme-wren    FPGA rails (MGT_*, P0V85) by name, supply-named nets with no source part left out and
              said; the plane rails meshed under the cell budget, in seconds
  jetson      domain-suffixed signals (I2C5_SCL_1V8) and controls (CARRIER_PWR_ON) are no rails;
              DC-DC outputs found by their V_OUT pin
"""

import argparse
import os
import re
import shutil
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from m5_e2e import FAILS, Mcp, Server, binaries, check  # noqa: E402


def section(t, name="ir_drop"):
    out, on = [], False
    for ln in t.splitlines():
        if not ln.startswith("  "):
            on = ln == name + ":"
        elif on:
            out.append(ln.strip())
    return out


def board(a, work, demo, fname):
    dst = os.path.join(work, demo[:6])
    shutil.copytree(os.path.join(a.demos, demo), dst)
    sock = os.path.join(work, demo[:6] + ".sock")
    _, kicadd_mcp, env = binaries(a.build)
    srv = Server(a.build, sock, os.path.join(dst, fname), env, os.path.join(work, demo[:6] + ".log"))
    check(srv.start(), "api-server up with " + fname)
    return srv, Mcp([kicadd_mcp, "--socket", sock, "--stdio"], env)


def row(lines, net):
    got = [ln for ln in lines if ln.startswith("info: " + net + ": ")]
    return got[0] if got else ""


def drop(text):
    """The worst drop (mV) of a row: "drop 3.87-7.12 mV" -> 7.12."""
    m = re.search(r"drop (?:[\d.]+-)?([\d.]+) mV", text)
    return float(m.group(1)) if m else -1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", required=True)
    ap.add_argument("--demos", required=True)
    ap.add_argument("--work", required=True)
    a = ap.parse_args()
    a.build, a.demos = os.path.abspath(a.build), os.path.abspath(a.demos)
    work = os.path.abspath(a.work)
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(work)

    # ---- cm5_minima
    print("[cm5_minima]")
    srv, m = board(a, work, "cm5_minima", "CM5_MINIMA_3.kicad_pcb")
    try:
        err, t = m.tool("audit")
        ir = section(t)
        print("      ", ir[0][:220] if ir else t[:200])
        check(not err and len(ir) == 1, "by default one ir_drop line")
        check(ir and ir[0].startswith("info: 6 rail(s) solved: worst drop 141 mV per A on +BATT at Module301.76 (from CN1.1)")
              and "No verdicts" in ir[0] and "The worst: +BATT:" in ir[0], "the header: 6 rails, the worst per A with its row")

        _, t = m.tool("audit", {"ir_limit": 10})
        ir = section(t)
        check(len(ir) == 7 and not [ln for ln in ir if ln.startswith("warning")], "ir_limit: a row per rail, no verdicts")
        r = row(ir, "M2_3V3")
        check("L701.2 (guessed: an inductor) -> U703 (2 pads), per A (no current given)" in r and "mesh 0.1 mm" in r
              and "busiest via 1 A" in r and "loss in tracks" in r, "M2_3V3: from the buck's inductor, losses, the busiest via")
        r = row(ir, "+3V3_PI")
        check("Module301.84 (+3.3v_(Output)), Module301.86 (guessed: an output pin)" in r,
              "+3V3_PI: the CM5 module's +3.3v_(Output) pins are the source")
        r = row(ir, "+1V8")
        print("       +1V8:", r[:160])
        check(abs(drop(r) - 3.15) < 0.05, "+1V8: 3.15 mV per A, as one 0.96 mm 0.15 mm track (1.72e-8 * 0.96 / (0.15 * 0.035) = 3.14)")
        r = row(ir, "VBUS_USB2.0")
        check("USB701 (4 pads)" in r and "no copper to the source" not in r,
              "VBUS_USB2.0: a track ending along another (a T) connects all four VBUS pads")

        _, t = m.tool("audit", {"ir_nets": ["+5V"], "currents": {"+5V": 2}})
        r2 = row(section(t), "+5V")
        _, t = m.tool("audit", {"ir_nets": ["+5V"], "currents": {"+5V": 4}})
        r4 = row(section(t), "+5V")
        print("       +5V at 2 A / 4 A:", drop(r2), drop(r4))
        check(drop(r2) > 0 and abs(drop(r4) / drop(r2) - 2) < 0.01 and "IPC-2221 10 C" in r2,
              "linear: twice the current, twice the drop; with a current the loaded track's IPC width")

        _, t = m.tool("audit", {"currents": {"+5V": 2, "+3V3_PI": 0.5}, "ir_drop": {"*": 10, "+5V": 5},
                                "via_current": {"*": 0.5}})
        ir = section(t)
        check("warning: ir_drop +5V (max 5 mV): 1 rail(s) judged, 1 over" in ir, "ir_drop +5V 5 mV: over")
        check(any(ln.startswith("warning: +5V: 7.12 mV at U702.3 (126.31, 71.46), 2.12 over;") for ln in ir),
              "the +5V row: the worst load, where, how much over")
        check(any(ln.startswith("warning: ir_drop * (max 10 mV): 1 rail(s) judged, 1 over; 4 not judged, no current given")
                  for ln in ir), "rails with no current are not judged, and said so")
        check(any(ln.startswith("warning: +5V: 2 via(s) over 0.5 A (at 2 A): 0.522 A at (87.83, 48.83)") for ln in ir),
              "via_current 0.5 A: the two busiest +5V vias")

        given = re.search(r"currents given for (\d+) of (\d+)", t)
        check(given and int(given.group(1)) == 2 and int(given.group(1)) <= int(given.group(2)),
              f"currents given for {given and given.group(1)} of {given and given.group(2)} (the two given, of the rails in the answer)")
        _, t = m.tool("audit", {"ir_nets": ["+5V"], "ir_loads": {"+5V": "U702:1.5,U701"}, "currents": {"+5V": 2}})
        r = row(section(t), "+5V")
        check("-> U702:1.5A, U701, 2 A" in r and drop(r) > 10, "ir_loads: U702 draws 1.5 A, U701 the rest")
        _, t = m.tool("audit", {"ir_nets": ["M2_3V3"], "ir_source": {"M2_3V3": "C999"}})
        check("not solved: ir_source C999: no pad on M2_3V3" in row(section(t), "M2_3V3"), "ir_source with no pad on the rail")
        _, t = m.tool("audit", {"ir_nets": ["+5V"], "ir_loads": {"+5V": "U702:x"}})
        check('ir_loads U702:x: not "REF:A"' in row(section(t), "+5V"), "ir_loads with a bad current")
        err, t = m.tool("audit", {"ir_drop": {"*": "big"}})
        check(err and "mV, a number" in t, "an ir_drop limit that is not a number is refused")
        err, t = m.tool("audit", {"ir_source": {"*": 5}})
        check(err and "a string" in t, "an ir_source that is not a string is refused")

        _, t = m.tool("audit", {"ir_nets": ["+5V"], "ir_mesh_mm": 0.05})
        r = row(section(t), "+5V")
        print("       +5V at 0.05 mm:", drop(r))
        check("mesh 0.05 mm" in r and abs(drop(r) - 3.56) < 0.15, "ir_mesh_mm 0.05: within a few % of the 0.1 mm mesh")
        err, t = m.tool("audit", {"ir_mesh_mm": 0})
        check(err and "a positive number" in t, "ir_mesh_mm 0 refused")

        _, t = m.tool("audit", {"ir_nets": ["GND"]})
        r = row(section(t), "GND")
        check("CN1.2 (guessed: a connector)" in r and "loss in planes" in r, "GND by ir_nets: from the connector, mostly planes")
        _, t = m.tool("audit", {"ir_nets": ["NOPE*"]})
        check("warning: no rail matches NOPE* (a net with pads on two parts or more)" in section(t), "a glob that matches nothing")

        m.tool("delete_tracks", {"nets": ["+1V8"]})
        _, t = m.tool("audit", {"ir_nets": ["+1V8"]})
        check("not solved: no load has copper to the source" in row(section(t), "+1V8"), "tracks deleted: no copper to the load")
        m.tool("undo")
        _, t = m.tool("audit", {"ir_nets": ["+1V8"]})
        check(abs(drop(row(section(t), "+1V8")) - 3.15) < 0.05, "undo: solved again")

        err, t = m.tool("board_render", {"nets": ["+5V"], "marks": [[126.31, 71.46, "U702.3 7.1 mV"], [87.83, 48.83]],
                                         "path": os.path.join(work, "marks.png")})
        check(not err and "orange rings: the points asked for" in t and os.path.getsize(os.path.join(work, "marks.png")) > 10000,
              "board_render marks: rings the hotspots, zoomed to them")
        err, t = m.tool("board_render", {"marks": [[80, 30, "a"], [130, 78, "b"]]})
        ext = re.search(r"x ([\d.]+) \.\. ([\d.]+), y ([\d.]+) \.\. ([\d.]+) mm", t)
        check(ext and float(ext.group(1)) < 80 and float(ext.group(2)) > 130 and float(ext.group(3)) < 30
              and float(ext.group(4)) > 78, "two marks far apart: the picture holds both (their union plus the margin)")
        err, t = m.tool("board_render", {"marks": [[1, "x"]]})
        check(err and "marks is [[x, y" in t, "bad marks refused")
    finally:
        m.close()
        srv.stop()

    # ---- vme-wren: FPGA rails, planes
    print("[vme-wren]")
    srv, m = board(a, work, "vme-wren", "vme-wren.kicad_pcb")
    try:
        t0 = time.time()
        _, t = m.tool("audit", {"ir_limit": 20})
        dt = time.time() - t0
        ir = section(t)
        print("      ", ir[0][:200], f"({dt:.1f} s)")
        check(ir and ir[0].startswith("info: 14 rail(s) solved") and dt < 30, f"14 rails solved, the audit in {dt:.1f} s")
        check("3 supply-named net(s) with no source part, left out, the most copper first: VCC_PSPLL (8 pads, 215 mm2), MGT_0V85 (6 pads, 118 mm2), /power-supply-3/5VPG (3 pads, 17 mm2)" in ir[0],
              "nets with no source part are left out and named, the most copper first")
        _, t = m.tool("audit", {"currents": {"VCC_PSPLL": 1}})
        given = re.search(r"currents given for (\d+) of (\d+)", t)
        check("currents for VCC_PSPLL not used: no source part on it" in t and given and given.group(1) == "0",
              f"a current for VCC_PSPLL (no source part, left out): said not used, not counted ({given and given.group(0)})")
        check(row(ir, "MGT_1V8") and row(ir, "P0V85"), "FPGA rails by name (MGT_1V8, P0V85)")
        r = row(ir, "P3V3")
        check("IC40.34 (VOUT)" in r and re.search(r"mesh 0\.(1\d|2[0-4])\d* mm", r) and "loss in tracks 1%, planes 9" in r,
              "P3V3: from the regulator's VOUT, mostly planes, a mesh under 0.25 mm")
        check("(coarse: antipads and necks under about" in r and "ir_mesh_mm 0.1" in r, "a coarse mesh is said, with how to refine it")
    finally:
        m.close()
        srv.stop()

    # ---- jetson: what is a rail
    print("[jetson]")
    srv, m = board(a, work, "jetson-agx-thor-baseboard", "jetson-agx-thor-baseboard.kicad_pcb")
    try:
        _, t = m.tool("audit", {"ir_limit": 60})
        ir = section(t)
        names = [ln.split(":")[1].strip() for ln in ir[1:] if ln.startswith("info: ") and ": " in ln]
        print("      ", ir[0][:200])
        check(not any("_1V8" in n and ("I2C" in n or "LED" in n or "ID" in n) for n in names)
              and not any("PWR_ON" in n or "PWR_BAD" in n for n in names), "no domain-suffixed signals or controls as rails")
        check("the most copper first: +5V (55 pads, 7339 mm2), VCC (67 pads, 3499 mm2), +1V8 (58 pads, 2771 mm2), "
              "+20V (11 pads, 277 mm2)" in ir[0],
              "the main supplies with no source part first by copper (fills + tracks), +20V with 11 pads among them")
        check(any(ln.startswith("info: /DCDC/5V_OUT: U12.18 (V_{OUT}) (guessed: an output pin)") for ln in ir),
              "a DC-DC output from its V_OUT pin")
    finally:
        m.close()
        srv.stop()

    print("\nRESULT:", "PASS" if not FAILS else f"FAIL ({len(FAILS)})")
    sys.exit(1 if FAILS else 0)


if __name__ == "__main__":
    main()

"""Impedance along the line in audit (headless), on the demos as they are. Made for an agent:
without targets a header line (rows with z_limit / z_nets), no verdicts; with z_target /
z_diff_target {glob: ohm} warnings. Measures only.

usage: python3 -I zprofile_e2e.py --build BUILD --demos DEMOS_DIR --work DIR

  cm5_minima  the header: diff pairs first (main Zdiff range), nets' main Z0 range, the net that
              varies most, with where and why (a gap in a plane: the next copper past it);
              z_limit rows; z_target / z_diff_target judged rule by rule, the length off and
              where; z_tolerance widens it; bad arguments refused
  vme-wren    PCIe REFCLK's Zdiff on B.Cu and on In9.Cu the impedance tool's for the same width
              and gap (one arithmetic); with In1.Cu's zones taken off and a GND pour laid on
              In1.Cu under 10 mm of a F.Cu track (USERIO_A.IO30), that stretch is over In1.Cu copper (not the
              In3.Cu plane) at the Z0 the untouched board's F.Cu has (over In1.Cu), the rest over
              In3.Cu across In1 and In2
  jetson      in seconds
"""

import argparse
import os
import re
import shutil
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from m5_e2e import FAILS, check  # noqa: E402
from xtalk_e2e import board, section  # noqa: E402

QUIET = {"ir_nets": ["NO_RAIL"]}


def imp(t):
    return section(t, "impedance")


def z_of(t, layer):
    m = re.search(re.escape(layer) + r": width [\d.]+ mm(?:, gap [\d.]+ mm)? -> ([\d.]+) ohm", t)
    return float(m.group(1)) if m else None


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
        err, t = m.tool("audit", QUIET)
        x = imp(t)
        print("      ", x[0][:300] if x else t[:200])
        check(not err and len(x) == 1, "by default one impedance line")
        check(x and x[0].startswith("info: Z along 39 net(s) and 18 diff pair(s), from each piece's width, layer and the "
                                    "copper over / under it (0.5 mm around a net's own pads and vias left out)")
              and "; diff pairs: main Zdiff 103-113 ohm, 0 vary 10% or more" in x[0]
              and "; nets: main Z0 36-57 ohm, 6 vary" in x[0] and "No verdicts" in x[0],
              "the header: diff pairs first, each kind's main Z range")
        check(x and "the most: /CM5/ETH_nLED_ACTIVITY: 26.9 mm, main Z0 40.1 ohm (In2.Cu 0.25 mm); 6.6 mm 10% or more off "
                    "it, the longest 3.9 mm at 66.4 ohm (In2.Cu 0.25 mm, over In4.Cu (a gap in In3.Cu)) at (119, 41.5)" in x[0],
              "the net varying most: where, and why (a gap in In3.Cu, the copper past it)")

        _, t = m.tool("audit", dict(QUIET, z_limit=3))
        x = imp(t)
        check(len(x) == 5 and x[1].startswith("info: /USB_C.D_P/N (pair): 13.2 mm, main Zdiff 102.9 ohm (F.Cu 0.14 mm gap 0.155)")
              and x[4] == "info: 54 more net(s) (z_limit)", "z_limit: rows, diff pairs first")

        _, t = m.tool("audit", dict(QUIET, z_target={"*HDMI*": 50}, z_diff_target={"*ETH*": 100}))
        x = imp(t)
        check("warning: z_target *HDMI* (50 ohm +/-10%, 1 mm off allowed): 4 net(s) judged, 2 off" in x,
              "z_target: HDMI's DDC 2 of 4 off 50 ohm")
        check("warning: /CM5/HDMI_PI.SDA: 11.6 mm of 13.4 off 50 ohm, the longest 11.6 mm at 56.9 ohm (B.Cu 0.13 mm) "
              "at (96.58, 68.91)" in x, "the row: how much off, at what, where")
        check(any(ln.startswith("warning: /CM5/HDMI_PI.SCL: 1.1 mm of 11.2 off 50 ohm") and ln.endswith("; main 53 ohm (B.Cu 0.15 mm)")
                  for ln in x), "its main Z when that is not the off one")
        check("warning: z_diff_target *ETH* (100 ohm +/-10%, 1 mm off allowed): 4 pair(s) judged, 4 off" in x
              and any(ln.startswith("warning: /CM5/ETH_PI.TRD2_P/N (pair): 36.8 mm of 36.8 off 100 ohm, the longest 35.1 mm "
                                    "at 112.6 ohm (B.Cu 0.13 mm gap 0.25)") for ln in x),
              "z_diff_target: the Ethernet pairs at 112.6 ohm, all off 100")

        _, t = m.tool("audit", dict(QUIET, z_diff_target={"*ETH*": 100}, z_tolerance=15))
        check("info: z_diff_target *ETH* (100 ohm +/-15%, 1 mm off allowed): 4 pair(s) judged, all within" in imp(t),
              "z_tolerance 15: within")
        _, t = m.tool("audit", dict(QUIET, z_target={"*ETH_PI.TRD*": 50}))
        check(any("z_target *ETH_PI.TRD*: no net (not a diff pair member) with tracks matches" in ln for ln in imp(t)),
              "z_target on pair members only: said (z_diff_target is for pairs)")
        _, t = m.tool("audit", dict(QUIET, z_nets=["*CEC"]))
        x = imp(t)
        check(len(x) == 2 and x[1].startswith("info: /CM5/HDMI_PI.CEC: "), "z_nets: that net's row")

        err, t = m.tool("audit", {"z_target": {"*": 0}})
        check(err and "ohm, a positive number" in t, "a 0 ohm target refused")
        err, t = m.tool("audit", {"z_tolerance": -1})
        check(err and "a positive number" in t, "a negative tolerance refused")
    finally:
        m.close()
        srv.stop()

    # ---- vme-wren: the same arithmetic as the impedance tool; a local pour on a signal layer between
    print("[vme-wren]")
    srv, m = board(a, work, "vme-wren", "vme-wren.kicad_pcb")
    try:
        _, t = m.tool("audit", dict(QUIET, z_nets=["*REFCLK*"]))
        x = imp(t)
        print("      ", x[1][:300] if len(x) > 1 else x)
        row = next((ln for ln in x if ln.startswith("info: /PCIE.REFCLK_P/N (pair)")), "")
        _, b = m.tool("impedance", {"width": 0.115, "gap": 0.125, "layers": ["B.Cu"]})
        _, i = m.tool("impedance", {"width": 0.1, "gap": 0.16, "layers": ["In9.Cu"]})
        check(f"main Zdiff {z_of(b, 'B.Cu')} ohm (B.Cu 0.115 mm gap 0.125)" in row
              and f"at {z_of(i, 'In9.Cu')} ohm (In9.Cu 0.1 mm gap 0.16)" in row,
              f"REFCLK: B.Cu {z_of(b, 'B.Cu')}, In9.Cu {z_of(i, 'In9.Cu')} ohm, as the impedance tool has them")
        _, f = m.tool("impedance", {"width": 0.15, "layers": ["F.Cu"]})
        z_in1 = z_of(f, "F.Cu")
        check(z_in1 and "microstrip over In1.Cu, 0.119 mm" in f, f"F.Cu 0.15 mm over In1.Cu: {z_in1} ohm")
    finally:
        m.close()
        srv.stop()

    print("[vme-wren, In1.Cu not poured, a GND pour on it under USERIO_A.IO30]")
    io30 = "/vme_interface/VMEPX_USERIO_A.IO30"   # F.Cu only, 0.15 mm, straight down x 212.94 from y 268.9 to 298.9
    srv, m = board(a, work, "vme-wren", "vme-wren.kicad_pcb", unpour=("In1.Cu",))
    try:
        _, f = m.tool("impedance", {"width": 0.15, "layers": ["F.Cu"]})
        z_in3 = z_of(f, "F.Cu")
        err, t = m.tool("zones", {"create": {"net": "GND", "layers": ["In1.Cu"], "rect": [211, 278, 215, 288]}})
        check(not err and t.startswith("new zone"), "a 4 x 10 mm GND pour on In1.Cu under IO30 (F.Cu)")
        _, t = m.tool("audit", dict(QUIET, z_nets=[io30]))
        x = imp(t)
        print("      ", x[1][:400] if len(x) > 1 else x)
        row = x[1] if len(x) > 1 else ""
        check(f"main Z0 {z_in3} ohm (F.Cu 0.15 mm)" in row, f"the rest over In3.Cu across In1 and In2: {z_in3} ohm")
        mm_ = re.search(r"the longest ([\d.]+) mm at ([\d.]+) ohm \(F\.Cu 0\.15 mm, over In1\.Cu copper \(not the In3\.Cu plane\)\)", row)
        check(mm_ and abs(float(mm_.group(1)) - 10) < 0.6 and float(mm_.group(2)) == z_in1,
              f"10 mm over the pour: {mm_ and mm_.group(1)} mm at {mm_ and mm_.group(2)} ohm, the untouched board's F.Cu ({z_in1})")
        m.tool("undo")
        _, t = m.tool("audit", dict(QUIET, z_nets=[io30]))
        x = imp(t)
        check(len(x) > 1 and "over In1.Cu copper" not in x[1], "undo: the pour gone, no stretch over In1.Cu")
    finally:
        m.close()
        srv.stop()

    # ---- jetson
    print("[jetson]")
    srv, m = board(a, work, "jetson-agx-thor-baseboard", "jetson-agx-thor-baseboard.kicad_pcb")
    try:
        t0 = time.time()
        _, t = m.tool("audit", QUIET)
        dt = time.time() - t0
        x = imp(t)
        print("      ", x[0][:300] if x else t[:200])
        check(dt < 20, f"audit in {dt:.1f} s")
        check(x and re.search(r"diff pairs: main Zdiff \d+-\d+ ohm, \d+ vary", x[0]), "the header with the pairs' range")
    finally:
        m.close()
        srv.stop()

    print("\nRESULT:", "PASS" if not FAILS else f"FAIL ({len(FAILS)})")
    sys.exit(1 if FAILS else 0)


if __name__ == "__main__":
    main()

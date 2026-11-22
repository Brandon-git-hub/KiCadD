"""Diff pairs and lengths in audit (headless), on the demos as they are. Made for an agent: without
limits a header line (or rows with length_limit / length_nets), no verdicts; with skew / uncoupled /
pair_gap / length_match / max_length / min_length {glob: mm} warnings. Checks only, no routing.

usage: python3 -I length_e2e.py --build BUILD --demos DEMOS_DIR --work DIR

  cm5_minima  18 routed pairs: the header names the worst skew (USB2P, 6.28 mm); rows with
              length_limit, the gap target from the net class (100ohm 0.25); skew / uncoupled
              judged rule by rule; pair_gap passes (breakouts at pins and vias left out); the class
              gap set to 0.3 (set_rules) makes the 100ohm pairs off target, undo puts it back;
              length_match names what the short ETH pair lacks; max_length; deleting one member's
              tracks shows as skew; globs that match nothing; bad limits refused
  stickhub    pairs with no class gap: each layer's median is the target; pair_gap finds U3D+/- (0.15 vs 0.17)
  vme-wren    the DDR4 clock pair routed at 0.12 mm against its class's 0.25
  jetson      144 pairs in seconds; Default's 5 mm gap is not taken as a target; per layer medians
"""

import argparse
import os
import re
import shutil
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from m5_e2e import FAILS, Mcp, Server, binaries, check  # noqa: E402


def section(t, name):
    """The lines of one audit section ("diff_pair:", "length:")."""
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


def warnings(lines):
    return [ln for ln in lines if ln.startswith("warning:")]


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
        dp = section(t, "diff_pair")
        print("      ", dp[0][:200] if dp else t[:200])
        check(not err and len(dp) == 2 and dp[1].startswith("info: pair order: ") and not section(t, "length"),
              "by default the diff_pair header and the pair order line, no length section")
        check(dp and dp[0].startswith("info: 18 routed diff pair(s) (14 not routed yet): worst skew 6.28 mm (/CM5/USB2P.D_P/N (pair))")
              and "No verdicts" in dp[0] and "The worst skew: /CM5/USB2P.D_P/N (pair): 16.23 / 22.51 mm" in dp[0],
              "the header: 18 pairs, the worst skew (USB2P, 6.28 mm) with its row")

        _, t = m.tool("audit", {"length_limit": 30})
        dp = section(t, "diff_pair")
        check(len(dp) == 20 and not warnings(dp), "length_limit: a row per pair (with the pair order line), no verdicts")
        d0 = [ln for ln in dp if ln.startswith("info: /CM5/DPHY0_P.D0_P/N (pair): 35.52 / 35.51 mm, skew 0.01 mm;")]
        check(d0 and "gap 0.25" in d0[0] and "(target 0.25, class 100ohm)" in d0[0] and "uncoupled 0 mm" in d0[0]
              and "vias 1 / 1" in d0[0], "a row: both lengths, skew, the gap against the class's 0.25, uncoupled, vias")
        usb = [ln for ln in dp if ln.startswith("info: /USB_C.D_P/N (pair): 22.69 / 28.68 mm")]
        check(usb and "class 90ohm" in usb[0] and "longest 7.29 at (86.16, 50.72)" in usb[0],
              "the USB-C pair: uncoupled with its longest stretch and where")

        _, t = m.tool("audit", {"skew": {"*USB*": 0.15, "*": 0.5}, "uncoupled": {"*": 3}})
        dp = section(t, "diff_pair")
        check("warning: skew *USB* (max 0.15 mm): 2 pair(s) judged, 2 over" in dp, "skew *USB*: 2 judged, 2 over")
        check("info: skew * (max 0.5 mm): 16 pair(s) judged, all within" in dp, "skew *: the rest, a passing count line")
        check(any("/CM5/USB2P.D_P/N (pair): skew 6.28 mm (16.23 / 22.51; /CM5/USB2P.D_P needs +6.13 to 6.28 mm)" in ln for ln in dp),
              "a skew row says which member needs how much")
        check("warning: uncoupled * (max 3 mm): 18 pair(s) judged, 6 over" in dp, "uncoupled 3 mm: 6 over")

        _, t = m.tool("audit", {"pair_gap": {"*": 0.02}})
        check("info: pair_gap * (±0.02 mm): 18 pair(s) judged, all within" in section(t, "diff_pair"),
              "pair_gap 0.02: all within (breakouts at pins and vias are not off target)")

        _, t = m.tool("set_rules", {"classes": {"100ohm": {"diff_pair_gap": 0.3}}, "drc": False})
        _, t = m.tool("audit", {"pair_gap": {"*": 0.02}})
        dp = section(t, "diff_pair")
        print("      ", [ln[:160] for ln in dp[1:3]])
        check(any(re.match(r"warning: pair_gap \* \(±0.02 mm\): 18 pair\(s\) judged, 1\d over", ln) for ln in dp)
              and any("(target 0.3, class 100ohm)" in ln for ln in dp),
              "set_rules 100ohm diff_pair_gap 0.3: the 100ohm pairs are off their target")
        m.tool("undo")
        _, t = m.tool("audit", {"pair_gap": {"*": 0.02}})
        check("info: pair_gap * (±0.02 mm): 18 pair(s) judged, all within" in section(t, "diff_pair"), "undo: within again")

        _, t = m.tool("audit", {"length_match": {"*ETH_PI.TRD*": 0.3}, "max_length": {"*PCIE*": 50}})
        ln_ = section(t, "length")
        check(any(ln.startswith("warning: length_match *ETH_PI.TRD* (within 0.3 mm): 4 net(s) / pair(s), 43.54") for ln in ln_)
              and "warning: /CM5/ETH_PI.TRD0_P/N (pair): 43.54 mm (mean), needs +0.18 to +0.48 mm" in ln_,
              "length_match: the 4 ETH pairs, TRD0 needs +0.18 to +0.48 mm")
        check("warning: max_length *PCIE* (50 mm): 12 net(s) judged, 7 longer" in ln_
              and "warning: /CM5/PCIE_PI.CLK_N: 56.92 mm, 6.92 over" in ln_, "max_length *PCIE*: 7 longer, the worst first")

        _, t = m.tool("audit", {"length_nets": ["*HDMI_PI.CEC"]})
        check("info: /CM5/HDMI_PI.CEC: 37.05 mm (track 35.54, 1 via(s) 1.51 mm); F.Cu, B.Cu" in section(t, "length"),
              "length_nets: a net's length, track plus via span")

        m.tool("delete_tracks", {"nets": ["/CM5/HDMI_PI.D0_N"]})
        _, t = m.tool("audit", {"skew": {"*HDMI*": 0.1}})
        check(any(ln.startswith("warning: /CM5/HDMI_PI.D0_P/N (pair): skew 17.49 mm") and "not fully routed" in ln
                  for ln in section(t, "diff_pair")), "one member's tracks deleted: the skew is its length, not fully routed")
        m.tool("undo")
        _, t = m.tool("audit", {"skew": {"*HDMI*": 0.1}})
        check("info: skew *HDMI* (max 0.1 mm): 4 pair(s) judged, all within" in section(t, "diff_pair"), "undo: the HDMI pairs match again")

        _, t = m.tool("audit", {"length_match": {"NOPE*": 1}})
        check("warning: no net matches NOPE*" in section(t, "length"), "a glob that matches nothing is said")
        err, t = m.tool("audit", {"skew": {"*": "small"}})
        check(err and "mm, a number" in t, "a limit that is not a number is refused")
    finally:
        m.close()
        srv.stop()

    # ---- stickhub: no class gap, the median
    print("[stickhub]")
    srv, m = board(a, work, "stickhub", "StickHub.kicad_pcb")
    try:
        _, t = m.tool("audit", {"pair_gap": {"*": 0.02}, "length_limit": 10})
        dp = section(t, "diff_pair")
        print("      ", [ln[:170] for ln in dp[:3]])
        check("warning: pair_gap * (±0.02 mm): 8 pair(s) judged, 5 over" in dp, "pair_gap: 5 of 8 pairs off their median")
        check(any(re.match(r"warning: /U3D\+/- \(pair\): gap off its target by more than 0.02 mm for 5\.\d+ mm", ln)
                  and "(target 0.17, the layer's median)" in ln for ln in dp), "U3D+/-: 5 mm off the layer's median (0.17)")
    finally:
        m.close()
        srv.stop()

    # ---- a USB-C's doubled pads (A6 / B6, A7 / B7): no false pair order; move_footprints says it
    print("[usb-c pair order]")
    import tiny_board
    tiny = os.path.join(work, "tiny")
    os.makedirs(tiny)
    two = lambda p, n: [("1", (-0.5 if p else 0.5, 0), (0.4, 0.6), "D_P"), ("2", (0.5 if p else -0.5, 0), (0.4, 0.6), "D_N")]
    tiny_board.write(os.path.join(tiny, "tiny.kicad_pcb"), [
        # in a row, as on a USB-C: B7 A6 A7 B6 = N P N P (the first P is A6, the first N is B7)
        ("J1", (100, 100), "F", [("B7", (-0.75, 0), (0.3, 1), "D_N"), ("A6", (-0.25, 0), (0.3, 1), "D_P"),
                                 ("A7", (0.25, 0), (0.3, 1), "D_N"), ("B6", (0.75, 0), (0.3, 1), "D_P")]),
        ("U1", (100, 110), "F", two(True, False)),     # P left, N right
        ("U3", (100, 120), "F", two(False, True)),     # P right, N left: swapped against U1
    ])
    _, kicadd_mcp, env = binaries(a.build)
    sock = os.path.join(work, "tiny.sock")
    srv = Server(a.build, sock, os.path.join(tiny, "tiny.kicad_pcb"), env, os.path.join(work, "tiny.log"))
    check(srv.start(), "api-server up with the tiny board")
    m = Mcp([kicadd_mcp, "--socket", sock, "--stdio"], env)
    try:
        _, t = m.tool("audit")
        po = [ln for ln in section(t, "diff_pair") if "pair order" in ln]
        print("      ", po[0][:220] if po else t[:200])
        check(po and "1 of 1 pair(s)" in po[0] and po[0].endswith("D_P / D_N: U1 - U3"),
              "only U1 - U3 swapped: J1's doubled pads (the pad toward each part) are not")
        err, t = m.tool("move_footprints", {"refs": ["U3"], "rotate": 180})
        check(not err and "pair order" not in t, "U3 turned: in order, nothing said")
        err, t = m.tool("move_footprints", {"parts": {"U1": {"rotation": 180}}})
        note = next((ln for ln in t.splitlines() if "pair order" in ln), "")
        print("      ", note[:200])
        check(not err and note.endswith("D_P / D_N: U1 - U3"),
              "U1 turned: the move says its pair is swapped against U3 now (J1's N P N P row fits either order)")
        _, t = m.tool("audit")
        check("1 of 1 pair(s)" in t, "and audit agrees")
    finally:
        m.close()
        srv.stop()

    # ---- vme-wren: a pair against its class
    print("[vme-wren]")
    srv, m = board(a, work, "vme-wren", "vme-wren.kicad_pcb")
    try:
        _, t = m.tool("audit", {"pair_gap": {"*DDR4*": 0.02}})
        dp = section(t, "diff_pair")
        check(any(ln.startswith("warning: /DDR4-PS.CK_P/N (pair): gap off its target") and "worst 0.12" in ln
                  and "(target 0.25, class DDR4_CMD)" in ln for ln in dp),
              "the DDR4 clock pair: routed at 0.12 mm against its class's 0.25")
    finally:
        m.close()
        srv.stop()

    # ---- jetson: size, Default's gap
    print("[jetson]")
    srv, m = board(a, work, "jetson-agx-thor-baseboard", "jetson-agx-thor-baseboard.kicad_pcb")
    try:
        t0 = time.time()
        err, t = m.tool("audit", {"length_nets": ["*USBSS2*"]})
        dt = time.time() - t0
        dp = section(t, "diff_pair")
        check(not err and dt < 20, f"audit in {dt:.1f} s")
        rx = [ln for ln in dp if ln.startswith("info: /SoM_IO2/USBSS2.RX+/- (pair): 193.29 / 193.29 mm")]
        check(rx and "median 0.12 In7.Cu, 0.21 F.Cu" in rx[0], "USBSS2.RX: per layer medians, not Default's 5 mm")
        _, t = m.tool("audit", {"pair_gap": {"*": 0.02}})
        check(any(re.match(r"warning: pair_gap \* \(±0.02 mm\): 144 pair\(s\) judged, \d over", ln) for ln in section(t, "diff_pair")),
              "pair_gap over 144 pairs: a few off target")
    finally:
        m.close()
        srv.stop()

    print("\nRESULT:", "PASS" if not FAILS else f"FAIL ({len(FAILS)})")
    sys.exit(1 if FAILS else 0)


if __name__ == "__main__":
    main()

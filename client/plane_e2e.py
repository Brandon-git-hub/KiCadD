"""Reference planes in audit (headless), on the demos as they are. Made for an agent: without
limits an overview per net (measured, worst first, no verdicts); with plane_gap / return_via
{glob: mm} (two_sided, bga_return_via_mm) warnings.

usage: python3 -I plane_e2e.py --build BUILD --demos DEMOS_DIR --work DIR

  cm5_minima      6 layers, GND on In1 / In4. Overview: USB_C.D_P (a pair) has its farthest return
                  via 3.17 mm away, nothing is a warning. return_via {"*HDMI*": 1.5}: D0_N (1.6 mm)
                  fails, at 2 it passes; an exact name beats the glob; two_sided on the HDMI clock
                  pair: one side only; plane_nets globs; bad limits refused
  stickhub        2 layers, B.Cu poured GND / +5V: the USB pairs lead the overview (they cross the
                  split); plane_gap on D+ makes it a warning; deleting D+'s tracks takes it away,
                  undo brings it back
  pic_programmer  2 layers, GND on the bottom only: the bottom layer has no plane (said once); R8's
                  net has a 6.1 mm gap; no via changes planes
  vme-wren        12 layers: vias in IC14's BGA field say so (784 pads, the GND vias there);
                  bga_return_via_mm applies there
  video           4 layers, GND / +5V: HD_PAL- 166 mm past the edge of the In1 GND plane with
                  plane_gap; vias between +5V and GND grouped (capacitor distance); in seconds
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
    """The lines of one audit section ("plane:", "return_via:")."""
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

    # ---- cm5_minima: return vias
    print("[cm5_minima]")
    srv, m = board(a, work, "cm5_minima", "CM5_MINIMA_3.kicad_pcb")
    try:
        err, t = m.tool("audit")
        pl, rv = section(t, "plane"), section(t, "return_via")
        check(not err and any(re.match(r"info: 8\d signal net\(s\) measured", ln) for ln in pl),
              "overview: the signal nets measured (+BATT is a supply)")
        check(any(ln.startswith("info: 77 signal via(s) change reference planes") for ln in rv), "77 vias change planes")
        usb = [ln for ln in rv if ln.startswith("info: /USB_C.D_P (pair): 1 via(s); farthest return via 3.17 mm")]
        print("      ", usb[0][:160] if usb else rv[:2])
        check(usb, "USB_C.D_P (a pair): its return via 3.17 mm away, measured")
        check(not warnings(pl) and not warnings(rv), "no limits: no verdicts")
        check(len(rv) <= 12 and any("more net(s) (plane_limit)" in ln for ln in rv), "the overview stops at plane_limit (10)")

        _, t = m.tool("audit", {"return_via": {"*HDMI*": 1.5}})
        rv = warnings(section(t, "return_via"))
        check(any("HDMI_PI.D0_N (pair)" in ln and "return via 1.6 mm (max 1.5 for *HDMI*)" in ln for ln in rv),
              "return_via *HDMI* 1.5: D0_N (1.6 mm) is a warning")
        check(not any("USB" in ln for ln in rv), "nets no glob matches are not judged")
        _, t = m.tool("audit", {"return_via": {"*HDMI*": 2}})
        check(not any("D0_N" in ln for ln in warnings(section(t, "return_via"))), "at 2 mm D0_N passes")
        _, t = m.tool("audit", {"return_via": {"*HDMI*": 1.5, "/CM5/HDMI_PI.D0_N": 2}})
        rv = warnings(section(t, "return_via"))
        check(not any("D0_N" in ln for ln in rv) and any("D1_P" in ln for ln in rv), "an exact name beats the glob")
        _, t = m.tool("audit", {"return_via": {"*HDMI*": 1.5}, "two_sided": ["*HDMI_PI.CK*"]})
        check(any("HDMI_PI.CK_N (pair)" in ln and "return via on one side only" in ln for ln in warnings(section(t, "return_via"))),
              "two_sided: the HDMI clock pair has return vias on one side only")
        _, t = m.tool("audit", {"plane_nets": ["*HDMI_PI.D0*", "NOPE*"]})
        rv, pl = section(t, "return_via"), section(t, "plane")
        check(any("2 signal via(s) change" in ln for ln in rv), "plane_nets globs: the D0 pair only")
        check(any("no net matches NOPE*" in ln for ln in pl), "a glob that matches nothing is said")
        err, t = m.tool("audit", {"return_via": {"*": "near"}})
        check(err and "mm, a number" in t, "a limit that is not a number is refused")
    finally:
        m.close()
        srv.stop()

    # ---- stickhub: a split under the USB pairs
    print("[stickhub]")
    srv, m = board(a, work, "stickhub", "StickHub.kicad_pcb")
    try:
        _, t = m.tool("audit")
        pl = section(t, "plane")
        print("      ", pl[1][:160] if len(pl) > 1 else pl)
        check(any(ln.startswith("info: 40 signal net(s) measured: 5 cross a plane split") for ln in pl), "5 nets cross a split")
        check(len(pl) > 1 and re.match(r"info: /U\dD[+-] \(pair\): .* split crossing", pl[1]), "a USB pair leads the overview")
        _, t = m.tool("audit", {"plane_gap": {"/D?": 5}})
        d = [ln for ln in warnings(section(t, "plane")) if ln.startswith("warning: /D+ on F.Cu crosses a plane split")]
        check(d and "+5V (B.Cu)" in d[0] and "GND (B.Cu)" in d[0], "plane_gap: D+ crossing the B.Cu GND / +5V split is a warning")
        check(not any("/U3D" in ln for ln in warnings(section(t, "plane"))), "other nets are not judged")
        m.tool("delete_tracks", {"nets": ["/D+"]})
        _, t = m.tool("audit", {"plane_gap": {"/D?": 5}})
        check(not any("/D+ on F.Cu" in ln for ln in section(t, "plane")), "no D+ crossing without its tracks")
        m.tool("undo")
        _, t = m.tool("audit", {"plane_gap": {"/D?": 5}})
        check(any("/D+ on F.Cu crosses" in ln for ln in section(t, "plane")), "undo: the crossing is back")
    finally:
        m.close()
        srv.stop()

    # ---- pic_programmer: two layers, one plane
    print("[pic_programmer]")
    srv, m = board(a, work, "pic_programmer", "pic_programmer.kicad_pcb")
    try:
        _, t = m.tool("audit")
        pl, rv = section(t, "plane"), section(t, "return_via")
        check(any(ln.startswith("info: bottom_layer: no zone on top_layer") for ln in pl), "the bottom layer has no plane (said once)")
        check(any(ln.startswith("info: Net-(R8-Pad1): ") and "longest gap 6.1 mm" in ln for ln in pl), "R8's net: a 6.1 mm gap")
        check(not rv, "no via changes planes: nothing said")
    finally:
        m.close()
        srv.stop()

    # ---- vme-wren: BGA fields
    print("[vme-wren]")
    srv, m = board(a, work, "vme-wren", "vme-wren.kicad_pcb")
    try:
        _, t = m.tool("audit", {"plane_limit": 400})
        bga = [ln for ln in section(t, "return_via") if "in IC14's BGA field (" in ln]
        print("      ", bga[0][bga[0].find("; 1 in") if bga else 0:][:120] if bga else "no BGA line")
        check(bga and re.search(r"IC14's BGA field \(\d+ GND vias, 784 pads\)", bga[0]), "vias in IC14's BGA field say so")
        _, t = m.tool("audit", {"return_via": {"*": 0.1}, "bga_return_via_mm": 50, "plane_limit": 400})
        rv = warnings(section(t, "return_via"))
        check(rv and not any("BGA field" in ln for ln in rv), "bga_return_via_mm 50: no via in a BGA field fails")
        _, t = m.tool("audit", {"return_via": {"*": 0.1}, "plane_limit": 400})
        check(any("BGA field" in ln for ln in warnings(section(t, "return_via"))), "without it they are judged by the glob")
    finally:
        m.close()
        srv.stop()

    # ---- video: past the plane's edge, different plane nets, the time
    print("[video]")
    srv, m = board(a, work, "video", "video.kicad_pcb")
    try:
        t0 = time.time()
        err, t = m.tool("audit")
        dt = time.time() - t0
        check(not err and dt < 20, f"whole-board audit in {dt:.1f} s")
        check(any(re.match(r"info: 5\d\d via\(s\) go between planes of \+5V / GND", ln) for ln in section(t, "return_via")),
              "vias between +5V and GND grouped, with the capacitor distance")
        _, t = m.tool("audit", {"plane_gap": {"/HD_PAL-": 10}})
        pl = warnings(section(t, "plane"))
        print("      ", pl[:1])
        check(any(ln.startswith("warning: /HD_PAL- on top_copper runs 166.") and "past the edge of GND (In1.Cu)" in ln
                  and "(max 10 for /HD_PAL-)" in ln for ln in pl), "HD_PAL- runs 166 mm past the edge of the In1 GND plane")
    finally:
        m.close()
        srv.stop()

    print("\nRESULT:", "PASS" if not FAILS else f"FAIL ({len(FAILS)})")
    sys.exit(1 if FAILS else 0)


if __name__ == "__main__":
    main()

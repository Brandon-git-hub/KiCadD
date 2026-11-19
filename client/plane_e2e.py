"""Reference-plane checks in audit (headless): plane splits, gaps, stretches past a plane's edge,
and signal vias without a return via, on the demos as they are.

usage: python3 -I plane_e2e.py --build BUILD --demos DEMOS_DIR --work DIR

  cm5_minima      6 layers, GND on In1 / In4: USB_C.D_P's via F.Cu -> B.Cu has its nearest GND via
                  3.17 mm away (a warning, a fast net); HDMI D0_N's at 1.6 mm passes at the default
                  2 mm and fails at return_via_mm 1.5; plane_nets picks nets; an unknown one is said
  stickhub        2 layers, B.Cu poured GND / +5V: the USB D+ on F.Cu crosses the split; deleting
                  its tracks takes the finding away, undo brings it back
  pic_programmer  2 layers, GND on the bottom only: the bottom layer has no plane (said once);
                  top-layer gaps are notes (slow nets)
  video           4 layers: HD_PAL- runs along the board edge outside the In1 GND plane; the
                  whole board is checked in seconds
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
        first = t.splitlines()[0] if t else ""
        rv = section(t, "return_via")
        print("      ", first[first.find("board:"):][:200])
        mm = re.search(r"(\d+) signal net\(s\) checked against their planes \(([\d.]+) of ([\d.]+) mm", first)
        check(not err and mm and int(mm.group(1)) > 50 and float(mm.group(2)) > 0.9 * float(mm.group(3)),
              "the summary: signal nets checked, most of their track over a plane")
        usb = [ln for ln in rv if "takes /USB_C.D_P from F.Cu" in ln]
        check(usb and usb[0].startswith("warning:") and "3.17 mm away (max 2)" in usb[0] and "[fast]" in usb[0],
              "USB_C.D_P's via: nearest GND via joining In1 / In4 3.17 mm away, a warning (fast net)")
        check(not any("HDMI_PI.D0_N" in ln for ln in rv), "HDMI D0_N's via (GND via 1.6 mm away) passes at 2 mm")

        err, t = m.tool("audit", {"plane_nets": ["/CM5/HDMI_PI.D0_N", "NOPE"], "return_via_mm": 1.5})
        rv, pl = section(t, "return_via"), section(t, "plane")
        check(any("HDMI_PI.D0_N" in ln and "1.6 mm away (max 1.5)" in ln for ln in rv),
              "return_via_mm 1.5: HDMI D0_N's via fails (1.6 mm)")
        check(len([ln for ln in rv if ln.startswith(("warning", "info"))]) == 1 and "1 signal net(s) checked" in t,
              "plane_nets: only that net is checked")
        check(any("no net NOPE" in ln for ln in pl), "an unknown plane net is said")
    finally:
        m.close()
        srv.stop()

    # ---- stickhub: a split under USB D+
    print("[stickhub]")
    srv, m = board(a, work, "stickhub", "StickHub.kicad_pcb")
    try:
        err, t = m.tool("audit")
        pl = section(t, "plane")
        d = [ln for ln in pl if ln.startswith("warning: /D+ on F.Cu crosses a plane split")]
        print("      ", d[0] if d else pl[:2])
        check(d and "+5V (B.Cu)" in d[0] and "GND (B.Cu)" in d[0] and "[fast]" in d[0],
              "D+ on F.Cu crosses the B.Cu GND / +5V split")
        check(any(ln.startswith("info: +3.3V") for ln in pl) is False, "+3.3V is a supply, not checked")
        err, t = m.tool("delete_tracks", {"nets": ["/D+"]})
        check(not err, "delete D+'s tracks")
        _, t = m.tool("audit")
        check(not any("/D+ on F.Cu crosses" in ln for ln in section(t, "plane")), "no D+ split crossing without its tracks")
        m.tool("undo")
        _, t = m.tool("audit")
        check(any("/D+ on F.Cu crosses" in ln for ln in section(t, "plane")), "undo: the crossing is back")
    finally:
        m.close()
        srv.stop()

    # ---- pic_programmer: two layers, one plane
    print("[pic_programmer]")
    srv, m = board(a, work, "pic_programmer", "pic_programmer.kicad_pcb")
    try:
        err, t = m.tool("audit")
        pl = section(t, "plane")
        check(any(ln.startswith("info: bottom_layer: no zone on top_layer") for ln in pl),
              "the bottom layer has no plane next to it (said once)")
        gaps = [ln for ln in pl if "over a gap in GND (bottom_layer)" in ln]
        check(gaps and all(ln.startswith("info:") for ln in gaps), "top-layer gaps over the GND pour, as notes (slow nets)")
        check("Net-(R8-Pad1) on top_layer runs 6.1 mm" in "\n".join(gaps), "the longest: R8's net, 6.1 mm")
    finally:
        m.close()
        srv.stop()

    # ---- video: a long track past the plane's edge, and the time
    print("[video]")
    srv, m = board(a, work, "video", "video.kicad_pcb")
    try:
        t0 = time.time()
        err, t = m.tool("audit")
        dt = time.time() - t0
        check(not err and dt < 20, f"whole-board audit in {dt:.1f} s")
        _, t = m.tool("audit", {"plane_nets": ["/HD_PAL-"]})
        pl = section(t, "plane")
        print("      ", pl[:2])
        check(any(ln.startswith("warning: /HD_PAL- on top_copper runs 166.") and "past the edge of GND (In1.Cu)" in ln
                  for ln in pl), "HD_PAL- runs 166 mm past the edge of the In1 GND plane (named: a warning)")
    finally:
        m.close()
        srv.stop()

    print("\nRESULT:", "PASS" if not FAILS else f"FAIL ({len(FAILS)})")
    sys.exit(1 if FAILS else 0)


if __name__ == "__main__":
    main()

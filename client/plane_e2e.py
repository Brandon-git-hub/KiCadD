"""Reference planes in audit (headless), on the demos as they are. Made for an agent: without
limits an overview per net (measured, worst first, no verdicts); with plane_gap / return_via
{glob: mm} (two_sided, bga_return_via_mm) warnings.

usage: python3 -I plane_e2e.py --build BUILD --demos DEMOS_DIR --work DIR

  cm5_minima      6 layers, GND on In1 / In4. By default a header line per section; plane_limit
                  rows, a pair as one (USB_C.D_P/N, 3.17 mm), no warnings; pairs_only.
                  return_via by rule: a count line each (passing too), then the worst; D0_N (1.6 mm)
                  fails at 1.5 and passes at 2; an exact name beats the glob; two_sided on the HDMI
                  clock pair; rows per rule stop at plane_limit; plane_nets globs; bad limits refused
  stickhub        2 layers, B.Cu poured GND / +5V: the USB pair D+/- is the header's worst (4
                  crossings of the split); plane_gap on D+/- counts 4 crossings; deleting D+'s tracks leaves D-'s 2,
                  undo brings back 4
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
        check(not err and len(pl) == 1 and len(rv) == 1, "by default one line each (the header)")
        check(rv and rv[0].startswith("info: 77 signal via(s) change reference planes")
              and "The farthest: /CM5/PCIE_PI.nWAKE: 1 via(s); farthest return via 4.63 mm" in rv[0],
              "the header: 77 vias, the farthest return via (nWAKE, 4.63 mm)")
        _, t = m.tool("audit", {"plane_limit": 20})
        rv = section(t, "return_via")
        usb = [ln for ln in rv if ln.startswith("info: /USB_C.D_P/N (pair): 4 via(s); farthest return via 3.17 mm")]
        print("      ", usb[0][:150] if usb else rv[:2])
        check(usb and "F.Cu (GND In1.Cu) to B.Cu (GND In4.Cu)" in usb[0], "plane_limit: a pair is one row (USB_C.D_P/N, 3.17 mm)")
        check(not warnings(section(t, "plane")) and not warnings(rv), "no limits: no verdicts")
        check(any("more (plane_limit)" in ln for ln in rv), "the rows stop at plane_limit")

        _, t = m.tool("audit", {"pairs_only": True, "plane_limit": 40})
        rv = section(t, "return_via")
        check(rv[0].startswith("info: 39 signal via(s)") and all("(pair)" in ln for ln in rv[1:]), "pairs_only: pairs only")

        _, t = m.tool("audit", {"return_via": {"*HDMI*": 1.5, "*DPHY*": 1.5}})
        rv = section(t, "return_via")
        check("info: *DPHY* (max 1.5 mm): 10 via(s) of 5 net(s) / pair(s) judged, all within" in rv, "a passing rule says so")
        hd = [ln for ln in rv if ln.startswith("warning: *HDMI* (max 1.5 mm):")]
        check(hd and "over" in hd[0], "a failing rule: its count line is a warning")
        d0 = [ln for ln in rv if "HDMI_PI.D0_P via" in ln]
        check(d0 and "2.29 mm (max 1.5)" in d0[0] and "1 more via(s) of /CM5/HDMI_PI.D0_P/N (pair) over" in d0[0],
              "the D0 pair: one row, its worst via (2.29 mm) and the other over too (1.6 mm)")
        check(not any("USB" in ln for ln in rv), "nets no glob matches are not judged")
        _, t = m.tool("audit", {"return_via": {"*HDMI*": 2}})
        d0 = [ln for ln in section(t, "return_via") if "HDMI_PI.D0_P via" in ln]
        check(d0 and "more via(s)" not in d0[0], "at 2 mm only D0_P is over (D0_N, 1.6 mm, passes)")
        _, t = m.tool("audit", {"return_via": {"*HDMI*": 1.5, "/CM5/HDMI_PI.D0_N": 2}})
        check("info: /CM5/HDMI_PI.D0_N (max 2 mm): 1 via(s) of 1 net(s) / pair(s) judged, all within" in section(t, "return_via"),
              "an exact name beats the glob")
        _, t = m.tool("audit", {"return_via": {"*HDMI*": 1.5}, "two_sided": ["*HDMI_PI.CK*"]})
        check(any("HDMI_PI.CK_" in ln and "one side only" in ln for ln in warnings(section(t, "return_via"))),
              "two_sided: the HDMI clock pair has return vias on one side only")
        _, t = m.tool("audit", {"return_via": {"*": 0.1}, "plane_limit": 2})
        rv = section(t, "return_via")
        check(len(rv) <= 1 + 2 + 1 and any("more net(s) / pair(s) over for * (plane_limit)" in ln for ln in rv),
              "rows per rule stop at plane_limit")
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
        print("      ", pl[0][:220] if pl else pl)
        check(pl and ": 3 cross a plane split" in pl[0] and re.search(r"The worst: /D\+/- \(pair\): .* 4 split crossing", pl[0]),
              "the header: 3 nets / pairs cross a split, the worst the USB pair D+/- (4 crossings)")

        def splits():
            _, t = m.tool("audit", {"plane_gap": {"/D?": 5}})
            p = section(t, "plane")
            mm = re.search(r"/D\? \(max 5 mm with no plane\): 1 net\(s\) / pair\(s\) judged, (?:all within|1 over: (\d+) split)", p[0] if p else "")
            return p, int(mm.group(1) or 0) if mm else -1
        pl, n = splits()
        check(n == 4 and any("crosses a plane split" in ln and "+5V (B.Cu)" in ln and "GND (B.Cu)" in ln for ln in pl),
              "plane_gap /D?: the pair D+/- crosses the B.Cu GND / +5V split 4 times, a warning")
        check(not any("/U3D" in ln for ln in pl), "other nets are not judged")
        m.tool("delete_tracks", {"nets": ["/D+"]})
        pl, n = splits()
        check(n == 2 and not any("/D+ on F.Cu" in ln for ln in pl), "without D+'s tracks: D-'s 2 crossings")
        m.tool("undo")
        check(splits()[1] == 4, "undo: 4 again")
    finally:
        m.close()
        srv.stop()

    # ---- pic_programmer: two layers, one plane
    print("[pic_programmer]")
    srv, m = board(a, work, "pic_programmer", "pic_programmer.kicad_pcb")
    try:
        _, t = m.tool("audit", {"plane_limit": 5})
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
        check(bga and re.search(r"IC14's BGA field \(\d+ GND vias, 784 pads\)", bga[0]), "vias in IC14's BGA field say so")
        _, t = m.tool("audit", {"return_via": {"*": 0.1}, "bga_return_via_mm": 50, "plane_limit": 400})
        rv = warnings(section(t, "return_via"))
        check(len(rv) > 1 and not any("BGA field" in ln for ln in rv), "bga_return_via_mm 50: no via in a BGA field fails")
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
        print("      ", pl[:2])
        check(any(ln.startswith("warning: /HD_PAL- on top_copper runs 166.") and "past the edge of GND (In1.Cu)" in ln for ln in pl),
              "plane_gap: HD_PAL- runs 166 mm past the edge of the In1 GND plane")
    finally:
        m.close()
        srv.stop()

    print("\nRESULT:", "PASS" if not FAILS else f"FAIL ({len(FAILS)})")
    sys.exit(1 if FAILS else 0)


if __name__ == "__main__":
    main()

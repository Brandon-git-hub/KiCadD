"""Crosstalk pre-screen in audit (headless), on the demos as they are. Made for an agent: without
limits a header line (rows with xtalk_limit / xtalk_nets), no verdicts; with xtalk_length
{glob: mm} warnings. Measures only.

usage: python3 -I xtalk_e2e.py --build BUILD --demos DEMOS_DIR --work DIR

  cm5_minima  two pairs within 3H (H 0.099 mm on B.Cu): HDMI HOTPLUG / CEC 9.9 mm at 2H (checked
              by sampling every 0.01 mm: 9.92); xtalk_length judged rule by rule, a pair by the
              tightest glob of its nets; xtalk_spacing 1 leaves none; deleting a net's tracks
              removes its pair; bad arguments refused
  stickhub    a 2-layer board: H is the board (1.51 mm), said so
  vme-wren    long parallel buses on inner layers; a diff pair's partners are no pair of their own;
              with In1.Cu's zones taken off, H follows the planes by their copper (F.Cu over In3.Cu
              across In1 and In2, their copper in H) and the adjacent signal layers are said
  jetson      diff pairs as one name against each other; in seconds
"""

import argparse
import os
import re
import shutil
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from m5_e2e import FAILS, Mcp, Server, binaries, check  # noqa: E402


def section(t, name="crosstalk"):
    out, on = [], False
    for ln in t.splitlines():
        if not ln.startswith("  "):
            on = ln == name + ":"
        elif on:
            out.append(ln.strip())
    return out


def board(a, work, demo, fname, unpour=()):
    dst = os.path.join(work, demo[:6] + ("u" if unpour else ""))
    shutil.copytree(os.path.join(a.demos, demo), dst)
    if unpour:
        p = os.path.join(dst, fname)
        t = open(p).read()
        open(p, "w").write(strip(t, unpour))
    sock = dst + ".sock"
    _, kicadd_mcp, env = binaries(a.build)
    srv = Server(a.build, sock, os.path.join(dst, fname), env, dst + ".log")
    check(srv.start(), "api-server up with " + fname)
    return srv, Mcp([kicadd_mcp, "--socket", sock, "--stdio"], env)


# ---- a board's zones off some layers (the file edited before it is opened): balanced blocks
def blocks(t, head):
    """(start, end) of each balanced block starting with head"""
    out, i = [], 0
    while True:
        i = t.find(head, i)
        if i < 0: return out
        d, j = 0, i
        while True:
            c = t[j]
            if c == '"':
                j = t.index('"', j + 1)
                while t[j - 1] == '\\': j = t.index('"', j + 1)
            elif c == '(': d += 1
            elif c == ')':
                d -= 1
                if d == 0: break
            j += 1
        out.append((i, j + 1)); i = j + 1
def strip(t, layers):
    cut = []
    for a, b in blocks(t, "(zone\n"):
        z = t[a:b]
        m = re.search(r'\(layers? ((?:"[^"]+"\s*)+)\)', z)
        ls = re.findall(r'"([^"]+)"', m.group(1))
        keep = [l for l in ls if l not in layers]
        if not keep: cut.append((a, b, "")); continue
        z2 = z[:m.start()] + "(layers " + " ".join(f'"{l}"' for l in keep) + ")" + z[m.end():]
        for fa, fb in reversed(blocks(z2, "(filled_polygon")):
            if any(f'(layer "{l}")' in z2[fa:fb] for l in layers): z2 = z2[:fa] + z2[fb:]
        cut.append((a, b, z2))
    for a, b, r in reversed(cut): t = t[:a] + r + t[b:]
    return t


# the IR drop solve is not what this test is about
QUIET = {"ir_nets": ["NO_RAIL"]}


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
        x = section(t)
        print("      ", x[0][:220] if x else t[:200])
        check(not err and len(x) == 1, "by default one crosstalk line")
        check(x and x[0].startswith("info: 2 net pair(s) side by side within 3H for 2 mm or more")
              and "the longest: /CM5/HDMI_PI.HOTPLUG / /CM5/HDMI_PI.CEC: 9.9 mm within 3H on B.Cu, closest 0.204 mm = 2H "
                  "(B.Cu, H 0.099) at (97.21, 57.83)" in x[0] and "No verdicts" in x[0],
              "the header: 2 pairs, the longest (9.9 mm, sampled independently: 9.92) with H and where")

        _, t = m.tool("audit", dict(QUIET, xtalk_limit=5))
        x = section(t)
        check(len(x) == 3 and x[2].startswith("info: /CM5/HDMI_PI.SCL / /CM5/HDMI_PI.SDA: 6 mm within 3H on B.Cu"),
              "xtalk_limit: a row per pair, the longest first")

        _, t = m.tool("audit", dict(QUIET, xtalk_length={"*HOTPLUG*": 5, "*": 15}))
        x = section(t)
        check("warning: xtalk_length *HOTPLUG* (max 5 mm within 3H): 2 coupled pair(s) judged, 1 over" in x,
              "xtalk_length *HOTPLUG* 5 mm: over")
        check(any(ln.startswith("warning: /CM5/HDMI_PI.HOTPLUG / /CM5/HDMI_PI.CEC: 9.9 mm") and "4.9 mm over" in ln for ln in x),
              "the row: how far over, and what to do")
        check(any(re.match(r"info: xtalk_length \* \(max 15 mm within 3H\): \d+ coupled pair\(s\) judged, all within", ln) for ln in x),
              "the rest under 15 mm: a passing count line")

        _, t = m.tool("audit", dict(QUIET, xtalk_spacing=1))
        check(section(t)[0].startswith("info: 0 net pair(s) side by side within 1H"), "xtalk_spacing 1: the 2H pairs drop out")

        _, t = m.tool("audit", dict(QUIET, xtalk_nets=["*CEC"], xtalk_limit=5))
        x = section(t)
        check(len(x) == 2 and "/CM5/HDMI_PI.CEC" in x[1], "xtalk_nets: only the pairs with that net")
        _, t = m.tool("audit", dict(QUIET, xtalk_nets=["NOPE*"]))
        check("warning: no signal net with tracks matches NOPE*" in section(t), "a glob that matches nothing")

        m.tool("delete_tracks", {"nets": ["/CM5/HDMI_PI.CEC"]})
        _, t = m.tool("audit", QUIET)
        check(section(t)[0].startswith("info: 1 net pair(s)"), "CEC's tracks deleted: its pair is gone")
        m.tool("undo")
        _, t = m.tool("audit", QUIET)
        check(section(t)[0].startswith("info: 2 net pair(s)"), "undo: back")

        err, t = m.tool("audit", {"xtalk_spacing": 0})
        check(err and "xtalk_spacing: > 0; got 0" in t, "xtalk_spacing 0 refused")
        err, t = m.tool("audit", {"xtalk_length": {"*": "long"}})
        check(err and 'xtalk_length.*: a number in mm; got "long"' in t, "an xtalk_length that is not a number refused")
    finally:
        m.close()
        srv.stop()

    # ---- stickhub: 2 layers
    print("[stickhub]")
    srv, m = board(a, work, "stickhub", "StickHub.kicad_pcb")
    try:
        _, t = m.tool("audit", QUIET)
        x = section(t)
        check(x and "On F.Cu, B.Cu H is 1.51 mm (no plane closer), so 3H = 4.5 mm takes in most "
                    "neighbours" in x[0], "a 2-layer board: H is the board, said so")
    finally:
        m.close()
        srv.stop()

    # ---- vme-wren: buses, pairs
    print("[vme-wren]")
    srv, m = board(a, work, "vme-wren", "vme-wren.kicad_pcb")
    try:
        _, t = m.tool("audit", dict(QUIET, xtalk_limit=200))
        x = section(t)
        print("      ", x[0][:200])
        check(x and re.match(r"info: \d{3} net pair\(s\)", x[0])
              and "the longest: /io_drivers_pp/bank2/OUT6 / /io_drivers_pp/bank2/OUT7: 184.5 mm within 3H (In9.Cu 120.3, In2.Cu 64.1)" in x[0],
              "the longest: a parallel bus on two inner layers")

        def partners(ln):
            mm_ = re.match(r"info: (.+?) / (.+?): [\d.]+ mm", ln)
            return mm_ and mm_.group(1)[:-1] == mm_.group(2)[:-1] and mm_.group(1)[-1] in "P+" and mm_.group(2)[-1] in "N-"

        check(not any(partners(ln) for ln in x[1:]), "no diff pair's partners as a pair of their own")
    finally:
        m.close()
        srv.stop()

    # ---- vme-wren with In1.Cu's planes taken off: F.Cu, In1, In2 are signal layers next to each other
    print("[vme-wren, In1.Cu not poured]")
    srv, m = board(a, work, "vme-wren", "vme-wren.kicad_pcb", unpour=("In1.Cu",))
    try:
        _, t = m.tool("audit", QUIET)
        x = section(t)
        print("      ", x[0][:200])
        check(x and "F.Cu / In1.Cu, In1.Cu / In2.Cu: signal layers with no plane between, tracks over each other "
                    "couple broadside (not screened)" in x[0], "adjacent signal layers: the broadside note")
        n = re.match(r"info: (\d+) net pair", x[0]) if x else None
        check(n and int(n.group(1)) > 1000, f"F.Cu's plane now In3.Cu (H 0.119 -> 0.477 mm): {n and n.group(1)} pairs, not 598")
        _, t = m.tool("impedance", {"z": 50, "layers": ["F.Cu", "In2.Cu"]})
        check("F.Cu: width" in t and "microstrip over In3.Cu, 0.477 mm" in t and "across In1.Cu, In2.Cu" in t,
              "impedance: F.Cu over In3.Cu, H with In1 and In2's copper (0.119 + 0.018 + 0.203 + 0.018 + 0.119)")
        check("In2.Cu: width" in t and "embedded microstrip over In3.Cu, 0.119 mm" in t and "the 0.368 mm of board over it" in t,
              "In2.Cu: a plane on one side only, an embedded microstrip under the board above")
    finally:
        m.close()
        srv.stop()

    # ---- jetson
    print("[jetson]")
    srv, m = board(a, work, "jetson-agx-thor-baseboard", "jetson-agx-thor-baseboard.kicad_pcb")
    try:
        t0 = time.time()
        _, t = m.tool("audit", dict(QUIET, xtalk_nets=["*USBSS2*"]))
        dt = time.time() - t0
        x = section(t)
        check(dt < 20, f"audit in {dt:.1f} s")
        check(any(ln.startswith("info: /SoM_IO2/USBSS2.RX+/- (pair) / /SoM_IO2/USBSS2.TX+/- (pair): 10.7 mm within 3H on In7.Cu")
                  for ln in x), "pairs against each other, as one name each")
    finally:
        m.close()
        srv.stop()

    print("\nRESULT:", "PASS" if not FAILS else f"FAIL ({len(FAILS)})")
    sys.exit(1 if FAILS else 0)


if __name__ == "__main__":
    main()

"""lcsc_match end to end (headless): LCSC numbers for many schematic parts at once.

usage: python3 -I match_e2e.py --build BUILD --kicad KICAD_SRC --work DIR --index PARTS_DB

PARTS_DB: an unpacked parts index (ci/build_parts_index.py's parts.db), used as is
(KICADD_PARTS_DB). Offline: the live check is not run here.

Blank project (m5_e2e's) with resistors, capacitors, an inductor, a ferrite bead, LEDs and a crystal:
  * equal value + footprint is one group; R / C / L / FB / LED in chip packages are matched
    (basic first), the rest is listed with the reason (no footprint, not a chip package, the
    LED's colour, not a passive)
  * a part with an LCSC number already, DNP and not-in-BOM parts are left alone
  * the rating in the value narrows the capacitor ("100nF 50V X7R")
  * dry run writes nothing; apply writes all of them as one sch_undo; pick picks another part
"""

import argparse
import os
import re
import shutil
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from m5_e2e import FAILS, Mcp, Server, check, make_project  # noqa: E402


def lcsc_of(mcp):
    err, t = mcp.tool("sch_summary")
    out = {}
    for line in t.splitlines()[1:]:
        m = re.match(r"(\S+) .*?(?:\((C\d+)\))?(?: dnp| exclude_from_bom)*$", line)
        if m:
            out[m.group(1)] = m.group(2)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", required=True)
    ap.add_argument("--kicad", required=True)
    ap.add_argument("--work", required=True)
    ap.add_argument("--index", required=True)
    a = ap.parse_args()

    work = os.path.abspath(a.work)
    if os.path.exists(work):
        shutil.rmtree(work)
    os.makedirs(work)
    proj = make_project(a.kicad, work)

    sock = os.path.join(work, "s.sock")
    env = dict(os.environ, KICAD_RUN_FROM_BUILD_DIR="1", KICADD_LCSC_OFFLINE="1",
               KICADD_PARTS_DB=os.path.abspath(a.index))
    server = Server(a.build, sock, os.path.join(proj, "blank.kicad_pcb"), env, os.path.join(work, "server.log"))
    assert server.start()
    try:
        mcp = Mcp([os.path.join(a.build, "kicadd-mcp"), "--socket", sock, "--stdio"], env)
        parts = [("R1", "Device:R", "10k", "Resistor_SMD:R_0603_1608Metric"),
                 ("R2", "Device:R", "10k", "Resistor_SMD:R_0603_1608Metric"),
                 ("R3", "Device:R", "4k7 1%", "Resistor_SMD:R_0402_1005Metric"),
                 ("R4", "Device:R", "1k", "Resistor_THT:R_Axial_DIN0207_L6.3mm_D2.5mm_P10.16mm_Horizontal"),
                 ("R5", "Device:R", "22", ""),
                 ("R6", "Device:R", "10k", "Resistor_SMD:R_0603_1608Metric"),     # has a number already
                 ("R7", "Device:R", "10k", "Resistor_SMD:R_0603_1608Metric"),     # DNP
                 ("C1", "Device:C", "100nF 50V X7R", "Capacitor_SMD:C_0402_1005Metric"),
                 ("C2", "Device:C", "100nF 50V X7R", "Capacitor_SMD:C_0402_1005Metric"),
                 ("C3", "Device:C", "10uF", "Capacitor_SMD:C_0805_2012Metric"),
                 ("L1", "Device:L", "2.2uH", "Inductor_SMD:L_0805_2012Metric"),
                 ("FB1", "Device:FerriteBead", "600R", "Inductor_SMD:L_0603_1608Metric"),
                 ("D1", "Device:LED", "LED red", "LED_SMD:LED_0603_1608Metric"),
                 ("D2", "Device:LED", "LED yellow", "LED_SMD:LED_0603_1608Metric"),
                 ("D3", "Device:LED", "LED", "LED_SMD:LED_0603_1608Metric"),
                 ("Y1", "Device:Crystal", "8MHz", "Crystal:Crystal_SMD_3225-4Pin_3.2x2.5mm")]
        x = 25.4
        for ref, lib, value, fp in parts:
            args = {"lib_id": lib, "ref": ref, "value": value, "x": x, "y": 50.8}
            if fp:
                args["footprint"] = fp
            err, t = mcp.tool("sch_add_part", args)
            check(not err, f"add {ref}: " + t.splitlines()[0][:60])
            x += 12.7
        mcp.tool("sch_set_fields", {"parts": {"R6": {"LCSC": "C99999"}, "R7": {"dnp": True}}})

        print("[dry run]")
        err, t = mcp.tool("lcsc_match", {"live": False})
        print("\n".join("       " + l[:150] for l in t.splitlines()))
        lines = {l.split(" (")[0]: l for l in t.splitlines()[2:]}
        check(not err and t.startswith("12 group(s)"), "12 groups (R1 R2 one, C1 C2 one; R6 and R7 left out)")
        check("1 part(s) have one already" in t and "1 not assembled" in t, "R6 (has one) and R7 (DNP) counted apart")
        check(re.search(r"-> C\d+ BASIC", lines.get("R1 R2", "")) is not None, "R1 R2 10k 0603: a basic part")
        check(re.search(r"JLCPCB assembly stock \d+ \(index \d{4}-\d\d-\d\d\)", lines.get("R1 R2", "")) is not None
              and re.search(r"first of \d+ matching parts$", lines.get("R1 R2", "")) is not None
              and "in stock)" not in t, "the stock says whose it is; the candidate count says what it is")
        check(re.search(r"-> C\d+ .*4\.7kΩ.*±1%", lines.get("R3", "")) is not None, "R3 4k7 1% 0402: 4.7k, 1%")
        check("not a chip package" in lines.get("R4", ""), "R4 axial: not a chip package")
        check("no footprint" in lines.get("R5", ""), "R5: no footprint")
        c1 = lines.get("C1 C2", "")
        check(re.search(r"-> C\d+ ", c1) is not None and "50V" in c1 and "X7R" in c1, "C1 C2 100nF 50V X7R: " + c1[:120])
        check(re.search(r"-> C\d+ ", lines.get("L1", "")) is not None, "L1 2.2uH 0805")
        check(re.search(r"-> C\d+ ", lines.get("FB1", "")) is not None, "FB1 600R 0603")
        d1, d2 = lines.get("D1", ""), lines.get("D2", "")
        check(re.search(r"-> C\d+ ", d1) is not None and "White" not in d1, "D1 red LED: " + d1[:100])
        check(re.search(r"-> C\d+ .*Yellow", d2) is not None and "White" not in d2, "D2 yellow, not a white one with a yellow lens")
        check("colour" in lines.get("D3", ""), "D3 'LED': the colour is asked for")
        check("only resistors" in lines.get("Y1", ""), "Y1 crystal: not matched")
        check("dry run" in t and lcsc_of(mcp).get("R1") is None, "dry run wrote nothing")
        r12 = re.search(r"-> (C\d+)", lines["R1 R2"]).group(1)

        print("[apply, pick]")
        err, t = mcp.tool("lcsc_match", {"live": False, "apply": True, "pick": {"Y1": "C9002", "R3": "C25900"}})
        print("       " + t.splitlines()[-1])
        got = lcsc_of(mcp)
        check(not err and "LCSC number set on 11 part(s)" in t, "apply: 11 parts (8 groups matched + the crystal picked)")
        check(got.get("R1") == r12 and got.get("R2") == r12, f"R1 R2 = {r12}")
        check(got.get("R3") == "C25900" and got.get("Y1") == "C9002", "pick: R3 and Y1 as given")
        check(got.get("R6") == "C99999" and got.get("R7") is None and got.get("R4") is None, "R6 kept, R7 / R4 not set")
        err, t = mcp.tool("lcsc_match", {"live": False})
        check(not err and t.startswith("3 group(s)") and "0 matched" in t, "again: only the 3 not matched are left (D3 R4 R5): " + t.splitlines()[0])

        err, t = mcp.tool("sch_undo")
        check(not err and lcsc_of(mcp).get("R1") is None, "one sch_undo takes them all back: " + t.splitlines()[0][:60])
        mcp.tool("sch_redo")
        check(lcsc_of(mcp).get("C1") is not None, "sch_redo")

        err, t = mcp.tool("lcsc_match", {"refs": ["R1", "R6"], "replace": True, "live": False})
        check(not err and t.startswith("1 group(s)") and "R1 R6 (10k" in t, "refs + replace: R1 and R6 (one group) matched again: " + t.splitlines()[0])
        err, t = mcp.tool("lcsc_match", {"refs": ["R99"]})
        check(err and "not in the schematic: R99" in t, "an unknown ref is refused")

        mcp.close()
    finally:
        server.stop()
    print("\nRESULT:", "PASS" if not FAILS else f"FAIL ({len(FAILS)})")
    sys.exit(1 if FAILS else 0)


if __name__ == "__main__":
    main()

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


def lcsc_library(proj):
    """Lc:C, Device:C with the fields an LCSC symbol has (C1525, a 100 nF 0402): in the tables."""
    text = open(os.path.join(proj, "Device.kicad_sym"), encoding="utf-8").read()
    start = text.index('\t(symbol "C"\n')
    depth, i = 0, start
    while True:
        depth += {"(": 1, ")": -1}.get(text[i], 0)
        i += 1
        if depth == 0 and text[i - 1] == ")":
            break
    sym = text[start:i]
    hidden = '(at 0 0 0)(effects (font (size 1.27 1.27)) (hide yes))'
    extra = "".join(f'\n\t\t(property "{k}" "{v}" {hidden})' for k, v in (
        ("LCSC", "C1525"), ("Supplier Part", "C1525"), ("Manufacturer Part", "CL05B104KO5NNNC")))
    sym = re.sub(r'\(property "Description" "[^"]*"', '(property "Description" "CL05B104KO5NNNC (LCSC C1525), 100nF"', sym, 1)
    sym = sym.replace('\t\t(property "Reference"', extra.lstrip("\n") + '\n\t\t(property "Reference"', 1)
    head = text[:text.index("\t(symbol ")]
    open(os.path.join(proj, "Lc.kicad_sym"), "w", encoding="utf-8").write(head + sym + "\n)\n")
    table = os.path.join(proj, "sym-lib-table")
    t = open(table).read().rstrip()
    assert t.endswith(")")
    open(table, "w").write(t[:-1] + '  (lib (name "Lc")(type "KiCad")(uri "${KIPRJMOD}/Lc.kicad_sym")(options "")(descr ""))\n)\n')


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
    lcsc_library(proj)

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

        err, t = mcp.tool("undo", {"target": "schematic"})
        check(not err and lcsc_of(mcp).get("R1") is None, "one sch_undo takes them all back: " + t.splitlines()[0][:60])
        mcp.tool("undo", {"target": "schematic", "redo": True})
        check(lcsc_of(mcp).get("C1") is not None, "sch_redo")

        err, t = mcp.tool("lcsc_match", {"refs": ["R1", "R6"], "replace": True, "live": False})
        check(not err and t.startswith("1 group(s)") and "R1 R6 (10k" in t, "refs + replace: R1 and R6 (one group) matched again: " + t.splitlines()[0])
        print("[pick checked, the old part's fields]")
        err, t = mcp.tool("lcsc_match", {"refs": ["R1"], "replace": True, "live": False, "pick": {"R1": "C1525"}})
        check("does not fit" in t and "its value is" in t, "a pick of another value (a 100 nF cap for 10k): not taken: "
              + next((l.strip() for l in t.splitlines() if "does not fit" in l), t.splitlines()[0])[:120])
        mcp.tool("sch_set_fields", {"parts": {"R1": {"Supplier Part": "C1", "Manufacturer Part": "OLD-PART"}}})
        err, t = mcp.tool("lcsc_match", {"refs": ["R1"], "replace": True, "live": False, "apply": True})
        err2, p = mcp.tool("sch_part", {"ref": "R1"})
        check("field(s) of the old part rewritten" in t and "Supplier Part=C1," not in p and "OLD-PART" not in p,
              "apply: Supplier / Manufacturer Part of the old part rewritten: " + next((l for l in p.splitlines() if "Supplier" in l), p[:100])[:120])
        print("[a symbol with an LCSC number placed with another value]")
        err, t = mcp.tool("sch_add_part", {"lib_id": "Lc:C", "ref": "C20", "value": "4.7uF", "x": 25.4, "y": 152.4})
        check(not err and "(C1525)" in t.splitlines()[0], "Lc:C placed: " + t.splitlines()[0][:80])
        check("warning: its LCSC number C1525 is 100 nF" in t, "the value is not the LCSC part's: warned")
        err, t = mcp.tool("batch", {"steps": [{"tool": "sch_add_part", "args": {"lib_id": "Lc:C", "ref": "C21", "value": "4.7uF 10V X5R",
                                                                                 "x": 38.1, "y": 152.4}},
                                               {"tool": "sch_add_part", "args": {"lib_id": "Lc:C", "ref": "C22", "value": "100nF",
                                                                                 "x": 50.8, "y": 152.4}}]})
        print("       " + t.replace("\n", "\n       ")[:600])
        check(not err and t.count("warning: its LCSC number C1525") == 1, "in a batch too (the step's warning line is kept), "
              "and not for the 100nF one")
        print("[sch_set_fields LCSC: the old part's fields go with it]")
        err, t = mcp.tool("sch_set_fields", {"parts": {"C20": {"LCSC": r12}}})
        err2, p = mcp.tool("sch_part", {"ref": "C20"})
        fields = p.splitlines()[1] if len(p.splitlines()) > 1 else ""
        print("       " + fields[:200])
        check(not err and "field(s) of the old part rewritten" in t, "said: " + t.replace("\n", " ")[:120])
        check(f"Supplier Part={r12}" in fields and "CL05B104KO5NNNC" not in fields and "C1525" not in fields,
              "Supplier Part is the new number; Manufacturer Part and Description no longer the old part's")
        err, t = mcp.tool("sch_set_fields", {"parts": {"C21": {"LCSC": None}}})
        err2, p = mcp.tool("sch_part", {"ref": "C21"})
        check(not err and "C1525" not in p and "CL05B104KO5NNNC" not in p, "LCSC removed: the old part's fields cleared too")
        err, t = mcp.tool("sch_set_fields", {"parts": {"C22": {"LCSC": r12, "Manufacturer Part": "MINE"}}})
        err2, p = mcp.tool("sch_part", {"ref": "C22"})
        check("Manufacturer Part=MINE" in p and f"Supplier Part={r12}" in p, "a field set in the same call is kept")

        err, t = mcp.tool("lcsc_match", {"refs": ["R99"]})
        check(err and "not in the schematic: R99" in t, "an unknown ref is refused")

        print("[the LCSC library's footprint names]")
        x = 25.4
        for ref, lib, value, fp in [("R8", "Device:R", "10k", "LCSC:R0603"), ("C8", "Device:C", "1uF", "LCSC:C0805"),
                                    ("D8", "Device:LED", "LED red", "LCSC:LED0603-RD"), ("C9", "Device:C", "1uF", "LCSC:C0603Metric"),
                                    ("D9", "Device:LED", "LED green", "LCSC:LED-SMD_L1.6-W0.8-R-RD")]:
            mcp.tool("sch_add_part", {"lib_id": lib, "ref": ref, "value": value, "x": x, "y": 101.6, "footprint": fp})
            x += 12.7
        err, t = mcp.tool("lcsc_match", {"refs": ["R8", "C8", "D8", "C9", "D9"], "live": False})
        print("\n".join("       " + l[:150] for l in t.splitlines()))
        lines = {l.split(" (")[0].strip(): l for l in t.splitlines()}
        check(re.search(r"-> C\d+ ", lines.get("R8", "")) is not None, "R0603: 0603 read from the LCSC library's name")
        check(re.search(r"-> C\d+ ", lines.get("C8", "")) is not None, "C0805")
        check(re.search(r"-> C\d+ ", lines.get("D8", "")) is not None, "LED0603-RD")
        check("not a chip package" in lines.get("C9", ""), "C0603Metric: not taken as 0603 (metric 0603 is 0201)")
        check(re.search(r"-> C\d+ ", lines.get("D9", "")) is not None, "LED-SMD_L1.6-W0.8-R-RD: 1.6 x 0.8 mm is an 0603")

        mcp.close()
    finally:
        server.stop()
    print("\nRESULT:", "PASS" if not FAILS else f"FAIL ({len(FAILS)})")
    sys.exit(1 if FAILS else 0)


if __name__ == "__main__":
    main()

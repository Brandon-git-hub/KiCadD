"""Hierarchical sheets end to end: an agent builds a three-sheet design on the headless server.

usage: python3 -I hier_e2e.py --build BUILD --kicad KICAD_SRC --work DIR

Blank project (as m5_e2e: Device symbols, a 60 x 40 mm board) plus a footprint library with one
two-pad footprint. Through kicadd-mcp:
  * sch_add_sheet Power and LED (new files), errors for a taken name / an unknown parent
  * parts on the sub-sheets (sch_add_part sheet), connected there; the nets that leave a sheet
    are ports: hierarchical labels inside, sheet pins on the sheet symbols (inputs left)
  * on the root the sheet pins are wired like part pins (Power.VOUT, LED.VCC): the netlist has
    one +5V net across all three sheets, one GND, the LED's local net stays /LED/LED_A
  * a port on a root pin is refused; sch_disconnect on a sheet pin splits the net, reconnect
  * save: Power.kicad_sch and LED.kicad_sch on disk; restart: the same netlist from the files
  * sync_pcb brings all parts onto the board with the cross-sheet nets; auto_place with
    groups "sheets" sees three blocks
  * sch_delete LED removes the sheet and its parts
"""

import argparse
import os
import re
import shutil
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from m5_e2e import FAILS, Mcp, Server, check, make_project  # noqa: E402

FP = "fp:SolderJumper-2"


def netlist(mcp):
    err, text = mcp.tool("sch_netlist")
    nets = {}
    for line in text.splitlines()[1:]:
        if line.startswith("unconnected"):
            continue
        name, _, pins = line.partition(":")
        nets[name.strip()] = set(pins.split())
    return nets


def net_with(nets, pin):
    return next((n for n, pins in nets.items() if pin in pins), None)


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

    # one two-pad footprint for every part
    pretty = os.path.join(proj, "fp.pretty")
    os.makedirs(pretty)
    shutil.copy(os.path.join(a.kicad, "demos", "pic_programmer", "libs", "pic_programmer_fp.pretty",
                             "SolderJumper-2_P1.3mm_Open_TrianglePad1.0x1.5mm.kicad_mod"),
                os.path.join(pretty, "SolderJumper-2.kicad_mod"))
    open(os.path.join(proj, "fp-lib-table"), "w").write(
        '(fp_lib_table\n  (version 7)\n  (lib (name "fp")(type "KiCad")(uri "${KIPRJMOD}/fp.pretty")'
        '(options "")(descr ""))\n)\n')

    sock = os.path.join(work, "s.sock")
    env = dict(os.environ, KICAD_RUN_FROM_BUILD_DIR="1", KICADD_LCSC_OFFLINE="1")
    server = Server(a.build, sock, os.path.join(proj, "blank.kicad_pcb"), env, os.path.join(work, "server.log"))
    mcp_cmd = [os.path.join(a.build, "kicadd-mcp"), "--socket", sock, "--stdio"]

    check(server.start(), "api-server up on the blank project")
    mcp = Mcp(mcp_cmd, env)
    try:
        print("[sheets]")
        err, t = mcp.tool("sch_add_sheet", {"name": "Power"})
        check(not err and t.startswith("added SHEET Power /Power/ (Power.kicad_sch) on /"), t.splitlines()[0][:90])
        err, t = mcp.tool("sch_add_sheet", {"name": "LED"})
        check(not err and "/LED/" in t, t.splitlines()[0][:90])
        err, t = mcp.tool("sch_add_sheet", {"name": "Power"})
        check(err and "already exists" in t, "a second Power is refused: " + t[:60])
        err, t = mcp.tool("sch_add_sheet", {"name": "X", "parent": "Nope"})
        check(err and "no sheet Nope" in t, "unknown parent refused: " + t[:60])
        err, t = mcp.tool("sch_add_part", {"lib_id": "Device:R", "sheet": "Nope"})
        check(err and "no sheet Nope" in t, "a part on an unknown sheet refused")

        print("[parts on the sub-sheets, ports]")
        added = {}
        for key, lib, value, sheet in (("R1", "Device:R", "100", "Power"), ("C1", "Device:C", "10u", "Power"),
                                       ("D1", "Device:LED", "red", "LED"), ("R2", "Device:R", "1k", "LED"),
                                       ("C2", "Device:C", "100n", "")):
            args = {"lib_id": lib, "value": value, "footprint": FP}
            if sheet:
                args["sheet"] = sheet
            err, t = mcp.tool("sch_add_part", args)
            m = re.match(r"added (\S+) ", t)
            added[key] = m.group(1) if m else None
            where = f" on /{sheet}/" if sheet else ""
            check(not err and m and (where in t.splitlines()[0] if sheet else " on " not in t.splitlines()[0]),
                  t.splitlines()[0][:100])
        r1, c1, d1, r2, c2 = (added[k] for k in ("R1", "C1", "D1", "R2", "C2"))

        err, t = mcp.tool("sch_connect", {"nets": {"VIN": [f"{r1}.1"], "VOUT": [f"{r1}.2", f"{c1}.1"],
                                                    "GND": [f"{c1}.2"]},
                                           "ports": ["VIN:input", "VOUT:output", "GND:passive"]})
        check(not err and t.startswith("4 pin(s) connected"), "Power: R1, C1 with ports VIN, VOUT, GND: " + t[:40])
        err, t = mcp.tool("sch_connect", {"nets": {"VCC": [f"{r2}.1"], "LED_A": [f"{r2}.2", f"{d1}.2"],
                                                    "GND": [f"{d1}.1"]},
                                           "ports": ["VCC:input", "GND:passive"]})
        check(not err and t.startswith("4 pin(s) connected"), "LED: R2, D1 with ports VCC, GND: " + t[:40])

        err, t = mcp.tool("sch_summary")
        power = re.search(r"SHEET Power .*?(?=\nSHEET|\n\S|\Z)", t, re.S)
        pins = dict(re.findall(r"pin (\S+) \S+ (left|right)", power.group(0))) if power else {}
        check(pins == {"VIN": "left", "VOUT": "right", "GND": "right"},
              f"Power's sheet pins: inputs left, the rest right {pins}")
        check("(not connected on /)" in t, "sheet pins not wired on the root yet")

        err, t = mcp.tool("sch_connect", {"nets": {"VOUT": [f"{c2}.1"]}, "ports": ["VOUT"]})
        check(err and "root sheet" in t, "a port on a root pin is refused: " + t.strip()[:80])

        print("[the root wires the sheets]")
        err, t = mcp.tool("sch_connect", {"nets": {"+5V": ["Power.VOUT", "LED.VCC"], "GND": ["Power.GND", "LED.GND",
                                                                                          f"{c2}.2"],
                                                    "VIN": ["Power.VIN", f"{c2}.1"]}})
        check(not err and t.startswith("7 pin(s) connected"), "sheet pins wired like part pins: " + t[:40])
        err, t = mcp.tool("sch_summary")
        check("pin VOUT output right" in t and "-> /+5V" in t, "the summary shows Power.VOUT -> /+5V")
        nets = netlist(mcp)
        five = net_with(nets, f"{r1}.2")
        check(five and {f"{r1}.2", f"{c1}.1", f"{r2}.1"} <= nets[five] and "+5V" in five,
              f"one +5V net across three sheets: {five} {sorted(nets.get(five, []))}")
        gnd = net_with(nets, f"{c1}.2")
        check(gnd and {f"{c1}.2", f"{d1}.1", f"{c2}.2"} <= nets[gnd], f"one GND: {gnd} {sorted(nets.get(gnd, []))}")
        vin = net_with(nets, f"{r1}.1")
        check(vin and f"{c2}.1" in nets[vin], f"VIN from the root into Power: {vin}")
        led = net_with(nets, f"{d1}.2")
        check(led == "/LED/LED_A" and nets[led] == {f"{d1}.2", f"{r2}.2"}, f"the LED's local net stays local: {led}")

        err, t = mcp.tool("sch_disconnect", {"pins": ["Power.VOUT"]})
        nets = netlist(mcp)
        check(not err and f"{r1}.2" not in nets.get(five, set()) and f"{r2}.1" in nets.get(five, set()),
              "sch_disconnect Power.VOUT splits +5V: " + str(net_with(nets, f"{r1}.2")))
        mcp.tool("sch_connect", {"nets": {"+5V": ["Power.VOUT"]}})
        nets = netlist(mcp)
        check({f"{r1}.2", f"{r2}.1"} <= nets.get(net_with(nets, f"{r1}.2"), set()), "reconnected")
        before = nets

        print("[save, restart]")
        err, t = mcp.tool("sch_save")
        check(not err, "sch_save: " + t[-50:])
        check(all(os.path.exists(os.path.join(proj, f)) for f in ("Power.kicad_sch", "LED.kicad_sch")),
              "Power.kicad_sch and LED.kicad_sch written")
        mcp.close()
        mcp = None
        server.stop()
        check(server.start(), "api-server restarted")
        mcp = Mcp(mcp_cmd, env)
        after = netlist(mcp)
        check(after == before, f"the same {len(after)} nets from the saved files")
        err, t = mcp.tool("sch_summary")
        check(" 5 parts" in t and t.count("SHEET ") == 2, "5 parts, 2 sheets after reload")

        print("[board]")
        err, t = mcp.tool("sync_pcb")
        check(not err and t.startswith("5 footprint(s) added"), "sync_pcb: " + t.splitlines()[0][:90])
        err, q = mcp.tool("query_net", {"net": five if five.startswith("/") else "/" + five})
        if "PAD" not in q:
            err, q = mcp.tool("query_net", {"net": five})
        have = set(re.findall(r"^PAD (\S+) ", q, re.M))
        check({f"{r1}.2", f"{c1}.1", f"{r2}.1"} <= have, f"the board's {five}: {sorted(have)}")
        err, t = mcp.tool("auto_place", {"groups": "sheets"})
        check(not err and "3 blocks (sheets)" in t, "auto_place sees 3 sheets: "
              + (re.search(r"\d+ blocks[^;]*", t) or re.search(r".*", t)).group(0)[:80])

        print("[delete a sheet]")
        err, t = mcp.tool("sch_delete", {"refs": ["LED"]})
        check(not err, "sch_delete LED: " + t[:60])
        err, t = mcp.tool("sch_summary")
        check(" 3 parts" in t and t.count("SHEET ") == 1 and "SHEET Power" in t, "Power and 3 parts left")
    finally:
        if mcp:
            mcp.close()
        server.stop()

    print("\nRESULT:", "PASS" if not FAILS else f"FAIL ({len(FAILS)})")
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())

"""Schematic editing end to end (headless): power symbols, sch_move, sch_wire, run_erc, lib_register.

usage: python3 -I sch_edit_e2e.py --build BUILD --kicad KICAD_SRC --work DIR

Blank project (m5_e2e's: Device symbols, a 60 x 40 mm board, no power library):
  * sch_connect power: GND / +3V3 get power symbols (made here: no library), the nets are right,
    the #PWR symbols are not parts (summary, netlist); sch_add_part lib_id power:GND works too
  * sch_move dx/dy, rotation, x/y: the nets stay the same; sch_disconnect takes a power symbol off
  * sch_wire: an L to a pin joins that pin's net; a T onto its middle adds a junction
  * run_erc lists violations; lib_register adds a .kicad_sym and a .pretty to the project tables
  * save, restart: the same netlist from the file; sync_pcb brings no #PWR footprints
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
        if line.startswith(("unconnected", "no-connect")):
            continue
        name, _, pins = line.partition(":")
        nets[name.strip()] = set(pins.split())
    return nets


def pins_of(mcp, ref):
    err, t = mcp.tool("sch_part", {"ref": ref})
    out = {}
    for m in re.finditer(r"^\s+(\S+) .*?@(-?[\d.]+),(-?[\d.]+)(?: -> (\S+))?", t, re.M):
        out[m.group(1)] = (float(m.group(2)), float(m.group(3)), m.group(4))
    return out


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

    pretty = os.path.join(proj, "fp.pretty")
    os.makedirs(pretty)
    src_fp = os.path.join(a.kicad, "demos", "pic_programmer", "libs", "pic_programmer_fp.pretty",
                          "SolderJumper-2_P1.3mm_Open_TrianglePad1.0x1.5mm.kicad_mod")
    shutil.copy(src_fp, os.path.join(pretty, "SolderJumper-2.kicad_mod"))
    open(os.path.join(proj, "fp-lib-table"), "w").write(
        '(fp_lib_table\n  (version 7)\n  (lib (name "fp")(type "KiCad")(uri "${KIPRJMOD}/fp.pretty")'
        '(options "")(descr ""))\n)\n')
    # libraries on disk but not in the tables yet (lib_register)
    os.makedirs(os.path.join(proj, "mylib", "Mine.pretty"))
    shutil.copy(os.path.join(proj, "Device.kicad_sym"), os.path.join(proj, "mylib", "Mine.kicad_sym"))
    shutil.copy(src_fp, os.path.join(proj, "mylib", "Mine.pretty", "Jumper.kicad_mod"))

    sock = os.path.join(work, "s.sock")
    env = dict(os.environ, KICAD_RUN_FROM_BUILD_DIR="1", KICADD_LCSC_OFFLINE="1")
    server = Server(a.build, sock, os.path.join(proj, "blank.kicad_pcb"), env, os.path.join(work, "server.log"))
    mcp_cmd = [os.path.join(a.build, "kicadd-mcp"), "--socket", sock, "--stdio"]

    check(server.start(), "api-server up on the blank project")
    mcp = Mcp(mcp_cmd, env)
    try:
        print("[power symbols]")
        refs = {}
        for key, lib, value, x, y in (("R", "Device:R", "10k", 50.8, 50.8), ("C", "Device:C", "100n", 101.6, 50.8)):
            err, t = mcp.tool("sch_add_part", {"lib_id": lib, "value": value, "footprint": FP, "x": x, "y": y})
            refs[key] = re.match(r"added (\S+) ", t).group(1)
            check(not err, t.splitlines()[0][:90])
        r1, c1 = refs["R"], refs["C"]
        err, t = mcp.tool("sch_connect", {"nets": {"N1": [f"{r1}.2", f"{c1}.1"], "GND": [f"{c1}.2"],
                                                    "+3V3": [f"{r1}.1"]},
                                           "power": ["GND", "+3V3"]})
        check(not err and t.startswith("4 pin(s) connected"), "sch_connect with power: " + t[:60])
        nets = netlist(mcp)
        check(nets.get("/N1") == {f"{r1}.2", f"{c1}.1"}, f"N1 {nets.get('N1')}")
        check(nets.get("GND") == {f"{c1}.2"} and nets.get("+3V3") == {f"{r1}.1"},
              f"GND {nets.get('GND')}, +3V3 {nets.get('+3V3')} (power symbols)")
        err, t = mcp.tool("sch_summary")
        check("#PWR" not in t, "the power symbols are not listed as parts")
        err, t = mcp.tool("sch_add_part", {"lib_id": "power:GND", "x": 127, "y": 76.2})
        check(not err and t.startswith("added #PWR"), "sch_add_part power:GND: " + t.splitlines()[0][:70])
        before = netlist(mcp)

        print("[sch_move]")
        err, t = mcp.tool("sch_move", {"ref": r1, "dx": 25.4, "dy": 12.7})
        check(not err and t.startswith("moved"), t.splitlines()[0][:80])
        check(netlist(mcp) == before, "nets unchanged after dx/dy")
        p = pins_of(mcp, r1)
        check(abs(p["1"][0] - 76.2) < 0.01, f"R pin 1 moved with it {p['1'][:2]}")
        err, t = mcp.tool("sch_move", {"ref": r1, "rotation": 90})
        q = pins_of(mcp, r1)
        check(not err and netlist(mcp) == before, "nets unchanged after rotation 90")
        check(q["1"][1] == q["2"][1] and q["1"][0] != q["2"][0], f"rotated: pins side by side {q['1'][:2]} {q['2'][:2]}")
        err, t = mcp.tool("sch_move", {"ref": c1, "x": 152.4, "y": 101.6})
        check(not err and "@152.4,101.6" in t.splitlines()[0] and netlist(mcp) == before, "x/y: " + t.splitlines()[0][:80])
        err, t = mcp.tool("sch_move", {"ref": "Q9", "dx": 1})
        check(err and "no part Q9" in t, "unknown part refused")

        print("[sch_disconnect a power pin]")
        err, t = mcp.tool("sch_disconnect", {"pins": [f"{c1}.2"]})
        nets = netlist(mcp)
        check(not err and f"{c1}.2" not in set().union(*nets.values()), "C pin 2 off GND (its power symbol gone)")
        err, t = mcp.tool("sch_connect", {"nets": {"GND": [f"{c1}.2"]}, "power": ["GND"]})
        check(not err and netlist(mcp) == before, "reconnected")

        print("[sch_wire]")
        err, t = mcp.tool("sch_add_part", {"lib_id": "Device:R", "value": "1k", "footprint": FP, "x": 50.8, "y": 127})
        r2 = re.match(r"added (\S+) ", t).group(1)
        err, t = mcp.tool("sch_add_part", {"lib_id": "Device:R", "value": "2k", "footprint": FP, "x": 25.4, "y": 152.4})
        r3 = re.match(r"added (\S+) ", t).group(1)
        cp = pins_of(mcp, c1)["1"]
        rp = pins_of(mcp, r2)["1"]
        corner = (rp[0], cp[1])
        err, t = mcp.tool("sch_wire", {"points": [[rp[0], rp[1]], list(corner), [cp[0], cp[1]]]})
        print("      ", t.splitlines()[0][:110])
        check(not err and "net /N1" in t and f"{r2}.1" in t and f"{c1}.1" in t, "an L from R2.1 to C1.1 joins N1")
        # a T: from R3.1 up to the middle of the wire's first leg
        r3p = pins_of(mcp, r3)["1"]
        mid_y = round((rp[1] + corner[1]) / 2 / 1.27) * 1.27
        err, t = mcp.tool("sch_wire", {"points": [[r3p[0], r3p[1]], [r3p[0], mid_y], [rp[0], mid_y]]})
        print("      ", t.splitlines()[0][:110])
        check(not err and "1 junction" in t and f"{r3}.1" in t, "a T onto the wire: one junction, R3.1 on N1")
        nets = netlist(mcp)
        check({f"{r2}.1", f"{r3}.1"} <= nets.get("/N1", set()), f"N1 now {sorted(nets.get('N1', []))}")

        print("[wires off: sch_disconnect, sch_delete wires / dangling]")
        err, t = mcp.tool("sch_disconnect", {"pins": [f"{r2}.1"]})
        nets = netlist(mcp)
        on = {p for ps in nets.values() for p in ps}
        n1 = next((ps for ps in nets.values() if f"{c1}.1" in ps), set())
        check(not err and f"{r2}.1" not in on and f"{r3}.1" in n1,
              f"R2.1 off: its wire up to the T is gone, R3.1 and C1.1 still joined ({sorted(n1)})")
        err, t = mcp.tool("sch_delete", {"dangling": True})
        check(not err and t.startswith("0 "), "nothing left hanging: " + t[:60])
        err, t = mcp.tool("sch_wire", {"points": [[rp[0], rp[1]], [rp[0] - 5.08, rp[1]]]})
        check(not err, "a wire from R2.1 to nowhere: " + t.splitlines()[0][:60])
        err, t = mcp.tool("sch_delete", {"wires": [[rp[0] - 2.54, rp[1]]]})
        check(not err and "1 wire" in t, "sch_delete wires: a point on it deletes it: " + t[:60])
        err, t = mcp.tool("sch_delete", {"wires": [[rp[0] - 2.54, rp[1]]]})
        check(err and "no wire at" in t, "no wire there any more")
        mcp.tool("sch_wire", {"points": [[152.4, 152.4], [162.56, 152.4], [162.56, 160.02]]})
        err, t = mcp.tool("sch_delete", {"dangling": True})
        check(not err and "1 wire" in t, "dangling=true: a two-segment wire on nothing, one wire: " + t[:60])
        check(netlist(mcp) == nets, "the nets as before")
        after_wires = nets

        print("[run_erc]")
        err, t = mcp.tool("run_erc", {"warnings": True})
        print("      ", " | ".join(t.splitlines()[:3])[:200])
        check(not err and t.startswith("ERC:"), "run_erc answers")
        check("power_pin_not_driven" in t, "the undriven power nets are reported (no PWR_FLAG)")

        print("[lib_register]")
        err, t = mcp.tool("lib_register", {"path": "mylib/Mine.kicad_sym"})
        print("      ", t.splitlines()[0][:100])
        check(not err and t.startswith("added Mine -> ${KIPRJMOD}/mylib/Mine.kicad_sym in sym-lib-table"), "symbol library added")
        check(re.search(r"\((\d+) symbol", t) and int(re.search(r"\((\d+) symbol", t).group(1)) > 10, "it has symbols")
        err, t = mcp.tool("sch_add_part", {"lib_id": "Mine:R", "footprint": FP})
        check(not err and t.startswith("added R"), "sch_add_part Mine:R: " + t.splitlines()[0][:60])
        err, t = mcp.tool("lib_register", {"path": "mylib/Mine.kicad_sym"})
        check(not err and "already" in t, "again: already there")
        err, t = mcp.tool("lib_register", {"path": "mylib/Mine.kicad_sym", "nickname": "Device"})
        check(err and "already has a library Device" in t, "a taken nickname is refused")
        err, t = mcp.tool("lib_register", {"path": os.path.join(proj, "mylib", "Mine.pretty")})
        check(not err and "fp-lib-table (1 footprint" in t, "footprint library added: " + t[:90])
        table = open(os.path.join(proj, "fp-lib-table")).read()
        check('(name "Mine")' in table and "${KIPRJMOD}/mylib/Mine.pretty" in table, "fp-lib-table row on disk")
        err, t = mcp.tool("add_footprint", {"footprint": "Mine:Jumper", "ref": "JP?", "x": 10, "y": 10})
        check(not err and "from library table" in t, "add_footprint finds Mine:Jumper right away: " + t.splitlines()[0][:60])
        mcp.tool("undo")

        print("[save, restart, sync]")
        err, t = mcp.tool("sch_save")
        check(not err, "saved")
        mcp.close()
        server.stop()
        check(server.start(), "restarted")
        mcp = Mcp(mcp_cmd, env)
        nets = netlist(mcp)
        check(nets == after_wires, "the same nets from the file (power symbols and wires saved)")
        err, t = mcp.tool("sync_pcb")
        print("      ", t.splitlines()[0][:110])
        err, s = mcp.tool("board_summary")
        n = int(re.search(r"footprints=(\d+)", s).group(1))
        check(not err and n == 5, f"sync_pcb: 5 parts, no #PWR footprints (footprints={n})")
    finally:
        mcp.close()
        server.stop()
    print("\nRESULT:", "PASS" if not FAILS else f"FAIL ({len(FAILS)})")
    sys.exit(1 if FAILS else 0)


if __name__ == "__main__":
    main()

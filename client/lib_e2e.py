"""New symbols and footprints (headless): lib_create_symbol, lib_create_footprint, used in a design.

usage: python3 -I lib_e2e.py --build BUILD --kicad KICAD_SRC --work DIR

Blank project (m5_e2e's). A box symbol from a pin list goes to kicadd.kicad_sym (registered),
sch_add_part places it with its pins on the asked sides; footprints for each kind go to
kicadd.pretty with the right pad count (SOIC-8 against KiCad's own land pattern), a schematic
part using kicadd:R_0603 reaches the board through sync_pcb, and DRC finds the footprint the
same as its library.
"""
import argparse, os, re, shutil, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from m5_e2e import FAILS, Mcp, Server, check, make_project  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", required=True); ap.add_argument("--kicad", required=True); ap.add_argument("--work", required=True)
    a = ap.parse_args()
    work = os.path.abspath(a.work)
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(work)
    proj = make_project(a.kicad, work)
    sock = os.path.join(work, "s.sock")
    env = dict(os.environ, KICAD_RUN_FROM_BUILD_DIR="1", KICADD_LCSC_OFFLINE="1")
    server = Server(a.build, sock, os.path.join(proj, "blank.kicad_pcb"), env, os.path.join(work, "server.log"))
    check(server.start(), "api-server up")
    mcp = Mcp([os.path.join(a.build, "kicadd-mcp"), "--socket", sock, "--stdio"], env)
    try:
        print("[lib_create_symbol]")
        pins = [{"number": "1", "name": "VIN", "type": "power_in"}, {"number": "2", "name": "GND", "type": "power_in", "side": "bottom"},
                {"number": "3", "name": "EN", "type": "input"}, {"number": "4", "name": "VOUT", "type": "power_out"},
                {"number": "5", "name": "PG", "type": "open_collector"}, {"number": "6", "name": "NC", "type": "no_connect", "side": "top"}]
        err, t = mcp.tool("lib_create_symbol", {"name": "MYLDO", "reference": "U", "value": "MYLDO-3.3",
                                                "footprint": "kicadd:SOT-23-5", "pins": pins})
        print("      ", t[:140])
        check(not err and t.startswith("made kicadd:MYLDO: 6 pins") and "added to sym-lib-table" in t, "symbol made, library registered")
        check('(name "kicadd")' in open(os.path.join(proj, "sym-lib-table")).read(), "sym-lib-table row on disk")
        check("no footprint library kicadd" in t, "the footprint it names is not there yet: said so")
        err, t = mcp.tool("lib_create_symbol", {"name": "MYLDO", "pins": pins})
        check(err and "exists already" in t, "a second one of that name is refused")
        err, t = mcp.tool("lib_create_symbol", {"name": "MYLDO", "pins": pins + [{"number": "1"}]})
        check(err and "twice" in t, "a pin number twice is refused")
        err, t = mcp.tool("sch_add_part", {"lib_id": "kicadd:MYLDO", "x": 50.8, "y": 50.8})
        sides = dict(re.findall(r"^\s+\d+ (\S+) \S+ (left|right|up|down) @", t, re.M))
        print("      ", t.splitlines()[0][:100])
        check(not err and t.startswith("added U1 MYLDO-3.3 kicadd:MYLDO [kicadd:SOT-23-5]"), "placed from kicadd:MYLDO")
        check(sides == {"VIN": "left", "EN": "left", "VOUT": "right", "PG": "right", "GND": "down", "NC": "up"},
              f"pins on their sides {sides}")
        types = dict(re.findall(r"^\s+\d+ (\S+) (\S+) ", t, re.M))
        check(types.get("VIN") == "power_in" and types.get("PG") == "open_collector", f"pin types {types}")

        print("[lib_create_footprint]")
        # the first footprint registers kicadd.pretty; add_footprint finds it at once
        err, t = mcp.tool("lib_create_footprint", {"kind": "sot23", "pins": 5})
        check(not err and "added to fp-lib-table" in t, "SOT-23-5 made, kicadd.pretty registered")
        err, t = mcp.tool("add_footprint", {"footprint": "kicadd:SOT-23-5", "ref": "U?", "x": 10, "y": 10})
        check(not err and "from library table" in t, "add_footprint finds it right away: " + t.splitlines()[0][:60])
        mcp.tool("undo")
        err, t = mcp.tool("sch_add_part", {"lib_id": "kicadd:MYLDO", "x": 152.4, "y": 50.8})
        check(not err and "has no" not in t and "no footprint library" not in t, "now MYLDO's footprint is found")
        mcp.tool("sch_delete", {"refs": [re.match(r"added (\S+)", t).group(1)]})
        err, t = mcp.tool("sch_add_part", {"lib_id": "kicadd:MYLDO", "footprint": "kicadd:Nope", "x": 152.4, "y": 50.8})
        check(not err and "footprint library kicadd has no Nope" in t, "sch_add_part with a missing footprint: said so")
        mcp.tool("sch_delete", {"refs": [re.match(r"added (\S+)", t).group(1)]})
        for args, pads, name in (({"kind": "chip", "size": "0603", "prefix": "R"}, 2, "R_0603"),
                                 ({"kind": "chip", "size": 402, "prefix": "C"}, 2, "C_0402"),
                                 ({"kind": "soic", "pins": 8}, 8, "SOIC-8_3.9mm_P1.27mm"),
                                 ({"kind": "so", "pins": 14, "pitch": 0.65, "body": 4.4, "span": 6.4}, 14, "SO-14_4.4mm_P0.65mm"),
                                 ({"kind": "qfn", "pins": 32, "pitch": 0.5, "body": 5, "ep": 3.4}, 33, "QFN-32_5x5mm_P0.5mm_EP3.4mm"),
                                 ({"kind": "qfp", "pins": 48, "pitch": 0.5, "body": 7}, 48, "QFP-48_7x7mm_P0.5mm"),
                                 ({"kind": "header", "pins": 10, "rows": 2}, 10, "Header_2x05_P2.54mm")):
            err, t = mcp.tool("lib_create_footprint", args)
            check(not err and t.startswith(f"made kicadd:{name}: {pads} pads"), t[:100])
        err, t = mcp.tool("lib_create_footprint", {"kind": "soic", "pins": 7})
        check(err and "even" in t, "an odd SOIC is refused")
        err, t = mcp.tool("lib_create_footprint", {"kind": "chip", "size": "0603", "prefix": "R"})
        check(err and "exists already" in t, "the same name again is refused")

        # SOIC-8 against KiCad's own land pattern (a demo's copy): pad 1 within 0.1 mm
        ref = open(os.path.join(a.kicad, "demos", "kit-dev-coldfire-xilinx_5213", "kit-dev-coldfire.pretty",
                                "SOIC-8_3.9x4.9mm_P1.27mm.kicad_mod")).read()
        ours = open(os.path.join(proj, "kicadd.pretty", "SOIC-8_3.9mm_P1.27mm.kicad_mod")).read()
        pat = r'\(pad "1"[^()]*\(at (\S+) (\S+)\)\s*\(size (\S+) (\S+)\)'
        r1 = [float(v) for v in re.search(pat, " ".join(ref.split())).groups()]
        o1 = [float(v) for v in re.search(pat, ours).groups()]
        check(all(abs(x - y) <= 0.1 for x, y in zip(r1, o1)), f"SOIC-8 pad 1 {o1} vs KiCad {r1}")

        print("[used in a design]")
        err, t = mcp.tool("sch_add_part", {"lib_id": "Device:R", "value": "1k", "footprint": "kicadd:R_0603", "x": 101.6, "y": 50.8})
        check(not err, "a resistor on kicadd:R_0603")
        mcp.tool("sch_connect", {"nets": {"VIN": ["U1.1", "R1.1"], "GND": ["U1.2"], "PG": ["U1.5", "R1.2"]}, "power": ["GND"]})
        err, t = mcp.tool("sync_pcb")
        print("      ", t.splitlines()[0][:100])
        check(not err and "2 footprint(s) added" in t, "sync_pcb brings U1 and R1")
        err, q = mcp.tool("query_net", {"net": "/PG"})
        check("PAD U1.5" in q and "PAD R1.2" in q, "their pads on /PG")
        err, t = mcp.tool("run_drc", {"include_warnings": True})
        check("lib_footprint" not in t, "DRC: the footprints match their library (" + " ".join(t.split())[:100] + ")")
    finally:
        mcp.close()
        server.stop()
    print("\nRESULT:", "PASS" if not FAILS else f"FAIL ({len(FAILS)})")
    sys.exit(1 if FAILS else 0)


if __name__ == "__main__":
    main()

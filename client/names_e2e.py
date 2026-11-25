"""Net names as the agent gives them (headless): one set of rules (src/common/kicadd_names.h) for the
board tools, the schematic tools and kicadd-mcp.

usage: python3 -I names_e2e.py --build BUILD --demos DEMOS_DIR --work DIR [--ngspice NGSPICE]

KiCad's complex_hierarchy demo (two instances of one sheet: /ampli_ht_vertical/PIEZO_IN and
/ampli_ht_horizontal/PIEZO_IN; /12Vext on the root sheet; GND, VCC):
  rule 1   the same name                       "/12Vext"
  rule 2   the leading "/" added or taken off  "12Vext"; "ampli_ht_vertical/PIEZO_IN"
  rule 3   a unique leaf                       none here with two instances: "PIEZO_IN" is refused,
                                               naming both; cm5's "/CM5/GPIO7" as "GPIO7"
  rule 4   1 - 3 again, blind to case          "gnd", "12vext"; cm5's "cc1" is "/CC1" (not "/USB/CC1")
  none     the three closest names             "S_OUT" -> the two S_OUT+ ...
Pin types (mcp/pintypes.h): the pad's, else the schematic's; reflection finds its driver on a copy
of the board without any (pintype ...), and says where the type came from (with --ngspice).
Checked on: query_net (board handler), topology (reflection's net), board_render nets (globs too,
blind to case), delete_tracks nets, sch_render nets (schematic), simulate around (the schematic's
region) and simulate sources / probes (kicadd-mcp).
"""

import argparse
import os
import re
import shutil
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from m5_e2e import FAILS, Mcp, Server, binaries, check  # noqa: E402


def main():
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", required=True)
    ap.add_argument("--demos", required=True)
    ap.add_argument("--work", required=True)
    ap.add_argument("--ngspice", default="")
    a = ap.parse_args()
    work = os.path.abspath(a.work)
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(work)
    _, kicadd_mcp, env = binaries(os.path.abspath(a.build))
    env = dict(env, KICADD_LCSC_OFFLINE="1")
    if a.ngspice:
        env["KICADD_NGSPICE"] = os.path.abspath(a.ngspice)

    def serve(demo, board, tag="", strip_types=False):
        d = os.path.join(work, demo.replace(" ", "_") + tag)
        shutil.copytree(os.path.join(a.demos, demo), d)
        if strip_types:   # a board from elsewhere: no pin types on its pads
            f = os.path.join(d, board)
            t = open(f, encoding="utf-8").read()
            open(f, "w", encoding="utf-8").write(re.sub(r'\s*\(pintype "[^"]*"\)', "", t))
        sock = os.path.join(work, demo[:4] + tag + ".sock")
        server = Server(a.build, sock, os.path.join(d, board), env, os.path.join(work, demo[:4] + tag + ".log"))
        check(server.start(), f"api-server up on {demo}")
        return server, Mcp([kicadd_mcp, "--socket", sock, "--stdio"], env)

    server, mcp = serve("complex_hierarchy", "complex_hierarchy.kicad_pcb")
    try:
        def net_of(name):
            err, t = mcp.tool("query_net", {"net": name})
            return err, t

        print("[board: query_net]")
        for want, got in [("/12Vext", "/12Vext"), ("12Vext", "/12Vext"), ("ampli_ht_vertical/PIEZO_IN", "/ampli_ht_vertical/PIEZO_IN"),
                          ("gnd", "GND"), ("12vext", "/12Vext"), ("/ampli_ht_vertical/piezo_in", "/ampli_ht_vertical/PIEZO_IN")]:
            err, t = net_of(want)
            check(not err and got in t, f"{want!r} -> {got}: " + t.splitlines()[0][:90])
        err, t = net_of("PIEZO_IN")
        check(err and "which net" in t and "/ampli_ht_horizontal/PIEZO_IN" in t and "/ampli_ht_vertical/PIEZO_IN" in t,
              "'PIEZO_IN' (two sheet instances): refused, naming both: " + t.splitlines()[0][:120])
        err, t = net_of("S_OUT")
        check(err and "no net S_OUT (closest:" in t and "S_OUT+" in t, "a typo: the closest names: " + t.splitlines()[0][:120])

        print("[board: topology, board_render, delete_tracks]")
        if a.ngspice:
            err, t = mcp.tool("reflection", {"net": "vcc", "rise": "1n"})
            check("no net" not in t, "reflection {net: vcc}: VCC found (" + t.splitlines()[0][:80] + ")")
        err, t, imgs = mcp.tool_images("board_render", {"nets": ["*piezo_out", "gnd"], "size": 400})
        check(not err and imgs and "missing" not in t.lower(), "board_render nets ['*piezo_out', 'gnd']: both instances and GND drawn: " + t[:100])
        err, t, imgs = mcp.tool_images("board_render", {"nets": ["PIEZO_OUT"], "size": 400})
        check("which net" in t, "board_render nets ['PIEZO_OUT']: which one, said: " + t[:140])
        err, t = mcp.tool("delete_tracks", {"nets": ["nosuchnet"]})
        check(err or "closest" in t, "delete_tracks an unknown net: the closest names: " + t[:120])

        print("[schematic: sch_render, simulate around]")
        err, t, imgs = mcp.tool_images("sch_render", {"nets": ["vcc"], "size": 400})
        check(not err and imgs and "missing" not in t.lower(), "sch_render nets ['vcc']: VCC drawn: " + t[:100])
        err, t = mcp.tool("sch_netlist", {"net": "vcc"})
        check(not err and t.startswith("1 nets") and "\nVCC:" in t, "sch_netlist {net: vcc}: VCC: " + t[:80].replace("\n", " | "))
        err, t = mcp.tool("sch_netlist", {"net": "PIEZO_IN"})
        check(err and "which net" in t, "sch_netlist {net: PIEZO_IN}: which one, said: " + t[:120])
        err, t = mcp.tool("simulate", {"around": ["PIEZO_IN"]})
        check(err and "which part or net" in t, "simulate around ['PIEZO_IN']: which one, said: " + t[:140])
        err, t = mcp.tool("simulate", {"around": ["12vxt"]})
        check(err and "closest" in t and "/12Vext" in t, "simulate around ['12vxt'] (a typo): the closest: " + t[:140])
        if a.ngspice:
            err, t = mcp.tool("simulate", {"around": ["12vext"], "sources": {"12VEXT": 12}})
            check("no part or net" not in t and "no net" not in t, "around / sources by another case: found (" + t.splitlines()[0][:100] + ")")
    finally:
        mcp.close()
        server.stop()

    print("[pin types: the board's, else the schematic's (mcp/pintypes.h)]")
    for tag, strip in (("", False), ("_nt", True)) if a.ngspice else ():
        server, mcp = serve("complex_hierarchy", "complex_hierarchy.kicad_pcb", "_pt" + tag, strip)
        try:
            err, t = mcp.tool("reflection", {"net": "ampli_ht_horizontal/Vpil_0_3,3V", "rise": "1n"})
            first = t.splitlines()[0] if t else ""
            check(not err and "driver U301.7 (output)" in first and (("pin types from the schematic" in first) == strip),
                  ("no pin types on the board: the schematic's, said" if strip else "the board's own pin types") + ": " + first[:140])
        finally:
            mcp.close()
            server.stop()

    server, mcp = serve("cm5_minima", "CM5_MINIMA_3.kicad_pcb")
    try:
        print("[cm5: a unique leaf]")
        err, t = mcp.tool("query_net", {"net": "GPIO7"})
        check(not err and "/CM5/GPIO7" in t, "'GPIO7' -> /CM5/GPIO7 (its leaf, one net has it): " + t.splitlines()[0][:90])
        err, t = mcp.tool("query_net", {"net": "cc1"})
        check(not err and "/CC1" in t and "/USB/CC1" not in t, "'cc1' -> /CC1 (case blind: '/' added comes before the leaf of /USB/CC1): " + t.splitlines()[0][:90])
        err, t = mcp.tool("query_net", {"net": "CC2"})
        check(not err and "/USB/CC2" not in t, "'CC2' -> /CC2 ('/' added, before the leaf of /USB/CC2): " + t.splitlines()[0][:90])
    finally:
        mcp.close()
        server.stop()

    print("\nRESULT:", "PASS" if not FAILS else f"FAIL ({len(FAILS)})")
    sys.exit(1 if FAILS else 0)


if __name__ == "__main__":
    main()

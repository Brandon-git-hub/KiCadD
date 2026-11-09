"""M5 end to end: an agent draws a schematic from nothing on the headless server.

usage: python3 -I m5_e2e.py --build BUILD --kicad KICAD_SRC --work DIR --cache LCSC_CACHE

Makes a blank project in DIR (board = 60 x 40 mm outline, empty schematic, the Device symbol
library from KiCad's QA data), starts `kicad-cli api-server` on it (KICADD_LCSC_OFFLINE: parts
come from the cache) and, through kicadd-mcp:
  * places LCSC parts (AMS1117-3.3, 2 x 100 nF, 10 k) and a library part (Device:R with an
    LCSC footprint), automatically numbered and placed
  * connects them by net name and checks the netlist pin by pin
  * a pin already on another net is refused, NC flags, disconnect, delete with its labels
  * saves, restarts the server and finds the same netlist in the saved file
  * M5b: sync_pcb brings the 5 footprints onto the board with the schematic's nets (pad by pad)
  * M5c: auto_place puts them inside the outline: full DRC has no courtyard, clearance or
    silkscreen errors (only the unrouted connections), the regulator's caps sit next to it,
    and undo takes the placement back
"""

import argparse
import json
import math
import os
import re
import shutil
import subprocess
import sys
import time
import uuid

FAILS = []


def check(cond, label):
    print(("  ok   " if cond else "  FAIL ") + label, flush=True)
    if not cond:
        FAILS.append(label)


class Mcp:
    def __init__(self, cmd, env):
        self.p = subprocess.Popen(cmd, env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True,
                                  encoding="utf-8")
        self.rid = 0
        self.rpc("initialize", {"protocolVersion": "2025-06-18", "capabilities": {},
                                "clientInfo": {"name": "m5-e2e", "version": "1"}})
        self.p.stdin.write(json.dumps({"jsonrpc": "2.0", "method": "notifications/initialized"}) + "\n")
        self.p.stdin.flush()

    def rpc(self, method, params):
        self.rid += 1
        self.p.stdin.write(json.dumps({"jsonrpc": "2.0", "id": self.rid, "method": method, "params": params}) + "\n")
        self.p.stdin.flush()
        return json.loads(self.p.stdout.readline())

    def tool(self, name, args=None):
        r = self.rpc("tools/call", {"name": name, "arguments": args or {}}).get("result", {})
        return bool(r.get("isError")), "".join(c.get("text", "") for c in r.get("content", []))

    def close(self):
        self.p.stdin.close()
        self.p.wait(10)


def top_level_blocks(text):
    """(name, start, end) of the children of the outer s-expression."""
    out, depth, i, start = [], 0, 0, None
    while i < len(text):
        c = text[i]
        if c == '"':
            i += 1
            while text[i] != '"':
                i += 2 if text[i] == "\\" else 1
        elif c == "(":
            depth += 1
            if depth == 2:
                start = i
        elif c == ")":
            if depth == 2:
                name = re.match(r"\((\S+)", text[start:start + 40]).group(1)
                out.append((name, start, i + 1))
            depth -= 1
        i += 1
    return out


def make_project(kicad, work):
    demo = os.path.join(kicad, "demos", "pic_programmer")
    proj = os.path.join(work, "proj")
    os.makedirs(proj)
    shutil.copy(os.path.join(demo, "pic_programmer.kicad_pro"), os.path.join(proj, "blank.kicad_pro"))

    pcb = open(os.path.join(demo, "pic_programmer.kicad_pcb"), encoding="utf-8").read()
    keep = {"version", "generator", "generator_version", "general", "paper", "layers", "setup"}
    body = "\n\t".join(pcb[s:e] for n, s, e in top_level_blocks(pcb) if n in keep)
    outline = ('(gr_rect (start 100 100) (end 160 140) (stroke (width 0.1) (type default)) (fill no) '
               '(layer "Edge.Cuts") (uuid "%s"))' % uuid.uuid4())
    open(os.path.join(proj, "blank.kicad_pcb"), "w", encoding="utf-8").write(
        "(kicad_pcb\n\t" + body + "\n\t" + outline + "\n)\n")

    sch = open(os.path.join(demo, "pic_programmer.kicad_sch"), encoding="utf-8").read()
    version = re.search(r"\(version (\d+)\)", sch).group(1)
    open(os.path.join(proj, "blank.kicad_sch"), "w", encoding="utf-8").write(
        f'(kicad_sch (version {version}) (generator "eeschema") (generator_version "10.0") '
        f'(uuid "{uuid.uuid4()}") (paper "A4") (lib_symbols) '
        f'(sheet_instances (path "/" (page "1"))) (embedded_fonts no))\n')

    shutil.copy(os.path.join(kicad, "qa", "data", "libraries", "Device.kicad_sym"), proj)
    open(os.path.join(proj, "sym-lib-table"), "w").write(
        '(sym_lib_table\n  (version 7)\n  (lib (name "Device")(type "KiCad")(uri "${KIPRJMOD}/Device.kicad_sym")'
        '(options "")(descr ""))\n)\n')
    return proj


class Server:
    def __init__(self, build, sock, board, env, log):
        self.cmd = [os.path.join(build, "kicad", "kicad-cli"), "api-server", "--socket", sock, board]
        self.env, self.log, self.sock = env, log, sock
        self.p = None

    def start(self):
        if os.path.exists(self.sock):
            os.unlink(self.sock)
        self.p = subprocess.Popen(self.cmd, env=self.env, stdout=open(self.log, "a"), stderr=subprocess.STDOUT)
        for _ in range(240):     # the log is block-buffered: wait for the socket
            if os.path.exists(self.sock):
                time.sleep(0.5)
                return self.p.poll() is None
            if self.p.poll() is not None:
                return False
            time.sleep(0.5)
        return False

    def stop(self):
        if self.p:
            self.p.terminate()
            self.p.wait(30)
            open(self.log, "a").write("\n--restart--\n")


def pins_of(text):
    """{number: (name, side)} from an sch_add_part / sch_part answer."""
    return {m.group(1): (m.group(2) or "", m.group(4))
            for m in re.finditer(r"^  (\S+)(?: (\S+))? (\S+) (left|right|up|down) @", text, re.M)}


def netlist(mcp):
    err, text = mcp.tool("sch_netlist")
    nets = {}
    for line in text.splitlines()[1:]:
        if line.startswith("unconnected"):
            continue
        name, _, pins = line.partition(":")
        nets[name.lstrip("/")] = set(pins.split())
    unconnected = set(re.search(r"unconnected \(\d+\):(.*)", text).group(1).split()) \
        if "unconnected (" in text else set()
    return nets, unconnected


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", required=True)
    ap.add_argument("--kicad", required=True)
    ap.add_argument("--work", required=True)
    ap.add_argument("--cache", required=True)
    a = ap.parse_args()

    work = os.path.abspath(a.work)
    if os.path.exists(work):
        shutil.rmtree(work)
    os.makedirs(work)
    proj = make_project(a.kicad, work)
    sock = os.path.join(work, "s.sock")
    env = dict(os.environ, KICADD_LCSC_CACHE=os.path.abspath(a.cache), KICADD_LCSC_OFFLINE="1",
               KICAD_RUN_FROM_BUILD_DIR="1")
    server = Server(a.build, sock, os.path.join(proj, "blank.kicad_pcb"), env, os.path.join(work, "server.log"))
    mcp_cmd = [os.path.join(a.build, "kicadd-mcp"), "--socket", sock, "--stdio"]

    print("[blank project]")
    check(server.start(), "api-server up with blank.kicad_pcb (+ blank.kicad_sch)")
    mcp = Mcp(mcp_cmd, env)
    try:
        err, text = mcp.tool("sch_summary")
        check(not err and " 0 parts" in text, "empty schematic: " + text.splitlines()[0][-60:])

        print("[parts]")
        err, u1 = mcp.tool("sch_add_part", {"lcsc": "C6186"})
        check(not err and u1.startswith("added U1 AMS1117-3.3 LCSC:AMS1117-3.3"), "LCSC C6186 -> " + u1.splitlines()[0][:90])
        p = pins_of(u1)
        names = {}
        for k, v in sorted(p.items()):
            names.setdefault(v[0], k)      # AMS1117 has two VOUT pins (2 and the tab, 4): take the first
        check({"GND", "VIN", "VOUT"} <= set(names) or len(p) == 4, f"U1 pins {sorted(p.items())}")
        check("[LCSC:SOT-223" in u1, "U1's footprint is the LCSC one")

        caps = []
        for _ in range(2):
            err, t = mcp.tool("sch_add_part", {"lcsc": "C14663"})
            m = re.match(r"added (C\d+) ", t)
            caps.append(m.group(1) if m else None)
        check(caps == ["C1", "C2"], f"two LCSC 100 nF caps numbered {caps}")
        err, t = mcp.tool("sch_add_part", {"lcsc": "C25804"})
        check(t.startswith("added R1 "), "LCSC 10k -> " + t.splitlines()[0][:60])
        err, t = mcp.tool("sch_add_part", {"lib_id": "Device:R", "value": "1k", "footprint": "LCSC:R0603",
                                           "ref": "R?", "rotation": 90})
        check(not err and re.match(r"added R2 1k Device:R \[LCSC:R0603\] @[\d.]+,[\d.]+ r90", t),
              "library part Device:R, value, footprint override, rotation: " + t.splitlines()[0][:80])
        err, t = mcp.tool("sch_add_part", {"lib_id": "Device:R", "ref": "R2"})
        check(err and "already exists" in t, "an explicit ref that exists is refused")
        err, t = mcp.tool("sch_add_part", {"lib_id": "Nope:X"})
        check(err and "not found" in t, "unknown symbol refused: " + t[:60])

        err, t = mcp.tool("sch_summary")
        pos = {m.group(1): (float(m.group(2)), float(m.group(3)))
               for m in re.finditer(r"^(\S+) .*? @([\d.]+),([\d.]+)", t, re.M)}
        check(len(pos) == 5 and len(set(pos.values())) == 5, f"5 parts at 5 different places {pos}")

        print("[connect]")
        vin, vout, gnd = names.get("VIN", "3"), names.get("VOUT", "2"), names.get("GND", "1")
        err, t = mcp.tool("sch_connect", {"nets": {
            "VIN": [f"U1.{vin}", "C1.1"],
            "+3V3": [f"U1.{vout}", "C2.1", "R1.1"],
            "GND": [f"U1.{gnd}", "C1.2", "C2.2", "R2.2"],
            "LED_K": ["R1.2", "R2.1"]}})
        check(not err and t.startswith("11 pin(s) connected"), "11 pins connected: " + t[:80])
        nets, unconnected = netlist(mcp)
        want = {"VIN": {f"U1.{vin}", "C1.1"}, "+3V3": {f"U1.{vout}", "C2.1", "R1.1"},
                "GND": {f"U1.{gnd}", "C1.2", "C2.2", "R2.2"}, "LED_K": {"R1.2", "R2.1"}}
        for n, pins in want.items():
            have = nets.get(n, set())
            # AMS1117's tab is a second VOUT pin: it may join +3V3 on its own number
            check(pins <= have and len(have - pins) <= 1, f"net {n} = {sorted(have)}")
        tab = [k for k, v in p.items() if k not in (vin, vout, gnd)]
        check(set(unconnected) == {f"U1.{k}" for k in tab}, f"only the tab is left unconnected: {sorted(unconnected)}")

        err, t = mcp.tool("sch_connect", {"nets": {"GND": ["R1.1"]}})
        check(err and "already on net" in t and "+3V3" in t, "pin on another net refused: " + t.splitlines()[-1][:80])
        err, t = mcp.tool("sch_connect", {"nets": {"+3V3": ["R1.1"]}})
        check(not err and t.startswith("1 pin"), "same net again is a no-op")
        if tab:
            err, t = mcp.tool("sch_connect", {"nets": {"NC": [f"U1.{tab[0]}"]}})
            check(not err, f"U1.{tab[0]} marked NC")
            nets, unconnected = netlist(mcp)
            check(not unconnected, "nothing unconnected after the NC flag")
            err, t = mcp.tool("sch_summary")
            check(" 0 unconnected pins" in t, "summary: " + t.splitlines()[0][-50:])

        err, t = mcp.tool("sch_disconnect", {"pins": ["R2.1"]})
        nets, unconnected = netlist(mcp)
        check(not err and "R2.1" in unconnected and nets.get("LED_K") == {"R1.2"}, "disconnect R2.1")
        err, t = mcp.tool("sch_connect", {"nets": {"LED_K": ["R2.1"]}})
        nets, unconnected = netlist(mcp)
        check(nets.get("LED_K") == {"R1.2", "R2.1"}, "reconnected")

        err, t = mcp.tool("sch_delete", {"refs": ["R2"]})
        nets, unconnected = netlist(mcp)
        check(not err and "R2.2" not in nets.get("GND", set()) and nets.get("LED_K") == {"R1.2"},
              "deleting R2 takes its stubs and labels with it")
        err, t = mcp.tool("sch_add_part", {"lib_id": "Device:R", "value": "1k", "footprint": "LCSC:R0603"})
        check(t.startswith("added R2 "), "R2 is free again")
        mcp.tool("sch_connect", {"nets": {"LED_K": ["R2.1"], "GND": ["R2.2"]}})
        before, _ = netlist(mcp)

        print("[save, restart]")
        err, t = mcp.tool("sch_save")
        check(not err and "blank.kicad_sch" in t, "sch_save: " + t[-40:])
        mcp.close()
        mcp = None
        server.stop()
        check(server.start(), "api-server restarted on the saved project")
        mcp = Mcp(mcp_cmd, env)
        after, _ = netlist(mcp)
        check(after == before, f"same {len(after)} nets after reload")
        text = open(os.path.join(proj, "blank.kicad_sch"), encoding="utf-8").read()
        check(text.count('(lib_id "LCSC:') == 4 and text.count('(lib_id "Device:R")') == 1,
              "saved file: 4 LCSC symbols, 1 Device:R")

        print("[M5b: sync_pcb]")
        err, t = mcp.tool("sync_pcb", {"dry_run": True})
        check(not err and t.startswith("dry run: 5 footprint(s) added"), "dry run: " + t.splitlines()[0][:70])
        err, t = mcp.tool("board_summary")
        check("footprints=0" in t, "dry run changed nothing")
        err, t = mcp.tool("sync_pcb")
        check(not err and re.match(r"5 footprint\(s\) added: (\S+ ?){5}; board now 5 footprints", t),
              "sync_pcb: " + t.splitlines()[0][:100])
        for net, pins in after.items():
            pins = {p for p in pins}
            if len(pins) < 2:
                continue
            err, q = mcp.tool("query_net", {"net": "/" + net if not net.startswith("/") else net})
            have = {m.group(1) for m in re.finditer(r"^PAD (\S+) ", q, re.M)}
            check(have == pins, f"board net {net}: {sorted(have)}")
        err, t = mcp.tool("sync_pcb")
        check(not err and t.startswith("0 footprint(s) added"), "a second sync adds nothing")

        print("[M5c: auto_place]")
        err, t = mcp.tool("auto_place")
        m = re.search(r"placed (\d+) part\(s\).*?wire length \(half-perimeter\) ([\d.]+) -> ([\d.]+) mm", t)
        check(not err and m and m.group(1) == "5" and float(m.group(3)) < float(m.group(2)),
              "auto_place: " + t.splitlines()[0][:120])
        err, t = mcp.tool("run_drc", {"include_warnings": False})
        kinds = re.search(r"errors: (.*)", t).group(1) if "errors:" in t else t
        check(set(re.findall(r"(\w+) \d+", kinds)) <= {"unconnected_items"},
              "DRC after placement: only unrouted connections (" + kinds[:80] + ")")
        err, t = mcp.tool("board_summary")
        outline = re.search(r"outline=([\d.]+),([\d.]+)\.\.([\d.]+),([\d.]+)mm", t)
        x0, y0, x1, y1 = (float(v) for v in outline.groups())
        err, q = mcp.tool("query_area", {"x": (x0 + x1) / 2, "y": (y0 + y1) / 2, "radius": 60, "kinds": ["footprint"]})
        fps = {mm.group(1): (float(mm.group(2)), float(mm.group(3)))
               for mm in re.finditer(r"^FP (\S+) \S+ @(-?[\d.]+),(-?[\d.]+)", q, re.M)}
        check(len(fps) == 5 and all(x0 < x < x1 and y0 < y < y1 for x, y in fps.values()),
              f"all 5 inside the outline {sorted(fps)}")
        u1 = fps.get("U1")
        near = [c for c in ("C1", "C2") if u1 and c in fps and math.dist(u1, fps[c]) < 8]
        check(near == ["C1", "C2"], f"the regulator's caps within 8 mm of U1: {near}")
        err, t = mcp.tool("undo")
        err, q = mcp.tool("query_area", {"x": (x0 + x1) / 2, "y": (y0 + y1) / 2, "radius": 60, "kinds": ["footprint"]})
        check(not err and "FP U1" not in q, "undo takes the placement back (parts outside again)")
        mcp.tool("redo")
        err, t = mcp.tool("save_board")
        check(not err, "board saved")
    finally:
        if mcp:
            mcp.close()
        server.stop()

    print("\nRESULT:", "PASS" if not FAILS else f"FAIL ({len(FAILS)})")
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())

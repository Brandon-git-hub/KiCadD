"""M4a/M4b end to end: LCSC part number -> KiCad footprint and symbol, checked against the raw
EasyEDA data.

usage: python3 -I lcsc_e2e.py --mcp KICADD_MCP --socket SOCK --cache DIR --project DIR [PART...]

The server (kicad-cli api-server) must have a board from DIR loaded and KICADD_LCSC_CACHE=DIR
(--cache) in its environment. For every part:
  * lcsc_fetch converts it and writes DIR/lcsc.pretty/<package>.kicad_mod
  * every pad in that file is compared with the PAD / HOLE records of the cached EasyEDA
    answer, parsed here on its own (position, size, drill, SMD/THT): the importer is not
    trusted to check itself
  * a courtyard and the attribution are there, no STEP model is referenced
  * M4b: the symbol is in DIR/lcsc.kicad_sym; its pin numbers are the P records of the EasyEDA
    symbol (subparts for multi-unit parts) and the footprint's numbered pads, its Footprint field
    is the footprint's LCSC:<package>, it carries the LCSC number and the attribution
  * add_footprint lcsc=... places it on the board, then undo takes it off again
Then: the LCSC row is in DIR/fp-lib-table and DIR/sym-lib-table once, a second fetch comes from the cache, an
unknown part and a malformed number are refused.
"""

import argparse
import json
import math
import os
import re
import subprocess
import sys

PARTS = ["C8734", "C25804", "C14663", "C49678", "C20917", "C5446", "C6186", "C7171", "C81598",
         "C2040", "C165948", "C2765186", "C2337"]
UNIT = 0.254          # EasyEDA Standard: 1 unit = 10 mil
TOL = 0.002           # mm

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
                                "clientInfo": {"name": "lcsc-e2e", "version": "1"}})
        self.p.stdin.write(json.dumps({"jsonrpc": "2.0", "method": "notifications/initialized"}) + "\n")
        self.p.stdin.flush()

    def rpc(self, method, params):
        self.rid += 1
        self.p.stdin.write(json.dumps({"jsonrpc": "2.0", "id": self.rid, "method": method, "params": params}) + "\n")
        self.p.stdin.flush()
        return json.loads(self.p.stdout.readline())

    def tool(self, name, args):
        r = self.rpc("tools/call", {"name": name, "arguments": args}).get("result", {})
        return bool(r.get("isError")), "".join(c.get("text", "") for c in r.get("content", []))

    def close(self):
        self.p.stdin.close()
        self.p.wait(10)


def easyeda_pads(cache, part):
    """Pads and holes of the cached EasyEDA answer, in mm relative to the footprint origin."""
    ds = json.load(open(os.path.join(cache, part + ".json"), encoding="utf-8"))["result"]["packageDetail"]["dataStr"]
    ox, oy = float(ds["head"]["x"]), float(ds["head"]["y"])
    pads = []
    for s in ds["shape"]:
        f = s.split("~")
        if f[0] == "PAD":
            x, y, w, h = (float(v) for v in f[2:6])
            rot = float(f[11] or 0)
            if f[1] == "POLYGON":           # size from the outline points
                pts = [float(v) for v in f[10].split()]
                xs, ys = pts[0::2], pts[1::2]
                w, h, rot = max(xs) - min(xs), max(ys) - min(ys), 0
            if round(rot) % 180 == 90:
                w, h = h, w
            drill = 2 * float(f[9] or 0)
            slot = float(f[13] or 0)
            pads.append({"num": f[8], "x": (x - ox) * UNIT, "y": (y - oy) * UNIT, "w": w * UNIT, "h": h * UNIT,
                         "tht": f[6] == "11", "drill": drill * UNIT, "slot": slot * UNIT,
                         "poly": f[1] == "POLYGON"})
        elif f[0] == "HOLE":
            x, y, r = float(f[1]), float(f[2]), float(f[3])
            pads.append({"num": "", "x": (x - ox) * UNIT, "y": (y - oy) * UNIT, "w": 2 * r * UNIT, "h": 2 * r * UNIT,
                         "tht": True, "drill": 2 * r * UNIT, "slot": 0, "poly": False})
    return pads


def easyeda_pins(cache, part):
    """Pin numbers of the cached EasyEDA symbol (all units)."""
    res = json.load(open(os.path.join(cache, part + ".json"), encoding="utf-8"))["result"]
    docs = [sp["dataStr"] for sp in res.get("subparts") or []] or [res["dataStr"]]
    pins = set()
    for d in docs:
        d = json.loads(d) if isinstance(d, str) else d
        for sh in d["shape"]:
            f = sh.split("~")
            if f[0] == "P":
                pins.add(f[3])
    return pins


def kicad_symbol(path, name):
    """(properties, pin numbers) of one symbol in a .kicad_sym."""
    text = open(path, encoding="utf-8").read()
    blocks = re.split(r'\n\t\(symbol ', text)
    for b in blocks[1:]:
        if b.startswith(json.dumps(name)):
            props = dict(re.findall(r'\(property "([^"]*)" "((?:[^"\\]|\\.)*)"', b))
            pins = set(re.findall(r'\(number "([^"]*)"', b))
            return props, pins
    return None, None


def check_symbol(part, cache, text, pads_file):
    m = re.search(r"symbol (LCSC:.+?) \((.+?\.kicad_sym)\)", text)
    check(m is not None, f"{part}: symbol saved")
    if not m:
        return
    props, pins = kicad_symbol(m.group(2), m.group(1).split(":", 1)[1])
    check(props is not None, f"{part}: {m.group(1)} is in {os.path.basename(m.group(2))}")
    if props is None:
        return
    want = easyeda_pins(cache, part)
    check(pins == want, f"{part}: symbol pins = EasyEDA pins ({len(pins)})"
          + ("" if pins == want else f" -- only KiCad {sorted(pins - want)[:8]}, only EasyEDA {sorted(want - pins)[:8]}"))
    pads = {p["num"] for p in kicad_pads(pads_file)[0] if p["num"]}
    check(pins == pads, f"{part}: symbol pins = footprint pads"
          + ("" if pins == pads else f" -- pins only {sorted(pins - pads)[:8]}, pads only {sorted(pads - pins)[:8]}"))
    fp = re.search(r"saved as (LCSC:\S+)", text).group(1)
    check(props.get("Footprint") == fp, f"{part}: Footprint field {props.get('Footprint')} = {fp}")
    check(props.get("LCSC") == part, f"{part}: LCSC field")
    check("JLCEDA/EasyEDA Official Library" in props.get("Description", "")
          and "JLCEDA/EasyEDA Official Library" in props.get("Source", ""), f"{part}: symbol names its source")
    check(props.get("Reference", "") and not re.search(r"[0-9?]", props["Reference"]),
          f"{part}: reference prefix {props.get('Reference')!r}")
    check("WARNING" not in text, f"{part}: no pin/pad mismatch reported")


def kicad_pads(path):
    """Pads of a .kicad_mod: number, kind, position, size, drill, rotation (custom pads: size of
    the primitives' bounding box)."""
    text = open(path, encoding="utf-8").read()
    pads = []
    for m in re.finditer(r'\(pad "([^"]*)" (\S+) (\S+)\s*\(at ([-\d.]+) ([-\d.]+)(?: ([-\d.]+))?\)\s*\(size ([-\d.]+) ([-\d.]+)\)(.*?)\n\t\t\)',
                         text, re.S):
        num, kind, shape, x, y, rot, w, h, rest = m.groups()
        x, y, w, h, rot = float(x), float(y), float(w), float(h), float(rot or 0)
        if round(rot) % 180 == 90:
            w, h = h, w
        drill = 0.0
        if (d := re.search(r'\(drill (?:oval )?([-\d.]+)(?: ([-\d.]+))?', rest)):
            drill = min(float(d.group(1)), float(d.group(2) or d.group(1)))
        if shape == "custom":
            pts = [tuple(map(float, p)) for p in re.findall(r'\(xy ([-\d.]+) ([-\d.]+)\)', rest)]
            if pts:
                xs, ys = [p[0] for p in pts], [p[1] for p in pts]
                w, h = max(max(xs) - min(xs), w), max(max(ys) - min(ys), h)
        pads.append({"num": num, "kind": kind, "x": x, "y": y, "w": w, "h": h, "drill": drill,
                     "tht": kind in ("thru_hole", "np_thru_hole")})
    return pads, text


def compare(part, cache, path):
    want = easyeda_pads(cache, part)
    have, text = kicad_pads(path)
    check(len(have) == len(want), f"{part}: {len(have)} pads in KiCad, {len(want)} pads/holes in EasyEDA")
    unmatched = []
    for e in want:
        best = None
        for k in have:
            if k["num"] == e["num"] and abs(k["x"] - e["x"]) < TOL and abs(k["y"] - e["y"]) < TOL:
                best = k
                break
        if not best:
            unmatched.append(f"{e['num'] or 'hole'}@{e['x']:.3f},{e['y']:.3f}")
            continue
        bad = []
        if not e["poly"] and (abs(best["w"] - e["w"]) > TOL or abs(best["h"] - e["h"]) > TOL):
            bad.append(f"size {best['w']:.3f}x{best['h']:.3f} != {e['w']:.3f}x{e['h']:.3f}")
        if e["poly"] and (abs(best["w"] - e["w"]) > 0.02 or abs(best["h"] - e["h"]) > 0.02):
            bad.append(f"polygon extent {best['w']:.3f}x{best['h']:.3f} != {e['w']:.3f}x{e['h']:.3f}")
        if best["tht"] != e["tht"]:
            bad.append(f"tht {best['tht']} != {e['tht']}")
        if e["tht"] and abs(best["drill"] - e["drill"]) > TOL:
            bad.append(f"drill {best['drill']:.3f} != {e['drill']:.3f}")
        if bad:
            unmatched.append(f"{e['num']}: " + ", ".join(bad))
    check(not unmatched, f"{part}: every pad matches position/size/drill/type"
          + ("" if not unmatched else " -- " + "; ".join(unmatched[:6])))
    check("F.CrtYd" in text, f"{part}: has a courtyard")
    check("JLCEDA/EasyEDA Official Library" in text, f"{part}: names its source")
    check(".step" not in text.lower(), f"{part}: no STEP model referenced")
    check("User." not in text, f"{part}: nothing left on User layers")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--mcp", required=True)
    ap.add_argument("--socket", required=True)
    ap.add_argument("--cache", required=True)
    ap.add_argument("--project", required=True)
    ap.add_argument("parts", nargs="*")
    a = ap.parse_args()
    parts = a.parts or PARTS

    m = Mcp([a.mcp, "--socket", a.socket, "--stdio"], dict(os.environ))
    try:
        for i, part in enumerate(parts):
            print(f"[{part}]")
            err, text = m.tool("lcsc_fetch", {"lcsc": part})
            check(not err, f"{part}: lcsc_fetch: " + text.splitlines()[0][:110])
            if err:
                continue
            saved = re.search(r"saved as (LCSC:\S+) \((.+?\.kicad_mod)\)", text)
            check(saved is not None, f"{part}: saved to the project library")
            if not saved:
                continue
            compare(part, a.cache, saved.group(2))
            check_symbol(part, a.cache, text, saved.group(2))

            err, text = m.tool("add_footprint", {"lcsc": part, "ref": "X?", "x": 20 + 25 * (i % 6),
                                                  "y": -60 - 25 * (i // 6)})
            check(not err and "added" in text.lower(), f"{part}: add_footprint placed it: "
                  + " ".join(text.split())[:120])
            err, text = m.tool("undo", {})
            check(not err and "undid" in text, f"{part}: undo took it off")

        print("[library table, cache, errors]")
        table = open(os.path.join(a.project, "fp-lib-table"), encoding="utf-8").read()
        check(table.count('(name "LCSC")') == 1, "LCSC row in the project fp-lib-table exactly once")
        stable = open(os.path.join(a.project, "sym-lib-table"), encoding="utf-8").read()
        check(stable.count('(name "LCSC")') == 1, "LCSC row in the project sym-lib-table exactly once")
        err, text = m.tool("lcsc_fetch", {"lcsc": parts[0].lower()})
        check(not err and "from the cache" in text, "second fetch (lower case) comes from the cache")
        err, text = m.tool("lcsc_fetch", {"lcsc": "C999999999"})
        offline = os.environ.get("KICADD_LCSC_OFFLINE")   # the server's environment, as this test is run
        check(err and ("not found" in text or "rate-limits" in text or (offline and "not in the cache" in text)),
              "unknown part refused: " + text.strip()[:90])
        err, text = m.tool("lcsc_fetch", {"lcsc": "STM32"})
        check(err and "not an LCSC part number" in text, "malformed number refused")
        err, text = m.tool("add_footprint", {"lcsc": "C25804", "ref": "R?", "x": 0, "y": -50,
                                              "nets": {"1": "LCSC_TEST", "2": "GND"}})
        check(not err and "LCSC_TEST" in text, "add_footprint lcsc with nets: " + " ".join(text.split())[:100])
        m.tool("undo", {})
    finally:
        m.close()

    print("\nRESULT:", "PASS" if not FAILS else f"FAIL ({len(FAILS)})")
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())

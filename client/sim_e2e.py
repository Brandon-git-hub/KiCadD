"""Regional simulation with ngspice (headless): simulate. Measures only; the numbers are checked
against hand arithmetic.

usage: python3 -I sim_e2e.py --build BUILD --kicad KICAD_SRC --work DIR --index PARTS_DB[.gz] --ngspice NGSPICE

The parts index must have the params tables (2026-11-21 on).

Blank project (m5_e2e's), three circuits:
  LDO     U1 (a box symbol, LCSC C6186 AMS1117-3.3) VIN -> +3V3, C1 10 µF 0402 X5R 6.3 V (C15525) and
          C2 100 nF 0402 X7R (C1525) on +3V3, C3 10 µF 0603 X5R (C19702) on VIN; U2 a load IC on +3V3.
          around [U1]: U1 and its capacitors, not U2 (a supply is passed to two-pin parts only);
          VIN has no voltage in its name: refused until sources gives it; C1's C_eff at 3.3 V as
          the bias model has it (layer from C per volume, 1/(1+(E/2.5 V/µm)^1.5)), said estimated;
          Vout 3.3 V under a load, the input current the load plus Iq; a load step's dip; dc_bias
          false, models {esr}, corners
  RC      R2 1 k, C4 100 nF C0G (the value says so) to OUT, OUT also on U2: ac -3 dB at 1/(2πRC),
          tran rise 10-90% 2.2 RC; OUT is cut at the region's edge (U2) and said
  own     R3 with Sim.Device R / Sim.Params r=2k (KiCad's model) over R4 1 k: 3 V -> 1 V
  op-amp  U3 (C7950 LM358, no GBW in the data: 1 MHz assumed and said) as a follower on +5V (the
          voltage from its name, said): -3 dB near the GBW
  bad     no region, an unknown net, a bad source, models for a part not there
KiCad's demos/simulation/rectifier: its own models (Sim.* fields, a .include'd 1N4148, VSIN).
"""

import argparse
import gzip
import math
import os
import re
import shutil
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from m5_e2e import FAILS, Mcp, Server, binaries, check, make_project  # noqa: E402


def num(t, pat):
    m = re.search(pat, t)
    return float(m.group(1)) if m else None


def si(text):
    m = re.match(r"([-\d.e+]+) ?([GMkmµnpf]?)", text)
    return float(m.group(1)) * {"G": 1e9, "M": 1e6, "k": 1e3, "": 1, "m": 1e-3, "µ": 1e-6, "n": 1e-9, "p": 1e-12, "f": 1e-15}[m.group(2)]


def main():
    # the answers have µ, ≈, Ω: a Windows console is cp1252, and -I ignores PYTHONUTF8
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", required=True)
    ap.add_argument("--kicad", required=True)
    ap.add_argument("--work", required=True)
    ap.add_argument("--index", required=True)
    ap.add_argument("--ngspice", required=True)
    a = ap.parse_args()
    work = os.path.abspath(a.work)
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(work)
    proj = make_project(a.kicad, work)
    if a.index.endswith(".gz"):   # the packed index (run_all's --index-gz): unpacked here
        with gzip.open(a.index) as src, open(os.path.join(work, "parts.db"), "wb") as dst:
            shutil.copyfileobj(src, dst)
        a.index = os.path.join(work, "parts.db")
    sock = os.path.join(work, "s.sock")
    # a build tree or an installed folder (Windows CI: dist/bin), as the other tests
    _, kicadd_mcp, env = binaries(os.path.abspath(a.build))
    env = dict(env, KICADD_LCSC_OFFLINE="1", KICADD_PARTS_DB=os.path.abspath(a.index), KICADD_NGSPICE=os.path.abspath(a.ngspice))
    server = Server(a.build, sock, os.path.join(proj, "blank.kicad_pcb"), env, os.path.join(work, "server.log"))
    check(server.start(), "api-server up")
    mcp = Mcp([kicadd_mcp, "--socket", sock, "--stdio"], env)
    try:
        def part(lib, value, x, y, fp="", lcsc=""):
            err, t = mcp.tool("sch_add_part", {"lib_id": lib, "value": value, "x": x, "y": y, **({"footprint": fp} if fp else {})})
            check(not err, t.splitlines()[0][:80])
            ref = re.match(r"added (\S+) ", t).group(1)
            if lcsc:
                mcp.tool("sch_set_fields", {"parts": {ref: {"LCSC": lcsc}}})
            return ref

        def sim(args):
            err, t = mcp.tool("simulate", args)
            return err, t

        print("[build the circuits]")
        mcp.tool("lib_create_symbol", {"name": "LDO3", "value": "AMS1117-3.3", "pins": [
            {"number": "1", "name": "GND", "type": "power_in"}, {"number": "2", "name": "VOUT", "type": "power_out"},
            {"number": "3", "name": "VIN", "type": "power_in"}]})
        mcp.tool("lib_create_symbol", {"name": "LOADIC", "value": "LOADIC", "pins": [
            {"number": "1", "name": "VDD", "type": "power_in"}, {"number": "2", "name": "GND", "type": "power_in"},
            {"number": "3", "name": "IN", "type": "input"}]})
        mcp.tool("lib_create_symbol", {"name": "OPA1", "value": "LM358", "pins": [
            {"number": "1", "name": "OUT", "type": "output"}, {"number": "2", "name": "-IN", "type": "input"},
            {"number": "3", "name": "+IN", "type": "input"}, {"number": "4", "name": "V-", "type": "power_in"},
            {"number": "8", "name": "V+", "type": "power_in"}]})
        u1 = part("kicadd:LDO3", "AMS1117-3.3", 50.8, 50.8, lcsc="C6186")
        c1 = part("Device:C", "10uF", 76.2, 50.8, "Capacitor_SMD:C_0402_1005Metric", "C15525")
        c2 = part("Device:C", "100nF", 88.9, 50.8, "Capacitor_SMD:C_0402_1005Metric", "C1525")
        c3 = part("Device:C", "10uF", 25.4, 50.8, "Capacitor_SMD:C_0603_1608Metric", "C19702")
        u2 = part("kicadd:LOADIC", "LOADIC", 127, 50.8)
        r2 = part("Device:R", "1k", 50.8, 101.6)
        c4 = part("Device:C", "100nF C0G", 76.2, 101.6, "Capacitor_SMD:C_0603_1608Metric")
        r3 = part("Device:R", "2k", 50.8, 140)
        r4 = part("Device:R", "1k", 76.2, 140)
        mcp.tool("sch_set_fields", {"parts": {r3: {"Sim.Device": "R", "Sim.Params": "r=2k"}}})
        u3 = part("kicadd:OPA1", "LM358", 127, 127, lcsc="C7950")
        r5 = part("Device:R", "1k", 101.6, 76.2)
        d2 = part("Device:LED", "RED", 127, 76.2)
        err, t = mcp.tool("sch_connect", {"nets": {
            "VIN": [f"{u1}.3", f"{c3}.1"], "+3V3": [f"{u1}.2", f"{c1}.1", f"{c2}.1", f"{u2}.1", f"{r5}.1"],
            "GND": [f"{d2}.1", f"{u1}.1", f"{c1}.2", f"{c2}.2", f"{c3}.2", f"{u2}.2", f"{c4}.2", f"{r4}.2", f"{u3}.4"],
            "SIG": [f"{r2}.1"], "OUT": [f"{r2}.2", f"{c4}.1", f"{u2}.3"],
            "SIG2": [f"{r3}.1"], "MID": [f"{r3}.2", f"{r4}.1"],
            "+5V": [f"{u3}.8"], "AIN": [f"{u3}.3"], "AOUT": [f"{u3}.1", f"{u3}.2"],
            "LEDA": [f"{r5}.2", f"{d2}.2"]},
            "power": ["GND", "+3V3", "+5V"]})
        check(not err, "wired: " + t[:60])

        print("[LDO]")
        err, t = sim({"around": [u1]})
        check(err and "VIN" in t and "sources" in t, "VIN has no voltage in its name: refused, said: " + t[:100])
        err, t = sim({"around": [u1], "sources": {"VIN": 5}, "loads": {"+3V3": 0.1}})
        print("      ", t.replace("\n", "\n       ")[:1800])
        check(not err and "5 of 5 part(s) modelled" in t, "around U1: U1, C1-C3 and the LED's resistor, not U2 (a supply only to two-pin parts)")
        check(f"cut at the region's edge (open there): LEDA ({r5} here; {d2} outside)" in t and "exclude: true" in t,
              f"{r5}'s far end open at LEDA ({d2} not taken in): said, with what to do")
        # C1: 10 µF 0402 X5R at 3.3 V
        layer = math.sqrt(8.854e-12 * 3000 * (1.0 * 0.5 * 0.5e-9 * 0.5) / 10e-6)
        share = 1 / (1 + (3.3 / layer / 2.5e6) ** 1.5)
        m = re.search(re.escape(c1) + r" 10uF \(C15525\): 10 µF MLCC X5R 0402 6\.3 V @ ([\d.]+) V: C_eff ≈ ([\d.]+) µF \(est\. DC bias", t)
        check(m and abs(float(m.group(1)) - 3.3) < 0.02 and abs(float(m.group(2)) - 10 * share) < 0.02,
              f"{c1} C_eff {m and m.group(2)} µF at {m and m.group(1)} V, the bias model {10 * share:.3g} µF: said estimated")
        check(re.search(re.escape(c2) + r" .*100 nF MLCC X7R 0402 16 V @ [\d.]+ V: C_eff ≈ 9\d\.?\d* nF \(est\. DC bias: 9\d%", t),
              f"{c2} 100 nF X7R at 3.3 V keeps most of it")
        check(re.search(re.escape(c3) + r" .*@ 5 V: C_eff", t), f"{c3} at VIN's 5 V")
        check("ESR ~" in t and "(est.: typical for MLCC 0402)" in t, "ESR / ESL said estimated")
        check(re.search(re.escape(u1) + r" AMS1117-3.3 \(C6186\): LDO, Vout 3.3 V, dropout 1.1 V, Iq 5 mA, PSRR 72 dB \(one number", t),
              "U1 from C6186's data: Vout, dropout, Iq, PSRR")
        check("loop 100 kHz assumed" in t, "the loop bandwidth said assumed")
        v = num(t, r"\n  \+3V3: ([\d.]+) V")
        check(v and abs(v - 3.3) < 0.01, f"+3V3 {v} V")
        i = num(t, r"VIN source: ([\d.]+) mA")
        check(i and abs(i - 105) < 1, f"VIN delivers {i} mA (the load 100 mA + Iq 5 mA)")

        err, t = sim({"around": [u1], "sources": {"VIN": 5}, "loads": {"+3V3": "pulse(0.05 0.5 1m 1u 1u 1m 4m)"},
                      "tran": {"stop": "3m", "step": "1u", "window": "0.5m"}})
        print("      ", "\n       ".join(ln for ln in t.splitlines() if ln.startswith("  +3V3") or "source" in ln or "out:" in ln))
        mn = num(t, r"\+3V3: [\d.]+ V, ripple .*?; min ([\d.]+) V at 1(?:\.0\d*)? ms")
        check(mn and 3.0 < mn < 3.299, f"a 0.05 -> 0.5 A load step at 1 ms: +3V3 dips to {mn} V")
        v = num(t, r"\n  \+3V3: ([\d.]+) V, ripple")
        check(v and abs(v - 3.3) < 0.002, f"back at {v} V in the window (50 mA again; Rout 10 mΩ)")
        check(re.search(re.escape(u1) + r" out: [\d.]+ mA rms, peak 50\d mA \(Iout 1 A\)", t), "U1's output current with its Iout")

        err, t = sim({"around": [u1], "sources": {"VIN": 5}, "dc_bias": False})
        check(re.search(re.escape(c1) + r" .*\(DC bias off: nominal C\)", t), "dc_bias false: nominal C")
        err, t = sim({"around": [u1], "sources": {"VIN": 5}, "models": {c1: {"esr": "20m", "esl": "1n"}}})
        check(re.search(re.escape(c1) + r" .*ESR 20 mΩ, ESL 1 nH;", t), "models: ESR / ESL as given (not estimated)")
        err, t = sim({"around": [u1], "sources": {"VIN": 5}, "models": {u1: {"vout": 3.0}}})
        v = num(t, r"\n  \+3V3: ([\d.]+) V")
        check(v and abs(v - 3.0) < 0.01, f"models {u1} vout 3: +3V3 {v} V")

        print("[RC]")
        err, t = sim({"around": [r2], "sources": {"SIG": "dc 0 ac 1"}, "ac": {"from": 10, "to": "1Meg"}, "probes": ["OUT"]})
        print("      ", "\n       ".join(t.splitlines()[:12]))
        f3 = re.search(r"\n  OUT: .*-3 dB at ([\d.]+ [kM]?)Hz", t)
        want = 1 / (2 * math.pi * 1e3 * 100e-9)
        check(f3 and abs(si(f3.group(1)) - want) / want < 0.02, f"RC -3 dB {f3 and f3.group(1)}Hz, 1/(2πRC) {want:.1f} Hz")
        check(re.search(re.escape(c4) + r" 100nF C0G: 100 nF MLCC C0G", t), "C4 C0G from its value: no DC bias")
        check(f"left out (their pins open): {u2} LOADIC (no model" in t and "+3V3" not in t,
              f"{u2} on OUT is in the region but has no model: left out, said; its supply not made")
        err, t = sim({"around": [r2], "sources": {"SIG": "pulse(0 1 0.1m 1n 1n 10m 20m)"}, "tran": {"stop": "2m", "step": "1u"}, "probes": ["OUT"]})
        rise = re.search(r"rise 10-90% ([\d.]+ [µm]?)s", t)
        check(rise and abs(si(rise.group(1)) - 2.197e-4) / 2.197e-4 < 0.03, f"RC rise {rise and rise.group(1)}s, 2.2 RC 220 µs")
        err, t = sim({"around": [r2], "sources": {"SIG": "dc 1 ac 1"}, "ac": {"from": 10, "to": "1Meg"}, "probes": ["OUT"], "corners": True})
        check("all low, all high: every R / C / L moved together, not a worst case" in t, "corners said to be all-low / all-high")
        check("[corners " in t and "(assumed)" not in t.split(c4 + " ")[1].split("\n")[0] or "±5% (assumed)" in t,
              "corners: spread of the -3 dB point; C4's tolerance assumed (5%, C0G) and said")

        print("[own model]")
        err, t = sim({"refs": [r3, r4], "sources": {"SIG2": 3}})
        print("      ", "\n       ".join(t.splitlines()[-4:]))
        check(re.search(re.escape(r3) + r" 2k: its own SPICE model", t), f"{r3}: KiCad's model (Sim.* fields)")
        v = num(t, r"\n  MID: ([\d.]+) V")
        check(v and abs(v - 1.0) < 1e-3, f"MID {v} V (3 V over 2k / 1k)")

        print("[op-amp]")
        err, t = sim({"refs": [u3], "sources": {"AIN": "dc 2.5 ac 1"}, "ac": {"from": 1000, "to": "100Meg"}, "probes": ["AOUT"]})
        print("      ", "\n       ".join(t.splitlines()[:10]))
        check("+5V dc 5 (from its name)" in t, "+5V's 5 V from its name, said")
        check(re.search(re.escape(u3) + r" LM358 \(C7950\): op-amp, 1 channel\(s\): GBW 1 MHz assumed", t), "GBW not in C7950's data: assumed, said")
        f3 = re.search(r"\n  AOUT: .*-3 dB at ([\d.]+ [kM]?)Hz", t)
        check(f3 and 0.7e6 < si(f3.group(1)) < 1.6e6, f"follower -3 dB {f3 and f3.group(1)}Hz (GBW 1 MHz)")
        err, t = sim({"refs": [u3], "sources": {"AIN": 2.5}})
        v = num(t, r"\n  AOUT: ([\d.]+) V")
        check(v and abs(v - 2.5 - 0.007) < 1e-3, f"follower DC {v} V (2.5 V + Vos 7 mV from the data)")

        print("[bad]")
        err, t = sim({})
        check(err and "one of refs, sheet or around" in t, "no region: refused")
        err, t = sim({"refs": [r2], "sources": {"NOPE": 1}})
        check(err and "no net NOPE" in t, "an unknown net: refused")
        err, t = sim({"refs": [r2], "sources": {"SIG": "1\n.end"}})
        check(err and "SPICE source value" in t, "a source with a new line: refused")
        err, t = sim({"refs": [r2], "models": {u1: {"r": 1}}})
        check(err and "not in the region" in t, "models for a part not in the region: refused")
        err, t = sim({"refs": [r2], "sources": {"SIG": 1}, "ac": {}})
        check(err and "ac 1" in t, "ac with no AC source: refused")
    finally:
        mcp.close()
        server.stop()

    # ---- KiCad's simulation demo: its own models (Sim.* fields, a .include'd diode model, VSIN)
    print("[KiCad demo: rectifier]")
    rect = os.path.join(work, "rect")
    shutil.copytree(os.path.join(a.kicad, "demos", "simulation", "rectifier"), rect)
    shutil.copy(os.path.join(proj, "blank.kicad_pcb"), os.path.join(rect, "rectifier.kicad_pcb"))
    sock2 = os.path.join(work, "r.sock")
    server = Server(a.build, sock2, os.path.join(rect, "rectifier.kicad_pcb"), env, os.path.join(work, "server-rect.log"))
    check(server.start(), "api-server up on the rectifier demo")
    mcp = Mcp([kicadd_mcp, "--socket", sock2, "--stdio"], env)
    try:
        err, t = mcp.tool("simulate", {"sheet": "/", "tran": {"stop": "50m", "step": "10u"}, "netlist": True})
        print("      ", "\n       ".join(ln for ln in t.splitlines() if ln.startswith(("simulate", "  D1", "  V1", "  rect_out", ".include"))))
        check(not err and "5 of 5 part(s) modelled" in t, "all five parts modelled")
        check("D1 1N4148: its own SPICE model" in t and re.search(r'\.include ".*diode\.mod"', t), "D1: KiCad's model, its library included")
        check(re.search(r"\nV1 \S+ 0 DC 0 SIN\( 0 1 1k", t), "V1: the demo's VSIN as KiCad writes it, on our node names")
        v = num(t, r"\n  rect_out: ([\d.]+) mV, ripple ([\d.]+) mV")
        check(v and 300 < v < 450, f"rect_out {v} mV (1 V peak through a 1N4148 into 100 nF || 100 k)")
        check("not within 2% of the step by the end" in t, "the ripple keeps it outside 2%: said, not a settling time")
    finally:
        mcp.close()
        server.stop()

    print("\nRESULT:", "PASS" if not FAILS else f"FAIL ({len(FAILS)})")
    sys.exit(1 if FAILS else 0)


if __name__ == "__main__":
    main()

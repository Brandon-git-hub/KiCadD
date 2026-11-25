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
        # a dual in one unit with the names twice (as some EasyEDA symbols), numbered so the nearest
        # pin numbers pair it wrong (OUT 1 next to 2 3, channel B's): each channel drawn together
        L, R = "left", "right"
        mcp.tool("lib_create_symbol", {"name": "OPA2", "value": "GS8552", "pins": [
            {"number": "6", "name": "NONINVERTING_INPUT", "type": "input", "side": L},
            {"number": "7", "name": "INVERTING_INPUT", "type": "input", "side": L},
            {"number": "9", "name": "NC", "type": "no_connect", "side": L}, {"number": "10", "name": "NC", "type": "no_connect", "side": L},
            {"number": "2", "name": "NONINVERTING_INPUT", "type": "input", "side": L},
            {"number": "3", "name": "INVERTING_INPUT", "type": "input", "side": L},
            {"number": "1", "name": "OUT", "type": "output", "side": R}, {"number": "4", "name": "+V", "type": "power_in", "side": R},
            {"number": "5", "name": "-V", "type": "power_in", "side": R}, {"number": "11", "name": "NC", "type": "no_connect", "side": R},
            {"number": "8", "name": "OUT", "type": "output", "side": R}]})
        u3 = part("kicadd:OPA1", "LM358", 127, 127, lcsc="C7950")
        u4 = part("kicadd:OPA2", "GS8552", 127, 165, lcsc="C157716")
        r5 = part("Device:R", "1k", 101.6, 76.2)
        d2 = part("Device:LED", "RED", 127, 76.2)
        err, t = mcp.tool("sch_connect", {"nets": {
            "VIN": [f"{u1}.3", f"{c3}.1"], "+3V3": [f"{u1}.2", f"{c1}.1", f"{c2}.1", f"{u2}.1", f"{r5}.1"],
            "GND": [f"{d2}.1", f"{u1}.1", f"{c1}.2", f"{c2}.2", f"{c3}.2", f"{u2}.2", f"{c4}.2", f"{r4}.2", f"{u3}.4", f"{u4}.5"],
            "SIG": [f"{r2}.1"], "OUT": [f"{r2}.2", f"{c4}.1", f"{u2}.3"],
            "SIG2": [f"{r3}.1"], "MID": [f"{r3}.2", f"{r4}.1"],
            "+5V": [f"{u3}.8", f"{u4}.4"], "AIN": [f"{u3}.3"], "AOUT": [f"{u3}.1", f"{u3}.2"],
            "LEDA": [f"{r5}.2", f"{d2}.2"],
            "BIN1": [f"{u4}.6"], "BOUT1": [f"{u4}.1", f"{u4}.7"], "BIN2": [f"{u4}.2"], "BOUT2": [f"{u4}.8", f"{u4}.3"]},
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
        check(re.search(re.escape(u1) + r" AMS1117-3.3 \(C6186\): LDO, Vout 3.3 V, dropout 1.1 V, Iq 5 mA, PSRR 72 dB \(the data's one number\) to 10 kHz assumed, then -20 dB/dec to 20 dB assumed", t),
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

        print("[LDO start-up, current limit, sources null, unused keys]")
        # VIN from 0 V: no wind-up at the DC point (the output used to sit at VIN - dropout)
        err, t = sim({"around": [u1], "sources": {"VIN": "pulse(0 5 0.2m 0.5m 0.5m 10 20)"}, "loads": {"+3V3": 0.1},
                      "tran": {"stop": "3m", "step": "1u", "window": "0.5m"}})
        v = num(t, r"\n  \+3V3: ([\d.]+) V, ripple")
        mx = num(t, r"\n  \+3V3: .*?max ([\d.]+) V")
        check(not err and v and abs(v - 3.3) < 0.01 and mx and mx < 3.32, f"VIN ramped from 0 V: +3V3 {v} V (max {mx} V), not VIN - dropout")
        check(re.search(re.escape(u1) + r" .*current limit 1\.5 A \(1\.5 x Iout, assumed\)", t), "current limit from C6186's Iout 1 A, said assumed")
        err, t = sim({"around": [u1], "sources": {"VIN": "pulse(0 5 0.2m 1u 1u 10 20)"}, "loads": {"+3V3": 0.1},
                      "tran": {"stop": "1m", "step": "1u"}, "models": {u1: {"ilim": 0.3}}})
        pk = num(t, re.escape(u1) + r" out: [\d.]+ mA rms, peak ([\d.]+) mA")
        check(not err and pk and pk <= 301, f"a 1 µs VIN step into the output caps: {u1} peak {pk} mA with ilim 300 mA")
        check(re.search(r"at its current limit 300 mA for [\d.]+ [µm]?s \(the circuit would draw more", t), "the limit reached: said, with for how long")
        err, t = sim({"around": [u1], "sources": {"VIN": "pulse(0 5 0.2m 1u 1u 10 20)"}, "loads": {"+3V3": 0.1},
                      "tran": {"stop": "2m", "step": "1u", "window": "0.5m"}, "models": {u1: {"soft_start": "1m"}}})
        pk = num(t, re.escape(u1) + r" out: [\d.]+ mA rms, peak ([\d.]+) mA")
        rise = re.search(r"\+3V3: .*?rise 10-90% ([\d.]+ [µm]?)s", t)
        check(not err and pk and pk < 160 and "soft start 1 ms" in t and "current limit" in t and "at its current limit" not in t,
              f"soft_start 1 ms: peak {pk} mA (100 mA load + C x 3.3 V / 1 ms), not at the limit")
        check(rise and 0.6e-3 < si(rise.group(1)) < 1.0e-3, f"soft_start 1 ms: +3V3 rises 10-90% in {rise and rise.group(1)}s (0.8 ms for a ramp)")
        err, t = sim({"around": [u1], "sources": {"VIN": 5, "+3V3": None}, "models": {u1: {"exclude": True, "vout": 3}, c1: {"vf": 2}}})
        check(not err and "+3V3 none (sources: null)" in t and "from its name" not in t, "sources {+3V3: null}: no source there, not even from its name")
        check(f"models.{u1}: vout unused, {u1} is left out" in t and f"models.{c1}: vf not used by its model (C)" in t,
              "keys some model reads but this part's does not: said")
        # (the parameter table, 2b: a key no model reads, a bad value, a value in extra quotes)
        err, t = sim({"around": [u1], "sources": {"VIN": 5}, "models": {c1: {"foo": 2}}})
        check(err and f"models.{c1}.foo: unknown key" in t and "esr" in t, "a key no model reads: refused, with the keys: " + t[:120])
        err, t = sim({"around": [u1], "sources": {"VIN": 5}, "tran": {"stop": "\"4n\""}})
        check(err and "tran.stop:" in t and "(in extra quotes)" in t, "tran.stop in extra quotes: said so: " + t[:140])
        err, t = sim({"around": [u1], "sources": {"VIN": 5}, "tran": {"stop": "-1m"}})
        check(err and "tran.stop: > 0 s; got -0.001" in t, "tran.stop -1m: out of range: " + t[:100])
        err, t = sim({"around": [u1], "sources": {"VIN": 5}, "tran": "1m", "probes": ["+3V3"]})
        check(not err and "simulate tran 1 ms" in t, "tran \"1m\" (the stop alone) still taken: " + t[:60])

        print("[RC]")
        err, t = sim({"around": [r2], "sources": {"SIG": "dc 0 ac 1"}, "ac": {"from": 10, "to": "1Meg"}, "probes": ["OUT"]})
        print("      ", "\n       ".join(t.splitlines()[:12]))
        f3 = re.search(r"\n  OUT: .*-3 dB at ([\d.]+ [kM]?)Hz", t)
        want = 1 / (2 * math.pi * 1e3 * 100e-9)
        check(f3 and abs(si(f3.group(1)) - want) / want < 0.02, f"RC -3 dB {f3 and f3.group(1)}Hz, 1/(2πRC) {want:.1f} Hz")
        line = re.search(r"\n  OUT: .*? ([▁▂▃▄▅▆▇█]{32}) \(log f\)", t)
        check(line and line.group(1).startswith("█" * 12) and line.group(1).endswith("▁"),
              f"the line by log f: flat over the first decades (1.6 kHz of 10 Hz - 1 MHz), then down: {line and line.group(1)}")
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

        print("[dc sweep, plots]")
        err, t, imgs = mcp.tool_images("simulate", {"refs": [r3, r4], "sources": {"SIG2": 3}, "dc": {"source": "SIG2", "from": 0, "to": 3, "step": 0.1},
                                                     "probes": ["MID"], "plot": True})
        print("      ", "\n       ".join(t.splitlines()[-3:]))
        m = re.search(r"\n  MID: ([-\d.]+) [µm]?V at 0 V -> ([\d.]+) V at 3 V", t)
        check(not err and m and abs(float(m.group(2)) - 1.0) < 1e-3, f"dc sweep SIG2 0 -> 3 V: MID ends at {m and m.group(2)} V (a third)")
        check(re.search(r"\n  SIG2 source: .* -> 1 mA at 3 V", t), "the source's current over the sweep: 1 mA at 3 V (3 kΩ)")
        check(len(imgs) == 1 and imgs[0][:8] == b"\x89PNG\r\n\x1a\n", "plot: a PNG")
        if imgs:
            open(os.path.join(work, "dc.png"), "wb").write(imgs[0])
        err, t, imgs = mcp.tool_images("simulate", {"around": [r2], "sources": {"SIG": "dc 1 ac 1"}, "ac": {"from": 10, "to": "1Meg"},
                                                     "probes": ["OUT"], "corners": True, "plot": True})
        check(not err and len(imgs) == 1, "ac plot (gain, phase, corners faint)")
        if imgs:
            open(os.path.join(work, "ac.png"), "wb").write(imgs[0])
        err, t, imgs = mcp.tool_images("simulate", {"around": [r2], "sources": {"SIG": "pulse(0 1 0.1m 1n 1n 10m 20m)"}, "tran": {"stop": "2m", "step": "1u"},
                                                     "probes": ["OUT", "SIG"], "plot": True})
        check(not err and len(imgs) == 1, "tran plot")
        if imgs:
            open(os.path.join(work, "tran.png"), "wb").write(imgs[0])
        err, t = sim({"refs": [r3, r4], "sources": {"SIG2": 3}, "dc": {"source": "MID", "from": 0, "to": 1}})
        check(err and "not in sources" in t, "dc on a net that is no source: refused")

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

        # powered down: the input's ESD diode to V+ conducts (2.5 V on AIN, +5V at 0 V); the output
        # does not drive 1.2 V from a 0 V supply
        err, t = sim({"refs": [u3], "sources": {"AIN": 2.5, "+5V": 0}})
        i = num(t, r"\n  AIN source: ([\d.]+) mA")
        check(not err and i and i > 1 and "clamped to the rails by ESD diodes" in t, f"powered down: AIN pushes {i} mA into +5V through the input's ESD diode")
        m = re.search(r"\n  AOUT: (-?[\d.]+ ?[mµn]?)V", t)
        v = si(m.group(1)) if m else None
        check(v is not None and abs(v) < 0.6, f"powered down: AOUT {v} V, not 1.2 V")
        err, t = sim({"refs": [u3], "sources": {"AIN": 2.5, "+5V": 0}, "models": {u3: {"input_clamp": 0}}})
        check(not err and "not clamped (input_clamp 0)" in t, "input_clamp 0: no ESD diodes")

        err, t = sim({"refs": [u4], "sources": {"BIN1": 1, "BIN2": 2}})
        print("      ", "\n       ".join(t.splitlines()[-6:]))
        check(not err and re.search(re.escape(u4) + r" GS8552 \(C157716\): op-amp, 2 channel\(s\)", t),
              f"{u4} (INVERTING INPUT / OUT twice in one unit): 2 channels")
        v1, v2 = num(t, r"\n  BOUT1: ([\d.]+) V"), num(t, r"\n  BOUT2: ([\d.]+) V")
        check(v1 and v2 and abs(v1 - 1) < 0.02 and abs(v2 - 2) < 0.02, f"each output with the inputs drawn next to it, not the nearest numbers: BOUT1 {v1}, BOUT2 {v2}")
        check("inferred pairing: OUT 1 with +6 -7; OUT 8 with +2 -3 (by place in the symbol)" in t, "the guessed pairing said")
        check("inferred" not in t.split(f"{u4} GS8552")[0], "LM358's 1IN+ / 1OUT style or a single channel: not inferred")

        print("[a probe on a part left out]")
        err, t = sim({"refs": [u2, r3, r4], "sources": {"SIG2": 3}, "probes": ["OUT"]})
        check(err and "OUT has no modelled part on it" in t and f"left out (their pins open): {u2} LOADIC (no model" in t,
              "refused, with what was left out: " + t.replace("\n", " | ")[:140])

        print("[ac: the MLCCs at their DC bias]")
        err, t = sim({"around": [u1], "sources": {"VIN": "dc 5 ac 1"}, "loads": {"+3V3": 0.1}, "ac": {"from": 100, "to": "10Meg"}, "probes": ["+3V3"]})
        check(not err and re.search(re.escape(c1) + r" .*@ 3\.\d+ V: C_eff", t), f"ac: {c1} at the op point's 3.3 V: " +
              next((l.strip()[:120] for l in t.splitlines() if l.strip().startswith(c1 + " ")), t[:120]))
        print("      ", "\n       ".join(ln for ln in t.splitlines() if ln.startswith("  +3V3")))
        lo = num(t, r"\n  \+3V3: (-[\d.]+) dB at 100 Hz")
        check(lo and abs(lo + 72) < 1, f"PSRR at 100 Hz {lo} dB: the data's 72 dB (the loop does not take it lower again)")
        err, t = sim({"around": [u1], "sources": {"VIN": "dc 5 ac 1"}, "loads": {"+3V3": 0.1}, "ac": {"from": "100k", "to": "1Meg"},
                      "probes": ["+3V3"], "models": {c1: {"exclude": True}, c2: {"exclude": True}}})
        hi = num(t, r"\n  \+3V3: (-[\d.]+) dB at 100 kHz")
        check(hi and abs(hi + 52) < 1.5, f"at 100 kHz without the output caps {hi} dB: 20 dB/dec worse from 10 kHz" + ("" if hi else " " + t.replace("\n", " | ")[:400]))

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

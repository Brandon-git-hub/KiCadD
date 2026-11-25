"""The built-in simulate models in fixed scenarios (headless): physical bounds only, no exact numbers.

usage: python3 -I simmodel_e2e.py --build BUILD --kicad KICAD_SRC --work DIR --ngspice NGSPICE [--index PARTS_DB[.gz]]

Each class of built-in model (LDO, op-amp, N / P MOSFET, diode, Zener, R / C / L / bead) is built from
box symbols in a blank project with its parameters fixed by models {kind, ...}, so nothing depends on
the parts index; with --index (one with the params tables) each class also gets one real part read
from the index (the data path). The scenarios (start-up from 0 V, EN, load step, line step, overload,
powered down, reverse) check what physics allows, not what a datasheet says:
  the output stays within its rails / its input; it does not stick at Vin - dropout (wind-up); the
  current stays under the limit; off means no output; it settles in the time given; a pass element,
  switch or diode does not conduct backwards past leakage; a 0 V supply delivers no power.
A part that tripped one of these in model_survey is pinned here with its parameters written out.
"""

import argparse
import gzip
import os
import re
import shutil
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from m5_e2e import FAILS, Mcp, Server, binaries, check, make_project  # noqa: E402

PRE = {"G": 1e9, "M": 1e6, "k": 1e3, "": 1, "m": 1e-3, "µ": 1e-6, "n": 1e-9, "p": 1e-12, "f": 1e-15}
NUM = r"(-?[\d.]+(?:e[-+]?\d+)? ?[GMkmµnpf]?)"


def si(text):
    m = re.match(r"(-?[\d.]+(?:e[-+]?\d+)?) ?([GMkmµnpf]?)", text.strip())
    return float(m.group(1)) * PRE[m.group(2)]


def op_v(t, net):
    """op: a net's voltage"""
    m = re.search(r"\n  " + re.escape(net) + r": " + NUM + r"V", t)
    return si(m.group(1)) if m else None


def tran(t, net):
    """tran: avg, pp, min, max of a probe (None when not there)"""
    m = re.search(r"\n  " + re.escape(net) + r": " + NUM + r"V, ripple " + NUM + r"V p-p over the last [^;]*; min " + NUM
                  + r"V at " + NUM + r"s, max " + NUM + r"V at " + NUM + "s", t)
    if not m:
        return None
    return {"avg": si(m.group(1)), "pp": si(m.group(2)), "min": si(m.group(3)), "tmin": si(m.group(4)),
            "max": si(m.group(5)), "tmax": si(m.group(6)),
            "settled": None if "settled to" not in t.split("\n  " + net + ": ")[1].split("\n")[0] else
            si(re.search(r"settled to [\d.]+% at " + NUM + "s", t.split("\n  " + net + ": ")[1]).group(1))}


def src_i(t, net):
    """a source's current (delivering positive), and its peak (tran)"""
    m = re.search(r"\n  " + re.escape(net) + r" source: " + NUM + r"A(?: \([^)]*\))?(?:, peak " + NUM + "A)?", t)
    if not m:
        return None, None
    return si(m.group(1)), (si(m.group(2)) if m.group(2) else None)


def out_i(t, who):
    """a model's current line ("U1 out: 10 mA rms, peak 20 mA"): (rms, peak)"""
    m = re.search(r"\n  " + re.escape(who) + r": " + NUM + r"A(?: rms, peak " + NUM + "A)?", t)
    if not m:
        return None, None
    return si(m.group(1)), (si(m.group(2)) if m.group(2) else None)


def main():
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", required=True)
    ap.add_argument("--kicad", required=True)
    ap.add_argument("--work", required=True)
    ap.add_argument("--ngspice", required=True)
    ap.add_argument("--index", default="")
    a = ap.parse_args()
    work = os.path.abspath(a.work)
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(work)
    proj = make_project(a.kicad, work)
    if a.index.endswith(".gz"):
        with gzip.open(a.index) as src, open(os.path.join(work, "parts.db"), "wb") as dst:
            shutil.copyfileobj(src, dst)
        a.index = os.path.join(work, "parts.db")
    sock = os.path.join(work, "s.sock")
    _, kicadd_mcp, env = binaries(os.path.abspath(a.build))
    env = dict(env, KICADD_LCSC_OFFLINE="1", KICADD_NGSPICE=os.path.abspath(a.ngspice))
    if a.index:
        env["KICADD_PARTS_DB"] = os.path.abspath(a.index)
    server = Server(a.build, sock, os.path.join(proj, "blank.kicad_pcb"), env, os.path.join(work, "server.log"))
    check(server.start(), "api-server up")
    mcp = Mcp([kicadd_mcp, "--socket", sock, "--stdio"], env)
    try:
        refs = {}

        def sym(name, value, pins):
            err, t = mcp.tool("lib_create_symbol", {"name": name, "value": value, "pins": [
                {"number": n, "name": nm, "type": ty} for n, nm, ty in pins]})
            check(not err, f"symbol {name}")

        def part(lib, value, lcsc=""):
            y = 25.4 + 12.7 * len(refs)
            err, t = mcp.tool("sch_add_part", {"lib_id": lib, "value": value, "x": 25.4 + 50.8 * (len(refs) % 6), "y": y})
            check(not err, t.splitlines()[0][:80])
            ref = re.match(r"added (\S+) ", t).group(1)
            if lcsc:
                mcp.tool("sch_set_fields", {"parts": {ref: {"LCSC": lcsc}}})
            refs[ref] = value
            return ref

        def sim(args, show=False, expect=None):
            """expect: what a model check must say (None: no model check at all)"""
            err, t = mcp.tool("simulate", args)
            if show or err:
                print("       " + t.strip().replace("\n", "\n       ")[:2400])
            said = [ln for ln in t.splitlines() if ln.startswith("model check:")]
            if expect is None:
                check(not said, "no model check" + (": " + " | ".join(said)[:300] if said else ""))
            else:
                for e in [expect] if isinstance(expect, str) else expect:
                    check(any(e in ln for ln in said), f"model check says '{e}': " + (" | ".join(said)[:300] or "nothing"))
            return err, t

        print("[build the circuits]")
        sym("LDOX", "LDO", [("1", "VIN", "power_in"), ("2", "GND", "power_in"), ("3", "EN", "input"), ("4", "VOUT", "power_out")])
        sym("LDO3", "LDO3", [("1", "GND", "power_in"), ("2", "VOUT", "power_out"), ("3", "VIN", "power_in")])
        sym("OPAX", "OPAMP", [("1", "OUT", "output"), ("2", "-IN", "input"), ("3", "+IN", "input"),
                              ("4", "V-", "power_in"), ("8", "V+", "power_in")])
        sym("FETX", "FET", [("1", "G", "input"), ("2", "S", "passive"), ("3", "D", "passive")])
        sym("DIOX", "DIODE", [("1", "K", "passive"), ("2", "A", "passive")])

        # fixed parameters (no LCSC number): kind + models
        u1 = part("kicadd:LDOX", "LDO")
        c1 = part("Device:C", "10uF")
        u2 = part("kicadd:OPAX", "OPAMP")
        q1 = part("kicadd:FETX", "NMOS")
        r2 = part("Device:R", "10")
        q2 = part("kicadd:FETX", "PMOS")
        r3 = part("Device:R", "10")
        d1 = part("kicadd:DIOX", "DIODE")
        r4 = part("Device:R", "1k")
        d2 = part("kicadd:DIOX", "ZENER")
        r5 = part("Device:R", "1k")
        r6 = part("Device:R", "1k")
        c2 = part("Device:C", "1uF")
        r7 = part("Device:R", "0.1")
        l1 = part("Device:L", "10uH")
        c3 = part("Device:C", "10uF")
        fb1 = part("Device:R", "BEAD")
        r8 = part("Device:R", "1")
        c4 = part("Device:C", "100uF")
        mcp.tool("sch_set_fields", {"parts": {c4: {"Footprint": "Capacitor_SMD:CP_Elec_6.3x5.4"}}})
        r9 = part("Device:R", "10")
        nets = {
            "L_VIN": [f"{u1}.1"], "L_EN": [f"{u1}.3"], "L_OUT": [f"{u1}.4", f"{c1}.1"],
            "A_VCC": [f"{u2}.8"], "A_IN": [f"{u2}.3"], "A_OUT": [f"{u2}.1", f"{u2}.2"],
            "M_VDD": [f"{r2}.1"], "M_D": [f"{r2}.2", f"{q1}.3"], "M_G": [f"{q1}.1"],
            "P_VIN": [f"{q2}.2"], "P_G": [f"{q2}.1"], "P_OUT": [f"{q2}.3", f"{r3}.1"],
            "D_IN": [f"{d1}.2"], "D_OUT": [f"{d1}.1", f"{r4}.1"],
            "Z_IN": [f"{r5}.1"], "Z_K": [f"{r5}.2", f"{d2}.1"],
            "RC_IN": [f"{r6}.1"], "RC_OUT": [f"{r6}.2", f"{c2}.1"],
            "LC_IN": [f"{r7}.1"], "LC_M": [f"{r7}.2", f"{l1}.1"], "LC_OUT": [f"{l1}.2", f"{c3}.1"],
            "FB_IN": [f"{fb1}.1"], "FB_OUT": [f"{fb1}.2", f"{r8}.1"],
            "EC_IN": [f"{r9}.1"], "EC_P": [f"{r9}.2", f"{c4}.1"],
            "GND": [f"{u1}.2", f"{c1}.2", f"{u2}.4", f"{q1}.2", f"{r3}.2", f"{r4}.2", f"{d2}.2", f"{c2}.2", f"{c3}.2", f"{r8}.2", f"{c4}.2"],
        }
        err, t = mcp.tool("sch_connect", {"nets": nets, "power": ["GND"]})
        check(not err, "wired: " + t[:60])

        LDO = {u1: {"kind": "ldo", "vout": 3.3, "dropout": 0.3, "iq": "50u", "iout": 0.5},
               c1: {"c": "10u", "esr": "5m", "esl": "0.5n", "dc_bias": False}}
        ldo = {"refs": [u1, c1], "models": LDO}

        # ---------------------------------------------------------------- LDO
        print("[LDO: Vin from 0 V]")
        err, t = sim(dict(ldo, sources={"L_VIN": "pulse(0 5 0.1m 0.5m 0.5m 1 2)", "L_EN": 5}, loads={"L_OUT": 0.1},
                          tran={"stop": "2m", "step": "1u", "window": "0.5m"}))
        o = tran(t, "L_OUT")
        check(not err and o and abs(o["avg"] - 3.3) < 0.02, f"settles at its setpoint: {o and o['avg']} V")
        check(o and o["max"] < 3.3 * 1.03, f"no overshoot past 3% while Vin ramps (wind-up): max {o and o['max']} V")
        rms, pk = out_i(t, f"{u1} out")
        check(pk is not None and pk <= 0.75 * 1.02, f"output current within its limit 0.75 A (1.5 x Iout): peak {pk} A")

        print("[LDO: dropout, Vin below the setpoint]")
        err, t = sim(dict(ldo, sources={"L_VIN": 3.0, "L_EN": 5}, loads={"L_OUT": 0.1}))
        v = op_v(t, "L_OUT")
        check(not err and v is not None and 2.6 < v < 3.0, f"Vin 3 V: the output under Vin by about the dropout: {v} V")

        print("[LDO: EN]")
        err, t = sim(dict(ldo, sources={"L_VIN": 5, "L_EN": 0}, loads={"L_OUT": "1k ohm"}))
        v = op_v(t, "L_OUT")
        i, _ = src_i(t, "L_VIN")
        check(not err and v is not None and abs(v) < 0.01, f"EN low: no output ({v} V)")
        check(i is not None and i < 1e-3, f"EN low: the input draws no more than Iq ({i} A)")
        err, t = sim(dict(ldo, sources={"L_VIN": 5, "L_EN": "pulse(0 5 0.5m 1u 1u 1 2)"}, loads={"L_OUT": 0.05},
                          tran={"stop": "2m", "step": "1u", "window": "0.5m"}))
        o = tran(t, "L_OUT")
        check(not err and o and abs(o["min"]) < 0.01 and abs(o["avg"] - 3.3) < 0.02 and o["max"] < 3.3 * 1.03,
              f"EN on at 0.5 ms: from {o and o['min']} V to {o and o['avg']} V, max {o and o['max']} V")

        print("[LDO: load step]")
        err, t = sim(dict(ldo, sources={"L_VIN": 5, "L_EN": 5}, loads={"L_OUT": "pulse(0.01 0.4 0.5m 1u 1u 1 2)"},
                          tran={"stop": "2m", "step": "1u", "window": "0.5m"}))
        o = tran(t, "L_OUT")
        check(not err and o and o["min"] > 2.5 and abs(o["avg"] - 3.3) < 0.02 and o["max"] < 3.4,
              f"10 -> 400 mA: dips to {o and o['min']} V, back at {o and o['avg']} V")

        print("[LDO: line step]")
        err, t = sim(dict(ldo, sources={"L_VIN": "pulse(4.5 5.5 0.5m 1u 1u 1 2)", "L_EN": 5}, loads={"L_OUT": 0.1},
                          tran={"stop": "2m", "step": "1u", "window": "0.5m"}))
        o = tran(t, "L_OUT")
        check(not err and o and abs(o["avg"] - 3.3) < 0.02 and 3.2 < o["min"] and o["max"] < 3.4,
              f"Vin 4.5 -> 5.5 V: the output {o and o['min']} .. {o and o['max']} V")

        print("[LDO: overload]")
        err, t = sim(dict(ldo, sources={"L_VIN": 5, "L_EN": 5}, loads={"L_OUT": "1 ohm"}))
        v = op_v(t, "L_OUT")
        i, _ = src_i(t, "L_VIN")
        check(not err and v is not None and v < 0.8, f"1 Ω: the output falls to the limit's voltage ({v} V)")
        check(i is not None and i < 0.75 * 1.05, f"1 Ω: the input current within the limit plus Iq: {i} A")

        print("[LDO: powered down]")
        err, t = sim(dict(ldo, sources={"L_VIN": 0, "L_EN": 3.3}, loads={"L_OUT": "1k ohm"}))
        v = op_v(t, "L_OUT")
        check(not err and v is not None and abs(v) < 0.01, f"Vin 0 V, EN driven: no output ({v} V)")

        print("[LDO: reverse, Vout above Vin]")
        err, t = sim(dict(ldo, sources={"L_VIN": 0, "L_EN": 0, "L_OUT": 3.3}), expect=f"{u1}'s output over its input by up to 3.3 V")
        i, _ = src_i(t, "L_OUT")
        check(not err and i is not None and abs(i) < 1e-4, f"Vin 0 V, the output held at 3.3 V: reverse current {i} A (leakage)")
        err, t = sim(dict(ldo, sources={"L_VIN": 5, "L_EN": 5, "L_OUT": 4.0}))
        i, _ = src_i(t, "L_OUT")
        check(not err and i is not None and abs(i) < 1e-3, f"the output held above its setpoint: the pass element does not sink ({i} A)")
        print("[LDO: into a lower source (two sources fighting)]")
        err, t = sim(dict(ldo, sources={"L_VIN": 5, "L_EN": 5, "L_OUT": 3.0}), expect="L_OUT absorbs 750 mA")
        i, _ = src_i(t, "L_OUT")
        check(not err and i is not None and abs(i + 0.75) < 0.02, f"{u1} pushes its current limit into the 3 V source ({i} A)")

        print("[LDO: pinned from model_survey 2026-11-26]")
        # XC6216B802PR-G (C725707): 8 V, PSRR 30 dB: its PSRR high-pass took the start-up from 0 V as
        # ripple and the output sat at 8.32 V (PSRR x Vin) for the high-pass's second
        xc = {u1: {"kind": "ldo", "vout": 8, "dropout": 1.15, "iq": "5u", "psrr": 30, "iout": 0.15}, c1: LDO[c1]}
        err, t = sim({"refs": [u1, c1], "models": xc, "sources": {"L_VIN": "pulse(0 10.15 0.1m 0.5m 0.5m 1 2)", "L_EN": 10}, "loads": {"L_OUT": 0.075},
                      "tran": {"stop": "3m", "step": "1u", "window": "0.5m"}})
        o = tran(t, "L_OUT")
        check(not err and o and abs(o["avg"] - 8) < 0.03 and o["max"] < 8 * 1.03, f"8 V, PSRR 30 dB, from 0 V: settles at {o and o['avg']} V, max {o and o['max']} V")

        # ---------------------------------------------------------------- op-amp
        OPA = {u2: {"kind": "opamp", "gbw": "1Meg", "slew": 0.5, "vos": 0, "rr_out": 0}}
        opa = {"refs": [u2], "models": OPA}
        print("[op-amp: supply from 0 V]")
        err, t = sim(dict(opa, sources={"A_VCC": "pulse(0 5 10u 50u 50u 1 2)", "A_IN": 2.5}, loads={"A_OUT": "1k ohm"},
                          tran={"stop": "300u", "step": "0.1u", "window": "50u"}))
        o = tran(t, "A_OUT")
        check(not err and o and abs(o["avg"] - 2.5) < 0.01, f"follower settles at 2.5 V: {o and o['avg']} V")
        check(o and o["max"] < 2.5 * 1.03 and o["min"] > -0.05, f"within the rails while V+ rises: {o and o['min']} .. {o and o['max']} V")

        print("[op-amp: load step]")
        err, t = sim(dict(opa, sources={"A_VCC": 5, "A_IN": 2.5}, loads={"A_OUT": "pulse(0.001 0.01 50u 0.1u 0.1u 1 2)"},
                          tran={"stop": "200u", "step": "0.1u", "window": "50u"}))
        o = tran(t, "A_OUT")
        check(not err and o and abs(o["avg"] - 2.5) < 0.01 and 2.2 < o["min"] and o["max"] < 2.8,
              f"1 -> 10 mA: {o and o['min']} .. {o and o['max']} V, back at {o and o['avg']} V")

        print("[op-amp: input step, slew]")
        err, t = sim(dict(opa, sources={"A_VCC": 5, "A_IN": "pulse(1 3 10u 1n 1n 1 2)"}, loads={"A_OUT": "10k ohm"},
                          tran={"stop": "40u", "step": "10n", "window": "10u"}))
        o = tran(t, "A_OUT")
        rise = re.search(r"\n  A_OUT: .*?rise 10-90% " + NUM + "s", t)
        check(not err and o and abs(o["avg"] - 3) < 0.01, f"1 -> 3 V: settles at {o and o['avg']} V")
        check(rise and si(rise.group(1)) >= 0.9 * 1.6 / 0.5e6, f"10-90% in {rise and rise.group(1)}s: no faster than the slew 0.5 V/µs (3.2 µs)")

        print("[op-amp: overload]")
        err, t = sim(dict(opa, sources={"A_VCC": 5, "A_IN": 3.5}, loads={"A_OUT": "10 ohm"}))
        v = op_v(t, "A_OUT")
        i, _ = src_i(t, "A_VCC")
        check(not err and v is not None and -0.05 < v < 3.8, f"10 Ω: within its output range ({v} V)")
        check(i is not None and i >= 0, f"10 Ω: the output current comes from V+ ({i} A)")

        print("[op-amp: powered down]")
        err, t = sim(dict(opa, sources={"A_VCC": 0, "A_IN": 2.5}, loads={"A_OUT": "1k ohm"}),
                     expect=["A_VCC absorbs", "A_IN delivers"])
        v = op_v(t, "A_OUT")
        check(not err and v is not None and abs(v) < 0.1, f"V+ 0 V: no output into 1 kΩ ({v} V)")
        i, _ = src_i(t, "A_IN")
        check(i is not None and i > 1e-4, f"V+ 0 V: the input drives V+ through its ESD diode ({i} A)")

        # ---------------------------------------------------------------- MOSFETs
        FET = {"kind": "mosfet", "vgs_th": 2, "rds_on": "50m"}
        print("[N MOSFET: gate drive]")
        nfet = {"refs": [q1, r2], "models": {q1: FET}}
        err, t = sim(dict(nfet, sources={"M_VDD": 12, "M_G": "pulse(0 10 50u 1u 1u 1 2)"}, tran={"stop": "100u", "step": "0.1u", "window": "20u"},
                          probes=["M_D"]))
        o = tran(t, "M_D")
        check(not err and o and o["max"] > 11.9 and o["max"] < 12.01, f"off: the drain at Vdd ({o and o['max']} V)")
        check(o and 0 < o["avg"] < 2 * 1.2 * 0.05, f"on (10 V): Vds about I x Rds(on) = 60 mV: {o and o['avg']} V")

        print("[N MOSFET: load step]")
        err, t = sim(dict(nfet, sources={"M_VDD": 12, "M_G": 10}, loads={"M_D": "pulse(0 -2 20u 1u 1u 1 2)"},
                          tran={"stop": "60u", "step": "0.1u", "window": "20u"}))
        o = tran(t, "M_D")
        check(not err and o and 0 < o["avg"] < 2 * 3.2 * 0.05, f"1.2 A + 2 A pushed into the drain: Vds {o and o['avg']} V (3.2 A x 50 mΩ)")

        print("[N MOSFET: powered down, body diode]")
        err, t = sim(dict(nfet, sources={"M_VDD": 0, "M_G": 10}, probes=["M_D"]))
        v = op_v(t, "M_D")
        i, _ = src_i(t, "M_VDD")
        check(not err and v is not None and abs(v) < 1e-3 and abs(i) < 1e-4, f"Vdd 0 V: no current ({i} A), the drain at {v} V")
        err, t = sim(dict(nfet, sources={"M_VDD": -5, "M_G": 0}, probes=["M_D"]))
        v = op_v(t, "M_D")
        i, _ = src_i(t, "M_VDD")
        check(not err and v is not None and -1.2 < v < -0.4, f"drain 5 V below the source, gate off: the body diode at {v} V")
        check(i is not None and -0.5 < i < -0.35, f"body diode current about (5 - Vf) / 10 Ω: {i} A")

        print("[P MOSFET: high-side switch]")
        pfet = {"refs": [q2, r3], "models": {q2: FET}}
        err, t = sim(dict(pfet, sources={"P_VIN": 12, "P_G": "pulse(12 0 50u 1u 1u 1 2)"}, tran={"stop": "100u", "step": "0.1u", "window": "20u"},
                          probes=["P_OUT"]))
        o = tran(t, "P_OUT")
        check(not err and o and abs(o["min"]) < 0.01, f"gate at the source: off ({o and o['min']} V)")
        check(o and 12 - 2 * 1.2 * 0.05 < o["avg"] <= 12, f"gate 12 V below: on, {o and o['avg']} V")
        check("P-ch" in t, "PMOS in the value: a P-channel model")
        err, t = sim(dict(pfet, sources={"P_VIN": 0, "P_G": 0}, probes=["P_OUT"]))
        v = op_v(t, "P_OUT")
        check(not err and v is not None and abs(v) < 1e-3, f"P_VIN 0 V: no output ({v} V)")

        # ---------------------------------------------------------------- diodes
        DIO = {d1: {"kind": "d", "vf": 0.7, "if": "10m"}, d2: {"kind": "zener", "vz": 5.1, "zzt": 30}}
        only = lambda m, *rs: {r: m[r] for r in rs if r in m}  # noqa: E731
        print("[diode: from 0 V, forward, reverse]")
        dio = {"refs": [d1, r4], "models": only(DIO, d1)}
        err, t = sim(dict(dio, sources={"D_IN": "pulse(0 5 10u 50u 50u 1 2)"}, tran={"stop": "200u", "step": "0.1u", "window": "50u"},
                          probes=["D_OUT"]))
        o = tran(t, "D_OUT")
        check(not err and o and 4.1 < o["avg"] < 4.5 and o["max"] < 5, f"5 V into 1 kΩ: {o and o['avg']} V (5 - ~0.7)")
        check(o and o["min"] > -1e-3, f"never below 0 V at the start ({o and o['min']} V)")
        err, t = sim(dict(dio, sources={"D_IN": -5}, probes=["D_OUT"]))
        v = op_v(t, "D_OUT")
        check(not err and v is not None and abs(v) < 1e-3, f"reverse 5 V: off ({v} V)")
        err, t = sim(dict(dio, sources={"D_IN": "pulse(-5 5 10u 1n 1n 20u 40u)"}, tran={"stop": "60u", "step": "10n", "window": "10u"},
                          probes=["D_OUT"]))
        o = tran(t, "D_OUT")
        check(not err and o and o["min"] > -0.05 and o["max"] < 5, f"a -5 -> 5 V square: the output {o and o['min']} .. {o and o['max']} V")

        print("[diode: pinned from model_survey 2026-11-26]")
        # ESTF30SS120U (C41417722): Vf 2.2 V @ 30 A, all of it once the junction's: 1.78 V at 4 mA
        err, t = sim({"refs": [d1, r4], "models": {d1: {"kind": "d", "vf": 2.2, "if": 30}}, "sources": {"D_IN": 5}, "probes": ["D_OUT"]})
        v = op_v(t, "D_OUT")
        check(not err and v is not None and 4.2 < v < 4.6 and "junction +" in t, f"Vf 2.2 V @ 30 A, at 4 mA: {v and 5 - v:.3f} V (the rest is series resistance)")

        print("[model checks: a diode made to leak]")
        err, t = sim({"refs": [d1, r4], "models": {d1: {"kind": "d", "vf": -0.5, "if": "10m"}}, "sources": {"D_IN": -5, "D_OUT": 0}},
                     expect=f"{d1} conducts up to")

        print("[Zener: below, at and past Vz; forward]")
        zen = {"refs": [d2, r5], "models": only(DIO, d2)}
        err, t = sim(dict(zen, sources={"Z_IN": 12}, probes=["Z_K"]))
        v = op_v(t, "Z_K")
        check(not err and v is not None and 5.0 < v < 5.4, f"12 V through 1 kΩ: Vz {v} V (5.1 V, Zzt 30 Ω x 7 mA)")
        err, t = sim(dict(zen, sources={"Z_IN": 3}, probes=["Z_K"]))
        v = op_v(t, "Z_K")
        check(not err and v is not None and v > 2.99, f"3 V (under Vz): not conducting, {v} V")
        err, t = sim(dict(zen, sources={"Z_IN": -5}, probes=["Z_K"]))
        v = op_v(t, "Z_K")
        check(not err and v is not None and -0.9 < v < -0.5, f"-5 V: forward biased, {v} V (Zzt is not a forward resistance)")
        err, t = sim(dict(zen, sources={"Z_IN": "pulse(0 12 10u 50u 50u 1 2)"}, tran={"stop": "200u", "step": "0.1u", "window": "50u"},
                          probes=["Z_K"]))
        o = tran(t, "Z_K")
        check(not err and o and o["max"] < 5.4 and o["min"] > -1e-3, f"0 -> 12 V ramp: clamped, {o and o['min']} .. {o and o['max']} V")

        # ---------------------------------------------------------------- passives
        PAS = {c2: {"c": "1u", "esr": "1m", "esl": "0.1n", "dc_bias": False}, c3: {"c": "10u", "esr": "5m", "esl": "0.5n", "dc_bias": False},
               l1: {"dcr": "10m"}, fb1: {"kind": "bead", "dcr": "50m"}}
        print("[RC: charge, step]")
        err, t = sim({"refs": [r6, c2], "models": only(PAS, c2), "sources": {"RC_IN": "pulse(0 5 0.1m 1n 1n 10 20)"},
                      "tran": {"stop": "6m", "step": "1u", "window": "0.5m"}, "probes": ["RC_OUT"]})
        o = tran(t, "RC_OUT")
        check(not err and o and abs(o["avg"] - 5) < 0.02 and o["max"] < 5.001 and o["min"] > -1e-3,
              f"0 -> 5 V into 1 kΩ / 1 µF: {o and o['min']} .. {o and o['max']} V, settles at {o and o['avg']} V")
        check(o and o["settled"] and 0.1e-3 + 3.5e-3 < o["settled"] < 0.1e-3 + 4.5e-3, f"settled to 2% at {o and o['settled']} s (4 RC = 4 ms after the step)")

        print("[LC: start-up]")
        err, t = sim({"refs": [r7, l1, c3], "models": only(PAS, l1, c3), "sources": {"LC_IN": "pulse(0 5 0.1m 1n 1n 10 20)"},
                      "tran": {"stop": "3m", "step": "0.2u", "window": "0.5m"}, "probes": ["LC_OUT"]})
        o = tran(t, "LC_OUT")
        check(not err and o and abs(o["avg"] - 5) < 0.05, f"settles at 5 V ({o and o['avg']} V)")
        check(o and o["max"] < 10.0 and o["min"] > -5.0, f"an LC step rings no higher than twice the step: max {o and o['max']} V")
        rms, pk = out_i(t, l1)
        check(pk is not None and pk < 5 * (10e-6 / 10e-6) ** 0.5 * 1.05, f"the inductor's peak current under V / Z0 = 5 A: {pk} A")

        print("[bead: DC drop]")
        err, t = sim({"refs": [fb1, r8], "models": only(PAS, fb1), "sources": {"FB_IN": 1}, "probes": ["FB_OUT"]})
        v = op_v(t, "FB_OUT")
        check(not err and v is not None and abs(v - 1 / 1.05) < 2e-3, f"50 mΩ over 1 Ω: {v} V")

        print("[electrolytic: reverse bias]")
        err, t = sim({"refs": [r9, c4], "sources": {"EC_IN": -5}, "probes": ["EC_P"]})
        line = next((ln for ln in t.splitlines() if ln.strip().startswith(c4 + " ")), "")
        check(not err and "aluminium" in line and "reverse" in line, f"{c4} reverse biased: said ({line.strip()[:120]})")
        err, t = sim({"refs": [r9, c4], "sources": {"EC_IN": 5}, "probes": ["EC_P"]})
        line = next((ln for ln in t.splitlines() if ln.strip().startswith(c4 + " ")), "")
        check(not err and "reverse" not in line, f"{c4} forward biased: nothing said")

        # ---------------------------------------------------------------- the data path
        if a.index:
            print("[from the parts index]")
            u3 = part("kicadd:LDO3", "AMS1117-3.3", "C6186")
            c5 = part("Device:C", "10uF", "C19702")
            u4 = part("kicadd:OPAX", "LM358", "C7950")
            q3 = part("kicadd:FETX", "AO3400A", "C20917")
            r10 = part("Device:R", "10")
            q4 = part("kicadd:FETX", "AO3401A", "C15127")
            r11 = part("Device:R", "10")
            d3 = part("kicadd:DIOX", "1N4148W", "C81598")
            r12 = part("Device:R", "1k")
            d4 = part("kicadd:DIOX", "BZT52C5V1", "C173407")
            r13 = part("Device:R", "1k")
            err, t = mcp.tool("sch_connect", {"nets": {
                "X_VIN": [f"{u3}.3"], "X_OUT": [f"{u3}.2", f"{c5}.1"],
                "XA_VCC": [f"{u4}.8"], "XA_IN": [f"{u4}.3"], "XA_OUT": [f"{u4}.1", f"{u4}.2"],
                "XM_VDD": [f"{r10}.1"], "XM_D": [f"{r10}.2", f"{q3}.3"], "XM_G": [f"{q3}.1"],
                "XP_VIN": [f"{q4}.2"], "XP_G": [f"{q4}.1"], "XP_OUT": [f"{q4}.3", f"{r11}.1"],
                "XD_IN": [f"{d3}.2"], "XD_OUT": [f"{d3}.1", f"{r12}.1"],
                "XZ_IN": [f"{r13}.1"], "XZ_K": [f"{r13}.2", f"{d4}.1"],
                "GND": [f"{u3}.1", f"{c5}.2", f"{u4}.4", f"{q3}.2", f"{r11}.2", f"{r12}.2", f"{d4}.2"]}, "power": ["GND"]})
            check(not err, "wired the index parts")
            err, t = sim({"refs": [u3, c5], "sources": {"X_VIN": "pulse(0 5 0.1m 0.5m 0.5m 1 2)"}, "loads": {"X_OUT": 0.2},
                          "tran": {"stop": "2m", "step": "1u", "window": "0.5m"}})
            o = tran(t, "X_OUT")
            rms, pk = out_i(t, f"{u3} out")
            check(not err and o and abs(o["avg"] - 3.3) < 0.03 and o["max"] < 3.3 * 1.03 and pk <= 1.5 * 1.02,
                  f"C6186 from 0 V: {o and o['avg']} V, max {o and o['max']} V, peak {pk} A (limit 1.5 A)")
            err, t = sim({"refs": [u3, c5], "sources": {"X_VIN": 0, "X_OUT": 3.3}}, expect="output over its input")
            i, _ = src_i(t, "X_OUT")
            check(not err and i is not None and abs(i) < 1e-4, f"C6186 reverse: {i} A")
            err, t = sim({"refs": [u4], "sources": {"XA_VCC": "pulse(0 5 10u 50u 50u 1 2)", "XA_IN": 2.5}, "loads": {"XA_OUT": "1k ohm"},
                          "tran": {"stop": "300u", "step": "0.1u", "window": "50u"}})
            o = tran(t, "XA_OUT")
            check(not err and o and abs(o["avg"] - 2.5) < 0.02 and o["max"] < 2.6 and o["min"] > -0.05,
                  f"C7950 follower, V+ from 0 V: {o and o['min']} .. {o and o['max']} V, at {o and o['avg']} V")
            err, t = sim({"refs": [q3, r10], "sources": {"XM_VDD": 5, "XM_G": "pulse(0 4.5 20u 1u 1u 1 2)"},
                          "tran": {"stop": "60u", "step": "0.1u", "window": "20u"}, "probes": ["XM_D"]})
            o = tran(t, "XM_D")
            check(not err and o and o["max"] > 4.99 and 0 < o["avg"] < 0.1, f"C20917 at 4.5 V gate: off {o and o['max']} V, on {o and o['avg']} V")
            err, t = sim({"refs": [q4, r11], "sources": {"XP_VIN": 5, "XP_G": "pulse(5 0 20u 1u 1u 1 2)"},
                          "tran": {"stop": "60u", "step": "0.1u", "window": "20u"}, "probes": ["XP_OUT"]})
            o = tran(t, "XP_OUT")
            check(not err and o and abs(o["min"]) < 0.01 and 4.9 < o["avg"] <= 5, f"C15127 high side: off {o and o['min']} V, on {o and o['avg']} V")
            err, t = sim({"refs": [d3, r12], "sources": {"XD_IN": 5}, "probes": ["XD_OUT"]})
            v = op_v(t, "XD_OUT")
            check(not err and v is not None and 4.0 < v < 4.5, f"C81598 forward 5 V into 1 kΩ: {v} V")
            err, t = sim({"refs": [d3, r12], "sources": {"XD_IN": -5}, "probes": ["XD_OUT"]})
            v = op_v(t, "XD_OUT")
            check(not err and v is not None and abs(v) < 1e-3, f"C81598 reverse: {v} V")
            err, t = sim({"refs": [d4, r13], "sources": {"XZ_IN": 12}, "probes": ["XZ_K"]})
            v = op_v(t, "XZ_K")
            check(not err and v is not None and 4.8 < v < 6.2, f"C173407 12 V through 1 kΩ: Vz {v} V")
            err, t = sim({"refs": [d4, r13], "sources": {"XZ_IN": -5}, "probes": ["XZ_K"]})
            v = op_v(t, "XZ_K")
            check(not err and v is not None and -0.95 < v < -0.5, f"C173407 forward: {v} V (Zzt 130 Ω is not a forward resistance)")
    finally:
        mcp.close()
        server.stop()

    print("\nRESULT:", "PASS" if not FAILS else f"FAIL ({len(FAILS)})")
    sys.exit(1 if FAILS else 0)


if __name__ == "__main__":
    main()

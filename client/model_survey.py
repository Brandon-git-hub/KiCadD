"""Survey the built-in simulate models on real parts (headless; not in run_all: run it before a release).

usage: python3 -I model_survey.py --build BUILD --kicad KICAD_SRC --ngspice NGSPICE --index PARTS_DB[.gz]
                                  [--per-class 100] [--seed 1] [--classes ldo,opamp,...] [--out run/survey]
                                  [--compare run/survey/<date>]

Samples parts of each class from the parts index (one with the params tables; the same seed gives the
same parts), puts each into its class's standard circuit (a box symbol or Device:R / C / L with the
LCSC field set), runs simulate_e2e's scenarios on it and judges only by physics: the "model check:"
lines simulate adds (1c) and the bounds of simmodel_e2e (output within its rails / input, settles at
its setpoint, the current under its limit, off is off, no reverse conduction). No design judgement.

Writes <out>/<date>/survey.md and survey.json: per class the sampled count, how many got a model,
why the others were left out, how often each check tripped, and the parts that tripped most (LCSC,
description, scenario, check). With --compare (or the latest earlier survey in <out>) the rates are
set against that one: a rate that went up is listed first. A part that trips a check, once fixed,
belongs in simmodel_e2e as a regression with its parameters written out.

Classes: ldo, opamp, nmos, pmos, diode, schottky, zener, mlcc, alu, tantalum, polymer, inductor, bead,
resistor.
"""

import argparse
import collections
import datetime
import gzip
import json
import os
import random
import re
import shutil
import sqlite3
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from m5_e2e import Mcp, Server, binaries, make_project  # noqa: E402
from simmodel_e2e import op_v, out_i, si, src_i, tran  # noqa: E402

# class -> (subcategory SQL condition, extra condition)
CLASSES = {
    "ldo": ("(subcategory LIKE '%LDO%' OR subcategory LIKE 'Linear Voltage Regulators%' OR subcategory LIKE '%Dropout Regulators%')",
            "EXISTS(SELECT 1 FROM params q WHERE q.lcsc=p.lcsc AND q.name='vout')"),
    "opamp": ("(subcategory LIKE '%Op Amp%' OR subcategory LIKE '%Operational Amplifier%' OR subcategory LIKE '%OpAmp%' "
              "OR subcategory LIKE 'FET Input%' OR subcategory LIKE 'General Purpose Amplifiers')", ""),
    "nmos": ("subcategory LIKE '%MOSFET%'", "(description LIKE '%N-Channel%' OR description LIKE '%N Channel%') AND description NOT LIKE '%P-Channel%'"),
    "pmos": ("subcategory LIKE '%MOSFET%'", "(description LIKE '%P-Channel%' OR description LIKE '%P Channel%') AND description NOT LIKE '%N-Channel%'"),
    "diode": ("(subcategory LIKE 'Switching Diode%' OR subcategory LIKE 'Diodes - General Purpose' OR subcategory LIKE 'Fast Recovery%' "
              "OR subcategory LIKE 'Diodes - Fast Recovery%')", ""),
    "schottky": ("(subcategory LIKE 'Schottky%')", ""),
    "zener": ("subcategory LIKE 'Zener Diodes'", "EXISTS(SELECT 1 FROM params q WHERE q.lcsc=p.lcsc AND q.name='vz')"),
    "mlcc": ("subcategory LIKE 'Multilayer Ceramic Capacitors MLCC - SMD%'", "value > 0 AND voltage > 0"),
    "alu": ("subcategory LIKE 'Aluminum Electrolytic Capacitors - SMD'", "value > 0 AND voltage > 0"),
    "tantalum": ("subcategory LIKE 'Tantalum Capacitors'", "value > 0 AND voltage > 0"),
    "polymer": ("subcategory LIKE 'Polymer Aluminum Capacitors'", "value > 0 AND voltage > 0"),
    "inductor": ("subcategory LIKE 'Power Inductors'", "EXISTS(SELECT 1 FROM params q WHERE q.lcsc=p.lcsc AND q.name='l')"),
    "bead": ("subcategory LIKE 'Ferrite Beads'", ""),
    "resistor": ("subcategory LIKE 'Chip Resistor - Surface Mount'", "value > 0"),
}


def sample(db, cls, n, seed):
    sub, extra = CLASSES[cls]
    where = sub + (" AND " + extra if extra else "")
    rows = db.execute(f"SELECT lcsc, mfr, description, value, voltage FROM parts p WHERE {where} ORDER BY lcsc").fetchall()
    rnd = random.Random(f"{seed}:{cls}")
    rows = rnd.sample(rows, min(n, len(rows)))
    out = []
    for lcsc, mfr, desc, value, volt in rows:
        params = {k: v for k, v in db.execute("SELECT name, value FROM params WHERE lcsc=?", (lcsc,))}
        out.append({"lcsc": f"C{lcsc}", "mfr": mfr or "", "desc": (desc or "")[:160], "value": value, "voltage": volt, "params": params})
    return out


class Survey:
    def __init__(self, mcp):
        self.mcp = mcp
        self.refs = {}
        self.last = []   # this part's answers (--verbose prints them when it trips)

    def part(self, lib, value):
        err, t = self.mcp.tool("sch_add_part", {"lib_id": lib, "value": value, "x": 25.4 + 50.8 * (len(self.refs) % 6),
                                                "y": 25.4 + 12.7 * len(self.refs)})
        if err:
            raise RuntimeError(t)
        ref = re.match(r"added (\S+) ", t).group(1)
        self.refs[ref] = value
        return ref

    def sim(self, args):
        err, t = self.mcp.tool("simulate", args)
        self.last.append(t)
        return err, t


def judge(trips, scen, cond, what):
    """one bound: False adds (scenario, check) to trips"""
    if not cond:
        trips.append((scen, what))


def model_checks(t, scen, trips, expected=()):
    """the model check lines as trips, but those the scenario provokes on purpose (expected words)"""
    for ln in t.splitlines():
        if ln.startswith("model check:") and not any(e in ln for e in expected):
            # the check's kind: its words without the numbers / refs
            body = ln[len("model check: "):]
            key = re.sub(r"\b[A-Z]+\d+\b", "X", body)
            key = re.sub(r"-?[\d.]+ ?[GMkmµnpf]?(V|A|s|%)", "#", key)
            trips.append((scen, "model check: " + key[:90]))


def left_out(t):
    m = re.search(r"\nleft out \(their pins open\): (.*)", t)
    return m.group(1) if m else ""


def run_ldo(s, refs, p):
    u, c = refs
    vout = p["params"].get("vout", 0)
    drop = p["params"].get("dropout", 0.3)
    vin_max = p["params"].get("vin_max", 0)
    iout = p["params"].get("iout", 0.1)
    vin = vout + max(drop, 0.3) + 1.0
    if vin_max:
        vin = min(vin, vin_max * 0.9)
    trips = []
    if vin < vout + drop + 0.05:
        return "Vin max too close to Vout", trips
    load = max(min(0.5 * iout, 0.5), 1e-3)
    reg = {"refs": [u, c], "models": {c: {"c": "10u", "esr": "5m", "esl": "0.5n", "dc_bias": False}}}
    # long enough for a small current limit (1.5 x Iout) to charge the 10 µF past the load
    charge = 10e-6 * vout / max(1.5 * iout - load, 1e-4)
    stop = min(max(3e-3, 0.6e-3 + 3 * charge), 0.2)
    err, t = s.sim(dict(reg, sources={"S_LVIN": f"pulse(0 {vin:.4g} 0.1m 0.5m 0.5m 1 2)"}, loads={"S_LOUT": load},
                        tran={"stop": f"{stop:.4g}", "step": f"{stop / 3000:.4g}", "window": f"{stop / 6:.4g}"}))
    if err:
        return "error: " + t.splitlines()[0][:100], trips
    if f"\n  {u} " not in t:
        return "no model: " + left_out(t)[:100], trips
    model_checks(t, "start-up", trips)
    o = tran(t, "S_LOUT")
    rms, pk = out_i(t, f"{u} out")
    judge(trips, "start-up", o and abs(o["avg"] - vout) < 0.03 * vout + 0.01, "settles at Vout ±3%")
    judge(trips, "start-up", o and o["max"] < vout * 1.05 + 0.01, "no overshoot past 5%")
    lim = re.search(r"current limit " + r"(-?[\d.]+ ?[GMkmµnpf]?)A", t)
    if lim and pk is not None:
        judge(trips, "start-up", pk <= si(lim.group(1)) * 1.02, "peak under its current limit")
    err, t = s.sim(dict(reg, sources={"S_LVIN": vin}, loads={"S_LOUT": f"{vout / max(iout * 3, 0.01):.4g} ohm"}))
    if not err:
        model_checks(t, "overload", trips)
        i, _ = src_i(t, "S_LVIN")
        if lim and i is not None:
            judge(trips, "overload", i <= si(lim.group(1)) * 1.05 + p["params"].get("iq", 0), "input current within the limit")
    err, t = s.sim(dict(reg, sources={"S_LVIN": 0, "S_LOUT": vout}))
    if not err:
        model_checks(t, "reverse", trips, expected=("output over its input",))
        i, _ = src_i(t, "S_LOUT")
        judge(trips, "reverse", i is not None and abs(i) < 1e-4, "reverse current under 100 µA")
    return "", trips


def run_opamp(s, refs, p):
    (u,) = refs
    smin, smax = p["params"].get("supply_min", 0), p["params"].get("supply_max", 0)
    vcc = 5.0
    if smax and smax < 5:
        vcc = smax * 0.95
    if smin and smin > vcc:
        vcc = smin * 1.05
    trips = []
    vin = vcc / 2
    # long enough for a slow one to slew there and settle (GBW, slew from the data or its defaults)
    slew = p["params"].get("slew", 0.5) * 1e6
    gbw = p["params"].get("gbw", 1e6)
    stop = min(max(400e-6, 60e-6 + 3 * vin / max(slew, 1) + 20 / max(gbw, 1)), 0.5)
    err, t = s.sim({"refs": [u], "sources": {"S_AVCC": f"pulse(0 {vcc:.4g} 10u 50u 50u 1 2)", "S_AIN": vin}, "loads": {"S_AOUT": "10k ohm"},
                    "tran": {"stop": f"{stop:.4g}", "step": f"{stop / 4000:.4g}", "window": f"{stop / 8:.4g}"}})
    if err:
        return "error: " + t.splitlines()[0][:100], trips
    if f"\n  {u} " not in t:
        return "no model: " + left_out(t)[:100], trips
    model_checks(t, "start-up", trips)
    o = tran(t, "S_AOUT")
    rr = "rail to rail" in t
    if o and (rr or 1.3 < vin < vcc - 1.3):
        judge(trips, "start-up", abs(o["avg"] - vin) < 0.02 + abs(p["params"].get("vos", 0)), "follower settles at its input")
        judge(trips, "start-up", o["max"] < vin * 1.03 + 0.02 and o["min"] > -0.05, "within its input / the rails while V+ rises")
    err, t = s.sim({"refs": [u], "sources": {"S_AVCC": 0, "S_AIN": vin}, "loads": {"S_AOUT": "1k ohm"}})
    if not err:
        model_checks(t, "powered down", trips, expected=("S_AVCC absorbs", "S_AIN delivers"))
        v = op_v(t, "S_AOUT")
        judge(trips, "powered down", v is not None and abs(v) < 0.1, "no output with V+ at 0 V")
    return "", trips


def run_fet(s, refs, p, pch):
    q, r = refs
    vds = p["params"].get("vds", 30)
    vdd = min(abs(vds) * 0.5, 12)
    vg = min(10.0, max(abs(p["params"].get("vgs_th", 2)) * 3, 4.5))
    rds = p["params"].get("rds_on", 0.1)
    trips = []
    if pch:
        args = {"refs": [q, r], "sources": {"S_PVIN": vdd, "S_PG": f"pulse({vdd:.4g} {vdd - vg:.4g} 20u 1u 1u 1 2)"}, "probes": ["S_POUT"]}
    else:
        args = {"refs": [q, r], "sources": {"S_MVDD": vdd, "S_MG": f"pulse(0 {vg:.4g} 20u 1u 1u 1 2)"}, "probes": ["S_MD"]}
    err, t = s.sim(dict(args, tran={"stop": "60u", "step": "0.1u", "window": "20u"}))
    if err:
        return "error: " + t.splitlines()[0][:100], trips
    if f"\n  {q} " not in t:
        return "no model: " + left_out(t)[:100], trips
    model_checks(t, "switch", trips)
    o = tran(t, "S_POUT" if pch else "S_MD")
    i = vdd / 10
    if o and not pch:
        judge(trips, "switch", o["max"] > vdd * 0.99 and o["max"] < vdd * 1.01 + 0.05, "off: the drain at Vdd (the gate edge's kick through Crss aside)")
        judge(trips, "switch", 0 <= o["avg"] < max(4 * i * rds, 0.02), "on: Vds under 4 x I x Rds(on)")
    if o and pch:
        judge(trips, "switch", o["min"] > -0.02 * vdd - 0.05 and o["min"] < 0.01, "off: no output (the gate edge's kick through Crss aside)")
        judge(trips, "switch", vdd - max(4 * i * rds, 0.02) < o["avg"] <= vdd + 0.001, "on: the drop under 4 x I x Rds(on)")
    return "", trips


def run_diode(s, refs, p):
    d, r = refs
    vr = p["params"].get("vr", 50)
    trips = []
    err, t = s.sim({"refs": [d, r], "sources": {"S_DIN": 5}, "probes": ["S_DOUT"]})
    if err:
        return "error: " + t.splitlines()[0][:100], trips
    if f"\n  {d} " not in t:
        return "no model: " + left_out(t)[:100], trips
    model_checks(t, "forward", trips)
    v = op_v(t, "S_DOUT")
    judge(trips, "forward", v is not None and 3.5 < v < 4.95, "forward drop 0.05 - 1.5 V at ~4 mA")
    err, t = s.sim({"refs": [d, r], "sources": {"S_DIN": -min(vr * 0.5, 50)}, "probes": ["S_DOUT"]})
    if not err:
        model_checks(t, "reverse", trips)
        v = op_v(t, "S_DOUT")
        judge(trips, "reverse", v is not None and abs(v) < 0.1, "reverse: under 100 µA into 1 kΩ")
    return "", trips


def run_zener(s, refs, p):
    d, r = refs
    vz = p["params"].get("vz", 5)
    trips = []
    err, t = s.sim({"refs": [d, r], "sources": {"S_ZIN": vz + 5}, "probes": ["S_ZK"]})
    if err:
        return "error: " + t.splitlines()[0][:100], trips
    if f"\n  {d} " not in t:
        return "no model: " + left_out(t)[:100], trips
    model_checks(t, "breakdown", trips)
    v = op_v(t, "S_ZK")
    judge(trips, "breakdown", v is not None and 0.85 * vz < v < 1.25 * vz + 0.3, "at about Vz with 5 mA")
    err, t = s.sim({"refs": [d, r], "sources": {"S_ZIN": -5}, "probes": ["S_ZK"]})
    if not err:
        model_checks(t, "forward", trips)
        v = op_v(t, "S_ZK")
        judge(trips, "forward", v is not None and -1.1 < v < -0.4, "forward drop 0.4 - 1.1 V")
    return "", trips


def run_cap(s, refs, p):
    r, c = refs
    trips = []
    cval = p["value"] or 1e-6
    vr = p["voltage"] or 10
    v = min(vr * 0.5, 12)
    rr = max(min(1e-3 / cval, 1e6), 0.1)   # tau 1 ms
    tau = rr * cval
    err, t = s.sim({"refs": [r, c], "models": {r: {"r": rr}}, "sources": {"S_CIN": f"pulse(0 {v:.4g} {tau:.4g} 1n 1n 1 2)"},
                    "tran": {"stop": f"{tau * 8:.4g}", "step": f"{tau / 500:.4g}", "window": f"{tau:.4g}"}, "probes": ["S_COUT"]})
    if err:
        return "error: " + t.splitlines()[0][:100], trips
    if f"\n  {c} " not in t:
        return "no model: " + left_out(t)[:100], trips
    model_checks(t, "charge", trips)
    o = tran(t, "S_COUT")
    judge(trips, "charge", o and abs(o["avg"] - v) < 0.03 * v and o["max"] < v * 1.01 and o["min"] > -0.01 * v, "charges to the step, no overshoot")
    return "", trips


def run_l(s, refs, p, bead):
    r, l, c = refs
    trips = []
    if bead:
        err, t = s.sim({"refs": [l, r], "sources": {"S_FIN": 1}, "probes": ["S_FOUT"]})
        if err:
            return "error: " + t.splitlines()[0][:100], trips
        if f"\n  {l} " not in t:
            return "no model: " + left_out(t)[:100], trips
        model_checks(t, "dc", trips)
        v = op_v(t, "S_FOUT")
        judge(trips, "dc", v is not None and 0.5 < v <= 1.0, "DC drop under half the volt into 1 Ω")
        return "", trips
    lval = p["params"].get("l", 1e-5)
    err, t = s.sim({"refs": [r, l, c], "models": {c: {"c": "10u", "esr": "5m", "esl": "0.5n", "dc_bias": False}, r: {"r": 1}},
                    "sources": {"S_LIN": "pulse(0 5 0.1m 1n 1n 1 2)"},
                    "tran": {"stop": f"{max(0.1e-3 + 40 * (lval * 10e-6) ** 0.5, 1e-3):.4g}", "step": f"{(lval * 10e-6) ** 0.5 / 50:.4g}"},
                    "probes": ["S_LOUT2"]})
    if err:
        return "error: " + t.splitlines()[0][:100], trips
    if f"\n  {l} " not in t:
        return "no model: " + left_out(t)[:100], trips
    model_checks(t, "step", trips)
    o = tran(t, "S_LOUT2")
    judge(trips, "step", o and o["max"] < 10.0 and o["min"] > -5.0, "rings no higher than twice the step")
    return "", trips


def run_r(s, refs, p):
    r1, r2 = refs
    trips = []
    err, t = s.sim({"refs": [r1, r2], "models": {r2: {"r": p["value"]}}, "sources": {"S_RIN": 1}, "probes": ["S_RMID"]})
    if err:
        return "error: " + t.splitlines()[0][:100], trips
    if f"\n  {r1} " not in t:
        return "no model: " + left_out(t)[:100], trips
    model_checks(t, "divider", trips)
    v = op_v(t, "S_RMID")
    judge(trips, "divider", v is not None and abs(v - 0.5) < 0.01, "a divider of two equal values: half")
    return "", trips


def main():
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", required=True)
    ap.add_argument("--kicad", required=True)
    ap.add_argument("--ngspice", required=True)
    ap.add_argument("--index", required=True)
    ap.add_argument("--per-class", type=int, default=100)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--classes", default=",".join(CLASSES))
    ap.add_argument("--out", default=os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "run", "survey"))
    ap.add_argument("--work", default="")
    ap.add_argument("--compare", default="")
    ap.add_argument("--verbose", action="store_true", help="print the answers of each part that trips")
    a = ap.parse_args()
    a.per_class = max(1, min(a.per_class, 500))
    stamp = datetime.date.today().isoformat()
    outdir = os.path.join(os.path.abspath(a.out), stamp)
    os.makedirs(outdir, exist_ok=True)
    work = os.path.abspath(a.work or os.path.join(os.path.dirname(os.path.abspath(a.out)), "svw"))
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(work)
    if a.index.endswith(".gz"):
        with gzip.open(a.index) as src, open(os.path.join(work, "parts.db"), "wb") as dst:
            shutil.copyfileobj(src, dst)
        a.index = os.path.join(work, "parts.db")
    db = sqlite3.connect(a.index)
    if not db.execute("SELECT name FROM sqlite_master WHERE name='params'").fetchone():
        sys.exit("the index has no params tables")
    meta = dict(db.execute("SELECT key, value FROM meta").fetchall())
    classes = [c for c in a.classes.split(",") if c in CLASSES]

    proj = make_project(a.kicad, work)
    sock = os.path.join(work, "s.sock")
    _, kicadd_mcp, env = binaries(os.path.abspath(a.build))
    env = dict(env, KICADD_LCSC_OFFLINE="1", KICADD_NGSPICE=os.path.abspath(a.ngspice), KICADD_PARTS_DB=os.path.abspath(a.index))
    server = Server(a.build, sock, os.path.join(proj, "blank.kicad_pcb"), env, os.path.join(work, "server.log"))
    if not server.start():
        sys.exit("api-server did not start")
    mcp = Mcp([kicadd_mcp, "--socket", sock, "--stdio"], env)
    s = Survey(mcp)
    report = {"date": stamp, "seed": a.seed, "per_class": a.per_class, "index": meta, "classes": {}}
    t0 = time.time()
    try:
        def sym(name, pins):
            mcp.tool("lib_create_symbol", {"name": name, "value": name, "pins": [{"number": n, "name": nm, "type": ty} for n, nm, ty in pins]})

        sym("SV_LDO", [("1", "VIN", "power_in"), ("2", "GND", "power_in"), ("3", "VOUT", "power_out")])
        sym("SV_OPA", [("1", "OUT", "output"), ("2", "-IN", "input"), ("3", "+IN", "input"), ("4", "V-", "power_in"), ("8", "V+", "power_in")])
        sym("SV_FET", [("1", "G", "input"), ("2", "S", "passive"), ("3", "D", "passive")])
        sym("SV_DIO", [("1", "K", "passive"), ("2", "A", "passive")])
        # one standard circuit per class; each sampled part goes into it through its LCSC field
        C = {}
        u = s.part("kicadd:SV_LDO", "LDO"); c = s.part("Device:C", "10uF"); C["ldo"] = (u, c)
        nets = {"S_LVIN": [f"{u}.1"], "S_LOUT": [f"{u}.3", f"{c}.1"], "GND": [f"{u}.2", f"{c}.2"]}
        u = s.part("kicadd:SV_OPA", "OPAMP"); C["opamp"] = (u,)
        nets.update({"S_AVCC": [f"{u}.8"], "S_AIN": [f"{u}.3"], "S_AOUT": [f"{u}.1", f"{u}.2"]}); nets["GND"].append(f"{u}.4")
        q = s.part("kicadd:SV_FET", "NMOS"); r = s.part("Device:R", "10"); C["nmos"] = (q, r)
        nets.update({"S_MVDD": [f"{r}.1"], "S_MD": [f"{r}.2", f"{q}.3"], "S_MG": [f"{q}.1"]}); nets["GND"].append(f"{q}.2")
        q = s.part("kicadd:SV_FET", "PMOS"); r = s.part("Device:R", "10"); C["pmos"] = (q, r)
        nets.update({"S_PVIN": [f"{q}.2"], "S_PG": [f"{q}.1"], "S_POUT": [f"{q}.3", f"{r}.1"]}); nets["GND"].append(f"{r}.2")
        d = s.part("kicadd:SV_DIO", "D"); r = s.part("Device:R", "1k"); C["diode"] = C["schottky"] = (d, r)
        nets.update({"S_DIN": [f"{d}.2"], "S_DOUT": [f"{d}.1", f"{r}.1"]}); nets["GND"].append(f"{r}.2")
        d = s.part("kicadd:SV_DIO", "Z"); r = s.part("Device:R", "1k"); C["zener"] = (d, r)
        nets.update({"S_ZIN": [f"{r}.1"], "S_ZK": [f"{r}.2", f"{d}.1"]}); nets["GND"].append(f"{d}.2")
        r = s.part("Device:R", "1k"); c = s.part("Device:C", "1uF"); C["mlcc"] = C["alu"] = C["tantalum"] = C["polymer"] = (r, c)
        nets.update({"S_CIN": [f"{r}.1"], "S_COUT": [f"{r}.2", f"{c}.1"]}); nets["GND"].append(f"{c}.2")
        r = s.part("Device:R", "1"); l = s.part("Device:L", "10uH"); c = s.part("Device:C", "10uF"); C["inductor"] = (r, l, c)
        nets.update({"S_LIN": [f"{r}.1"], "S_LM": [f"{r}.2", f"{l}.1"], "S_LOUT2": [f"{l}.2", f"{c}.1"]}); nets["GND"].append(f"{c}.2")
        fb = s.part("Device:L", "FB"); r = s.part("Device:R", "1"); C["bead"] = (r, fb, None)
        nets.update({"S_FIN": [f"{fb}.1"], "S_FOUT": [f"{fb}.2", f"{r}.1"]}); nets["GND"].append(f"{r}.2")
        r1 = s.part("Device:R", "R"); r2 = s.part("Device:R", "R"); C["resistor"] = (r1, r2)
        nets.update({"S_RIN": [f"{r1}.1"], "S_RMID": [f"{r1}.2", f"{r2}.1"]}); nets["GND"].append(f"{r2}.2")
        err, t = mcp.tool("sch_connect", {"nets": nets, "power": ["GND"]})
        if err:
            sys.exit("wiring: " + t)

        for cls in classes:
            parts = sample(db, cls, a.per_class, a.seed)
            refs = C[cls]
            target = refs[1] if cls in ("mlcc", "alu", "tantalum", "polymer", "inductor", "bead") else refs[0]
            st = {"sampled": len(parts), "modelled": 0, "skipped": collections.Counter(), "trips": collections.Counter(),
                  "tripped_parts": 0, "examples": collections.defaultdict(list)}
            print(f"[{cls}] {len(parts)} part(s)", flush=True)
            for p in parts:
                mcp.tool("sch_set_fields", {"parts": {target: {"LCSC": p["lcsc"], "Value": p["mfr"] or p["lcsc"]}}})
                s.last = []
                if cls == "ldo":
                    why, trips = run_ldo(s, refs, p)
                elif cls == "opamp":
                    why, trips = run_opamp(s, refs, p)
                elif cls in ("nmos", "pmos"):
                    why, trips = run_fet(s, refs, p, cls == "pmos")
                elif cls in ("diode", "schottky"):
                    why, trips = run_diode(s, refs, p)
                elif cls == "zener":
                    why, trips = run_zener(s, refs, p)
                elif cls in ("mlcc", "alu", "tantalum", "polymer"):
                    why, trips = run_cap(s, refs, p)
                elif cls == "inductor":
                    why, trips = run_l(s, refs, p, False)
                elif cls == "bead":
                    why, trips = run_l(s, refs, p, True)
                else:
                    why, trips = run_r(s, refs, p)
                if why:
                    st["skipped"][re.sub(r"\b(?:[A-Z]+\d+|C\d{3,})\b", "X", why)[:100]] += 1
                    continue
                st["modelled"] += 1
                if trips:
                    st["tripped_parts"] += 1
                    if a.verbose:
                        print(f"  {p['lcsc']} {p['mfr']}: {trips}\n    " + "\n    ".join(x.strip().replace("\n", "\n    ") for x in s.last))
                for scen, what in sorted(set(trips)):
                    key = f"{scen}: {what}"
                    st["trips"][key] += 1
                    if len(st["examples"][key]) < 3:
                        st["examples"][key].append({"lcsc": p["lcsc"], "mfr": p["mfr"], "desc": p["desc"]})
            report["classes"][cls] = {k: (dict(v) if isinstance(v, (collections.Counter, collections.defaultdict)) else v) for k, v in st.items()}
            print(f"       modelled {st['modelled']}, tripped {st['tripped_parts']}, skipped {sum(st['skipped'].values())}", flush=True)
    finally:
        mcp.close()
        server.stop()
    report["seconds"] = round(time.time() - t0, 1)

    # the earlier survey to compare with
    prev = None
    cmp_dir = a.compare
    if not cmp_dir:
        earlier = sorted(d for d in os.listdir(os.path.abspath(a.out)) if d < stamp and os.path.isfile(os.path.join(a.out, d, "survey.json")))
        cmp_dir = os.path.join(a.out, earlier[-1]) if earlier else ""
    if cmp_dir and os.path.isfile(os.path.join(cmp_dir, "survey.json")):
        prev = json.load(open(os.path.join(cmp_dir, "survey.json"), encoding="utf-8"))

    json.dump(report, open(os.path.join(outdir, "survey.json"), "w", encoding="utf-8"), indent=1, ensure_ascii=False)
    md = [f"# Model survey {stamp}", "", f"seed {a.seed}, {a.per_class} per class, index {meta.get('built', meta.get('date', '?'))}, "
          f"{report['seconds']} s" + (f"; compared with {os.path.basename(cmp_dir)}" if prev else ""), ""]
    rises = []
    md += ["| class | sampled | modelled | tripped | rate | before |", "|---|---|---|---|---|---|"]
    for cls, st in report["classes"].items():
        rate = st["tripped_parts"] / st["modelled"] if st["modelled"] else 0
        before = ""
        if prev and cls in prev["classes"] and prev["classes"][cls]["modelled"]:
            pr = prev["classes"][cls]["tripped_parts"] / prev["classes"][cls]["modelled"]
            before = f"{pr:.0%}"
            if rate > pr + 0.02:
                rises.append(f"{cls}: {pr:.0%} -> {rate:.0%}")
        md.append(f"| {cls} | {st['sampled']} | {st['modelled']} | {st['tripped_parts']} | {rate:.0%} | {before} |")
    if rises:
        md[4:4] = ["**Rates up since the last survey (look first):** " + "; ".join(rises), ""]
    for cls, st in report["classes"].items():
        md += ["", f"## {cls}", ""]
        if st["skipped"]:
            md.append("left out: " + "; ".join(f"{k} ({n})" for k, n in sorted(st["skipped"].items(), key=lambda kv: -kv[1])))
            md.append("")
        for key, n in sorted(st["trips"].items(), key=lambda kv: -kv[1]):
            ex = "; ".join(f"{e['lcsc']} {e['mfr']}" for e in st["examples"][key])
            md.append(f"- {n} / {st['modelled']} ({n / max(st['modelled'], 1):.0%}) {key} -- e.g. {ex}")
        if not st["trips"]:
            md.append("no check tripped")
    open(os.path.join(outdir, "survey.md"), "w", encoding="utf-8").write("\n".join(md) + "\n")
    print("\n".join(md[:30]))
    print("\nwritten:", os.path.join(outdir, "survey.md"))


if __name__ == "__main__":
    main()

"""PDN impedance (pdn) and reflections (reflection), headless, on the demos as they are. Measures
only; the numbers checked against hand arithmetic.

usage: python3 -I si_e2e.py --build BUILD --demos DEMOS_DIR --work DIR --ngspice NGSPICE

  pdn         vme-wren P0V85 / P1V8 (values written as part names, CC0402_100NF_16V_10%_X7R):
              the plane pair and its ε A / d; each kind of capacitor's SRF is 1/(2π√(L C_eff)) of
              its own numbers; |Z| at 10 kHz the regulator in parallel with all the C_eff; the
              anti-resonances named by the capacitors on each side; target bands; vrm, models
              (mount, exclude), add; a PNG; bad arguments refused
  reflection  vme-wren /VMEF.SYSRESET_N (IC9 output, 142 mm on In2.Cu): the open end's peak is
              2 vdd Z0 / (Z0 + rout), the suggested series resistor (Z0 - rout, E24) takes it
              away, the flight time the line's delay; Net-(IC55-SER) with rs; /PP_OUT2 driven
              through R337 from Net-(IC124-1Y) (the series resistor found, the driver behind it);
              two drivers / no tracks / a driver not on the net refused; a PNG
"""

import argparse
import math
import os
import re
import shutil
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from m5_e2e import FAILS, Mcp, Server, binaries, check  # noqa: E402

UNIT = {"G": 1e9, "M": 1e6, "k": 1e3, "": 1, "m": 1e-3, "µ": 1e-6, "n": 1e-9, "p": 1e-12, "f": 1e-15}


def si(text):
    m = re.match(r"\s*([-\d.e+]+) ?([GMkmµnpf]?)", text)
    return float(m.group(1)) * UNIT[m.group(2)] if m else None


def board(a, work, demo, fname):
    dst = os.path.join(work, demo[:6])
    shutil.copytree(os.path.join(a.demos, demo), dst)
    sock = dst + ".sock"
    _, kicadd_mcp, env = binaries(a.build)
    env = dict(env, KICADD_NGSPICE=os.path.abspath(a.ngspice), KICADD_LCSC_OFFLINE="1")
    srv = Server(a.build, sock, os.path.join(dst, fname), env, dst + ".log")
    check(srv.start(), "api-server up with " + fname)
    return srv, Mcp([kicadd_mcp, "--socket", sock, "--stdio"], env)


def groups(t):
    """{key: (count, C_eff, L body, L mount (max), SRF)} from pdn's capacitor lines"""
    out = {}
    for m in re.finditer(r"\n  (\d+)x (.+?): C_eff ([\d.]+ [µnmp]?)F.*?, L ([\d.]+ [µnmp]?)H \+ (?:[\d.]+ [µnmp]?H\.\.)?([\d.]+ [µnmp]?)H.*?, SRF ([\d.]+ [kMG]?)Hz", t):
        out[m.group(2)] = (int(m.group(1)), si(m.group(3)), si(m.group(4)), si(m.group(5)), si(m.group(6)))
    return out


def main():
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", required=True)
    ap.add_argument("--demos", required=True)
    ap.add_argument("--work", required=True)
    ap.add_argument("--ngspice", required=True)
    a = ap.parse_args()
    a.build, a.demos = os.path.abspath(a.build), os.path.abspath(a.demos)
    work = os.path.abspath(a.work)
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(work)

    srv, m = board(a, work, "vme-wren", "vme-wren.kicad_pcb")
    try:
        print("[pdn]")
        err, t = m.tool("pdn", {"net": "P0V85", "vrm": {"r": "1m", "l": "5n"}})
        print("      ", "\n       ".join(t.splitlines()[:8]))
        check(not err and re.search(r"P0V85 at IC14 .* to GND, 10 kHz - 1 GHz: 17 capacitor\(s\); 0\.85 V \(from its name\)", t),
              "P0V85: IC14 the load (most pads), GND, 17 capacitors, 0.85 V from the name")
        pm = re.search(r"planes: (\S+) over (\S+), ([\d.e+]+) mm² at ([\d.]+) mm Er ([\d.]+): ([\d.]+ [µnp]?)F", t)
        if pm:
            c = 8.854e-12 * float(pm.group(5)) * float(pm.group(3)) * 1e-6 / (float(pm.group(4)) * 1e-3)
            check(abs(si(pm.group(6)) - c) / c < 0.02, f"plane pair {pm.group(1)}/{pm.group(2)}: {pm.group(6)}F = ε0 Er A / d ({c * 1e12:.0f} pF)")
        else:
            check(False, "the plane pair line")
        g = groups(t)
        check(len(g) >= 5, f"the capacitors by kind: {len(g)} kinds")
        bad = []
        for k, (n, ceff, lb, lm, srf) in g.items():
            if n == 1:
                want = 1 / (2 * math.pi * math.sqrt((lb + lm) * ceff))
                if abs(srf - want) / want > 0.03:
                    bad.append(f"{k}: {srf:.3g} vs {want:.3g}")
        check(not bad, "each single capacitor's SRF is 1/(2π√(L C_eff)) of its own numbers " + "; ".join(bad))
        check("CC0402_10UF" not in t.split("capacitors (by SRF")[1].split("\n")[1] and re.search(r"\dx 10 µF MLCC 0402 X5R", t),
              "values written as part names read (CC0402_10UF_6.3V_20%_X5R -> 10 µF 0402 X5R)")
        z10 = re.search(r"\|Z\|: ([\d.]+ [µm]?)Ω at 10 kHz", t)
        ctot = sum(n * c for n, c, *_ in g.values())
        w = 2 * math.pi * 1e4
        zv = complex(1e-3, w * 5e-9)
        zc = 1 / complex(0, w * ctot)
        want = abs(zv * zc / (zv + zc))
        check(z10 and abs(si(z10.group(1)) - want) / want < 0.1,
              f"|Z| at 10 kHz {z10 and z10.group(1)}Ω ≈ the regulator ∥ ΣC_eff ({want * 1e3:.2f} mΩ)")
        check("regulator: 1 mΩ + 5 nH (vrm)" in t, "vrm given: said")
        check(re.search(r"est\. DC bias \d+% at 0\.85 V", t), "MLCC DC bias at the rail's voltage, said est.")
        check("peaks (anti-resonances" in t and re.search(r"below every SRF: the regulator's L with", t), "the regulator / bulk anti-resonance named")

        err, t = m.tool("pdn", {"net": "P1V8", "target": 0.03, "plot": True})
        err2, t2, imgs = m.tool_images("pdn", {"net": "P1V8", "target": 0.03, "plot": True})
        print("      ", "\n       ".join(ln for ln in t.splitlines() if ln.startswith("  ") and "Ω at" in ln)[:600])
        check(not err and re.search(r"between 1 µF MLCC 0402 X5R \(SRF [\d.]+ MHz\) and 100 nF MLCC 0402 X7R \(SRF [\d.]+ MHz\)", t),
              "P1V8: a peak between the 1 µF and 100 nF capacitors, named")
        check(re.search(r"target 30 mΩ: \d+ band\(s\) above it\n  [\d.]+ [kMG]?Hz - ", t), "target: the bands above it")
        check("regulator: 2 mΩ + 20 nH (assumed" in t, "no vrm: 2 mΩ + 20 nH assumed, said")
        check(len(imgs) == 1 and imgs[0][:4] == b"\x89PNG", "plot: a PNG")
        if imgs:
            open(os.path.join(work, "pdn.png"), "wb").write(imgs[0])
        err, t = m.tool("pdn", {"net": "P1V8", "target": 100})
        check("|Z| under it over the whole range" in t, "a loose target: under it throughout, said")

        err, t = m.tool("pdn", {"net": "P0V85", "models": {"C272": {"mount": "0"}}})
        g2 = groups(t)
        k22 = next((k for k in g2 if k.startswith("22 µF")), None)
        check(k22 and g2[k22][3] < 1e-12 and "(models)" in t, f"models mount 0: C272's mounting gone ({k22 and g2[k22][3]})")
        err, t0 = m.tool("pdn", {"net": "P0V85"})
        ex = [r for r in ("C33", "C273")]
        err, t = m.tool("pdn", {"net": "P0V85", "models": {r: {"exclude": True} for r in ex}})
        low = lambda x: si(re.search(r"lowest ([\d.]+ [µm]?)Ω", x).group(1))  # noqa: E731
        lo0, lo1 = low(t0), low(t)
        check(re.search(r"\nleft out: (C33, C273|C273, C33)\n", t) and lo1 > lo0, f"the bulk capacitors left out: the lowest |Z| {lo0 * 1e3:.3f} -> {lo1 * 1e3:.3f} mΩ")
        err, t = m.tool("pdn", {"net": "P0V85", "add": [{"c": "10u", "package": "0402", "count": 4}], "detail": True})
        check(re.search(r"\n  4x 10 µF MLCC 0402 X5R \(added\)", t) and "added: the board's median" in t, "add: four 10 µF, the board's median mounting")

        for args, want in (({"net": "NOPE"}, "no net NOPE"), ({"net": "P0V85", "at": "U999"}, "at: no part U999"),
                           ({"net": "P0V85", "models": {"R1": {"c": 1}}}, "is not one of P0V85's capacitors"),
                           ({"net": "P0V85", "from": "1M", "to": "1k"}, "from < to")):
            err, t = m.tool("pdn", args)
            check(err and want in t, f"refused: {want}")

        print("[reflection]")
        err, t, imgs = m.tool_images("reflection", {"net": "/VMEF.SYSRESET_N", "rise": "0.5n", "plot": True})
        print("      ", "\n       ".join(t.splitlines()[:9]))
        z = re.search(r"In2\.Cu [\d.]+ mm ([\d.]+) Ω", t)
        z0 = float(z.group(1)) if z else 50
        pk = re.search(r"as routed \(no series resistor\):\n  IC14\.W13 \(bidirectional\): final ([\d.]+) V, peak ([\d.]+) V", t)
        want = 2 * 3.3 * z0 / (z0 + 30)
        check(pk and abs(float(pk.group(2)) - want) / want < 0.04, f"open end: peak {pk and pk.group(2)} V = 2 vdd Z0/(Z0+rout) ({want:.2f} V, Z0 {z0})")
        rs = re.search(r"with (\d+) Ω in series at IC9\.3 \(Z0 ([\d.]+) Ω - rout 30 Ω, E24\):\n  IC14\.W13 .*?\(overshoot ([\d.e+-]+)%\)", t)
        check(rs and abs(int(rs.group(1)) - (float(rs.group(2)) - 30)) <= 2 and float(rs.group(3)) < 2,
              f"suggested series {rs and rs.group(1)} Ω (Z0 - rout, E24): overshoot {rs and rs.group(3)}%")
        td = re.search(r"the farthest receiver ([\d.]+) ps one way", t)
        fl = re.search(r"\(94\.\d%\), flight ([\d.]+ [pn]?)s", t) or re.search(r"flight ([\d.]+ [pn]?)s", t)
        check(td and fl and abs(si(fl.group(1)) - float(td.group(1)) * 1e-12) < 0.25e-9,
              f"flight {fl and fl.group(1)}s ≈ the line's delay {td and td.group(1)} ps")
        check("assumed: rout 30 Ω, vdd 3.3 V, cin 3 pF" in t, "the driver's numbers assumed, said")
        check(len(imgs) == 1 and imgs[0][:4] == b"\x89PNG", "plot: a PNG")
        if imgs:
            open(os.path.join(work, "reflection.png"), "wb").write(imgs[0])

        # a slow edge: the short stretches become capacitances, the delay to the receivers stays;
        # the net named without the sheet's "/" (as a schematic tool or a board from elsewhere may)
        err, t = m.tool("reflection", {"net": "VMEF.SYSRESET_N", "rise": "20n"})
        td2 = re.search(r"the farthest receiver ([\d.]+ [pn]?)s one way", t)
        check(not err and td2 and td and abs(si(td2.group(1)) - float(td.group(1)) * 1e-12) < 2e-12,
              f"rise 20 ns, the lines lumped: the farthest receiver still {td2 and td2.group(1)}s (not 0 s); the net without its '/'")

        err, t = m.tool("reflection", {"net": "Net-(IC55-SER)", "rs": [47], "rout": 20, "vdd": 3.3, "cin": "2p", "rise": "1n"})
        check(not err and "with 47 Ω in series (rs):" in t and "assumed" not in t, "rs tried; nothing assumed when all given")

        err, t = m.tool("reflection", {"net": "/PP_OUT2", "rise": "2n"})
        print("      ", t.splitlines()[0][:200])
        check(not err and re.search(r"driver IC124\.\d+ \((output|tri_state)\) on Net-\(IC124-1Y\) through R337 [\d.]+ [kΩ]", t),
              "through a series resistor: R337 found, the driver IC124 behind it")
        check(re.search(r"as routed \(R337 [\d.]+ [kΩ]+\)", t), "as routed: with R337's own value")

        for args, want in (({"net": "/DDR4-PS.ALERTN"}, "more than one driver"),
                           ({"net": "/VMEF.SYSRESET_N", "driver": "U999"}, "driver U999 is not on"),
                           ({"net": "NOPE"}, "no net NOPE")):
            err, t = m.tool("reflection", args)
            check(err and want in t, f"refused: {want}")
    finally:
        m.close()
        srv.stop()

    print("\nRESULT: " + ("PASS" if not FAILS else "FAIL (%d)" % len(FAILS)))
    sys.exit(1 if FAILS else 0)


if __name__ == "__main__":
    main()

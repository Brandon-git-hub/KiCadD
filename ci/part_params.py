"""The second-order electrical parameters KiCadD keeps from jlcparts' attributes (ROADMAP phase 3,
item 1), shared by build_parts_index.py (the index's params table) and param_coverage.py.

Each parameter: an id (what lcsc_search's `where` and output use), the attribute keys it is read
from, and how: the number in its unit (SI; slew in V/us), and the test condition after '@'
("12mΩ@10V" -> 0.012, "10V"). A value that does not read as a number is left out, never guessed.
"""
import re

SI = {"f": 1e-15, "p": 1e-12, "n": 1e-9, "u": 1e-6, "µ": 1e-6, "μ": 1e-6, "m": 1e-3, "": 1.0,
      "k": 1e3, "K": 1e3, "M": 1e6, "G": 1e9}
NUM = r"([-+±]?\d+(?:\.\d+)?)\s*([fpnuµμmkKMG]?)\s*"


def _cond(text):
    i = text.find("@")
    return text[i + 1:].strip().strip("()") if i >= 0 else ""


def number(unit, scale=1.0, last=False):
    """a reader: the first (last=True: the last, for 'a~b' ranges) '<num><prefix><unit>' before
    any '@', times scale -> (value, condition) or None"""
    rx = re.compile(NUM + r"(?:" + unit + r")(?![a-qs-zA-Z])")

    def read(text):
        t = text.replace(",", "")
        head = t.split("@", 1)[0]
        found = [m for m in rx.finditer(head) if SI.get(m.group(2)) is not None]
        if not found:
            return None
        m = found[-1] if last else found[0]
        return float(m.group(1).lstrip("±+")) * SI[m.group(2)] * scale, _cond(t)
    return read


def first_of(*readers):
    def read(text):
        for r in readers:
            v = r(text)
            if v is not None:
                return v
        return None
    return read


def flag(word):
    """1 when the text names it ('Rail-to-Rail Input, Rail-to-Rail Output' -> input / output)"""
    rx = re.compile(word, re.I)
    return lambda text: (1.0, "") if rx.search(text) else (0.0, "")


V, A, F, C, HZ, DB, W = (number(u) for u in ("V", "A", "F", "C", "Hz", "dB", "W"))
OHM = number("Ω|ohm|R")
S = number("s")
VMAX = number("V", last=True)
VMIN = number("V")
SLEW = first_of(number(r"V/[uµμ]s"), number(r"V/ms", 1e-3), number(r"V/ns", 1e3))   # V/us
EN = number(r"V/√Hz|V/rtHz|V/sqrt\(Hz\)|V")                                          # V/√Hz

# class -> (JLCPCB subcategories, [(id, label, unit shown, attribute keys, reader)])
CLASSES = {
    "mosfet": (["MOSFETs", "MOSFET", "Silicon Carbide Field Effect Transistor (MOSFET)"], [
        ("vds", "Vds", "V", ["Drain to Source Voltage"], V),
        ("id", "Id", "A", ["Current - Continuous Drain(Id)"], A),
        ("rds_on", "Rds(on)", "Ω", ["RDS(on)"], OHM),
        ("vgs_th", "Vgs(th)", "V", ["Gate Threshold Voltage (Vgs(th))"], V),
        ("qg", "Qg", "C", ["Gate Charge(Qg)"], C),
        ("ciss", "Ciss", "F", ["Input Capacitance(Ciss)", "Ciss-Input Capacitance"], F),
        ("coss", "Coss", "F", ["Output Capacitance(Coss)"], F),
        ("crss", "Crss", "F", ["Reverse Transfer Capacitance (Crss@Vds)", "Reverse Transfer Capacitance (Crss)"], F),
    ]),
    "ldo": (["Voltage Regulators - Linear, Low Drop Out (LDO) Regulators", "Linear Voltage Regulators (LDO)",
             "Dropout Regulators(LDO)", "Low Dropout Regulators(LDO)", "Linear Voltage Regulators"], [
        ("vin_max", "Vin max", "V", ["Voltage - Supply"], VMAX),
        ("vout", "Vout", "V", ["Output Voltage"], V),
        ("iout", "Iout", "A", ["Output Current"], A),
        ("dropout", "Dropout", "V", ["Voltage Dropout"], V),
        ("iq", "Iq", "A", ["standby current", "Quiescent Current"], A),
        ("psrr", "PSRR", "dB", ["Power Supply Rejection Ratio (PSRR)"], DB),
        ("noise", "Noise", "Vrms", ["Noise"], V),
    ]),
    "opamp": (["Operational Amplifier", "Precision Op Amps", "FET Input Amplifiers", "Precision OpAmps",
               "FET InputAmplifiers", "General Purpose Amplifiers", "Low Noise OpAmps"], [
        ("gbw", "GBW", "Hz", ["Gain Bandwidth Product"], HZ),
        ("slew", "Slew", "V/us", ["Slew Rate"], SLEW),
        ("vos", "Vos", "V", ["Vos - Input Offset Voltage"], V),
        ("ib", "Ib", "A", ["Ib - Input Bias Current"], A),
        ("cmrr", "CMRR", "dB", ["Common Mode Rejection Ratio(CMRR)", "Common Mode Rejection Ratio (CMRR)"], DB),
        ("iout", "Iout", "A", ["Output Current"], A),
        ("iq", "Iq", "A", ["Quiescent Current", "Quiescent Current(Iq)"], A),
        ("en", "en", "V/√Hz", ["Input Voltage Noise Density", "Noise density(eN)"], EN),
        ("supply_min", "Supply min", "V", ["Single supply", "Single Supply"], VMIN),
        ("supply_max", "Supply max", "V", ["Single supply", "Single Supply"], VMAX),
        ("rr_in", "RR in", "", ["Rail to Rail", "Rail To Rail"], flag(r"input")),
        ("rr_out", "RR out", "", ["Rail to Rail", "Rail To Rail"], flag(r"output")),
    ]),
    "inductor": (["Power Inductors", "Inductors (SMD)", "Color Ring Inductors / Through Hole Inductors",
                  "Color Ring Inductors/Through Hole Inductors"], [
        ("l", "L", "H", ["Inductance"], number("H")),
        ("dcr", "DCR", "Ω", ["DC Resistance(DCR)"], OHM),
        ("i_rated", "Irated", "A", ["Current Rating"], A),
        ("i_sat", "Isat", "A", ["Current - Saturation(Isat)", "Current - Saturation (Isat)"], A),
        ("srf", "SRF", "Hz", ["Frequency - Self Resonant"], HZ),
    ]),
    "diode": (["Schottky Diodes", "Schottky Barrier Diodes (SBD)", "Fast Recovery / High Efficiency Diodes",
               "Diodes - General Purpose", "Diodes - Fast Recovery Rectifiers", "Fast Recovery/High Efficiency Diode",
               "Switching Diodes", "Switching Diode", "SiC Diodes"], [
        ("vr", "Vr", "V", ["Voltage - DC Reverse(Vr)"], V),
        ("if", "If", "A", ["Current - Rectified"], A),
        ("vf", "Vf", "V", ["Voltage - Forward(Vf@If)"], V),
        ("ir", "Ir", "A", ["Reverse Leakage Current (Ir)"], A),
        ("trr", "trr", "s", ["Reverse Recovery Time (trr)"], S),
    ]),
    "zener": (["Zener Diodes"], [
        ("vz", "Vz", "V", ["Zener Voltage(Nom)"], V),
        ("zzt", "Zzt", "Ω", ["Impedance(Zzt)"], OHM),
        ("pd", "Pd", "W", ["Pd - Power Dissipation"], W),
        ("ir", "Ir", "A", ["Reverse Leakage Current (Ir)"], A),
    ]),
}

CLASS_OF = {sub: name for name, (subs, _) in CLASSES.items() for sub in subs}


def text(v):
    if isinstance(v, dict):
        v = v.get("values", v.get("value"))
    if isinstance(v, (list, tuple)):
        v = " ".join(str(x) for x in v if x is not None)
    return "" if v in (None, "-", "--") else str(v).strip()


def extract(subcategory, attrs):
    """[(id, value, condition)] of a part's parameters that read as numbers; [] for other classes"""
    cls = CLASS_OF.get(subcategory)
    if not cls:
        return []
    out = []
    for pid, _, _, keys, read in CLASSES[cls][1]:
        t = next((text(attrs[k]) for k in keys if k in attrs and text(attrs[k])), "")
        v = read(t) if t else None
        if v is not None:
            out.append((pid, v[0], v[1]))
    return out

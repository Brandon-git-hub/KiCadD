"""How much of jlcparts' data the phase-3 models could use (ROADMAP phase 3, its entry gate).

usage: python3 -I param_coverage.py CACHE_SQLITE3 [--md OUT.md]

CACHE_SQLITE3: jlcparts' cache.sqlite3 (the split zip ci/build_parts_index.py downloads, unzipped).
For each part class of part_params.py (diodes by kind) and MLCCs, each parameter: the share of
the parts in stock whose attribute is there, and whose value reads as a number in the model's
unit ("12mΩ@10V" -> 0.012 ohm) -- over all parts in stock and over JLC's basic / preferred ones
(what auto-selection prefers). Only the standard library; reads the file read-only.
"""
import argparse, collections, datetime, json, os, sqlite3, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import part_params as pp  # noqa: E402

_DIODE = pp.CLASSES["diode"][1]
MLCC = [
    ("c", "C", "F", ["Capacitance"], pp.number("F")),
    ("v", "Rated V", "V", ["Voltage Rating"], pp.V),
    ("tol", "Tolerance", "", ["Tolerance"], lambda t: (1.0, "") if "%" in t or "pF" in t else None),
    ("diel", "Dielectric", "", ["Temperature Coefficient"], lambda t: (1.0, "") if t else None),
    ("esr", "ESR", "Ω", ["Equivalent Series Resistance(ESR)", "ESR"], pp.OHM),
    ("bias", "DC bias", "F", ["DC Bias", "Capacitance @ DC Bias"], pp.number("F")),
]

# (title, subcategories, parameters)
GROUPS = [(n, subs, ps) for n, (subs, ps) in pp.CLASSES.items() if n != "diode"] + [
    ("diode: Schottky", ["Schottky Diodes", "Schottky Barrier Diodes (SBD)"], _DIODE),
    ("diode: rectifier / fast recovery", ["Fast Recovery / High Efficiency Diodes", "Diodes - General Purpose",
                                          "Diodes - Fast Recovery Rectifiers", "Fast Recovery/High Efficiency Diode"], _DIODE),
    ("diode: switching", ["Switching Diodes", "Switching Diode"], _DIODE),
    ("MLCC", ["Multilayer Ceramic Capacitors MLCC - SMD/SMT"], MLCC),
]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cache")
    ap.add_argument("--md")
    a = ap.parse_args()
    db = sqlite3.connect("file:" + a.cache + "?mode=ro", uri=True)
    newest = db.execute("select max(fetched_at) from jlc_components").fetchone()[0]
    when = datetime.datetime.fromtimestamp(newest, datetime.timezone.utc).strftime("%Y-%m-%d") if newest else "?"

    out = ["# Part parameter coverage (jlcparts)", "",
           f"jlcparts cache.sqlite3, parts fetched up to {when}, in stock. *Given*: the attribute is there; "
           "*usable*: it reads as a number in the model's unit (`ci/part_params.py`). Made by `ci/param_coverage.py`.", ""]
    for name, subs, params in GROUPS:
        q = "select library_type, preferred, attributes from jlc_components where stock > 0 and subcategory in (%s)" % ",".join("?" * len(subs))
        n = {"all": 0, "basic": 0}
        have = {"all": collections.Counter(), "basic": collections.Counter()}
        used = {"all": collections.Counter(), "basic": collections.Counter()}
        bad = collections.defaultdict(collections.Counter)
        for lib, pref, attrs in db.execute(q, subs):
            try:
                d = json.loads(attrs)
            except ValueError:
                d = {}
            groups = ["all"] + (["basic"] if lib == "base" or pref else [])
            for g in groups:
                n[g] += 1
            for pid, _, _, keys, read in params:
                t = next((pp.text(d[k]) for k in keys if k in d and pp.text(d[k])), "")
                ok = bool(t) and read(t) is not None
                for g in groups:
                    have[g][pid] += bool(t)
                    used[g][pid] += ok
                if t and not ok:
                    bad[pid][t] += 1
        if not n["all"]:
            continue
        pct = lambda c, g: f"{100 * c / n[g]:.0f}%" if n[g] else "-"
        out += [f"## {name}: {n['all']} in stock, {n['basic']} basic / preferred", "",
                "| parameter | id | given | usable | usable (basic / preferred) | unreadable, e.g. |",
                "|---|---|---|---|---|---|"]
        for pid, label, unit, _, _ in params:
            ex = ", ".join(f"`{t}`" for t, _ in bad[pid].most_common(2))
            out.append(f"| {label}{' (' + unit + ')' if unit else ''} | `{pid}` | {pct(have['all'][pid], 'all')} | "
                       f"{pct(used['all'][pid], 'all')} | {pct(used['basic'][pid], 'basic')} | {ex} |")
        out.append("")

    md = "\n".join(out)
    print(md)
    if a.md:
        open(a.md, "w").write(md + "\n")


if __name__ == "__main__":
    sys.exit(main())

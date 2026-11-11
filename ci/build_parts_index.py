"""Build KiCadD's parts index (lcsc_search) from jlcparts' daily data.

usage: python3 -I build_parts_index.py OUT_DIR [--source DIR_WITH_cache.zXX]

Downloads https://yaqwsx.github.io/jlcparts/data/cache.zip + cache.z01.. (a split zip of
jlcparts' cache.sqlite3, MIT, scraped daily from JLCPCB's catalogue), keeps the parts in stock,
turns their attributes into numbers (value + unit, voltage, tolerance, dielectric, power,
current, output voltage), adds a full-text index and writes OUT_DIR/parts.db.gz plus
OUT_DIR/parts-meta.json (date, counts, sha256). Only the standard library.
"""
import argparse, datetime, gzip, hashlib, json, os, re, shutil, sqlite3, struct, sys, tempfile, urllib.request, zlib

BASE = "https://yaqwsx.github.io/jlcparts/data/"
UA = {"User-Agent": "KiCadD-parts-index (+https://github.com/Brandon-git-hub/KiCadD)"}

SI = {"p": 1e-12, "n": 1e-9, "u": 1e-6, "µ": 1e-6, "μ": 1e-6, "m": 1e-3, "": 1.0, "k": 1e3, "K": 1e3, "M": 1e6, "G": 1e9}
NUM = re.compile(r"([-+]?\d+(?:\.\d+)?)\s*([pnuµμmkKMG]?)\s*(Ω|ohm|F|H|V|W|A)", re.I)


def si(text, unit):
    """'4.7uF' -> 4.7e-6 (the first number with that unit), None if there is none"""
    if not text:
        return None
    for m in NUM.finditer(text.replace(",", "")):
        u = "Ω" if m.group(3).lower() == "ohm" else m.group(3)
        if u.upper() != unit.upper():
            continue
        factor = SI.get(m.group(2))
        if factor is not None:
            return float(m.group(1)) * factor
    return None


def percent(text):
    m = re.search(r"(\d+(?:\.\d+)?)\s*%", text or "")
    return float(m.group(1)) if m else None


def first_price(text):
    # "1-999:0.0043,1000-2999:0.0032,..."
    m = re.match(r"\s*\d+-\d*:(\d+(?:\.\d+)?)", text or "")
    return float(m.group(1)) if m else None


def download(dst):
    parts = []
    for i in range(1, 100):
        name = f"cache.z{i:02d}"
        try:
            req = urllib.request.Request(BASE + name, headers=UA)
            with urllib.request.urlopen(req, timeout=600) as r, open(os.path.join(dst, name), "wb") as f:
                shutil.copyfileobj(r, f)
            parts.append(name)
        except urllib.error.HTTPError as e:
            if e.code == 404:
                break
            raise
    req = urllib.request.Request(BASE + "cache.zip", headers=UA)
    with urllib.request.urlopen(req, timeout=600) as r, open(os.path.join(dst, "cache.zip"), "wb") as f:
        shutil.copyfileobj(r, f)
        modified = r.headers.get("Last-Modified", "")
    return parts + ["cache.zip"], modified


def unsplit(src, names, out):
    """The split zip holds one deflated file: inflate it straight through the volumes."""
    def chunks():
        for n in names:
            with open(os.path.join(src, n), "rb") as f:
                while (b := f.read(1 << 20)):
                    yield b
    g = chunks()
    buf = next(g)
    off = 4 if buf[:4] == b"PK\x07\x08" else 0      # spanning marker
    sig, ver, flag, meth, t, d, crc, cs, us, nl, el = struct.unpack("<IHHHHHIIIHH", buf[off:off + 30])
    if sig != 0x04034B50 or meth != 8:
        raise SystemExit("unexpected archive layout")
    buf = buf[off + 30 + nl + el:]
    dec = zlib.decompressobj(-15)
    with open(out, "wb") as f:
        while True:
            f.write(dec.decompress(buf))
            if dec.eof:
                break
            buf = next(g)


def build(source_db, out_db):
    src = sqlite3.connect(f"file:{source_db}?mode=ro", uri=True)
    if os.path.exists(out_db):
        os.unlink(out_db)
    db = sqlite3.connect(out_db)
    db.executescript("""
        CREATE TABLE meta(key TEXT PRIMARY KEY, value TEXT);
        CREATE TABLE parts(
            lcsc INTEGER PRIMARY KEY, category TEXT, subcategory TEXT, mfr TEXT, package TEXT,
            manufacturer TEXT, basic INTEGER, preferred INTEGER, stock INTEGER, price REAL,
            description TEXT,
            value REAL, unit TEXT, voltage REAL, tolerance REAL, dielectric TEXT, power REAL,
            current REAL, vout REAL);
    """)
    rows = []
    for (lcsc, cat, sub, mfr, pkg, maker, lib, pref, stock, price, desc, attrs) in src.execute(
            "SELECT lcsc, category, subcategory, mfr, package, manufacturer, library_type, preferred, stock, "
            "price, description, attributes FROM jlc_components WHERE stock > 0 AND present = 1"):
        try:
            a = json.loads(attrs) if attrs else {}
        except ValueError:
            a = {}
        value = unit = None
        for key, u in (("Resistance", "Ω"), ("Capacitance", "F"), ("Inductance", "H")):
            if key in a:
                value, unit = si(a[key], u), u
                break
        voltage = None
        for key in ("Voltage Rating", "Voltage-Supply(Max)", "Drain to Source Voltage", "Reverse Voltage (Vr)",
                    "Voltage - DC Reverse(Vr)", "Reverse Stand-Off Voltage (Vrwm)", "Voltage - Max"):
            if key in a:
                voltage = si(a[key], "V")
                if voltage is not None:
                    break
        tol = percent(a.get("Tolerance"))
        diel = None
        tc = a.get("Temperature Coefficient", "")
        if unit == "F" and re.fullmatch(r"[XYCZNP]\d[A-Z]|C0G|NP0|COG|X\dS|X\dT", tc.strip().upper() or "-"):
            diel = tc.strip().upper().replace("COG", "C0G").replace("NP0", "C0G")
        power = si(a.get("Power(Watts)") or a.get("Pd - Power Dissipation"), "W")
        current = si(a.get("Current Rating") or a.get("Output Current") or a.get("Current - Continuous Drain(Id)")
                     or a.get("Rectified Current"), "A")
        vout = si(a.get("Output Voltage"), "V")
        rows.append((lcsc, cat, sub, mfr, pkg, maker, 1 if lib == "base" else 0, pref, stock, first_price(price),
                     desc, value, unit, voltage, tol, diel, power, current, vout))
    db.executemany("INSERT INTO parts VALUES (" + ",".join("?" * 19) + ")", rows)
    db.executescript("""
        CREATE INDEX parts_value ON parts(unit, value);
        CREATE INDEX parts_package ON parts(package);
        CREATE VIRTUAL TABLE fts USING fts5(description, mfr, package, subcategory, category,
            content='parts', content_rowid='lcsc', tokenize="unicode61 tokenchars '.-+%/'");
        INSERT INTO fts(rowid, description, mfr, package, subcategory, category)
            SELECT lcsc, description, mfr, package, subcategory, category FROM parts;
    """)
    counts = db.execute("SELECT count(*), sum(basic), sum(preferred) FROM parts").fetchone()
    db.commit()
    db.execute("VACUUM")
    db.close()
    return counts


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--source", help="a directory with cache.z01.. and cache.zip already downloaded")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    work = tempfile.mkdtemp(prefix="kicadd-parts-")
    try:
        if a.source:
            names = sorted(n for n in os.listdir(a.source) if re.fullmatch(r"cache\.z\d\d", n)) + ["cache.zip"]
            src_dir, modified = a.source, ""
        else:
            names, modified = download(work)
            src_dir = work
        raw = os.path.join(work, "cache.sqlite3")
        unsplit(src_dir, names, raw)
        db = os.path.join(work, "parts.db")
        total, basic, preferred = build(raw, db)
        os.unlink(raw)
        today = datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%d")
        con = sqlite3.connect(db)
        con.executemany("INSERT INTO meta VALUES (?, ?)", [("date", today), ("source", BASE),
                                                           ("source_modified", modified), ("parts", str(total))])
        con.commit()
        con.close()
        gz = os.path.join(a.out, "parts.db.gz")
        with open(db, "rb") as f, gzip.open(gz, "wb", compresslevel=9) as g:
            shutil.copyfileobj(f, g)
        sha = hashlib.sha256(open(gz, "rb").read()).hexdigest()
        meta = {"date": today, "source": BASE, "source_modified": modified, "parts": total, "basic": basic,
                "preferred": preferred, "bytes": os.path.getsize(gz), "db_bytes": os.path.getsize(db), "sha256": sha}
        json.dump(meta, open(os.path.join(a.out, "parts-meta.json"), "w"), indent=1)
        print(json.dumps(meta))
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())

"""lcsc_search end to end (headless): the parts index is downloaded (from a local HTTP server
here, KICADD_PARTS_URL), unpacked into the user cache, searched by value / package / tolerance /
dielectric / voltage / keyword, the first hits checked live (EasyEDA, needs the network).

usage: python3 -I search_e2e.py --build BUILD --demos DEMOS_DIR --work DIR --index PARTS_DB_GZ [--no-live]

PARTS_DB_GZ: ci/build_parts_index.py's parts.db.gz. """
import argparse, functools, http.server, os, re, shutil, sys, threading
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from m5_e2e import Mcp, Server
ap = argparse.ArgumentParser(); ap.add_argument("--build", required=True); ap.add_argument("--demos", required=True)
ap.add_argument("--work", required=True); ap.add_argument("--index", required=True); ap.add_argument("--no-live", action="store_true")
a = ap.parse_args()
build, work = os.path.abspath(a.build), os.path.abspath(a.work)
shutil.rmtree(work, ignore_errors=True)
os.makedirs(os.path.join(work, "www"))
shutil.copy(a.index, os.path.join(work, "www", "parts.db.gz"))
shutil.copytree(os.path.join(a.demos, "pic_programmer"), os.path.join(work, "p"))

handler = functools.partial(http.server.SimpleHTTPRequestHandler, directory=os.path.join(work, "www"))
handler.log_message = lambda *x: None
httpd = http.server.ThreadingHTTPServer(("127.0.0.1", 0), handler)
threading.Thread(target=httpd.serve_forever, daemon=True).start()
url = f"http://127.0.0.1:{httpd.server_address[1]}/parts.db.gz"

sock = os.path.join(work, "s.sock")
env = dict(os.environ, KICAD_RUN_FROM_BUILD_DIR="1", KICADD_PARTS_URL=url, XDG_CACHE_HOME=os.path.join(work, "cache"))
env.pop("KICADD_PARTS_DB", None)
srv = Server(build, sock, os.path.join(work, "p", "pic_programmer.kicad_pcb"), env, os.path.join(work, "server.log"))
assert srv.start()
fails = []


def check(c, label):
    print(("  ok   " if c else "  FAIL ") + label); c or fails.append(label)


def hits(t):
    return re.findall(r"^(C\d+) (BASIC|preferred|extended) ", t, re.M)


try:
    m = Mcp([os.path.join(build, "kicadd-mcp"), "--socket", sock, "--stdio"], env)
    print("[first search downloads the index]")
    err, t = m.tool("lcsc_search", {"value": "10k", "package": "0603", "tolerance": 1, "live": 0})
    print("      ", " | ".join(t.splitlines()[:3])[:200])
    check(not err and "parts index downloaded" in t, "downloaded from KICADD_PARTS_URL")
    check(any(f == "parts.db" for _, _, fs in os.walk(os.path.join(work, "cache")) for f in fs), "unpacked into the user cache")
    h = hits(t)
    check(h and h[0] == ("C25804", "BASIC"), f"10k 0603 1%: the basic C25804 first {h[:2]}")

    print("[parameters]")
    err, t = m.tool("lcsc_search", {"value": "100nF", "package": "0402", "dielectric": "X5R", "voltage_min": 16, "live": 0})
    h = hits(t)
    descr = re.findall(r"^    (\d+nF .*)$", t, re.M)
    check(not err and h and all("X5R" in d and "0402" in d for d in descr), f"100nF 0402 X5R >=16V: {len(h)} shown, all X5R 0402")
    check("parts index downloaded" not in t, "the second search uses the cached index")
    err, t = m.tool("lcsc_search", {"category": "capacitor", "value": "100n", "package": "0402", "basic_only": True, "live": 0})
    h = hits(t)
    check(not err and h and all(k == "BASIC" for _, k in h), f"basic_only: {h[:3]}")
    err, t = m.tool("lcsc_search", {"keyword": "AMS1117 3.3", "live": 0})
    check(not err and "AMS1117" in t and hits(t), "keyword AMS1117 3.3: " + (t.splitlines()[1][:80] if len(t.splitlines()) > 1 else t))
    err, t = m.tool("lcsc_search", {"value": "4k7", "package": "0402", "live": 0})
    check(not err and "4.7kΩ" in t, "4k7 means 4.7 kΩ")
    err, t = m.tool("lcsc_search", {"value": "banana", "live": 0})
    check(err and "not understood" in t, "a bad value is refused")

    if not a.no_live:
        print("[live check]")
        err, t = m.tool("lcsc_search", {"value": "10k", "package": "0603", "tolerance": 1, "limit": 2, "live": 1})
        print("      ", " | ".join(t.splitlines()[1:4])[:200])
        check(not err and re.search(r"now: LCSC stock \d+", t), "the first hit checked live")
    m.close()
finally:
    srv.stop()
    httpd.shutdown()
print("\nRESULT:", "PASS" if not fails else f"FAIL ({len(fails)})")
sys.exit(1 if fails else 0)

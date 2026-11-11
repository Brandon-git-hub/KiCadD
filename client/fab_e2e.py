"""export_fab on pic_programmer (headless): the Gerbers and drill files are the same as
`kicad-cli pcb export gerbers / drill` writes, the zip holds them, BOM / CPL in JLCPCB's columns,
and an unsaved move shows in the CPL (the board in memory, not the file).

usage: python3 -I fab_e2e.py --build BUILD --demos DEMOS_DIR --work DIR """
import argparse, csv, os, re, shutil, subprocess, sys, zipfile
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from m5_e2e import Mcp, Server
ap = argparse.ArgumentParser(); ap.add_argument("--build", required=True); ap.add_argument("--demos", required=True)
ap.add_argument("--work", required=True)
a = ap.parse_args()
build, work = os.path.abspath(a.build), os.path.abspath(a.work)
shutil.rmtree(work, ignore_errors=True)
shutil.copytree(os.path.join(a.demos, "pic_programmer"), os.path.join(work, "p"))
pcb = os.path.join(work, "p", "pic_programmer.kicad_pcb")
sock = os.path.join(work, "s.sock"); env = dict(os.environ, KICAD_RUN_FROM_BUILD_DIR="1")
fails = []


def check(c, label):
    print(("  ok   " if c else "  FAIL ") + label); c or fails.append(label)


def body(path):
    return [l for l in open(path, errors="replace").read().splitlines()
            if "CreationDate" not in l and not l.startswith(("G04", ";"))]


# the reference: kicad-cli on the file as it is
cli = os.path.join(work, "cli"); os.makedirs(cli)
cmd = os.path.join(build, "kicad", "kicad-cli")
subprocess.run([cmd, "pcb", "export", "gerbers", "--layers", "F.Cu,B.Cu,F.Mask,F.Silkscreen,Edge.Cuts", "--subtract-soldermask", "-o", cli + "/", pcb],
               env=env, capture_output=True)
subprocess.run([cmd, "pcb", "export", "drill", "--format", "excellon", "--excellon-units", "mm", "--excellon-separate-th",
                "-o", cli + "/", pcb], env=env, capture_output=True)

srv = Server(build, sock, pcb, env, os.path.join(work, "server.log")); assert srv.start()
try:
    m = Mcp([os.path.join(build, "kicadd-mcp"), "--socket", sock, "--stdio"], env)
    print("[export_fab]")
    err, t = m.tool("export_fab", {"refill": False})     # the fills as saved, as kicad-cli plots them
    print("      ", " | ".join(t.splitlines()[1:6]))
    fab = os.path.join(work, "p", "fab")
    check(not err and "9 layer file(s)" in t and "63 placements" in t, "9 Gerbers, 63 placements")
    for name in ("top_layer.gtl", "bottom_layer.gbl", "F_Mask.gts", "F_Silkscreen.gto", "Edge_Cuts.gm1", "PTH.drl", "NPTH.drl"):
        ours, ref = os.path.join(fab, "pic_programmer-" + name), os.path.join(cli, "pic_programmer-" + name)
        same = os.path.exists(ref) and body(ours) == body(ref)
        if not same:
            shutil.copy(ours, os.path.join(work, "diff-" + name))
        check(same, f"{name} same as kicad-cli")
    z = zipfile.ZipFile(os.path.join(fab, "pic_programmer-gerbers.zip")).namelist()
    check(len(z) == 12 and "pic_programmer-job.gbrjob" in z and "pic_programmer-PTH.drl" in z, f"zip: {len(z)} files")
    bom = list(csv.reader(open(os.path.join(fab, "pic_programmer-bom.csv"), encoding="utf-8")))
    check(bom[0] == ["Comment", "Designator", "Footprint", "LCSC Part #"], f"BOM header {bom[0]}")
    check(any(r[1] == "C6,C7" and r[0] == "100nF" for r in bom), "equal parts on one line (C6,C7 100nF)")
    rows = {r[0]: r for r in csv.reader(open(os.path.join(fab, "pic_programmer-cpl.csv"), encoding="utf-8"))}
    check(rows["Designator"] == ["Designator", "Mid X", "Mid Y", "Layer", "Rotation"], "CPL header")
    x0 = float(rows["U1"][1].rstrip("mm"))

    print("[an unsaved move]")
    m.tool("move_footprints", {"refs": ["U1"], "dx": 5, "dy": 0})
    err, t = m.tool("export_fab")
    check(t.startswith("all zones refilled"), "refills the zones by default")
    rows = {r[0]: r for r in csv.reader(open(os.path.join(fab, "pic_programmer-cpl.csv"), encoding="utf-8"))}
    check(abs(float(rows["U1"][1].rstrip("mm")) - x0 - 5) < 1e-3, f"CPL U1 x {x0} -> {rows['U1'][1]}")

    print("[audit]")
    err, t = m.tool("audit")
    print("      ", t.splitlines()[0])
    check(not err and "U2.14: nearest decoupling cap C1 is 33.28 mm away" in t, "far decoupling cap measured on the board")
    check("(14, 14" not in t, "a multi-unit part's shared power pin counted once")
    check("J1 (DB9-FEMAL): 4 net(s) without a TVS / ESD part" in t, "the DB9's nets have no ESD part")
    check(re.search(r"not checked .*P2", t) is not None and "P2 (SUPP28)" not in t, "the ZIF sockets are not treated as cable connectors")
    m.close()
finally:
    srv.stop()
print("\nRESULT:", "PASS" if not fails else f"FAIL ({len(fails)})")
sys.exit(1 if fails else 0)

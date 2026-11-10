"""Headless: background auto_place, progress while running, cancel moves nothing, edits refused meanwhile.

usage: KiCadD/.venv/bin/python -I autoplace_bg_e2e.py --build BUILD_DIR --demos DEMOS_DIR --work DIR

Needs the venv with the generated Python protos (README: Python client). """
import os, shutil, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from m5_e2e import Server
from kicadd_client import Client, ApiError, kd
import argparse
ap = argparse.ArgumentParser(); ap.add_argument("--build", required=True); ap.add_argument("--demos", required=True)
ap.add_argument("--work", required=True); ap.add_argument("--display", default=":58")
a = ap.parse_args()
build, work, disp = os.path.abspath(a.build), os.path.abspath(a.work), a.display
shutil.rmtree(work, ignore_errors=True)
shutil.copytree(os.path.join(a.demos, "pic_programmer"), os.path.join(work, "p"))
sock = os.path.join(work, "s.sock"); env = dict(os.environ, KICAD_RUN_FROM_BUILD_DIR="1")
srv = Server(build, sock, os.path.join(work, "p", "pic_programmer.kicad_pcb"), env, os.path.join(work, "server.log")); assert srv.start()
fails = []
def check(c, label):
    print(("  ok   " if c else "  FAIL ") + label); c or fails.append(label)
try:
    k = Client("ipc://" + sock)
    before = {f.handle: (f.pos.x, f.pos.y, f.rot_decideg) for f in k.query_radius(2000, point_mm=(0, 0), kinds=[kd.IK_FOOTPRINT], limit=500).items}
    r = k.call(kd.AutoPlace(all=True, background=True), kd.AutoPlaceResponse)
    check(r.running, f"background start answers at once (running, {r.progress!r})")
    time.sleep(1.5)
    r = k.call(kd.AutoPlaceStatus(), kd.AutoPlaceResponse)
    check(r.running and r.progress, f"progress while running: {r.progress!r} at {r.seconds:.1f} s")
    try:
        k.call(kd.MoveDelta(refs=["R1"], dx=1000000), kd.MoveResponse); check(False, "edit refused while running")
    except ApiError as e:
        check("auto_place is running" in str(e), f"edit refused while running: {str(e)[:70]!r}")
    r = k.call(kd.AutoPlaceStatus(cancel=True), kd.AutoPlaceResponse)
    check(r.cancelled and r.moved == 0, "cancel: nothing moved")
    try:
        k.call(kd.AutoPlaceStatus(), kd.AutoPlaceResponse); check(False, "no job after cancel")
    except ApiError as e:
        check("no auto_place is running" in str(e), "no job left after cancel")
    after = {f.handle: (f.pos.x, f.pos.y, f.rot_decideg) for f in k.query_radius(2000, point_mm=(0, 0), kinds=[kd.IK_FOOTPRINT], limit=500).items}
    check(before == after, "footprints where they were")
    r = k.call(kd.MoveDelta(refs=["R1"], dx=1000000), kd.MoveResponse)
    check(True, "edits accepted again after cancel")
    r = k.call(kd.AutoPlace(all=True, background=False, time_limit=2), kd.AutoPlaceResponse)
    check(r.moved == 62 and not r.running and r.timed_out, f"synchronous call still works (moved {r.moved}, timed_out {r.timed_out})")
finally:
    srv.stop()
print("\nRESULT:", "PASS" if not fails else f"FAIL ({len(fails)})")
sys.exit(1 if fails else 0)

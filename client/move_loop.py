"""Hammer MoveDelta for profiling. usage: python -I move_loop.py <sock> <ref> <seconds>"""

import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kicadd_client as kc  # noqa: E402

sock, ref, secs = sys.argv[1], sys.argv[2], float(sys.argv[3])
c = kc.Client("ipc://" + sock, timeout_ms=600000)
while True:
    try:
        c.summary()
        break
    except kc.ApiError:
        time.sleep(0.2)

t_end = time.time() + secs
n, s = 0, 1
srv = []
while time.time() < t_end:
    s = -s
    r = c.move([ref], dx_mm=0.1 * s)
    srv.append(r.elapsed_us)
    n += 1
srv.sort()
print(f"{n} moves, server p50={srv[len(srv)//2]}us")

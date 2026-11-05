"""A/B equivalence: same random move sequence on two servers, compare derived state.

usage: python -I ab_equivalence.py <sockA> <sockB> [n_moves] [seed]

Run A with the optimisations, B with KICADD_FULL_CONNECTIVITY=1 (upstream behaviour).
Compares net assignments, per-net ratsnest, teardrop zones and unconnected count.
"""

import os
import random
import statistics
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kicadd_client as kc  # noqa: E402

kd = kc.kd


def connect(sock):
    c = kc.Client("ipc://" + sock, timeout_ms=900000)
    while True:
        try:
            return c, c.summary()
        except Exception:
            time.sleep(0.3)


def digest(c):
    d = c.call(kd.GetStateDigest(), kd.StateDigestResponse)
    return (d.net_hash, d.ratsnest_hash, d.teardrop_hash, d.teardrops, d.unconnected)


def main():
    a, s = connect(sys.argv[1])
    b, _ = connect(sys.argv[2])
    # B is the reference: no lazy ratsnest (run it with KICADD_FULL_CONNECTIVITY=1 too).
    b.call(kd.SetRatsnestDeferral(enabled=False), kd.RatsnestDeferralResponse)
    pending_seen = 0
    n = int(sys.argv[3]) if len(sys.argv) > 3 else 100
    rng = random.Random(int(sys.argv[4]) if len(sys.argv) > 4 else 1)

    cx = (s.bbox_min.x + s.bbox_max.x) / 2 / kc.NM_PER_MM
    cy = (s.bbox_min.y + s.bbox_max.y) / 2 / kc.NM_PER_MM
    refs = [f.handle for f in a.query_radius(1000, point_mm=(cx, cy), kinds=[kd.IK_FOOTPRINT],
                                             limit=1_000_000).items]
    print(f"{os.path.basename(s.file)}: {len(refs)} footprints, {n} moves")

    ok = digest(a) == digest(b)
    print(f"  load: {'same' if ok else 'DIFFERENT'} {digest(a)[3:]}")
    ta, tb = [], []
    for i in range(1, n + 1):
        sel = rng.sample(refs, rng.choice([1, 1, 1, 2, 3]))
        dx, dy = rng.uniform(-3, 3), rng.uniform(-3, 3)
        rot = rng.choice([0, 0, 0, 90, -90, 180])
        ra = a.move(sel, dx_mm=dx, dy_mm=dy, drot_deg=rot)
        pending_seen = max(pending_seen, len(ra.ratsnest_pending))
        ta.append(ra.elapsed_us)
        tb.append(b.move(sel, dx_mm=dx, dy_mm=dy, drot_deg=rot).elapsed_us)
        if i % 10 == 0 or i == n:
            da, db = digest(a), digest(b)
            same = da == db
            ok &= same
            if not same or i % 50 == 0 or i == n:
                print(f"  after {i:4d}: {'same' if same else 'DIFFERENT'}  "
                      f"teardrops={da[3]}/{db[3]} unconnected={da[4]}/{db[4]}"
                      + ("" if same else f"  fields differ: "
                         f"{[k for k, x, y in zip(['net', 'rn', 'td', 'ntd', 'unc'], da, db) if x != y]}"))
    print(f"  commit p50: A={statistics.median(ta) / 1000:.1f}ms  B={statistics.median(tb) / 1000:.1f}ms"
          f"  (A max pending nets: {pending_seen})")
    print("RESULT:", "EQUIVALENT" if ok else "DIVERGED")


if __name__ == "__main__":
    main()

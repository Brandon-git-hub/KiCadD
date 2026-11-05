"""Verify KiCadD incremental connectivity and local DRC against upstream full computation.

usage: python -I verify_incremental.py <socket-path> [n_moves] [seed] [parts=AB]

A. Random footprint moves; every few moves VerifyConnectivity diffs the incremental state
   against a from-scratch BuildConnectivity(). Expect zero mismatches.
B. Moves a set of footprints into violation, then compares KiCadD CheckLocalDrc with the
   upstream full DRC task, restricted to the categories local DRC covers.
"""

import os
import random
import statistics
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import kicadd_client as kc  # noqa: E402
from board import board_rules_pb2, board_tasks_pb2  # noqa: E402
from common import tasks_pb2  # noqa: E402
from common.types import base_types_pb2  # noqa: E402

kd = kc.kd

TYPE_MAP = {
    kd.DT_CLEARANCE: board_rules_pb2.DRCET_CLEARANCE,
    kd.DT_SHORT: board_rules_pb2.DRCET_SHORTING_ITEMS,
    kd.DT_HOLE_CLEARANCE: board_rules_pb2.DRCET_HOLE_CLEARANCE,
    kd.DT_COURTYARD_OVERLAP: board_rules_pb2.DRCET_OVERLAPPING_FOOTPRINTS,
    kd.DT_EDGE_CLEARANCE: board_rules_pb2.DRCET_EDGE_CLEARANCE,
    kd.DT_COPPER_OUTSIDE_BOARD: board_rules_pb2.DRCET_COPPER_OUTSIDE_BOARD,
}
COVERED = set(TYPE_MAP.values())


def connect(sock):
    c = kc.Client("ipc://" + sock, timeout_ms=900000)
    while True:
        try:
            return c, c.summary()
        except Exception:
            time.sleep(0.3)


def all_items(c, summary, kinds):
    cx = (summary.bbox_min.x + summary.bbox_max.x) / 2 / kc.NM_PER_MM
    cy = (summary.bbox_min.y + summary.bbox_max.y) / 2 / kc.NM_PER_MM
    return c.query_radius(1000, point_mm=(cx, cy), kinds=kinds, limit=1_000_000,
                          with_ids=True).items


def verify_conn(c, label):
    r = c.call(kd.VerifyConnectivity(), kd.VerifyConnectivityResponse)
    ok = not r.item_mismatches and not r.net_mismatches \
        and r.unconnected_before == r.unconnected_after
    print(f"  [{label}] verify: items={r.items_checked} nets={r.nets_checked} "
          f"unconnected {r.unconnected_before}->{r.unconnected_after} "
          f"full rebuild={r.rebuild_us / 1000:.0f}ms  {'OK' if ok else 'MISMATCH'}")
    for m in list(r.item_mismatches)[:10] + list(r.net_mismatches)[:10]:
        print("     ", m)
    return ok


def full_drc(c, board_file):
    req = board_tasks_pb2.StartDrcTask()
    req.board.type = base_types_pb2.DOCTYPE_PCB
    req.board.board_filename = board_file
    req.report_all_track_errors = True
    tok = c.call(req, tasks_pb2.StartTaskResponse).token
    t0 = time.time()
    while True:
        st = c.call(tasks_pb2.GetTaskStatus(token=tok), tasks_pb2.TaskStatusResponse)
        if st.state != tasks_pb2.TS_RUNNING:
            break
        time.sleep(0.5)
    if st.state != tasks_pb2.TS_COMPLETED:
        raise RuntimeError(f"DRC task ended in state {st.state}: {st.error_message}")
    res = c.call(board_tasks_pb2.GetDrcTaskResult(token=tok), board_tasks_pb2.DrcTaskResult)
    return res, time.time() - t0


def main():
    sock = sys.argv[1]
    n_moves = int(sys.argv[2]) if len(sys.argv) > 2 else 200
    rng = random.Random(int(sys.argv[3]) if len(sys.argv) > 3 else 1)
    parts = sys.argv[4] if len(sys.argv) > 4 else "AB"
    ok = drc_ok = True

    c, s = connect(sock)
    board_file = os.path.basename(s.file)
    fps = all_items(c, s, [kd.IK_FOOTPRINT])
    refs = [f.handle for f in fps]
    print(f"board {board_file}: {len(refs)} footprints")

    # ------------------------------------------------------------------ A. connectivity
    if "A" in parts:
        ok = part_a(c, refs, n_moves, rng)
    if "B" in parts:
        drc_ok = part_b(c, s, fps, refs, board_file, rng)
    print("\nRESULT:", "PASS" if ok and drc_ok else "FAIL")
    c.close()


def part_a(c, refs, n_moves, rng):
    print("\nA. incremental connectivity vs full rebuild")
    ok = verify_conn(c, "load")
    times, drc_times = [], []
    for i in range(1, n_moves + 1):
        k = rng.choice([1, 1, 1, 2, 3])
        sel = rng.sample(refs, k)
        r = c.move(sel, dx_mm=rng.uniform(-3, 3), dy_mm=rng.uniform(-3, 3),
                   drot_deg=rng.choice([0, 0, 0, 90, -90, 180]))
        times.append(r.elapsed_us)
        drc_times.append(r.drc_us)
        if i % 25 == 0:
            ok &= verify_conn(c, f"after {i} moves")
    times.sort()
    drc_times.sort()
    print(f"  MoveDelta commit p50={statistics.median(times) / 1000:.1f}ms "
          f"p95={times[int(len(times) * .95) - 1] / 1000:.1f}ms   "
          f"local DRC p50={statistics.median(drc_times) / 1000:.2f}ms "
          f"p95={drc_times[int(len(drc_times) * .95) - 1] / 1000:.2f}ms")

    return ok


def part_b(c, s, fps, refs, board_file, rng):
    print("\nB. local DRC vs upstream full DRC")
    base, _ = full_drc(c, board_file)
    by_type = {}
    for m in base.violations:
        by_type[m.error_type] = by_type.get(m.error_type, 0) + 1
    name = board_rules_pb2.DrcErrorType.Name
    print("  baseline full DRC by type:",
          {name(t): n for t, n in sorted(by_type.items()) if t in COVERED})
    moved = rng.sample(refs, 12)
    for ref in moved:
        c.move([ref], dx_mm=rng.uniform(-1.5, 1.5), dy_mm=rng.uniform(-1.5, 1.5))

    local = c.call(kd.CheckLocalDrc(refs=moved), kd.LocalDrcResponse)
    print(f"  local DRC on {len(moved)} footprints: {len(local.drc)} violations "
          f"in {local.drc_us / 1000:.2f}ms")

    full, secs = full_drc(c, board_file)
    print(f"  full DRC: {len(full.violations)} markers in {secs:.1f}s")

    # Map ids -> owning footprint, so we can scope full-DRC markers to the moved set.
    owner = {}
    moved_set = set(moved)
    for ref in moved:
        for it in c.query_radius(0.001, ref=ref, kinds=[kd.IK_FOOTPRINT, kd.IK_PAD],
                                 limit=100000, with_ids=True).items:
            if it.kind == kd.IK_FOOTPRINT and it.handle == ref:
                owner[it.id] = ref
    for f in fps:
        owner.setdefault(f.id, f.handle)
    # pads: ask around each moved fp (radius large enough to cover the footprint)
    for ref in moved:
        for it in c.query_radius(30, ref=ref, kinds=[kd.IK_PAD], limit=100000,
                                 with_ids=True).items:
            owner[it.id] = it.handle.split(".")[0]
    zone_ids = {z.id for z in all_items(c, s, [kd.IK_ZONE])}

    def key(t, a, b):
        return (t, frozenset((a, b)))

    local_set = {key(TYPE_MAP[v.type], v.a_id, v.b_id) for v in local.drc}

    full_set = set()
    for m in full.violations:
        if m.error_type not in COVERED or len(m.items) not in (1, 2) or m.excluded:
            continue
        a, b = m.items[0].value, m.items[-1].value   # one item: copper outside the board
        if a in zone_ids or b in zone_ids:
            continue  # local DRC deliberately skips zone fills
        oa, ob = owner.get(a), owner.get(b)
        if not ({oa, ob} & moved_set):
            continue
        if oa is not None and oa == ob:
            continue  # intra-footprint pairs do not change when a footprint moves
        full_set.add(key(m.error_type, a, b))

    both = local_set & full_set
    only_local = local_set - full_set
    only_full = full_set - local_set
    print(f"  in scope: full={len(full_set)} local={len(local_set)} "
          f"matched={len(both)} only_local={len(only_local)} only_full={len(only_full)}")

    for t, pair in list(only_local)[:10]:
        print("    only local:", name(t), [owner.get(i, i[:8]) for i in pair])
    for t, pair in list(only_full)[:10]:
        print("    only full :", name(t), [owner.get(i, i[:8]) for i in pair])

    return not only_local and not only_full


if __name__ == "__main__":
    main()

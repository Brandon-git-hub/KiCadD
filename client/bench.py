"""Phase-1 baseline: latency of upstream vs KiCadD queries on a resident board.

usage: python -I bench.py <socket-path> [start-timestamp-file]
"""

import os
import statistics
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import kicadd_client as kc  # noqa: E402
from common.commands import editor_commands_pb2  # noqa: E402  (upstream stubs)
from common.types import base_types_pb2, enums_pb2  # noqa: E402
from google.protobuf import text_format  # noqa: E402


def wait_ready(sock_path, timeout=600):
    t0 = time.time()
    while time.time() - t0 < timeout:
        if os.path.exists(sock_path):
            try:
                c = kc.Client("ipc://" + sock_path, timeout_ms=2000)
                c.ping()
                c.close()
                return
            except Exception:
                pass
        time.sleep(0.2)
    raise TimeoutError("server not ready")


def bench(label, fn, n=50):
    samples = []
    for _ in range(n):
        t0 = time.perf_counter()
        r = fn()
        samples.append((time.perf_counter() - t0) * 1e6)
    samples.sort()
    print(f"  {label:<38} p50={statistics.median(samples):9.0f}us  "
          f"p95={samples[int(len(samples) * 0.95) - 1]:9.0f}us")
    return r


def approx_tokens(msg):
    # The MCP layer will render protobuf as text/JSON; ~4 chars per token.
    return len(text_format.MessageToString(msg, as_one_line=True)) // 4


def main():
    sock = sys.argv[1]
    wait_ready(sock)
    if len(sys.argv) > 2:
        start = float(open(sys.argv[2]).read())
        print(f"server ready (load + parse + listen): {time.time() - start:.1f}s")

    c = kc.Client("ipc://" + sock, timeout_ms=600000)

    # The board is preloaded after the listener starts; the first summary blocks until done.
    while True:
        try:
            s = c.summary()
            break
        except kc.ApiError:
            time.sleep(0.2)
    if len(sys.argv) > 2:
        print(f"board resident (first summary ok): {time.time() - start:.1f}s")
    print("board:", text_format.MessageToString(s, as_one_line=True))

    # --- pick an anchor footprint with neighbours --------------------------------
    cx = (s.bbox_min.x + s.bbox_max.x) / 2 / kc.NM_PER_MM
    cy = (s.bbox_min.y + s.bbox_max.y) / 2 / kc.NM_PER_MM
    fps = c.query_radius(30, point_mm=(cx, cy), kinds=[kc.kd.IK_FOOTPRINT], limit=1)
    ref = fps.items[0].handle
    net_resp = c.query_radius(3, ref=ref, kinds=[kc.kd.IK_PAD], limit=8)
    net = next(i.net for i in net_resp.items if i.net)
    print(f"anchor footprint: {ref}   sample net: {net}\n")

    print("latency (round trip, Python client, 50 runs):")
    bench("upstream Ping", c.ping)

    # Upstream: fetch all footprints (what a naive MCP tool does today)
    def upstream_all_fps():
        req = editor_commands_pb2.GetItems()
        req.header.document.type = base_types_pb2.DOCTYPE_PCB
        req.header.document.board_filename = os.path.basename(s.file)
        req.types.append(enums_pb2.KOT_PCB_FOOTPRINT)
        return c.call(req, editor_commands_pb2.GetItemsResponse)

    up = bench("upstream GetItems(all footprints)", upstream_all_fps, n=5)
    r5 = bench(f"kicadd QueryRadius({ref}, 5mm)", lambda: c.query_radius(5, ref=ref))
    rn = bench(f"kicadd QueryNet({net})", lambda: c.query_net(net))
    bench("kicadd GetBoardSummary", c.summary)

    flip = [1]

    def mv():
        flip[0] = -flip[0]
        return c.move([ref], dx_mm=0.1 * flip[0])

    m = bench(f"kicadd MoveDelta({ref}, ±0.1mm)", mv)
    print(f"  (server-side MoveDelta commit time: {m.elapsed_us}us)")

    print("\napprox. LLM tokens if rendered as text:")
    print(f"  upstream all footprints : {approx_tokens(up):>9,}  ({len(up.items)} items)")
    print(f"  kicadd radius 5mm       : {approx_tokens(r5):>9,}  ({len(r5.items)} items, "
          f"total={r5.total})")
    print(f"  kicadd single net       : {approx_tokens(rn):>9,}  ({len(rn.items)} items)")

    print("\nsample QueryRadius items:")
    for it in r5.items[:5]:
        print("  ", text_format.MessageToString(it, as_one_line=True))

    c.close()


if __name__ == "__main__":
    main()

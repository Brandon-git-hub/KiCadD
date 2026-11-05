"""Flip / add / delete footprints and board-edge local DRC, end to end.

usage: python -I layout_e2e.py <base-url> <socket>

Needs a daemon + MCP serving a disposable copy of the Jetson AGX Thor baseboard (the test
saves it at the end). Checks:
  - flip: side, padstack side, mirrored geometry, exact round trip, undo; the saved file has
    the footprint and its silk / fab / courtyard on the back layers
  - delete: footprint and its pads leave the board and connectivity (VerifyConnectivity diff
    empty), undo restores the same ids and nets, redo removes them again
  - add: from the footprint library and as a copy of a board part, nets by pad number, new
    nets, auto references, errors; undo / redo keep the same ids
  - edge DRC: footprints pushed over / outside the board edge; local DRC finds exactly the
    pads that upstream full DRC reports as copper_edge_clearance / copper_outside_board
"""

import json
import os
import re
import sys
import time
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from kicadd_client import Client, ApiError, kd  # noqa: E402
from kicadd import render_pb2 as kr  # noqa: E402

FAILS = []


def check(cond, label):
    print(("  ok   " if cond else "  FAIL ") + label)
    if not cond:
        FAILS.append(label)


class Mcp:
    def __init__(self, base):
        self.base = base

    def __call__(self, name, args=None):
        body = json.dumps({"jsonrpc": "2.0", "id": 1, "method": "tools/call",
                           "params": {"name": name, "arguments": args or {}}}).encode()
        req = urllib.request.Request(self.base + "/mcp", data=body, headers={"Content-Type": "application/json"})
        r = json.load(urllib.request.urlopen(req))["result"]
        return r["content"][0]["text"], r["isError"]


def footprints(c):
    s = c.call(kr.GetRenderSnapshot(), kr.RenderSnapshot)
    return {f.ref: f for f in s.items.footprints}


def geom(f):
    """Everything about a footprint's copper that a flip/undo must reproduce exactly."""
    return (f.id, f.back, f.pos.x, f.pos.y, f.rot_decideg,
            tuple(sorted((p.number, p.net, p.side, p.pos.x, p.pos.y, p.size.x, p.size.y, p.rot_decideg,
                          tuple(p.poly.xy)) for p in f.pads)),
            tuple(f.courtyard.xy))


def verify(c, label):
    r = c.call(kd.VerifyConnectivity(), kd.VerifyConnectivityResponse)
    check(not r.item_mismatches and not r.net_mismatches and r.unconnected_before == r.unconnected_after,
          f"{label}: incremental connectivity == full rebuild ({r.items_checked} items, {r.nets_checked} nets, "
          f"unconnected {r.unconnected_before}/{r.unconnected_after})")


def unconnected(c):
    return c.call(kd.FlushRatsnest(), kd.FlushRatsnestResponse).unconnected


def history(c):
    return c.call(kd.GetHistory(), kd.HistoryResponse)


def test_flip(c, mcp):
    print("[flip]")
    fps = footprints(c)
    u = fps["U78"]
    g0 = geom(u)
    check(not u.back and all(p.side == kr.SIDE_FRONT for p in u.pads), "U78 starts on the front")

    text, err = mcp("move_footprints", {"refs": ["U78"], "flip": True})
    f = footprints(c)["U78"]
    check(not err and "B.Cu" in text.splitlines()[0], f"MCP flip -> {text.splitlines()[0]}")
    check(f.back and all(p.side == kr.SIDE_BACK for p in f.pads), "all pads moved to the back side")
    check((f.pos.x, f.pos.y) == (u.pos.x, u.pos.y), "anchor stays put")
    mirrored = all(any(q.number == p.number and q.pos.x == 2 * u.pos.x - p.pos.x and q.pos.y == p.pos.y
                       for q in f.pads) for p in u.pads)
    check(mirrored, "pads mirrored left-right around the anchor")
    cy0 = sorted(zip(u.courtyard.xy[0::2], u.courtyard.xy[1::2]))
    cy1 = sorted(zip(f.courtyard.xy[0::2], f.courtyard.xy[1::2]))
    check(cy0 and sorted((2 * u.pos.x - x, y) for x, y in cy0) == cy1, "courtyard mirrored too")
    check(history(c).undo[-1] == "flip U78 to B.Cu", f"history label '{history(c).undo[-1]}'")
    verify(c, "after flip")

    mcp("move_footprints", {"refs": ["U78"], "flip": True})
    check(geom(footprints(c)["U78"]) == g0, "flip twice = identical geometry")
    mcp("undo")
    check(footprints(c)["U78"].back, "undo of the second flip -> back again")
    mcp("undo")
    check(geom(footprints(c)["U78"]) == g0, "undo of the first flip -> exact original")

    # move + rotate + flip in one edit, then one undo
    r = c.call(kd.MoveDelta(refs=["U78", "C216"], dx=1_000_000, drot_decideg=900, flip=True), kd.MoveResponse)
    check(all(m.back for m in r.moved) and len(r.moved) == 2, "move+rotate+flip of two parts in one commit")
    c.call(kd.Undo(), kd.HistoryResponse)
    fps2 = footprints(c)
    check(geom(fps2["U78"]) == g0 and geom(fps2["C216"]) == geom(fps["C216"]), "one undo restores both")
    verify(c, "after flip undo")


def test_delete(c, mcp):
    print("[delete]")
    fps = footprints(c)
    victim = fps["C216"]
    g0 = geom(victim)
    nets = sorted({p.net for p in victim.pads if p.net})
    u0 = unconnected(c)

    text, err = mcp("delete_footprints", {"refs": ["C216", "NOPE1"]})
    check(not err and "C216 deleted" in text and "not found: NOPE1" in text, "MCP delete (one missing ref reported)")
    check("C216" not in footprints(c), "C216 gone from the board")
    for net in nets:
        q = c.call(kd.QueryNet(net=net), kd.QueryResponse)
        check(not any(i.handle.startswith("C216.") for i in q.items), f"net {net} no longer lists C216 pads")
    verify(c, "after delete")
    u1 = unconnected(c)
    print(f"       unconnected {u0} -> {u1}")

    h = c.call(kd.Undo(), kd.HistoryResponse)
    check(h.ok and h.label == "delete C216", f"undo '{h.label}'")
    back = footprints(c).get("C216")
    check(back is not None and geom(back) == g0, "undo restores C216 with the same id, nets and geometry")
    verify(c, "after undo delete")
    check(unconnected(c) == u0, f"unconnected back to {u0}")

    c.call(kd.Redo(), kd.HistoryResponse)
    check("C216" not in footprints(c), "redo deletes it again")
    verify(c, "after redo delete")
    check(unconnected(c) == u1, "redo: same unconnected count as the delete")
    c.call(kd.Undo(), kd.HistoryResponse)
    check(geom(footprints(c)["C216"]) == g0, "undo once more: still identical")

    # several at once, one undo
    refs = ["C216", "U78"]
    r = c.call(kd.DeleteFootprints(refs=refs), kd.MoveResponse)
    check(sorted(r.removed) == sorted(refs), "delete two in one commit")
    c.call(kd.Undo(), kd.HistoryResponse)
    fps2 = footprints(c)
    check(all(geom(fps2[x]) == geom(fps[x]) for x in refs), "one undo brings both back")
    verify(c, "after multi delete + undo")


def test_add(c, mcp):
    print("[add]")
    fps = footprints(c)
    u0 = unconnected(c)
    refs_c = [int(m.group(1)) for r in fps for m in [re.fullmatch(r"C(\d+)", r)] if m]
    expect = f"C{max(refs_c) + 1}"

    text, err = mcp("add_footprint", {"footprint": "Capacitor_SMD:C_0603_1608Metric", "ref": "C?", "value": "100nF",
                                      "x": 20, "y": 20, "rotate": 90, "nets": {"1": "+3V3", "2": "GND"}})
    first = text.splitlines()
    check(not err and f"{expect} added" in text and "Capacitor_SMD.pretty" in first[0],
          f"MCP add from the library -> {expect}: {first[:2]}")
    f = footprints(c).get(expect)
    check(f is not None and f.value == "100nF" and f.rot_decideg == 900 and not f.back, "value, rotation, side")
    check(f and {p.number: p.net for p in f.pads} == {"1": "+3V3", "2": "GND"}, "pad nets by number")
    verify(c, "after add")
    u1 = unconnected(c)
    check(u1 == u0 + 2, f"two new unrouted connections ({u0} -> {u1})")
    gid = geom(f)

    c.call(kd.Undo(), kd.HistoryResponse)
    check(expect not in footprints(c) and unconnected(c) == u0, "undo removes it")
    verify(c, "after undo add")
    c.call(kd.Redo(), kd.HistoryResponse)
    check(geom(footprints(c)[expect]) == gid, "redo puts the same footprint (same id) back")
    verify(c, "after redo add")

    # copy of a board part, on the back, with a new net
    r = c.call(kd.AddFootprint(like="U78", pos=kd.Point(x=30_000_000, y=30_000_000), back=True,
                               nets={"1": "KICADD_TEST_NET"}), kd.MoveResponse)
    ref = r.added[0] if r.added else ""
    nu = footprints(c).get(ref)
    check(ref.startswith("U") and nu is not None and nu.back and nu.id != fps["U78"].id,
          f"like=U78 -> {ref}, new id, back side")
    check(list(r.created_nets) == ["KICADD_TEST_NET"] and r.source.startswith("copy of U78"),
          f"new net created, source '{r.source}'")
    check(nu and sorted(p.net for p in nu.pads if p.net) == ["KICADD_TEST_NET"], "copy keeps none of U78's nets")
    verify(c, "after add like")
    c.call(kd.Undo(), kd.HistoryResponse)

    for bad, why in [(kd.AddFootprint(lib_id="Nope:Nothing", ref="X1"), "not found"),
                     (kd.AddFootprint(lib_id="Capacitor_SMD:C_0603_1608Metric", ref="C216"), "already exists"),
                     (kd.AddFootprint(lib_id="Capacitor_SMD:C_0603_1608Metric", ref="C?", nets={"3": "GND"}),
                      "pads: 1,2"),
                     (kd.AddFootprint(lib_id="Capacitor_SMD:C_0603_1608Metric"), "needs ref")]:
        try:
            c.call(bad, kd.MoveResponse)
            check(False, f"rejected: {why}")
        except ApiError as e:
            check(why in str(e), f"rejected: {e}")
    c.call(kd.Undo(), kd.HistoryResponse)   # the library add
    check(expect not in footprints(c), "cleanup")


def parse_drc(text, local):
    """{(kind, pad handle)} from run_drc / check_drc output."""
    out = set()
    for line in text.splitlines():
        if local:
            m = re.match(r"(EDGE|OUTSIDE_BOARD) (\S+?)(\[|\s)", line)
            if m:
                out.add(("edge" if m.group(1) == "EDGE" else "outside", m.group(2)))
        else:
            m = re.match(r"[EW] edge_clearance (\S+?)(?:\[[^\]]*\])? x (\S+?)(\[|\s|$)", line)
            if m:   # upstream lists the edge first
                out.add(("edge", m.group(2) if "." in m.group(2) and not m.group(2).endswith(("edge", "margin"))
                         else m.group(1)))
            m = re.match(r"[EW] copper_outside_board (\S+?)(\[|\s|$)", line)
            if m:
                out.add(("outside", m.group(1)))
    return out


def test_edge(c, mcp):
    print("[edge clearance: local vs full DRC]")
    s = c.call(kd.GetBoardSummary(), kd.BoardSummaryResponse)
    x0, y0, x1, y1 = s.bbox_min.x, s.bbox_min.y, s.bbox_max.x, s.bbox_max.y
    fps = footprints(c)
    small = [f for f in fps.values() if 2 <= len(f.pads) <= 8 and not f.locked]
    small.sort(key=lambda f: f.ref)
    picked = small[:24]
    W, H = x1 - x0, y1 - y0
    moved = []
    for i, f in enumerate(picked):
        t = 0.15 + 0.7 * i / len(picked)
        kind = i % 4
        if kind == 0:
            target = (x0 + 200_000, y0 + int(t * H))          # straddles the left edge
        elif kind == 1:
            target = (x1 - 600_000, y0 + int(t * H))          # just inside the right edge
        elif kind == 2:
            target = (x0 + int(t * W), y0 + 100_000)          # on the top edge
        else:
            target = (x0 - 20_000_000, y0 + int(t * H))       # entirely off the board
        c.call(kd.MoveDelta(refs=[f.ref], dx=target[0] - f.pos.x, dy=target[1] - f.pos.y), kd.MoveResponse)
        moved.append(f.ref)

    t0 = time.perf_counter()
    loc = c.call(kd.CheckLocalDrc(refs=moved), kd.LocalDrcResponse)
    local_ms = (time.perf_counter() - t0) * 1000
    text, err = mcp("check_drc", {"refs": moved})
    local = parse_drc(text, True)

    text, err = mcp("run_drc", {"types": ["edge_clearance", "copper_outside_board"], "limit": 100000})
    full = {k for k in parse_drc(text, False) if k[1].split(".")[0] in moved}
    print(f"       {len(moved)} footprints moved to the edge; local {len(local)} ({loc.drc_us} us in the daemon, "
          f"{local_ms:.1f} ms round trip), full {len(full)}")
    print("       " + text.splitlines()[0])
    check(len(full) > 10 and any(k[0] == "outside" for k in full) and any(k[0] == "edge" for k in full),
          "the setup produces both kinds of violation")
    check(local == full, "local edge DRC == full DRC (same pads, same kind)")
    if local != full:
        print("       only local:", sorted(local - full)[:10])
        print("       only full: ", sorted(full - local)[:10])

    # one footprint, as an agent would see it during a move
    probe = sorted(k[1] for k in local if k[0] == "edge")[0].split(".")[0]
    r = c.call(kd.MoveDelta(refs=[probe], dx=10_000), kd.MoveResponse)
    edge = [v for v in r.drc if v.type in (kd.DT_EDGE_CLEARANCE, kd.DT_COPPER_OUTSIDE_BOARD)]
    check(edge, f"MoveDelta reports the edge violation of {probe} immediately "
                f"(local DRC {r.drc_us} us, commit {r.elapsed_us} us)")
    c.call(kd.Undo(), kd.HistoryResponse)

    for _ in moved:
        c.call(kd.Undo(), kd.HistoryResponse)
    fps2 = footprints(c)
    check(all(geom(fps2[r]) == geom(fps[r]) for r in moved), "all edge moves undone exactly")


def test_saved_layers(c, mcp, board_file):
    print("[flip in the saved file: silk / fab / courtyard on the back]")
    mcp("move_footprints", {"refs": ["U78"], "flip": True})
    text, err = mcp("save_board")
    check(not err, f"save: {text.strip()}")
    src = open(board_file, encoding="utf-8").read()
    i = src.find('(property "Reference" "U78"')
    start = src.rfind("\n\t(footprint ", 0, i)
    end = src.find("\n\t(footprint ", i)
    block = src[start:end]
    check(start >= 0 and block.split("\n")[2].strip() == '(layer "B.Cu")', "footprint on B.Cu")
    layers = set(re.findall(r'\(layer "([^"]+)"', block))
    check({"B.SilkS", "B.Fab", "B.CrtYd"} <= layers and not ({"F.SilkS", "F.Fab", "F.CrtYd"} & layers),
          f"graphics moved to the back layers: {sorted(layers)}")
    pad_layers = set(re.findall(r'\(pad "[^"]*" smd .*?\(layers ((?:"[^"]+"\s*)+)\)', block, re.S))
    flat = {ly for group in pad_layers for ly in re.findall(r'"([^"]+)"', group)}
    check(flat and flat <= {"B.Cu", "B.Paste", "B.Mask"}, f"SMD pads on {sorted(flat)}")


def main(base, sock):
    mcp = Mcp(base)
    c = Client("ipc://" + sock, timeout_ms=120000)
    for _ in range(120):                     # the board may still be loading
        try:
            board_file = c.call(kd.GetBoardSummary(), kd.BoardSummaryResponse).file
            break
        except ApiError as e:
            if "NOT_READY" not in str(e) and "BUSY" not in str(e):
                raise
            time.sleep(0.5)
    h = history(c)
    check(len(h.undo) == 0, "fresh board")

    test_flip(c, mcp)
    test_delete(c, mcp)
    test_add(c, mcp)
    test_edge(c, mcp)
    check(len(history(c).undo) == 0, "everything undone: history empty")
    test_saved_layers(c, mcp, board_file)

    print("\nRESULT:", "PASS" if not FAILS else f"FAIL ({len(FAILS)})")
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1].rstrip("/"), sys.argv[2]))

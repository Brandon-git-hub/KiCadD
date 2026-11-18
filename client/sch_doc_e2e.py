"""Schematic documents (headless, pic_programmer: a root sheet and pic_sockets): sch_export_pdf, sch_annotate.

usage: python3 -I sch_doc_e2e.py --build BUILD --demos DEMOS_DIR --work DIR

  * sch_annotate: an unnumbered part (R1 made "R?" in the file) gets the first free number;
    a full schematic numbered already: nothing to do; reset as a dry run lists changes and
    changes nothing; reset on a sheet renumbers only that sheet; sync_pcb renames the board's
    parts in place (matched by UUID, nothing moved); sch_undo / sch_redo; refs + start; per_sheet;
    bad arguments refused
  * sch_export_pdf: every page in one PDF, from memory (an unsaved value is in it, the file on
    disk does not have it); a relative path under the project; black and white
"""

import argparse
import os
import re
import shutil
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from m5_e2e import FAILS, Mcp, Server, binaries, check  # noqa: E402

ap = argparse.ArgumentParser()
ap.add_argument("--build", required=True)
ap.add_argument("--demos", required=True)
ap.add_argument("--work", required=True)
a = ap.parse_args()

build, work = os.path.abspath(a.build), os.path.abspath(a.work)
shutil.rmtree(work, ignore_errors=True)
proj = os.path.join(work, "p")
shutil.copytree(os.path.join(a.demos, "pic_programmer"), proj)
pcb = os.path.join(proj, "pic_programmer.kicad_pcb")
sch_file = os.path.join(proj, "pic_programmer.kicad_sch")

# R1 unnumbered, as a part placed by hand and not annotated yet
s = open(sch_file, encoding="utf-8").read()
assert s.count('(property "Reference" "R1"') == 1 and s.count('(reference "R1")') == 1
s = s.replace('(property "Reference" "R1"', '(property "Reference" "R?"').replace('(reference "R1")', '(reference "R?")')
open(sch_file, "w", encoding="utf-8").write(s)

kicad_cli, kicadd_mcp, env = binaries(build)
sock = os.path.join(work, "s.sock")


def pdf_streams(data):
    out = b""
    for m in re.finditer(rb"stream\r?\n(.*?)\r?\nendstream", data, re.S):
        try:
            out += zlib.decompress(m.group(1))
        except zlib.error:
            pass
    return out


def changes(text):
    return re.findall(r"^\s+(\S+) -> (\S+) \((.*), (\S+)\)$", text, re.M)


srv = Server(build, sock, pcb, env, os.path.join(work, "server.log"))
assert srv.start(), "api-server did not start"
try:
    m = Mcp([kicadd_mcp, "--socket", sock, "--stdio"], env)

    print("[sch_annotate]")
    err, t = m.tool("sch_annotate", {"dry_run": True})
    print("      ", t.strip().replace("\n", " | ")[:160])
    check(not err and t.startswith("dry run: 1 part(s) would be renumbered") and ("R?", "R1") in [c[:2] for c in changes(t)],
          "dry run: the unnumbered R? would be R1 (the first free)")
    check("problem:" not in t, "dry run: no problem left after numbering")
    err, t = m.tool("sch_part", {"ref": "R1"})
    check(err or "no part" in t or "R1" not in t.splitlines()[0], "dry run changed nothing (no R1 yet)")
    err, t = m.tool("sch_annotate", {})
    check(not err and t.startswith("1 part(s) renumbered") and "R? -> R1" in t, "annotate: R? -> R1")
    err, t = m.tool("sch_annotate", {})
    check(not err and t.startswith("0 part(s) renumbered") and "has a number already" in t, "again: every part numbered already")

    err, t = m.tool("sch_annotate", {"reset": True, "dry_run": True})
    full = changes(t)
    print("      ", t.splitlines()[0])
    check(not err and len(full) > 10, f"reset dry run: {len(full)} change(s) listed")
    old, new, value, sheet = full[0]
    err, t = m.tool("sch_part", {"ref": old})
    check(not err and value in t, f"after the dry run {old} is still {value}")

    err, t = m.tool("sch_annotate", {"reset": True, "order": "y", "sheet": "pic_sockets"})
    sock_changes = changes(t)
    print("      ", t.strip().replace("\n", " | ")[:200])
    check(not err and sock_changes and all(c[3] == "/pic_sockets/" for c in sock_changes),
          f"sheet pic_sockets: {len(sock_changes)} renumbered, all on that sheet")
    renamed = {c[0]: (c[1], c[2]) for c in sock_changes}
    first_old, (first_new, first_value) = next(iter(renamed.items()))
    err, t = m.tool("sch_part", {"ref": first_new})
    check(not err and first_value in t, f"{first_new} is the part that was {first_old} ({first_value})")

    err, t = m.tool("pcb_set_fields", {"parts": {"P101": {"exclude_from_pos": True}}})
    check(not err, "P101 excluded from position files on the board only")
    err, t = m.tool("sync_pcb")
    check(not err and f"Changed {first_old} reference designator to {first_new}" in t, "sync_pcb renames the board's part")
    check("Removed P101 'exclude from position files' fabrication attribute." in t
          and 'sch_set_fields {"parts":{"P101":{"exclude_from_pos":true}}}' in t,
          "the board-only attribute dropped by the sync is shown, with the call that keeps it")
    err, t = m.tool("board_diff")
    print("      ", t.strip().replace("\n", " | ")[:400])
    check(not err and f"{len(sock_changes)} renumbered" in t and not re.search(r"\bmoved\b", t),
          "board_diff: the parts renumbered in place, none moved")
    err, t = m.tool("undo")
    print("      ", t.strip().replace("\n", " | ")[:200])
    check(not err and t.startswith("undid: sync_pcb"), "undo sync_pcb")
    check("put back as they were" in t and not re.search(r"part\(s\) moved|@\d", t) and "courtyard" not in t,
          "undo sync_pcb: the parts put back, none reported as moved, no local DRC of parts that did not move")
    m.tool("undo")   # the pcb_set_fields

    err, t = m.tool("sch_undo")
    check(not err and t.startswith("undid: annotate") and "already" not in t, "sch_undo: " + t.splitlines()[0])
    err, t = m.tool("sch_part", {"ref": first_old})
    check(not err and first_value in t, f"{first_old} is {first_value} again")
    err, t = m.tool("sch_redo")
    check(not err and t.startswith("redid: annotate"), "sch_redo")
    err, t = m.tool("sch_part", {"ref": first_new})
    check(not err and first_value in t, f"{first_new} is {first_value} again")
    m.tool("sch_undo")

    err, t = m.tool("sch_annotate", {"refs": ["R1", "nope"], "reset": True, "start": 50})
    one = changes(t)
    check(not err and len(one) == 1 and one[0][0] == "R1" and int(one[0][1][1:]) >= 50 and "no part nope" in t,
          "refs + start 50: " + (" ".join(one[0][:2]) if one else t.strip()[:80]))
    m.tool("sch_undo")

    err, t = m.tool("sch_annotate", {"reset": True, "per_sheet": "100", "dry_run": True})
    nums = [(c[3], int(re.sub(r"\D", "", c[1]) or 0)) for c in changes(t)]
    check(not err and nums and all(n >= 200 for s_, n in nums if s_ == "/pic_sockets/")
          and all(100 <= n < 200 for s_, n in nums if s_ == "/"), "per_sheet 100: page 1 R1xx, page 2 R2xx")

    err, t = m.tool("sch_annotate", {"order": "z"})
    check(err and "order" in t, "order z: refused")
    err, t = m.tool("sch_annotate", {"refs": ["R1"], "sheet": "pic_sockets"})
    check(err and "not both" in t, "refs and sheet: refused")

    print("[sch_export_pdf]")
    err, t = m.tool("sch_export_pdf")
    print("      ", t.strip().replace("\n", " | "))
    path = os.path.join(proj, "pic_programmer.pdf")
    data = open(path, "rb").read() if os.path.exists(path) else b""
    check(not err and data.startswith(b"%PDF"), "the project's pic_programmer.pdf")
    check(len(re.findall(rb"/Type\s*/Page[^s]", data)) == 2 and "page 2 /pic_sockets/" in t, "2 pages: / and /pic_sockets/")
    check(b"24Cxx" in data, "the parts' fields are in it (24Cxx)")

    err, t = m.tool("sch_set_fields", {"parts": {"R1": {"Value": "47k-KICADD"}}})
    check(not err, "an unsaved value on R1")
    err, t = m.tool("sch_export_pdf", {"path": "docs/review"})
    path2 = os.path.join(proj, "docs", "review.pdf")
    data2 = open(path2, "rb").read() if os.path.exists(path2) else b""
    check(not err and data2.startswith(b"%PDF"), "relative path: docs/review.pdf in the project (folder made)")
    check(b"47k-KICADD" in data2 and b"47k-KICADD" not in data, "the unsaved value is in the new PDF")
    check("47k-KICADD" not in open(sch_file, encoding="utf-8").read(), "and not in the file on disk")

    err, t = m.tool("sch_export_pdf", {"path": "bw.pdf", "black_and_white": True})
    bw = open(os.path.join(proj, "bw.pdf"), "rb").read() if os.path.exists(os.path.join(proj, "bw.pdf")) else b""
    colour = rb"0.517647 0 0 rg"   # the frame's dark red
    check(not err and colour in pdf_streams(data) and colour not in pdf_streams(bw), "black_and_white: no colour")
    m.tool("sch_undo")
    m.close()
finally:
    srv.stop()

print("\nRESULT:", "PASS" if not FAILS else f"FAIL ({len(FAILS)})")
sys.exit(1 if FAILS else 0)

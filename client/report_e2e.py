"""What the tools say they did (headless): nothing left out, removed or overwritten without a word.

usage: python3 -I report_e2e.py --build BUILD --demos DEMOS_DIR --cache LCSC_CACHE --work DIR

On a copy of KiCad's pic_programmer demo, with an LCSC cache of its own (offline):
  render      board_render draws the values (a fab plot does): a longer value, more ink on F.Fab
  pour        a pour with nothing of its net in it: the refill says it is floating (or gone), and why
  kept        a footprint edited in lcsc.pretty is kept when the part is fetched or placed again
              (the edit is what gets placed); refresh=true replaces it; fetching an unchanged one
              writes no note
  npth        a shell whose legs come as unnumbered non-plated holes with copper (EasyEDA's
              C456012): counted as unnumbered pads, numbering plates them; fetching it again keeps
              the numbered footprint without calling it an edit
"""

import argparse
import json
import os
import re
import shutil
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from m5_e2e import FAILS, Mcp, Server, binaries, check, png_pixels  # noqa: E402

FAB = (175, 175, 175)   # F.Fab in board_render's palette


def fab_ink(png):
    _, _, rows = png_pixels(png)
    return sum(1 for row in rows for p in row if abs(p[0] - FAB[0]) < 30 and abs(p[1] - FAB[1]) < 30
               and abs(p[2] - FAB[2]) < 30)


def shell_as_npth(src, dst, part):
    """The cached EasyEDA part with its numbered through-hole shell legs made pad "", not plated."""
    for name in os.listdir(src):
        if name.startswith(part + "."):
            shutil.copy(os.path.join(src, name), os.path.join(dst, name))
    path = os.path.join(dst, part + ".json")
    data = json.load(open(path, encoding="utf-8"))
    shapes = data["result"]["packageDetail"]["dataStr"]["shape"]
    n = 0
    for i, s in enumerate(shapes):
        f = s.split("~")
        if f[0] == "PAD" and f[6] == "11" and f[9] not in ("", "0"):   # multi-layer with a hole
            f[8] = ""          # number
            f[15] = "N"        # plated
            shapes[i] = "~".join(f)
            n += 1
    json.dump(data, open(path, "w", encoding="utf-8"))
    return n


def main():
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", required=True)
    ap.add_argument("--demos", required=True)
    ap.add_argument("--cache", required=True)
    ap.add_argument("--work", required=True)
    a = ap.parse_args()
    work = os.path.abspath(a.work)
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(work)
    proj = os.path.join(work, "p")
    shutil.copytree(os.path.join(a.demos, "pic_programmer"), proj)
    cache = os.path.join(work, "cache")
    os.makedirs(cache)
    for name in os.listdir(a.cache):
        if name.startswith("C25804.") or name.startswith("3d-"):
            shutil.copy(os.path.join(a.cache, name), os.path.join(cache, name))
    legs = shell_as_npth(a.cache, cache, "C165948")

    _, kicadd_mcp, env = binaries(os.path.abspath(a.build))
    env = dict(env, KICADD_LCSC_CACHE=cache, KICADD_LCSC_OFFLINE="1")
    sock = os.path.join(work, "s.sock")
    board = os.path.join(proj, "pic_programmer.kicad_pcb")
    server = Server(a.build, sock, board, env, os.path.join(work, "server.log"))
    check(server.start(), "api-server up on pic_programmer")
    m = Mcp([kicadd_mcp, "--socket", sock, "--stdio"], env)
    try:
        print("[render: values drawn]")
        ref = "R1"
        err, t, imgs = m.tool_images("board_render", {"layers": ["fab"], "refs": [ref], "zoom": True, "size": 600})
        check(not err and imgs, f"board_render fab around {ref}: " + t.splitlines()[0][:80] if t else "")
        before = fab_ink(imgs[0]) if imgs else 0
        err, t = m.tool("pcb_set_fields", {"parts": {ref: {"Value": "100k_VALUE_DRAWN_ON_FAB"}}})
        check(not err, f"{ref}'s value made longer: " + t.splitlines()[0][:80])
        err, t, imgs = m.tool_images("board_render", {"layers": ["fab"], "refs": [ref], "zoom": True, "size": 600})
        after = fab_ink(imgs[0]) if imgs else 0
        check(after > before * 1.2, f"more F.Fab ink with the longer value ({before} -> {after} px): values are drawn")
        m.tool("undo", {})

        print("[pour: an empty refill is said]")
        # a 4 x 4 mm square in an empty corner of the board, on a net with no copper there
        err, t = m.tool("zones", {"create": {"net": "VCC", "layers": ["B.Cu"], "name": "lonely",
                                             "rect": [75.0, 42.0, 79.0, 46.0]}})
        def said(t):
            return next((ln.strip() for ln in t.splitlines()
                         if "lonely" in ln and ("no fill at all" in ln or "joined to nothing" in ln)), "")
        line = said(t)
        check(line and "via" in line, "zones create on VCC where no VCC copper is: said, with what to do: " + (line or t[:200]))
        err, t = m.tool("refill_zones", {})
        check(said(t), "refill_zones says it again: " + (said(t) or t.strip()[:160]))
        m.tool("undo", {})

        print("[kept: an edited library footprint]")
        mod = os.path.join(proj, "lcsc.pretty", "R0603.kicad_mod")
        err, t = m.tool("lcsc_fetch", {"lcsc": "C25804"})
        check(not err and os.path.isfile(mod), "C25804 fetched into lcsc.pretty/R0603.kicad_mod")
        err, t = m.tool("lcsc_fetch", {"lcsc": "C25804"})
        check(not err and "kept" not in t, "fetched again, unchanged: no 'kept' note")
        text = open(mod, encoding="utf-8").read()
        edited, n = re.subn(r'(\(pad "1"[^\n]*\n(?:[^\n]*\n)*?\s*\(size )[\d.]+ [\d.]+', r"\g<1>1.234 1.234", text, count=1)
        check(n == 1, "pad 1 of R0603.kicad_mod made 1.234 mm by hand")
        open(mod, "w", encoding="utf-8").write(edited)
        err, t = m.tool("lcsc_fetch", {"lcsc": "C25804"})
        check(not err and "kept lcsc.pretty/R0603.kicad_mod" in t and "1.234 1.234" in open(mod, encoding="utf-8").read(),
              "fetched again: the edit is kept and said: " + next((ln.strip() for ln in t.splitlines() if "kept" in ln), t[:120]))
        err, t = m.tool("add_footprint", {"lcsc": "C25804", "ref": "R900", "x": 60, "y": 60})
        check(not err and "kept" in t, "add_footprint lcsc=: says the library's own footprint is placed")
        err, t = m.tool("save_board", {})
        pcb = open(board, encoding="utf-8").read()
        r900 = pcb[pcb.find('"R900"') - 2000:pcb.find('"R900"') + 6000] if '"R900"' in pcb else ""
        check("1.234 1.234" in r900, "R900 on the board has the edited pad (1.234 mm)")
        err, t = m.tool("lcsc_fetch", {"lcsc": "C25804", "refresh": True})
        check(not err and "kept" not in t and "1.234" not in open(mod, encoding="utf-8").read(),
              "refresh=true: replaced by EasyEDA's")

        print("[npth: copper on unplated shell legs]")
        check(legs >= 2, f"C165948 in the test cache: {legs} shell legs made pad \"\" / not plated")
        err, t = m.tool("lcsc_fetch", {"lcsc": "C165948"})
        line = next((ln.strip() for ln in t.splitlines() if "with no number" in ln), t.strip()[:200])
        check(not err and f"{legs} of them non-plated holes with copper" in line and line.startswith(f"note: {legs} ")
              or f" {legs} copper pad(s) with no number" in line,
              f"fetched: the {legs} unplated legs count as shell pads (not the locating pegs): " + line[:200])
        usb = [f for f in os.listdir(os.path.join(proj, "lcsc.pretty")) if f != "R0603.kicad_mod"]
        check(len(usb) == 1, "its footprint in lcsc.pretty: " + ", ".join(usb))
        pin = re.search(r"symbol's pins with no pad: ([^)\s,]+)", line)
        err, t = m.tool("lcsc_fetch", {"lcsc": "C165948", "unnumbered_pads": pin.group(1) if pin else "1"})
        line = next((ln.strip() for ln in t.splitlines() if "numbered" in ln), t.strip()[:200])
        check(not err and f"({legs} non-plated holes with copper made plated)" in line,
              "unnumbered_pads: numbered and plated: " + line[:200])
        fp = open(os.path.join(proj, "lcsc.pretty", usb[0]), encoding="utf-8").read() if usb else ""
        check(fp.count("np_thru_hole") == 2, f"the library footprint: the 2 locating pegs still non-plated "
              f"({fp.count('np_thru_hole')} np_thru_hole)")
        err, t = m.tool("lcsc_fetch", {"lcsc": "C165948"})
        check(not err and "kept" not in t and open(os.path.join(proj, "lcsc.pretty", usb[0]),
                                                   encoding="utf-8").read().count("np_thru_hole") == 2,
              "fetched once more: the numbered, plated footprint stays, not called an edit")
    finally:
        m.close()
        server.stop()

    print("\nRESULT:", "PASS" if not FAILS else f"FAIL ({len(FAILS)})")
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())

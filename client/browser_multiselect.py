"""Browser: multi-select (Shift+click, box select), group drag / rotate / flip / delete, undo;
silk follows the parts. Headless Chromium against a disposable copy of the Jetson board.

usage: python -I browser_multiselect.py <base-url> <screenshot-dir>
"""

import math
import os
import sys

from playwright.sync_api import sync_playwright

FAILS = []


def check(cond, label):
    print(("  ok   " if cond else "  FAIL ") + label)
    if not cond:
        FAILS.append(label)


STATE = """(refs) => refs.map(r => { const f = [...board.fp.values()].find(f => f.ref === r);
    return f ? { id: f.id, x: f.x, y: f.y, r: f.r, back: !!f.back, bb: f.bb,
                 g0: f.gx && f.gx.length ? [f.gx[0].pts[0], f.gx[0].pts[1]] : null } : null; })"""


def main(base, shots):
    with sync_playwright() as p:
        browser = p.chromium.launch()
        page = browser.new_page(viewport={"width": 1500, "height": 950})
        page.on("pageerror", lambda e: print("pageerror:", e))
        page.goto(base + "/")
        page.wait_for_function("document.getElementById('conn').textContent === 'live' && artLoaded.has('F.SilkS')",
                               timeout=60000)
        page.wait_for_timeout(400)
        refs = ["U78", "C216"]
        page.evaluate("""() => { const fs = ['U78', 'C216', 'C356'].map(r => [...board.fp.values()].find(f => f.ref === r));
            const x0 = Math.min(...fs.map(f => f.bb[0])), y0 = Math.min(...fs.map(f => f.bb[1]));
            const x1 = Math.max(...fs.map(f => f.bb[2])), y1 = Math.max(...fs.map(f => f.bb[3]));
            fit([x0 - 2000, y0 - 2000, x1 + 2000, y1 + 2000]); }""")
        page.wait_for_timeout(200)

        def screen(wx, wy):
            r = page.evaluate("""([x, y]) => { const r = cv.getBoundingClientRect();
                return [r.left + view.ox + x * view.s, r.top + view.oy + y * view.s]; }""", [wx, wy])
            return r[0], r[1]

        def centre(st):
            return screen((st["bb"][0] + st["bb"][2]) / 2, (st["bb"][1] + st["bb"][3]) / 2)

        s0 = page.evaluate(STATE, refs)
        sel = lambda: page.evaluate("selFps().map(f => f.ref).sort()")

        print("[select]")
        page.mouse.click(*centre(s0[0]))
        page.keyboard.down("Shift"); page.mouse.click(*centre(s0[1])); page.keyboard.up("Shift")
        check(sel() == sorted(refs), f"click + Shift+click -> {sel()}")
        check("2 footprints" in page.inner_text("#sel"), "selection panel shows 2 footprints")
        page.keyboard.down("Shift"); page.mouse.click(*centre(s0[1])); page.keyboard.up("Shift")
        check(sel() == ["U78"], "Shift+click again toggles C216 off")

        # Box select left -> right: only enclosed parts
        u = s0[0]
        a = screen(u["bb"][0] - 300, u["bb"][1] - 300)
        b = screen(u["bb"][2] + 300, u["bb"][3] + 300)
        page.keyboard.press("Escape")
        page.mouse.move(*a); page.mouse.down(); page.mouse.move(*b, steps=5); page.mouse.up()
        enclosed = page.evaluate("""([x0, y0, x1, y1]) => [...board.fp.values()].filter(f => !f.back &&
            f.bb[0] >= x0 && f.bb[2] <= x1 && f.bb[1] >= y0 && f.bb[3] <= y1).map(f => f.ref).sort()""",
                                 [u["bb"][0] - 300, u["bb"][1] - 300, u["bb"][2] + 300, u["bb"][3] + 300])
        check("U78" in sel() and sel() == enclosed, f"box left->right selects the enclosed parts {sel()}")
        page.mouse.move(*b); page.mouse.down(); page.mouse.move(*a, steps=5); page.mouse.up()
        check(len(sel()) > len(enclosed), f"box right->left also takes touched parts ({len(sel())} > {len(enclosed)})")
        page.screenshot(path=os.path.join(shots, "multi-box.png"))

        print("[group edits]")
        page.keyboard.press("Escape")
        page.mouse.click(*centre(s0[0]))
        page.keyboard.down("Shift"); page.mouse.click(*centre(s0[1])); page.keyboard.up("Shift")

        # drag the pair by grabbing U78
        start = centre(s0[0])
        px_per_mm = page.evaluate("view.s * 1000")
        page.mouse.move(*start); page.mouse.down()
        page.mouse.move(start[0] + 2 * px_per_mm, start[1] + 1 * px_per_mm, steps=8); page.mouse.up()
        page.wait_for_function(f"(() => {{ const f = [...board.fp.values()].find(f => f.ref === 'C216'); return f && f.x !== {s0[1]['x']}; }})()",
                               timeout=10000)
        page.wait_for_timeout(300)
        s1 = page.evaluate(STATE, refs)
        d = [(s1[i]["x"] - s0[i]["x"], s1[i]["y"] - s0[i]["y"]) for i in range(2)]
        check(d[0] == d[1] and d[0] != (0, 0), f"dragging one moves both by the same delta {d[0]} µm")
        check(s1[0]["g0"] and (s1[0]["g0"][0] - s0[0]["g0"][0], s1[0]["g0"][1] - s0[0]["g0"][1]) == d[0],
              "U78's silkscreen moved with it")

        # rotate the pair as a group
        bb = [min(s["bb"][0] for s in s1), min(s["bb"][1] for s in s1), max(s["bb"][2] for s in s1), max(s["bb"][3] for s in s1)]
        cx, cy = (bb[0] + bb[2]) / 2, (bb[1] + bb[3]) / 2
        page.keyboard.press("r")
        page.wait_for_function(f"(() => {{ const f = [...board.fp.values()].find(f => f.ref === 'C216'); return f && f.r !== {s1[1]['r']}; }})()",
                               timeout=10000)
        page.wait_for_timeout(300)
        s2 = page.evaluate(STATE, refs)
        ok = True
        for a_, b_ in zip(s1, s2):
            ex, ey = cx + (a_["y"] - cy), cy - (a_["x"] - cx)   # +90° in KiCad (Y down)
            ok &= abs(b_["x"] - ex) <= 2 and abs(b_["y"] - ey) <= 2 and (b_["r"] - a_["r"]) % 360 == 90
        check(ok, "R rotates the pair +90° around their common centre")

        page.keyboard.press("f")
        page.wait_for_function("(() => { const f = [...board.fp.values()].find(f => f.ref === 'C216'); return f && f.back; })()",
                               timeout=10000)
        page.wait_for_timeout(300)
        s3 = page.evaluate(STATE, refs)
        bb = [min(s["bb"][0] for s in s2), max(s["bb"][2] for s in s2)]
        mx = (bb[0] + bb[1]) / 2
        check(all(s["back"] for s in s3) and all(abs(b_["x"] - (2 * mx - a_["x"])) <= 2 and b_["y"] == a_["y"]
                                                  for a_, b_ in zip(s2, s3)),
              "F flips the pair to the back, mirrored around their common centre")
        page.screenshot(path=os.path.join(shots, "multi-flipped.png"))

        page.keyboard.press("Delete")
        page.wait_for_function("![...board.fp.values()].some(f => f.ref === 'U78' || f.ref === 'C216')", timeout=10000)
        check(sel() == [], "Del deletes both, selection empty")

        for _ in range(5):   # delete, flip, rotate, move ... and one extra (nothing left)
            page.keyboard.press("Control+z")
            page.wait_for_timeout(500)
        s5 = page.evaluate(STATE, refs)
        check(all(a_ and b_ and (a_["x"], a_["y"], a_["r"], a_["back"]) == (b_["x"], b_["y"], b_["r"], b_["back"])
                  for a_, b_ in zip(s0, s5)), "undo x4 restores both exactly")
        browser.close()

    print("\nRESULT:", "PASS" if not FAILS else f"FAIL ({len(FAILS)})")
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1].rstrip("/"), sys.argv[2]))

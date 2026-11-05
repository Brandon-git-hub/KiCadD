"""Browser: flip (F), delete (Del), add (A, with the two prompts), undo — in headless Chromium.

usage: python -I browser_layout.py <base-url> <screenshot-dir>
The daemon must serve a disposable copy of the Jetson board (U78 is used); everything is undone.
"""

import os
import sys

from playwright.sync_api import sync_playwright

FAILS = []


def check(cond, label):
    print(("  ok   " if cond else "  FAIL ") + label)
    if not cond:
        FAILS.append(label)


def main(base, shots):
    with sync_playwright() as p:
        browser = p.chromium.launch()
        page = browser.new_page(viewport={"width": 1500, "height": 950})
        answers = []
        page.on("dialog", lambda d: d.accept(answers.pop(0) if answers else ""))
        page.goto(base + "/")
        page.wait_for_function("document.getElementById('conn').textContent === 'live' && board.fp.size > 0",
                               timeout=60000)
        page.wait_for_timeout(500)
        fp = "(ref) => { const f = [...board.fp.values()].find(f => f.ref === ref); return f ? {id: f.id, back: f.back, " \
             "x: f.x, y: f.y, side: f.pads.map(p => p.side)} : null; }"
        u0 = page.evaluate(fp, "U78")
        page.evaluate("""() => { const f = [...board.fp.values()].find(f => f.ref === 'U78');
                                 select([f.id]); fit([f.bb[0]-3000, f.bb[1]-3000, f.bb[2]+3000, f.bb[3]+3000]); }""")
        page.mouse.move(700, 450)

        page.keyboard.press("f")
        page.wait_for_function("(() => { const f = selFps()[0]; return f && f.back; })()", timeout=10000)
        u1 = page.evaluate(fp, "U78")
        check(u1["back"] and (u1["x"], u1["y"]) == (u0["x"], u0["y"]) and set(u1["side"]) != set(u0["side"]),
              "F flips the selected part to the back (pads change side, anchor stays)")
        check("Back" in page.inner_text("#sel"), "selection panel shows Side: Back")
        page.screenshot(path=os.path.join(shots, "layout-flip.png"))

        page.keyboard.press("Delete")
        page.wait_for_function("![...board.fp.values()].some(f => f.ref === 'U78')", timeout=10000)
        check("Click a footprint" in page.inner_text("#sel"), "Del deletes it; selection cleared")

        page.keyboard.press("Control+z")
        page.wait_for_function("[...board.fp.values()].some(f => f.ref === 'U78')", timeout=10000)
        check(page.evaluate(fp, "U78")["back"], "Ctrl+Z brings it back (still flipped)")
        page.keyboard.press("Control+z")
        page.wait_for_function("(() => { const f = [...board.fp.values()].find(f => f.ref === 'U78'); return f && !f.back; })()",
                               timeout=10000)
        check(page.evaluate(fp, "U78") == u0, "second Ctrl+Z: front again, identical")

        n = page.evaluate("board.fp.size")
        answers[:] = ["Capacitor_SMD:C_0603_1608Metric", "C?"]
        page.mouse.move(650, 420)
        page.keyboard.press("a")
        page.wait_for_function(f"board.fp.size === {n + 1} && selFps().length === 1 && selFps()[0].ref.startsWith('C')",
                               timeout=10000)
        added = page.evaluate("selFps()[0]")
        check(added["val"] and len(added["pads"]) == 2, f"A adds {added['ref']} ({added['val']}) at the cursor, selected")
        log = page.inner_text("#log")
        check("Capacitor_SMD.pretty" in log, "activity log names the library file")
        page.screenshot(path=os.path.join(shots, "layout-add.png"))
        page.keyboard.press("Control+z")
        page.wait_for_function(f"board.fp.size === {n}", timeout=10000)
        check(True, "Ctrl+Z removes the added part")
        browser.close()

    print("\nRESULT:", "PASS" if not FAILS else f"FAIL ({len(FAILS)})")
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1].rstrip("/"), sys.argv[2]))

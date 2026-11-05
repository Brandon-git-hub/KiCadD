"""Drive the 2D view in headless Chromium: render, drag a footprint, watch an agent edit arrive.

usage: python -I browser_check.py <base-url> <screenshot-dir>
"""

import json
import os
import sys
import time
import urllib.request

from playwright.sync_api import sync_playwright

FAILS = []


def check(cond, label):
    print(("  ok   " if cond else "  FAIL ") + label)
    if not cond:
        FAILS.append(label)


def mcp_call(base, name, args):
    body = json.dumps({"jsonrpc": "2.0", "id": 1, "method": "tools/call",
                       "params": {"name": name, "arguments": args}}).encode()
    req = urllib.request.Request(base + "/mcp", data=body, headers={"Content-Type": "application/json"})
    return json.load(urllib.request.urlopen(req))["result"]["content"][0]["text"]


def screen_of(page, ref):
    """Screen position (CSS px, page coords) of a footprint's bbox centre."""
    return page.evaluate("""(ref) => {
        const f = [...board.fp.values()].find(f => f.ref === ref);
        const r = cv.getBoundingClientRect();
        const x = (f.bb[0] + f.bb[2]) / 2, y = (f.bb[1] + f.bb[3]) / 2;
        return [r.left + x * view.s + view.ox, r.top + y * view.s + view.oy, f.x, f.y];
    }""", ref)


def main(base, outdir):
    os.makedirs(outdir, exist_ok=True)
    errors = []
    with sync_playwright() as p:
        browser = p.chromium.launch()
        page = browser.new_page(viewport={"width": 1600, "height": 950}, device_scale_factor=1)
        page.on("console", lambda m: errors.append(m.text) if m.type == "error" else None)
        page.on("pageerror", lambda e: errors.append(str(e)))

        t0 = time.perf_counter()
        page.goto(base + "/")
        page.wait_for_function("document.getElementById('conn').textContent === 'live' && board.fp.size > 0",
                               timeout=30000)
        t_load = (time.perf_counter() - t0) * 1000
        page.wait_for_timeout(300)
        check(True, f"page live with snapshot in {t_load:.0f} ms")

        # Measure a full redraw (cache rebuild + draw) and a pan-only redraw.
        times = page.evaluate("""() => {
            const t0 = performance.now(); fpCache.clear(); trkDirty = fpDirty = true; draw(); const t1 = performance.now();
            for (let i = 0; i < 10; i++) { view.ox += 1; draw(); } const t2 = performance.now();
            return [t1 - t0, (t2 - t1) / 10];
        }""")
        one = page.evaluate("""() => {
            const f = [...board.fp.values()].find(f => f.ref === 'U78');
            const t0 = performance.now(); fpCache.delete(f.id); fpDirty = true; draw(); return performance.now() - t0;
        }""")
        check(times[0] < 80 and one < 20,
              f"cold cache build+draw {times[0]:.0f} ms; redraw after one footprint changes {one:.1f} ms")
        page.screenshot(path=f"{outdir}/1_board.png")

        # Zoom into U78 / C216.
        page.evaluate("""() => { const f = [...board.fp.values()].find(f => f.ref === 'U78');
                                 const m = 5000; fit([f.bb[0]-m, f.bb[1]-m, f.bb[2]+m, f.bb[3]+m]); }""")
        page.wait_for_timeout(200)
        page.screenshot(path=f"{outdir}/2_zoom.png")

        # Drag C216 left by ~1 mm with the mouse.
        sx, sy, x0, _ = screen_of(page, "C216")
        px_per_mm = page.evaluate("view.s * 1000")
        page.mouse.move(sx, sy)
        page.mouse.down()
        for i in range(1, 11):
            page.mouse.move(sx - px_per_mm * 1.0 * i / 10, sy)
        page.wait_for_timeout(100)
        page.screenshot(path=f"{outdir}/3_dragging.png")
        page.mouse.up()
        page.wait_for_function("document.getElementById('drcCount').textContent !== ''", timeout=10000)
        page.wait_for_timeout(300)
        drc_text = page.inner_text("#drc")
        _, _, x1, _ = screen_of(page, "C216")
        check(abs((x1 - x0) + 1000) <= 60, f"drag moved C216 by {(x1 - x0) / 1000:.2f} mm on the server (snap 0.05)")
        check("short" in drc_text, f"DRC panel lists violations: {page.inner_text('#drcCount')}")
        lat = page.inner_text("#lat")
        check(lat.startswith("move:"), f"UI round trip after drop: '{lat}'")
        page.screenshot(path=f"{outdir}/4_after_drop_drc.png")

        # Agent moves it back via MCP; the view must update and flash it.
        t0 = time.perf_counter()
        mcp_call(base, "move_footprints", {"refs": ["C216"], "dx": round(-(x1 - x0) / 1000, 3)})
        page.wait_for_function(f"[...board.fp.values()].find(f => f.ref === 'C216').x === {x0}", timeout=5000)
        t_agent = (time.perf_counter() - t0) * 1000
        log = page.inner_text("#log")
        check("remote edit: C216" in log, f"agent edit shown in the view after {t_agent:.0f} ms and logged as remote")
        page.wait_for_timeout(150)
        page.screenshot(path=f"{outdir}/5_agent_moved_back.png")

        # Keyboard rotate.
        page.mouse.click(*screen_of(page, "C216")[:2])
        r0 = page.evaluate("[...board.fp.values()].find(f => f.ref === 'C216').r")
        page.keyboard.press("r")
        page.wait_for_function(f"[...board.fp.values()].find(f => f.ref === 'C216').r !== {r0}", timeout=5000)
        page.keyboard.press("Shift+R")
        page.wait_for_function(f"[...board.fp.values()].find(f => f.ref === 'C216').r === {r0}", timeout=5000)
        check(True, "R / Shift+R rotate the selected footprint and back")

        check(not errors, f"no console errors{': ' + '; '.join(errors[:3]) if errors else ''}")
        browser.close()

    print("\nRESULT:", "PASS" if not FAILS else f"FAIL ({len(FAILS)})")
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1].rstrip("/"), sys.argv[2]))

"""The 2D view's page in a real browser (headless Chromium through Playwright): what the
WebSocket tests cannot see.

usage: python -I webui_e2e.py <base-url> [--chromium /usr/bin/chromium] [--shots DIR]

  net        double-click a pad: its net is highlighted (label, pad count); a net named in the
             selection panel highlights it too; Esc clears
  agent      the Agent section lists the agent's edit; Pause / Resume reach KiCad (the agent's
             move is refused while paused)
  schematic  the Schematic tab shows a sheet (KiCad's SVG); a click on a part selects its
             footprint on the board; a click on a wire highlights the net (the board too); a
             sub-sheet opens from the picker; an agent's sch_set_fields reloads the sheet
             (KiCad's "sch" notification) with the new value; the edit undone
  chart      pdn on the rail with the most decoupling capacitors: the Chart tab gets its |Z|
             (a badge on the tab, the chart's title, the cursor's readout, a legend entry
             switched off, wheel zoom, double-click back)
"""

import argparse
import json
import os
import sys
import urllib.request

from playwright.sync_api import sync_playwright

FAILS = []


def check(cond, label):
    print(("  ok   " if cond else "  FAIL ") + label, flush=True)
    if not cond:
        FAILS.append(label)


def mcp_call(base, name, args):
    body = json.dumps({"jsonrpc": "2.0", "id": 1, "method": "tools/call",
                       "params": {"name": name, "arguments": args}}).encode()
    req = urllib.request.Request(base + "/mcp", data=body, headers={"Content-Type": "application/json"})
    return json.load(urllib.request.urlopen(req))["result"]["content"][0]["text"]


# a pad of a net with several pads, on the front, in screen pixels (and its footprint's ref)
FIND_PAD = """() => {
  const count = new Map();
  for (const f of board.fp.values()) for (const p of f.pads) if (p.net) count.set(p.net, (count.get(p.net) || 0) + 1);
  for (const f of board.fp.values()) {
    if (f.back) continue;
    for (const p of f.pads) {
      const n = count.get(p.net) || 0;
      if (n < 3 || n > 40 || p.side !== 0 && p.side !== 2) continue;
      // zoomed in on it (40 px per mm), centred: on a large board a pad is a pixel at fit
      const r = cv.getBoundingClientRect();
      view.s = 0.04; view.ox = r.width / 2 - p.x * view.s; view.oy = r.height / 2 - p.y * view.s; dirty = true;
      return { net: p.net, ref: f.ref, n, x: r.left + r.width / 2, y: r.top + r.height / 2 };
    }
  }
  return null;
}"""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("base")
    ap.add_argument("--chromium", default="/usr/bin/chromium")
    ap.add_argument("--shots", default="")
    a = ap.parse_args()
    base = a.base.rstrip("/")
    if a.shots:
        os.makedirs(a.shots, exist_ok=True)

    with sync_playwright() as pw:
        browser = pw.chromium.launch(executable_path=a.chromium, args=["--no-sandbox"])
        page = browser.new_page(viewport={"width": 1400, "height": 850})
        errors = []
        page.on("pageerror", lambda e: errors.append(str(e)))
        page.goto(base + "/")
        page.wait_for_function("() => typeof board !== 'undefined' && board.fp.size > 0", timeout=60000)
        page.wait_for_timeout(500)

        print("[net]")
        pad = page.evaluate(FIND_PAD)
        check(pad is not None, f"a front pad on a net with several pads: {pad and pad['ref']} on {pad and pad['net']}")
        if pad:
            page.mouse.dblclick(pad["x"], pad["y"])
            page.wait_for_timeout(300)
            hi = page.evaluate("() => hiNet")
            label = page.inner_text("#hiNet")
            check(hi == pad["net"], f"double-click highlights {pad['net']} (got {hi})")
            check(page.is_visible("#hiNet") and f"{pad['n']} pads" in label, f"the label: {label.strip()}")
            if a.shots:
                page.screenshot(path=os.path.join(a.shots, "webui-net.png"))
            page.keyboard.press("Escape")
            page.wait_for_timeout(200)
            check(page.evaluate("() => hiNet") is None and not page.is_visible("#hiNet"), "Esc clears it")

            # the selection panel's net names
            page.mouse.click(pad["x"], pad["y"])
            page.wait_for_timeout(300)
            link = page.locator("#selNets .netlink", has_text=pad["net"]).first
            check(link.count() == 1, f"{pad['ref']} selected: its nets are listed")
            if link.count():
                link.click()
                page.wait_for_timeout(200)
                check(page.evaluate("() => hiNet") == pad["net"], "a net name in the selection panel highlights it")
                page.keyboard.press("Escape")

        print("[agent]")
        mcp_call(base, "move_footprints", {"refs": [pad["ref"] if pad else "R1"], "dx": 0.5})
        page.wait_for_function("() => agent && agent.events.length > 0", timeout=5000)
        page.wait_for_timeout(300)
        text = page.inner_text("#agentLog")
        check("move" in text, "the Agent section lists the agent's move: " + text.splitlines()[0][:60])
        page.click("#pauseAgent")
        page.wait_for_function("() => agent && agent.paused", timeout=5000)
        check(page.is_visible("#agentPill") and page.inner_text("#pauseAgent") == "Resume", "Pause: the header says so, the button resumes")
        out = mcp_call(base, "move_footprints", {"refs": [pad["ref"] if pad else "R1"], "dx": -0.5})
        check("paused" in out, "the agent's move is refused while paused")
        if a.shots:
            page.wait_for_timeout(500)
            page.screenshot(path=os.path.join(a.shots, "webui-paused.png"))
        page.click("#pauseAgent")
        page.wait_for_function("() => agent && !agent.paused", timeout=5000)
        out = mcp_call(base, "move_footprints", {"refs": [pad["ref"] if pad else "R1"], "dx": -0.5})
        check("paused" not in out, "Resume: the agent's move goes through")

        print("[schematic]")
        page.click("#tabs button[data-tab=sch]")
        try:
            page.wait_for_function("() => sch.loaded", timeout=30000)
        except Exception:
            pass
        loaded = page.evaluate("() => sch.loaded")
        check(loaded and page.evaluate("() => !!document.querySelector('#schSvg svg')"),
              "the root sheet as SVG: " + (page.evaluate("() => `${sch.msg.symbols.length} parts, ${sch.msg.nets.length} nets, ${sch.msg.sheets.length} sheets, ${(sch.msg.svg.length / 1e6).toFixed(1)} MB`") if loaded else "-"))
        if loaded:
            page.wait_for_timeout(300)
            hit = page.evaluate("""() => {
              const r = $('schPane').getBoundingClientRect();
              const at = (x, y) => ({ x: r.left + sch.x + x * sch.s, y: r.top + sch.y + y * sch.s });
              const onBoard = new Set([...board.fp.values()].map(f => f.ref));
              const s = sch.msg.symbols.find(s => onBoard.has(s[0]) && (s[3][2] - s[3][0]) * sch.s > 12);
              const n = sch.msg.nets.find(n => n.s.length >= 4 && Math.hypot(n.s[2] - n.s[0], n.s[3] - n.s[1]) * sch.s > 20);
              return { part: s && { ref: s[0], ...at((s[3][0] + s[3][2]) / 2, (s[3][1] + s[3][3]) / 2) },
                       net: n && { n: n.n, ...at((n.s[0] + n.s[2]) / 2, (n.s[1] + n.s[3]) / 2) } };
            }""")
            if hit["part"]:
                # zoom so the part is large enough to hit, around it
                page.mouse.click(hit["part"]["x"], hit["part"]["y"])
                page.wait_for_timeout(300)
                check(page.evaluate("() => selFps().map(f => f.ref)") == [hit["part"]["ref"]],
                      f"a click on {hit['part']['ref']} selects its footprint on the board")
                check(page.evaluate("() => $('schOver').querySelectorAll('rect').length") >= 1, "... framed in the schematic")
            if hit["net"]:
                page.mouse.click(hit["net"]["x"], hit["net"]["y"])
                page.wait_for_timeout(300)
                hi = page.evaluate("() => hiNet")
                check(hi is not None and hi.strip("/") == hit["net"]["n"].strip("/"),
                      f"a click on a wire of {hit['net']['n']} highlights it (board: {hi})")
                check(page.evaluate("() => $('schOver').querySelectorAll('line').length") >= 1, "... drawn bright in the schematic")
            if a.shots:
                page.screenshot(path=os.path.join(a.shots, "webui-schematic.png"))
            sheets = page.evaluate("() => sch.msg.sheets.map(s => s[1])")
            if len(sheets) > 1:
                page.select_option("#schPick", sheets[1])
                page.wait_for_function(f"() => sch.sheet === {json.dumps(sheets[1])}", timeout=20000)
                check(True, f"the picker opens {sheets[1]}")
            # an agent edit on the shown sheet: the view reloads it
            ref, val = page.evaluate("() => [sch.msg.symbols[0][0], sch.msg.symbols[0][1]]")
            mcp_call(base, "sch_set_fields", {"parts": {ref: {"Value": val + "_X"}}})
            try:
                page.wait_for_function(f"() => sch.msg.symbols.some(s => s[0] === {json.dumps(ref)} && s[1] === {json.dumps(val + '_X')})", timeout=10000)
                ok = True
            except Exception:
                ok = False
            check(ok, f"the agent's sch_set_fields on {ref} reloads the sheet with the new value")
            mcp_call(base, "sch_undo", {})
            page.click("#tabs button[data-tab=board]")

        print("[chart]")
        rail = page.evaluate("""() => {
          const caps = new Map();
          for (const f of board.fp.values()) {
            if (!/^C\\d/.test(f.ref) || f.pads.length !== 2) continue;
            const [a, b] = f.pads.map(p => p.net);
            const r = /GND/i.test(a) ? b : /GND/i.test(b) ? a : null;
            if (r && !/GND/i.test(r)) caps.set(r, (caps.get(r) || 0) + 1);
          }
          return [...caps].sort((x, y) => y[1] - x[1])[0] || null;
        }""")
        check(rail is not None, f"the rail with the most decoupling: {rail}")
        if rail:
            out = mcp_call(base, "pdn", {"net": rail[0]})
            check(out.startswith("pdn "), "pdn: " + out.splitlines()[0][:80])
            page.wait_for_function("() => charts.length > 0", timeout=10000)
            check(page.inner_text("#chartBadge") == "●", "a badge on the Chart tab")
            page.click("#tabs button[data-tab=chart]")
            page.wait_for_timeout(300)
            check(page.is_visible("#ch") and page.evaluate("() => chart && chart.tool") == "pdn"
                  and rail[0] in page.evaluate("() => chart.title"), "the Chart tab shows it: " + page.evaluate("() => chart.title"))
            box = page.locator("#ch").bounding_box()
            page.mouse.move(box["x"] + box["width"] * 0.5, box["y"] + box["height"] * 0.5)
            page.wait_for_timeout(200)
            tip = page.inner_text("#chTip")
            check("Hz" in tip and "Ω" in tip, "the cursor reads the curve: " + tip.replace("\n", " | ")[:80])
            if a.shots:
                page.screenshot(path=os.path.join(a.shots, "webui-chart.png"))
            x0 = page.evaluate("() => chart._xr.x1 / chart._xr.x0")
            page.mouse.wheel(0, -400)
            page.wait_for_timeout(200)
            x1 = page.evaluate("() => chart._xr.x1 / chart._xr.x0")
            check(x1 < x0 / 2, f"wheel zooms in (x span ratio {x0:.0f} -> {x1:.0f})")
            page.mouse.dblclick(box["x"] + box["width"] * 0.5, box["y"] + box["height"] * 0.5)
            page.wait_for_timeout(200)
            check(abs(page.evaluate("() => chart._xr.x1 / chart._xr.x0") - x0) < 1e-6 * x0, "double-click: the whole range again")
            b0 = page.evaluate("() => chLegendBoxes[0]")
            page.mouse.click(box["x"] + (b0["x0"] + b0["x1"]) / 2, box["y"] + (b0["y0"] + b0["y1"]) / 2)
            page.wait_for_timeout(200)
            check(page.evaluate("() => chHidden.size") == 1, "a legend entry switches its curve off")
            page.click("#tabs button[data-tab=board]")

        check(not errors, "no page errors" + (": " + errors[0] if errors else ""))
        browser.close()

    print("\nRESULT:", "PASS" if not FAILS else f"FAIL ({len(FAILS)})")
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())

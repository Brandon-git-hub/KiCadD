"""The 2D view's page in a real browser (headless Chromium through Playwright): what the
WebSocket tests cannot see.

usage: python -I webui_e2e.py <base-url> [--chromium /usr/bin/chromium] [--shots DIR]

  net        double-click a pad: its net is highlighted (label, pad count); a net named in the
             selection panel highlights it too; Esc clears
  agent      the Agent section lists the agent's edit; Pause / Resume reach KiCad (the agent's
             move is refused while paused)
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

        check(not errors, "no page errors" + (": " + errors[0] if errors else ""))
        browser.close()

    print("\nRESULT:", "PASS" if not FAILS else f"FAIL ({len(FAILS)})")
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())

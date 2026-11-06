"""Platform-neutral smoke test of a KiCadD build (Linux build tree or a Windows install).

usage: python -I smoke.py --bin DIR --demos DIR --work DIR [--sock PATH] [--build-tree]

  --bin         directory holding kicad-cli and kicadd-mcp (build tree: pass the build dir root)
  --demos       KiCad's demos/ directory
  --work        scratch directory (created)
  --sock        API socket (default WORK/api.sock; Unix socket paths are limited to ~100 chars)
  --build-tree  binaries are in a build tree (sets KICAD_RUN_FROM_BUILD_DIR=1)

Checks: kicad-cli runs; pcbnew loads a board and runs DRC; eeschema loads a schematic and
exports a SPICE netlist (SPICE models without ngspice); the API server serves a board and
kicadd-mcp answers MCP over stdio. Only the Python standard library is used.
"""

import argparse
import json
import os
import shutil
import subprocess
import sys
import threading
import time

FAILS = []
EXE = ".exe" if os.name == "nt" else ""


def check(cond, label):
    print(("  ok   " if cond else "  FAIL ") + label, flush=True)
    if not cond:
        FAILS.append(label)


def find(bin_dir, name):
    for sub in ("", "bin", "kicad", "kicadd"):
        p = os.path.join(bin_dir, sub, name + EXE)
        if os.path.isfile(p):
            return p
    sys.exit(f"{name}{EXE} not found under {bin_dir}")


def run(cmd, env, timeout=300):
    t0 = time.perf_counter()
    r = subprocess.run(cmd, env=env, capture_output=True, text=True, timeout=timeout)
    ms = (time.perf_counter() - t0) * 1000
    if r.returncode != 0:
        print(f"    $ {' '.join(cmd)}\n    exit {r.returncode}\n{r.stdout[-2000:]}{r.stderr[-2000:]}")
    return r, ms


class Mcp:
    """kicadd-mcp --stdio: newline-delimited JSON-RPC."""

    def __init__(self, cmd, env):
        self.p = subprocess.Popen(cmd, env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.PIPE, text=True, encoding="utf-8")
        self.err = []
        threading.Thread(target=lambda: self.err.extend(self.p.stderr), daemon=True).start()
        self.next_id = 1

    def call(self, method, params=None, timeout=60):
        rid = self.next_id
        self.next_id += 1
        self.p.stdin.write(json.dumps({"jsonrpc": "2.0", "id": rid, "method": method,
                                       "params": params or {}}) + "\n")
        self.p.stdin.flush()
        box = []
        reader = threading.Thread(target=lambda: box.append(self.p.stdout.readline()), daemon=True)
        reader.start()
        reader.join(timeout)
        if not box or not box[0]:
            raise RuntimeError(f"no reply to {method}; stderr: {''.join(self.err)[-1000:]}")
        return json.loads(box[0])

    def notify(self, method):
        self.p.stdin.write(json.dumps({"jsonrpc": "2.0", "method": method}) + "\n")
        self.p.stdin.flush()

    def close(self):
        try:
            self.p.stdin.close()
            self.p.wait(10)
        except Exception:
            self.p.kill()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", required=True)
    ap.add_argument("--demos", required=True)
    ap.add_argument("--work", required=True)
    ap.add_argument("--sock")
    ap.add_argument("--build-tree", action="store_true")
    a = ap.parse_args()

    env = dict(os.environ)
    if a.build_tree:
        env["KICAD_RUN_FROM_BUILD_DIR"] = "1"
    cli, mcp = find(a.bin, "kicad-cli"), find(a.bin, "kicadd-mcp")
    shutil.rmtree(a.work, ignore_errors=True)
    os.makedirs(a.work)

    print("[kicad-cli]")
    r, ms = run([cli, "version", "--format", "about"], env)
    about = r.stdout
    check(r.returncode == 0 and "Version:" in about, f"kicad-cli version ({ms:.0f} ms)")
    for lib in ("OCC", "ngspice"):
        line = next((l.strip() for l in about.splitlines() if l.strip().startswith(lib)), "")
        print(f"         {line}")

    print("[pcbnew: board + DRC]")
    board_dir = os.path.join(a.work, "pic_programmer")
    shutil.copytree(os.path.join(a.demos, "pic_programmer"), board_dir)
    board = os.path.join(board_dir, "pic_programmer.kicad_pcb")
    rpt = os.path.join(a.work, "drc.rpt")
    r, ms = run([cli, "pcb", "drc", "-o", rpt, board], env)
    check(r.returncode == 0 and os.path.isfile(rpt) and "drc report" in open(rpt, encoding="utf-8").read().lower(),
          f"pcb drc on pic_programmer ({ms:.0f} ms)")

    print("[eeschema: schematic + SPICE netlist, no ngspice]")
    cir = os.path.join(a.work, "rectifier.cir")
    sch = os.path.join(a.demos, "simulation", "rectifier", "rectifier.kicad_sch")
    r, ms = run([cli, "sch", "export", "netlist", "--format", "spice", "-o", cir, sch], env)
    text = open(cir, encoding="utf-8").read() if os.path.isfile(cir) else ""
    check(r.returncode == 0 and ".end" in text.lower() and "\nD" in text,
          f"sch export netlist --format spice ({ms:.0f} ms, {len(text.splitlines())} lines)")

    print("[api-server + kicadd-mcp --stdio]")
    sock = a.sock or os.path.join(a.work, "api.sock")
    if os.path.exists(sock):
        os.remove(sock)
    server = subprocess.Popen([cli, "api-server", "--socket", sock, board], env=env,
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    out = []
    threading.Thread(target=lambda: out.extend(server.stdout), daemon=True).start()
    try:
        # The server needs a moment to load the board; kicadd-mcp dials once at start-up, so
        # retry the whole session until it answers.
        t0, last = time.perf_counter(), ""
        while True:
            m = Mcp([mcp, "--socket", sock, "--stdio"], env)
            try:
                init = m.call("initialize", {"protocolVersion": "2025-06-18", "capabilities": {},
                                             "clientInfo": {"name": "kicadd-smoke", "version": "1"}}, 20)
                m.notify("notifications/initialized")
                res = m.call("tools/call", {"name": "board_summary", "arguments": {}}).get("result", {})
                text = "".join(c.get("text", "") for c in res.get("content", []))
                if not res.get("isError") and "footprint" in text.lower():
                    break
                last = text
            except Exception as e:
                last = str(e)
            finally:
                if server.poll() is not None or time.perf_counter() - t0 > 120:
                    m.close()
                    raise RuntimeError(f"server exit={server.poll()}; last: {last[-500:]}")
            m.close()
            time.sleep(1)
        ready = time.perf_counter() - t0
        check("result" in init, f"MCP initialize -> {init['result'].get('serverInfo')}")
        tools = m.call("tools/list").get("result", {}).get("tools", [])
        check(len(tools) >= 10, f"tools/list: {len(tools)} tools")
        check(True, f"board_summary via the API server (ready after {ready:.1f} s): {text.splitlines()[0]}")
        m.close()
    except Exception as e:
        check(False, f"MCP session: {e}")
    finally:
        server.terminate()
        try:
            server.wait(15)
        except subprocess.TimeoutExpired:
            server.kill()
        if FAILS:
            print("    api-server output:\n" + "".join(out)[-3000:])

    print("\nRESULT:", "PASS" if not FAILS else f"FAIL ({len(FAILS)})")
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())

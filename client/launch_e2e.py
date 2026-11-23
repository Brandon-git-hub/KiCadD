"""Start and stop KiCad from MCP (launch_kicad / close_kicad), roadmap phase 4 item 3.

usage: python3 -I launch_e2e.py --build BUILD --kicad KICAD_SRC --work DIR [--desktop] [--display :97]

kicadd-mcp runs on a socket nobody serves yet, then:
  none       board_summary says launch_kicad starts one; close_kicad has nothing to close
  server     launch_kicad mode=server on a project folder: up, the board and schematic named;
             board_summary works; a second launch is refused
  unsaved    a moved part and a changed value: close_kicad refuses and lists both (board_diff);
             after save_board only the schematic; discard=true closes, the process and the
             socket are gone, board_summary fails again
  empty      a server with no project, then new_project on it, close_kicad (all saved)
  refused    a relative path, a missing project, a desktop without a board, a bad mode
  foreign    a server started by someone else on the socket: launch refused, close refused
             (left running)
  http       kicadd-mcp --http without --allow-launch: both refused
  desktop    (--desktop, needs Xvfb + xdotool) the PCB editor on the same socket, an edit
             through it, closed with discard
"""

import argparse
import json
import os
import re
import shutil
import socket
import subprocess
import sys
import threading
import time
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from m5_e2e import FAILS, Mcp, Server, binaries, check  # noqa: E402


def gone(pid):
    if os.name == "nt":
        out = subprocess.run(["tasklist", "/FI", f"PID eq {pid}"], capture_output=True, text=True).stdout
        return str(pid) not in out
    try:
        os.kill(pid, 0)
    except OSError:
        return True
    return False


def pid_of(text):
    m = re.search(r"pid (\d+)", text)
    return int(m.group(1)) if m else 0


def http_tool(port, name, args):
    def post(body, sid=None):
        req = urllib.request.Request(f"http://127.0.0.1:{port}/mcp", data=json.dumps(body).encode(), method="POST",
                                     headers={"Content-Type": "application/json",
                                              "Accept": "application/json, text/event-stream",
                                              **({"Mcp-Session-Id": sid} if sid else {})})
        with urllib.request.urlopen(req, timeout=30) as r:
            raw = r.read().decode()
            sid = r.headers.get("Mcp-Session-Id") or sid
        if raw.startswith("event:") or raw.startswith("data:"):
            raw = "".join(l[5:] for l in raw.splitlines() if l.startswith("data:"))
        return (json.loads(raw) if raw.strip() else {}), sid

    _, sid = post({"jsonrpc": "2.0", "id": 1, "method": "initialize",
                   "params": {"protocolVersion": "2025-06-18", "capabilities": {}, "clientInfo": {"name": "t", "version": "1"}}})
    post({"jsonrpc": "2.0", "method": "notifications/initialized"}, sid)
    r, _ = post({"jsonrpc": "2.0", "id": 2, "method": "tools/call", "params": {"name": name, "arguments": args}}, sid)
    res = r.get("result", {})
    return bool(res.get("isError")), "".join(c.get("text", "") for c in res.get("content", []))


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def main():
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", required=True)
    ap.add_argument("--kicad", required=True)
    ap.add_argument("--work", required=True)
    ap.add_argument("--desktop", action="store_true", help="also the desktop editor (Xvfb + xdotool)")
    ap.add_argument("--display", default=":97")
    a = ap.parse_args()
    work = os.path.abspath(a.work)
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(work)
    pic = os.path.join(work, "pic")
    shutil.copytree(os.path.join(a.kicad, "demos", "pic_programmer"), pic)
    _, kicadd_mcp, env = binaries(os.path.abspath(a.build))
    sock = os.path.join(work, "s.sock")
    m = Mcp([kicadd_mcp, "--socket", sock, "--stdio", "--timeout-ms", "30000"], env)
    started = []
    try:
        print("[none]")
        err, t = m.tool("board_summary")
        check(err and "launch_kicad starts one" in t, "nothing up: board_summary says launch_kicad starts one")
        err, t = m.tool("close_kicad")
        check(err and "nothing to close" in t, "close_kicad: nothing to close")

        print("[server]")
        err, t = m.tool("launch_kicad", {"path": pic})
        print("      ", "\n       ".join(t.splitlines()))
        pid = pid_of(t)
        started.append(pid)
        check(not err and "started the server" in t and "board pic_programmer.kicad_pcb" in t,
              "launch_kicad: the server up on the folder's project, its board named")
        check("schematic pic_programmer.kicad_sch" in t, "its schematic open too")
        err, t = m.tool("board_summary")
        check(not err and "file=pic_programmer.kicad_pcb" in t, "board_summary works on it")
        err, t = m.tool("launch_kicad", {"path": pic})
        check(err and "already running" in t and str(pid) in t, "a second launch refused (" + t.strip()[:80] + ")")

        print("[unsaved]")
        err, t = m.tool("move_footprints", {"refs": ["R1"], "dx": 2})
        check(not err, "R1 moved")
        err, t = m.tool("sch_set_fields", {"parts": {"R1": {"Value": "4k7"}}})
        check(not err, "R1's value changed in the schematic" + ("" if not err else ": " + t[:100]))
        err, t = m.tool("close_kicad")
        print("      ", "\n       ".join(t.splitlines()[:6]))
        check(err and "the board (pic_programmer.kicad_pcb) has unsaved changes" in t and "board_diff" in t,
              "close refused: the board's changes (board_diff)")
        check("the schematic (pic_programmer.kicad_sch) has unsaved changes" in t, "... and the schematic's")
        check(not gone(pid), "still running")
        m.tool("save_board")
        err, t = m.tool("close_kicad")
        check(err and "schematic" in t and "the board (" not in t, "after save_board: only the schematic left")
        err, t = m.tool("close_kicad", {"discard": True})
        check(not err and "closed the server" in t and "dropped (schematic)" in t, "discard=true: closed (" + t.strip()[:90] + ")")
        check(gone(pid), "the process is gone")
        check(os.name == "nt" or not os.path.exists(sock), "the socket is gone")
        err, t = m.tool("board_summary")
        check(err, "board_summary fails again")
        check("4k7" not in open(os.path.join(pic, "pic_programmer.kicad_sch"), encoding="utf-8").read(),
              "the dropped schematic change is not on disk")

        print("[empty]")
        err, t = m.tool("launch_kicad", {})
        pid = pid_of(t)
        started.append(pid)
        check(not err and "no board open" in t, "a server with no project (" + t.strip().splitlines()[-1][:60] + ")")
        err, t = m.tool("new_project", {"path": os.path.join(work, "fresh"), "outline": [30, 20]})
        check(not err and "file=fresh.kicad_pcb" in t, "new_project on it")
        err, t = m.tool("close_kicad")
        check(not err and "dropped" not in t, "closed (all saved)")
        check(gone(pid), "the process is gone")

        print("[refused]")
        nopcb = os.path.join(work, "nopcb")
        os.makedirs(nopcb)
        shutil.copy(os.path.join(pic, "pic_programmer.kicad_sch"), os.path.join(nopcb, "x.kicad_sch"))
        for args, want in (({"path": "pic/pic_programmer.kicad_pro"}, "must be absolute"),
                           ({"path": os.path.join(work, "none.kicad_pro")}, "no such file"),
                           ({"path": os.path.join(nopcb, "x.kicad_sch"), "mode": "desktop"}, "needs a board"),
                           ({"path": work}, ".kicad_pro files"),
                           ({"mode": "gui"}, "mode:")):
            err, t = m.tool("launch_kicad", args)
            check(err and want in t, f"refused: {want} ({t.strip()[:80]})")

        print("[foreign]")
        srv = Server(a.build, sock, os.path.join(pic, "pic_programmer.kicad_pcb"), env, os.path.join(work, "foreign.log"))
        check(srv.start(), "someone else's server on the socket")
        err, t = m.tool("launch_kicad", {"path": pic})
        check(err and "already answers" in t and "not started here" in t, "launch refused (" + t.strip()[:70] + ")")
        err, t = m.tool("close_kicad", {"discard": True})
        check(err and "not started by launch_kicad" in t, "close refused, even with discard")
        check(srv.p.poll() is None, "it is still running")
        srv.p.terminate()
        srv.p.wait(30)

        print("[http]")
        port = free_port()
        h = subprocess.Popen([kicadd_mcp, "--socket", sock, "--http", f"127.0.0.1:{port}", "--no-web"], env=env,
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            for _ in range(50):
                try:
                    socket.create_connection(("127.0.0.1", port), 0.2).close()
                    break
                except OSError:
                    time.sleep(0.1)
            err, t = http_tool(port, "launch_kicad", {"path": pic})
            check(err and "--allow-launch" in t, "HTTP without --allow-launch: launch refused")
            err, t = http_tool(port, "close_kicad", {})
            check(err and "--allow-launch" in t, "... and close")
        finally:
            h.terminate()
            h.wait(10)

        if a.desktop:
            desktop(a, m, work, pic, sock, started)
    finally:
        try:
            m.tool("close_kicad", {"discard": True})
        except Exception:
            pass
        m.p.stdin.close()
        m.p.wait(10)
        for pid in started:
            if pid and not gone(pid):
                os.kill(pid, 9)

    print("FAILED: " + ", ".join(FAILS) if FAILS else "all passed")
    return 1 if FAILS else 0


def desktop(a, m_unused, work, pic, sock_unused, started):
    """The desktop editor through launch_kicad, in its own kicadd-mcp with Xvfb's DISPLAY and its
    own config dir (the first-run wizard is dismissed with xdotool)."""
    print("[desktop]")
    from desktop_e2e import Desktop  # noqa: E402
    for tool in ("Xvfb", "xdotool"):
        if not shutil.which(tool):
            check(False, tool + " not found")
            return
    cfg, tmp = os.path.join(work, "cfg"), os.path.join(work, "tmp")
    os.makedirs(os.path.join(cfg, "10.99"))
    os.makedirs(tmp)
    for name, head in (("fp-lib-table", "fp_lib_table"), ("sym-lib-table", "sym_lib_table"),
                       ("design-block-lib-table", "design_block_lib_table")):
        with open(os.path.join(cfg, "10.99", name), "w") as f:
            f.write(f"({head}\n  (version 7)\n)\n")
    _, kicadd_mcp, env = binaries(os.path.abspath(a.build))
    env = dict(env, KICAD_CONFIG_HOME=cfg, TMPDIR=tmp, DISPLAY=a.display, LIBGL_ALWAYS_SOFTWARE="1")
    xvfb = subprocess.Popen(["Xvfb", a.display, "-screen", "0", "1600x1000x24"],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(1)
    d = Desktop(a.display, env)
    stop = threading.Event()

    def wizard():
        while not stop.is_set():
            if (w := d.window("KiCad Setup")):
                d.keys(w, "Escape")
                time.sleep(1)
                if (c := d.window("Confirmation")):
                    d.keys(c, "Return")
            time.sleep(0.5)

    th = threading.Thread(target=wizard, daemon=True)
    th.start()
    dsock = os.path.join(work, "d.sock")
    m = Mcp([kicadd_mcp, "--socket", dsock, "--stdio"], env)
    try:
        err, t = m.tool("launch_kicad", {"mode": "desktop", "path": os.path.join(pic, "pic_programmer.kicad_pro")})
        print("      ", "\n       ".join(t.splitlines()))
        pid = pid_of(t)
        started.append(pid)
        check(not err and "started the desktop" in t and "board pic_programmer.kicad_pcb" in t,
              "the PCB editor up on kicadd-mcp's socket (KICADD_API_SOCKET)")
        err, t = m.tool("move_footprints", {"refs": ["R1"], "dx": 1})
        check(not err, "an edit through the editor")
        err, t = m.tool("close_kicad")
        check(err and "unsaved changes" in t, "close refused with the unsaved edit")
        err, t = m.tool("close_kicad", {"discard": True})
        check(not err and "closed the desktop" in t, "discard=true: closed")
        check(gone(pid), "the editor is gone")
    finally:
        stop.set()
        m.p.stdin.close()
        m.p.wait(10)
        xvfb.terminate()


if __name__ == "__main__":
    sys.exit(main())

"""Run every end-to-end test of KiCadD and print one line each.

usage: python3 -I client/run_all.py [--build ../build-lean] [--only a,b] [--skip a,b]
                                     [--no-desktop] [--live] [--list] [--keep-going]

Run from anywhere; paths default to this checkout's layout:
  --build       the build tree (../build-lean), or an installed / portable folder (Windows CI:
                dist/bin; the headless and stack tests run there, the desktop ones need Xvfb)
  --kicad       KiCad's source tree (../kicad); demos are its demos/
  --work        scratch root (run/all; each test gets run/all/<name>, socket paths stay short)
  --index       an unpacked parts index (run/sr/cache/kicad/10.99/kicadd/parts.db)
  --index-gz    the packed one (run/sr/www/parts.db.gz), for search_e2e and sim_e2e (with its params)
  --ngspice     the ngspice program, for sim_e2e ($KICADD_NGSPICE, the PATH, or run/sim/ngspice/usr/bin)
  --lcsc-cache  cached EasyEDA answers ($KICADD_LCSC_CACHE or run/lcsc-cache), for m5 / lcsc
  --venv        a Python with the MCP SDK, websockets and the generated protos (.venv/bin/python)
  --live        also the checks that need the network (search_e2e's live check)
  --no-desktop  skip the tests that run the GUI under Xvfb

A test whose inputs are missing is SKIPPED with the reason, never silently passed. Logs:
<work>/<name>.log. Exit status 1 when any test failed.

Groups:
  headless   the api-server with a board, kicadd-mcp over stdio (each test starts its own)
  stack      the Jetson baseboard served over HTTP (daemon + kicadd-mcp --http), a fresh copy
             for each: mcp_e2e, web_e2e, layout_e2e, history_e2e (restarts the stack itself)
  desktop    pcbnew / eeschema under Xvfb
"""

import argparse
import json
import os
import shutil
import signal
import socket
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)                       # KiCadD/
TOP = os.path.dirname(ROOT)                        # the folder holding KiCadD/, kicad/, build-lean/
JETSON = "jetson-agx-thor-baseboard"
EXE = ".exe" if os.name == "nt" else ""
# the stack outlives a test (history_e2e restarts it from another process): its own group / session
DETACH = {"creationflags": subprocess.CREATE_NEW_PROCESS_GROUP} if os.name == "nt" else {"start_new_session": True}


# ---------------------------------------------------------------------------- the HTTP stack

def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def stack_start(state):
    """Daemon on state['board'] + kicadd-mcp --http on state['port']; pids into state."""
    sys.path.insert(0, HERE)
    from m5_e2e import Server, binaries
    kicad_cli, kicadd_mcp, env = binaries(state["build"])
    sock = state["sock"]
    if os.path.exists(sock):
        os.unlink(sock)
    log = open(state["log"], "a")
    d = subprocess.Popen([kicad_cli, "api-server", "--socket", sock, state["board"]], env=env, stdout=log,
                         stderr=subprocess.STDOUT, **DETACH)
    if os.name == "nt":      # a named pipe there, no file: ask until it answers
        probe = Server(state["build"], sock, state["board"], env, state["log"])
        probe.p = d
        up = probe.answers()
    else:
        for _ in range(240):
            if os.path.exists(sock) or d.poll() is not None:
                break
            time.sleep(0.5)
        time.sleep(0.5)
        up = d.poll() is None and os.path.exists(sock)
    if not up:
        raise RuntimeError("the daemon did not start (see " + state["log"] + ")")
    m = subprocess.Popen([kicadd_mcp, "--socket", sock, "--http", "127.0.0.1:%d" % state["port"]], env=env,
                         stdout=log, stderr=subprocess.STDOUT, **DETACH)
    for _ in range(100):
        try:
            with socket.create_connection(("127.0.0.1", state["port"]), timeout=0.5):
                break
        except OSError:
            if m.poll() is not None:
                raise RuntimeError("kicadd-mcp did not start (see " + state["log"] + ")")
            time.sleep(0.2)
    state["pids"] = [d.pid, m.pid]
    json.dump(state, open(state["file"], "w"))


def alive(pid):
    if os.name == "nt":      # os.kill would end it there
        out = subprocess.run(["tasklist", "/FI", "PID eq %d" % pid, "/NH"], capture_output=True, text=True).stdout
        return str(pid) in out.split()
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    try:
        return not os.waitpid(pid, os.WNOHANG)[0]
    except ChildProcessError:
        return True


def stack_stop(state):
    for pid in reversed(state.get("pids", [])):
        if os.name == "nt":
            subprocess.run(["taskkill", "/PID", str(pid), "/T", "/F"], capture_output=True)
        else:
            try:
                os.killpg(pid, signal.SIGTERM)
            except ProcessLookupError:
                continue
        for _ in range(60):
            if not alive(pid):
                break
            time.sleep(0.5)
    state["pids"] = []
    json.dump(state, open(state["file"], "w"))


def stack_restart(state_file):
    """history_e2e's restart command: the same board (now saved) on the same port."""
    state = json.load(open(state_file))
    stack_stop(state)
    stack_start(state)


def new_stack(build, kicad, work):
    shutil.rmtree(work, ignore_errors=True)
    shutil.copytree(os.path.join(kicad, "demos", JETSON), os.path.join(work, "p"))
    board = [f for f in os.listdir(os.path.join(work, "p")) if f.endswith(".kicad_pcb")][0]
    state = {"build": build, "board": os.path.join(work, "p", board), "sock": os.path.join(work, "s.sock"),
             "port": free_port(), "log": os.path.join(work, "stack.log"), "file": os.path.join(work, "stack.json")}
    stack_start(state)
    return state


# ---------------------------------------------------------------------------- the tests

def tests(a):
    """(name, group, needs, argv or callable(work) -> argv, stack?)"""
    py, venv = [sys.executable, "-I"], [a.venv, "-I"]
    b, k, d = ["--build", a.build], ["--kicad", a.kicad], ["--demos", a.demos]
    c = lambda n: os.path.join(HERE, n)   # noqa: E731
    out = [
        ("smoke", "headless", [], lambda w: py + [c("smoke.py"), "--bin", a.build, "--demos", a.demos, "--work", w, "--build-tree"]),
        ("fab", "headless", [], lambda w: py + [c("fab_e2e.py")] + b + d + ["--work", w]),
        ("smd", "headless", ["index"], lambda w: py + [c("smd_e2e.py")] + b + d + ["--work", w, "--index", a.index]),
        ("tools", "headless", [], lambda w: py + [c("tools_e2e.py")] + b + d + ["--work", w]),
        ("route", "headless", [], lambda w: py + [c("route_e2e.py")] + b + k + ["--work", w]),
        ("routed", "headless", [], lambda w: py + [c("routed_e2e.py")] + b + d + ["--work", w]),
        ("copper", "headless", [], lambda w: py + [c("copper_e2e.py")] + b + d + ["--work", w]),
        ("setup", "headless", [], lambda w: py + [c("setup_e2e.py")] + b + k + ["--work", w]),
        ("rules", "headless", [], lambda w: py + [c("rules_e2e.py")] + b + k + ["--work", w]),
        ("impedance", "headless", [], lambda w: py + [c("impedance_e2e.py")] + b + k + ["--work", w]),
        ("overhang", "headless", [], lambda w: py + [c("overhang_e2e.py")] + b + k + ["--work", w]),
        ("plane", "headless", [], lambda w: py + [c("plane_e2e.py")] + b + d + ["--work", w]),
        ("length", "headless", [], lambda w: py + [c("length_e2e.py")] + b + d + ["--work", w]),
        ("irdrop", "headless", [], lambda w: py + [c("irdrop_e2e.py")] + b + d + ["--work", w]),
        ("xtalk", "headless", [], lambda w: py + [c("xtalk_e2e.py")] + b + d + ["--work", w]),
        ("zprofile", "headless", [], lambda w: py + [c("zprofile_e2e.py")] + b + d + ["--work", w]),
        ("hier", "headless", [], lambda w: py + [c("hier_e2e.py")] + b + k + ["--work", w]),
        ("sch_edit", "headless", [], lambda w: py + [c("sch_edit_e2e.py")] + b + k + ["--work", w]),
        ("sch_doc", "headless", [], lambda w: py + [c("sch_doc_e2e.py")] + b + d + ["--work", w]),
        ("lib", "headless", [], lambda w: py + [c("lib_e2e.py")] + b + k + ["--work", w]),
        ("match", "headless", ["index"], lambda w: py + [c("match_e2e.py")] + b + k + ["--work", w, "--index", a.index]),
        ("search", "headless", ["index_gz"],
         lambda w: py + [c("search_e2e.py")] + b + d + ["--work", w, "--index", a.index_gz] + ([] if a.live else ["--no-live"])),
        ("sim", "headless", ["index_gz", "ngspice"],
         lambda w: py + [c("sim_e2e.py")] + b + k + ["--work", w, "--index", a.index_gz, "--ngspice", a.ngspice]),
        ("m5", "headless", ["lcsc_cache"], lambda w: py + [c("m5_e2e.py")] + b + k + ["--work", w, "--cache", a.lcsc_cache]),
        ("lcsc", "headless", ["lcsc_cache"], "lcsc"),
        ("model", "headless", ["lcsc_cache"], lambda w: py + [c("model_e2e.py")] + b + k + ["--work", w, "--cache", a.lcsc_cache]),
        ("autoplace_bg", "headless", ["venv"], lambda w: venv + [c("autoplace_bg_e2e.py")] + b + d + ["--work", w]),
        ("mcp", "stack", ["venv"], "mcp"),
        ("web", "stack", ["venv"], "web"),
        ("layout", "stack", ["venv"], "layout"),
        ("history", "stack", ["venv"], "history"),
        ("desktop", "desktop", ["xvfb"], lambda w: py + [c("desktop_e2e.py")] + b + d + ["--work", w]),
        ("desktop_sch", "desktop", ["xvfb"], lambda w: py + [c("desktop_sch_e2e.py")] + b + d + ["--work", w]),
        ("desktop_autoplace", "desktop", ["xvfb"], lambda w: py + [c("desktop_autoplace_e2e.py")] + b + d + ["--work", w]),
    ]
    return out


def missing(a, needs):
    have = {
        "index": (os.path.isfile(a.index), "no parts index at " + a.index),
        "index_gz": (os.path.isfile(a.index_gz), "no packed parts index at " + a.index_gz),
        "lcsc_cache": (os.path.isdir(a.lcsc_cache) and bool(os.listdir(a.lcsc_cache)), "no LCSC cache at " + a.lcsc_cache),
        "venv": (os.path.isfile(a.venv), "no venv Python at " + a.venv + " (README: Python client)"),
        "ngspice": (os.path.isfile(a.ngspice), "no ngspice at " + a.ngspice + " (--ngspice, $KICADD_NGSPICE)"),
        "xvfb": (shutil.which("Xvfb") is not None and shutil.which("xdotool") is not None, "Xvfb / xdotool not installed"),
    }
    for n in needs:
        if not have[n][0]:
            return have[n][1]
    return None


def run(argv, log, timeout, env=None):
    t0 = time.time()
    with open(log, "w") as f:
        try:
            p = subprocess.run(argv, stdout=f, stderr=subprocess.STDOUT, timeout=timeout, env=env, cwd=ROOT)
            code = p.returncode
        except subprocess.TimeoutExpired:
            f.write("\n[run_all] timed out after %d s\n" % timeout)
            code = "timeout"
    return code, time.time() - t0


def verdict(code, log):
    lines = open(log, errors="replace").read().splitlines()
    result = next((l for l in reversed(lines) if l.startswith("RESULT:")), "")
    fails = [l.strip() for l in lines if l.lstrip().startswith("FAIL ")]
    if code == 0 and "FAIL" not in result:
        return "PASS", result or "exit 0"
    why = result or ("timed out" if code == "timeout" else "exit %s" % code)
    return "FAIL", why + ("; first: " + fails[0][5:].strip()[:100] if fails else "")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", default=os.path.join(TOP, "build-lean"))
    ap.add_argument("--kicad", default=os.path.join(TOP, "kicad"))
    ap.add_argument("--work", default=os.path.join(ROOT, "run", "all"))
    ap.add_argument("--index", default=os.path.join(ROOT, "run", "sr", "cache", "kicad", "10.99", "kicadd", "parts.db"))
    ap.add_argument("--index-gz", default=os.path.join(ROOT, "run", "sr", "www", "parts.db.gz"))
    ap.add_argument("--ngspice", default=os.environ.get("KICADD_NGSPICE") or shutil.which("ngspice")
                    or os.path.join(ROOT, "run", "sim", "ngspice", "usr", "bin", "ngspice"))
    ap.add_argument("--lcsc-cache", default=os.environ.get("KICADD_LCSC_CACHE") or os.path.join(ROOT, "run", "lcsc-cache"))
    ap.add_argument("--venv", default=os.path.join(ROOT, ".venv", "Scripts", "python.exe") if os.name == "nt"
                    else os.path.join(ROOT, ".venv", "bin", "python"))
    ap.add_argument("--only", default="")
    ap.add_argument("--skip", default="")
    ap.add_argument("--no-desktop", action="store_true")
    ap.add_argument("--live", action="store_true")
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--timeout", type=int, default=900, help="seconds per test")
    ap.add_argument("--restart-stack", help=argparse.SUPPRESS)   # history_e2e's restart command
    a = ap.parse_args()

    if a.restart_stack:
        stack_restart(a.restart_stack)
        return 0

    for k in ("build", "kicad", "work", "index", "index_gz", "lcsc_cache", "venv", "ngspice"):
        setattr(a, k, os.path.abspath(getattr(a, k)))
    a.demos = os.path.join(a.kicad, "demos")

    all_tests = tests(a)
    if a.list:
        for name, group, needs, _ in all_tests:
            print("%-18s %-9s %s" % (name, group, missing(a, needs) or "ready"))
        return 0

    only = set(filter(None, a.only.split(",")))
    skip = set(filter(None, a.skip.split(",")))
    unknown = (only | skip) - {t[0] for t in all_tests}
    if unknown:
        print("unknown test(s): " + ", ".join(sorted(unknown)) + " (--list shows them)")
        return 2
    chosen = [t for t in all_tests if (not only or t[0] in only) and t[0] not in skip
              and not (a.no_desktop and t[1] == "desktop")]

    if not os.path.isfile(mcp_bin(a.build)):
        print("no kicadd-mcp" + EXE + " in " + a.build + ": build first")
        return 2
    os.makedirs(a.work, exist_ok=True)
    results = []

    def report(name, status, why, secs):
        results.append((name, status, why, secs))
        print("%-18s %-5s %6.0f s  %s" % (name, status, secs, why), flush=True)

    stack = None
    try:
        for name, group, needs, how in chosen:
            work = os.path.join(a.work, name)
            log = os.path.join(a.work, name + ".log")
            why = missing(a, needs)
            if why:
                report(name, "SKIP", why, 0)
                continue

            env = None
            if group == "stack":
                if stack is None:
                    try:
                        stack = new_stack(a.build, a.kicad, os.path.join(a.work, "stack"))
                    except RuntimeError as e:
                        report(name, "FAIL", str(e), 0)
                        continue
                base = "http://127.0.0.1:%d" % stack["port"]
                argv = {
                    "mcp": venv_argv(a, "mcp_e2e.py", [mcp_bin(a.build), stack["sock"], str(free_port())]),
                    "web": venv_argv(a, "web_e2e.py", [base]),
                    "layout": venv_argv(a, "layout_e2e.py", [base, stack["sock"]]),
                    "history": venv_argv(a, "history_e2e.py", [base, sys.executable, "-I", os.path.abspath(__file__),
                                                              "--restart-stack", stack["file"]]),
                }[how]
            elif how == "lcsc":
                argv, env, srv = lcsc_setup(a, work)
                if srv is None:
                    report(name, "FAIL", "the daemon did not start", 0)
                    continue
            else:
                argv = how(work)

            try:
                code, secs = run(argv, log, a.timeout, env)
            finally:
                if how == "lcsc":
                    srv.stop()
            status, why = verdict(code, log)
            report(name, status, why, secs)

            # each stack test wants the board as loaded (layout checks a fresh history; layout and
            # history save it): the next one gets a fresh copy and server
            if group == "stack" and stack is not None:
                stack_stop(stack)
                stack = None
    finally:
        if stack is not None:
            stack_stop(stack)

    n = {s: sum(1 for r in results if r[1] == s) for s in ("PASS", "FAIL", "SKIP")}
    print("\n%d passed, %d failed, %d skipped (logs in %s)" % (n["PASS"], n["FAIL"], n["SKIP"], a.work))
    return 1 if n["FAIL"] else 0


def mcp_bin(build):
    """kicadd-mcp in a build tree or an installed / portable folder (BUILD or BUILD/bin)."""
    for sub in ("", "bin"):
        if os.path.isfile(os.path.join(build, sub, "kicadd-mcp" + EXE)):
            return os.path.join(build, sub, "kicadd-mcp" + EXE)
    return os.path.join(build, "kicadd-mcp" + EXE)


def venv_argv(a, script, args):
    return [a.venv, "-I", os.path.join(HERE, script)] + args


def lcsc_setup(a, work):
    """lcsc_e2e wants a running daemon on a project folder, with the cache in its environment."""
    sys.path.insert(0, HERE)
    from m5_e2e import Server
    shutil.rmtree(work, ignore_errors=True)
    shutil.copytree(os.path.join(a.demos, "pic_programmer"), os.path.join(work, "p"))
    env = dict(os.environ, KICAD_RUN_FROM_BUILD_DIR="1", KICADD_LCSC_CACHE=a.lcsc_cache, KICADD_LCSC_OFFLINE="1")
    sock = os.path.join(work, "s.sock")
    srv = Server(a.build, sock, os.path.join(work, "p", "pic_programmer.kicad_pcb"), env, os.path.join(work, "server.log"))
    if not srv.start():
        return None, None, None
    argv = [sys.executable, "-I", os.path.join(HERE, "lcsc_e2e.py"), "--mcp", mcp_bin(a.build),
            "--socket", sock, "--cache", a.lcsc_cache, "--project", os.path.join(work, "p")]
    return argv, env, srv


if __name__ == "__main__":
    sys.exit(main())

"""The merged tools (beta.13 phase 2) and their old names, headless on pic_programmer.

usage: python3 -I merge_e2e.py --build BUILD --kicad KICAD_SRC --work DIR

  list        tools/list has undo, project, export and not the old names (redo, sch_undo, sch_redo,
              get_rules, check_drc, flush_ratsnest, open_board, new_project, export_fab,
              export_assembly, sch_export_pdf, board_render_3d); tool_help on an old name says the new
  undo        a board move undone and redone with undo {redo}; a schematic edit with
              undo {target: "schematic"} (and redo); a bad target refused
  rules       set_rules {} answers the rules (as get_rules did, no DRC run)
  drc         run_drc {refs, local: true} is the local DRC; local without refs, or with types, refused
  summary     board_summary ends with the full unrouted count (as flush_ratsnest)
  export      export {fab, assembly, schematic_pdf} writes all three in one call; none given refused
  render      board_render {view: "3d"} renders (size sets the width, 4:3); 2d-only arguments
              refused in 3d and 3d-only ones in 2d; a numeric zoom refused in 2d
  project     project {new} makes and opens a project, project {open} goes back; both or neither refused
  aliases     each old name still works (batch too) and ends "note: X is now Y"
"""

import argparse
import os
import re
import shutil
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from m5_e2e import FAILS, Mcp, Server, binaries, check, png_pixels  # noqa: E402

OLD = ["redo", "sch_undo", "sch_redo", "get_rules", "check_drc", "flush_ratsnest", "open_board", "new_project",
       "export_fab", "export_assembly", "sch_export_pdf", "board_render_3d"]


def first(t):
    return t.strip().splitlines()[0] if t.strip() else ""


def main():
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", required=True)
    ap.add_argument("--kicad", required=True)
    ap.add_argument("--work", required=True)
    a = ap.parse_args()
    work = os.path.abspath(a.work)
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(work)
    shutil.copytree(os.path.join(a.kicad, "demos", "pic_programmer"), os.path.join(work, "pic"))
    pcb = os.path.join(work, "pic", "pic_programmer.kicad_pcb")
    _, kicadd_mcp, env = binaries(os.path.abspath(a.build))
    sock = os.path.join(work, "s.sock")
    srv = Server(a.build, sock, pcb, env, os.path.join(work, "server.log"))
    check(srv.start(), "api-server up on pic_programmer")
    m = Mcp([kicadd_mcp, "--socket", sock, "--stdio"], env)
    try:
        print("[list]")
        names = [t["name"] for t in m.rpc("tools/list", {})["result"]["tools"]]
        check(all(n in names for n in ("undo", "project", "export", "set_rules", "run_drc", "board_summary", "board_render")),
              "the merged tools are listed")
        listed = [n for n in OLD if n in names]
        check(not listed, f"no old name listed ({len(names)} tools)" + (": " + ", ".join(listed) if listed else ""))
        err, t = m.tool("tool_help", {"name": "check_drc"})
        check(not err and "use run_drc" in t, "tool_help on an old name names the new tool")
        err, t = m.tool("tool_help", {"name": "nope"})
        check(err and "check_drc" not in t and "run_drc" in t, "tool_help's list of tools leaves out the old names")

        print("[undo]")
        err, t = m.tool("move_footprints", {"refs": ["R1"], "dx": 1.0})
        check(not err, "R1 moved 1 mm")
        err, t = m.tool("undo", {})
        check(not err and first(t).startswith("undid"), "undo {}: the board move undone: " + first(t)[:80])
        err, t = m.tool("undo", {"redo": True})
        check(not err and first(t).startswith("redid"), "undo {redo: true}: redone: " + first(t)[:80])
        err, t = m.tool("sch_set_fields", {"parts": {"R1": {"Value": "4k7"}}})
        check(not err, "a schematic edit (R1 value 4k7)")
        err, t = m.tool("undo", {"target": "schematic"})
        check(not err and t.startswith("undid:"), "undo {target: schematic}: " + first(t)[:80])
        err, t = m.tool("undo", {"target": "schematic", "redo": True})
        check(not err and t.startswith("redid:"), "undo {target: schematic, redo: true}: " + first(t)[:80])
        err, t = m.tool("undo", {"target": "pcb"})
        check(err and "schematic" in t, "a bad target refused")

        print("[rules]")
        err, t = m.tool("set_rules", {})
        check(not err and "Default" in t and "DRC" not in first(t), "set_rules {}: the rules, no edit: " + first(t)[:80])

        print("[drc]")
        err, t = m.tool("run_drc", {"refs": ["R1"], "local": True})
        _, old = m.tool("check_drc", {"refs": ["R1"]})
        check(not err and first(t) == first(old), "run_drc {refs, local}: the local DRC (as check_drc): " + first(t)[:80])
        err, t = m.tool("run_drc", {"local": True})
        check(err and "refs" in t, "local without refs refused")
        err, t = m.tool("run_drc", {"refs": ["R1"], "local": True, "types": ["clearance"]})
        check(err and "types" in t, "local with types refused")
        err, t = m.tool("run_drc", {"limit": 1})
        check(not err and "note:" not in t, "run_drc without local: the full DRC as before")

        print("[summary]")
        err, t = m.tool("board_summary", {})
        last = t.strip().splitlines()[-1]
        check(not err and t.startswith("file=pic_programmer.kicad_pcb") and last.startswith("ratsnest: ")
              and re.search(r"\d", last), "board_summary ends with the unrouted count: " + last[:90])

        print("[export]")
        fab = os.path.join(work, "fab")
        sch_pdf = os.path.join(work, "pic", "sch.pdf")
        err, t = m.tool("export", {"fab": {"dir": fab, "drc": False}, "assembly": {"dir": fab},
                                   "schematic_pdf": {"path": sch_pdf}})
        print("      ", "\n       ".join(t.strip().splitlines()[:6]))
        files = os.listdir(fab) if os.path.isdir(fab) else []
        check(not err and any(f.endswith(".zip") for f in files) and any(f.endswith("-assembly.pdf") for f in files)
              and os.path.isfile(sch_pdf), "fab zip, assembly PDF and schematic PDF in one call")
        err, t = m.tool("export", {})
        check(err and "fab" in t, "export with nothing to write refused")
        err, t = m.tool("export", {"fab": {"dir": fab, "drc": False, "strict": True, "refill": False}, "schematic_pdf": {"path": "/nonexistent/dir/x.pdf"}})
        check("FAILED" in t and err, "one part failing is marked and the call is an error")

        print("[render]")
        err, t, imgs = m.tool_images("board_render", {"view": "3d", "size": 400, "side": "bottom"})
        dims = png_pixels(imgs[0])[:2] if imgs else None
        _, old, oimgs = m.tool_images("board_render_3d", {"width": 400, "height": 300, "side": "bottom"})
        odims = png_pixels(oimgs[0])[:2] if oimgs else None
        check(not err and imgs and t.startswith("3D bottom") and dims == odims,
              f"board_render view=3d, size 400: as board_render_3d 400 x 300: {first(t)[:40]} {dims}")
        err, t, imgs = m.tool_images("board_render", {"view": "3d", "size": 320, "back": True})
        check(not err and t.startswith("3D bottom"), "back=true in 3d is the bottom side")
        err, t = m.tool("board_render", {"view": "3d", "refs": ["R1"]})
        check(err and "refs" in t, "a 2d argument refused in 3d")
        err, t = m.tool("board_render", {"rot_x": -45})
        check(err and "rot_x" in t and "3d" in t, "a 3d argument refused in 2d")
        err, t = m.tool("board_render", {"zoom": 2})
        check(err and "zoom" in t, "a numeric zoom refused in 2d")
        err, t, imgs = m.tool_images("board_render", {"size": 300})
        check(not err and imgs, "board_render 2d as before")

        print("[project]")
        one = os.path.join(work, "projects", "alpha")
        err, t = m.tool("project", {"new": {"path": one, "outline": [40, 30]}, "discard": True})
        check(not err and "file=alpha.kicad_pcb" in t and os.path.isfile(os.path.join(one, "alpha.kicad_pro")),
              "project {new}: made and opened: " + first(t)[:80])
        err, t = m.tool("project", {"open": pcb, "discard": True})
        check(not err and "pic_programmer" in t, "project {open}: back to pic_programmer")
        err, t = m.tool("project", {})
        check(err and "open" in t and "new" in t, "neither open nor new refused")
        err, t = m.tool("project", {"open": pcb, "new": {"path": one}})
        check(err, "both open and new refused")

        print("[aliases]")
        for name, args in (("get_rules", {}), ("check_drc", {"refs": ["R1"]}), ("flush_ratsnest", {}),
                           ("open_board", {"path": pcb, "discard": True}), ("export_assembly", {"dir": fab}),
                           ("sch_export_pdf", {"path": sch_pdf})):
            err, t = m.tool(name, args)
            check(not err and re.search(r"note: " + name + r" is now \S", t), f"{name} still works and names the new form")
        err, t = m.tool("move_footprints", {"refs": ["R1"], "dx": 1.0})
        err, t = m.tool("batch", {"steps": [{"tool": "check_drc", "args": {"refs": ["R1"]}}, {"tool": "redo"}]})
        check(not err or "nothing to redo" in t, "batch takes old names: " + t.strip().splitlines()[0][:80])
        err, t = m.tool("sch_undo", {})
        check("note: sch_undo is now undo {target: \"schematic\"}" in t, "sch_undo still works and names the new form")
    finally:
        m.close()
        srv.stop()

    print("\nRESULT: " + ("PASS" if not FAILS else "FAIL (%d)" % len(FAILS)))
    sys.exit(1 if FAILS else 0)


if __name__ == "__main__":
    main()

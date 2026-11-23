"""Count mcp__kicadd__* tool calls (and the steps inside batch) per tool in Claude Code transcripts.

usage: python3 -I ci/tool_usage.py ~/.claude/projects/<project-dir>   (prints JSON: calls, errors, transcripts, inner)
Only tool names are read; no content is printed."""
import json, os, sys, collections
roots = sys.argv[1:]
calls, errs, files, inner = collections.Counter(), collections.Counter(), collections.Counter(), collections.Counter()
for root in roots:
    for dp, _, fs in os.walk(root):
        for f in fs:
            if not f.endswith(".jsonl"):
                continue
            path = os.path.join(dp, f); ids = {}; seen = False
            for line in open(path, encoding="utf-8", errors="replace"):
                if "mcp__kicadd__" not in line and "tool_result" not in line:
                    continue
                try:
                    d = json.loads(line)
                except ValueError:
                    continue
                c = (d.get("message") or {}).get("content")
                if not isinstance(c, list):
                    continue
                for b in c:
                    if not isinstance(b, dict):
                        continue
                    if b.get("type") == "tool_use" and str(b.get("name", "")).startswith("mcp__kicadd__"):
                        n = b["name"][len("mcp__kicadd__"):]
                        calls[n] += 1; ids[b.get("id")] = n; seen = True
                        if n == "batch":
                            inp = b.get("input") or {}
                            steps = inp.get("steps") if isinstance(inp, dict) else None
                            if isinstance(steps, str):
                                try: steps = json.loads(steps)
                                except ValueError: steps = None
                            rep = inp.get("repeat", 1) if isinstance(inp, dict) else 1
                            rep = rep if isinstance(rep, int) and rep > 0 else 1
                            for st in steps or []:
                                if isinstance(st, dict) and isinstance(st.get("tool"), str):
                                    inner[st["tool"].replace("mcp__kicadd__", "")] += rep
                    elif b.get("type") == "tool_result" and b.get("tool_use_id") in ids and b.get("is_error"):
                        errs[ids[b["tool_use_id"]]] += 1
            if seen:
                for n in set(ids.values()):
                    files[n] += 1
print(json.dumps({"calls": calls, "errors": errs, "transcripts": files, "inner": inner}))

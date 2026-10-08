#!/usr/bin/env python3
"""Groups piglit tests whose result differs between Apple and GLMetal by
area, with GLMetal's first output line, to find the biggest causes.

usage: triage.py APPLE_DIR GLMETAL_DIR [area-regex] [--details N]"""
import bz2, json, os, re, sys
from collections import Counter, defaultdict


def load(path):
    f = os.path.join(path, "results.json.bz2")
    return json.load(bz2.open(f, "rt"))["tests"] if os.path.exists(f) else json.load(open(os.path.join(path, "results.json")))["tests"]


apple, glm = load(sys.argv[1]), load(sys.argv[2])
area_filter = sys.argv[3] if len(sys.argv) > 3 and not sys.argv[3].startswith("--") else None
details = int(sys.argv[sys.argv.index("--details") + 1]) if "--details" in sys.argv else 0
same = lambda a, b: {"warn": "pass"}.get(a, a) == {"warn": "pass"}.get(b, b)
areas = Counter()
reasons = defaultdict(Counter)
examples = defaultdict(list)
for name, a in apple.items():
    g = glm.get(name)
    if not g or same(a["result"], g["result"]):
        continue
    parts = name.split("@")
    area = "@".join(parts[:3]) if len(parts) > 3 else "@".join(parts[:2])
    if area_filter and not re.search(area_filter, name):
        continue
    areas[area] += 1
    text = (g.get("out") or "") + "\n" + (g.get("err") or "")
    lines = [l.strip() for l in text.splitlines() if l.strip() and not l.startswith("PIGLIT:")]
    reason = re.sub(r"\d+(\.\d+)?", "N", lines[0])[:110] if lines else f"({g['result']})"
    reasons[area][reason] += 1
    examples[area].append((name, a["result"], g["result"], lines[:details]))
for area, n in areas.most_common():
    print(f"{n:4} {area}")
    for reason, k in reasons[area].most_common(3):
        print(f"       {k:4} {reason}")
    for name, a, g, lines in examples[area][: (3 if details else 0)]:
        print(f"         - {name}: apple {a}, glmetal {g}")
        for l in lines:
            print(f"             {l[:150]}")

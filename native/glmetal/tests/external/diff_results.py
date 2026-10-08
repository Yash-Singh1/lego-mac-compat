#!/usr/bin/env python3
"""Differential comparison of two conformance runs: Apple's OpenGL (the
reference) against GLMetal. GLMetal aims to behave like Apple's
implementation, so any difference in a test's result counts, in either
direction, unless the known-differences file lists it.

usage: diff_results.py APPLE GLMETAL KNOWN [--update]
  APPLE / GLMETAL: a piglit results directory (results.json[.bz2]) or a
  glcts results TSV (case<TAB>status, from cts_runner.py).
  KNOWN: known differences, one "test<TAB>apple<TAB>glmetal" per line
  (# comments). --update rewrites it with the current differences.
Exits 1 when a difference is not known."""
import bz2, json, os, sys


def load(path):
    if os.path.isdir(path):
        for name in ("results.json.bz2", "results.json"):
            f = os.path.join(path, name)
            if os.path.exists(f):
                data = json.load(bz2.open(f, "rt") if f.endswith(".bz2") else open(f))
                return {t: r["result"] for t, r in data["tests"].items()}
        raise SystemExit(f"no piglit results in {path}")
    results = {}
    for line in open(path):
        parts = line.rstrip("\n").split("\t")
        if len(parts) >= 2:
            results[parts[0]] = parts[1]
    return results


def normal(status):
    """Statuses that mean the same thing for parity."""
    s = status.lower()
    return {"warn": "pass", "notsupported": "skip", "compatibilitywarning": "pass", "qualitywarning": "pass"}.get(s, s)


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    update = "--update" in sys.argv
    if len(args) != 3:
        raise SystemExit(__doc__)
    apple, glmetal = load(args[0]), load(args[1])
    known = {}
    if os.path.exists(args[2]):
        for line in open(args[2]):
            if line.strip() and not line.startswith("#"):
                parts = line.rstrip("\n").split("\t")
                known[parts[0]] = tuple(parts[1:3])
    diffs = {t: (apple[t], glmetal.get(t, "missing")) for t in sorted(apple)
             if normal(apple[t]) != normal(glmetal.get(t, "missing"))}
    unknown = {t: d for t, d in diffs.items() if t not in known}
    fixed = [t for t in known if t in apple and t not in diffs]
    same = sum(1 for t in apple if t not in diffs)
    print(f"{len(apple)} tests: {same} match Apple, {len(diffs)} differ "
          f"({len(diffs) - len(unknown)} known, {len(unknown)} new); {len(fixed)} known differences now match")
    by_kind = {}
    for t, (a, g) in diffs.items():
        by_kind[(normal(a), normal(g))] = by_kind.get((normal(a), normal(g)), 0) + 1
    for (a, g), n in sorted(by_kind.items(), key=lambda kv: -kv[1]):
        print(f"  apple {a:>8} -> glmetal {g:<8} {n}")
    for t, (a, g) in list(unknown.items())[:40]:
        print(f"NEW  {t}: apple {a}, glmetal {g}")
    if len(unknown) > 40:
        print(f"... and {len(unknown) - 40} more new differences")
    if update:
        with open(args[2], "w") as f:
            f.write("# Known differences from Apple's OpenGL: test<TAB>apple<TAB>glmetal (diff_results.py --update)\n")
            for t, (a, g) in diffs.items():
                f.write(f"{t}\t{a}\t{g}\n")
        print(f"wrote {len(diffs)} known differences to {args[2]}")
        return 0
    return 1 if unknown else 0


if __name__ == "__main__":
    sys.exit(main())

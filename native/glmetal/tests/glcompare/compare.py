#!/usr/bin/env python3
"""Runs the glcompare cases on a reference OpenGL implementation (Apple's by
default) and on a test implementation (GLMetal by default), compares every
image pixel by pixel, and writes an HTML report.

  compare.py [--test PATH] [--reference apple|PATH] [--filter TEXT]
             [--arch x86_64|arm64] [--out DIR] [--reuse-reference]

A crash in the test implementation does not end the run: the remaining cases
are rerun one process each, and cases that crash are reported as such.
Standard library only, so the suite can be dropped into other projects.
"""
import argparse
import html
import os
import pathlib
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent
GLMETAL = HERE.parents[1]
BUILD = GLMETAL / 'build'


def runner_command(args, provider, out, extra=()):
    command = [str(BUILD / 'glcompare'), '--provider', provider, '--cases', str(BUILD / 'libglcases.dylib'),
               '--out', str(out), *extra]
    if args.arch:
        command = ['arch', f'-{args.arch}', *command]
    return command


def list_cases(args, provider):
    output = subprocess.run(runner_command(args, provider, '/tmp', ['--list']), capture_output=True, text=True, check=True)
    names = [line for line in output.stdout.splitlines() if line]
    return [name for name in names if not args.filter or args.filter in name]


def completed_cases(out):
    path = out / 'results.tsv'
    if not path.exists():
        return set()
    return {line.split('\t', 1)[0] for line in path.read_text().splitlines()[1:] if line}


def run_provider(args, provider, out, label):
    """Runs every case once; falls back to one process per case after a crash."""
    out.mkdir(parents=True, exist_ok=True)
    for stale in out.glob('*.png'):
        stale.unlink()
    (out / 'results.tsv').unlink(missing_ok=True)
    extra = ['--filter', args.filter] if args.filter else []
    try:
        batch = subprocess.run(runner_command(args, provider, out, extra), capture_output=True, text=True,
                               timeout=args.timeout)
        returncode, log = batch.returncode, batch.stdout + batch.stderr
    except subprocess.TimeoutExpired as expired:
        returncode, log = -9, f'timed out after {args.timeout} s\n{expired.stdout or ""}{expired.stderr or ""}'
    (out / 'runner.log').write_text(log if isinstance(log, str) else str(log))
    crashed = []
    if returncode < 0 or returncode > 1:
        done = completed_cases(out)
        remaining = [name for name in list_cases(args, provider) if name not in done]
        print(f'{label}: runner exited with {returncode}; running {len(remaining)} remaining cases one by one')
        for name in remaining:
            try:
                single = subprocess.run(runner_command(args, provider, out, ['--only', name]), capture_output=True,
                                        text=True, timeout=args.case_timeout)
                code = single.returncode
            except subprocess.TimeoutExpired:
                code = 'timeout'
            if code == 'timeout' or code < 0 or code > 1:
                crashed.append(name)
                with open(out / 'results.tsv', 'a') as tsv:
                    tsv.write(f'{name}\tcrashed\t0x0000\t0\t0\t0\t0\t0\texit {code}\n')
    return crashed


def read_tsv(path):
    lines = path.read_text().splitlines()
    header = lines[0].split('\t')
    return {row[0]: dict(zip(header, row)) for row in (line.split('\t') for line in lines[1:] if line)}


def write_report(args, reference_dir, test_dir, report_dir):
    subprocess.run([str(BUILD / 'glcompare'), '--compare', str(reference_dir), str(test_dir), '--out', str(report_dir)],
                   check=False)
    compared = read_tsv(report_dir / 'compare.tsv')
    reference = read_tsv(reference_dir / 'results.tsv')
    test = read_tsv(test_dir / 'results.tsv') if (test_dir / 'results.tsv').exists() else {}
    rows = []
    counts = {}
    for name in sorted(compared):
        c = compared[name]
        t = test.get(name, {})
        verdict = c['verdict']
        if t.get('status') not in (None, 'ok'):
            verdict = t['status']
        counts[verdict] = counts.get(verdict, 0) + 1
        rel = lambda d, suffix: os.path.relpath(d / f'{name}{suffix}', report_dir)
        diff = f'<img src="{rel(report_dir, ".diff.png")}">' if (report_dir / f'{name}.diff.png').exists() else ''
        message = html.escape(t.get('message', '') or reference.get(name, {}).get('message', ''))
        rows.append((verdict != 'match', name,
                     f'<tr class="{verdict}"><td>{html.escape(name)}</td><td>{verdict}</td><td>{c["max_diff"]}</td>'
                     f'<td>{c["bad_pixels"]}/{c["total_pixels"]}</td>'
                     f'<td><img src="{rel(reference_dir, ".png")}"></td><td><img src="{rel(test_dir, ".png")}"></td>'
                     f'<td>{diff}</td><td>{message}</td></tr>'))
    rows.sort(key=lambda r: (not r[0], r[1]))
    summary = ', '.join(f'{v}: {n}' for v, n in sorted(counts.items()))
    page = f"""<!doctype html><meta charset="utf-8"><title>glcompare</title>
<style>body{{font:13px -apple-system,sans-serif;background:#1c1c1e;color:#ddd}}
img{{width:128px;height:128px;image-rendering:pixelated;background:repeating-conic-gradient(#555 0 25%,#333 0 50%) 0 0/16px 16px}}
td{{padding:4px;vertical-align:middle}} tr.match td:nth-child(2){{color:#7c7}} tr:not(.match) td:nth-child(2){{color:#f77}}</style>
<h2>glcompare: {html.escape(str(args.reference))} vs {html.escape(str(args.test))}</h2><p>{summary}</p>
<table><tr><th>case</th><th>verdict</th><th>max diff</th><th>bad pixels</th><th>reference</th><th>test</th><th>diff</th><th>message</th></tr>
{''.join(r[2] for r in rows)}</table>"""
    (report_dir / 'report.html').write_text(page)
    return counts


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--reference', default='apple')
    parser.add_argument('--test', default=str(BUILD / 'libGLMetal.dylib'))
    parser.add_argument('--filter')
    parser.add_argument('--arch', choices=['x86_64', 'arm64'])
    parser.add_argument('--out', default=str(BUILD / 'glcompare-report'), type=pathlib.Path)
    parser.add_argument('--reuse-reference', action='store_true', help='keep the previous reference images')
    parser.add_argument('--timeout', type=float, default=600, help='for the whole batch')
    parser.add_argument('--case-timeout', type=float, default=30, help='per case, after a crash')
    args = parser.parse_args()

    reference_dir, test_dir, report_dir = args.out / 'reference', args.out / 'test', args.out / 'report'
    if not (args.reuse_reference and (reference_dir / 'results.tsv').exists()):
        run_provider(args, args.reference, reference_dir, 'reference')
    crashed = run_provider(args, args.test, test_dir, 'test')
    report_dir.mkdir(parents=True, exist_ok=True)
    counts = write_report(args, reference_dir, test_dir, report_dir)
    total = sum(counts.values())
    print(f'{counts.get("match", 0)}/{total} match; crashed: {len(crashed)}; report: {report_dir / "report.html"}')
    return 0 if counts.get('match', 0) == total else 1


if __name__ == '__main__':
    sys.exit(main())

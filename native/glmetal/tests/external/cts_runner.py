#!/usr/bin/env python3
"""Runs Khronos glcts over a case list in chunks, resuming after crashes
(Apple's own GL crashes on a few cases), and writes case<TAB>status.

usage: cts_runner.py GLCTS_DIR CASELIST OUT.tsv [KEY=VALUE environment...]"""
import os, re, subprocess, sys, tempfile, time
from collections import Counter

glcts_dir, caselist, out = sys.argv[1:4]
env = dict(os.environ)
chunk_size = int(os.environ.get("GLCTS_CHUNK_SIZE", "200"))
chunk_delay_ms = int(os.environ.get("GLCTS_CHUNK_DELAY_MS", "250"))
if not 1 <= chunk_size <= 2000 or not 0 <= chunk_delay_ms <= 5000:
    raise SystemExit("GLCTS_CHUNK_SIZE must be 1..2000 and GLCTS_CHUNK_DELAY_MS must be 0..5000")
for kv in sys.argv[4:]:
    key, value = kv.split("=", 1)
    env[key] = value
cases = [l.strip() for l in open(caselist) if l.strip()]
results, start = {}, time.time()
pending = list(cases)
chunk_index = 0
work = tempfile.mkdtemp(prefix="glcts-")
print("CTS logs:", work, flush=True)
while pending:
    chunk = pending[:chunk_size]
    chunk_index += 1
    chunk_file = os.path.join(work, f"chunk-{chunk_index:04}.txt")
    qpa = os.path.join(work, f"chunk-{chunk_index:04}.qpa")
    open(chunk_file, "w").write("\n".join(chunk) + "\n")
    if os.path.exists(qpa):
        os.remove(qpa)
    try:
        rc = subprocess.run(["./glcts", "--deqp-caselist-file=" + chunk_file, "--deqp-surface-type=fbo",
                             "--deqp-surface-width=64", "--deqp-surface-height=64",
                             "--deqp-terminate-on-device-lost=disable", "--deqp-log-images=disable",
                             "--deqp-log-shader-sources=disable", "--deqp-log-filename=" + qpa],
                            cwd=glcts_dir, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                            timeout=600).returncode
    except subprocess.TimeoutExpired:
        rc = "timeout"
    text = open(qpa, errors="replace").read() if os.path.exists(qpa) else ""
    done = re.findall(r'#beginTestCaseResult (\S+).*?(?:<Result StatusCode="(\w+)"|#terminateTestCaseResult (\w+))',
                      text, re.S)
    for name, status, terminated in done:
        results[name] = status or terminated
    unfinished = [name for name in re.findall(r"#beginTestCaseResult (\S+)", text)
                  if name in chunk and name not in results]
    if unfinished:
        # CTS uses its own tree order, which can differ from the case-list order.
        # Attribute a crash to the case that actually began, then retry the rest.
        results[unfinished[-1]] = "Timeout" if rc == "timeout" else "Crash"
    if not any(name in results for name in chunk):
        # Failure before any case began must still make progress.
        results[chunk[0]] = "Timeout" if rc == "timeout" else "LaunchError"
    pending = [name for name in pending if name not in results]
    if pending and chunk_delay_ms:
        time.sleep(chunk_delay_ms / 1000)
with open(out, "w") as f:
    for c in cases:
        f.write(f"{c}\t{results.get(c, 'Missing')}\n")
print(dict(Counter(results.get(c, "Missing") for c in cases)), f"{time.time() - start:.0f} s")

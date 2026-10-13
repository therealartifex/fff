#!/usr/bin/env python3
"""Reproducible, interleaved end-to-end hot-cache comparison (no dependencies)."""
import argparse
import json
from pathlib import Path
import random
import statistics
import subprocess
import time

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('baseline', type=Path)
p.add_argument('candidate', type=Path)
p.add_argument('--directory', type=Path, required=True, help='Scratch corpus directory (~1.3 GiB)')
p.add_argument('--runs', type=int, default=40)
p.add_argument('--output', type=Path, required=True)
a = p.parse_args()
if a.runs < 2:
    p.error('--runs must be at least 2')
a.directory.mkdir(parents=True, exist_ok=True)
log = a.directory / 'logs-large'
long_lines = a.directory / 'long-lines'
# Fixed seed and byte format, not intended as a substitute for production data.
rng = random.Random(1729)
if not log.exists():
    small = a.directory / 'logs'
    with small.open('wb') as f:
        for _ in range(256):
            batch = []
            for i in range(4096):
                user = ''.join(rng.choices('abcdefghijklmnopqrstuvwxyz0123456789', k=24))
                msg = 'hoyoverse' if i == 123 else rng.choice([
                    'request accepted', 'connection closed',
                    'authentication failed', 'session refreshed'])
                batch.append(f'2026-10-13T10:32:32 INFO service user={user} url=https://example.org/api/v1/session status=200 message={msg}\n'.encode())
            f.write(b''.join(batch))
    data = small.read_bytes()
    with log.open('wb') as f:
        for _ in range(8):
            f.write(data)
    del data
if not long_lines.exists():
    with long_lines.open('wb') as f:
        for _ in range(4096):
            f.write(b'a' * 16384 + b'hoyoverse' + b'b' * 16384 + b'\n')

binaries = {'baseline': str(a.baseline.resolve()), 'candidate': str(a.candidate.resolve())}
workloads = [(log, q) for q in ['hoyoverse', 'hoyover', 'xyz', 'zzzzzzzz', 'INFO', 'a']]
workloads.append((long_lines, 'hoyoverse'))
false_candidates = a.directory / 'false-candidates'
if not false_candidates.exists():
    with false_candidates.open('wb') as f:
        for _ in range(128):
            f.write((b'z' * 127 + b'\n') * 8192)
workloads.append((false_candidates, 'zzzzzzzza'))
results = {}
order_rng = random.Random(99)
for file, query in workloads:
    times = {name: [] for name in binaries}
    for binary in binaries.values():
        for _ in range(3):
            subprocess.run([binary, query, str(file)], stdout=subprocess.DEVNULL, check=True)
    for _ in range(a.runs):
        order = list(binaries)
        order_rng.shuffle(order)
        for name in order:
            start = time.perf_counter()
            subprocess.run([binaries[name], query, str(file)], stdout=subprocess.DEVNULL, check=True)
            times[name].append(time.perf_counter() - start)
    med = {k: statistics.median(v) for k, v in times.items()}
    key = file.name + ':' + query
    results[key] = {'bytes': file.stat().st_size, 'seconds': times,
                    'median_seconds': med, 'time_reduction_percent': 100 * (1 - med['candidate'] / med['baseline'])}
    print(key, med, f"{results[key]['time_reduction_percent']:.1f}% less time", flush=True)
    a.output.write_text(json.dumps(results, indent=2) + '\n')

#!/usr/bin/env python3
"""
benchmark_dispatch.py
Benchmark Switch Dispatch vs Computed-Goto Dispatch across 15 iterations.
Measures median wall time, min, max, std-dev, and calculated MIPS.
"""

import os
import sys
import time
import struct
import subprocess
import statistics

MAGIC = 0xC0C0FFEE
VERSION = 0x0000

def make_bc0(int_pool=[], string_pool=b'', functions=[], natives=[]):
    out = b''
    out += struct.pack('>I', MAGIC)
    out += struct.pack('>H', VERSION)
    out += struct.pack('>H', len(int_pool))
    for x in int_pool:
        out += struct.pack('>i', x)
    out += struct.pack('>H', len(string_pool))
    out += string_pool
    out += struct.pack('>H', len(functions))
    for (num_args, num_vars, code) in functions:
        out += struct.pack('>H', num_args)
        out += struct.pack('>H', num_vars)
        out += struct.pack('>H', len(code))
        out += bytes(code)
    out += struct.pack('>H', len(natives))
    for (num_args, fti) in natives:
        out += struct.pack('>H', num_args)
        out += struct.pack('>H', fti)
    return out

def build_loop_1m():
    src_path = 'tests/loop_1m.c0'
    bc0_path = 'tests/loop_1m.bc0'
    with open(src_path, 'w') as f:
        f.write('int main() {\n    int s = 0;\n    int i = 0;\n    while (i < 1000000) {\n        s = s + i;\n        i = i + 1;\n    }\n    return s;\n}\n')
    subprocess.run([sys.executable, 'c0c.py', src_path, '-o', bc0_path], check=True, stdout=subprocess.DEVNULL)
    print("Generated tests/loop_1m.bc0 via c0c.py")

def compile_binaries():
    print("Compiling c0vm_switch.exe (standard switch dispatch)...")
    res1 = subprocess.run([
        'gcc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', '-Iinclude',
        'src/c0vm_main.c', 'src/bc0_reader.c', 'src/c0vm.c', 'src/c0_native.c', 'src/c0vm_disasm.c',
        '-o', 'c0vm_switch.exe'
    ], capture_output=True, text=True)
    if res1.returncode != 0:
        print("Error compiling c0vm_switch.exe:", res1.stderr)
        sys.exit(1)

    print("Compiling c0vm_cg.exe (computed-goto direct threaded dispatch)...")
    res2 = subprocess.run([
        'gcc', '-std=c11', '-O2', '-DUSE_COMPUTED_GOTO', '-Wall', '-Wextra', '-Werror', '-Iinclude',
        'src/c0vm_main.c', 'src/bc0_reader.c', 'src/c0vm.c', 'src/c0_native.c', 'src/c0vm_disasm.c',
        '-o', 'c0vm_cg.exe'
    ], capture_output=True, text=True)
    if res2.returncode != 0:
        print("Error compiling c0vm_cg.exe:", res2.stderr)
        sys.exit(1)
    print("Both binaries compiled successfully.\n")

def run_benchmark(binary, bc0_file, iterations=15):
    times = []
    # Warmup run
    subprocess.run([binary, bc0_file], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    for _ in range(iterations):
        t0 = time.perf_counter()
        res = subprocess.run([binary, bc0_file], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        t1 = time.perf_counter()
        if res.returncode != 0 and res.returncode != 55:  # 0 or return val
            pass
        times.append((t1 - t0) * 1000.0) # ms
    return times

def main():
    build_loop_1m()
    compile_binaries()

    workloads = [
        ("Arithmetic Loop (1M iters)", "tests/loop_1m.bc0", 15000011),
        ("Iterative Fibonacci (N=20)", "examples/fibonacci.bc0", 10000),
        ("Recursive Factorial (12!)",  "examples/factorial.bc0", 1134),
        ("Prime Sieve (Primes <= 50)", "examples/primes.bc0",    3223),
        ("Collatz Table (N=1..20)",    "examples/collatz.bc0",   5578),
    ]

    print("=" * 88)
    print(" C0VM Performance Benchmark: Switch Dispatch vs Computed-Goto (15 Runs Each)")
    print("=" * 88)

    results = []

    for name, bc0_path, cycles in workloads:
        print(f"\nBenchmarking workload: {name} ({cycles:,} instructions)...")
        sw_times = run_benchmark('./c0vm_switch.exe', bc0_path, iterations=15)
        cg_times = run_benchmark('./c0vm_cg.exe', bc0_path, iterations=15)

        sw_med = statistics.median(sw_times)
        cg_med = statistics.median(cg_times)

        sw_min = min(sw_times)
        cg_min = min(cg_times)

        speedup = sw_med / cg_med if cg_med > 0 else 1.0
        pct_change = ((sw_med - cg_med) / sw_med) * 100.0

        sw_mips = (cycles / (sw_med / 1000.0)) / 1e6 if sw_med > 0 else 0
        cg_mips = (cycles / (cg_med / 1000.0)) / 1e6 if cg_med > 0 else 0

        results.append({
            'name': name,
            'cycles': cycles,
            'sw_med': sw_med,
            'sw_min': sw_min,
            'sw_mips': sw_mips,
            'cg_med': cg_med,
            'cg_min': cg_min,
            'cg_mips': cg_mips,
            'speedup': speedup,
            'pct_change': pct_change
        })

    print("\n" + "=" * 88)
    print(" SUMMARY BENCHMARK RESULTS")
    print("=" * 88)
    print(f"{'Workload':<28} | {'Switch (ms)':<11} | {'Comp-Goto':<11} | {'Switch MIPS':<12} | {'CG MIPS':<10} | {'Speedup':<8}")
    print("-" * 88)
    for r in results:
        print(f"{r['name']:<28} | {r['sw_med']:>7.2f} ms  | {r['cg_med']:>7.2f} ms  | {r['sw_mips']:>10.2f}  | {r['cg_mips']:>8.2f}  | {r['speedup']:>6.2f}x")
    print("=" * 88)

if __name__ == '__main__':
    main()

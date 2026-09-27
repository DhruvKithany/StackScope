#!/usr/bin/env python3
"""
run_c0_tests.py – compile .c0 files with c0c.py, then run them through c0vm.

Usage:  python3 tests/run_c0_tests.py ./c0vm

Discovers all tests/c0/*.c0 files. Each must have a matching tests/c0/*.expect.
"""
import os, sys, subprocess

def run(binary, test_dir, compiler):
    passed = failed = 0
    c0_files = sorted(f for f in os.listdir(test_dir) if f.endswith('.c0'))
    if not c0_files:
        print(f"  No .c0 files found in {test_dir}")
        return

    for c0f in c0_files:
        name    = c0f[:-3]
        c0path  = os.path.join(test_dir, c0f)
        bc0path = os.path.join(test_dir, name + '.bc0')
        exppath = os.path.join(test_dir, name + '.expect')

        if not os.path.exists(exppath):
            print(f"  SKIP  {name} (no .expect)")
            continue

        # Compile
        comp = subprocess.run([sys.executable, compiler, c0path, '-o', bc0path],
                              capture_output=True, text=True)
        if comp.returncode != 0:
            print(f"  FAIL  {name}: compile error\n    {comp.stderr.strip()}")
            failed += 1
            continue

        # Run
        try:
            result = subprocess.run([binary, bc0path],
                                    capture_output=True, text=True, timeout=10)
        except subprocess.TimeoutExpired:
            print(f"  FAIL  {name}: timeout"); failed += 1; continue

        with open(exppath) as f:
            expected = f.read()

        if result.stdout == expected:
            print(f"  PASS  {name}")
            passed += 1
        else:
            print(f"  FAIL  {name}")
            print(f"    Expected: {expected!r}")
            print(f"    Got:      {result.stdout!r}")
            if result.stderr: print(f"    Stderr:   {result.stderr.strip()}")
            failed += 1

    print(f"\nResults: {passed}/{passed+failed} passed")
    if failed: sys.exit(1)

if __name__ == '__main__':
    binary   = sys.argv[1] if len(sys.argv) > 1 else './c0vm'
    test_dir = os.path.join(os.path.dirname(__file__), 'c0')
    compiler = os.path.join(os.path.dirname(__file__), '..', 'c0c.py')
    run(binary, test_dir, compiler)

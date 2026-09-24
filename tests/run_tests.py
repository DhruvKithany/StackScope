#!/usr/bin/env python3
"""
run_tests.py – simple test harness for the C0VM.

Usage:  python3 run_tests.py <c0vm_binary> <tests_dir>

Each test is a pair of files:
  <name>.bc0   – pre-compiled bytecode
  <name>.expect – expected output (stdout) or expected exit code on last line

A test PASSES if the VM's stdout matches the .expect file exactly.
If the .expect file contains a single line starting with "EXIT:", the test
passes when the program exits with that return code.
"""

import os
import subprocess
import sys

def run_tests(binary: str, test_dir: str) -> None:
    passed = failed = 0

    bc0_files = sorted(
        f for f in os.listdir(test_dir) if f.endswith('.bc0')
    )

    if not bc0_files:
        print(f"  No .bc0 test files found in {test_dir}")
        return

    for bc0 in bc0_files:
        name    = bc0[:-4]
        bc0path = os.path.join(test_dir, bc0)
        exppath = os.path.join(test_dir, name + '.expect')

        if not os.path.exists(exppath):
            print(f"  SKIP  {name} (no .expect file)")
            continue

        with open(exppath) as f:
            expected = f.read()

        try:
            result = subprocess.run(
                [binary, bc0path],
                capture_output=True, text=True, timeout=10
            )
        except subprocess.TimeoutExpired:
            print(f"  FAIL  {name}: timeout")
            failed += 1
            continue

        # Check exit-code-only tests
        if expected.startswith("EXIT:"):
            expected_code = int(expected.split(':')[1].strip())
            if result.returncode == expected_code:
                print(f"  PASS  {name}")
                passed += 1
            else:
                print(f"  FAIL  {name}: expected exit {expected_code}, "
                      f"got {result.returncode}")
                failed += 1
            continue

        # Check stdout
        if result.stdout == expected:
            print(f"  PASS  {name}")
            passed += 1
        else:
            print(f"  FAIL  {name}")
            print(f"    Expected: {expected!r}")
            print(f"    Got:      {result.stdout!r}")
            if result.stderr:
                print(f"    Stderr:   {result.stderr.strip()}")
            failed += 1

    total = passed + failed
    print(f"\nResults: {passed}/{total} passed")
    if failed > 0:
        sys.exit(1)


if __name__ == '__main__':
    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} <binary> <test_dir>")
        sys.exit(1)
    run_tests(sys.argv[1], sys.argv[2])

#!/usr/bin/env python3
"""
run_all_tests.py - Complete test suite runner for C0 Virtual Machine.

Runs:
  1. Bytecode (.bc0) unit tests (tests/*.bc0)
  2. C0 source language tests (tests/c0/*.c0 compiled with c0c.py)
  3. Example programs (examples/*.c0 compiled with c0c.py)

Usage:
  python run_all_tests.py
"""

import os
import sys
import subprocess

def find_c0vm_binary():
    root = os.path.dirname(os.path.abspath(__file__))
    candidates = [
        os.path.join(root, "c0vm.exe"),
        os.path.join(root, "c0vm"),
    ]
    for c in candidates:
        if os.path.isfile(c):
            return c
    return None

def build_c0vm():
    root = os.path.dirname(os.path.abspath(__file__))
    srcs = [
        os.path.join(root, "src", "c0vm_main.c"),
        os.path.join(root, "src", "bc0_reader.c"),
        os.path.join(root, "src", "c0vm.c"),
        os.path.join(root, "src", "c0_native.c"),
        os.path.join(root, "src", "c0vm_disasm.c"),
    ]
    include_dir = os.path.join(root, "include")
    out_bin = os.path.join(root, "c0vm.exe" if os.name == "nt" else "c0vm")

    cmd = [
        "gcc", "-std=c11", "-Wall", "-Wextra",
        f"-I{include_dir}", "-O2",
        "-o", out_bin
    ] + srcs

    print(f"[*] Building C0VM: {' '.join(cmd[:6])} ...")
    res = subprocess.run(cmd, capture_output=True, text=True)
    if res.returncode != 0:
        print("[-] Build failed:")
        print(res.stderr)
        sys.exit(1)
    print(f"[+] Build successful: {out_bin}\n")
    return out_bin

def run_suite():
    root = os.path.dirname(os.path.abspath(__file__))
    c0vm = find_c0vm_binary()
    if not c0vm:
        c0vm = build_c0vm()

    c0c = os.path.join(root, "c0c.py")
    passed = 0
    failed = 0

    print("==================================================")
    print(" 1. Running Bytecode Unit Tests (tests/*.bc0)")
    print("==================================================")
    tests_dir = os.path.join(root, "tests")
    bc0_files = sorted(f for f in os.listdir(tests_dir) if f.endswith(".bc0"))

    for bc0 in bc0_files:
        name = bc0[:-4]
        bc0_path = os.path.join(tests_dir, bc0)
        exp_path = os.path.join(tests_dir, name + ".expect")
        if not os.path.exists(exp_path):
            continue

        with open(exp_path, "r", encoding="utf-8") as f:
            expected = f.read()

        res = subprocess.run([c0vm, bc0_path], capture_output=True, text=True)
        # Normalize newlines
        actual_out = res.stdout.replace("\r\n", "\n")
        exp_out = expected.replace("\r\n", "\n")

        if actual_out == exp_out:
            print(f"  [PASS] {name}")
            passed += 1
        else:
            print(f"  [FAIL] {name}")
            print(f"         Expected: {exp_out.strip()!r}")
            print(f"         Got:      {actual_out.strip()!r}")
            failed += 1

    print("\n==================================================")
    print(" 2. Running C0 Source Tests (tests/c0/*.c0)")
    print("==================================================")
    c0_dir = os.path.join(root, "tests", "c0")
    c0_files = sorted(f for f in os.listdir(c0_dir) if f.endswith(".c0"))

    for c0f in c0_files:
        name = c0f[:-3]
        c0_path = os.path.join(c0_dir, c0f)
        bc0_path = os.path.join(c0_dir, name + ".bc0")
        exp_path = os.path.join(c0_dir, name + ".expect")
        if not os.path.exists(exp_path):
            continue

        # Compile
        comp = subprocess.run([sys.executable, c0c, c0_path, "-o", bc0_path],
                              capture_output=True, text=True)
        if comp.returncode != 0:
            print(f"  [FAIL] {name} (compile error: {comp.stderr.strip()})")
            failed += 1
            continue

        # Run
        res = subprocess.run([c0vm, bc0_path], capture_output=True, text=True)
        actual_out = res.stdout.replace("\r\n", "\n")
        with open(exp_path, "r", encoding="utf-8") as f:
            exp_out = f.read().replace("\r\n", "\n")

        if actual_out == exp_out:
            print(f"  [PASS] {name}")
            passed += 1
        else:
            print(f"  [FAIL] {name}")
            print(f"         Expected: {exp_out.strip()!r}")
            print(f"         Got:      {actual_out.strip()!r}")
            failed += 1

    print("\n==================================================")
    print(" 3. Running Examples (examples/*.c0)")
    print("==================================================")
    ex_dir = os.path.join(root, "examples")
    ex_files = sorted(f for f in os.listdir(ex_dir) if f.endswith(".c0"))

    for exf in ex_files:
        name = exf[:-3]
        c0_path = os.path.join(ex_dir, exf)
        bc0_path = os.path.join(ex_dir, name + ".bc0")

        comp = subprocess.run([sys.executable, c0c, c0_path, "-o", bc0_path],
                              capture_output=True, text=True)
        if comp.returncode != 0:
            print(f"  [FAIL] {name} (compile error: {comp.stderr.strip()})")
            failed += 1
            continue

        res = subprocess.run([c0vm, bc0_path], capture_output=True, text=True)
        if res.returncode == 0:
            first_line = res.stdout.splitlines()[0] if res.stdout else "ok"
            print(f"  [PASS] {name} -> (First line: '{first_line}')")
            passed += 1
        else:
            print(f"  [FAIL] {name} exited with code {res.returncode}")
            failed += 1

    total = passed + failed
    print("\n==================================================")
    print(f" TOTAL RESULTS: {passed}/{total} PASSED")
    print("==================================================")
    if failed > 0:
        sys.exit(1)

if __name__ == "__main__":
    run_suite()

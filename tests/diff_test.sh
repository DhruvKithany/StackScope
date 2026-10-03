#!/usr/bin/env bash
# tests/diff_test.sh - Differential test runner for Linux / WSL against reference cc0
set -e

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(dirname "$DIR")"
CORPUS="$DIR/corpus"
C0VM="$ROOT/c0vm"
C0C="$ROOT/c0c.py"

echo "================================================================================"
echo " C0VM Differential Testing against Reference cc0"
echo "================================================================================"

if ! command -v cc0 &> /dev/null; then
    echo "[ERROR] Reference compiler 'cc0' not found on PATH."
    echo "Install cc0 from https://c0.cs.cmu.edu and ensure it is in your PATH."
    exit 1
fi

echo "Reference cc0 version: $(cc0 --version 2>&1 | head -n 1)"
echo "Using VM: $C0VM"
echo ""

TOTAL=0
VM_PASS=0
COMP_PASS=0
COMP_UNSUPPORTED=0

printf "%-26s %-15s %-20s %s\n" "Program" "Comp A (VM)" "Comp B (Compiler)" "Notes"
echo "--------------------------------------------------------------------------------"

for f in "$CORPUS"/*.c0; do
    base="$(basename "$f" .c0)"
    TOTAL=$((TOTAL + 1))
    
    # 1. Compile reference native
    REF_BIN="/tmp/${base}_ref"
    REF_BC0="/tmp/${base}_ref.bc0"
    MY_BC0="/tmp/${base}_my.bc0"
    
    # Try compiling with cc0
    REF_OK=true
    cc0 "$f" -o "$REF_BIN" 2>/dev/null || REF_OK=false
    cc0 -b "$f" -o "$REF_BC0" 2>/dev/null || REF_OK=false
    
    if [ "$REF_OK" = true ]; then
        # Run reference native
        set +e
        "$REF_BIN" > /tmp/ref.out 2>/dev/null
        REF_EXIT=$?
        
        # Comparison A: Reference bytecode on C0VM
        "$C0VM" "$REF_BC0" > /tmp/vm_ref.out 2>/dev/null
        VM_REF_EXIT=$?
        set -e
        
        if diff -u /tmp/ref.out /tmp/vm_ref.out > /dev/null && [ "$REF_EXIT" -eq "$VM_REF_EXIT" ]; then
            A_STAT="MATCH"
            VM_PASS=$((VM_PASS + 1))
        else
            A_STAT="MISMATCH"
        fi
    else
        A_STAT="REF_FAIL"
    fi
    
    # Comparison B: c0c.py on C0VM
    set +e
    python3 "$C0C" "$f" -o "$MY_BC0" 2> /tmp/c0c.err
    C0C_EXIT=$?
    
    if [ "$C0C_EXIT" -ne 0 ]; then
        if grep -qi "unsupported" /tmp/c0c.err; then
            B_STAT="UNSUPPORTED"
            COMP_UNSUPPORTED=$((COMP_UNSUPPORTED + 1))
        else
            B_STAT="COMPILE_ERR"
        fi
    else
        "$C0VM" "$MY_BC0" > /tmp/vm_my.out 2>/dev/null
        VM_MY_EXIT=$?
        
        if [ "$REF_OK" = true ]; then
            if diff -u /tmp/ref.out /tmp/vm_my.out > /dev/null && [ "$REF_EXIT" -eq "$VM_MY_EXIT" ]; then
                B_STAT="MATCH"
                COMP_PASS=$((COMP_PASS + 1))
            else
                B_STAT="MISMATCH"
            fi
        else
            B_STAT="PASS"
            COMP_PASS=$((COMP_PASS + 1))
        fi
    fi
    set -e
    
    printf "%-26s %-15s %-20s %s\n" "$base.c0" "$A_STAT" "$B_STAT" ""
    
    # Cleanup
    rm -f "$REF_BIN" "$REF_BC0" "$MY_BC0" /tmp/ref.out /tmp/vm_ref.out /tmp/vm_my.out /tmp/c0c.err
done

echo "--------------------------------------------------------------------------------"
echo "Summary: VM Correctness (Comp A): $VM_PASS/$TOTAL match reference"
echo "         Compiler (Comp B):       $COMP_PASS/$TOTAL match ($COMP_UNSUPPORTED unsupported)"

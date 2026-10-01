#!/usr/bin/env python3
"""
generate_tests.py – generate hand-crafted .bc0 bytecode test files.

Run this script once to produce the .bc0 files in the tests/ directory.
Each function encodes the bc0 binary format directly.

BC0 Binary layout (big-endian):
  uint32  magic     = 0xC0C0FFEE
  uint16  version   = 0x0000
  uint16  int_count
  int32[] int_pool
  uint16  string_count
  char[]  string_pool
  uint16  function_count
  [function:
    uint16 num_args
    uint16 num_vars
    uint16 code_length
    uint8[] code ]
  uint16  native_count
  [native:
    uint16 num_args
    uint16 function_table_index ]

Opcodes used:
  0x10 BIPUSH  <b>
  0x13 ILDC    <c1> <c2>
  0x15 VLOAD   <i>
  0x36 VSTORE  <i>
  0x57 POP
  0x59 DUP
  0x60 IADD
  0x64 ISUB
  0x68 IMUL
  0x6C IDIV
  0x70 IREM
  0x74 INEG
  0x7E IAND
  0x80 IOR
  0x82 IXOR
  0x99 IFEQ    <o1> <o2>
  0x9A IFNE    <o1> <o2>
  0x9B IFLT    <o1> <o2>
  0x9C IFGE    <o1> <o2>
  0xA7 GOTO    <o1> <o2>
  0xB0 RETURN
  0xB7 INVOKENATIVE <c1> <c2>
  0xB8 INVOKESTATIC <c1> <c2>
  0xBB NEW     <s>
  0xBC NEWARRAY <s>
  0xBE ARRAYLENGTH
  0x62 AADDF   <f>
  0x63 AADDS
  0x2E IMLOAD
  0x4F IMSTORE
"""

import struct
import os

MAGIC   = 0xC0C0FFEE
VERSION = 0x0000

# Native function indices (must match c0_native_table[] in c0_native.c)
NATIVE_PRINTINT  = 2
NATIVE_PRINTLN   = 1
NATIVE_PRINT     = 0

def u8(v):   return struct.pack('B', v & 0xFF)
def u16(v):  return struct.pack('>H', v & 0xFFFF)
def u32(v):  return struct.pack('>I', v & 0xFFFFFFFF)
def i32(v):  return struct.pack('>i', v)

def make_bc0(int_pool=[], string_pool=b'', functions=[], natives=[]):
    out = b''
    out += u32(MAGIC)
    out += u16(VERSION)
    out += u16(len(int_pool))
    for x in int_pool:
        out += i32(x)
    out += u16(len(string_pool))
    out += string_pool
    out += u16(len(functions))
    for (num_args, num_vars, code) in functions:
        out += u16(num_args)
        out += u16(num_vars)
        out += u16(len(code))
        out += bytes(code)
    out += u16(len(natives))
    for (num_args, fti) in natives:
        out += u16(num_args)
        out += u16(fti)
    return out

def write_test(name, bc0_bytes, expected_stdout):
    os.makedirs('tests', exist_ok=True)
    with open(f'tests/{name}.bc0', 'wb') as f:
        f.write(bc0_bytes)
    with open(f'tests/{name}.expect', 'w') as f:
        f.write(expected_stdout)
    print(f'  Generated {name}')

# -----------------------------------------------------------------------
# Test 1: return42 – _c0_main pushes 42 and returns it.
# Expected: "Program exited with return value 42\n"
# -----------------------------------------------------------------------
# Code for _c0_main():
#   bipush 42
#   return
code_return42 = [
    0x10, 42,   # bipush 42
    0xB0,       # return
]
write_test('return42',
    make_bc0(functions=[(0, 0, code_return42)]),
    "Program exited with return value 42\n")

# -----------------------------------------------------------------------
# Test 2: add – 3 + 4 = 7
# -----------------------------------------------------------------------
code_add = [
    0x10, 3,    # bipush 3
    0x10, 4,    # bipush 4
    0x60,       # iadd
    0xB0,       # return
]
write_test('add',
    make_bc0(functions=[(0, 0, code_add)]),
    "Program exited with return value 7\n")

# -----------------------------------------------------------------------
# Test 3: multiply – 6 * 7 = 42
# -----------------------------------------------------------------------
code_mul = [
    0x10, 6,    # bipush 6
    0x10, 7,    # bipush 7
    0x68,       # imul
    0xB0,       # return
]
write_test('multiply',
    make_bc0(functions=[(0, 0, code_mul)]),
    "Program exited with return value 42\n")

# -----------------------------------------------------------------------
# Test 4: divide – 100 / 5 = 20
# -----------------------------------------------------------------------
code_div = [
    0x13, 0, 0,  # ildc 0  (int_pool[0] = 100)
    0x10, 5,     # bipush 5
    0x6C,        # idiv
    0xB0,        # return
]
write_test('divide',
    make_bc0(int_pool=[100], functions=[(0, 0, code_div)]),
    "Program exited with return value 20\n")

# -----------------------------------------------------------------------
# Test 5: modulo – 17 % 5 = 2
# -----------------------------------------------------------------------
code_rem = [
    0x10, 17,
    0x10, 5,
    0x70,       # irem
    0xB0,
]
write_test('modulo',
    make_bc0(functions=[(0, 0, code_rem)]),
    "Program exited with return value 2\n")

# -----------------------------------------------------------------------
# Test 6: negate – -(10) = -10  (return -10, but C main returns unsigned,
# so we wrap to positive; instead return 246 via (uint8) trick – or just
# test that the VM correctly returns -10 as int32.)
# We test that the VM prints the right value to stdout via printint.
#
# _c0_main:
#   bipush 10
#   ineg
#   invokenative PRINTINT  (prints "-10")
#   bipush 0
#   return
# -----------------------------------------------------------------------
# Native: printint takes 1 arg, index 2
code_neg = [
    0x10, 10,                           # bipush 10
    0x74,                               # ineg
    0xB7, 0x00, 0x00,                   # invokenative 0 (native_pool[0])
    0x57,                               # pop return value of printint
    0x10, 0,                            # bipush 0
    0xB0,                               # return
]
write_test('negate',
    make_bc0(
        functions=[(0, 0, code_neg)],
        natives=[(1, NATIVE_PRINTINT)]  # native 0: printint, 1 arg, fti=2
    ),
    "-10Program exited with return value 0\n")

# -----------------------------------------------------------------------
# Test 7: locals – store and load local variables
# int x = 5; int y = 3; return x - y;
# V[0]=x, V[1]=y (num_vars=2)
# -----------------------------------------------------------------------
code_locals = [
    0x10, 5,    # bipush 5
    0x36, 0,    # vstore 0  (x = 5)
    0x10, 3,    # bipush 3
    0x36, 1,    # vstore 1  (y = 3)
    0x15, 0,    # vload 0   (push x)
    0x15, 1,    # vload 1   (push y)
    0x64,       # isub
    0xB0,       # return
]
write_test('locals',
    make_bc0(functions=[(0, 2, code_locals)]),
    "Program exited with return value 2\n")

# -----------------------------------------------------------------------
# Test 8: ifelse – if (3 > 5) return 1; else return 0;
#
# bipush 3
# bipush 5
# IF_ICMPGT <offset to return 1>
# bipush 0
# return
# bipush 1
# return
#
# Encoding IFGT: 0x9D <o1> <o2> where offset is from the START of IFGT.
# -----------------------------------------------------------------------
# Layout of code_ifelse:
# 00: 10 03       bipush 3
# 02: 10 05       bipush 5
# 04: 9B 00 07    iflt +7  (if 3<5 jump to 04+7=11)
#   (else fall through)
# 07: 10 00       bipush 0
# 09: B0          return
# 10: (dead)
# 11: 10 01       bipush 1
# 13: B0          return
#
# Wait – we want: if 3 > 5 -> 1 else -> 0
# 3 > 5 is false, so we want 0.
# Let's use IFLT: pop y=5, pop x=3; branch if x < y (3 < 5 is true)
# So: push 3, push 5, IF_ICMPLT -> branch to "push 1; return"
# Fall-through: push 0; return
#
# 00: 10 03       bipush 3
# 02: 10 05       bipush 5
# 04: A1 00 07    IF_ICMPLT +7  (target = 04 + 7 = 11? no, target=pc-3+off)
#                 After fetching A1 o1 o2, pc=07, target = 07-3+off = 04+off
#                 We want target = 11, so off = 11-04 = 7
# 07: 10 00       bipush 0
# 09: B0          return
# 0A: 10 01       bipush 1
# 0C: B0          return
code_ifelse = [
    0x10, 3,       # 00: bipush 3
    0x10, 5,       # 02: bipush 5
    0xA1, 0,  7,   # 04: IF_ICMPLT +7  (3<5 → true → jump to 0A+1=0B? let me recount)
    0x10, 0,       # 07: bipush 0
    0xB0,          # 09: return
    0x10, 1,       # 0A: bipush 1
    0xB0,          # 0C: return
]
# Recount: opcode at pc=4; after fetch of 3 bytes pc=7; target = 7-3+7 = 11 = 0x0B
# But 0x0A = 10 (bipush 1), 0x0B = 01 (bipush arg). Let me redo:
# pc after fetching A1,0,7 is 7. target = pc - 3 + offset = 7-3+7=11
# but code[0x0A]=0x10, code[0x0B]=0x01, code[0x0C]=0xB0
# 3<5 is true → we jump to index 11 = 0x0B = 0x01 → tries to execute 0x01 = ACONST_NULL → wrong
#
# Let me be careful. Target should be 0x0A (the bipush 1 instruction).
# target = pc_after_fetch - 3 + offset  = 7 - 3 + offset = 4 + offset
# We want 4 + offset = 0x0A = 10 → offset = 6
code_ifelse = [
    0x10, 3,       # 00,01: bipush 3
    0x10, 5,       # 02,03: bipush 5
    0xA1, 0, 6,    # 04,05,06: IF_ICMPLT +6 -> target = (07-3)+6 = 10 = 0x0A [OK]
    0x10, 0,       # 07,08: bipush 0
    0xB0,          # 09: return
    0x10, 1,       # 0A,0B: bipush 1
    0xB0,          # 0C: return
]
# 3 < 5 → true → jump to 0x0A (push 1, return 1)
write_test('ifelse',
    make_bc0(functions=[(0, 0, code_ifelse)]),
    "Program exited with return value 1\n")

# -----------------------------------------------------------------------
# Test 9: loop – sum 1..10 = 55
# int s = 0; int i = 1;
# while (i <= 10): s += i; i++;
# return s;
# V[0]=s, V[1]=i
#
# 00: 10 00       bipush 0
# 02: 36 00       vstore 0   (s=0)
# 04: 10 01       bipush 1
# 06: 36 01       vstore 1   (i=1)
# -- loop start at 08 --
# 08: 15 01       vload 1    (push i)
# 0A: 10 0A       bipush 10
# 0C: A4 00 12    IF_ICMPLE +18  → target = (0F-3)+18 = 0C+18=24=0x18 (exit)
#                 Wait: pc after fetch = 0F, target = 0F-3+off = 0C+off
#                 We want exit at end of loop body. Let's figure out loop body length.
# 0F: 15 00       vload 0   (s)
# 11: 15 01       vload 1   (i)
# 13: 60          iadd
# 14: 36 00       vstore 0  (s += i)
# 16: 15 01       vload 1   (i)
# 18: 10 01       bipush 1
# 1A: 60          iadd
# 1B: 36 01       vstore 1  (i++)
# 1D: A7 FF EA    goto -22  → target = (20-3)+(-22) = 1D+(-22) = 1D-16 = 7?
#     Hmm, we want to go back to 08. pc after fetch = 20 (0x20)
#     target = 0x20 - 3 + offset = 0x1D + offset = 0x08
#     offset = 0x08 - 0x1D = -21 (signed)
#     -21 as int16: 0xFFEB
# 20: 15 00       vload 0   (s = 55)
# 22: B0          return
#
# Exit point (after loop): at 0x20
# IF_ICMPLE: branch if i <= 10 (i is on stack, 10 is on stack, compares a<=b)
# Wait – IF_ICMPLE pops b then a, branches if a <= b.
# We push i first, then 10. So a=i, b=10. We want to LOOP while i<=10.
# So we should fall through to body when i<=10 and jump OUT when i>10.
# Flip: use IF_ICMPGT to jump out.
# 0C: A3 00 ?? IF_ICMPGT +?? → jump to exit when i > 10
# exit is at 0x20. pc after fetch = 0x0F. target = 0x0F-3+off = 0x0C+off=0x20 → off=0x14=20
#
code_loop = [
    0x10, 0,          # 00,01: bipush 0
    0x36, 0,          # 02,03: vstore 0   (s=0)
    0x10, 1,          # 04,05: bipush 1
    0x36, 1,          # 06,07: vstore 1   (i=1)
    # loop top at 0x08
    0x15, 1,          # 08,09: vload 1    (i)
    0x10, 10,         # 0A,0B: bipush 10
    0xA3, 0, 20,      # 0C,0D,0E: IF_ICMPGT +20 → exit at 0x0F-3+20=0x20
    # loop body at 0x0F
    0x15, 0,          # 0F,10: vload 0    (s)
    0x15, 1,          # 11,12: vload 1    (i)
    0x60,             # 13:    iadd
    0x36, 0,          # 14,15: vstore 0   (s+=i)
    0x15, 1,          # 16,17: vload 1    (i)
    0x10, 1,          # 18,19: bipush 1
    0x60,             # 1A:    iadd
    0x36, 1,          # 1B,1C: vstore 1   (i++)
    0xA7, 0xFF, 0xEB, # 1D,1E,1F: goto -21 -> target=0x20-3+(-21)=0x1D-21=0x08 [OK]
    # exit at 0x20
    0x15, 0,          # 20,21: vload 0    (s)
    0xB0,             # 22:    return
]
write_test('loop',
    make_bc0(functions=[(0, 2, code_loop)]),
    "Program exited with return value 55\n")

# -----------------------------------------------------------------------
# Test 10: function call – factorial(5) = 120
#
# function_pool[0] = _c0_main: calls factorial(5)
# function_pool[1] = factorial(n): recursive
#
# _c0_main:
#   bipush 5
#   invokestatic 1     (factorial)
#   return
#
# factorial(n):   [num_args=1, num_vars=1, V[0]=n]
#   vload 0             (n)
#   ifeq -> base        (if n==0 jump to base case)
#   vload 0             (n)
#   vload 0             (n)
#   bipush 1
#   isub                (n-1)
#   invokestatic 1      (factorial(n-1))
#   imul                (n * factorial(n-1))
#   return
#   (base:)
#   bipush 1
#   return
#
# Layout of factorial code:
# 00: 15 00       vload 0
# 02: 99 00 0C    ifeq +12  → target=02+12=14? pc_after=05; target=05-3+12=14=0x0E
# 05: 15 00       vload 0   (n)
# 07: 15 00       vload 0   (n)
# 09: 10 01       bipush 1
# 0B: 64          isub      (n-1)
# 0C: B8 00 01    invokestatic 1
# 0F: 68          imul
# 10: B0          return
# 11: (dead? no: 0x0E)
# Let me recalculate: pc after fetching 99,0,off is 0x05
# target = 0x05 - 3 + off = 0x02 + off
# We want base case at 0x11 (after return):
# Actually let me place base case right after the recursive return:
# 11: 10 01       bipush 1
# 13: B0          return
# target = 0x02 + off = 0x11 → off = 0x0F = 15

code_factorial_main = [
    0x10, 5,          # bipush 5
    0xB8, 0x00, 0x01, # invokestatic 1
    0xB0,             # return
]

code_factorial = [
    0x15, 0,          # 00,01: vload 0   (n)
    0x99, 0x00, 0x0F, # 02,03,04: ifeq +15 -> target=0x05-3+15=0x11 [OK]
    0x15, 0,          # 05,06: vload 0   (n)
    0x15, 0,          # 07,08: vload 0   (n)
    0x10, 1,          # 09,0A: bipush 1
    0x64,             # 0B:    isub      (n-1)
    0xB8, 0x00, 0x01, # 0C,0D,0E: invokestatic 1
    0x68,             # 0F:    imul      (n * fact(n-1))
    0xB0,             # 10:    return
    0x10, 1,          # 11,12: bipush 1  (base case)
    0xB0,             # 13:    return
]

write_test('factorial',
    make_bc0(functions=[
        (0, 0, code_factorial_main),   # function 0: _c0_main
        (1, 1, code_factorial),         # function 1: factorial
    ]),
    "Program exited with return value 120\n")

# -----------------------------------------------------------------------
# Test 11: bitwise – (0b1100 & 0b1010) | 0b0001 = 0b1001 = 9
# -----------------------------------------------------------------------
code_bitwise = [
    0x10, 0b00001100,  # bipush 12
    0x10, 0b00001010,  # bipush 10
    0x7E,              # iand  → 8
    0x10, 0b00000001,  # bipush 1
    0x80,              # ior   → 9
    0xB0,              # return
]
write_test('bitwise',
    make_bc0(functions=[(0, 0, code_bitwise)]),
    "Program exited with return value 9\n")

# -----------------------------------------------------------------------
# Test 12: negative_return – returns -1 (255 as uint8 exit, but as int32 -1)
# The test checks that the VM returns the integer -1 from main.
# Our VM prints the actual int32 value.
# -----------------------------------------------------------------------
code_neg_ret = [
    0x10, 0xFF,  # bipush -1 (0xFF sign-extended from int8 = -1)
    0xB0,
]
write_test('negative_return',
    make_bc0(functions=[(0, 0, code_neg_ret)]),
    "Program exited with return value -1\n")

# -----------------------------------------------------------------------
# Test 13: array_heap – allocate an array of 3 ints, store & load
# arr[0] = 10; arr[1] = 20; arr[2] = 30; return arr[0] + arr[1] + arr[2] = 60
# -----------------------------------------------------------------------
code_array = [
    0x10, 3,       # bipush 3 (array length)
    0xBC, 4,       # newarray elt_size=4
    0x36, 0,       # vstore 0 (arr = V[0])

    # arr[0] = 10
    0x15, 0,       # vload 0
    0x10, 0,       # bipush 0 (idx)
    0x63,          # aadds
    0x10, 10,      # bipush 10
    0x4F,          # imstore

    # arr[1] = 20
    0x15, 0,       # vload 0
    0x10, 1,       # bipush 1
    0x63,          # aadds
    0x10, 20,      # bipush 20
    0x4F,          # imstore

    # arr[2] = 30
    0x15, 0,       # vload 0
    0x10, 2,       # bipush 2
    0x63,          # aadds
    0x10, 30,      # bipush 30
    0x4F,          # imstore

    # load arr[0] + arr[1] + arr[2]
    0x15, 0, 0x10, 0, 0x63, 0x2E,  # load arr[0] -> 10
    0x15, 0, 0x10, 1, 0x63, 0x2E,  # load arr[1] -> 20
    0x60,                          # iadd -> 30
    0x15, 0, 0x10, 2, 0x63, 0x2E,  # load arr[2] -> 30
    0x60,                          # iadd -> 60
    0xB0                           # return 60
]
write_test('array_heap',
    make_bc0(functions=[(0, 1, code_array)]),
    "Program exited with return value 60\n")

print("\nAll test files generated in tests/")


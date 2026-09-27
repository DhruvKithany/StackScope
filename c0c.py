#!/usr/bin/env python3
"""
c0c.py – A minimal C0 → .bc0 bytecode compiler.

Supports a useful subset of C0:
  - int/bool/string variables and literals
  - Arithmetic: + - * / %
  - Bitwise:    & | ^ ~ << >>
  - Comparison: == != < <= > >=
  - Logical:    && || !
  - if / else
  - while loops
  - Function definitions and calls (including recursion)
  - return statements
  - print / println / printint (via native calls)
  - string literals and int literals (including large ints via int_pool)

Usage:
  python3 c0c.py program.c0          → produces program.bc0
  python3 c0c.py program.c0 -o out.bc0

Then run:
  ./c0vm program.bc0
"""

import sys
import re
import struct
import os
from dataclasses import dataclass, field
from typing import List, Dict, Optional, Tuple

# ── Opcodes ─────────────────────────────────────────────────────────────────
ACONST_NULL  = 0x01
BIPUSH       = 0x10
ILDC         = 0x13
ALDC         = 0x14
VLOAD        = 0x15
VSTORE       = 0x36
POP          = 0x57
DUP          = 0x59
SWAP         = 0x5F
IADD         = 0x60
ISUB         = 0x64
IMUL         = 0x68
IDIV         = 0x6C
IREM         = 0x70
INEG         = 0x74
ISHL         = 0x78
ISHR         = 0x7A
IAND         = 0x7E
IOR          = 0x80
IXOR         = 0x82
IFEQ         = 0x99
IFNE         = 0x9A
IFLT         = 0x9B
IFGE         = 0x9C
IFGT         = 0x9D
IFLE         = 0x9E
IF_ICMPEQ    = 0x9F
IF_ICMPNE    = 0xA0
IF_ICMPLT    = 0xA1
IF_ICMPGE    = 0xA2
IF_ICMPGT    = 0xA3
IF_ICMPLE    = 0xA4
ACMPEQ       = 0xA5
GOTO         = 0xA7
INVOKEVIRTUAL= 0xB6
INVOKENATIVE = 0xB7
INVOKESTATIC = 0xB8
RETURN       = 0xB0
ATHROW       = 0xBF
NEW          = 0xBB
NEWARRAY     = 0xBC
ARRAYLENGTH  = 0xBE
IMLOAD       = 0x2E
IMSTORE      = 0x4F
AMLOAD       = 0x2F
AMSTORE      = 0x50
AADDF        = 0x62
AADDS        = 0x63

# ── Native function table (must match c0_native_table[] in c0_native.c) ────
NATIVE = {
    'print':          (0,  1),   # (num_args, function_table_index)
    'println':        (1,  1),
    'printint':       (2,  1),
    'printbool':      (3,  1),
    'printchar':      (4,  1),
    'readline':       (5,  0),
    'string_length':  (6,  1),
    'string_charat':  (7,  2),
    'string_compare': (8,  2),
    'string_equal':   (9,  2),
    'string_sub':     (10, 3),
    'string_join':    (11, 2),
    'string_fromchar':(12, 1),
    'int_to_string':  (16, 1),
    'string_to_int':  (17, 1),
    'bool_to_string': (18, 1),
    'error':          (22, 1),
}

# ── Tokeniser ────────────────────────────────────────────────────────────────
TOKEN_RE = re.compile(r"""
    (?P<COMMENT>//[^\n]*) |
    (?P<MCOMMENT>/\*.*?\*/) |
    (?P<USE>\#use\s+<[^>]+>) |
    (?P<STRING>"(?:[^"\\]|\\.)*") |
    (?P<CHAR>'(?:[^'\\]|\\.)') |
    (?P<NUM>0x[0-9A-Fa-f]+|\d+) |
    (?P<ID>[A-Za-z_][A-Za-z0-9_]*) |
    (?P<OP>&&|\|\||==|!=|<=|>=|<<|>>|[+\-*/%&|^~<>!(){}[\];,=]) |
    (?P<NEWLINE>\n) |
    (?P<SPACE>[ \t\r]+)
""", re.VERBOSE | re.DOTALL)

KEYWORDS = {
    'int','bool','string','char','void',
    'if','else','while','for','return',
    'true','false','NULL',
    'struct','typedef','alloc','alloc_array',
}

@dataclass
class Token:
    kind: str
    val:  str
    line: int

def tokenise(src: str) -> List[Token]:
    tokens = []
    line = 1
    for m in TOKEN_RE.finditer(src):
        kind = m.lastgroup
        val  = m.group()
        if kind in ('COMMENT','MCOMMENT','SPACE','USE'):
            line += val.count('\n')
            continue
        if kind == 'NEWLINE':
            line += 1
            continue
        if kind == 'ID' and val in KEYWORDS:
            kind = val.upper()
        tokens.append(Token(kind, val, line))
    return tokens

# ── Parser & Code Generator ─────────────────────────────────────────────────

class CompileError(Exception):
    pass

class FunctionCompiler:
    """Compiles a single C0 function to bytecode."""

    def __init__(self, name: str, params: List[Tuple[str,str]],
                 all_fns: Dict, string_pool_ref, int_pool_ref,
                 native_pool_ref):
        self.name       = name
        self.code       = []           # list of ints (bytecodes)
        self.locals     : Dict[str,int] = {}   # var_name → slot index
        self.num_vars   = 0
        self.all_fns    = all_fns     # name → fn index (populated after parse)
        self.string_pool: List[str]   = string_pool_ref
        self.int_pool   : List[int]   = int_pool_ref
        self.native_pool: List[Tuple[int,int]] = native_pool_ref  # (num_args, fti)
        self.native_index: Dict[str,int] = {}  # native fn name → pool index

        # Allocate slots for parameters
        for (_, pname) in params:
            self._declare(pname)

    def _declare(self, name: str) -> int:
        if name in self.locals:
            return self.locals[name]
        idx = self.num_vars
        self.locals[name] = idx
        self.num_vars += 1
        return idx

    def _local(self, name: str) -> int:
        if name not in self.locals:
            raise CompileError(f"undeclared variable '{name}'")
        return self.locals[name]

    # ── emit helpers ──────────────────────────────────────────────────────

    def emit(self, *bytes_):
        self.code.extend(bytes_)

    def emit_bipush(self, v: int):
        self.emit(BIPUSH, v & 0xFF)

    def emit_ildc(self, v: int) -> None:
        # Add to int pool if not present
        if v in self.int_pool:
            idx = self.int_pool.index(v)
        else:
            idx = len(self.int_pool)
            self.int_pool.append(v)
        self.emit(ILDC, (idx >> 8) & 0xFF, idx & 0xFF)

    def emit_int(self, v: int):
        """Emit the most compact integer push."""
        if -128 <= v <= 127:
            self.emit_bipush(v & 0xFF)
        else:
            self.emit_ildc(v)

    def emit_aldc(self, s: str) -> None:
        """Emit an ALDC pushing a pointer into the string pool."""
        # Find or add the string
        # String pool stores null-terminated strings concatenated
        encoded = s + '\0'
        pool_str = ''.join(self.string_pool)
        if encoded in pool_str:
            idx = pool_str.index(encoded)
        else:
            idx = len(pool_str)
            self.string_pool.append(encoded)
        self.emit(ALDC, (idx >> 8) & 0xFF, idx & 0xFF)

    def emit_vload(self, slot: int):
        self.emit(VLOAD, slot & 0xFF)

    def emit_vstore(self, slot: int):
        self.emit(VSTORE, slot & 0xFF)

    # ── Patch helpers (for forward jumps) ────────────────────────────────

    def emit_jump(self, opcode: int) -> int:
        """Emit a branch opcode with placeholder offset. Returns patch position."""
        pos = len(self.code)
        self.emit(opcode, 0x00, 0x00)
        return pos

    def patch_jump(self, pos: int):
        """Patch the jump at pos to target current position."""
        # offset = target - start_of_instruction
        target = len(self.code)
        offset = target - pos   # signed offset from start of branch instruction
        if not (-32768 <= offset <= 32767):
            raise CompileError("jump offset too large")
        self.code[pos+1] = (offset >> 8) & 0xFF
        self.code[pos+2] = offset & 0xFF

    def emit_goto(self, target: int):
        """Emit a goto with a known backward target."""
        cur = len(self.code)   # position of GOTO instruction
        offset = target - cur
        self.emit(GOTO, (offset >> 8) & 0xFF, offset & 0xFF)

    def _get_or_add_native(self, fname: str) -> int:
        """Return the index in native_pool for a native function, adding if needed."""
        if fname in self.native_index:
            return self.native_index[fname]
        fti, nargs = NATIVE[fname][0], NATIVE[fname][1]
        idx = len(self.native_pool)
        self.native_pool.append((nargs, fti))
        self.native_index[fname] = idx
        return idx

    # ── Statement compilation ─────────────────────────────────────────────

    def compile_stmt(self, tokens, pos):
        t = tokens[pos]

        # -- Variable declaration: int x = expr; or int x;
        if t.kind in ('INT','BOOL','STRING','CHAR','VOID'):
            pos += 1  # skip type
            name_tok = tokens[pos]; pos += 1  # variable name
            slot = self._declare(name_tok.val)
            if tokens[pos].val == '=':
                pos += 1  # skip =
                pos = self.compile_expr(tokens, pos)
                self.emit_vstore(slot)
            # might be just a declaration; either way expect ;
            if tokens[pos].val == ';':
                pos += 1
            return pos

        # -- Assignment: x = expr;
        if t.kind == 'ID' and pos+1 < len(tokens) and tokens[pos+1].val == '=':
            slot = self._local(t.val)
            pos += 2  # skip name =
            pos = self.compile_expr(tokens, pos)
            self.emit_vstore(slot)
            if tokens[pos].val == ';': pos += 1
            return pos

        # -- Compound assignment: x += expr; x -= expr; x *= expr; etc.
        if t.kind == 'ID' and pos+1 < len(tokens) and tokens[pos+1].val in ('+=','-=','*=','/=','%=','&=','|=','^='):
            slot = self._local(t.val)
            op = tokens[pos+1].val
            pos += 2
            self.emit_vload(slot)
            pos = self.compile_expr(tokens, pos)
            ops = {'+=':IADD,'-=':ISUB,'*=':IMUL,'/=':IDIV,'%=':IREM,
                   '&=':IAND,'|=':IOR,'^=':IXOR}
            self.emit(ops[op])
            self.emit_vstore(slot)
            if tokens[pos].val == ';': pos += 1
            return pos

        # -- return expr;
        if t.kind == 'RETURN':
            pos += 1
            if tokens[pos].val != ';':
                pos = self.compile_expr(tokens, pos)
            else:
                self.emit_int(0)
            if tokens[pos].val == ';': pos += 1
            self.emit(RETURN)
            return pos

        # -- if ( expr ) { ... } [else { ... }]
        if t.kind == 'IF':
            pos += 1  # skip 'if'
            assert tokens[pos].val == '('; pos += 1
            pos = self.compile_expr(tokens, pos)   # condition on stack
            assert tokens[pos].val == ')'; pos += 1
            jump_false = self.emit_jump(IFEQ)      # jump if false
            pos = self.compile_block(tokens, pos)  # then-block
            if pos < len(tokens) and tokens[pos].kind == 'ELSE':
                pos += 1
                jump_end = self.emit_jump(GOTO)    # skip else
                self.patch_jump(jump_false)
                pos = self.compile_block(tokens, pos)  # else-block
                self.patch_jump(jump_end)
            else:
                self.patch_jump(jump_false)
            return pos

        # -- while ( expr ) { ... }
        if t.kind == 'WHILE':
            pos += 1; assert tokens[pos].val == '('; pos += 1
            loop_top = len(self.code)
            pos = self.compile_expr(tokens, pos)   # condition
            assert tokens[pos].val == ')'; pos += 1
            jump_out = self.emit_jump(IFEQ)
            pos = self.compile_block(tokens, pos)
            self.emit_goto(loop_top)
            self.patch_jump(jump_out)
            return pos

        # -- for ( init ; cond ; update ) { ... }
        if t.kind == 'FOR':
            pos += 1; assert tokens[pos].val == '('; pos += 1
            # init
            if tokens[pos].val != ';':
                pos = self.compile_stmt(tokens, pos)
            else:
                pos += 1
            loop_top = len(self.code)
            # condition
            if tokens[pos].val != ';':
                pos = self.compile_expr(tokens, pos)
                jump_out = self.emit_jump(IFEQ)
            else:
                jump_out = None
            assert tokens[pos].val == ';'; pos += 1
            # collect update tokens until ')'
            update_tokens = []
            depth = 0
            while not (tokens[pos].val == ')' and depth == 0):
                if tokens[pos].val == '(': depth += 1
                if tokens[pos].val == ')': depth -= 1
                update_tokens.append(tokens[pos])
                pos += 1
            assert tokens[pos].val == ')'; pos += 1
            pos = self.compile_block(tokens, pos)
            # emit update
            if update_tokens:
                update_tokens.append(Token('OP', ';', 0))
                self.compile_stmt(update_tokens, 0)
            self.emit_goto(loop_top)
            if jump_out is not None:
                self.patch_jump(jump_out)
            return pos

        # -- Block: { stmts }
        if t.val == '{':
            return self.compile_block(tokens, pos)

        # -- Expression statement (function call, etc.)
        pos = self.compile_expr(tokens, pos)
        self.emit(POP)   # discard unused result
        if tokens[pos].val == ';': pos += 1
        return pos

    def compile_block(self, tokens, pos):
        assert tokens[pos].val == '{', f"expected '{{' got '{tokens[pos].val}'"
        pos += 1
        while tokens[pos].val != '}':
            pos = self.compile_stmt(tokens, pos)
        pos += 1  # skip '}'
        return pos

    # ── Expression compilation (recursive descent) ────────────────────────

    def compile_expr(self, tokens, pos):
        return self._parse_or(tokens, pos)

    def _parse_or(self, tokens, pos):
        pos = self._parse_and(tokens, pos)
        while pos < len(tokens) and tokens[pos].val == '||':
            pos += 1
            # short-circuit: if left is truthy, skip right
            self.emit(DUP)
            jump = self.emit_jump(IFNE)
            self.emit(POP)
            pos = self._parse_and(tokens, pos)
            self.patch_jump(jump)
        return pos

    def _parse_and(self, tokens, pos):
        pos = self._parse_eq(tokens, pos)
        while pos < len(tokens) and tokens[pos].val == '&&':
            pos += 1
            self.emit(DUP)
            jump = self.emit_jump(IFEQ)
            self.emit(POP)
            pos = self._parse_eq(tokens, pos)
            self.patch_jump(jump)
        return pos

    def _parse_eq(self, tokens, pos):
        pos = self._parse_cmp(tokens, pos)
        while pos < len(tokens) and tokens[pos].val in ('==','!='):
            op = tokens[pos].val; pos += 1
            pos = self._parse_cmp(tokens, pos)
            if op == '==':
                # Emit: push 0 if equal (subtraction trick)
                self.emit(ISUB)
                jump_t = self.emit_jump(IFEQ)
                self.emit_int(0); jump_e = self.emit_jump(GOTO)
                self.patch_jump(jump_t); self.emit_int(1)
                self.patch_jump(jump_e)
            else:
                self.emit(ISUB)
                jump_f = self.emit_jump(IFNE)
                self.emit_int(0); jump_e = self.emit_jump(GOTO)
                self.patch_jump(jump_f); self.emit_int(1)
                self.patch_jump(jump_e)
        return pos

    def _parse_cmp(self, tokens, pos):
        pos = self._parse_bitor(tokens, pos)
        while pos < len(tokens) and tokens[pos].val in ('<','<=','>','>='):
            op = tokens[pos].val; pos += 1
            pos = self._parse_bitor(tokens, pos)
            op_map = {'<': IF_ICMPGE, '<=': IF_ICMPGT,
                      '>': IF_ICMPLE, '>=': IF_ICMPLT}
            jump_f = self.emit_jump(op_map[op])
            self.emit_int(1); jump_e = self.emit_jump(GOTO)
            self.patch_jump(jump_f); self.emit_int(0)
            self.patch_jump(jump_e)
        return pos

    def _parse_bitor(self, tokens, pos):
        pos = self._parse_bitxor(tokens, pos)
        while pos < len(tokens) and tokens[pos].val == '|' and \
              tokens[pos+1].val != '|':
            pos += 1
            pos = self._parse_bitxor(tokens, pos)
            self.emit(IOR)
        return pos

    def _parse_bitxor(self, tokens, pos):
        pos = self._parse_bitand(tokens, pos)
        while pos < len(tokens) and tokens[pos].val == '^':
            pos += 1
            pos = self._parse_bitand(tokens, pos)
            self.emit(IXOR)
        return pos

    def _parse_bitand(self, tokens, pos):
        pos = self._parse_shift(tokens, pos)
        while pos < len(tokens) and tokens[pos].val == '&' and \
              tokens[pos+1].val != '&':
            pos += 1
            pos = self._parse_shift(tokens, pos)
            self.emit(IAND)
        return pos

    def _parse_shift(self, tokens, pos):
        pos = self._parse_add(tokens, pos)
        while pos < len(tokens) and tokens[pos].val in ('<<','>>'):
            op = tokens[pos].val; pos += 1
            pos = self._parse_add(tokens, pos)
            self.emit(ISHL if op == '<<' else ISHR)
        return pos

    def _parse_add(self, tokens, pos):
        pos = self._parse_mul(tokens, pos)
        while pos < len(tokens) and tokens[pos].val in ('+','-'):
            op = tokens[pos].val; pos += 1
            pos = self._parse_mul(tokens, pos)
            self.emit(IADD if op == '+' else ISUB)
        return pos

    def _parse_mul(self, tokens, pos):
        pos = self._parse_unary(tokens, pos)
        while pos < len(tokens) and tokens[pos].val in ('*','/','%'):
            op = tokens[pos].val; pos += 1
            pos = self._parse_unary(tokens, pos)
            ops = {'*':IMUL, '/':IDIV, '%':IREM}
            self.emit(ops[op])
        return pos

    def _parse_unary(self, tokens, pos):
        t = tokens[pos]
        if t.val == '-':
            pos += 1
            pos = self._parse_unary(tokens, pos)
            self.emit(INEG)
        elif t.val == '!':
            pos += 1
            pos = self._parse_unary(tokens, pos)
            # !x = (x == 0) ? 1 : 0
            jump_t = self.emit_jump(IFEQ)
            self.emit_int(0); jump_e = self.emit_jump(GOTO)
            self.patch_jump(jump_t); self.emit_int(1)
            self.patch_jump(jump_e)
        elif t.val == '~':
            pos += 1
            pos = self._parse_unary(tokens, pos)
            # ~x = x ^ -1
            self.emit_int(-1)
            self.emit(IXOR)
        else:
            pos = self._parse_primary(tokens, pos)
        return pos

    def _parse_primary(self, tokens, pos):
        t = tokens[pos]

        # Integer literal
        if t.kind == 'NUM':
            v = int(t.val, 16 if t.val.startswith('0x') else 10)
            self.emit_int(v)
            return pos + 1

        # Boolean literals
        if t.kind == 'TRUE':
            self.emit_int(1); return pos + 1
        if t.kind == 'FALSE':
            self.emit_int(0); return pos + 1
        if t.kind == 'NULL':
            self.emit(ACONST_NULL); return pos + 1

        # String literal
        if t.kind == 'STRING':
            s = t.val[1:-1]   # strip quotes; handle basic escapes
            s = s.replace('\\n','\n').replace('\\t','\t').replace('\\"','"').replace('\\\\','\\')
            self.emit_aldc(s)
            return pos + 1

        # Char literal
        if t.kind == 'CHAR':
            ch = t.val[1:-1]
            if ch.startswith('\\'):
                esc = {'\\n':'\n','\\t':'\t',"\\'":"'",'\\\\':'\\'}
                ch = esc.get(ch, ch[1])
            self.emit_int(ord(ch[0]))
            return pos + 1

        # Grouped expression
        if t.val == '(':
            pos += 1
            pos = self.compile_expr(tokens, pos)
            assert tokens[pos].val == ')'; pos += 1
            return pos

        # Identifier: variable load or function call
        if t.kind == 'ID':
            fname = t.val
            pos += 1

            # Function call?
            if pos < len(tokens) and tokens[pos].val == '(':
                pos += 1
                # Collect arguments
                args_count = 0
                while tokens[pos].val != ')':
                    pos = self.compile_expr(tokens, pos)
                    args_count += 1
                    if tokens[pos].val == ',': pos += 1
                assert tokens[pos].val == ')'; pos += 1

                # Native function?
                if fname in NATIVE:
                    ni = self._get_or_add_native(fname)
                    self.emit(INVOKENATIVE, (ni >> 8) & 0xFF, ni & 0xFF)
                # User-defined function?
                elif fname in self.all_fns:
                    fi = self.all_fns[fname]
                    self.emit(INVOKESTATIC, (fi >> 8) & 0xFF, fi & 0xFF)
                else:
                    raise CompileError(f"unknown function '{fname}'")
                return pos

            # Variable load
            slot = self._local(fname)
            self.emit_vload(slot)
            return pos

        raise CompileError(f"unexpected token '{t.val}' (kind={t.kind}) at line {t.line}")


# ── Top-level compiler ────────────────────────────────────────────────────────

class Compiler:
    def __init__(self):
        self.functions   = []          # list of FunctionCompiler
        self.fn_index    = {}          # name → index in functions[]
        self.string_pool : List[str]   = []
        self.int_pool    : List[int]   = []
        self.native_pool : List[Tuple] = []

    def compile(self, src: str) -> bytes:
        tokens = tokenise(src)
        tokens.append(Token('EOF', '', 0))

        # ── First pass: collect function signatures ────────────────────
        pos = 0
        signatures = []   # (ret_type, name, params)
        while tokens[pos].kind != 'EOF':
            t = tokens[pos]
            # Skip typedefs/structs/includes for now
            if t.kind == 'TYPEDEF':
                while tokens[pos].val != ';': pos += 1
                pos += 1; continue

            # Function: <type> name ( params ) { body }
            if t.kind in ('INT','BOOL','STRING','CHAR','VOID') or t.kind == 'ID':
                ret_type = t.val; pos += 1
                if tokens[pos].kind != 'ID': pos += 1; continue
                fname = tokens[pos].val; pos += 1
                if tokens[pos].val != '(':
                    # global variable or something else – skip to ;
                    while tokens[pos].val != ';' and tokens[pos].kind != 'EOF':
                        pos += 1
                    pos += 1; continue
                pos += 1  # skip (
                params = []
                while tokens[pos].val != ')':
                    ptype = tokens[pos].val; pos += 1
                    if tokens[pos].val == ')': break
                    pname = tokens[pos].val; pos += 1
                    params.append((ptype, pname))
                    if tokens[pos].val == ',': pos += 1
                pos += 1  # skip )
                # Skip body
                if tokens[pos].val == '{':
                    depth = 1; pos += 1
                    while depth > 0:
                        if tokens[pos].val == '{': depth += 1
                        if tokens[pos].val == '}': depth -= 1
                        pos += 1
                elif tokens[pos].val == ';':
                    pos += 1  # forward declaration
                    continue
                signatures.append((ret_type, fname, params))
                # C0VM expects main() at index 0 — we'll reorder below
            else:
                pos += 1

        # Assign indices: put 'main' first
        ordered = sorted(signatures, key=lambda x: 0 if x[1] == 'main' else 1)
        for i, (_, fname, _) in enumerate(ordered):
            self.fn_index[fname] = i

        # ── Second pass: compile bodies ────────────────────────────────
        pos = 0
        fn_compilers = {}  # name → FunctionCompiler

        while tokens[pos].kind != 'EOF':
            t = tokens[pos]
            if t.kind in ('INT','BOOL','STRING','CHAR','VOID') or t.kind == 'ID':
                ret_type = t.val; pos += 1
                if tokens[pos].kind != 'ID': pos += 1; continue
                fname = tokens[pos].val; pos += 1
                if tokens[pos].val != '(':
                    while tokens[pos].val != ';' and tokens[pos].kind != 'EOF':
                        pos += 1
                    pos += 1; continue
                pos += 1
                params = []
                while tokens[pos].val != ')':
                    ptype = tokens[pos].val; pos += 1
                    if tokens[pos].val == ')': break
                    pname = tokens[pos].val; pos += 1
                    params.append((ptype, pname))
                    if tokens[pos].val == ',': pos += 1
                pos += 1  # skip )
                if tokens[pos].val == ';':
                    pos += 1; continue   # forward declaration
                # Compile body
                fc = FunctionCompiler(fname, params, self.fn_index,
                                      self.string_pool, self.int_pool,
                                      self.native_pool)
                pos = fc.compile_block(tokens, pos)
                # Ensure every function ends with a return
                if not fc.code or fc.code[-1] != RETURN:
                    fc.emit_int(0)
                    fc.emit(RETURN)
                fn_compilers[fname] = fc
            else:
                pos += 1

        # ── Assemble bc0 ────────────────────────────────────────────────
        # Reorder function compilers
        ordered_fc = [fn_compilers[fname] for (_, fname, _) in ordered
                      if fname in fn_compilers]

        # Build native pool list (deduplicated via FunctionCompiler)
        # Each FunctionCompiler shares the same native_pool list reference.

        return self._assemble(ordered_fc)

    def _assemble(self, fcs: List[FunctionCompiler]) -> bytes:
        out = b''
        out += struct.pack('>I', 0xC0C0FFEE)  # magic
        out += struct.pack('>H', 0)            # version

        # Integer pool
        out += struct.pack('>H', len(self.int_pool))
        for v in self.int_pool:
            out += struct.pack('>i', v)

        # String pool: concatenate all null-terminated strings
        sp_bytes = ''.join(self.string_pool).encode('utf-8')
        out += struct.pack('>H', len(sp_bytes))
        out += sp_bytes

        # Function pool
        out += struct.pack('>H', len(fcs))
        for fc in fcs:
            code = bytes(fc.code)
            out += struct.pack('>H', len([s for s in fcs[0].locals if False]))  # dummy
            # Recompute: num_args from params
            # We stored params as local slots 0..num_args-1
            # num_args = fc.num_vars - (non-param locals) — tricky
            # Easier: trust fc.num_vars and set num_args to min(num_vars, params_len)
            # Actually FunctionCompiler tracks this correctly via _declare:
            # param slots are declared first in __init__
            # We need to pass num_args separately. Let's use a workaround:
            # Store num_vars as num_args for now (safe: args ⊆ vars)
            # TODO: track num_args separately
            num_args = getattr(fc, '_num_params', 0)
            num_vars = max(fc.num_vars, 1)
            out += struct.pack('>HHH', num_args, num_vars, len(code))
            out += code

        # Native pool
        out += struct.pack('>H', len(self.native_pool))
        for (num_args, fti) in self.native_pool:
            out += struct.pack('>HH', num_args, fti)

        return out


# ── Revised Compiler with proper param tracking ───────────────────────────────

class Compiler2:
    """Cleaner compiler with proper num_args tracking."""

    def __init__(self):
        self.string_pool : List[str]         = []
        self.int_pool    : List[int]         = []
        self.native_pool : List[Tuple]       = []
        self.fn_index    : Dict[str,int]     = {}
        self.fn_params   : Dict[str,int]     = {}   # fname → num_args
        self.fn_compiled : Dict[str, 'FunctionCompiler'] = {}

    def compile(self, src: str) -> bytes:
        tokens = tokenise(src)
        tokens.append(Token('EOF', '', 0))

        # Pass 1: scan function signatures and assign indices
        sigs = []   # (fname, params_list)
        pos  = 0
        while tokens[pos].kind != 'EOF':
            t = tokens[pos]
            if t.kind in ('INT','BOOL','STRING','CHAR','VOID') or \
               (t.kind == 'ID' and pos+1 < len(tokens) and tokens[pos+1].kind == 'ID'):
                pos += 1  # skip ret type
                if tokens[pos].kind != 'ID':
                    continue
                fname = tokens[pos].val; pos += 1
                if tokens[pos].val != '(':
                    # skip to ;
                    while tokens[pos].val not in (';','}') and tokens[pos].kind != 'EOF':
                        pos += 1
                    if tokens[pos].val == ';': pos += 1
                    continue
                pos += 1  # skip (
                params = []
                while tokens[pos].val != ')':
                    pos += 1  # skip type
                    if tokens[pos].val in (')',','): 
                        if tokens[pos].val == ')': break
                        pos += 1; continue
                    pname = tokens[pos].val; pos += 1
                    params.append(pname)
                    if tokens[pos].val == ',': pos += 1
                assert tokens[pos].val == ')'; pos += 1
                # Skip body or semicolon
                if tokens[pos].val == '{':
                    depth = 1; pos += 1
                    while depth > 0:
                        if tokens[pos].val == '{': depth += 1
                        if tokens[pos].val == '}': depth -= 1
                        pos += 1
                elif tokens[pos].val == ';':
                    pos += 1
                if fname not in [s[0] for s in sigs]:
                    sigs.append((fname, params))
            else:
                pos += 1

        # Assign indices: main first
        ordered = sorted(sigs, key=lambda x: 0 if x[0] == 'main' else 1)
        for i, (fname, params) in enumerate(ordered):
            self.fn_index[fname]  = i
            self.fn_params[fname] = len(params)

        # Pass 2: compile bodies
        pos = 0
        while tokens[pos].kind != 'EOF':
            t = tokens[pos]
            if t.kind in ('INT','BOOL','STRING','CHAR','VOID') or \
               (t.kind == 'ID' and pos+1 < len(tokens) and tokens[pos+1].kind == 'ID'):
                pos += 1  # skip ret type
                if tokens[pos].kind != 'ID': continue
                fname = tokens[pos].val; pos += 1
                if tokens[pos].val != '(':
                    while tokens[pos].val not in (';','}') and tokens[pos].kind != 'EOF':
                        pos += 1
                    if tokens[pos].val == ';': pos += 1
                    continue
                pos += 1
                params = []
                while tokens[pos].val != ')':
                    ptype = tokens[pos].val; pos += 1
                    if tokens[pos].val in (')',','): 
                        if tokens[pos].val == ')': break
                        pos += 1; continue
                    pname = tokens[pos].val
                    params.append((ptype, pname))
                    pos += 1
                    if tokens[pos].val == ',': pos += 1
                assert tokens[pos].val == ')'; pos += 1

                if tokens[pos].val == ';':
                    pos += 1; continue

                fc = FunctionCompiler(fname, params, self.fn_index,
                                      self.string_pool, self.int_pool,
                                      self.native_pool)
                fc._num_params = len(params)
                pos = fc.compile_block(tokens, pos)
                if not fc.code or fc.code[-1] != RETURN:
                    fc.emit_int(0); fc.emit(RETURN)
                self.fn_compiled[fname] = fc
            else:
                pos += 1

        # Assemble in order
        ordered_fc = [(fname, self.fn_compiled[fname])
                      for (fname, _) in ordered
                      if fname in self.fn_compiled]
        return self._assemble(ordered_fc)

    def _assemble(self, ordered_fc):
        out = b''
        out += struct.pack('>I', 0xC0C0FFEE)
        out += struct.pack('>H', 0)

        out += struct.pack('>H', len(self.int_pool))
        for v in self.int_pool:
            out += struct.pack('>i', v)

        sp_bytes = ''.join(self.string_pool).encode('utf-8')
        out += struct.pack('>H', len(sp_bytes))
        out += sp_bytes

        out += struct.pack('>H', len(ordered_fc))
        for (fname, fc) in ordered_fc:
            code     = bytes(fc.code)
            num_args = getattr(fc, '_num_params', 0)
            num_vars = max(fc.num_vars, num_args, 1)
            out += struct.pack('>HHH', num_args, num_vars, len(code))
            out += code

        out += struct.pack('>H', len(self.native_pool))
        for (num_args, fti) in self.native_pool:
            out += struct.pack('>HH', num_args, fti)

        return out


# ── CLI ───────────────────────────────────────────────────────────────────────

def main():
    args = sys.argv[1:]
    if not args:
        print(__doc__)
        sys.exit(1)

    src_path = args[0]
    out_path = src_path.replace('.c0', '.bc0')
    if '-o' in args:
        out_path = args[args.index('-o') + 1]

    with open(src_path) as f:
        src = f.read()

    try:
        c = Compiler2()
        bc0 = c.compile(src)
    except CompileError as e:
        print(f"Compile error: {e}", file=sys.stderr)
        sys.exit(1)

    with open(out_path, 'wb') as f:
        f.write(bc0)

    print(f"Compiled {src_path} -> {out_path}  ({len(bc0)} bytes)")


if __name__ == '__main__':
    main()

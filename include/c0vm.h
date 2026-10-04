/*
 * C0VM - C0 Virtual Machine
 * C0 Virtual Machine Implementation
 *
 * c0vm.h - Core type definitions for the C0 bytecode interpreter.
 *
 * The C0VM is a stack-based virtual machine (inspired by the JVM) that
 * executes compiled C0 bytecode (.bc0 files).  Each function call gets
 * its own frame with an operand stack, local-variable array, a pointer
 * to the bytecode, and a program counter.
 *
 * BC0 file layout (big-endian, binary):
 *   magic          : uint32   (0xC0C0FFEE)
 *   version        : uint16   (must be 0 or 1)
 *   int_count      : uint16   – number of entries in int_pool
 *   int_pool       : int32[]  – integer literal constants
 *   string_count   : uint16   – byte-length of the string pool
 *   string_pool    : char[]   – concatenated null-terminated strings
 *   function_count : uint16   – number of functions
 *   function_pool  : struct_function_info[]
 *   native_count   : uint16   – number of native (C) functions
 *   native_pool    : struct_native_info[]
 */

#ifndef C0VM_H
#define C0VM_H

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>

#define MAX_CALL_DEPTH  1024
#define MAX_TRACE_STEPS 10000

/* -----------------------------------------------------------------------
 * Forward declarations
 * --------------------------------------------------------------------- */
typedef struct bc0_file    bc0_file;
typedef struct function_info function_info;
typedef struct native_info   native_info;
typedef struct c0_value      c0_value;
typedef struct frame         frame;

/* -----------------------------------------------------------------------
 * BC0 file structures
 * --------------------------------------------------------------------- */

/* Per-function metadata stored in the .bc0 file */
struct function_info {
    uint16_t  num_args;    /* number of formal parameters              */
    uint16_t  num_vars;    /* total local variable slots (args + rest) */
    uint16_t  code_length; /* number of bytecode bytes                 */
    uint8_t  *code;        /* heap-allocated bytecode array            */
};

/* Per-native-function metadata stored in the .bc0 file */
struct native_info {
    uint16_t num_args;            /* number of arguments                */
    uint16_t function_table_index;/* index into the native function table*/
};

/* Complete parsed .bc0 file */
struct bc0_file {
    uint32_t       magic;

    uint16_t       int_count;
    int32_t       *int_pool;   /* heap-allocated */

    uint16_t       string_count; /* byte length of string_pool */
    char          *string_pool;  /* heap-allocated */

    uint16_t       function_count;
    function_info *function_pool; /* heap-allocated */

    uint16_t       native_count;
    native_info   *native_pool;   /* heap-allocated */
};

/* -----------------------------------------------------------------------
 * c0_value – tagged union used on the operand stack and in locals
 * --------------------------------------------------------------------- */
typedef enum {
    C0_INTEGER,   /* 32-bit signed integer  */
    C0_POINTER    /* generic heap pointer   */
} c0_val_kind;

struct c0_value {
    c0_val_kind kind;
    union {
        int32_t  i;   /* C0_INTEGER */
        void    *p;   /* C0_POINTER */
    } payload;
};

/* Convenience constructors */
static inline c0_value c0_int(int32_t i) {
    c0_value v; v.kind = C0_INTEGER; v.payload.i = i; return v;
}
static inline c0_value c0_ptr(void *p) {
    c0_value v; v.kind = C0_POINTER; v.payload.p = p; return v;
}

/* -----------------------------------------------------------------------
 * Execution frame – one per active function call
 * --------------------------------------------------------------------- */
struct frame {
    uint16_t  fn_id;    /* function_pool index */
    /* Bytecode and PC */
    uint8_t  *P;   /* bytecode array (points into function_pool entry) */
    uint16_t  pc;  /* program counter                                   */

    /* Local variables (includes arguments at low indices) */
    c0_value *V;   /* heap-allocated array of num_vars entries          */
    uint16_t  num_vars;

    /* Operand stack (fixed max size = code_length, safe upper bound) */
    c0_value *S;   /* heap-allocated array                              */
    int32_t   sp;  /* stack pointer: S[sp-1] is top (sp==0 => empty)   */
    int32_t   s_size; /* allocated capacity of S                        */
};

/* -----------------------------------------------------------------------
 * C0 heap array representation
 *   Arrays live on the C heap.  We store the element count and element
 *   size so we can bounds-check at runtime.
 * --------------------------------------------------------------------- */
typedef struct {
    int32_t  count;    /* number of elements           */
    int32_t  elt_size; /* sizeof each element in bytes */
    void    *data;     /* heap-allocated element store */
} c0_array;

/* -----------------------------------------------------------------------
 * Native-function table entry
 *   Each native function accepts an array of c0_value arguments and
 *   returns a single c0_value.
 * --------------------------------------------------------------------- */
typedef c0_value (*c0_native_fn)(c0_value *args);

/* -----------------------------------------------------------------------
 * Public API
 * --------------------------------------------------------------------- */

/* Parse a .bc0 file from disk; returns heap-allocated bc0_file or NULL */
bc0_file *read_bc0_file(const char *path);

/* Free all memory owned by a bc0_file */
void free_bc0_file(bc0_file *bcf);

/* Execute the bytecode; returns the integer result of _c0_main() */
int execute(bc0_file *bcf);

/* Execute with JSON execution tracing for the visualizer */
int execute_with_trace(bc0_file *bcf, const char *trace_path);

/* Disassemble all functions in a bc0_file to stdout */
void disassemble(bc0_file *bcf);

/* Decode a single instruction at code[pc]; sets *next_pc and fills mnemonic_buf */
void format_instruction(const uint8_t *code, uint16_t pc, uint16_t *next_pc,
                        char *mnemonic_buf, size_t buf_size);

/* -----------------------------------------------------------------------
 * Opcode constants  (mirror c0vm-ref.txt)
 * --------------------------------------------------------------------- */

#define NOP   0x00

/* Arithmetic */
#define IADD  0x60
#define ISUB  0x64
#define IMUL  0x68
#define IDIV  0x6C
#define IREM  0x70
#define INEG  0x74
#define ISHL  0x78
#define ISHR  0x7A   /* arithmetic (signed) right shift */
#define IUSHR 0x7C   /* logical (unsigned) right shift  */
#define IAND  0x7E
#define IOR   0x80
#define IXOR  0x82

/* Constants / local variables */
#define BIPUSH 0x10
#define ILDC   0x13  /* push int from int_pool[c1<<8|c2] */
#define ALDC   0x14  /* push ptr into string_pool[c1<<8|c2] */
#define VLOAD  0x15  /* push V[i] */
#define VSTORE 0x36  /* V[i] = pop */
#define ACONST_NULL 0x01

/* Stack manipulation */
#define POP  0x57
#define POP2 0x58   /* (unused in standard tests, included for completeness) */
#define DUP  0x59
#define SWAP 0x5F

/* Control flow */
#define IFEQ 0x99   /* branch if top == 0 */
#define IFNE 0x9A   /* branch if top != 0 */
#define IFLT 0x9B   /* branch if top <  0 */
#define IFGE 0x9C   /* branch if top >= 0 */
#define IFGT 0x9D   /* branch if top >  0 */
#define IFLE 0x9E   /* branch if top <= 0 */
#define IF_ICMPEQ 0x9F  /* branch if top two ints == */
#define IF_ICMPNE 0xA0
#define IF_ICMPLT 0xA1
#define IF_ICMPGE 0xA2
#define IF_ICMPGT 0xA3
#define IF_ICMPLE 0xA4
#define GOTO 0xA7

/* Function calls & return */
#define INVOKESTATIC 0xB8
#define INVOKENATIVE 0xB7
#define RETURN       0xB0
#define ATHROW       0xBF   /* raise runtime error */

/* Memory allocation */
#define NEW      0xBB  /* allocate struct of <s> bytes */
#define NEWARRAY 0xBC  /* allocate array of <s> elt_size */
#define ARRAYLENGTH 0xBE

/* Field / array access */
#define GETFIELD 0xB4  /* ptr, field_offset -> value  */
#define PUTFIELD 0xB5  /* ptr, value, field_offset -> */
#define AADDF    0x62  /* struct field address: ptr + offset -> ptr */
#define AADDS    0x63  /* array element address: ptr + index -> ptr */

/* Pointer comparisons */
#define ACMPEQ   0xA5
#define ACMPNE   0xA6

/* Indirect load/store through pointers */
#define IMLOAD  0x2E   /* *ptr (int)    -> push */
#define IMSTORE 0x4F   /* pop int -> *ptr       */
#define AMLOAD  0x2F   /* *ptr (ptr)    -> push */
#define AMSTORE 0x50   /* pop ptr -> *ptr       */

/* Type tag check (instanceof-like, not needed for basic interp) */
#define CHECKTAG 0xC0
#define HASTAG   0xC1

#endif /* C0VM_H */

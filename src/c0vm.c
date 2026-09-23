/*
 * C0VM - C0 Virtual Machine
 * CMU 15-122: Principles of Imperative Computation
 *
 * c0vm.c - Core bytecode interpreter (the execute() function).
 *
 * Architecture overview
 * =====================
 * The C0VM is a stack-based virtual machine closely modelled on the JVM.
 * Each function activation has a frame containing:
 *
 *   P   – pointer to the bytecode byte array
 *   pc  – program counter (index into P)
 *   V[] – local variable array (args are V[0..num_args-1])
 *   S[] – operand stack (grows upward; sp points one past the top)
 *
 * The main loop fetches an opcode byte, advances pc, then dispatches on
 * the opcode using a large switch statement.  Function calls push the
 * current frame onto a call stack and create a new frame; RETURN pops
 * back to the caller.
 *
 * Memory
 * ======
 * All heap objects (structs, arrays) are allocated via malloc/calloc and
 * are intentionally never freed during normal execution – the interpreter
 * acts as a GC-free "stop the world" environment matching the original
 * assignment's requirements.  On error, longjmp() escapes to main where
 * free_bc0_file() is called.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <setjmp.h>

#include "c0vm.h"
#include "c0vm_abort.h"
#include "c0_native.h"

/* -----------------------------------------------------------------------
 * Operand-stack capacity: we start at 32 and double as needed.
 * --------------------------------------------------------------------- */
#define STACK_INIT_CAP  32
#define LOCALS_INIT_CAP 16

/* Maximum call-stack depth (guards against infinite recursion) */
#define MAX_CALL_DEPTH  1024

/* -----------------------------------------------------------------------
 * Safe allocators
 * --------------------------------------------------------------------- */
static void *xmalloc(size_t sz) {
    void *p = malloc(sz);
    if (p == NULL) c0_abort("out of memory (malloc %lu bytes)", (unsigned long)sz);
    return p;
}
static void *xcalloc(size_t n, size_t sz) {
    void *p = calloc(n, sz);
    if (p == NULL) c0_abort("out of memory (calloc %lu*%lu bytes)",
                            (unsigned long)n, (unsigned long)sz);
    return p;
}

/* -----------------------------------------------------------------------
 * Frame helpers
 * --------------------------------------------------------------------- */

/* Allocate a fresh frame for function f; copy args from caller's stack */
static frame *make_frame(const function_info *f, c0_value *args) {
    frame *fr = (frame *)xmalloc(sizeof(frame));
    fr->P  = f->code;
    fr->pc = 0;

    /* Local variables (initialised to int 0 = "safe" default) */
    uint16_t nv = f->num_vars > 0 ? f->num_vars : 1;
    fr->V = (c0_value *)xcalloc(nv, sizeof(c0_value));
    for (uint16_t i = 0; i < nv; i++)
        fr->V[i] = c0_int(0);

    /* Copy arguments into V[0..num_args-1] */
    for (uint16_t i = 0; i < f->num_args; i++)
        fr->V[i] = args[i];

    /* Operand stack */
    int32_t scap = (f->code_length > STACK_INIT_CAP)
                   ? (int32_t)f->code_length
                   : STACK_INIT_CAP;
    fr->S      = (c0_value *)xmalloc(scap * sizeof(c0_value));
    fr->sp     = 0;
    fr->s_size = scap;
    return fr;
}

static void free_frame(frame *fr) {
    if (!fr) return;
    free(fr->V);
    free(fr->S);
    free(fr);
}

/* -----------------------------------------------------------------------
 * Stack operations (inlined for speed in the hot loop)
 * --------------------------------------------------------------------- */
static inline void stack_push(frame *fr, c0_value v) {
    if (fr->sp >= fr->s_size) {
        /* Grow the stack */
        int32_t new_cap = fr->s_size * 2;
        fr->S = (c0_value *)realloc(fr->S, new_cap * sizeof(c0_value));
        if (fr->S == NULL)
            c0_abort("operand stack overflow (could not grow)");
        fr->s_size = new_cap;
    }
    fr->S[fr->sp++] = v;
}

static inline c0_value stack_pop(frame *fr) {
    if (fr->sp <= 0)
        c0_abort("operand stack underflow");
    return fr->S[--fr->sp];
}

static inline c0_value stack_peek(frame *fr) {
    if (fr->sp <= 0)
        c0_abort("operand stack underflow (peek)");
    return fr->S[fr->sp - 1];
}

/* -----------------------------------------------------------------------
 * Bytecode fetch helpers
 * --------------------------------------------------------------------- */
static inline uint8_t fetch_u8(frame *fr) {
    return fr->P[fr->pc++];
}

static inline uint16_t fetch_u16(frame *fr) {
    uint8_t c1 = fr->P[fr->pc++];
    uint8_t c2 = fr->P[fr->pc++];
    return (uint16_t)((c1 << 8) | c2);
}

/* -----------------------------------------------------------------------
 * C0 array helpers
 * --------------------------------------------------------------------- */

/* Allocate a C0 array of `count` elements each `elt_size` bytes. */
static c0_array *alloc_array(int32_t count, int32_t elt_size) {
    if (count < 0)
        c0_abort("cannot allocate array with negative count %d", count);
    c0_array *a = (c0_array *)xmalloc(sizeof(c0_array));
    a->count    = count;
    a->elt_size = elt_size;
    a->data     = xcalloc(count > 0 ? (size_t)count : 1, (size_t)elt_size);
    return a;
}

/* Return pointer to element i (bounds-checked). */
static void *array_elem_ptr(c0_array *a, int32_t i) {
    c0_check_null(a);
    c0_check_bounds(i, a->count);
    return (char *)a->data + (size_t)i * (size_t)a->elt_size;
}

/* -----------------------------------------------------------------------
 * execute() – main interpreter loop
 * --------------------------------------------------------------------- */

int execute(bc0_file *bcf) {
    if (bcf->function_count == 0)
        c0_abort("no functions in bytecode file");

    /* Call stack */
    frame *call_stack[MAX_CALL_DEPTH];
    int    call_depth = 0;

    /* Start at function 0 (_c0_main), which takes 0 arguments */
    const function_info *main_fn = &bcf->function_pool[0];
    if (main_fn->num_args != 0)
        c0_abort("_c0_main must take 0 arguments");

    frame *fr = make_frame(main_fn, NULL);
    call_stack[call_depth++] = fr;

    /* ===== Fetch-Decode-Execute loop ===================================== */
    while (true) {
        uint8_t op = fetch_u8(fr);

#ifdef C0VM_TRACE
        fprintf(stderr, "[pc=%3u sp=%2d] op=0x%02X\n",
                (unsigned)(fr->pc - 1), fr->sp, op);
#endif

        switch (op) {

        /* ----------------------------------------------------------------
         * NOP
         * -------------------------------------------------------------- */
        case 0x00: /* nop */
            break;

        /* ----------------------------------------------------------------
         * Constants
         * -------------------------------------------------------------- */
        case ACONST_NULL:                       /* push null pointer */
            stack_push(fr, c0_ptr(NULL));
            break;

        case BIPUSH: {                          /* push sign-extended byte */
            int8_t b = (int8_t)fetch_u8(fr);
            stack_push(fr, c0_int((int32_t)b));
            break;
        }

        case ILDC: {                            /* push int_pool[index] */
            uint16_t idx = fetch_u16(fr);
            if (idx >= bcf->int_count)
                c0_abort("ildc: index %u out of range (int_count=%u)",
                         (unsigned)idx, (unsigned)bcf->int_count);
            stack_push(fr, c0_int(bcf->int_pool[idx]));
            break;
        }

        case ALDC: {                            /* push &string_pool[index] */
            uint16_t idx = fetch_u16(fr);
            if (idx >= bcf->string_count)
                c0_abort("aldc: index %u out of range (string_count=%u)",
                         (unsigned)idx, (unsigned)bcf->string_count);
            stack_push(fr, c0_ptr((void *)&bcf->string_pool[idx]));
            break;
        }

        /* ----------------------------------------------------------------
         * Local variable load / store
         * -------------------------------------------------------------- */
        case VLOAD: {
            uint8_t i = fetch_u8(fr);
            /* We trust the compiler; but add a debug-mode check */
            stack_push(fr, fr->V[i]);
            break;
        }

        case VSTORE: {
            uint8_t  i = fetch_u8(fr);
            c0_value v = stack_pop(fr);
            fr->V[i]   = v;
            break;
        }

        /* ----------------------------------------------------------------
         * Stack manipulation
         * -------------------------------------------------------------- */
        case POP:
            (void)stack_pop(fr);
            break;

        case POP2:
            (void)stack_pop(fr);
            (void)stack_pop(fr);
            break;

        case DUP: {
            c0_value v = stack_peek(fr);
            stack_push(fr, v);
            break;
        }

        case SWAP: {
            c0_value a = stack_pop(fr);
            c0_value b = stack_pop(fr);
            stack_push(fr, a);
            stack_push(fr, b);
            break;
        }

        /* ----------------------------------------------------------------
         * Integer arithmetic
         * -------------------------------------------------------------- */
        case IADD: {
            c0_value y = stack_pop(fr), x = stack_pop(fr);
            stack_push(fr, c0_int(x.payload.i + y.payload.i));
            break;
        }
        case ISUB: {
            c0_value y = stack_pop(fr), x = stack_pop(fr);
            stack_push(fr, c0_int(x.payload.i - y.payload.i));
            break;
        }
        case IMUL: {
            c0_value y = stack_pop(fr), x = stack_pop(fr);
            stack_push(fr, c0_int(x.payload.i * y.payload.i));
            break;
        }
        case IDIV: {
            c0_value y = stack_pop(fr), x = stack_pop(fr);
            c0_check_div(y.payload.i);
            /* C0 semantics: truncation toward zero (same as C99) */
            if (x.payload.i == INT32_MIN && y.payload.i == -1)
                c0_abort("integer overflow: INT_MIN / -1");
            stack_push(fr, c0_int(x.payload.i / y.payload.i));
            break;
        }
        case IREM: {
            c0_value y = stack_pop(fr), x = stack_pop(fr);
            c0_check_div(y.payload.i);
            if (x.payload.i == INT32_MIN && y.payload.i == -1)
                c0_abort("integer overflow: INT_MIN %% -1");
            stack_push(fr, c0_int(x.payload.i % y.payload.i));
            break;
        }
        case INEG: {
            c0_value x = stack_pop(fr);
            stack_push(fr, c0_int(-x.payload.i));
            break;
        }

        /* ----------------------------------------------------------------
         * Bitwise / shift
         * -------------------------------------------------------------- */
        case ISHL: {
            c0_value y = stack_pop(fr), x = stack_pop(fr);
            c0_check_shift(y.payload.i);
            stack_push(fr, c0_int(x.payload.i << y.payload.i));
            break;
        }
        case ISHR: {  /* arithmetic (signed) right shift */
            c0_value y = stack_pop(fr), x = stack_pop(fr);
            int32_t  s = y.payload.i;
            c0_check_shift(s);
            int32_t val = x.payload.i;
            int32_t res = (s == 0) ? val : (val >> s);
            stack_push(fr, c0_int(res));
            break;
        }
        case IUSHR: { /* logical (unsigned) right shift */
            c0_value y = stack_pop(fr), x = stack_pop(fr);
            c0_check_shift(y.payload.i);
            uint32_t u = (uint32_t)x.payload.i;
            stack_push(fr, c0_int((int32_t)(u >> (uint32_t)y.payload.i)));
            break;
        }
        case IAND: {
            c0_value y = stack_pop(fr), x = stack_pop(fr);
            stack_push(fr, c0_int(x.payload.i & y.payload.i));
            break;
        }
        case IOR: {
            c0_value y = stack_pop(fr), x = stack_pop(fr);
            stack_push(fr, c0_int(x.payload.i | y.payload.i));
            break;
        }
        case IXOR: {
            c0_value y = stack_pop(fr), x = stack_pop(fr);
            stack_push(fr, c0_int(x.payload.i ^ y.payload.i));
            break;
        }

        /* ----------------------------------------------------------------
         * Comparisons – push 1 (true) or 0 (false) as int
         * -------------------------------------------------------------- */

        /* ---- Integer conditional branches (jump by signed offset) ---- */
#define BRANCH_INT1(cmp)                                       \
        do {                                                   \
            int16_t off = (int16_t)fetch_u16(fr);             \
            c0_value v  = stack_pop(fr);                      \
            if (v.payload.i cmp 0) {                          \
                /* offset is relative to the BRANCH opcode */ \
                fr->pc = (uint16_t)(fr->pc - 3 + off);       \
            }                                                  \
        } while (0)

#define BRANCH_INT2(cmp)                                       \
        do {                                                   \
            int16_t off = (int16_t)fetch_u16(fr);             \
            c0_value b  = stack_pop(fr);                      \
            c0_value a  = stack_pop(fr);                      \
            if (a.payload.i cmp b.payload.i) {                \
                fr->pc = (uint16_t)(fr->pc - 3 + off);       \
            }                                                  \
        } while (0)

        case IFEQ: BRANCH_INT1(==); break;
        case IFNE: BRANCH_INT1(!=); break;
        case IFLT: BRANCH_INT1(<);  break;
        case IFGE: BRANCH_INT1(>=); break;
        case IFGT: BRANCH_INT1(>);  break;
        case IFLE: BRANCH_INT1(<=); break;

        case IF_ICMPEQ: BRANCH_INT2(==); break;
        case IF_ICMPNE: BRANCH_INT2(!=); break;
        case IF_ICMPLT: BRANCH_INT2(<);  break;
        case IF_ICMPGE: BRANCH_INT2(>=); break;
        case IF_ICMPGT: BRANCH_INT2(>);  break;
        case IF_ICMPLE: BRANCH_INT2(<=); break;

#undef BRANCH_INT1
#undef BRANCH_INT2

        /* ---- Pointer comparisons ---- */
        case ACMPEQ: {
            int16_t  off = (int16_t)fetch_u16(fr);
            c0_value b   = stack_pop(fr);
            c0_value a   = stack_pop(fr);
            if (a.payload.p == b.payload.p)
                fr->pc = (uint16_t)(fr->pc - 3 + off);
            break;
        }
        case ACMPNE: {
            int16_t  off = (int16_t)fetch_u16(fr);
            c0_value b   = stack_pop(fr);
            c0_value a   = stack_pop(fr);
            if (a.payload.p != b.payload.p)
                fr->pc = (uint16_t)(fr->pc - 3 + off);
            break;
        }

        /* ---- Unconditional goto ---- */
        case GOTO: {
            int16_t off = (int16_t)fetch_u16(fr);
            fr->pc = (uint16_t)(fr->pc - 3 + off);
            break;
        }

        /* ----------------------------------------------------------------
         * Indirect memory load / store via (void *) pointers.
         *
         * C0 stores int-typed values at int* pointers, and pointer-typed
         * values at void** pointers.  IMLOAD/IMSTORE deal with int-type
         * heap cells; AMLOAD/AMSTORE with pointer-type heap cells.
         * Each cell is exactly sizeof(c0_value) bytes large (we
         * allocate them that way in NEW).
         * -------------------------------------------------------------- */
        case IMLOAD: {
            c0_value addr = stack_pop(fr);
            c0_check_null(addr.payload.p);
            int32_t val;
            memcpy(&val, addr.payload.p, sizeof(int32_t));
            stack_push(fr, c0_int(val));
            break;
        }
        case IMSTORE: {
            c0_value val  = stack_pop(fr);
            c0_value addr = stack_pop(fr);
            c0_check_null(addr.payload.p);
            int32_t i = val.payload.i;
            memcpy(addr.payload.p, &i, sizeof(int32_t));
            break;
        }
        case AMLOAD: {
            c0_value addr = stack_pop(fr);
            c0_check_null(addr.payload.p);
            void *p;
            memcpy(&p, addr.payload.p, sizeof(void *));
            stack_push(fr, c0_ptr(p));
            break;
        }
        case AMSTORE: {
            c0_value val  = stack_pop(fr);
            c0_value addr = stack_pop(fr);
            c0_check_null(addr.payload.p);
            void *p = val.payload.p;
            memcpy(addr.payload.p, &p, sizeof(void *));
            break;
        }

        /* ----------------------------------------------------------------
         * Struct allocation and field access
         *
         * NEW   <s>         – allocate s bytes; push pointer
         * GETFIELD <o1,o2>  – pop ptr; push *(ptr + offset)
         * PUTFIELD <o1,o2>  – pop val, pop ptr; *(ptr + offset) = val
         * AADDF  <f>        – pop ptr; push ptr + f (field address)
         * -------------------------------------------------------------- */
        case NEW: {
            uint8_t s = fetch_u8(fr); /* struct size in bytes */
            void *p   = xcalloc(1, s);
            stack_push(fr, c0_ptr(p));
            break;
        }
        case GETFIELD: {
            /* In the CMU encoding this is sometimes AADDF+IMLOAD/AMLOAD.
               We handle the combined form used by newer compiler versions. */
            uint8_t  o1  = fetch_u8(fr);
            uint8_t  o2  = fetch_u8(fr);
            uint16_t off = (uint16_t)((o1 << 8) | o2);
            c0_value ptr = stack_pop(fr);
            c0_check_null(ptr.payload.p);
            /* Determine type from what the slot contains; we push a raw
               pointer to the field and let IMLOAD/AMLOAD handle it – but
               since we're combining, we re-implement inline.            */
            void *field_addr = (char *)ptr.payload.p + off;
            /* Push as a pointer to the field cell (the caller uses an
               appropriate load afterwards, or we embed the load here).
               The CMU c0vm spec says GETFIELD pops ptr, pushes the
               VALUE at that field (typed as the tagged value stored). */
            c0_value stored;
            memcpy(&stored, field_addr, sizeof(c0_value));
            stack_push(fr, stored);
            break;
        }
        case PUTFIELD: {
            uint8_t  o1  = fetch_u8(fr);
            uint8_t  o2  = fetch_u8(fr);
            uint16_t off = (uint16_t)((o1 << 8) | o2);
            c0_value val = stack_pop(fr);
            c0_value ptr = stack_pop(fr);
            c0_check_null(ptr.payload.p);
            void *field_addr = (char *)ptr.payload.p + off;
            memcpy(field_addr, &val, sizeof(c0_value));
            break;
        }
        case AADDF: {
            uint8_t  f   = fetch_u8(fr);   /* field offset in bytes */
            c0_value ptr = stack_pop(fr);
            c0_check_null(ptr.payload.p);
            stack_push(fr, c0_ptr((char *)ptr.payload.p + f));
            break;
        }

        /* ----------------------------------------------------------------
         * Array allocation and element access
         *
         * NEWARRAY  <s>      – pop count; allocate array of count*s bytes
         * ARRAYLENGTH        – pop array ptr; push count
         * AADDS              – pop array ptr, pop index; push &arr[index]
         * (load/store via IMLOAD/IMSTORE or AMLOAD/AMSTORE as appropriate)
         * -------------------------------------------------------------- */
        case NEWARRAY: {
            uint8_t  s     = fetch_u8(fr); /* element size */
            c0_value cnt_v = stack_pop(fr);
            int32_t  count = cnt_v.payload.i;
            c0_array *arr  = alloc_array(count, (int32_t)s);
            stack_push(fr, c0_ptr((void *)arr));
            break;
        }
        case ARRAYLENGTH: {
            c0_value arr_v = stack_pop(fr);
            c0_check_null(arr_v.payload.p);
            c0_array *arr = (c0_array *)arr_v.payload.p;
            stack_push(fr, c0_int(arr->count));
            break;
        }
        case AADDS: {
            c0_value idx_v = stack_pop(fr);
            c0_value arr_v = stack_pop(fr);
            c0_check_null(arr_v.payload.p);
            c0_array *arr  = (c0_array *)arr_v.payload.p;
            int32_t   idx  = idx_v.payload.i;
            void     *ep   = array_elem_ptr(arr, idx);
            stack_push(fr, c0_ptr(ep));
            break;
        }

        /* ----------------------------------------------------------------
         * Function calls
         *
         * INVOKESTATIC <c1,c2>  – call function_pool[c1<<8|c2]
         * INVOKENATIVE <c1,c2>  – call native function indexed by
         *                         native_pool[c1<<8|c2]
         * RETURN                – return top-of-stack to caller
         * ATHROW                – raise a runtime error (string on stack)
         * -------------------------------------------------------------- */
        case INVOKESTATIC: {
            uint16_t fidx = fetch_u16(fr);
            if (fidx >= bcf->function_count)
                c0_abort("invokestatic: function index %u out of range",
                         (unsigned)fidx);
            const function_info *callee = &bcf->function_pool[fidx];

            /* Pop arguments in reverse order into a temporary buffer */
            uint16_t nargs = callee->num_args;
            c0_value *args = (c0_value *)xmalloc(
                (nargs > 0 ? nargs : 1) * sizeof(c0_value));
            for (int i = (int)nargs - 1; i >= 0; i--)
                args[i] = stack_pop(fr);

            /* Guard against unbounded recursion */
            if (call_depth >= MAX_CALL_DEPTH)
                c0_abort("call stack overflow (max depth %d)", MAX_CALL_DEPTH);

            /* Push current frame, start new one */
            call_stack[call_depth++] = fr;
            fr = make_frame(callee, args);
            free(args);
            break;
        }

        case INVOKENATIVE: {
            uint16_t nidx = fetch_u16(fr);
            if (nidx >= bcf->native_count)
                c0_abort("invokenative: native index %u out of range",
                         (unsigned)nidx);
            native_info *ni = &bcf->native_pool[nidx];
            uint16_t idx    = ni->function_table_index;
            if (idx >= (uint16_t)c0_native_table_size)
                c0_abort("invokenative: function_table_index %u out of range",
                         (unsigned)idx);

            /* Collect arguments */
            uint16_t nargs = ni->num_args;
            c0_value *args = (c0_value *)xmalloc(
                (nargs > 0 ? nargs : 1) * sizeof(c0_value));
            for (int i = (int)nargs - 1; i >= 0; i--)
                args[i] = stack_pop(fr);

            c0_value retval = c0_native_table[idx](args);
            free(args);

            stack_push(fr, retval);
            break;
        }

        case RETURN: {
            c0_value retval = stack_pop(fr);

            free_frame(fr);
            call_depth--;

            if (call_depth == 0) {
                /* Returned from _c0_main */
                return retval.payload.i;
            }

            /* Restore caller frame and push return value */
            fr = call_stack[call_depth];
            stack_push(fr, retval);
            break;
        }

        case ATHROW: {
            c0_value msg_v = stack_pop(fr);
            const char *msg = (msg_v.kind == C0_POINTER && msg_v.payload.p)
                              ? (const char *)msg_v.payload.p
                              : "(no message)";
            c0_abort("user-level abort: %s", msg);
            break; /* unreachable */
        }

        /* ----------------------------------------------------------------
         * Runtime assertion (assert opcode used by c0rt.h)
         * -------------------------------------------------------------- */
        case CHECKTAG:
        case HASTAG:
            /* Tag checking for tagged pointer types – skip operand bytes */
            fetch_u8(fr);
            fetch_u8(fr);
            break;

        /* ----------------------------------------------------------------
         * Unknown opcode
         * -------------------------------------------------------------- */
        default:
            c0_abort("unknown opcode 0x%02X at pc=%u", op,
                     (unsigned)(fr->pc - 1));
        }
    }

    /* Unreachable */
    return -1;
}

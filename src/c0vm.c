/*
 * C0VM - C0 Virtual Machine
 * C0 Virtual Machine Implementation
 *
 * c0vm.c - Core bytecode interpreter and execution tracer.
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

#define STACK_INIT_CAP  32
#define MAX_CALL_DEPTH  1024
#define MAX_TRACE_STEPS 10000

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
 * Heap Object Tracker (for visualizer inspection and memory monitoring)
 * --------------------------------------------------------------------- */
typedef enum { HEAP_STRUCT, HEAP_ARRAY } heap_kind;

typedef struct heap_block {
    heap_kind kind;
    void     *ptr;
    int32_t   size;     /* struct byte size or array element count */
    int32_t   elt_size; /* element size for array */
    struct heap_block *next;
} heap_block;

static heap_block *global_heap = NULL;

static void track_heap_alloc(heap_kind kind, void *ptr, int32_t size, int32_t elt_size) {
    heap_block *b = (heap_block *)malloc(sizeof(heap_block));
    if (!b) return;
    b->kind     = kind;
    b->ptr      = ptr;
    b->size     = size;
    b->elt_size = elt_size;
    b->next     = global_heap;
    global_heap = b;
}

static void clear_heap_tracker(void) {
    heap_block *b = global_heap;
    while (b) {
        heap_block *next = b->next;
        free(b);
        b = next;
    }
    global_heap = NULL;
}

/* -----------------------------------------------------------------------
 * Frame helpers
 * --------------------------------------------------------------------- */
static frame *make_frame(uint16_t fn_id, const function_info *f, c0_value *args) {
    frame *fr = (frame *)xmalloc(sizeof(frame));
    fr->fn_id = fn_id;
    fr->P     = f->code;
    fr->pc    = 0;

    uint16_t nv = f->num_vars > 0 ? f->num_vars : 1;
    fr->num_vars = nv;
    fr->V = (c0_value *)xcalloc(nv, sizeof(c0_value));
    for (uint16_t i = 0; i < nv; i++)
        fr->V[i] = c0_int(0);

    for (uint16_t i = 0; i < f->num_args; i++)
        fr->V[i] = args[i];

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
 * Stack operations
 * --------------------------------------------------------------------- */
static inline void stack_push(frame *fr, c0_value v) {
    if (fr->sp >= fr->s_size) {
        int32_t new_cap = fr->s_size * 2;
        fr->S = (c0_value *)realloc(fr->S, new_cap * sizeof(c0_value));
        if (fr->S == NULL)
            c0_abort("operand stack overflow");
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
static c0_array *alloc_array(int32_t count, int32_t elt_size) {
    if (count < 0)
        c0_abort("cannot allocate array with negative count %d", count);
    c0_array *a = (c0_array *)xmalloc(sizeof(c0_array));
    a->count    = count;
    a->elt_size = elt_size;
    a->data     = xcalloc(count > 0 ? (size_t)count : 1, (size_t)elt_size);
    track_heap_alloc(HEAP_ARRAY, a, count, elt_size);
    return a;
}

static void *array_elem_ptr(c0_array *a, int32_t i) {
    c0_check_null(a);
    c0_check_bounds(i, a->count);
    return (char *)a->data + (size_t)i * (size_t)a->elt_size;
}

/* -----------------------------------------------------------------------
 * JSON output escaping
 * --------------------------------------------------------------------- */
static void json_print_string(FILE *f, const char *s) {
    fputc('"', f);
    while (*s) {
        if (*s == '"') fputs("\\\"", f);
        else if (*s == '\\') fputs("\\\\", f);
        else if (*s == '\n') fputs("\\n", f);
        else if (*s == '\r') fputs("\\r", f);
        else if (*s == '\t') fputs("\\t", f);
        else if ((unsigned char)*s < 32) fprintf(f, "\\u%04x", (unsigned char)*s);
        else fputc(*s, f);
        s++;
    }
    fputc('"', f);
}

/* -----------------------------------------------------------------------
 * execute() and execute_with_trace()
 * --------------------------------------------------------------------- */

int execute(bc0_file *bcf) {
    return execute_with_trace(bcf, NULL);
}

int execute_with_trace(bc0_file *bcf, const char *trace_path) {
    if (bcf->function_count == 0)
        c0_abort("no functions in bytecode file");

    FILE *tf = NULL;
    if (trace_path != NULL) {
        tf = fopen(trace_path, "w");
        if (!tf) {
            fprintf(stderr, "Warning: could not open trace output '%s'\n", trace_path);
        }
    }

    /* Initialize trace file header */
    if (tf) {
        c0_trace_clear_stdout();
        clear_heap_tracker();

        fprintf(tf, "{\n  \"functions\": [\n");
        for (uint16_t i = 0; i < bcf->function_count; i++) {
            function_info *fn = &bcf->function_pool[i];
            fprintf(tf, "    {\n      \"id\": %u,\n      \"name\": \"%s\",\n      \"num_args\": %u,\n      \"num_vars\": %u,\n      \"code_length\": %u,\n      \"instructions\": [\n",
                    (unsigned)i, (i == 0 ? "_c0_main" : "fn"), (unsigned)fn->num_args,
                    (unsigned)fn->num_vars, (unsigned)fn->code_length);

            uint16_t cur_pc = 0;
            char ins_buf[128];
            bool first_ins = true;
            while (cur_pc < fn->code_length) {
                uint16_t next_pc = cur_pc;
                format_instruction(fn->code, cur_pc, &next_pc, ins_buf, sizeof(ins_buf));
                if (!first_ins) fprintf(tf, ",\n");
                first_ins = false;
                fprintf(tf, "        {\"pc\": %u, \"mnemonic\": \"%s\"}", (unsigned)cur_pc, ins_buf);
                cur_pc = next_pc;
            }
            fprintf(tf, "\n      ]\n    }%s\n", (i + 1 < bcf->function_count ? "," : ""));
        }
        fprintf(tf, "  ],\n  \"steps\": [\n");
    }

    frame *call_stack[MAX_CALL_DEPTH];
    int    call_depth = 0;

    const function_info *main_fn = &bcf->function_pool[0];
    if (main_fn->num_args != 0)
        c0_abort("_c0_main must take 0 arguments");

    frame *fr = make_frame(0, main_fn, NULL);
    call_stack[call_depth++] = fr;

    size_t step_count = 0;

    /* ===== Main Execution Loop =========================================== */
    uint8_t op;

#if defined(__GNUC__) && defined(USE_COMPUTED_GOTO)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Woverride-init"
    static const void *const dispatch_table[256] = {
        [0 ... 255]   = &&op_DEFAULT,
        [NOP]         = &&op_NOP,
        [ACONST_NULL] = &&op_ACONST_NULL,
        [BIPUSH]      = &&op_BIPUSH,
        [ILDC]        = &&op_ILDC,
        [ALDC]        = &&op_ALDC,
        [VLOAD]       = &&op_VLOAD,
        [VSTORE]      = &&op_VSTORE,
        [POP]         = &&op_POP,
        [POP2]        = &&op_POP2,
        [DUP]         = &&op_DUP,
        [SWAP]        = &&op_SWAP,
        [IADD]        = &&op_IADD,
        [ISUB]        = &&op_ISUB,
        [IMUL]        = &&op_IMUL,
        [IDIV]        = &&op_IDIV,
        [IREM]        = &&op_IREM,
        [INEG]        = &&op_INEG,
        [ISHL]        = &&op_ISHL,
        [ISHR]        = &&op_ISHR,
        [IUSHR]       = &&op_IUSHR,
        [IAND]        = &&op_IAND,
        [IOR]         = &&op_IOR,
        [IXOR]        = &&op_IXOR,
        [IFEQ]        = &&op_IFEQ,
        [IFNE]        = &&op_IFNE,
        [IFLT]        = &&op_IFLT,
        [IFGE]        = &&op_IFGE,
        [IFGT]        = &&op_IFGT,
        [IFLE]        = &&op_IFLE,
        [IF_ICMPEQ]   = &&op_IF_ICMPEQ,
        [IF_ICMPNE]   = &&op_IF_ICMPNE,
        [IF_ICMPLT]   = &&op_IF_ICMPLT,
        [IF_ICMPGE]   = &&op_IF_ICMPGE,
        [IF_ICMPGT]   = &&op_IF_ICMPGT,
        [IF_ICMPLE]   = &&op_IF_ICMPLE,
        [GOTO]        = &&op_GOTO,
        [ACMPEQ]      = &&op_ACMPEQ,
        [ACMPNE]      = &&op_ACMPNE,
        [IMLOAD]      = &&op_IMLOAD,
        [IMSTORE]     = &&op_IMSTORE,
        [AMLOAD]      = &&op_AMLOAD,
        [AMSTORE]     = &&op_AMSTORE,
        [NEW]         = &&op_NEW,
        [NEWARRAY]    = &&op_NEWARRAY,
        [ARRAYLENGTH] = &&op_ARRAYLENGTH,
        [GETFIELD]    = &&op_GETFIELD,
        [PUTFIELD]    = &&op_PUTFIELD,
        [AADDF]       = &&op_AADDF,
        [AADDS]       = &&op_AADDS,
        [INVOKESTATIC]= &&op_INVOKESTATIC,
        [INVOKENATIVE]= &&op_INVOKENATIVE,
        [RETURN]      = &&op_RETURN,
        [ATHROW]      = &&op_ATHROW,
        [CHECKTAG]    = &&op_CHECKTAG,
        [HASTAG]      = &&op_HASTAG
    };
#pragma GCC diagnostic pop

#define DISPATCH() do { \
    if (__builtin_expect(tf != NULL, 0)) goto do_trace; \
    op = fetch_u8(fr); \
    goto *dispatch_table[op]; \
} while (0)
#define OP_CASE(name) op_##name:

    if (tf) goto do_trace;
    op = fetch_u8(fr);
    goto *dispatch_table[op];

do_trace:
#else
#define DISPATCH() break
#define OP_CASE(name) case name:

    while (true) {
#endif
        uint16_t current_pc = fr->pc;

        /* Emit JSON trace step before executing instruction */
        if (tf && step_count < MAX_TRACE_STEPS) {
            char mbuf[128];
            format_instruction(fr->P, current_pc, NULL, mbuf, sizeof(mbuf));

            if (step_count > 0) fprintf(tf, ",\n");
            fprintf(tf, "    {\n");
            fprintf(tf, "      \"step\": %lu,\n", (unsigned long)step_count);
            fprintf(tf, "      \"fn_id\": %u,\n", (unsigned)fr->fn_id);
            fprintf(tf, "      \"pc\": %u,\n", (unsigned)current_pc);
            fprintf(tf, "      \"mnemonic\": \"%s\",\n", mbuf);

            /* Operand stack */
            fprintf(tf, "      \"stack\": [");
            for (int32_t s = 0; s < fr->sp; s++) {
                if (s > 0) fprintf(tf, ", ");
                if (fr->S[s].kind == C0_INTEGER) {
                    fprintf(tf, "{\"type\": \"int\", \"val\": %d}", (int)fr->S[s].payload.i);
                } else {
                    fprintf(tf, "{\"type\": \"ptr\", \"addr\": \"%p\"}", fr->S[s].payload.p);
                }
            }
            fprintf(tf, "],\n");

            /* Call stack */
            fprintf(tf, "      \"call_stack\": [\n");
            for (int d = 0; d < call_depth; d++) {
                frame *f = call_stack[d];
                fprintf(tf, "        {\"fn_id\": %u, \"pc\": %u, \"locals\": [",
                        (unsigned)f->fn_id, (unsigned)f->pc);
                for (uint16_t v = 0; v < f->num_vars; v++) {
                    if (v > 0) fprintf(tf, ", ");
                    if (f->V[v].kind == C0_INTEGER) {
                        fprintf(tf, "{\"type\": \"int\", \"val\": %d}", (int)f->V[v].payload.i);
                    } else {
                        fprintf(tf, "{\"type\": \"ptr\", \"addr\": \"%p\"}", f->V[v].payload.p);
                    }
                }
                fprintf(tf, "]}%s\n", (d + 1 < call_depth ? "," : ""));
            }
            fprintf(tf, "      ],\n");

            /* Heap objects */
            fprintf(tf, "      \"heap\": [");
            heap_block *hb = global_heap;
            bool first_hb = true;
            while (hb) {
                if (!first_hb) fprintf(tf, ", ");
                first_hb = false;
                if (hb->kind == HEAP_ARRAY) {
                    c0_array *arr = (c0_array *)hb->ptr;
                    fprintf(tf, "{\"kind\": \"array\", \"addr\": \"%p\", \"count\": %d, \"elt_size\": %d, \"elements\": [",
                            (void *)arr, (int)arr->count, (int)arr->elt_size);
                    for (int32_t e = 0; e < arr->count && e < 16; e++) {
                        if (e > 0) fprintf(tf, ", ");
                        if (arr->elt_size == 4) {
                            int32_t ival = *((int32_t *)((char *)arr->data + e * 4));
                            fprintf(tf, "%d", ival);
                        } else if (arr->elt_size == 1) {
                            char cval = *((char *)arr->data + e);
                            fprintf(tf, "%d", (int)cval);
                        } else {
                            fprintf(tf, "0");
                        }
                    }
                    fprintf(tf, "]}");
                } else {
                    fprintf(tf, "{\"kind\": \"struct\", \"addr\": \"%p\", \"size\": %d}",
                            hb->ptr, (int)hb->size);
                }
                hb = hb->next;
            }
            fprintf(tf, "],\n");

            /* Stdout snapshot */
            fprintf(tf, "      \"stdout\": ");
            json_print_string(tf, c0_trace_get_stdout_snapshot());
            fprintf(tf, "\n    }");

            step_count++;
        }

        op = fetch_u8(fr);

#if defined(__GNUC__) && defined(USE_COMPUTED_GOTO)
        goto *dispatch_table[op];
#else
        switch (op) {
#endif

        OP_CASE(NOP)
            DISPATCH();

        OP_CASE(ACONST_NULL)
            stack_push(fr, c0_ptr(NULL));
            DISPATCH();

        OP_CASE(BIPUSH) {
            int8_t b = (int8_t)fetch_u8(fr);
            stack_push(fr, c0_int((int32_t)b));
            DISPATCH();
        }

        OP_CASE(ILDC) {
            uint16_t idx = fetch_u16(fr);
            if (idx >= bcf->int_count)
                c0_abort("ildc: index %u out of range (int_count=%u)",
                         (unsigned)idx, (unsigned)bcf->int_count);
            stack_push(fr, c0_int(bcf->int_pool[idx]));
            DISPATCH();
        }

        OP_CASE(ALDC) {
            uint16_t idx = fetch_u16(fr);
            if (idx >= bcf->string_count)
                c0_abort("aldc: index %u out of range (string_count=%u)",
                         (unsigned)idx, (unsigned)bcf->string_count);
            stack_push(fr, c0_ptr((void *)&bcf->string_pool[idx]));
            DISPATCH();
        }

        OP_CASE(VLOAD) {
            uint8_t i = fetch_u8(fr);
            stack_push(fr, fr->V[i]);
            DISPATCH();
        }

        OP_CASE(VSTORE) {
            uint8_t  i = fetch_u8(fr);
            c0_value v = stack_pop(fr);
            fr->V[i]   = v;
            DISPATCH();
        }

        OP_CASE(POP)
            (void)stack_pop(fr);
            DISPATCH();

        OP_CASE(POP2)
            (void)stack_pop(fr);
            (void)stack_pop(fr);
            DISPATCH();

        OP_CASE(DUP) {
            c0_value v = stack_peek(fr);
            stack_push(fr, v);
            DISPATCH();
        }

        OP_CASE(SWAP) {
            c0_value a = stack_pop(fr);
            c0_value b = stack_pop(fr);
            stack_push(fr, a);
            stack_push(fr, b);
            DISPATCH();
        }

        OP_CASE(IADD) {
            c0_value y = stack_pop(fr), x = stack_pop(fr);
            stack_push(fr, c0_int(x.payload.i + y.payload.i));
            DISPATCH();
        }
        OP_CASE(ISUB) {
            c0_value y = stack_pop(fr), x = stack_pop(fr);
            stack_push(fr, c0_int(x.payload.i - y.payload.i));
            DISPATCH();
        }
        OP_CASE(IMUL) {
            c0_value y = stack_pop(fr), x = stack_pop(fr);
            stack_push(fr, c0_int(x.payload.i * y.payload.i));
            DISPATCH();
        }
        OP_CASE(IDIV) {
            c0_value y = stack_pop(fr), x = stack_pop(fr);
            c0_check_div(y.payload.i);
            if (x.payload.i == INT32_MIN && y.payload.i == -1)
                c0_abort("integer overflow: INT_MIN / -1");
            stack_push(fr, c0_int(x.payload.i / y.payload.i));
            DISPATCH();
        }
        OP_CASE(IREM) {
            c0_value y = stack_pop(fr), x = stack_pop(fr);
            c0_check_div(y.payload.i);
            if (x.payload.i == INT32_MIN && y.payload.i == -1)
                c0_abort("integer overflow: INT_MIN %% -1");
            stack_push(fr, c0_int(x.payload.i % y.payload.i));
            DISPATCH();
        }
        OP_CASE(INEG) {
            c0_value x = stack_pop(fr);
            stack_push(fr, c0_int(-x.payload.i));
            DISPATCH();
        }

        OP_CASE(ISHL) {
            c0_value y = stack_pop(fr), x = stack_pop(fr);
            c0_check_shift(y.payload.i);
            stack_push(fr, c0_int(x.payload.i << y.payload.i));
            DISPATCH();
        }
        OP_CASE(ISHR) {
            c0_value y = stack_pop(fr), x = stack_pop(fr);
            int32_t  s = y.payload.i;
            c0_check_shift(s);
            int32_t val = x.payload.i;
            int32_t res = (s == 0) ? val : (val >> s);
            stack_push(fr, c0_int(res));
            DISPATCH();
        }
        OP_CASE(IUSHR) {
            c0_value y = stack_pop(fr), x = stack_pop(fr);
            c0_check_shift(y.payload.i);
            uint32_t u = (uint32_t)x.payload.i;
            stack_push(fr, c0_int((int32_t)(u >> (uint32_t)y.payload.i)));
            DISPATCH();
        }
        OP_CASE(IAND) {
            c0_value y = stack_pop(fr), x = stack_pop(fr);
            stack_push(fr, c0_int(x.payload.i & y.payload.i));
            DISPATCH();
        }
        OP_CASE(IOR) {
            c0_value y = stack_pop(fr), x = stack_pop(fr);
            stack_push(fr, c0_int(x.payload.i | y.payload.i));
            DISPATCH();
        }
        OP_CASE(IXOR) {
            c0_value y = stack_pop(fr), x = stack_pop(fr);
            stack_push(fr, c0_int(x.payload.i ^ y.payload.i));
            DISPATCH();
        }

#define BRANCH_INT1(cmp)                                       \
        do {                                                   \
            int16_t off = (int16_t)fetch_u16(fr);             \
            c0_value v  = stack_pop(fr);                      \
            if (v.payload.i cmp 0) {                          \
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

        OP_CASE(IFEQ) BRANCH_INT1(==); DISPATCH();
        OP_CASE(IFNE) BRANCH_INT1(!=); DISPATCH();
        OP_CASE(IFLT) BRANCH_INT1(<);  DISPATCH();
        OP_CASE(IFGE) BRANCH_INT1(>=); DISPATCH();
        OP_CASE(IFGT) BRANCH_INT1(>);  DISPATCH();
        OP_CASE(IFLE) BRANCH_INT1(<=); DISPATCH();

        OP_CASE(IF_ICMPEQ) BRANCH_INT2(==); DISPATCH();
        OP_CASE(IF_ICMPNE) BRANCH_INT2(!=); DISPATCH();
        OP_CASE(IF_ICMPLT) BRANCH_INT2(<);  DISPATCH();
        OP_CASE(IF_ICMPGE) BRANCH_INT2(>=); DISPATCH();
        OP_CASE(IF_ICMPGT) BRANCH_INT2(>);  DISPATCH();
        OP_CASE(IF_ICMPLE) BRANCH_INT2(<=); DISPATCH();

#undef BRANCH_INT1
#undef BRANCH_INT2

        OP_CASE(ACMPEQ) {
            int16_t  off = (int16_t)fetch_u16(fr);
            c0_value b   = stack_pop(fr);
            c0_value a   = stack_pop(fr);
            if (a.payload.p == b.payload.p)
                fr->pc = (uint16_t)(fr->pc - 3 + off);
            DISPATCH();
        }
        OP_CASE(ACMPNE) {
            int16_t  off = (int16_t)fetch_u16(fr);
            c0_value b   = stack_pop(fr);
            c0_value a   = stack_pop(fr);
            if (a.payload.p != b.payload.p)
                fr->pc = (uint16_t)(fr->pc - 3 + off);
            DISPATCH();
        }

        OP_CASE(GOTO) {
            int16_t off = (int16_t)fetch_u16(fr);
            fr->pc = (uint16_t)(fr->pc - 3 + off);
            DISPATCH();
        }

        OP_CASE(IMLOAD) {
            c0_value addr = stack_pop(fr);
            c0_check_null(addr.payload.p);
            int32_t val;
            memcpy(&val, addr.payload.p, sizeof(int32_t));
            stack_push(fr, c0_int(val));
            DISPATCH();
        }
        OP_CASE(IMSTORE) {
            c0_value val  = stack_pop(fr);
            c0_value addr = stack_pop(fr);
            c0_check_null(addr.payload.p);
            int32_t i = val.payload.i;
            memcpy(addr.payload.p, &i, sizeof(int32_t));
            DISPATCH();
        }
        OP_CASE(AMLOAD) {
            c0_value addr = stack_pop(fr);
            c0_check_null(addr.payload.p);
            void *p;
            memcpy(&p, addr.payload.p, sizeof(void *));
            stack_push(fr, c0_ptr(p));
            DISPATCH();
        }
        OP_CASE(AMSTORE) {
            c0_value val  = stack_pop(fr);
            c0_value addr = stack_pop(fr);
            c0_check_null(addr.payload.p);
            void *p = val.payload.p;
            memcpy(addr.payload.p, &p, sizeof(void *));
            DISPATCH();
        }

        OP_CASE(NEW) {
            uint8_t s = fetch_u8(fr);
            void *p   = xcalloc(1, s);
            track_heap_alloc(HEAP_STRUCT, p, (int32_t)s, 1);
            stack_push(fr, c0_ptr(p));
            DISPATCH();
        }
        OP_CASE(GETFIELD) {
            uint8_t  o1  = fetch_u8(fr);
            uint8_t  o2  = fetch_u8(fr);
            uint16_t off = (uint16_t)((o1 << 8) | o2);
            c0_value ptr = stack_pop(fr);
            c0_check_null(ptr.payload.p);
            void *field_addr = (char *)ptr.payload.p + off;
            c0_value stored;
            memcpy(&stored, field_addr, sizeof(c0_value));
            stack_push(fr, stored);
            DISPATCH();
        }
        OP_CASE(PUTFIELD) {
            uint8_t  o1  = fetch_u8(fr);
            uint8_t  o2  = fetch_u8(fr);
            uint16_t off = (uint16_t)((o1 << 8) | o2);
            c0_value val = stack_pop(fr);
            c0_value ptr = stack_pop(fr);
            c0_check_null(ptr.payload.p);
            void *field_addr = (char *)ptr.payload.p + off;
            memcpy(field_addr, &val, sizeof(c0_value));
            DISPATCH();
        }
        OP_CASE(AADDF) {
            uint8_t  f   = fetch_u8(fr);
            c0_value ptr = stack_pop(fr);
            c0_check_null(ptr.payload.p);
            stack_push(fr, c0_ptr((char *)ptr.payload.p + f));
            DISPATCH();
        }

        OP_CASE(NEWARRAY) {
            uint8_t  s     = fetch_u8(fr);
            c0_value cnt_v = stack_pop(fr);
            int32_t  count = cnt_v.payload.i;
            c0_array *arr  = alloc_array(count, (int32_t)s);
            stack_push(fr, c0_ptr((void *)arr));
            DISPATCH();
        }
        OP_CASE(ARRAYLENGTH) {
            c0_value arr_v = stack_pop(fr);
            c0_check_null(arr_v.payload.p);
            c0_array *arr = (c0_array *)arr_v.payload.p;
            stack_push(fr, c0_int(arr->count));
            DISPATCH();
        }
        OP_CASE(AADDS) {
            c0_value idx_v = stack_pop(fr);
            c0_value arr_v = stack_pop(fr);
            c0_check_null(arr_v.payload.p);
            c0_array *arr  = (c0_array *)arr_v.payload.p;
            int32_t   idx  = idx_v.payload.i;
            void     *ep   = array_elem_ptr(arr, idx);
            stack_push(fr, c0_ptr(ep));
            DISPATCH();
        }

        OP_CASE(INVOKESTATIC) {
            uint16_t fidx = fetch_u16(fr);
            if (fidx >= bcf->function_count)
                c0_abort("invokestatic: function index %u out of range",
                         (unsigned)fidx);
            const function_info *callee = &bcf->function_pool[fidx];

            uint16_t nargs = callee->num_args;
            c0_value *args = (c0_value *)xmalloc(
                (nargs > 0 ? nargs : 1) * sizeof(c0_value));
            for (int i = (int)nargs - 1; i >= 0; i--)
                args[i] = stack_pop(fr);

            if (call_depth >= MAX_CALL_DEPTH)
                c0_abort("call stack overflow (max depth %d)", MAX_CALL_DEPTH);

            call_stack[call_depth++] = fr;
            fr = make_frame(fidx, callee, args);
            free(args);
            DISPATCH();
        }

        OP_CASE(INVOKENATIVE) {
            uint16_t nidx = fetch_u16(fr);
            if (nidx >= bcf->native_count)
                c0_abort("invokenative: native index %u out of range",
                         (unsigned)nidx);
            native_info *ni = &bcf->native_pool[nidx];
            uint16_t idx    = ni->function_table_index;
            if (idx >= (uint16_t)c0_native_table_size)
                c0_abort("invokenative: function_table_index %u out of range",
                         (unsigned)idx);

            uint16_t nargs = ni->num_args;
            c0_value *args = (c0_value *)xmalloc(
                (nargs > 0 ? nargs : 1) * sizeof(c0_value));
            for (int i = (int)nargs - 1; i >= 0; i--)
                args[i] = stack_pop(fr);

            c0_value retval = c0_native_table[idx](args);
            free(args);

            stack_push(fr, retval);
            DISPATCH();
        }

        OP_CASE(RETURN) {
            c0_value retval = stack_pop(fr);

            free_frame(fr);
            call_depth--;

            if (call_depth == 0) {
                if (tf) {
                    fprintf(tf, "\n  ],\n  \"return_value\": %d\n}\n", (int)retval.payload.i);
                    fclose(tf);
                }
                clear_heap_tracker();
                return retval.payload.i;
            }

            fr = call_stack[call_depth];
            stack_push(fr, retval);
            DISPATCH();
        }

        OP_CASE(ATHROW) {
            c0_value msg_v = stack_pop(fr);
            const char *msg = (msg_v.kind == C0_POINTER && msg_v.payload.p)
                              ? (const char *)msg_v.payload.p
                              : "(no message)";
            if (tf) fclose(tf);
            c0_abort("user-level abort: %s", msg);
            DISPATCH();
        }

        OP_CASE(CHECKTAG)
        OP_CASE(HASTAG) {
            fetch_u8(fr);
            fetch_u8(fr);
            DISPATCH();
        }

#if defined(__GNUC__) && defined(USE_COMPUTED_GOTO)
        op_DEFAULT:
#else
        default:
#endif
            if (tf) fclose(tf);
            c0_abort("unknown opcode 0x%02X at pc=%u", op,
                     (unsigned)(fr->pc - 1));

#if !defined(__GNUC__) || !defined(USE_COMPUTED_GOTO)
        }
    }
#endif

    if (tf) fclose(tf);
    return -1;
}

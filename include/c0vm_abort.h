
/*
 * C0VM - C0 Virtual Machine
 * C0 Virtual Machine Implementation
 *
 * c0vm_abort.h - Runtime error handling helpers.
 *
 * In C0, any illegal operation (null-pointer deref, division by zero,
 * array out-of-bounds, etc.) aborts execution with a descriptive message.
 * We implement this with a setjmp/longjmp escape so memory allocated
 * in c0vm_main can be freed before exit.
 */

#ifndef C0VM_ABORT_H
#define C0VM_ABORT_H

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>

/* Global jump buffer used to escape from the execute() loop on error */
extern jmp_buf c0vm_error_buf;

/*
 * c0_abort(fmt, ...) – print an error message and longjmp back to the
 * top-level handler.  The va-args work exactly like printf.
 */
#define c0_abort(fmt, ...)                                                 \
    do {                                                                   \
        fprintf(stderr, "\nC0 VM runtime error: " fmt "\n", ##__VA_ARGS__);\
        longjmp(c0vm_error_buf, 1);                                        \
    } while (0)

/* Convenience assertions */
#define c0_assert(cond, fmt, ...)                                          \
    do { if (!(cond)) c0_abort(fmt, ##__VA_ARGS__); } while (0)

#define c0_check_null(p)                                                   \
    c0_assert((p) != NULL, "null pointer dereference")

#define c0_check_div(divisor)                                              \
    c0_assert((divisor) != 0, "division or modulo by zero")

#define c0_check_bounds(idx, count)                                        \
    c0_assert((idx) >= 0 && (idx) < (count),                              \
              "array index %d out of bounds [0, %d)", (idx), (count))

#define c0_check_shift(s)                                                  \
    c0_assert((s) >= 0 && (s) < 32,                                       \
              "shift amount %d out of range [0,32)", (s))

#endif /* C0VM_ABORT_H */

/*
 * C0VM - C0 Virtual Machine
 * C0 Virtual Machine Implementation
 *
 * c0_native.c - Implementations of the native (C library) functions that
 * can be called from C0 bytecode via the INVOKENATIVE opcode.
 *
 * Each function receives a c0_value[] array of arguments and returns a
 * single c0_value.  The order matches the C0 standard library interface.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <limits.h>

#include "c0vm.h"
#include "c0vm_abort.h"
#include "c0_native.h"

/* -----------------------------------------------------------------------
 * Helper to extract typed values from args[]
 * --------------------------------------------------------------------- */
static inline int32_t     arg_int(c0_value *args, int i) { return args[i].payload.i; }
static inline const char *arg_str(c0_value *args, int i) {
    c0_check_null(args[i].payload.p);
    return (const char *)args[i].payload.p;
}
static inline void *arg_ptr(c0_value *args, int i) { return args[i].payload.p; }

static inline char *c0_strdup(const char *s) {
    if (!s) return NULL;
    size_t len = strlen(s);
    char *copy = (char *)malloc(len + 1);
    if (copy) memcpy(copy, s, len + 1);
    return copy;
}

static char  *trace_out_buf = NULL;
static size_t trace_out_len = 0;
static size_t trace_out_cap = 0;

void c0_trace_append_stdout(const char *str) {
    if (!str) return;
    size_t slen = strlen(str);
    if (trace_out_len + slen + 1 > trace_out_cap) {
        size_t new_cap = (trace_out_cap == 0) ? 1024 : trace_out_cap * 2;
        while (new_cap < trace_out_len + slen + 1) new_cap *= 2;
        char *p = (char *)realloc(trace_out_buf, new_cap);
        if (!p) return;
        trace_out_buf = p;
        trace_out_cap = new_cap;
    }
    memcpy(trace_out_buf + trace_out_len, str, slen);
    trace_out_len += slen;
    trace_out_buf[trace_out_len] = '\0';
}

const char *c0_trace_get_stdout_snapshot(void) {
    return trace_out_buf ? trace_out_buf : "";
}

void c0_trace_clear_stdout(void) {
    trace_out_len = 0;
    if (trace_out_buf) trace_out_buf[0] = '\0';
}

/* -----------------------------------------------------------------------
 * conio – console I/O
 * --------------------------------------------------------------------- */

c0_value c0_print(c0_value *args) {
    const char *s = arg_str(args, 0);
    printf("%s", s);
    fflush(stdout);
    c0_trace_append_stdout(s);
    return c0_int(0);
}

c0_value c0_println(c0_value *args) {
    const char *s = arg_str(args, 0);
    printf("%s\n", s);
    fflush(stdout);
    c0_trace_append_stdout(s);
    c0_trace_append_stdout("\n");
    return c0_int(0);
}

c0_value c0_printint(c0_value *args) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", arg_int(args, 0));
    printf("%s", buf);
    fflush(stdout);
    c0_trace_append_stdout(buf);
    return c0_int(0);
}

c0_value c0_printbool(c0_value *args) {
    const char *s = arg_int(args, 0) ? "true" : "false";
    printf("%s", s);
    fflush(stdout);
    c0_trace_append_stdout(s);
    return c0_int(0);
}

c0_value c0_printchar(c0_value *args) {
    char buf[2] = { (char)arg_int(args, 0), '\0' };
    printf("%s", buf);
    fflush(stdout);
    c0_trace_append_stdout(buf);
    return c0_int(0);
}

c0_value c0_flush(c0_value *args) {
    (void)args;
    fflush(stdout);
    return c0_int(0);
}

c0_value c0_readline(c0_value *args) {
    (void)args;
    char  buf[4096];
    char *line = fgets(buf, sizeof(buf), stdin);
    if (line == NULL) {
        /* EOF – return empty string */
        char *s = c0_strdup("");
        return c0_ptr((void *)s);
    }
    /* Strip trailing newline */
    size_t len = strlen(line);
    if (len > 0 && line[len - 1] == '\n') line[len - 1] = '\0';
    char *s = c0_strdup(line);
    if (s == NULL) c0_abort("readline: out of memory");
    return c0_ptr((void *)s);
}

/* -----------------------------------------------------------------------
 * string – C0 string operations
 * (In C0, strings are immutable null-terminated C strings.)
 * --------------------------------------------------------------------- */

c0_value c0_string_length(c0_value *args) {
    return c0_int((int32_t)strlen(arg_str(args, 0)));
}

c0_value c0_string_charat(c0_value *args) {
    const char *s = arg_str(args, 0);
    int32_t     i = arg_int(args, 1);
    int32_t     n = (int32_t)strlen(s);
    c0_check_bounds(i, n);
    return c0_int((int32_t)(unsigned char)s[i]);
}

c0_value c0_string_compare(c0_value *args) {
    return c0_int(strcmp(arg_str(args, 0), arg_str(args, 1)));
}

c0_value c0_string_equal(c0_value *args) {
    return c0_int(strcmp(arg_str(args, 0), arg_str(args, 1)) == 0 ? 1 : 0);
}

c0_value c0_string_sub(c0_value *args) {
    const char *s   = arg_str(args, 0);
    int32_t     lo  = arg_int(args, 1);
    int32_t     hi  = arg_int(args, 2);
    int32_t     len = (int32_t)strlen(s);
    if (lo < 0 || hi < lo || hi > len)
        c0_abort("string_sub: indices [%d,%d) out of range for string of length %d",
                 lo, hi, len);
    int32_t sub_len = hi - lo;
    char   *buf     = (char *)malloc((size_t)(sub_len + 1));
    if (!buf) c0_abort("string_sub: out of memory");
    memcpy(buf, s + lo, (size_t)sub_len);
    buf[sub_len] = '\0';
    return c0_ptr((void *)buf);
}

c0_value c0_string_join(c0_value *args) {
    const char *a = arg_str(args, 0);
    const char *b = arg_str(args, 1);
    size_t la = strlen(a), lb = strlen(b);
    char  *buf = (char *)malloc(la + lb + 1);
    if (!buf) c0_abort("string_join: out of memory");
    memcpy(buf, a, la);
    memcpy(buf + la, b, lb + 1);
    return c0_ptr((void *)buf);
}

c0_value c0_string_fromchar(c0_value *args) {
    char  ch  = (char)arg_int(args, 0);
    char *buf = (char *)malloc(2);
    if (!buf) c0_abort("string_fromchar: out of memory");
    buf[0] = ch;
    buf[1] = '\0';
    return c0_ptr((void *)buf);
}

/* string_terminated: checks if a char[] (c0_array) contains a '\0'
   within its first len elements. Returns bool (0/1). */
c0_value c0_string_terminated(c0_value *args) {
    /* arg0 = c0_array*, arg1 = int limit */
    void    *p   = arg_ptr(args, 0);
    int32_t  lim = arg_int(args, 1);
    c0_check_null(p);
    c0_array *arr = (c0_array *)p;
    int32_t   n   = (lim < arr->count) ? lim : arr->count;
    char     *data = (char *)arr->data;
    for (int32_t i = 0; i < n; i++)
        if (data[i] == '\0') return c0_int(1);
    return c0_int(0);
}

c0_value c0_string_to_chararray(c0_value *args) {
    const char *s = arg_str(args, 0);
    int32_t     n = (int32_t)strlen(s) + 1; /* include null terminator */
    /* Allocate a c0_array of chars */
    /* We reuse the same struct layout as c0_array */
    void *mem = malloc(sizeof(int32_t) * 2 + (size_t)n);
    if (!mem) c0_abort("string_to_chararray: out of memory");
    int32_t *hdr = (int32_t *)mem;
    hdr[0] = n;
    hdr[1] = 1; /* elt_size = 1 byte */
    memcpy(hdr + 2, s, (size_t)n);
    return c0_ptr(mem);
}

c0_value c0_string_from_chararray(c0_value *args) {
    void    *p   = arg_ptr(args, 0);
    c0_check_null(p);
    c0_array *arr = (c0_array *)p;
    char     *data = (char *)arr->data;
    /* Find null terminator */
    int32_t i;
    for (i = 0; i < arr->count; i++)
        if (data[i] == '\0') break;
    if (i == arr->count)
        c0_abort("string_from_chararray: no null terminator found");
    char *s = (char *)malloc((size_t)i + 1);
    if (!s) c0_abort("string_from_chararray: out of memory");
    memcpy(s, data, (size_t)i);
    s[i] = '\0';
    return c0_ptr((void *)s);
}

/* -----------------------------------------------------------------------
 * parse – numeric conversions
 * --------------------------------------------------------------------- */

c0_value c0_int_to_string(c0_value *args) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", arg_int(args, 0));
    char *s = c0_strdup(buf);
    if (!s) c0_abort("int_to_string: out of memory");
    return c0_ptr((void *)s);
}

c0_value c0_string_to_int(c0_value *args) {
    const char *s = arg_str(args, 0);
    char *end;
    long  val = strtol(s, &end, 10);
    if (*end != '\0')
        c0_abort("string_to_int: cannot parse \"%s\"", s);
    return c0_int((int32_t)val);
}

c0_value c0_bool_to_string(c0_value *args) {
    const char *lit = arg_int(args, 0) ? "true" : "false";
    char *s = c0_strdup(lit);
    if (!s) c0_abort("bool_to_string: out of memory");
    return c0_ptr((void *)s);
}

c0_value c0_char_to_string(c0_value *args) {
    return c0_string_fromchar(args); /* same implementation */
}

/* -----------------------------------------------------------------------
 * util – assertions and errors
 * --------------------------------------------------------------------- */

c0_value c0_abort_err(c0_value *args) {
    const char *msg = (args[0].kind == C0_POINTER && args[0].payload.p)
                      ? (const char *)args[0].payload.p
                      : "(no message)";
    c0_abort("abort called: %s", msg);
    return c0_int(0); /* unreachable */
}

c0_value c0_assert_err(c0_value *args) {
    int32_t     cond = arg_int(args, 0);
    const char *msg  = (args[1].kind == C0_POINTER && args[1].payload.p)
                       ? (const char *)args[1].payload.p
                       : "(no message)";
    if (!cond) c0_abort("assertion failed: %s", msg);
    return c0_int(0);
}

c0_value c0_error(c0_value *args) {
    const char *msg = (args[0].kind == C0_POINTER && args[0].payload.p)
                      ? (const char *)args[0].payload.p
                      : "(error)";
    c0_abort("error: %s", msg);
    return c0_int(0); /* unreachable */
}

/* -----------------------------------------------------------------------
 * args – command-line argument access
 * (These are stubs; real programs rarely use them in tests.)
 * --------------------------------------------------------------------- */

c0_value c0_args_argc(c0_value *args) {
    (void)args;
    return c0_int(0);
}

c0_value c0_args_argv(c0_value *args) {
    (void)args;
    c0_abort("args_argv: not supported in this build");
    return c0_ptr(NULL); /* unreachable */
}

/* -----------------------------------------------------------------------
 * Native function dispatch table
 *
 * The index in this table must match function_table_index stored in
 * the bc0 file.  The C0 compiler assigns these in the order the standard
 * library headers are included.  The standard ordering used by the C0 reference compiler
 * compiler is reproduced here.
 *
 * Index 0-5   : conio
 * Index 6-15  : string
 * Index 16-19 : parse
 * Index 20-22 : util / error
 * Index 23-24 : args
 * --------------------------------------------------------------------- */

c0_native_fn c0_native_table[] = {
    /* 0  */ c0_print,
    /* 1  */ c0_println,
    /* 2  */ c0_printint,
    /* 3  */ c0_printbool,
    /* 4  */ c0_printchar,
    /* 5  */ c0_readline,
    /* 6  */ c0_string_length,
    /* 7  */ c0_string_charat,
    /* 8  */ c0_string_compare,
    /* 9  */ c0_string_equal,
    /* 10 */ c0_string_sub,
    /* 11 */ c0_string_join,
    /* 12 */ c0_string_fromchar,
    /* 13 */ c0_string_terminated,
    /* 14 */ c0_string_to_chararray,
    /* 15 */ c0_string_from_chararray,
    /* 16 */ c0_int_to_string,
    /* 17 */ c0_string_to_int,
    /* 18 */ c0_bool_to_string,
    /* 19 */ c0_char_to_string,
    /* 20 */ c0_abort_err,
    /* 21 */ c0_assert_err,
    /* 22 */ c0_error,
    /* 23 */ c0_args_argc,
    /* 24 */ c0_args_argv,
    /* 25 */ c0_flush,
};

size_t c0_native_table_size =
    sizeof(c0_native_table) / sizeof(c0_native_table[0]);

/*
 * C0VM - C0 Virtual Machine
 * C0 Virtual Machine Implementation
 *
 * c0_native.h - Declarations for the native (C library) functions that
 * C0 programs can call via the INVOKENATIVE opcode.
 *
 * The native function table maps function_table_index values (stored in
 * native_info structs) to actual C function pointers.
 *
 * Standard native libraries provided by the C0 runtime:
 *   - conio   : print, println, readline, etc.
 *   - string  : string operations (length, sub, compare, …)
 *   - parse   : int_to_string, string_to_int
 *   - args    : argc, argv access
 *   - util    : abort, assert
 *   - img     : image operations (optional)
 */

#ifndef C0_NATIVE_H
#define C0_NATIVE_H

#include "c0vm.h"

/* -----------------------------------------------------------------------
 * The native function table.
 * Each index corresponds to a function_table_index in native_info.
 * We expose a flat array of function pointers plus its length.
 * --------------------------------------------------------------------- */

extern c0_native_fn c0_native_table[];
extern size_t       c0_native_table_size;

/* -----------------------------------------------------------------------
 * Native library function declarations
 * (implementations are in c0_native.c)
 * --------------------------------------------------------------------- */

/* conio */
c0_value c0_print(c0_value *args);
c0_value c0_println(c0_value *args);
c0_value c0_printint(c0_value *args);
c0_value c0_printbool(c0_value *args);
c0_value c0_printchar(c0_value *args);
c0_value c0_readline(c0_value *args);
c0_value c0_flush(c0_value *args);

/* string */
c0_value c0_string_length(c0_value *args);
c0_value c0_string_charat(c0_value *args);
c0_value c0_string_compare(c0_value *args);
c0_value c0_string_equal(c0_value *args);
c0_value c0_string_sub(c0_value *args);
c0_value c0_string_join(c0_value *args);
c0_value c0_string_fromchar(c0_value *args);
c0_value c0_string_terminated(c0_value *args);
c0_value c0_string_to_chararray(c0_value *args);
c0_value c0_string_from_chararray(c0_value *args);

/* parse */
c0_value c0_int_to_string(c0_value *args);
c0_value c0_string_to_int(c0_value *args);
c0_value c0_bool_to_string(c0_value *args);
c0_value c0_char_to_string(c0_value *args);

/* util */
c0_value c0_abort_err(c0_value *args);
c0_value c0_assert_err(c0_value *args);
c0_value c0_error(c0_value *args);

/* args */
c0_value c0_args_argc(c0_value *args);
c0_value c0_args_argv(c0_value *args);

/* Execution trace stdout interception */
void        c0_trace_append_stdout(const char *str);
const char *c0_trace_get_stdout_snapshot(void);
void        c0_trace_clear_stdout(void);

#endif /* C0_NATIVE_H */

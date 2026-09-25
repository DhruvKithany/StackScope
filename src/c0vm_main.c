/*
 * C0VM - C0 Virtual Machine
 * CMU 15-122: Principles of Imperative Computation
 *
 * c0vm_main.c - Entry point.
 *
 * Usage:
 *   c0vm <file.bc0>                     -- Execute bytecode
 *   c0vm -d <file.bc0>                  -- Disassemble bytecode
 *   c0vm -t <trace.json> <file.bc0>     -- Execute and write execution trace for visualizer
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <setjmp.h>

#include "c0vm.h"
#include "c0vm_abort.h"

/* Definition of the global error jump-buffer (declared in c0vm_abort.h) */
jmp_buf c0vm_error_buf;

static void print_usage(const char *prog) {
    fprintf(stderr, "Usage:\n");
    fprintf(stderr, "  %s <file.bc0>                      Execute bytecode\n", prog);
    fprintf(stderr, "  %s -d <file.bc0>                   Disassemble bytecode\n", prog);
    fprintf(stderr, "  %s -t <trace.json> <file.bc0>      Execute and export JSON trace for visualizer\n", prog);
}

int main(int argc, char *argv[]) {
    bool disasm_mode = false;
    const char * volatile trace_path = NULL;
    const char *bc0_path = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "--disasm") == 0) {
            disasm_mode = true;
        } else if ((strcmp(argv[i], "-t") == 0 || strcmp(argv[i], "--trace") == 0) && i + 1 < argc) {
            trace_path = argv[++i];
        } else if (argv[i][0] != '-') {
            bc0_path = argv[i];
        } else {
            fprintf(stderr, "Error: unknown option '%s'\n\n", argv[i]);
            print_usage(argv[0]);
            return EXIT_FAILURE;
        }
    }

    if (bc0_path == NULL) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    /* Parse the bytecode file */
    bc0_file *bcf = read_bc0_file(bc0_path);
    if (bcf == NULL) {
        fprintf(stderr, "Error: could not read bytecode file '%s'\n", bc0_path);
        return EXIT_FAILURE;
    }

    /* Disassembly mode */
    if (disasm_mode) {
        disassemble(bcf);
        free_bc0_file(bcf);
        return EXIT_SUCCESS;
    }

    int result = 0;

    /* Catch any runtime errors from execute() */
    if (setjmp(c0vm_error_buf) == 0) {
        /* Normal path: execute the program */
        result = execute_with_trace(bcf, trace_path);
        printf("Program exited with return value %d\n", result);
        if (trace_path) {
            printf("Execution trace saved to: %s\n", trace_path);
        }
    } else {
        /* Error path: execute() called c0_abort() */
        fprintf(stderr, "Execution aborted.\n");
        free_bc0_file(bcf);
        return EXIT_FAILURE;
    }

    free_bc0_file(bcf);
    return EXIT_SUCCESS;
}

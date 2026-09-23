/*
 * C0VM - C0 Virtual Machine
 * CMU 15-122: Principles of Imperative Computation
 *
 * c0vm_main.c - Entry point.
 *
 * Usage:  c0vm <file.bc0>
 *
 * Parses the .bc0 bytecode file, initialises the VM, runs _c0_main()
 * and prints its return value.  Any C0 runtime error is caught via
 * setjmp and reported with a non-zero exit code.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>

#include "c0vm.h"
#include "c0vm_abort.h"

/* Definition of the global error jump-buffer (declared in c0vm_abort.h) */
jmp_buf c0vm_error_buf;

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <file.bc0>\n", argv[0]);
        return EXIT_FAILURE;
    }

    const char *path = argv[1];

    /* Parse the bytecode file */
    bc0_file *bcf = read_bc0_file(path);
    if (bcf == NULL) {
        fprintf(stderr, "Error: could not read bytecode file '%s'\n", path);
        return EXIT_FAILURE;
    }

    int result = 0;

    /* Catch any runtime errors from execute() */
    if (setjmp(c0vm_error_buf) == 0) {
        /* Normal path: execute the program */
        result = execute(bcf);
        printf("Program exited with return value %d\n", result);
    } else {
        /* Error path: execute() called c0_abort() */
        fprintf(stderr, "Execution aborted.\n");
        free_bc0_file(bcf);
        return EXIT_FAILURE;
    }

    free_bc0_file(bcf);
    return EXIT_SUCCESS;
}

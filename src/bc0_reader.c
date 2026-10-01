/*
 * C0VM - C0 Virtual Machine
 * C0 Virtual Machine Implementation
 *
 * bc0_reader.c - Binary parser for .bc0 bytecode files.
 *
 * The .bc0 format is big-endian binary.  We use helper macros to read
 * multi-byte integers and report errors cleanly.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "c0vm.h"
#include "c0vm_abort.h"

/* -----------------------------------------------------------------------
 * Internal read helpers
 * --------------------------------------------------------------------- */

static void die(const char *msg) {
    fprintf(stderr, "bc0 parse error: %s\n", msg);
    longjmp(c0vm_error_buf, 1);
}

/* Read exactly n bytes into buf; abort on short read */
static void read_bytes(FILE *fp, void *buf, size_t n) {
    if (fread(buf, 1, n, fp) != n)
        die("unexpected end of file");
}



/* Read a big-endian uint16 */
static uint16_t read_u16(FILE *fp) {
    uint8_t buf[2];
    read_bytes(fp, buf, 2);
    return (uint16_t)((buf[0] << 8) | buf[1]);
}

/* Read a big-endian uint32 */
static uint32_t read_u32(FILE *fp) {
    uint8_t buf[4];
    read_bytes(fp, buf, 4);
    return ((uint32_t)buf[0] << 24) |
           ((uint32_t)buf[1] << 16) |
           ((uint32_t)buf[2] <<  8) |
            (uint32_t)buf[3];
}

/* Read a big-endian int32 (two's-complement same bytes as uint32) */
static int32_t read_i32(FILE *fp) {
    uint32_t u = read_u32(fp);
    int32_t  i;
    memcpy(&i, &u, 4);   /* standards-compliant type-pun */
    return i;
}

/* -----------------------------------------------------------------------
 * Safe allocators (abort on OOM)
 * --------------------------------------------------------------------- */

static void *xmalloc(size_t sz) {
    void *p = malloc(sz);
    if (p == NULL) die("out of memory");
    return p;
}

static void *xcalloc(size_t n, size_t sz) {
    void *p = calloc(n, sz);
    if (p == NULL) die("out of memory");
    return p;
}

/* -----------------------------------------------------------------------
 * Public: read_bc0_file
 * --------------------------------------------------------------------- */

bc0_file *read_bc0_file(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (fp == NULL) return NULL;

    bc0_file *bcf = (bc0_file *)xcalloc(1, sizeof(bc0_file));

    /* --- Magic number --- */
    uint32_t magic = read_u32(fp);
    bcf->magic = magic;
    if (magic != 0xC0C0FFEE) {
        fclose(fp);
        free(bcf);
        fprintf(stderr, "bc0 parse error: bad magic number 0x%08X\n", magic);
        return NULL;
    }

    /* --- Version (skip) --- */
    read_u16(fp); /* version; we accept any */

    /* --- Integer pool --- */
    bcf->int_count = read_u16(fp);
    if (bcf->int_count > 0) {
        bcf->int_pool = (int32_t *)xmalloc(bcf->int_count * sizeof(int32_t));
        for (uint16_t i = 0; i < bcf->int_count; i++)
            bcf->int_pool[i] = read_i32(fp);
    }

    /* --- String pool --- */
    bcf->string_count = read_u16(fp);
    if (bcf->string_count > 0) {
        bcf->string_pool = (char *)xmalloc(bcf->string_count + 1);
        read_bytes(fp, bcf->string_pool, bcf->string_count);
        bcf->string_pool[bcf->string_count] = '\0'; /* safety sentinel */
    }

    /* --- Function pool --- */
    bcf->function_count = read_u16(fp);
    if (bcf->function_count > 0) {
        bcf->function_pool =
            (function_info *)xmalloc(bcf->function_count * sizeof(function_info));
        for (uint16_t i = 0; i < bcf->function_count; i++) {
            function_info *f = &bcf->function_pool[i];
            f->num_args    = read_u16(fp);
            f->num_vars    = read_u16(fp);
            f->code_length = read_u16(fp);
            f->code = (uint8_t *)xmalloc(f->code_length);
            read_bytes(fp, f->code, f->code_length);
        }
    }

    /* --- Native pool --- */
    bcf->native_count = read_u16(fp);
    if (bcf->native_count > 0) {
        bcf->native_pool =
            (native_info *)xmalloc(bcf->native_count * sizeof(native_info));
        for (uint16_t i = 0; i < bcf->native_count; i++) {
            bcf->native_pool[i].num_args             = read_u16(fp);
            bcf->native_pool[i].function_table_index = read_u16(fp);
        }
    }

    fclose(fp);
    return bcf;
}

/* -----------------------------------------------------------------------
 * Public: free_bc0_file
 * --------------------------------------------------------------------- */

void free_bc0_file(bc0_file *bcf) {
    if (bcf == NULL) return;

    free(bcf->int_pool);
    free(bcf->string_pool);

    for (uint16_t i = 0; i < bcf->function_count; i++)
        free(bcf->function_pool[i].code);
    free(bcf->function_pool);

    free(bcf->native_pool);
    free(bcf);
}

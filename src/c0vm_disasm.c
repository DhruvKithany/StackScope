/*
 * C0VM - C0 Virtual Machine
 * CMU 15-122: Principles of Imperative Computation
 *
 * c0vm_disasm.c - Bytecode disassembler and instruction decoder.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "c0vm.h"

void format_instruction(const uint8_t *code, uint16_t pc, uint16_t *next_pc,
                        char *buf, size_t buf_size) {
    uint8_t op = code[pc];
    uint16_t cur = pc + 1;

    switch (op) {
        case 0x00:
            snprintf(buf, buf_size, "nop");
            break;
        case ACONST_NULL:
            snprintf(buf, buf_size, "aconst_null");
            break;
        case BIPUSH: {
            int8_t b = (int8_t)code[cur++];
            snprintf(buf, buf_size, "bipush %d", (int)b);
            break;
        }
        case ILDC: {
            uint16_t idx = (uint16_t)((code[cur] << 8) | code[cur + 1]);
            cur += 2;
            snprintf(buf, buf_size, "ildc int_pool[%u]", (unsigned)idx);
            break;
        }
        case ALDC: {
            uint16_t idx = (uint16_t)((code[cur] << 8) | code[cur + 1]);
            cur += 2;
            snprintf(buf, buf_size, "aldc str_pool[%u]", (unsigned)idx);
            break;
        }
        case VLOAD: {
            uint8_t i = code[cur++];
            snprintf(buf, buf_size, "vload V[%u]", (unsigned)i);
            break;
        }
        case VSTORE: {
            uint8_t i = code[cur++];
            snprintf(buf, buf_size, "vstore V[%u]", (unsigned)i);
            break;
        }
        case POP:
            snprintf(buf, buf_size, "pop");
            break;
        case POP2:
            snprintf(buf, buf_size, "pop2");
            break;
        case DUP:
            snprintf(buf, buf_size, "dup");
            break;
        case SWAP:
            snprintf(buf, buf_size, "swap");
            break;
        case IADD:
            snprintf(buf, buf_size, "iadd");
            break;
        case ISUB:
            snprintf(buf, buf_size, "isub");
            break;
        case IMUL:
            snprintf(buf, buf_size, "imul");
            break;
        case IDIV:
            snprintf(buf, buf_size, "idiv");
            break;
        case IREM:
            snprintf(buf, buf_size, "irem");
            break;
        case INEG:
            snprintf(buf, buf_size, "ineg");
            break;
        case ISHL:
            snprintf(buf, buf_size, "ishl");
            break;
        case ISHR:
            snprintf(buf, buf_size, "ishr");
            break;
        case IUSHR:
            snprintf(buf, buf_size, "iushr");
            break;
        case IAND:
            snprintf(buf, buf_size, "iand");
            break;
        case IOR:
            snprintf(buf, buf_size, "ior");
            break;
        case IXOR:
            snprintf(buf, buf_size, "ixor");
            break;

        /* Branching */
        case IFEQ:
        case IFNE:
        case IFLT:
        case IFGE:
        case IFGT:
        case IFLE:
        case IF_ICMPEQ:
        case IF_ICMPNE:
        case IF_ICMPLT:
        case IF_ICMPGE:
        case IF_ICMPGT:
        case IF_ICMPLE:
        case ACMPEQ:
        case ACMPNE:
        case GOTO: {
            int16_t off = (int16_t)((code[cur] << 8) | code[cur + 1]);
            cur += 2;
            int target = (int)pc + off;
            const char *name = "branch";
            switch (op) {
                case IFEQ:      name = "ifeq"; break;
                case IFNE:      name = "ifne"; break;
                case IFLT:      name = "iflt"; break;
                case IFGE:      name = "ifge"; break;
                case IFGT:      name = "ifgt"; break;
                case IFLE:      name = "ifle"; break;
                case IF_ICMPEQ: name = "if_icmpeq"; break;
                case IF_ICMPNE: name = "if_icmpne"; break;
                case IF_ICMPLT: name = "if_icmplt"; break;
                case IF_ICMPGE: name = "if_icmpge"; break;
                case IF_ICMPGT: name = "if_icmpgt"; break;
                case IF_ICMPLE: name = "if_icmple"; break;
                case ACMPEQ:    name = "acmpeq"; break;
                case ACMPNE:    name = "acmpne"; break;
                case GOTO:      name = "goto"; break;
            }
            snprintf(buf, buf_size, "%s %d (target: %04x)", name, (int)off, target >= 0 ? (unsigned)target : 0);
            break;
        }

        /* Functions */
        case INVOKESTATIC: {
            uint16_t fidx = (uint16_t)((code[cur] << 8) | code[cur + 1]);
            cur += 2;
            snprintf(buf, buf_size, "invokestatic fn_%u", (unsigned)fidx);
            break;
        }
        case INVOKENATIVE: {
            uint16_t nidx = (uint16_t)((code[cur] << 8) | code[cur + 1]);
            cur += 2;
            snprintf(buf, buf_size, "invokenative native_%u", (unsigned)nidx);
            break;
        }
        case RETURN:
            snprintf(buf, buf_size, "return");
            break;
        case ATHROW:
            snprintf(buf, buf_size, "athrow");
            break;

        /* Memory */
        case NEW: {
            uint8_t s = code[cur++];
            snprintf(buf, buf_size, "new size=%u", (unsigned)s);
            break;
        }
        case NEWARRAY: {
            uint8_t s = code[cur++];
            snprintf(buf, buf_size, "newarray elt_size=%u", (unsigned)s);
            break;
        }
        case ARRAYLENGTH:
            snprintf(buf, buf_size, "arraylength");
            break;
        case AADDS:
            snprintf(buf, buf_size, "aadds");
            break;
        case AADDF: {
            uint8_t f = code[cur++];
            snprintf(buf, buf_size, "aaddf offset=%u", (unsigned)f);
            break;
        }
        case GETFIELD: {
            uint16_t off = (uint16_t)((code[cur] << 8) | code[cur + 1]);
            cur += 2;
            snprintf(buf, buf_size, "getfield offset=%u", (unsigned)off);
            break;
        }
        case PUTFIELD: {
            uint16_t off = (uint16_t)((code[cur] << 8) | code[cur + 1]);
            cur += 2;
            snprintf(buf, buf_size, "putfield offset=%u", (unsigned)off);
            break;
        }
        case IMLOAD:
            snprintf(buf, buf_size, "imload");
            break;
        case IMSTORE:
            snprintf(buf, buf_size, "imstore");
            break;
        case AMLOAD:
            snprintf(buf, buf_size, "amload");
            break;
        case AMSTORE:
            snprintf(buf, buf_size, "amstore");
            break;
        case CHECKTAG:
            cur += 2;
            snprintf(buf, buf_size, "checktag");
            break;
        case HASTAG:
            cur += 2;
            snprintf(buf, buf_size, "hastag");
            break;
        default:
            snprintf(buf, buf_size, "unknown (0x%02X)", op);
            break;
    }

    if (next_pc) {
        *next_pc = cur;
    }
}

void disassemble(bc0_file *bcf) {
    if (!bcf) return;

    printf("============================================================\n");
    printf(" C0VM Bytecode Disassembly\n");
    printf(" Magic: 0x%08X  Functions: %u  Natives: %u  Ints: %u\n",
           bcf->magic, (unsigned)bcf->function_count,
           (unsigned)bcf->native_count, (unsigned)bcf->int_count);
    printf("============================================================\n\n");

    if (bcf->int_count > 0) {
        printf("--- Integer Pool (%u entries) ---\n", (unsigned)bcf->int_count);
        for (uint16_t i = 0; i < bcf->int_count; i++) {
            printf("  [%2u]: %d\n", (unsigned)i, bcf->int_pool[i]);
        }
        printf("\n");
    }

    if (bcf->string_count > 0) {
        printf("--- String Pool (%u bytes) ---\n", (unsigned)bcf->string_count);
        uint16_t idx = 0;
        while (idx < bcf->string_count) {
            printf("  [%2u]: \"%s\"\n", (unsigned)idx, &bcf->string_pool[idx]);
            idx += (uint16_t)strlen(&bcf->string_pool[idx]) + 1;
        }
        printf("\n");
    }

    for (uint16_t fidx = 0; fidx < bcf->function_count; fidx++) {
        function_info *fn = &bcf->function_pool[fidx];
        const char *name = (fidx == 0) ? "_c0_main" : "";
        printf("Function %u %s(args=%u, vars=%u, code_length=%u):\n",
               (unsigned)fidx, name, (unsigned)fn->num_args,
               (unsigned)fn->num_vars, (unsigned)fn->code_length);

        uint16_t pc = 0;
        char mnemonic[128];
        while (pc < fn->code_length) {
            uint16_t next_pc = pc;
            format_instruction(fn->code, pc, &next_pc, mnemonic, sizeof(mnemonic));

            /* Print hex bytes of instruction */
            printf("  %04x: ", (unsigned)pc);
            int byte_count = next_pc - pc;
            for (int b = 0; b < 4; b++) {
                if (b < byte_count) {
                    printf("%02x ", (unsigned)fn->code[pc + b]);
                } else {
                    printf("   ");
                }
            }
            printf("  %s\n", mnemonic);
            pc = next_pc;
        }
        printf("\n");
    }
}

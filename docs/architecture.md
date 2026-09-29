# Architecture Deep-Dive: Stack-Based Virtual Machines & C0VM

A comprehensive technical reference explaining the internals, design decisions, and memory models of stack-based bytecode virtual machines.

---

## 1. Why a Stack-Based Virtual Machine?

Virtual machines generally fall into one of two fundamental execution models:

| Dimension | **Stack-Based VM** (JVM, C0VM, WebAssembly, Python) | **Register-Based VM** (LuaJIT, Dalvik, LLVM IR) |
|:---|:---|:---|
| **Instruction Operands** | Implicit (top of operand stack $S$) | Explicit (numbered registers $r_0, r_1, r_2$) |
| **Instruction Size** | **Extremely compact** (often 1 byte opcode) | Larger (needs 2–4 bytes to encode registers) |
| **Compiler Complexity** | **Low**: simple post-order AST traversal | **High**: requires Graph Coloring Register Allocation |
| **VM Code Size** | Smaller bytecode binaries | Larger bytecode binaries |
| **Instruction Count** | More instructions per expression (pushes + ALU) | Fewer instructions per expression |
| **Portability** | Highest — no assumptions about physical registers | Good, but emulates a fixed register set |

### Concrete Example: Computing `(a + b) * c`

#### In a Stack Machine (C0VM / JVM):
```assembly
vload a       # S: [a]
vload b       # S: [a, b]
iadd          # S: [a + b]
vload c       # S: [a + b, c]
imul          # S: [(a + b) * c]
```
- Each opcode is **1 byte**! No register IDs needed because arguments are implicitly at the top of stack.
- The compiler does not need to decide which physical CPU registers to allocate.

#### In a Register Machine (x86-64 / Lua):
```assembly
ADD r0, r1, r2    # r0 = r1 + r2
MUL r0, r0, r3    # r0 = r0 * r3
```
- Needs fewer instructions, but each instruction must encode source and destination register numbers.

---

## 2. The Three Memory Regions in C0VM

```
┌──────────────────────────────────────────────────────────────────┐
│                           C0VM Memory                            │
├────────────────────────────────┬─────────────────────────────────┤
│          Call Stack            │              Heap               │
│  ┌──────────────────────────┐  │  ┌───────────────────────────┐  │
│  │ Frame 2: factorial(n=1)  │  │  │ Dynamic Arrays (NEWARRAY) │  │
│  │   V[0]=1, pc=0x000c      │  │  │   count, elt_size, *data  │  │
│  │   S: [1]                 │  │  └───────────────────────────┘  │
│  ├──────────────────────────┤  │  ┌───────────────────────────┐  │
│  │ Frame 1: factorial(n=2)  │  │  │ Struct Blocks (NEW)       │  │
│  │   V[0]=2, pc=0x000c      │  │  │   raw byte fields         │  │
│  │   S: [2]                 │  │  └───────────────────────────┘  │
│  ├──────────────────────────┤  │  ┌───────────────────────────┐  │
│  │ Frame 0: _c0_main()      │  │  │ Immutable String Pool     │  │
│  │   V[0]=0, pc=0x0002      │  │  │   null-terminated chars   │  │
│  │   S: []                  │  │  └───────────────────────────┘  │
│  └──────────────────────────┘  │                                 │
└────────────────────────────────┴─────────────────────────────────┘
```

### A. The Call Stack & Activation Records (Frames)
Every function call in C0VM instantiates an independent execution frame:
```c
struct frame {
    uint16_t  fn_id;    /* Index in function_pool */
    uint8_t  *P;        /* Pointer to bytecode array */
    uint16_t  pc;       /* Program counter */
    c0_value *V;        /* Local variables array: V[0..num_vars-1] */
    uint16_t  num_vars; /* Total local slots (parameters + local variables) */
    c0_value *S;        /* Operand stack */
    int32_t   sp;       /* Stack pointer (index of next free slot) */
    int32_t   s_size;   /* Current allocated capacity of S */
};
```
- When a function calls another (`INVOKESTATIC`), the caller pushes arguments onto its operand stack.
- The VM pops those arguments in reverse order, allocates a new frame, stores them into `V[0..num_args-1]`, and pushes the caller frame onto `call_stack`.
- When `RETURN` executes, the active frame is deallocated, the caller frame is restored, and the return value is pushed onto the caller's operand stack.

### B. The Operand Stack ($S$)
- A Last-In, First-Out (LIFO) stack of tagged values (`c0_value`).
- Tag indicates either `C0_INTEGER` (32-bit signed two's-complement) or `C0_POINTER` (generic 64-bit heap address).
- In our VM, stack overflow is prevented by dynamic doubling (`STACK_INIT_CAP` doubled on overflow).

### C. The Heap
- Holds memory with lifetimes exceeding the function that created them.
- **Structs (`NEW <s>`)**: Allocates `s` contiguous zeroed bytes. Fields are accessed by base address + byte offset (`AADDF`).
- **Arrays (`NEWARRAY <s>`)**: Stored as a runtime-bounded header:
  ```c
  typedef struct {
      int32_t  count;    /* Number of elements (bounds checked!) */
      int32_t  elt_size; /* Size of element in bytes */
      void    *data;     /* Contiguous element store */
  } c0_array;
  ```
- **Bounds Checking**: Every array access (`AADDS`) verifies $0 \le \text{index} < \text{count}$. Out-of-bounds access triggers an immediate safe abort via `longjmp`.

---

## 3. Formal Stack Transition Semantics

In programming language theory, stack machines are formally specified using transition rules on the operand stack $S$:

| Instruction | Stack Transition | Description |
|:---|:---|:---|
| `bipush b` | $S \to S, b$ | Push immediate byte sign-extended to 32 bits |
| `vload i` | $S \to S, V[i]$ | Push value from local variable $i$ |
| `vstore i` | $S, v \to S$ | Pop value and assign to $V[i]$ |
| `iadd` | $S, x, y \to S, (x + y)$ | Pop two operands, push arithmetic sum |
| `isub` | $S, x, y \to S, (x - y)$ | Pop two operands, push arithmetic difference |
| `imul` | $S, x, y \to S, (x \cdot y)$ | Pop two operands, push product |
| `idiv` | $S, x, y \to S, (x / y)$ | Truncating division toward zero (aborts if $y=0$ or INT_MIN / -1) |
| `newarray s` | $S, n \to S, a$ | Allocate array of $n$ elements of size $s$; push pointer $a$ |
| `aadds` | $S, a, i \to S, \&a[i]$ | Bounds check $0 \le i < a.\text{count}$, push address |
| `imstore` | $S, p, v \to S$ | Store 32-bit integer $v$ into memory cell at address $p$ |
| `imload` | $S, p \to S, *p$ | Load 32-bit integer from memory cell at address $p$ |

---

## 4. Instruction Dispatch: How Interpreters Run Fast

In virtual machine engineering, the dispatch loop is the hottest code path.

### Model 1: Switch-Based Dispatch (Implemented in C0VM)
```c
while (true) {
    uint8_t op = fetch_u8(fr);
    switch (op) {
        case IADD: ... break;
        case ISUB: ... break;
    }
}
```
- **Pros**: 100% standard ANSI C, portable to any compiler/OS, easily instrumented for tracing.
- **Performance Trade-off**: The switch statement compiles to a single indirect jump through a jump table. The CPU branch predictor faces heavy misprediction penalties because every opcode jumps back to the same shared loop header.

### Model 2: Direct Threaded Code (Computed Goto)
Supported by GCC and Clang via label addresses (`&&label`):
```c
static void* dispatch_table[] = { &&do_nop, &&do_bipush, &&do_iadd, ... };
#define DISPATCH() goto *dispatch_table[fetch_u8(fr)]

do_iadd:
    // execute iadd
    DISPATCH();

do_isub:
    // execute isub
    DISPATCH();
```
- **Why it's faster**: Each instruction ends with its own indirect branch directly to the next instruction's handler. CPU branch target buffers (BTB) can predict bytecode sequences (e.g. `vload` followed by `iadd`) with significantly higher accuracy, yielding 15–30% speedups.

---

## 5. Runtime Error Handling via `setjmp` / `longjmp`

C does not have built-in exception handling (`try` / `catch`). In a virtual machine, invalid user code (such as array out-of-bounds or division by zero) must **not** crash the host process with a segmentation fault.

### The C0VM Solution:
1. At the entry point (`main`), we initialize a jump buffer:
   ```c
   if (setjmp(c0vm_error_buf) == 0) {
       result = execute(bcf);
   } else {
       // Catches any runtime error anywhere deep in the call stack
       fprintf(stderr, "Execution safely aborted.\n");
       free_bc0_file(bcf);
       return EXIT_FAILURE;
   }
   ```
2. When any instruction encounters an invalid state:
   ```c
   #define c0_assert(cond, fmt, ...) \
       do { if (!(cond)) { fprintf(stderr, fmt, ##__VA_ARGS__); longjmp(c0vm_error_buf, 1); } } while (0)
   ```
3. This unwinds the entire C call stack instantly, restores control to `main`, prevents undefined behavior, and allows proper cleanup of host resources.

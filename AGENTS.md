# AGENTS.md — AI Agent Guide for `tiny-xtensa-cc`

This document provides definitive instructions, architecture specifications, language rules, and verification workflows for AI coding agents and autonomous pair programmers working with or extending the **`tiny-xtensa-cc`** compiler codebase.

---

## 1. Executive Summary & Core Invariants

`tiny-xtensa-cc` is a lightweight, zero-heap C99 & Embedded C++ compiler written in C targeting the **ESP32 (Xtensa LX6 / LX7 CALL0 ABI)** and outputting native **ELF32 Relocatable Object Files (`.o`)**.

### Non-Negotiable Architectural Invariants:
1. **Zero OS Dynamic Heap (`malloc`/`free`) during Compilation**:
   - All parser tokens, AST expressions, symbol records, string literals, and emitter buffers are allocated in a deterministic linear bump allocator (`Arena`).
   - Do **NOT** introduce raw `malloc()`, `calloc()`, `realloc()`, or `free()` inside compiler modules (`src/`). Always pass and use `Arena *arena` and `arena_alloc()`.
2. **Xtensa CALL0 ABI Compliance**:
   - Stack Pointer / Frame Pointer: **`a1`**, strictly **16-byte aligned**.
   - Return Address: **`a0`** (spilled at `a1 + frame_size - 4` in prologue).
   - Arguments & Return Value: **`a2`..`a7`** (up to 6 function arguments; primary return in `a2`).
   - Scratch Registers: **`a8`..`a15`** (caller-saved; scratch depth tracked in parser).
3. **Direct ELF32 Relocatable Object Emission**:
   - The compiler directly writes valid ELF32 binaries (`ET_REL`, `EM_XTENSA = 94 / 0x005E`).
   - Never generate temporary assembly files (`.s`) or depend on GNU `as`/`objcopy` in production compiler workflows.

---

## 2. Supported Language Features & Code Generation

When writing code to be compiled by `tiny-xtensa-cc` (or adding compiler features), refer to these supported syntax and semantics:

### C99 Core Language
- **Types**: `void`, `char`, `short`, `int`, `long`, `unsigned`, `signed`, `float`, `double`, `bool` (via `<stdbool.h>`), `<stdint.h>` integer types (`uint8_t`, `int16_t`, `uint32_t`, etc.).
- **Pointers & Arrays**: Multi-level pointer dereferencing (`*ptr`, `**ptr`), address-of (`&val`), stack-allocated arrays (`int arr[N];`), array subscripting (`arr[i]`), pointer arithmetic.
- **Structs, Unions & Bitfields**:
  - `struct Tag { ... };` and `union Tag { ... };` declarations and typedefs.
  - Member access via `.` and `->`.
  - Bitfields: `unsigned int field : width;` packed within 32-bit word boundaries with automatic masking and shifting on read/write.
- **Designated Initializers**:
  - Struct: `struct Point pt = { .y = 200, .x = 50 };`
  - Array: `int arr[8] = { [2] = 40, [5] = 60 };` (remaining elements zero-filled).
- **Control Flow**: `if` / `else`, `while`, `for (init; cond; step)`, `do { ... } while (cond);`, `switch (disc) { case C: ... default: ... }`, `break`, `continue`, `return`, ternary `? :`.
- **Operators**: Full arithmetic, bitwise (`&`, `|`, `^`, `~`, `<<`, `>>`), logical (`&&`, `||`, `!`), increment/decrement (`++`, `--`), compound assignments (`+=`, `-=`, `*=`, `/=`, `%=`, `&=`, `|=`, `^=`, `<<=`, `>>=`).

### Embedded C++ (EC++)
- **Classes & Access Modifiers**: `class Name { public: int x; private: int y; };`.
- **Class Methods & `this` Pointer**:
  - Method calls (`obj.method(args...)` or `ptr->method(args...)`) automatically pass `this` in register `a2`.
  - Inside methods, `this->member` is evaluated relative to `a2`.
- **Constructors**: `ClassName obj(arg1, arg2);` invokes mangled constructor `_ZN<len><Class><len><Class>E`.
- **References**: `Type &ref = var;` stores address and transparently dereferences on read/write.
- **Namespaces**: `namespace Math { int square(int x); }` and scoped calls `Math::square(5)`.
- **Linkage**: `extern "C" { ... }` suppresses name mangling.

### GCC Extensions & ESP-IDF Compatibility
- **Attributes (`skip_attributes`)**: Consumes `__attribute__((...))` (`packed`, `aligned(N)`, `section("...")`, `noinline`, `unused`, etc.), `__declspec(...)`, and `__extension__`.
- **Type Extraction**: `typeof(var)` and `__typeof__(var)` inside type declarations.
- **Statement Expressions**: `({ stmt; stmt; expr; })` evaluating to the final expression.
- **Built-in Intrinsics**: `__builtin_expect(expr, val)` and `__builtin_bswap32(val)`.
- **Inline Assembly**: `__asm__ volatile ("memw" / "isync" / "esync" / "rsil" / "nop" : ...);`.

### Preprocessor
- Object-like macros: `#define NAME <tokens/val>`
- Function-like macros: `#define MACRO(a, b) ((a) + (b))`
- Nested include files: `#include "filename.h"` (nested up to 8 levels, searched in `-I <dir>` paths).
- Conditionals: `#ifdef`, `#ifndef`, `#else`, `#endif`.

---

## 3. Toolchain & CLI Usage

### Compiling Code
```bash
# Compile a C source file into native ELF32 object file
bin/tiny-xtensa-cc -Iinclude drivers/gpio.c -o build/gpio.o

# Compile an inline code snippet
bin/tiny-xtensa-cc -c "int add(int a, int b) { return a + b; }" -o build/add.o

# Compile and display objdump disassembly
bin/tiny-xtensa-cc -d drivers/uart.c -o build/uart.o
```

### Linking with Xtensa GNU Toolchain
To produce an executable ELF for flashing or QEMU:
```bash
xtensa-esp32-elf-gcc \
    -nostdlib \
    -Wl,-Ttext=0x40080000 \
    -Wl,-Tdata=0x3FFE0000 \
    -Wl,-e,_start \
    tests/qemu_harness/entry.S \
    build/gpio.o \
    build/main.o \
    -o build/app.elf
```

---

## 4. Codebase Architecture

```
src/
├── arena.c / .h           # Linear memory bump allocator (zero malloc/free)
├── lexer.c / .h           # Tokenizer, macro table, nested file include stack
├── xtensa_emitter.c / .h  # Xtensa machine code encoder, CALL0 frame, literal pool, peephole optimizer
├── symbol_table.c / .h    # Lexical scopes, variable offsets, struct/bitfield/union layout engine
├── parser.c / .h          # Recursive descent parser, type parser, code generation, C++ handler
├── elf_writer.c / .h      # Direct ELF32 writer (.text, .rodata, .rela.text, .symtab, .strtab, .shstrtab)
└── main.c                 # CLI flags, file I/O, compilation driver
```

### Key Internal Functions:
- **`emit_finalize(Emitter *e, size_t *out_size)`** in `xtensa_emitter.c`:
  - Runs peephole optimizations.
  - Prepends a 4-byte jump over the literal + string pool if literals/strings exist.
  - Writes 32-bit literals and null-terminated strings into pool.
  - Applies L32R displacement fixups and adjusts relocation offsets.
- **`parse_type(Parser *p, DataType *out_type)`** in `parser.c`:
  - Parses scalar types, struct/union tags, typedef aliases, pointer depths, and arrays.
- **`compile_statement(Parser *p)`** in `parser.c`:
  - Handles variable declarations, initializers, control flow, inline assembly, and block scopes.
- **`compile_expression(Parser *p)`** in `parser.c`:
  - Operator precedence climbing expression evaluator leaving result in register `a2`.

---

## 5. Verification & Test Execution Protocol

Whenever making changes to `src/`:

```bash
# Step 1: Clean build the compiler and test binaries
make clean && make

# Step 2: Run all unit tests (Phases 1 through 14)
make test_phase1 test_phase2 test_phase3 test_phase5 test_phase6 test_phase7 test_phase8 test_phase10 test_phase11 test_phase12 test_phase13 test_phase14

# Step 3: Run the automated baremetal QEMU hardware test suite (36 tests in qemu-system-xtensa)
make test_qemu

# Step 4: Run the ESP32 Hello World UART test in QEMU
make test_hello

# Step 5: Run the ESP32 Peripheral Drivers verification in QEMU
make test_drivers

# Or run everything with a single command:
make test
```

---

## 6. Common Pitfalls & Agent Invariants

1. **Register Spilling**:
   - When evaluating binary operations (`a + b`), the left operand is evaluated into `a2` and saved via `save_left_operand(p)` (which uses caller-saved scratch registers `a8..a15` or stack spills).
   - Always pair `save_left_operand(p)` with `restore_left_operand(p)`.
2. **Function Pointer Relocations**:
   - Loading a function address uses `emit_load_literal(p->emitter, XTENSA_A2, 0)` and attaches an `R_XTENSA_32` relocation to the literal pool slot (`4 + lit_idx * 4`).
   - Do not emit `R_XTENSA_32` on 3-byte `movi` opcode instructions.
3. **Literal Pool Alignment**:
   - The literal pool is placed immediately at offset 4 (preceded by a 4-byte jump `j code_offset + pad`). Literals must remain 4-byte aligned.
4. **Hardware MMIO Writes**:
   - Hardware registers are MMIO and volatile. In drivers, use `volatile int *` or `volatile struct GpioDev *` and insert `__asm__ volatile ("memw");` to enforce write synchronization barriers.

# tiny-xtensa-cc

**A Lightweight, Zero-Heap C99 & Embedded C++ Compiler for ESP32 (Xtensa LX6 / LX7 CALL0 ABI) emitting Native ELF32 Relocatable Objects (`.o`).**

---

## Highlights

- **Direct ELF32 Relocatable Object Generation**: Emits standard `ET_REL` binary objects (`.text`, `.rodata`, `.rela.text`, `.symtab`, `.strtab`, `.shstrtab`) for `EM_XTENSA` without relying on intermediate assembly (`.s`) or GNU `as` / `objcopy`.
- **Zero OS Heap Allocation**: 100% deterministic arena bump-allocator (`Arena`). Zero dynamic `malloc()` / `free()` calls during compilation, eliminating OS heap fragmentation.
- **ESP32 Baremetal & ESP-IDF Compatibility**:
  - Full CALL0 ABI standard (`-mabi=call0`): `a1` frame pointer/SP (16-byte aligned), `a0` return address, `a2..a7` arguments & return values, `a8..a15` caller-saved scratch registers.
  - Struct bitfields (`field : N`) with 32-bit word boundary packing.
  - C99 designated initializers (`.field = val` and `[idx] = val`).
  - GCC extensions: `__attribute__((...))`, `__typeof__`, statement expressions `({ ... })`, inline assembly `__asm__ volatile ("memw" / "isync" / ...)`, and built-ins (`__builtin_expect`, `__builtin_bswap32`).
- **Embedded C++ (EC++) Support**:
  - Classes, methods with implicit `this` pointer parameter passing (`a2`), access specifiers (`public:`, `private:`).
  - Constructors (`ClassName obj(args...)`), l-value references (`Type &ref`), namespaces (`namespace Name { ... }`), and `extern "C"`.
  - Itanium-compatible C++ symbol name mangling (`_ZN<len><Class><len><Method>E`).
- **Hardware Peripheral Drivers (`drivers/`)**:
  - Direct MMIO hardware driver suite for ESP32 GPIO, UART0, Timers, and DPORT.
- **Automated QEMU Simulation**:
  - Fully automated baremetal test harness executing compiled code inside `qemu-system-xtensa` (ESP32 machine) with GDB inspection.

---

## Quick Start

### 1. Building the Compiler

```bash
make clean && make
```
This builds the binary `bin/tiny-xtensa-cc` and all unit test suites in `build/`.

### 2. Compiling C Source to Native `.o` Object

```bash
# Compile a C source file into an ELF32 object file
bin/tiny-xtensa-cc -Iinclude drivers/gpio.c -o build/gpio.o

# Inspect the generated ELF symbols and disassembly
xtensa-esp32-elf-objdump -d build/gpio.o
```

### 3. Linking with ESP-IDF / GNU Toolchain

```bash
xtensa-esp32-elf-gcc -nostdlib -Tesp32.ld entry.S build/gpio.o main.o -o firmware.elf
```

### 4. Running the Full Test Suite

```bash
# Run all unit tests, driver verification, and headless QEMU hardware emulation
make test
```

---

## Compiler CLI Usage

```text
tiny-xtensa-cc [options] <input-file>

Options:
  -o <file>        Specify output object file (default: a.out.o)
  -c <string>      Compile inline C code string directly
  -I <dir>         Add directory to header include search path
  -d, --disasm     Disassemble emitted ELF object using xtensa objdump
  -v, --version    Display compiler version and target ABI
  -h, --help       Show help message
```

---

## Language & Feature Matrix

| Feature Area | Supported Capabilities |
|---|---|
| **Types & Keywords** | `void`, `char`, `short`, `int`, `long`, `unsigned`, `signed`, `float`, `double`, `bool`, `struct`, `union`, `enum`, `typedef`, `class`, `public`, `private`, `protected`, `namespace`, `extern`, `volatile`, `const` |
| **Control Flow** | `if` / `else`, `while`, `for (init; cond; step)`, `do { ... } while (cond);`, `switch` / `case` / `default`, `break`, `continue`, `return`, ternary `? :` |
| **Expressions & Operators** | `+`, `-`, `*`, `/`, `%`, `&`, `|`, `^`, `~`, `!`, `<<`, `>>`, `==`, `!=`, `<`, `<=`, `>`, `>=`, `&&`, `||`, `++`, `--`, `+=`, `-=`, `*=`, `/=`, `%=`, `&=`, `|=`, `^=`, `<<=`, `>>=` |
| **Structs & Memory** | Member access (`.` and `->`), bitfields (`unsigned int f : 3;`), unions, designated initializers (`.x = 10`, `[3] = 42`), pointer arithmetic, array indexing `arr[i]` |
| **Functions & Calls** | Call-by-value, pointers, function pointers `int (*cb)(int)`, indirect calls (`CALLX0`), recursive functions, variable spills |
| **Preprocessor** | `#define NAME <val>`, function-like macros `#define M(a, b) ((a) + (b))`, `#include "file.h"` (nested up to 8 levels), `#ifdef`, `#ifndef`, `#else`, `#endif` |
| **GCC Extensions** | `__attribute__((packed, aligned, section, noinline))`, `__typeof__`, statement expressions `({ stmt; expr; })`, `__builtin_expect`, `__builtin_bswap32` |
| **Inline Assembly** | `__asm__ volatile ("memw" / "isync" / "esync" / "rsil" / "nop" : ...);` |
| **Embedded C++** | Class methods, implicit `this` pointer in `a2`, constructors, references (`int &ref`), namespaces (`Math::square`), `extern "C"` |

---

## Project Structure

```
tiny-xtensa-compiler/
├── bin/                       # Generated compiler executables (tiny-xtensa-cc)
├── build/                     # Object files, test binaries, and ELF outputs
├── drivers/                   # ESP32 hardware MMIO peripheral drivers
│   ├── gpio.c                 # GPIO driver implementation
│   ├── uart.c                 # UART0 driver implementation
│   └── timer.c                # Hardware 64-bit timer driver
├── examples/                  # Standalone runnable examples & QEMU scripts
│   ├── hello_world.c          # Native UART "Hello, World!"
│   ├── driver_demo.c          # Full GPIO/UART/Timer demo
│   └── run_hello_qemu.py      # QEMU runner script for Hello World
├── include/                   # Standard and driver header files
│   ├── stdbool.h              # C99 boolean definitions
│   ├── stdint.h               # C99 fixed-width integer types
│   └── drivers/               # Peripheral driver headers (esp32_regs.h, gpio.h, etc.)
├── src/                       # Compiler core sources
│   ├── arena.c / .h           # Zero-heap bump allocator
│   ├── lexer.c / .h           # Tokenizer & C preprocessor
│   ├── xtensa_emitter.c / .h  # Xtensa machine instruction encoder & peephole optimizer
│   ├── symbol_table.c / .h    # Scope & struct/bitfield/union layout engine
│   ├── parser.c / .h          # AST parser, code generator, and C++ handler
│   ├── elf_writer.c / .h      # Native ELF32 relocatable object file writer
│   └── main.c                 # Compiler CLI entry point
├── tests/                     # Unit test suites and verification harness
│   ├── test_phase1.c .. 14.c  # Phase-by-phase test suites
│   ├── test_drivers.py        # Automated hardware driver test script
│   └── qemu_harness/          # Headless QEMU baremetal ESP32 execution harness
│       ├── entry.S            # Baremetal entry point and DRAM stack initialization
│       ├── runner.c           # Test dispatcher
│       └── test_qemu.py       # Automated QEMU + GDB test runner (36 tests)
├── Makefile                   # Build rules and automated test execution
├── AGENTS.md                  # Comprehensive AI Agent development and integration guide
└── README.md                  # This file
```

---

## Verification & Metrics

- **Unit Test Coverage**: 14 dedicated test suites verifying lexing, instruction encoding, expression trees, control flow, arrays, structs, preprocessor, C99, C++, GCC extensions, and peephole optimization.
- **QEMU Baremetal Hardware Suite**: **36 / 36 automated runtime tests passing** under `qemu-system-xtensa` (ESP32 machine).
- **Executable Footprint**: Standalone binary is **~111 KB**, requiring **< 1 MB RAM** during compilation.

#!/usr/bin/env python3
import subprocess
import os
import sys
import time
import shutil
import re

def find_binary(name, candidate_paths):
    found = shutil.which(name)
    if found:
        return found
    for p in candidate_paths:
        if os.path.exists(p) and os.access(p, os.X_OK):
            return p
    return name

CC_COMPILER = os.path.abspath("bin/tiny-xtensa-cc")
XTENSA_GCC = find_binary("xtensa-esp32-elf-gcc", [
    "/home/okan/.espressif/tools/xtensa-esp-elf/esp-14.2.0_20241119/xtensa-esp-elf/bin/xtensa-esp32-elf-gcc",
    "/home/okan/.espressif/tools/xtensa-esp-elf/esp-12.2.0_20230208/xtensa-esp-elf/bin/xtensa-esp32-elf-gcc",
])
XTENSA_GDB = find_binary("xtensa-esp32-elf-gdb", [
    "/home/okan/.espressif/tools/xtensa-esp-elf-gdb/14.2_20240403/xtensa-esp-elf-gdb/bin/xtensa-esp32-elf-gdb",
    "/home/okan/.espressif/tools/xtensa-esp-elf-gdb/12.1_20221002/xtensa-esp-elf-gdb/bin/xtensa-esp32-elf-gdb",
])
QEMU_XTENSA = find_binary("qemu-system-xtensa", [
    "/home/okan/.espressif/tools/qemu-xtensa/esp_develop_9.0.0_20240606/qemu/bin/qemu-system-xtensa",
])

BUILD_DIR = os.path.abspath("build")
os.makedirs(BUILD_DIR, exist_ok=True)

# Generate test_header.h
with open(os.path.join(BUILD_DIR, "test_header.h"), "w") as f:
    f.write("""#define UART_BASE 0x3FF40000
#define MAGIC_NUM 42

struct Vector2D {
    int x;
    int y;
};
""")

print("=" * 60)
print(" Headless QEMU Execution Test Suite (ESP32 CALL0)")
print("=" * 60)
print(f"Tiny-Xtensa Compiler: {CC_COMPILER}")
print(f"Xtensa GCC:           {XTENSA_GCC}")
print(f"Xtensa GDB:           {XTENSA_GDB}")
print(f"QEMU:                 {QEMU_XTENSA}")
print("-" * 60)

# 1. Compile test programs directly into native ELF32 .o objects
test_programs = [
    ("test_math", "int test_math() { return (10 + 20) * 3 - (50 / 2); }"),
    ("sum_to_n", "int sum_to_n(int n) { int s = 0; int i = 1; while (i <= n) { s = s + i; i = i + 1; } return s; }"),
    ("fib", "int fib(int n) { if (n <= 1) return n; int a = 0; int b = 1; int i = 2; while (i <= n) { int c = a + b; a = b; b = c; i = i + 1; } return b; }"),
    ("test_array_sum", """
int test_array_sum() {
    int arr[5];
    arr[0] = 10;
    arr[1] = 20;
    arr[2] = 30;
    arr[3] = 40;
    arr[4] = 50;
    int sum = 0;
    int i = 0;
    while (i < 5) {
        sum = sum + arr[i];
        i = i + 1;
    }
    return sum;
}
"""),
    ("test_sort", """
int test_sort() {
    int a[4];
    a[0] = 42; a[1] = 12; a[2] = 88; a[3] = 5;
    int i = 0;
    while (i < 4) {
        int j = 0;
        while (j < 3) {
            if (a[j] > a[j + 1]) {
                int tmp = a[j];
                a[j] = a[j + 1];
                a[j + 1] = tmp;
            }
            j = j + 1;
        }
        i = i + 1;
    }
    return a[0] * 1000 + a[1] * 100 + a[2] * 10 + a[3];
}
"""),
    ("test_macro", """
#include "build/test_header.h"
int test_macro() {
    return MAGIC_NUM + 8;
}
"""),
    ("test_struct_dot", """
#include "build/test_header.h"
int test_struct_dot() {
    struct Vector2D v;
    v.x = 15;
    v.y = 25;
    return v.x + v.y;
}
"""),
    ("test_struct_arrow", """
#include "build/test_header.h"
int test_struct_arrow() {
    struct Vector2D v;
    struct Vector2D *p = &v;
    p->x = 100;
    p->y = 200;
    return p->x + p->y;
}
"""),
    ("test_uart_struct", """
#include "build/test_header.h"
struct UartDev {
    volatile int fifo;
};
int test_uart_struct() {
    struct UartDev *uart = (struct UartDev*)UART_BASE;
    return sizeof(struct Vector2D);
}
"""),
    ("test_for_sum", """
int test_for_sum() {
    int s = 0;
    int i;
    for (i = 1; i <= 10; i = i + 1) {
        s = s + i;
    }
    return s;
}
"""),
    ("test_break_continue", """
int test_break_continue() {
    int s = 0;
    int i;
    for (i = 0; i < 20; i = i + 1) {
        if (i == 5) {
            continue;
        }
        if (i == 10) {
            break;
        }
        s = s + i;
    }
    return s;
}
"""),
    ("test_nested_break", """
int test_nested_break() {
    int total = 0;
    int i;
    int j;
    for (i = 0; i < 3; i = i + 1) {
        for (j = 0; j < 10; j = j + 1) {
            if (j == 4) break;
            total = total + 1;
        }
    }
    return total;
}
"""),
    ("test_ternary", """
int test_ternary(int a, int b) {
    int max = (a > b) ? a : b;
    int sign = (max > 0) ? 1 : ((max < 0) ? -1 : 0);
    return max * 10 + sign;
}
"""),
    ("test_logical", """
int test_logical(int a, int b) {
    int res = 0;
    if (a > 0 && b > 0) {
        res = 1;
    }
    if (a < 0 || b < 0) {
        res = res + 10;
    }
    return res;
}
"""),
    ("test_compound", """
int test_compound() {
    int a = 100;
    a += 50;
    a -= 20;
    a *= 2;
    a /= 4;
    a %= 30;
    a <<= 2;
    a |= 3;
    a &= 31;
    a ^= 15;
    return a;
}
"""),
    ("test_inc_dec", """
int test_inc_dec() {
    int a = 10;
    ++a;
    a++;
    --a;
    a--;
    return a;
}
"""),
    ("test_do_while", """
int test_do_while() {
    int s = 0;
    int i = 1;
    do {
        s += i;
        i++;
    } while (i <= 10);
    return s;
}
"""),
    ("test_switch", """
int test_switch(int mode) {
    int result = 0;
    switch (mode) {
        case 1:
            result = 100;
            break;
        case 2:
            result = 200;
            break;
        case 3:
            result = 300;
            break;
        default:
            result = 999;
            break;
    }
    return result;
}
"""),
    ("test_c99_typedef", """
typedef int my_int_t;
typedef struct Point { int x; int y; } Point_t;
int test_c99_typedef() {
    my_int_t a = 42;
    Point_t pt;
    pt.x = a;
    pt.y = 100;
    return pt.x + pt.y;
}
"""),
    ("test_c99_enum", """
enum State { IDLE = 0, RUNNING = 10, HALTED = 20 };
int test_c99_enum(int s) {
    if (s == IDLE) return 1;
    if (s == RUNNING) return 2;
    if (s == HALTED) return 3;
    return 0;
}
"""),
    ("test_c99_bool", """
#include <stdbool.h>
int test_c99_bool(int val) {
    bool flag = true;
    if (val < 0) flag = false;
    return flag ? 100 : 200;
}
"""),
    ("test_c99_inits", """
struct Point { int x; int y; };
int test_c99_inits() {
    int arr[4] = { 10, 20, 30, 40 };
    struct Point p = { 100, 200 };
    return arr[0] + arr[1] + arr[2] + arr[3] + p.x + p.y;
}
"""),
    ("test_c99_stdint", """
#include <stdint.h>
int test_c99_stdint() {
    uint8_t a = 250;
    int16_t b = -50;
    uint32_t c = 1000;
    return c + (uint32_t)a + (uint32_t)b;
}
"""),
    ("test_cpp_class", """
class Counter {
private:
    int val;
public:
    int get_val();
};
int Counter::get_val() {
    return this->val;
}
int test_cpp_class() {
    Counter c;
    c.val = 42;
    return c.get_val();
}
"""),
    ("test_cpp_ref", """
int test_cpp_ref() {
    int x = 10;
    int &ref = x;
    ref = 88;
    return x;
}
"""),
    ("test_cpp_ctor", """
class Point {
public:
    int x;
    int y;
    int sum();
};
int Point::Point(int px, int py) {
    this->x = px;
    this->y = py;
    return 0;
}
int Point::sum() {
    return this->x + this->y;
}
int test_cpp_ctor() {
    Point pt(15, 25);
    return pt.sum();
}
"""),
    ("test_cpp_ns", """
namespace Math {
    int square(int x) {
        return x * x;
    }
}
int test_cpp_ns() {
    return Math::square(5);
}
"""),
    ("test_gcc_attr", """
struct __attribute__((packed, aligned(4))) NetPacket {
    int header;
    char payload;
};
__attribute__((noinline, section(".iram0.text"))) int test_gcc_attr() {
    struct NetPacket pkt;
    pkt.header = 100;
    pkt.payload = 42;
    return pkt.header + pkt.payload;
}
"""),
    ("test_stmt_expr", """
int test_stmt_expr() {
    int x = 10;
    int res = ({ int a = x * 2; int b = a + 10; b * 6 + 25; });
    return res;
}
"""),
    ("test_bitfield", """
struct GpioBits {
    unsigned int mode : 2;
    unsigned int enable : 1;
    unsigned int val : 5;
    unsigned int reg : 24;
};
int test_bitfield() {
    struct GpioBits g;
    g.mode = 3;
    g.enable = 1;
    g.val = 15;
    g.reg = 1000;
    return g.mode + g.enable + g.val + g.reg;
}
"""),
    ("test_union", """
union RegOverlay {
    int word;
    char byte0;
};
int test_union() {
    union RegOverlay u;
    u.word = 120;
    return u.byte0;
}
"""),
    ("test_designated", """
struct Config {
    int baud;
    int pin;
};
int test_designated() {
    struct Config cfg = { .pin = 50, .baud = 200 };
    int arr[8] = { [2] = 40, [5] = 60 };
    return cfg.baud + cfg.pin + arr[2] + arr[5];
}
"""),
    ("test_fn_ptr", """
int add_ten(int a) {
    return a + 10;
}
int test_fn_ptr() {
    int (*cb)(int) = add_ten;
    return cb(42);
}
""")
]

obj_files = []
for name, code in test_programs:
    obj_path = os.path.join(BUILD_DIR, f"{name}.o")
    cmd = [CC_COMPILER, "-c", code, "-o", obj_path]
    print(f"[Compiler] Compiling {name} -> {name}.o ... ", end="")
    res = subprocess.run(cmd, capture_output=True, text=True)
    if res.returncode != 0:
        print("FAILED")
        print(res.stderr)
        sys.exit(1)
    print(f"OK ({os.path.getsize(obj_path)} bytes)")
    obj_files.append(obj_path)

# 2. Link directly with tests/qemu_harness/entry.S and tests/qemu_harness/runner.c
entry_s = os.path.abspath("tests/qemu_harness/entry.S")
runner_c = os.path.abspath("tests/qemu_harness/runner.c")
runner_elf = os.path.join(BUILD_DIR, "qemu_runner.elf")

print(f"[Linker] Linking {runner_elf} ... ", end="")
link_cmd = [
    XTENSA_GCC,
    "-g",
    "-mabi=call0",
    "-nostdlib",
    "-Wl,-Ttext=0x40080000",
    "-Wl,-Tdata=0x3FFE0000",
    "-Wl,-e,_start",
    entry_s,
    runner_c
] + obj_files + [
    "-o",
    runner_elf
]
link_res = subprocess.run(link_cmd, capture_output=True, text=True)
if link_res.returncode != 0:
    print("FAILED")
    print(link_res.stderr)
    sys.exit(1)
print("OK")

# 3. Run in QEMU and inspect runtime values via GDB
print("[QEMU] Spawning headless QEMU ESP32 instance ... ", end="")
qemu_proc = subprocess.Popen(
    [QEMU_XTENSA, "-nographic", "-machine", "esp32", "-no-reboot", "-s", "-S", "-kernel", runner_elf],
    stdout=subprocess.PIPE,
    stderr=subprocess.PIPE
)
time.sleep(0.5)
print("RUNNING")

gdb_script = os.path.join(BUILD_DIR, "gdb_verify.txt")
with open(gdb_script, "w") as f:
    f.write("""
target remote :1234
set $pc = _start
break halt_loop
continue
p g_results
p g_done
quit
""")

print("[GDB] Connecting and inspecting execution results ...")
try:
    gdb_res = subprocess.run(
        [XTENSA_GDB, "-batch", "-x", gdb_script, runner_elf],
        capture_output=True,
        text=True,
        timeout=8
    )
finally:
    qemu_proc.kill()

output = gdb_res.stdout
print("-" * 60)
print(output.strip())
print("-" * 60)

# Expected outputs in results array
expected_tests = [
    ("test_math()", 65, 0),
    ("sum_to_n(10)", 55, 1),
    ("fib(7)", 13, 2),
    ("fib(0)", 0, 3),
    ("fib(1)", 1, 4),
    ("sum_to_n(1)", 1, 5),
    ("test_array_sum()", 150, 6),
    ("test_sort()", 6708, 7),
    ("test_macro()", 50, 8),
    ("test_struct_dot()", 40, 9),
    ("test_struct_arrow()", 300, 10),
    ("test_uart_struct()", 8, 11),
    ("test_for_sum()", 55, 12),
    ("test_break_continue()", 40, 13),
    ("test_nested_break()", 12, 14),
    ("test_ternary(7, 3)", 71, 15),
    ("test_logical(5, 10)", 1, 16),
    ("test_compound()", 24, 17),
    ("test_inc_dec()", 10, 18),
    ("test_do_while()", 55, 19),
    ("test_switch(2)", 200, 20),
    ("test_c99_typedef()", 142, 21),
    ("test_c99_enum(10)", 2, 22),
    ("test_c99_bool(5)", 100, 23),
    ("test_c99_inits()", 400, 24),
    ("test_c99_stdint()", 1200, 25),
    ("test_cpp_class()", 42, 26),
    ("test_cpp_ref()", 88, 27),
    ("test_cpp_ctor()", 40, 28),
    ("test_cpp_ns()", 25, 29),
    ("test_gcc_attr()", 142, 30),
    ("test_stmt_expr()", 205, 31),
    ("test_bitfield()", 1019, 32),
    ("test_union()", 120, 33),
    ("test_designated()", 350, 34),
    ("test_fn_ptr()", 52, 35),
]

match_results = re.search(r'\$1\s*=\s*\{([^}]+)\}', output)

all_passed = False
if match_results:
    raw_vals = [int(v.strip()) for v in match_results.group(1).split(',')]
    passed_count = 0
    for label, exp_val, idx in expected_tests:
        actual = raw_vals[idx]
        if actual == exp_val:
            print(f"  [PASS] {label:<22} = {actual} (expected {exp_val})")
            passed_count += 1
        else:
            print(f"  [FAIL] {label:<22} = {actual} (expected {exp_val})")
    
    if passed_count == len(expected_tests) and "$2 = 1" in output:
        all_passed = True

if all_passed:
    print("=" * 60)
    print(f" ALL {len(expected_tests)} QEMU RUNTIME VERIFICATION TESTS PASSED SUCCESSFULLY!")
    print("=" * 60)
    sys.exit(0)
else:
    print("=" * 60)
    print(" QEMU VERIFICATION FAILED")
    print("=" * 60)
    sys.exit(1)

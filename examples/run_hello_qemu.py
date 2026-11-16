#!/usr/bin/env python3
import subprocess
import os
import sys
import time
import shutil

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
    "/home/okan/.espressif/tools/xtensa-esp-elf/esp-12.2.0_20230208/xtensa-esp32-elf/bin/xtensa-esp32-elf-gcc",
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

print("=" * 60)
print(" Running Hello World on ESP32 in QEMU (Native ELF32 .o)")
print("=" * 60)
print(f"Compiler: {CC_COMPILER}")
print(f"GCC:      {XTENSA_GCC}")
print(f"GDB:      {XTENSA_GDB}")
print(f"QEMU:     {QEMU_XTENSA}")
print("-" * 60)

# 1. Compile examples/hello_world.c directly to native ELF32 relocatable object (.o)
src_c = os.path.abspath("examples/hello_world.c")
obj_path = os.path.join(BUILD_DIR, "hello_world.o")
print(f"[Compiler] Compiling {src_c} -> {obj_path} ... ", end="")
cmd = [CC_COMPILER, src_c, "-o", obj_path]
res = subprocess.run(cmd, capture_output=True, text=True)
if res.returncode != 0:
    print("FAILED")
    print(res.stderr)
    sys.exit(1)
print(f"OK ({os.path.getsize(obj_path)} bytes)")

# 2. Link ELF directly using xtensa-esp32-elf-gcc without objcopy or assembly wrappers
entry_s = os.path.abspath("examples/entry_hello.S")
runner_c = os.path.abspath("examples/runner_hello.c")
hello_elf = os.path.join(BUILD_DIR, "hello_world.elf")

print(f"[Linker] Linking {obj_path} directly into {hello_elf} ... ", end="")
link_cmd = [
    XTENSA_GCC,
    "-g",
    "-mabi=call0",
    "-nostdlib",
    "-Wl,-Ttext=0x40080000",
    "-Wl,-Tdata=0x3FFE0000",
    "-Wl,-e,_start",
    entry_s,
    runner_c,
    obj_path,
    "-o",
    hello_elf
]
link_res = subprocess.run(link_cmd, capture_output=True, text=True)
if link_res.returncode != 0:
    print("FAILED")
    print(link_res.stderr)
    sys.exit(1)
print("OK")

# 3. Run in QEMU and capture UART serial output + return code
print("[QEMU] Launching QEMU ESP32 execution ...")
qemu_proc = subprocess.Popen(
    [QEMU_XTENSA, "-nographic", "-machine", "esp32", "-no-reboot", "-s", "-S", "-kernel", hello_elf],
    stdout=subprocess.PIPE,
    stderr=subprocess.PIPE
)
time.sleep(0.5)

gdb_script = os.path.join(BUILD_DIR, "gdb_hello.txt")
with open(gdb_script, "w") as f:
    f.write("""
target remote :1234
set $pc = _start
break halt_loop
continue
p g_hello_ret
quit
""")

try:
    gdb_res = subprocess.run(
        [XTENSA_GDB, "-batch", "-x", gdb_script, hello_elf],
        capture_output=True,
        text=True,
        timeout=8
    )
finally:
    time.sleep(0.2)
    qemu_proc.kill()
    qemu_out, qemu_err = qemu_proc.communicate()

uart_output = qemu_out.decode('utf-8', errors='replace')
print("-" * 60)
print("UART Output Captured from QEMU:")
print(repr(uart_output))
print(f"Console Display: {uart_output.strip()}")
print("-" * 60)
print("GDB Inspection:")
print(gdb_res.stdout.strip())
print("-" * 60)

if "$1 = 14" in gdb_res.stdout:
    print("SUCCESS: hello_world() returned status 14 (string length)!")
    sys.exit(0)
else:
    print("FAILED: Return value mismatch")
    sys.exit(1)

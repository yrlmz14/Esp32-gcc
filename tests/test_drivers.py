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
DRIVERS_BUILD = os.path.join(BUILD_DIR, "drivers")
os.makedirs(DRIVERS_BUILD, exist_ok=True)

print("=" * 60)
print(" ESP32 Peripheral Drivers Execution Test Suite")
print("=" * 60)
print(f"Tiny-Xtensa Compiler: {CC_COMPILER}")
print(f"Xtensa GCC:           {XTENSA_GCC}")
print(f"Xtensa GDB:           {XTENSA_GDB}")
print(f"QEMU:                 {QEMU_XTENSA}")
print("-" * 60)

# 1. Compile driver modules using tiny-xtensa-cc
drivers = [
    ("gpio", "drivers/gpio.c", os.path.join(DRIVERS_BUILD, "gpio.o")),
    ("uart", "drivers/uart.c", os.path.join(DRIVERS_BUILD, "uart.o")),
    ("timer", "drivers/timer.c", os.path.join(DRIVERS_BUILD, "timer.o")),
]

driver_objs = []
for name, src, out_obj in drivers:
    cmd = [CC_COMPILER, "-I.", src, "-o", out_obj]
    print(f"[Compiler] Compiling {src} -> {out_obj} ... ", end="")
    res = subprocess.run(cmd, capture_output=True, text=True)
    if res.returncode != 0:
        print("FAILED")
        print(res.stderr)
        sys.exit(1)
    print(f"OK ({os.path.getsize(out_obj)} bytes)")
    driver_objs.append(out_obj)

# 2. Compile examples/driver_demo.c using tiny-xtensa-cc
demo_src = os.path.abspath("examples/driver_demo.c")
demo_obj = os.path.join(BUILD_DIR, "driver_demo.o")
cmd = [CC_COMPILER, "-I.", demo_src, "-o", demo_obj]
print(f"[Compiler] Compiling {demo_src} -> {demo_obj} ... ", end="")
res = subprocess.run(cmd, capture_output=True, text=True)
if res.returncode != 0:
    print("FAILED")
    print(res.stderr)
    sys.exit(1)
print(f"OK ({os.path.getsize(demo_obj)} bytes)")

# 3. Link executable ELF
entry_s = os.path.abspath("examples/entry_driver_demo.S")
demo_elf = os.path.join(BUILD_DIR, "driver_demo.elf")
print(f"[Linker] Linking {demo_elf} ... ", end="")
link_cmd = [
    XTENSA_GCC,
    "-g",
    "-mabi=call0",
    "-nostdlib",
    "-Wl,-Ttext=0x3FFE0000",
    "-Wl,-e,_start",
    entry_s,
    demo_obj
] + driver_objs + [
    "-o",
    demo_elf
]
link_res = subprocess.run(link_cmd, capture_output=True, text=True)
if link_res.returncode != 0:
    print("FAILED")
    print(link_res.stderr)
    sys.exit(1)
print("OK")

# 4. Run in QEMU and capture UART output & register state
print("[QEMU] Spawning headless QEMU ESP32 instance ... ", end="")
qemu_proc = subprocess.Popen(
    [QEMU_XTENSA, "-nographic", "-machine", "esp32", "-no-reboot", "-s", "-S", "-kernel", demo_elf],
    stdout=subprocess.PIPE,
    stderr=subprocess.PIPE
)
time.sleep(0.5)
print("RUNNING")

gdb_script = os.path.join(BUILD_DIR, "gdb_driver_demo.txt")
with open(gdb_script, "w") as f:
    f.write("""
target remote :1234
set $pc = _start
break halt_loop
continue
p/d $a2
quit
""")

print("[GDB] Inspecting execution and waiting for halt_loop ...")
try:
    gdb_res = subprocess.run(
        [XTENSA_GDB, "-batch", "-x", gdb_script, demo_elf],
        capture_output=True,
        text=True,
        timeout=10
    )
finally:
    qemu_proc.kill()

stdout_qemu, _ = qemu_proc.communicate()
uart_text = stdout_qemu.decode('latin1', errors='ignore')

print("-" * 60)
print("UART Output Captured from QEMU:")
print(repr(uart_text))
print("Console Display:")
print(uart_text.strip())
print("-" * 60)

gdb_output = gdb_res.stdout
print("GDB Output:")
print(gdb_output.strip())
print("-" * 60)

# Verifications
assert "ESP32 Driver Demo Running!" in uart_text, "Missing UART banner"
assert "GPIO 2 Status: 1" in uart_text, "Missing GPIO initial status"
assert "Hex Value: 0x0000002A" in uart_text, "Missing Hex formatting output"
assert "GPIO 2 After Toggle: 0" in uart_text, "Missing GPIO toggled status"
assert "$1 = 1" in gdb_output, "run_driver_demo() did not return 1 in a2"

print("=" * 60)
print(" ALL ESP32 DRIVER VERIFICATIONS PASSED SUCCESSFULLY IN QEMU!")
print("=" * 60)
sys.exit(0)

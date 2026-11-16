CC ?= gcc
CFLAGS ?= -Wall -Wextra -O2 -Isrc -Iwin32_emu/include
BUILD_DIR ?= build
BIN_DIR ?= bin

SRCS = src/arena.c src/lexer.c src/xtensa_emitter.c src/symbol_table.c src/parser.c src/elf_writer.c
OBJS = $(patsubst src/%.c, $(BUILD_DIR)/%.o, $(SRCS))

MAIN_SRC = src/main.c
COMPILER_BIN = $(BIN_DIR)/tiny-xtensa-cc

TEST1_SRCS = tests/test_phase1.c
TEST1_BIN = $(BUILD_DIR)/test_phase1

TEST2_SRCS = tests/test_phase2.c
TEST2_BIN = $(BUILD_DIR)/test_phase2

TEST3_SRCS = tests/test_phase3.c
TEST3_BIN = $(BUILD_DIR)/test_phase3

TEST5_SRCS = tests/test_phase5.c
TEST5_BIN = $(BUILD_DIR)/test_phase5

TEST6_SRCS = tests/test_phase6.c
TEST6_BIN = $(BUILD_DIR)/test_phase6

TEST7_SRCS = tests/test_phase7.c
TEST7_BIN = $(BUILD_DIR)/test_phase7

TEST8_SRCS = tests/test_phase8.c
TEST8_BIN = $(BUILD_DIR)/test_phase8

TEST10_SRCS = tests/test_phase10.c
TEST10_BIN = $(BUILD_DIR)/test_phase10

TEST11_SRCS = tests/test_phase11.c
TEST11_BIN = $(BUILD_DIR)/test_phase11

TEST12_SRCS = tests/test_phase12.c
TEST12_BIN = $(BUILD_DIR)/test_phase12

TEST13_SRCS = tests/test_phase13.c
TEST13_BIN = $(BUILD_DIR)/test_phase13

TEST14_SRCS = tests/test_phase14.c
TEST14_BIN = $(BUILD_DIR)/test_phase14

WIN32_EMU_COMMON_SRCS = win32_emu/src/pe_loader.c win32_emu/src/win32_hle.c win32_emu/src/on_device_cache.c win32_emu/src/x86_jit.c win32_emu/src/framebuffer.c win32_emu/src/display_drv.c win32_emu/src/gdi32_hle.c win32_emu/src/gl_soft.c win32_emu/src/co5300_qspi.c win32_emu/src/ft3168_touch.c win32_emu/src/axp2101_pmu.c win32_emu/src/win32_cards_hle.c win32_emu/src/watch_ui.c win32_emu/src/advapi32_hle.c win32_emu/src/shell32_hle.c win32_emu/src/comctl32_hle.c win32_emu/src/win95_shell_stubs.c win32_emu/src/win95_icons.c win32_emu/src/pcf85063_rtc.c win32_emu/src/stillalive_data.c win32_emu/src/stillalive_app.c win32_emu/src/audio_driver.c win32_emu/src/generative_audio.c win32_emu/src/siren_inr.c win32_emu/src/riscv_linux_hle.c win32_emu/src/cyber_sentinel.c

TEST_WIN32_SRCS = tests/test_win32_emu.c $(WIN32_EMU_COMMON_SRCS)
TEST_WIN32_BIN = $(BUILD_DIR)/test_win32_emu

TEST_REAL_EXE_SRCS = tests/test_run_real_exe.c $(WIN32_EMU_COMMON_SRCS)
TEST_REAL_EXE_BIN = $(BUILD_DIR)/test_run_real_exe

TEST_GDI_SRCS = tests/test_framebuffer_gdi.c $(WIN32_EMU_COMMON_SRCS)
TEST_GDI_BIN = $(BUILD_DIR)/test_framebuffer_gdi

TEST_GL_SRCS = tests/test_gl_soft.c $(WIN32_EMU_COMMON_SRCS)
TEST_GL_BIN = $(BUILD_DIR)/test_gl_soft

TEST_WAVESHARE_SRCS = tests/test_waveshare_amoled.c $(WIN32_EMU_COMMON_SRCS)
TEST_WAVESHARE_BIN = $(BUILD_DIR)/test_waveshare_amoled

TEST_RECOMPILER_SRCS = src/x86_to_xtensa.c
TEST_RECOMPILER_BIN = $(BUILD_DIR)/test_recompiler

AOT_COMPILE_SRCS = tools/aot_compile_exe.c $(WIN32_EMU_COMMON_SRCS)
AOT_COMPILE_BIN = $(BUILD_DIR)/aot_compile_exe

TEST_EXPLORER_SRCS = tests/test_run_explorer.c $(WIN32_EMU_COMMON_SRCS)
TEST_EXPLORER_BIN = $(BUILD_DIR)/test_run_explorer

TEST_RISCV_SRCS = tests/test_riscv_linux_hle.c $(WIN32_EMU_COMMON_SRCS)
TEST_RISCV_BIN = $(BUILD_DIR)/test_riscv_linux_hle

PC_EMULATOR_SRCS = tools/pc_watch_emulator.c $(WIN32_EMU_COMMON_SRCS)
PC_EMULATOR_BIN = $(BUILD_DIR)/pc_watch_emulator.exe

.PHONY: all test test_phase1 test_phase2 test_phase3 test_phase5 test_phase6 test_phase7 test_phase8 test_phase10 test_phase11 test_phase12 test_phase13 test_phase14 test_win32_emu test_real_exe test_gdi test_gl test_waveshare test_recompiler test_explorer test_riscv_linux test_qemu test_hello test_drivers drivers pc_emulator run_pc clean

all: $(COMPILER_BIN) $(TEST1_BIN) $(TEST2_BIN) $(TEST3_BIN) $(TEST5_BIN) $(TEST6_BIN) $(TEST7_BIN) $(TEST8_BIN) $(TEST10_BIN) $(TEST11_BIN) $(TEST12_BIN) $(TEST13_BIN) $(TEST14_BIN) $(TEST_WIN32_BIN) $(TEST_REAL_EXE_BIN) $(TEST_GDI_BIN) $(TEST_GL_BIN) $(TEST_WAVESHARE_BIN) $(AOT_COMPILE_BIN) $(TEST_EXPLORER_BIN) $(TEST_RISCV_BIN) $(PC_EMULATOR_BIN) drivers

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BIN_DIR):
	mkdir -p $(BIN_DIR)

$(BUILD_DIR)/%.o: src/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(COMPILER_BIN): $(OBJS) $(MAIN_SRC) | $(BIN_DIR)
	$(CC) $(CFLAGS) $(OBJS) $(MAIN_SRC) -o $@

$(TEST1_BIN): $(BUILD_DIR)/arena.o $(BUILD_DIR)/lexer.o $(TEST1_SRCS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(BUILD_DIR)/arena.o $(BUILD_DIR)/lexer.o $(TEST1_SRCS) -o $@

$(TEST2_BIN): $(BUILD_DIR)/arena.o $(BUILD_DIR)/lexer.o $(BUILD_DIR)/xtensa_emitter.o $(TEST2_SRCS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(BUILD_DIR)/arena.o $(BUILD_DIR)/lexer.o $(BUILD_DIR)/xtensa_emitter.o $(TEST2_SRCS) -o $@

$(TEST3_BIN): $(OBJS) $(TEST3_SRCS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(OBJS) $(TEST3_SRCS) -o $@

$(TEST5_BIN): $(OBJS) $(TEST5_SRCS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(OBJS) $(TEST5_SRCS) -o $@

$(TEST6_BIN): $(OBJS) $(TEST6_SRCS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(OBJS) $(TEST6_SRCS) -o $@

$(TEST7_BIN): $(OBJS) $(TEST7_SRCS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(OBJS) $(TEST7_SRCS) -o $@

$(TEST8_BIN): $(OBJS) $(TEST8_SRCS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(OBJS) $(TEST8_SRCS) -o $@

$(TEST10_BIN): $(OBJS) $(TEST10_SRCS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(OBJS) $(TEST10_SRCS) -o $@

$(TEST11_BIN): $(OBJS) $(TEST11_SRCS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(OBJS) $(TEST11_SRCS) -o $@

$(TEST12_BIN): $(OBJS) $(TEST12_SRCS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(OBJS) $(TEST12_SRCS) -o $@

$(TEST13_BIN): $(OBJS) $(TEST13_SRCS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(OBJS) $(TEST13_SRCS) -o $@

$(TEST14_BIN): $(OBJS) $(TEST14_SRCS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(OBJS) $(TEST14_SRCS) -o $@

$(TEST_WIN32_BIN): $(BUILD_DIR)/arena.o $(BUILD_DIR)/xtensa_emitter.o $(TEST_WIN32_SRCS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -Iwin32_emu/include $(BUILD_DIR)/arena.o $(BUILD_DIR)/xtensa_emitter.o $(TEST_WIN32_SRCS) -lm -o $@

$(TEST_REAL_EXE_BIN): $(BUILD_DIR)/arena.o $(BUILD_DIR)/xtensa_emitter.o $(TEST_REAL_EXE_SRCS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -Iwin32_emu/include $(BUILD_DIR)/arena.o $(BUILD_DIR)/xtensa_emitter.o $(TEST_REAL_EXE_SRCS) -lm -o $@

$(TEST_GDI_BIN): $(BUILD_DIR)/arena.o $(BUILD_DIR)/xtensa_emitter.o $(TEST_GDI_SRCS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -Iwin32_emu/include $(BUILD_DIR)/arena.o $(BUILD_DIR)/xtensa_emitter.o $(TEST_GDI_SRCS) -lm -o $@

$(TEST_GL_BIN): $(BUILD_DIR)/arena.o $(BUILD_DIR)/xtensa_emitter.o $(TEST_GL_SRCS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -Iwin32_emu/include $(BUILD_DIR)/arena.o $(BUILD_DIR)/xtensa_emitter.o $(TEST_GL_SRCS) -lm -o $@

$(TEST_WAVESHARE_BIN): $(BUILD_DIR)/arena.o $(BUILD_DIR)/xtensa_emitter.o $(TEST_WAVESHARE_SRCS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -Iwin32_emu/include $(BUILD_DIR)/arena.o $(BUILD_DIR)/xtensa_emitter.o $(TEST_WAVESHARE_SRCS) -lm -o $@

$(TEST_RECOMPILER_BIN): $(TEST_RECOMPILER_SRCS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -DRECOMPILER_TEST_MAIN $(TEST_RECOMPILER_SRCS) -o $@

$(AOT_COMPILE_BIN): $(BUILD_DIR)/arena.o $(BUILD_DIR)/xtensa_emitter.o $(AOT_COMPILE_SRCS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -Iwin32_emu/include $(BUILD_DIR)/arena.o $(BUILD_DIR)/xtensa_emitter.o $(AOT_COMPILE_SRCS) -lm -o $@

$(TEST_EXPLORER_BIN): $(BUILD_DIR)/arena.o $(BUILD_DIR)/xtensa_emitter.o $(TEST_EXPLORER_SRCS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -Iwin32_emu/include $(BUILD_DIR)/arena.o $(BUILD_DIR)/xtensa_emitter.o $(TEST_EXPLORER_SRCS) -lm -o $@

$(TEST_RISCV_BIN): $(BUILD_DIR)/arena.o $(BUILD_DIR)/xtensa_emitter.o $(TEST_RISCV_SRCS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -Iwin32_emu/include $(BUILD_DIR)/arena.o $(BUILD_DIR)/xtensa_emitter.o $(TEST_RISCV_SRCS) -lm -o $@

$(PC_EMULATOR_BIN): $(BUILD_DIR)/arena.o $(BUILD_DIR)/xtensa_emitter.o $(PC_EMULATOR_SRCS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -Iwin32_emu/include $(BUILD_DIR)/arena.o $(BUILD_DIR)/xtensa_emitter.o $(PC_EMULATOR_SRCS) -lgdi32 -luser32 -lm -o $@

pc_emulator: $(PC_EMULATOR_BIN)

run_pc: $(PC_EMULATOR_BIN)
	@./$(PC_EMULATOR_BIN)

drivers: $(COMPILER_BIN)
	@if not exist $(BUILD_DIR)\drivers mkdir $(BUILD_DIR)\drivers
	@$(COMPILER_BIN) -I. drivers/gpio.c -o $(BUILD_DIR)/drivers/gpio.o
	@$(COMPILER_BIN) -I. drivers/uart.c -o $(BUILD_DIR)/drivers/uart.o
	@$(COMPILER_BIN) -I. drivers/timer.c -o $(BUILD_DIR)/drivers/timer.o

test_phase1: $(TEST1_BIN)
	@echo "Running Phase 1 tests..."
	@./$(TEST1_BIN)

test_phase2: $(TEST2_BIN)
	@echo "Running Phase 2 tests..."
	@./$(TEST2_BIN)

test_phase3: $(TEST3_BIN)
	@echo "Running Phase 3 tests..."
	@./$(TEST3_BIN)

test_phase5: $(TEST5_BIN)
	@echo "Running Phase 5 tests..."
	@./$(TEST5_BIN)

test_phase6: $(TEST6_BIN)
	@echo "Running Phase 6 tests..."
	@./$(TEST6_BIN)

test_phase7: $(TEST7_BIN)
	@echo "Running Phase 7 tests..."
	@./$(TEST7_BIN)

test_phase8: $(TEST8_BIN)
	@echo "Running Phase 8 tests..."
	@./$(TEST8_BIN)

test_phase10: $(TEST10_BIN)
	@echo "Running Phase 10 tests..."
	@./$(TEST10_BIN)

test_phase11: $(TEST11_BIN)
	@echo "Running Phase 11 tests..."
	@./$(TEST11_BIN)

test_phase12: $(TEST12_BIN)
	@echo "Running Phase 12 tests..."
	@./$(TEST12_BIN)

test_phase13: $(TEST13_BIN)
	@echo "Running Phase 13 tests..."
	@./$(TEST13_BIN)

test_phase14: $(TEST14_BIN)
	@echo "Running Phase 14 tests..."
	@./$(TEST14_BIN)

test_win32_emu: $(TEST_WIN32_BIN)
	@echo "Running Win32 HLE & JIT tests..."
	@./$(TEST_WIN32_BIN)

test_real_exe: $(TEST_REAL_EXE_BIN)
	@echo "Running Genuine Windows MinGW PE32 Binary in Emulator..."
	@./$(TEST_REAL_EXE_BIN) samples/real_mingw_app.exe

test_gdi: $(TEST_GDI_BIN)
	@echo "Running Framebuffer & GDI32 2D Graphics tests..."
	@./$(TEST_GDI_BIN)

test_gl: $(TEST_GL_BIN)
	@echo "Running Software OpenGL 3D Rasterizer tests..."
	@./$(TEST_GL_BIN)

test_waveshare: $(TEST_WAVESHARE_BIN)
	@echo "Running Waveshare ESP32-S3 2.06\" AMOLED Touch Watch tests..."
	@./$(TEST_WAVESHARE_BIN)

test_recompiler: $(TEST_RECOMPILER_BIN)
	@echo "Running x86-32 to Xtensa LX7 Binary Recompiler tests..."
	@./$(TEST_RECOMPILER_BIN)

test_explorer: $(AOT_COMPILE_BIN) $(TEST_EXPLORER_BIN)
	@echo "Running AOT Compiler & Execution Verification for Windows 95 Explorer..."
	@./$(AOT_COMPILE_BIN) "c:/Users/Okan Yorulmaz/Downloads/explorerwin95.EXE" $(BUILD_DIR)/explorer_aot.bin $(BUILD_DIR)/explorer_aot_blocks.c
	@./$(TEST_EXPLORER_BIN) "c:/Users/Okan Yorulmaz/Downloads/explorerwin95.EXE" $(BUILD_DIR)/explorer_aot.bin

test_riscv_linux: $(TEST_RISCV_BIN)
	@echo "Running Linux HLE & RISC-V (RV32I/M) Interpreter tests..."
	@./$(TEST_RISCV_BIN)

test_qemu: $(COMPILER_BIN)
	@python3 tests/qemu_harness/test_qemu.py

test_hello: $(COMPILER_BIN)
	@python3 examples/run_hello_qemu.py

test_drivers: $(COMPILER_BIN) drivers
	@python3 tests/test_drivers.py

test: test_phase1 test_phase2 test_phase3 test_phase5 test_phase6 test_phase7 test_phase8 test_phase10 test_phase11 test_phase12 test_phase13 test_phase14 test_win32_emu test_real_exe test_gdi test_gl test_waveshare test_recompiler test_explorer test_riscv_linux test_qemu test_hello test_drivers

clean:
	rm -rf $(BUILD_DIR) $(BIN_DIR)

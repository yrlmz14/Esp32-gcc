#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>

#include "arena.h"
#include "xtensa_emitter.h"

static int g_tests_run = 0;
static int g_tests_passed = 0;

#define TEST(name) static void name(void)
#define RUN_TEST(name) do { \
    g_tests_run++; \
    printf("Running %-45s ... ", #name); \
    name(); \
    g_tests_passed++; \
    printf("PASSED\n"); \
} while (0)

#define ASSERT(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "\nAssertion failed at %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        exit(1); \
    } \
} while (0)

static const char *get_objdump_path(void) {
    const char *env = getenv("OBJDUMP");
    if (env && access(env, X_OK) == 0) {
        return env;
    }

#ifndef _WIN32
    if (system("which xtensa-esp32-elf-objdump > /dev/null 2>&1") == 0) {
        return "xtensa-esp32-elf-objdump";
    }
#endif

    const char *candidates[] = {
        "/home/okan/.espressif/tools/xtensa-esp-elf/esp-14.2.0_20241119/xtensa-esp-elf/bin/xtensa-esp32-elf-objdump",
        "/home/okan/.espressif/tools/xtensa-esp32-elf/esp-12.2.0_20230208/xtensa-esp32-elf/bin/xtensa-esp32-elf-objdump",
    };
    for (size_t i = 0; i < sizeof(candidates)/sizeof(candidates[0]); i++) {
        if (access(candidates[i], X_OK) == 0) {
            return candidates[i];
        }
    }
#ifdef _WIN32
    return "wsl /home/okan/.espressif/tools/xtensa-esp-elf/esp-14.2.0_20241119/xtensa-esp-elf/bin/xtensa-esp32-elf-objdump";
#else
    return "xtensa-esp32-elf-objdump";
#endif
}

static void verify_disassembly(const uint8_t *code, size_t size, size_t start_addr,
                               const char *bin_filename, const char **expected_substrings,
                               size_t num_expected) {
    FILE *f = fopen(bin_filename, "wb");
    ASSERT(f != NULL);
    size_t written = fwrite(code, 1, size, f);
    ASSERT(written == size);
    fclose(f);

    char cmd[1024];
    if (start_addr > 0) {
        snprintf(cmd, sizeof(cmd), "%s -D -b binary -m xtensa --start-address=%zu %s 2>&1",
                 get_objdump_path(), start_addr, bin_filename);
    } else {
        snprintf(cmd, sizeof(cmd), "%s -D -b binary -m xtensa %s 2>&1",
                 get_objdump_path(), bin_filename);
    }

    FILE *p = popen(cmd, "r");
    ASSERT(p != NULL);

    char output[8192];
    size_t total_read = 0;
    while (!feof(p) && total_read < sizeof(output) - 1) {
        size_t n = fread(output + total_read, 1, sizeof(output) - 1 - total_read, p);
        total_read += n;
    }
    output[total_read] = '\0';
    int status = pclose(p);
    ASSERT(status == 0);

    // Ensure objdump did not report illegal instructions
    if (strstr(output, "<illegal>")) {
        fprintf(stderr, "\nDisassembly contains <illegal> instructions!\n%s\n", output);
        exit(1);
    }

    // Verify all expected opcodes and operands
    for (size_t i = 0; i < num_expected; i++) {
        if (!strstr(output, expected_substrings[i])) {
            fprintf(stderr, "\nMissing expected substring '%s' in objdump output:\n%s\n",
                    expected_substrings[i], output);
            exit(1);
        }
    }
}

// ============================================================================
// Tests
// ============================================================================

TEST(test_sequence_a_return_constant) {
    Arena a;
    ASSERT(arena_init(&a, 1024));

    Emitter e;
    ASSERT(emit_init(&e, &a, 128));

    // movi a2, 42
    // ret.n
    emit_movi(&e, XTENSA_A2, 42);
    emit_ret_n(&e);

    const char *expected[] = {
        "movi\ta2, 42",
        "ret.n"
    };

    verify_disassembly(e.code, e.code_size, 0, "build/test_seq_a.bin",
                       expected, sizeof(expected)/sizeof(expected[0]));

    arena_free(&a);
}

TEST(test_sequence_b_call0_arithmetic) {
    Arena a;
    ASSERT(arena_init(&a, 1024));

    Emitter e;
    ASSERT(emit_init(&e, &a, 128));

    // int add(int a, int b) { return a + b; }
    // add a2, a2, a3
    // ret.n
    emit_add(&e, XTENSA_A2, XTENSA_A2, XTENSA_A3);
    emit_ret_n(&e);

    const char *expected[] = {
        "add\ta2, a2, a3",
        "ret.n"
    };

    verify_disassembly(e.code, e.code_size, 0, "build/test_seq_b.bin",
                       expected, sizeof(expected)/sizeof(expected[0]));

    arena_free(&a);
}

TEST(test_sequence_c_stack_frame_spill_load) {
    Arena a;
    ASSERT(arena_init(&a, 1024));

    Emitter e;
    ASSERT(emit_init(&e, &a, 128));

    // Prologue (frame_size = 32, 16-byte aligned)
    emit_prologue(&e, 32);

    // Spills to stack frame
    emit_s32i(&e, XTENSA_A2, XTENSA_A1, 0);
    emit_s32i(&e, XTENSA_A3, XTENSA_A1, 4);

    // Reloads
    emit_l32i(&e, XTENSA_A8, XTENSA_A1, 0);
    emit_l32i(&e, XTENSA_A9, XTENSA_A1, 4);

    // Arithmetic
    emit_add(&e, XTENSA_A2, XTENSA_A8, XTENSA_A9);

    // Epilogue
    emit_epilogue(&e, 32);

    const char *expected[] = {
        "addi\ta1, a1, -32",
        "s32i\ta0, a1, 28",
        "s32i\ta2, a1, 0",
        "s32i\ta3, a1, 4",
        "l32i\ta8, a1, 0",
        "l32i\ta9, a1, 4",
        "add\ta2, a8, a9",
        "l32i\ta0, a1, 28",
        "addi\ta1, a1, 32",
        "ret.n"
    };

    verify_disassembly(e.code, e.code_size, 0, "build/test_seq_c.bin",
                       expected, sizeof(expected)/sizeof(expected[0]));

    arena_free(&a);
}

TEST(test_sequence_d_density_instructions) {
    Arena a;
    ASSERT(arena_init(&a, 1024));

    Emitter e;
    ASSERT(emit_init(&e, &a, 128));

    emit_mov_n(&e, XTENSA_A2, XTENSA_A3);
    emit_add_n(&e, XTENSA_A2, XTENSA_A2, XTENSA_A4);
    emit_addi_n(&e, XTENSA_A2, XTENSA_A2, 5);
    emit_addi_n(&e, XTENSA_A2, XTENSA_A2, -1);
    emit_movi_n(&e, XTENSA_A3, 20);
    emit_s32i_n(&e, XTENSA_A2, XTENSA_A1, 8);
    emit_l32i_n(&e, XTENSA_A4, XTENSA_A1, 8);
    emit_ret_n(&e);

    const char *expected[] = {
        "mov.n\ta2, a3",
        "add.n\ta2, a2, a4",
        "addi.n\ta2, a2, 5",
        "addi.n\ta2, a2, -1",
        "movi.n\ta3, 20",
        "s32i.n\ta2, a1, 8",
        "l32i.n\ta4, a1, 8",
        "ret.n"
    };

    verify_disassembly(e.code, e.code_size, 0, "build/test_seq_d.bin",
                       expected, sizeof(expected)/sizeof(expected[0]));

    arena_free(&a);
}

TEST(test_sequence_e_literal_pool_l32r) {
    Arena a;
    ASSERT(arena_init(&a, 2048));

    Emitter e;
    ASSERT(emit_init(&e, &a, 128));

    // Load 32-bit constants requiring literal pool
    emit_load_imm(&e, XTENSA_A2, 0x12345678);
    emit_load_imm(&e, XTENSA_A3, 0x00ABCDEF);
    emit_add(&e, XTENSA_A2, XTENSA_A2, XTENSA_A3);
    emit_ret_n(&e);

    size_t total_size = 0;
    uint8_t *final_binary = emit_finalize(&e, &total_size);
    ASSERT(final_binary != NULL);
    ASSERT(total_size >= 12 + e.code_size); // 4-byte jump + 2 literals (8 bytes)

    const char *expected[] = {
        "l32r\ta2,",
        "l32r\ta3,",
        "add\ta2, a2, a3",
        "ret.n"
    };

    // Disassemble starting from code offset (after jump + 8-byte literal pool)
    verify_disassembly(final_binary, total_size, 12, "build/test_seq_e.bin",
                       expected, sizeof(expected)/sizeof(expected[0]));

    arena_free(&a);
}

TEST(test_sequence_f_multiply_and_subtract) {
    Arena a;
    ASSERT(arena_init(&a, 1024));

    Emitter e;
    ASSERT(emit_init(&e, &a, 128));

    emit_mull(&e, XTENSA_A2, XTENSA_A2, XTENSA_A3);
    emit_sub(&e, XTENSA_A2, XTENSA_A2, XTENSA_A4);
    emit_ret_n(&e);

    const char *expected[] = {
        "mull\ta2, a2, a3",
        "sub\ta2, a2, a4",
        "ret.n"
    };

    verify_disassembly(e.code, e.code_size, 0, "build/test_seq_f.bin",
                       expected, sizeof(expected)/sizeof(expected[0]));

    arena_free(&a);
}

TEST(test_sequence_g_jumps_and_calls) {
    Arena a;
    ASSERT(arena_init(&a, 1024));

    Emitter e;
    ASSERT(emit_init(&e, &a, 128));

    emit_callx0(&e, XTENSA_A8);
    emit_jx(&e, XTENSA_A0);

    const char *expected[] = {
        "callx0\ta8",
        "jx\ta0"
    };

    verify_disassembly(e.code, e.code_size, 0, "build/test_seq_g.bin",
                       expected, sizeof(expected)/sizeof(expected[0]));

    arena_free(&a);
}

// ============================================================================
// Main
// ============================================================================

int main(void) {
    printf("====================================================\n");
    printf(" Phase 2 Tests: Xtensa CALL0 Emitter & Disassembler\n");
    printf("====================================================\n");

    RUN_TEST(test_sequence_a_return_constant);
    RUN_TEST(test_sequence_b_call0_arithmetic);
    RUN_TEST(test_sequence_c_stack_frame_spill_load);
    RUN_TEST(test_sequence_d_density_instructions);
    RUN_TEST(test_sequence_e_literal_pool_l32r);
    RUN_TEST(test_sequence_f_multiply_and_subtract);
    RUN_TEST(test_sequence_g_jumps_and_calls);

    printf("====================================================\n");
    printf(" All %d tests passed successfully!\n", g_tests_passed);
    printf("====================================================\n");
    return 0;
}

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>

#include "arena.h"
#include "lexer.h"
#include "xtensa_emitter.h"
#include "symbol_table.h"
#include "parser.h"

static int g_tests_run = 0;
static int g_tests_passed = 0;

#define TEST(name) static void name(void)
#define RUN_TEST(name) do { \
    g_tests_run++; \
    printf("Running %-48s ... ", #name); \
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

    char output[16384];
    size_t total_read = 0;
    while (!feof(p) && total_read < sizeof(output) - 1) {
        size_t n = fread(output + total_read, 1, sizeof(output) - 1 - total_read, p);
        total_read += n;
    }
    output[total_read] = '\0';
    int status = pclose(p);
    ASSERT(status == 0);

    // Verify no illegal instructions
    if (strstr(output, "<illegal>")) {
        fprintf(stderr, "\nDisassembly contains <illegal> instructions in %s!\n%s\n", bin_filename, output);
        exit(1);
    }
    if (strstr(output, "ill.n") || strstr(output, "ill ")) {
        fprintf(stderr, "\nDisassembly contains unexpected ILL instruction in %s!\n%s\n", bin_filename, output);
        exit(1);
    }

    // Verify expected substrings
    for (size_t i = 0; i < num_expected; i++) {
        if (!strstr(output, expected_substrings[i])) {
            fprintf(stderr, "\nMissing expected substring '%s' in objdump output for %s:\n%s\n",
                    expected_substrings[i], bin_filename, output);
            exit(1);
        }
    }
}

// ============================================================================
// Unit Tests
// ============================================================================

TEST(test_parser_math_precedence) {
    const char *src =
        "int math_test() {\n"
        "    return (10 + 20) * 3 - (50 / 2);\n"
        "}\n";

    Arena arena;
    ASSERT(arena_init(&arena, 65536));

    Lexer lexer;
    lexer_init(&lexer, src, &arena);

    Emitter emitter;
    ASSERT(emit_init(&emitter, &arena, 1024));

    Parser parser;
    parser_init(&parser, &lexer, &emitter, &arena);

    CompiledFunction fn;
    ASSERT(compile_function(&parser, &fn));
    ASSERT(!parser.has_error);
    ASSERT(strcmp(fn.name, "math_test") == 0);
    ASSERT(fn.code_size > 0);

    const char *expected[] = {
        "mull",
        "quos",
        "sub",
        "ret.n"
    };

    verify_disassembly(fn.code, fn.code_size, 0, "build/test_p3_math.bin",
                       expected, sizeof(expected)/sizeof(expected[0]));

    arena_free(&arena);
}

TEST(test_parser_local_vars_stack_spill_load) {
    const char *src =
        "int local_vars() {\n"
        "    int a = 15;\n"
        "    int b = 27;\n"
        "    int c = a + b;\n"
        "    return c;\n"
        "}\n";

    Arena arena;
    ASSERT(arena_init(&arena, 65536));

    Lexer lexer;
    lexer_init(&lexer, src, &arena);

    Emitter emitter;
    ASSERT(emit_init(&emitter, &arena, 1024));

    Parser parser;
    parser_init(&parser, &lexer, &emitter, &arena);

    CompiledFunction fn;
    ASSERT(compile_function(&parser, &fn));
    ASSERT(!parser.has_error);
    ASSERT(strcmp(fn.name, "local_vars") == 0);
    ASSERT(fn.code_size > 0);

    const char *expected[] = {
        "s32i",
        "l32i",
        "add",
        "ret.n"
    };

    verify_disassembly(fn.code, fn.code_size, 0, "build/test_p3_locals.bin",
                       expected, sizeof(expected)/sizeof(expected[0]));

    arena_free(&arena);
}

TEST(test_parser_if_else_branches) {
    const char *src =
        "int max(int a, int b) {\n"
        "    if (a > b) {\n"
        "        return a;\n"
        "    } else {\n"
        "        return b;\n"
        "    }\n"
        "}\n";

    Arena arena;
    ASSERT(arena_init(&arena, 65536));

    Lexer lexer;
    lexer_init(&lexer, src, &arena);

    Emitter emitter;
    ASSERT(emit_init(&emitter, &arena, 1024));

    Parser parser;
    parser_init(&parser, &lexer, &emitter, &arena);

    CompiledFunction fn;
    ASSERT(compile_function(&parser, &fn));
    ASSERT(!parser.has_error);
    ASSERT(strcmp(fn.name, "max") == 0);
    ASSERT(fn.code_size > 0);

    const char *expected[] = {
        "beqz",
        "j",
        "l32i",
        "ret.n"
    };

    verify_disassembly(fn.code, fn.code_size, 0, "build/test_p3_if_else.bin",
                       expected, sizeof(expected)/sizeof(expected[0]));

    arena_free(&arena);
}

TEST(test_parser_while_loop_accumulation) {
    const char *src =
        "int sum_to_n(int n) {\n"
        "    int sum = 0;\n"
        "    int i = 1;\n"
        "    while (i <= n) {\n"
        "        sum = sum + i;\n"
        "        i = i + 1;\n"
        "    }\n"
        "    return sum;\n"
        "}\n";

    Arena arena;
    ASSERT(arena_init(&arena, 65536));

    Lexer lexer;
    lexer_init(&lexer, src, &arena);

    Emitter emitter;
    ASSERT(emit_init(&emitter, &arena, 1024));

    Parser parser;
    parser_init(&parser, &lexer, &emitter, &arena);

    CompiledFunction fn;
    ASSERT(compile_function(&parser, &fn));
    ASSERT(!parser.has_error);
    ASSERT(strcmp(fn.name, "sum_to_n") == 0);
    ASSERT(fn.code_size > 0);

    const char *expected[] = {
        "beqz",
        "j",
        "add",
        "s32i",
        "l32i",
        "ret.n"
    };

    verify_disassembly(fn.code, fn.code_size, 0, "build/test_p3_while.bin",
                       expected, sizeof(expected)/sizeof(expected[0]));

    arena_free(&arena);
}

TEST(test_parser_bitwise_and_unary) {
    const char *src =
        "int bitwise_ops(int x, int y) {\n"
        "    int a = x & y;\n"
        "    int b = x | y;\n"
        "    int c = x ^ y;\n"
        "    int d = ~x;\n"
        "    int e = !y;\n"
        "    int f = -x;\n"
        "    int g = x << 2;\n"
        "    int h = y >> 1;\n"
        "    return a + b + c + d + e + f + g + h;\n"
        "}\n";

    Arena arena;
    ASSERT(arena_init(&arena, 65536));

    Lexer lexer;
    lexer_init(&lexer, src, &arena);

    Emitter emitter;
    ASSERT(emit_init(&emitter, &arena, 1024));

    Parser parser;
    parser_init(&parser, &lexer, &emitter, &arena);

    CompiledFunction fn;
    ASSERT(compile_function(&parser, &fn));
    ASSERT(!parser.has_error);
    ASSERT(strcmp(fn.name, "bitwise_ops") == 0);

    const char *expected[] = {
        "and",
        "or",
        "xor",
        "neg",
        "sll",
        "sra",
        "ret.n"
    };

    verify_disassembly(fn.code, fn.code_size, 0, "build/test_p3_bitwise.bin",
                       expected, sizeof(expected)/sizeof(expected[0]));

    arena_free(&arena);
}

TEST(test_parser_translation_unit_multiple_functions) {
    const char *src =
        "int foo(int x) {\n"
        "    return x * 2;\n"
        "}\n"
        "int bar(int a, int b) {\n"
        "    return a + b;\n"
        "}\n";

    Arena arena;
    ASSERT(arena_init(&arena, 65536));

    Lexer lexer;
    lexer_init(&lexer, src, &arena);

    Emitter emitter;
    ASSERT(emit_init(&emitter, &arena, 1024));

    Parser parser;
    parser_init(&parser, &lexer, &emitter, &arena);

    CompiledFunction *fns = NULL;
    size_t fn_count = 0;
    ASSERT(compile_translation_unit(&parser, &fns, &fn_count));
    ASSERT(!parser.has_error);
    ASSERT(fn_count == 2);
    ASSERT(strcmp(fns[0].name, "foo") == 0);
    ASSERT(strcmp(fns[1].name, "bar") == 0);

    const char *expected_foo[] = { "mull", "ret.n" };
    verify_disassembly(fns[0].code, fns[0].code_size, 0, "build/test_p3_multi_foo.bin",
                       expected_foo, sizeof(expected_foo)/sizeof(expected_foo[0]));

    const char *expected_bar[] = { "add", "ret.n" };
    verify_disassembly(fns[1].code, fns[1].code_size, 0, "build/test_p3_multi_bar.bin",
                       expected_bar, sizeof(expected_bar)/sizeof(expected_bar[0]));

    arena_free(&arena);
}

// ============================================================================
// Main
// ============================================================================

int main(void) {
    printf("========================================================\n");
    printf(" Phase 3 Tests: Single-Pass Parser & Xtensa CALL0 Codegen\n");
    printf("========================================================\n");

    RUN_TEST(test_parser_math_precedence);
    RUN_TEST(test_parser_local_vars_stack_spill_load);
    RUN_TEST(test_parser_if_else_branches);
    RUN_TEST(test_parser_while_loop_accumulation);
    RUN_TEST(test_parser_bitwise_and_unary);
    RUN_TEST(test_parser_translation_unit_multiple_functions);

    printf("========================================================\n");
    printf(" All %d tests passed successfully!\n", g_tests_passed);
    printf("========================================================\n");
    return 0;
}

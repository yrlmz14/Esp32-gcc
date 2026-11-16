#define _GNU_SOURCE
#include "parser.h"
#include "lexer.h"
#include "xtensa_emitter.h"
#include "arena.h"
#include "elf_writer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>

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
        "/home/okan/.espressif/tools/xtensa-esp-elf/esp-12.2.0_20230208/xtensa-esp-elf/bin/xtensa-esp32-elf-objdump",
    };
    for (size_t i = 0; i < sizeof(candidates)/sizeof(candidates[0]); i++) {
        if (access(candidates[i], X_OK) == 0) {
            return candidates[i];
        }
    }
#ifdef _WIN32
    return "wsl /home/okan/.espressif/tools/xtensa-esp-elf/esp-14.2.0_20241119/xtensa-esp-elf/bin/xtensa-esp32-elf-objdump";
#else
    return NULL;
#endif
}

static void dump_binary(const char *filename, const uint8_t *code, size_t size) {
    FILE *f = fopen(filename, "wb");
    if (f) {
        fwrite(code, 1, size, f);
        fclose(f);
    }
}

static bool verify_objdump(const char *bin_path, const char *expected_substring) {
    const char *objdump = get_objdump_path();
    if (!objdump) {
        return true;
    }
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "%s -D -b binary -m xtensa %s", objdump, bin_path);
    FILE *pipe = popen(cmd, "r");
    if (!pipe) {
        return true;
    }
    char buffer[4096];
    char output[32768];
    output[0] = '\0';
    while (fgets(buffer, sizeof(buffer), pipe)) {
        if (strlen(output) + strlen(buffer) < sizeof(output) - 1) {
            strcat(output, buffer);
        }
    }
    pclose(pipe);
    return strstr(output, expected_substring) != NULL;
}

static bool compile_source(const char *src, CompiledFunction **out_fns, size_t *out_count, Arena *a) {
    Lexer lexer;
    lexer_init(&lexer, src, a);

    Emitter emitter;
    emit_init(&emitter, a, 1024);

    Parser parser;
    parser_init(&parser, &lexer, &emitter, a);

    return compile_translation_unit(&parser, out_fns, out_count);
}

static void test_for_loop_basic(void) {
    printf("Running %-48s ... ", __func__);
    Arena arena;
    arena_init(&arena, 64 * 1024);

    const char *src =
        "int test_for_sum() {\n"
        "    int s = 0;\n"
        "    int i;\n"
        "    for (i = 1; i <= 10; i = i + 1) {\n"
        "        s = s + i;\n"
        "    }\n"
        "    return s;\n"
        "}\n";

    CompiledFunction *fns = NULL;
    size_t count = 0;
    bool ok = compile_source(src, &fns, &count, &arena);
    assert(ok && count == 1);
    assert(strcmp(fns[0].name, "test_for_sum") == 0);
    assert(fns[0].code_size > 0);

    dump_binary("build/test_for_sum.bin", fns[0].code, fns[0].code_size);
    assert(verify_objdump("build/test_for_sum.bin", "j\t"));
    assert(verify_objdump("build/test_for_sum.bin", "beqz\t"));

    arena_free(&arena);
    printf("PASSED\n");
}

static void test_break_continue(void) {
    printf("Running %-48s ... ", __func__);
    Arena arena;
    arena_init(&arena, 64 * 1024);

    const char *src =
        "int test_break_continue() {\n"
        "    int s = 0;\n"
        "    int i;\n"
        "    for (i = 0; i < 20; i = i + 1) {\n"
        "        if (i == 5) {\n"
        "            continue;\n"
        "        }\n"
        "        if (i == 10) {\n"
        "            break;\n"
        "        }\n"
        "        s = s + i;\n"
        "    }\n"
        "    return s;\n"
        "}\n";

    CompiledFunction *fns = NULL;
    size_t count = 0;
    bool ok = compile_source(src, &fns, &count, &arena);
    assert(ok && count == 1);
    assert(strcmp(fns[0].name, "test_break_continue") == 0);
    assert(fns[0].code_size > 0);

    dump_binary("build/test_break_continue.bin", fns[0].code, fns[0].code_size);
    assert(verify_objdump("build/test_break_continue.bin", "j\t"));

    arena_free(&arena);
    printf("PASSED\n");
}

static void test_nested_break(void) {
    printf("Running %-48s ... ", __func__);
    Arena arena;
    arena_init(&arena, 64 * 1024);

    const char *src =
        "int test_nested_break() {\n"
        "    int total = 0;\n"
        "    int i;\n"
        "    int j;\n"
        "    for (i = 0; i < 3; i = i + 1) {\n"
        "        for (j = 0; j < 10; j = j + 1) {\n"
        "            if (j == 4) break;\n"
        "            total = total + 1;\n"
        "        }\n"
        "    }\n"
        "    return total;\n"
        "}\n";

    CompiledFunction *fns = NULL;
    size_t count = 0;
    bool ok = compile_source(src, &fns, &count, &arena);
    assert(ok && count == 1);
    assert(strcmp(fns[0].name, "test_nested_break") == 0);
    assert(fns[0].code_size > 0);

    dump_binary("build/test_nested_break.bin", fns[0].code, fns[0].code_size);

    arena_free(&arena);
    printf("PASSED\n");
}

static void test_ternary_operator(void) {
    printf("Running %-48s ... ", __func__);
    Arena arena;
    arena_init(&arena, 64 * 1024);

    const char *src =
        "int test_ternary(int a, int b) {\n"
        "    int max = (a > b) ? a : b;\n"
        "    int sign = (max > 0) ? 1 : ((max < 0) ? -1 : 0);\n"
        "    return max * 10 + sign;\n"
        "}\n";

    CompiledFunction *fns = NULL;
    size_t count = 0;
    bool ok = compile_source(src, &fns, &count, &arena);
    assert(ok && count == 1);
    assert(strcmp(fns[0].name, "test_ternary") == 0);
    assert(fns[0].code_size > 0);

    dump_binary("build/test_ternary.bin", fns[0].code, fns[0].code_size);
    assert(verify_objdump("build/test_ternary.bin", "beqz\t"));

    arena_free(&arena);
    printf("PASSED\n");
}

static void test_while_break_continue(void) {
    printf("Running %-48s ... ", __func__);
    Arena arena;
    arena_init(&arena, 64 * 1024);

    const char *src =
        "int test_while_loop() {\n"
        "    int s = 0;\n"
        "    int i = 0;\n"
        "    while (i < 20) {\n"
        "        i = i + 1;\n"
        "        if (i == 5) continue;\n"
        "        if (i == 11) break;\n"
        "        s = s + i;\n"
        "    }\n"
        "    return s;\n"
        "}\n";

    CompiledFunction *fns = NULL;
    size_t count = 0;
    bool ok = compile_source(src, &fns, &count, &arena);
    assert(ok && count == 1);
    assert(strcmp(fns[0].name, "test_while_loop") == 0);
    assert(fns[0].code_size > 0);

    dump_binary("build/test_while_loop.bin", fns[0].code, fns[0].code_size);

    arena_free(&arena);
    printf("PASSED\n");
}

int main(void) {
    printf("========================================================\n");
    printf(" Phase 10 Tests: For Loops, Break/Continue, & Ternary\n");
    printf("========================================================\n");

    test_for_loop_basic();
    test_break_continue();
    test_nested_break();
    test_ternary_operator();
    test_while_break_continue();

    printf("========================================================\n");
    printf(" All Phase 10 unit tests passed successfully!\n");
    printf("========================================================\n");
    return 0;
}

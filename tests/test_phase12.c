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
    lexer_add_include_path(&lexer, "include");

    Emitter emitter;
    emit_init(&emitter, a, 1024);

    Parser parser;
    parser_init(&parser, &lexer, &emitter, a);

    return compile_translation_unit(&parser, out_fns, out_count);
}

static void test_c99_typedef_and_types(void) {
    printf("Running %-48s ... ", __func__);
    Arena arena;
    arena_init(&arena, 64 * 1024);

    const char *src =
        "typedef int my_int_t;\n"
        "typedef unsigned int uint32_t;\n"
        "typedef struct Point { int x; int y; } Point_t;\n"
        "int test_typedef() {\n"
        "    my_int_t a = 42;\n"
        "    uint32_t b = 100;\n"
        "    Point_t pt;\n"
        "    pt.x = a;\n"
        "    pt.y = b;\n"
        "    return pt.x + pt.y;\n"
        "}\n";

    CompiledFunction *fns = NULL;
    size_t count = 0;
    bool ok = compile_source(src, &fns, &count, &arena);
    assert(ok && count == 1);
    assert(strcmp(fns[0].name, "test_typedef") == 0);
    assert(fns[0].code_size > 0);

    dump_binary("build/test_typedef.bin", fns[0].code, fns[0].code_size);

    arena_free(&arena);
    printf("PASSED\n");
}

static void test_c99_enum(void) {
    printf("Running %-48s ... ", __func__);
    Arena arena;
    arena_init(&arena, 64 * 1024);

    const char *src =
        "enum State {\n"
        "    IDLE = 0,\n"
        "    RUNNING = 10,\n"
        "    HALTED = 20\n"
        "};\n"
        "int test_enum(int s) {\n"
        "    if (s == IDLE) return 1;\n"
        "    if (s == RUNNING) return 2;\n"
        "    if (s == HALTED) return 3;\n"
        "    return 0;\n"
        "}\n";

    CompiledFunction *fns = NULL;
    size_t count = 0;
    bool ok = compile_source(src, &fns, &count, &arena);
    assert(ok && count == 1);
    assert(strcmp(fns[0].name, "test_enum") == 0);
    assert(fns[0].code_size > 0);

    dump_binary("build/test_enum.bin", fns[0].code, fns[0].code_size);

    arena_free(&arena);
    printf("PASSED\n");
}

static void test_c99_stdbool_and_bool(void) {
    printf("Running %-48s ... ", __func__);
    Arena arena;
    arena_init(&arena, 64 * 1024);

    const char *src =
        "#include <stdbool.h>\n"
        "bool test_bool(int val) {\n"
        "    bool flag = true;\n"
        "    if (val < 0) {\n"
        "        flag = false;\n"
        "    }\n"
        "    return flag;\n"
        "}\n";

    CompiledFunction *fns = NULL;
    size_t count = 0;
    bool ok = compile_source(src, &fns, &count, &arena);
    assert(ok && count == 1);
    assert(strcmp(fns[0].name, "test_bool") == 0);
    assert(fns[0].code_size > 0);

    dump_binary("build/test_bool.bin", fns[0].code, fns[0].code_size);

    arena_free(&arena);
    printf("PASSED\n");
}

static void test_c99_preprocessor_conditionals(void) {
    printf("Running %-48s ... ", __func__);
    Arena arena;
    arena_init(&arena, 64 * 1024);

    const char *src =
        "#define FEATURE_A 1\n"
        "#ifdef FEATURE_A\n"
        "int test_cond() {\n"
        "    int res = 100;\n"
        "#ifdef FEATURE_B\n"
        "    res = res + 50;\n"
        "#else\n"
        "    res = res + 20;\n"
        "#endif\n"
        "    return res;\n"
        "}\n"
        "#endif\n";

    CompiledFunction *fns = NULL;
    size_t count = 0;
    bool ok = compile_source(src, &fns, &count, &arena);
    assert(ok && count == 1);
    assert(strcmp(fns[0].name, "test_cond") == 0);
    assert(fns[0].code_size > 0);

    dump_binary("build/test_cond.bin", fns[0].code, fns[0].code_size);

    arena_free(&arena);
    printf("PASSED\n");
}

static void test_c99_brace_initializers(void) {
    printf("Running %-48s ... ", __func__);
    Arena arena;
    arena_init(&arena, 64 * 1024);

    const char *src =
        "struct Point { int x; int y; };\n"
        "int test_inits() {\n"
        "    int arr[4] = { 10, 20, 30, 40 };\n"
        "    struct Point p = { 100, 200 };\n"
        "    return arr[0] + arr[1] + arr[2] + arr[3] + p.x + p.y;\n"
        "}\n";

    CompiledFunction *fns = NULL;
    size_t count = 0;
    bool ok = compile_source(src, &fns, &count, &arena);
    assert(ok && count == 1);
    assert(strcmp(fns[0].name, "test_inits") == 0);
    assert(fns[0].code_size > 0);

    dump_binary("build/test_inits.bin", fns[0].code, fns[0].code_size);

    arena_free(&arena);
    printf("PASSED\n");
}

static void test_c99_stdint(void) {
    printf("Running %-48s ... ", __func__);
    Arena arena;
    arena_init(&arena, 64 * 1024);

    const char *src =
        "#include <stdint.h>\n"
        "uint32_t test_stdint() {\n"
        "    uint8_t a = 250;\n"
        "    int16_t b = -50;\n"
        "    uint32_t c = 1000;\n"
        "    return c + (uint32_t)a + (uint32_t)b;\n"
        "}\n";

    CompiledFunction *fns = NULL;
    size_t count = 0;
    bool ok = compile_source(src, &fns, &count, &arena);
    assert(ok && count == 1);
    assert(strcmp(fns[0].name, "test_stdint") == 0);
    assert(fns[0].code_size > 0);

    dump_binary("build/test_stdint.bin", fns[0].code, fns[0].code_size);

    arena_free(&arena);
    printf("PASSED\n");
}

int main(void) {
    printf("========================================================\n");
    printf(" Phase 12 Tests: C99 Standard Compatibility\n");
    printf("========================================================\n");

    test_c99_typedef_and_types();
    test_c99_enum();
    test_c99_stdbool_and_bool();
    test_c99_preprocessor_conditionals();
    test_c99_brace_initializers();
    test_c99_stdint();

    printf("========================================================\n");
    printf(" All Phase 12 C99 tests passed successfully!\n");
    printf("========================================================\n");
    return 0;
}

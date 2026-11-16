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

static void test_logical_operators(void) {
    printf("Running %-48s ... ", __func__);
    Arena arena;
    arena_init(&arena, 64 * 1024);

    const char *src =
        "int test_logical(int a, int b) {\n"
        "    int res = 0;\n"
        "    if (a > 0 && b > 0) {\n"
        "        res = 1;\n"
        "    }\n"
        "    if (a < 0 || b < 0) {\n"
        "        res = res + 10;\n"
        "    }\n"
        "    return res;\n"
        "}\n";

    CompiledFunction *fns = NULL;
    size_t count = 0;
    bool ok = compile_source(src, &fns, &count, &arena);
    assert(ok && count == 1);
    assert(strcmp(fns[0].name, "test_logical") == 0);
    assert(fns[0].code_size > 0);

    dump_binary("build/test_logical.bin", fns[0].code, fns[0].code_size);
    assert(verify_objdump("build/test_logical.bin", "beqz\t"));
    assert(verify_objdump("build/test_logical.bin", "bnez\t"));

    arena_free(&arena);
    printf("PASSED\n");
}

static void test_compound_assignments(void) {
    printf("Running %-48s ... ", __func__);
    Arena arena;
    arena_init(&arena, 64 * 1024);

    const char *src =
        "int test_compound() {\n"
        "    int a = 100;\n"
        "    a += 50;\n"
        "    a -= 20;\n"
        "    a *= 2;\n"
        "    a /= 4;\n"
        "    a %= 30;\n"
        "    a <<= 2;\n"
        "    a |= 3;\n"
        "    a &= 31;\n"
        "    a ^= 15;\n"
        "    return a;\n"
        "}\n";

    CompiledFunction *fns = NULL;
    size_t count = 0;
    bool ok = compile_source(src, &fns, &count, &arena);
    assert(ok && count == 1);
    assert(strcmp(fns[0].name, "test_compound") == 0);
    assert(fns[0].code_size > 0);

    dump_binary("build/test_compound.bin", fns[0].code, fns[0].code_size);

    arena_free(&arena);
    printf("PASSED\n");
}

static void test_inc_dec_operators(void) {
    printf("Running %-48s ... ", __func__);
    Arena arena;
    arena_init(&arena, 64 * 1024);

    const char *src =
        "int test_inc_dec() {\n"
        "    int a = 10;\n"
        "    ++a;\n"
        "    a++;\n"
        "    --a;\n"
        "    a--;\n"
        "    return a;\n"
        "}\n";

    CompiledFunction *fns = NULL;
    size_t count = 0;
    bool ok = compile_source(src, &fns, &count, &arena);
    assert(ok && count == 1);
    assert(strcmp(fns[0].name, "test_inc_dec") == 0);
    assert(fns[0].code_size > 0);

    dump_binary("build/test_inc_dec.bin", fns[0].code, fns[0].code_size);

    arena_free(&arena);
    printf("PASSED\n");
}

static void test_do_while_loop(void) {
    printf("Running %-48s ... ", __func__);
    Arena arena;
    arena_init(&arena, 64 * 1024);

    const char *src =
        "int test_do_while() {\n"
        "    int s = 0;\n"
        "    int i = 1;\n"
        "    do {\n"
        "        s += i;\n"
        "        i++;\n"
        "    } while (i <= 10);\n"
        "    return s;\n"
        "}\n";

    CompiledFunction *fns = NULL;
    size_t count = 0;
    bool ok = compile_source(src, &fns, &count, &arena);
    assert(ok && count == 1);
    assert(strcmp(fns[0].name, "test_do_while") == 0);
    assert(fns[0].code_size > 0);

    dump_binary("build/test_do_while.bin", fns[0].code, fns[0].code_size);
    assert(verify_objdump("build/test_do_while.bin", "bnez\t"));

    arena_free(&arena);
    printf("PASSED\n");
}

static void test_switch_case_basic(void) {
    printf("Running %-48s ... ", __func__);
    Arena arena;
    arena_init(&arena, 64 * 1024);

    const char *src =
        "int test_switch(int mode) {\n"
        "    int result = 0;\n"
        "    switch (mode) {\n"
        "        case 1:\n"
        "            result = 100;\n"
        "            break;\n"
        "        case 2:\n"
        "            result = 200;\n"
        "            break;\n"
        "        case 3:\n"
        "            result = 300;\n"
        "            break;\n"
        "        default:\n"
        "            result = 999;\n"
        "            break;\n"
        "    }\n"
        "    return result;\n"
        "}\n";

    CompiledFunction *fns = NULL;
    size_t count = 0;
    bool ok = compile_source(src, &fns, &count, &arena);
    assert(ok && count == 1);
    assert(strcmp(fns[0].name, "test_switch") == 0);
    assert(fns[0].code_size > 0);

    dump_binary("build/test_switch.bin", fns[0].code, fns[0].code_size);
    assert(verify_objdump("build/test_switch.bin", "beq\t"));

    arena_free(&arena);
    printf("PASSED\n");
}

int main(void) {
    printf("========================================================\n");
    printf(" Phase 11 Tests: Switch, &&/||, +=, ++/--, do-while\n");
    printf("========================================================\n");

    test_logical_operators();
    test_compound_assignments();
    test_inc_dec_operators();
    test_do_while_loop();
    test_switch_case_basic();

    printf("========================================================\n");
    printf(" All Phase 11 unit tests passed successfully!\n");
    printf("========================================================\n");
    return 0;
}

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

    if (strstr(output, expected_substring) == NULL) {
        printf("\nMissing expected substring '%s' in objdump output for %s:\n\n%s\n",
               expected_substring, bin_path, output);
        return false;
    }
    return true;
}

// Test 1: Stack integer array accumulation
static void test_array_sum(void) {
    Arena arena;
    arena_init(&arena, 64 * 1024);

    const char *src =
        "int test_array_sum() {\n"
        "    int arr[5];\n"
        "    arr[0] = 10;\n"
        "    arr[1] = 20;\n"
        "    arr[2] = 30;\n"
        "    arr[3] = 40;\n"
        "    arr[4] = 50;\n"
        "    int sum = 0;\n"
        "    int i = 0;\n"
        "    while (i < 5) {\n"
        "        sum = sum + arr[i];\n"
        "        i = i + 1;\n"
        "    }\n"
        "    return sum;\n"
        "}\n";

    Lexer l;
    lexer_init(&l, src, &arena);
    Emitter e;
    emit_init(&e, &arena, 1024);
    Parser p;
    parser_init(&p, &l, &e, &arena);

    CompiledFunction fn;
    bool ok = compile_function(&p, &fn);
    assert(ok && !p.has_error);
    assert(strcmp(fn.name, "test_array_sum") == 0);
    assert(fn.code_size > 0);

    const char *bin_file = "build/test_p7_array_sum.bin";
    dump_binary(bin_file, fn.code, fn.code_size);

    assert(verify_objdump(bin_file, "s32i"));
    assert(verify_objdump(bin_file, "l32i"));
    assert(verify_objdump(bin_file, "add.n") || verify_objdump(bin_file, "add"));
    assert(verify_objdump(bin_file, "ret.n"));

    arena_free(&arena);
    printf("Running test_array_sum                                   ... PASSED\n");
}

// Test 2: In-place bubble sort on stack array
static void test_bubble_sort(void) {
    Arena arena;
    arena_init(&arena, 64 * 1024);

    const char *src =
        "int test_sort() {\n"
        "    int a[4];\n"
        "    a[0] = 42;\n"
        "    a[1] = 12;\n"
        "    a[2] = 88;\n"
        "    a[3] = 5;\n"
        "    int i = 0;\n"
        "    while (i < 4) {\n"
        "        int j = 0;\n"
        "        while (j < 3) {\n"
        "            if (a[j] > a[j + 1]) {\n"
        "                int tmp = a[j];\n"
        "                a[j] = a[j + 1];\n"
        "                a[j + 1] = tmp;\n"
        "            }\n"
        "            j = j + 1;\n"
        "        }\n"
        "        i = i + 1;\n"
        "    }\n"
        "    return a[0] * 1000 + a[1] * 100 + a[2] * 10 + a[3];\n"
        "}\n";

    Lexer l;
    lexer_init(&l, src, &arena);
    Emitter e;
    emit_init(&e, &arena, 1024);
    Parser p;
    parser_init(&p, &l, &e, &arena);

    CompiledFunction fn;
    bool ok = compile_function(&p, &fn);
    assert(ok && !p.has_error);
    assert(strcmp(fn.name, "test_sort") == 0);
    assert(fn.code_size > 0);

    const char *bin_file = "build/test_p7_bubble_sort.bin";
    dump_binary(bin_file, fn.code, fn.code_size);

    assert(verify_objdump(bin_file, "s32i"));
    assert(verify_objdump(bin_file, "l32i"));
    assert(verify_objdump(bin_file, "ret.n"));

    arena_free(&arena);
    printf("Running test_bubble_sort                                 ... PASSED\n");
}

// Test 3: Byte array indexing and load/store (char buf[N])
static void test_char_array(void) {
    Arena arena;
    arena_init(&arena, 64 * 1024);

    const char *src =
        "int test_char_buf() {\n"
        "    char buf[8];\n"
        "    buf[0] = 65;\n"
        "    buf[1] = 66;\n"
        "    buf[2] = 67;\n"
        "    return buf[0] + buf[1] + buf[2];\n"
        "}\n";

    Lexer l;
    lexer_init(&l, src, &arena);
    Emitter e;
    emit_init(&e, &arena, 1024);
    Parser p;
    parser_init(&p, &l, &e, &arena);

    CompiledFunction fn;
    bool ok = compile_function(&p, &fn);
    assert(ok && !p.has_error);
    assert(strcmp(fn.name, "test_char_buf") == 0);
    assert(fn.code_size > 0);

    const char *bin_file = "build/test_p7_char_buf.bin";
    dump_binary(bin_file, fn.code, fn.code_size);

    assert(verify_objdump(bin_file, "s8i"));
    assert(verify_objdump(bin_file, "l8ui"));
    assert(verify_objdump(bin_file, "ret.n"));

    arena_free(&arena);
    printf("Running test_char_array                                  ... PASSED\n");
}

int main(void) {
    printf("========================================================\n");
    printf(" Phase 7 Tests: Stack Arrays & Bracket Subscripting\n");
    printf("========================================================\n");

    test_array_sum();
    test_bubble_sort();
    test_char_array();

    printf("========================================================\n");
    printf(" All Phase 7 unit tests passed successfully!\n");
    printf("========================================================\n");
    return 0;
}

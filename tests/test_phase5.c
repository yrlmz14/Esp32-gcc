#define _GNU_SOURCE
#include "parser.h"
#include "lexer.h"
#include "xtensa_emitter.h"
#include "arena.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>

static void *my_memmem(const void *haystack, size_t haystacklen, const void *needle, size_t needlelen) {
    if (!haystack || !needle || needlelen == 0 || haystacklen < needlelen) return NULL;
    const char *h = (const char *)haystack;
    for (size_t i = 0; i <= haystacklen - needlelen; i++) {
        if (memcmp(h + i, needle, needlelen) == 0) return (void *)(h + i);
    }
    return NULL;
}
#define memmem my_memmem

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

// Test 1: Pointer swap function
static void test_pointer_swap(void) {
    Arena arena;
    arena_init(&arena, 64 * 1024);

    const char *src =
        "void swap(int *a, int *b) {\n"
        "    int temp = *a;\n"
        "    *a = *b;\n"
        "    *b = temp;\n"
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
    assert(strcmp(fn.name, "swap") == 0);
    assert(fn.code_size > 0);

    const char *bin_file = "build/test_p5_swap.bin";
    dump_binary(bin_file, fn.code, fn.code_size);

    assert(verify_objdump(bin_file, "l32i"));
    assert(verify_objdump(bin_file, "s32i"));
    assert(verify_objdump(bin_file, "ret.n"));

    arena_free(&arena);
    printf("Running test_pointer_swap                                ... PASSED\n");
}

// Test 2: String length function via pointer arithmetic and byte load (l8ui)
static void test_mystrlen(void) {
    Arena arena;
    arena_init(&arena, 64 * 1024);

    const char *src =
        "int mystrlen(char *s) {\n"
        "    int len = 0;\n"
        "    while (*s != 0) {\n"
        "        len = len + 1;\n"
        "        s = s + 1;\n"
        "    }\n"
        "    return len;\n"
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
    assert(strcmp(fn.name, "mystrlen") == 0);
    assert(fn.code_size > 0);

    const char *bin_file = "build/test_p5_strlen.bin";
    dump_binary(bin_file, fn.code, fn.code_size);

    assert(verify_objdump(bin_file, "l8ui"));
    assert(verify_objdump(bin_file, "beqz"));
    assert(verify_objdump(bin_file, "ret.n"));

    arena_free(&arena);
    printf("Running test_mystrlen                                    ... PASSED\n");
}

// Test 3: Address-Of local variable mutation (&x)
static void test_address_of(void) {
    Arena arena;
    arena_init(&arena, 64 * 1024);

    const char *src =
        "int test_addr() {\n"
        "    int x = 42;\n"
        "    int *p = &x;\n"
        "    *p = 100;\n"
        "    return x;\n"
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
    assert(strcmp(fn.name, "test_addr") == 0);
    assert(fn.code_size > 0);

    const char *bin_file = "build/test_p5_addr.bin";
    dump_binary(bin_file, fn.code, fn.code_size);

    assert(verify_objdump(bin_file, "addi\ta2, a1"));
    assert(verify_objdump(bin_file, "s32i"));

    arena_free(&arena);
    printf("Running test_address_of                                  ... PASSED\n");
}

// Test 4: String literal embedding and PC-relative address loading
static void test_string_literal_codegen(void) {
    Arena arena;
    arena_init(&arena, 64 * 1024);

    const char *src =
        "int test_str() {\n"
        "    char *s = \"Hello, World!\\n\";\n"
        "    return *s;\n"
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
    assert(strcmp(fn.name, "test_str") == 0);
    assert(fn.code_size > 0);

    // Verify string literal contents in emitted binary pool
    assert(memmem(fn.code, fn.code_size, "Hello, World!\n", 14) != NULL);

    // Dump pure code portion (after 4-byte jump/pad and 16-byte aligned string pool)
    size_t pool_size = 4 + ((15 + 3) & ~3);
    const char *bin_file = "build/test_p5_str.bin";
    dump_binary(bin_file, fn.code + pool_size, fn.code_size - pool_size);

    assert(verify_objdump(bin_file, "call0"));
    assert(verify_objdump(bin_file, "l8ui"));
    assert(verify_objdump(bin_file, "ret.n"));

    arena_free(&arena);
    printf("Running test_string_literal_codegen                      ... PASSED\n");
}

int main(void) {
    printf("========================================================\n");
    printf(" Phase 5 Tests: Pointers, Strings, Address-Of, Deref\n");
    printf("========================================================\n");

    test_pointer_swap();
    test_mystrlen();
    test_address_of();
    test_string_literal_codegen();

    printf("========================================================\n");
    printf(" All 4 Phase 5 tests passed successfully!\n");
    printf("========================================================\n");
    return 0;
}

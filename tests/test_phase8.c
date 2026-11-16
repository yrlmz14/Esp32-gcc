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

    if (strstr(output, expected_substring) == NULL) {
        printf("\nMissing expected substring '%s' in objdump output for %s:\n\n%s\n",
               expected_substring, bin_path, output);
        return false;
    }
    return true;
}

// Test 1: Preprocessor #define and #include
static void test_preprocessor(void) {
    // Create temporary header in build/
    int ret = system("mkdir -p build");
    (void)ret;
    FILE *hf = fopen("build/test_header.h", "w");
    assert(hf != NULL);
    fprintf(hf, "#define MAGIC_NUM 42\n");
    fprintf(hf, "struct Vector2D {\n    int x;\n    int y;\n};\n");
    fclose(hf);

    Arena arena;
    arena_init(&arena, 64 * 1024);

    const char *src =
        "#include \"build/test_header.h\"\n"
        "int test_macro() {\n"
        "    return MAGIC_NUM + 8;\n"
        "}\n";

    Lexer l;
    lexer_init(&l, src, &arena);
    Emitter e;
    emit_init(&e, &arena, 1024);
    Parser p;
    parser_init(&p, &l, &e, &arena);

    CompiledFunction *fns = NULL;
    size_t count = 0;
    bool ok = compile_translation_unit(&p, &fns, &count);
    assert(ok && !p.has_error);
    assert(count == 1);
    assert(strcmp(fns[0].name, "test_macro") == 0);

    const char *bin_file = "build/test_p8_macro.bin";
    dump_binary(bin_file, fns[0].code, fns[0].code_size);

    assert(verify_objdump(bin_file, "movi"));
    assert(verify_objdump(bin_file, "ret.n"));

    arena_free(&arena);
    printf("Running test_preprocessor                                ... PASSED\n");
}

// Test 2: Struct by value & dot operator (.x, .y)
static void test_struct_dot(void) {
    Arena arena;
    arena_init(&arena, 64 * 1024);

    const char *src =
        "struct Vector2D {\n"
        "    int x;\n"
        "    int y;\n"
        "};\n"
        "int test_struct_dot() {\n"
        "    struct Vector2D v;\n"
        "    v.x = 15;\n"
        "    v.y = 25;\n"
        "    return v.x + v.y;\n"
        "}\n";

    Lexer l;
    lexer_init(&l, src, &arena);
    Emitter e;
    emit_init(&e, &arena, 1024);
    Parser p;
    parser_init(&p, &l, &e, &arena);

    CompiledFunction *fns = NULL;
    size_t count = 0;
    bool ok = compile_translation_unit(&p, &fns, &count);
    assert(ok && !p.has_error);
    assert(count == 1);
    assert(strcmp(fns[0].name, "test_struct_dot") == 0);

    const char *bin_file = "build/test_p8_dot.bin";
    dump_binary(bin_file, fns[0].code, fns[0].code_size);

    assert(verify_objdump(bin_file, "s32i"));
    assert(verify_objdump(bin_file, "l32i"));
    assert(verify_objdump(bin_file, "ret.n"));

    arena_free(&arena);
    printf("Running test_struct_dot                                  ... PASSED\n");
}

// Test 3: Struct pointer & arrow operator (->x, ->y)
static void test_struct_arrow(void) {
    Arena arena;
    arena_init(&arena, 64 * 1024);

    const char *src =
        "struct Vector2D {\n"
        "    int x;\n"
        "    int y;\n"
        "};\n"
        "int test_struct_arrow() {\n"
        "    struct Vector2D v;\n"
        "    struct Vector2D *p = &v;\n"
        "    p->x = 100;\n"
        "    p->y = 200;\n"
        "    return p->x + p->y;\n"
        "}\n";

    Lexer l;
    lexer_init(&l, src, &arena);
    Emitter e;
    emit_init(&e, &arena, 1024);
    Parser p;
    parser_init(&p, &l, &e, &arena);

    CompiledFunction *fns = NULL;
    size_t count = 0;
    bool ok = compile_translation_unit(&p, &fns, &count);
    assert(ok && !p.has_error);
    assert(count == 1);
    assert(strcmp(fns[0].name, "test_struct_arrow") == 0);

    const char *bin_file = "build/test_p8_arrow.bin";
    dump_binary(bin_file, fns[0].code, fns[0].code_size);

    assert(verify_objdump(bin_file, "s32i"));
    assert(verify_objdump(bin_file, "l32i"));
    assert(verify_objdump(bin_file, "ret.n"));

    arena_free(&arena);
    printf("Running test_struct_arrow                                ... PASSED\n");
}

// Test 4: Struct size & sizeof(struct Vector2D)
static void test_struct_sizeof(void) {
    Arena arena;
    arena_init(&arena, 64 * 1024);

    const char *src =
        "struct Vector2D {\n"
        "    int x;\n"
        "    int y;\n"
        "};\n"
        "int test_sizeof() {\n"
        "    return sizeof(struct Vector2D);\n"
        "}\n";

    Lexer l;
    lexer_init(&l, src, &arena);
    Emitter e;
    emit_init(&e, &arena, 1024);
    Parser p;
    parser_init(&p, &l, &e, &arena);

    CompiledFunction *fns = NULL;
    size_t count = 0;
    bool ok = compile_translation_unit(&p, &fns, &count);
    assert(ok && !p.has_error);
    assert(count == 1);
    assert(strcmp(fns[0].name, "test_sizeof") == 0);

    const char *bin_file = "build/test_p8_sizeof.bin";
    dump_binary(bin_file, fns[0].code, fns[0].code_size);

    assert(verify_objdump(bin_file, "movi"));
    assert(verify_objdump(bin_file, "ret.n"));

    arena_free(&arena);
    printf("Running test_struct_sizeof                               ... PASSED\n");
}

int main(void) {
    printf("========================================================\n");
    printf(" Phase 8 Tests: Preprocessor & Struct Support\n");
    printf("========================================================\n");

    test_preprocessor();
    test_struct_dot();
    test_struct_arrow();
    test_struct_sizeof();

    printf("========================================================\n");
    printf(" All Phase 8 unit tests passed successfully!\n");
    printf("========================================================\n");
    return 0;
}

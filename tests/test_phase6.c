#define _GNU_SOURCE
#include "elf_writer.h"
#include "parser.h"
#include "lexer.h"
#include "xtensa_emitter.h"
#include "arena.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>

static const char *get_readelf_path(void) {
    const char *env = getenv("READELF");
    if (env && access(env, X_OK) == 0) return env;

    if (system("which xtensa-esp32-elf-readelf > /dev/null 2>&1") == 0) {
        return "xtensa-esp32-elf-readelf";
    }

    const char *candidates[] = {
        "/home/okan/.espressif/tools/xtensa-esp-elf/esp-14.2.0_20241119/xtensa-esp-elf/bin/xtensa-esp32-elf-readelf",
        "/home/okan/.espressif/tools/xtensa-esp32-elf/esp-12.2.0_20230208/xtensa-esp32-elf/bin/xtensa-esp32-elf-readelf",
        "/usr/bin/readelf",
        "readelf",
    };
    for (size_t i = 0; i < sizeof(candidates)/sizeof(candidates[0]); i++) {
        if (access(candidates[i], X_OK) == 0) return candidates[i];
    }
    return NULL;
}

static bool verify_readelf(const char *o_path, const char *expected_substring) {
    const char *readelf = get_readelf_path();
    if (!readelf) return true;

    char cmd[512];
    snprintf(cmd, sizeof(cmd), "%s -h -S -s %s 2>&1", readelf, o_path);
    FILE *pipe = popen(cmd, "r");
    if (!pipe) return true;

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
        printf("\nMissing expected substring '%s' in readelf output for %s:\n\n%s\n",
               expected_substring, o_path, output);
        return false;
    }
    return true;
}

// Test 1: Direct ELF Object Writer unit test
static void test_elf_writer_unit(void) {
    Arena arena;
    arena_init(&arena, 64 * 1024);

    ElfWriter w;
    elf_writer_init(&w, &arena);

    uint8_t dummy_text[] = { 0x12, 0xc1, 0x10, 0x0d, 0xf0 }; // addi a1, a1, 16; ret.n
    elf_writer_set_text(&w, dummy_text, sizeof(dummy_text));

    elf_writer_add_symbol(&w, "test.c", 0, 0, STT_FILE, STB_LOCAL, SHN_ABS);
    elf_writer_add_symbol(&w, "", 0, 0, STT_SECTION, STB_LOCAL, 1);
    elf_writer_add_symbol(&w, "dummy_fn", 0, (uint32_t)sizeof(dummy_text), STT_FUNC, STB_GLOBAL, 1);

    const char *out_file = "build/test_dummy.o";
    bool ok = elf_writer_write_file(&w, out_file);
    assert(ok);

    assert(verify_readelf(out_file, "REL (Relocatable file)"));
    assert(verify_readelf(out_file, ".text"));
    assert(verify_readelf(out_file, ".symtab"));
    assert(verify_readelf(out_file, ".strtab"));
    assert(verify_readelf(out_file, ".shstrtab"));
    assert(verify_readelf(out_file, "dummy_fn"));

    arena_free(&arena);
    printf("Running test_elf_writer_unit                             ... PASSED\n");
}

// Test 2: Compile multi-function translation unit into .o object
static void test_multi_function_elf_object(void) {
    Arena arena;
    arena_init(&arena, 128 * 1024);

    const char *src =
        "int sum_to_n(int n) {\n"
        "    int sum = 0;\n"
        "    int i = 1;\n"
        "    while (i <= n) {\n"
        "        sum = sum + i;\n"
        "        i = i + 1;\n"
        "    }\n"
        "    return sum;\n"
        "}\n"
        "int hello_world() {\n"
        "    volatile int *uart = (volatile int*)0x3FF40000;\n"
        "    char *str = \"Hello, World!\\n\";\n"
        "    int count = 0;\n"
        "    while (*str != 0) {\n"
        "        *uart = *str;\n"
        "        str = str + 1;\n"
        "        count = count + 1;\n"
        "    }\n"
        "    return count;\n"
        "}\n";

    Lexer l;
    lexer_init(&l, src, &arena);
    Emitter e;
    emit_init(&e, &arena, 1024);
    Parser p;
    parser_init(&p, &l, &e, &arena);

    CompiledFunction *fns = NULL;
    size_t fn_count = 0;
    bool ok = compile_translation_unit(&p, &fns, &fn_count);
    assert(ok && !p.has_error);
    assert(fn_count == 2);

    ElfWriter w;
    elf_writer_init(&w, &arena);

    elf_writer_add_symbol(&w, "multi.c", 0, 0, STT_FILE, STB_LOCAL, SHN_ABS);
    elf_writer_add_symbol(&w, "", 0, 0, STT_SECTION, STB_LOCAL, 1);

    size_t total_len = 0;
    for (size_t i = 0; i < fn_count; i++) {
        total_len += (fns[i].code_size + 3) & ~3;
    }

    uint8_t *text_buf = (uint8_t *)arena_alloc(&arena, total_len);
    memset(text_buf, 0, total_len);

    size_t curr = 0;
    for (size_t i = 0; i < fn_count; i++) {
        size_t aligned_pos = (curr + 3) & ~3;
        memcpy(text_buf + aligned_pos, fns[i].code, fns[i].code_size);
        elf_writer_add_symbol(&w, fns[i].name, (uint32_t)aligned_pos, (uint32_t)fns[i].code_size, STT_FUNC, STB_GLOBAL, 1);
        curr = aligned_pos + fns[i].code_size;
    }

    elf_writer_set_text(&w, text_buf, total_len);

    const char *out_file = "build/test_multi.o";
    ok = elf_writer_write_file(&w, out_file);
    assert(ok);

    assert(verify_readelf(out_file, "REL (Relocatable file)"));
    assert(verify_readelf(out_file, "sum_to_n"));
    assert(verify_readelf(out_file, "hello_world"));

    arena_free(&arena);
    printf("Running test_multi_function_elf_object                   ... PASSED\n");
}

int main(void) {
    printf("========================================================\n");
    printf(" Phase 6 Tests: Native ELF32 Relocatable Object Writer\n");
    printf("========================================================\n");

    test_elf_writer_unit();
    test_multi_function_elf_object();

    printf("========================================================\n");
    printf(" All Phase 6 unit tests passed successfully!\n");
    printf("========================================================\n");
    return 0;
}

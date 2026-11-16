#include "arena.h"
#include "lexer.h"
#include "parser.h"
#include "xtensa_emitter.h"
#include "symbol_table.h"
#include "elf_writer.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>

#define COLOR_GREEN "\033[32m"
#define COLOR_RED   "\033[31m"
#define COLOR_RESET "\033[0m"

static bool compile_string(const char *src, CompiledFunction **out_fns, size_t *out_count, Arena *arena) {
    Lexer lexer;
    lexer_init(&lexer, src, arena);

    Emitter emitter;
    emit_init(&emitter, arena, 1024);

    Parser parser;
    parser_init(&parser, &lexer, &emitter, arena);

    return compile_translation_unit(&parser, out_fns, out_count);
}

static void test_gcc_attributes(void) {
    printf("Testing GCC attributes parsing (__attribute__((packed, aligned(4), section(\"...\"))))... ");
    Arena arena;
    arena_init(&arena, 64 * 1024);

    const char *src =
        "struct __attribute__((packed, aligned(4))) Packet {\n"
        "    int header;\n"
        "    char payload;\n"
        "};\n"
        "__attribute__((section(\".iram0.text\"), noinline)) int get_code(void) {\n"
        "    struct Packet p;\n"
        "    p.header = 100;\n"
        "    p.payload = 42;\n"
        "    return p.header + p.payload;\n"
        "}\n";

    CompiledFunction *fns = NULL;
    size_t count = 0;
    bool ok = compile_string(src, &fns, &count, &arena);
    assert(ok);
    assert(count == 1);
    assert(fns[0].code_size > 0);

    arena_free(&arena);
    printf(COLOR_GREEN "PASSED\n" COLOR_RESET);
}

static void test_statement_expressions(void) {
    printf("Testing GCC statement expressions ({ stmt; stmt; expr; })... ");
    Arena arena;
    arena_init(&arena, 64 * 1024);

    const char *src =
        "int compute_stmt_expr(int x) {\n"
        "    int res = ({ int a = x * 2; int b = a + 10; b * 3; });\n"
        "    return res;\n"
        "}\n";

    CompiledFunction *fns = NULL;
    size_t count = 0;
    bool ok = compile_string(src, &fns, &count, &arena);
    assert(ok);
    assert(count == 1);
    assert(fns[0].code_size > 0);

    arena_free(&arena);
    printf(COLOR_GREEN "PASSED\n" COLOR_RESET);
}

static void test_bitfields(void) {
    printf("Testing struct bitfields (: width, read and write operations)... ");
    Arena arena;
    arena_init(&arena, 64 * 1024);

    const char *src =
        "struct BitfieldReg {\n"
        "    unsigned int mode : 2;\n"
        "    unsigned int enable : 1;\n"
        "    unsigned int val : 5;\n"
        "    unsigned int data : 24;\n"
        "};\n"
        "int test_bf(void) {\n"
        "    struct BitfieldReg reg;\n"
        "    reg.mode = 3;\n"
        "    reg.enable = 1;\n"
        "    reg.val = 15;\n"
        "    reg.data = 1000;\n"
        "    return reg.mode + reg.enable + reg.val + reg.data;\n"
        "}\n";

    CompiledFunction *fns = NULL;
    size_t count = 0;
    bool ok = compile_string(src, &fns, &count, &arena);
    assert(ok);
    assert(count == 1);
    assert(fns[0].code_size > 0);

    arena_free(&arena);
    printf(COLOR_GREEN "PASSED\n" COLOR_RESET);
}

static void test_unions(void) {
    printf("Testing union declarations and shared layout... ");
    Arena arena;
    arena_init(&arena, 64 * 1024);

    const char *src =
        "union WordOverlay {\n"
        "    int full_word;\n"
        "    char byte0;\n"
        "};\n"
        "int test_union(void) {\n"
        "    union WordOverlay u;\n"
        "    u.full_word = 0x12345678;\n"
        "    return u.byte0;\n"
        "}\n";

    CompiledFunction *fns = NULL;
    size_t count = 0;
    bool ok = compile_string(src, &fns, &count, &arena);
    assert(ok);
    assert(count == 1);
    assert(fns[0].code_size > 0);

    arena_free(&arena);
    printf(COLOR_GREEN "PASSED\n" COLOR_RESET);
}

static void test_designated_initializers(void) {
    printf("Testing C99 designated initializers (.field = val, [idx] = val)... ");
    Arena arena;
    arena_init(&arena, 64 * 1024);

    const char *src =
        "struct Point {\n"
        "    int x;\n"
        "    int y;\n"
        "};\n"
        "int test_desig(void) {\n"
        "    struct Point pt = { .y = 200, .x = 50 };\n"
        "    int arr[8] = { [2] = 40, [5] = 60 };\n"
        "    return pt.x + pt.y + arr[2] + arr[5];\n"
        "}\n";

    CompiledFunction *fns = NULL;
    size_t count = 0;
    bool ok = compile_string(src, &fns, &count, &arena);
    assert(ok);
    assert(count == 1);
    assert(fns[0].code_size > 0);

    arena_free(&arena);
    printf(COLOR_GREEN "PASSED\n" COLOR_RESET);
}

static void test_function_pointers(void) {
    printf("Testing function pointers and indirect calls (CALLX0)... ");
    Arena arena;
    arena_init(&arena, 64 * 1024);

    const char *src =
        "int add_ten(int a) {\n"
        "    return a + 10;\n"
        "}\n"
        "int invoke_cb(void) {\n"
        "    int (*cb)(int) = add_ten;\n"
        "    return cb(42);\n"
        "}\n";

    CompiledFunction *fns = NULL;
    size_t count = 0;
    bool ok = compile_string(src, &fns, &count, &arena);
    assert(ok);
    assert(count == 2);

    arena_free(&arena);
    printf(COLOR_GREEN "PASSED\n" COLOR_RESET);
}

static void test_inline_asm(void) {
    printf("Testing inline assembly __asm__ volatile (\"memw\")... ");
    Arena arena;
    arena_init(&arena, 64 * 1024);

    const char *src =
        "int test_sync(void) {\n"
        "    int x = 100;\n"
        "    __asm__ volatile (\"memw\");\n"
        "    __asm__ volatile (\"nop\");\n"
        "    __asm__ volatile (\"isync\");\n"
        "    return x + 5;\n"
        "}\n";

    CompiledFunction *fns = NULL;
    size_t count = 0;
    bool ok = compile_string(src, &fns, &count, &arena);
    assert(ok);
    assert(count == 1);
    assert(fns[0].code_size > 0);

    arena_free(&arena);
    printf(COLOR_GREEN "PASSED\n" COLOR_RESET);
}

static void test_builtins(void) {
    printf("Testing GCC built-ins (__builtin_expect, __builtin_bswap32)... ");
    Arena arena;
    arena_init(&arena, 64 * 1024);

    const char *src =
        "int test_builtins(int x) {\n"
        "    if (__builtin_expect(x == 42, 1)) {\n"
        "        return __builtin_bswap32(0x12345678);\n"
        "    }\n"
        "    return 0;\n"
        "}\n";

    CompiledFunction *fns = NULL;
    size_t count = 0;
    bool ok = compile_string(src, &fns, &count, &arena);
    assert(ok);
    assert(count == 1);
    assert(fns[0].code_size > 0);

    arena_free(&arena);
    printf(COLOR_GREEN "PASSED\n" COLOR_RESET);
}

int main(void) {
    printf("============================================================\n");
    printf(" Phase 14: Full ESP-IDF Compatibility & Optimizations Test Suite\n");
    printf("============================================================\n");

    test_gcc_attributes();
    test_statement_expressions();
    test_bitfields();
    test_unions();
    test_designated_initializers();
    test_function_pointers();
    test_inline_asm();
    test_builtins();

    printf("============================================================\n");
    printf(COLOR_GREEN " ALL PHASE 14 COMPATIBILITY TESTS PASSED!\n" COLOR_RESET);
    printf("============================================================\n");
    return 0;
}

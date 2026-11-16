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

static void dump_binary(const char *filename, const uint8_t *code, size_t size) {
    FILE *f = fopen(filename, "wb");
    if (f) {
        fwrite(code, 1, size, f);
        fclose(f);
    }
}

static bool compile_source(const char *src, CompiledFunction **out_fns, size_t *out_count, Arena *a) {
    Lexer lexer;
    lexer_init(&lexer, src, a);
    lexer_add_include_path(&lexer, "include");

    Emitter emitter;
    emit_init(&emitter, a, 1024);

    Parser parser;
    parser_init(&parser, &lexer, &emitter, a);

    bool res = compile_translation_unit(&parser, out_fns, out_count);
    if (!res) {
        fprintf(stderr, "\nPARSER ERROR: %s (token: %s '%.*s' at line %u, col %u)\n",
                parser.error_msg,
                token_type_name(parser.current_token.type),
                (int)parser.current_token.length,
                parser.current_token.start ? parser.current_token.start : "",
                parser.current_token.line,
                parser.current_token.col);
    }
    return res;
}

static void test_cpp_class_and_methods(void) {
    printf("Running %-48s ... ", __func__);
    Arena arena;
    arena_init(&arena, 64 * 1024);

    const char *src =
        "class Counter {\n"
        "private:\n"
        "    int val;\n"
        "public:\n"
        "    int get() {\n"
        "        return this->val;\n"
        "    }\n"
        "};\n"
        "int Counter::get_val() {\n"
        "    return this->val;\n"
        "}\n"
        "int test_method() {\n"
        "    Counter c;\n"
        "    return c.get_val();\n"
        "}\n";

    CompiledFunction *fns = NULL;
    size_t count = 0;
    bool ok = compile_source(src, &fns, &count, &arena);
    assert(ok);
    assert(count == 2);
    assert(fns[0].code_size > 0);
    assert(fns[1].code_size > 0);

    dump_binary("build/test_cpp_class.bin", fns[1].code, fns[1].code_size);

    arena_free(&arena);
    printf("PASSED\n");
}

static void test_cpp_references(void) {
    printf("Running %-48s ... ", __func__);
    Arena arena;
    arena_init(&arena, 64 * 1024);

    const char *src =
        "int test_ref() {\n"
        "    int a = 100;\n"
        "    int &ref = a;\n"
        "    ref = 42;\n"
        "    return a;\n"
        "}\n";

    CompiledFunction *fns = NULL;
    size_t count = 0;
    bool ok = compile_source(src, &fns, &count, &arena);
    assert(ok && count == 1);
    assert(strcmp(fns[0].name, "test_ref") == 0);
    assert(fns[0].code_size > 0);

    dump_binary("build/test_cpp_ref.bin", fns[0].code, fns[0].code_size);

    arena_free(&arena);
    printf("PASSED\n");
}

static void test_cpp_constructors(void) {
    printf("Running %-48s ... ", __func__);
    Arena arena;
    arena_init(&arena, 64 * 1024);

    const char *src =
        "class Point {\n"
        "public:\n"
        "    int x;\n"
        "    int y;\n"
        "    int sum();\n"
        "};\n"
        "int Point::Point(int px, int py) {\n"
        "    this->x = px;\n"
        "    this->y = py;\n"
        "    return 0;\n"
        "}\n"
        "int Point::sum() {\n"
        "    return this->x + this->y;\n"
        "}\n"
        "int test_point() {\n"
        "    Point pt(15, 25);\n"
        "    return pt.sum();\n"
        "}\n";

    CompiledFunction *fns = NULL;
    size_t count = 0;
    bool ok = compile_source(src, &fns, &count, &arena);
    assert(ok && count == 3);
    assert(fns[0].code_size > 0);
    assert(fns[1].code_size > 0);
    assert(fns[2].code_size > 0);

    dump_binary("build/test_cpp_ctor.bin", fns[2].code, fns[2].code_size);

    arena_free(&arena);
    printf("PASSED\n");
}

static void test_cpp_namespaces(void) {
    printf("Running %-48s ... ", __func__);
    Arena arena;
    arena_init(&arena, 64 * 1024);

    const char *src =
        "namespace Math {\n"
        "    int square(int x) {\n"
        "        return x * x;\n"
        "    }\n"
        "}\n"
        "int test_ns() {\n"
        "    return Math::square(5);\n"
        "}\n";

    CompiledFunction *fns = NULL;
    size_t count = 0;
    bool ok = compile_source(src, &fns, &count, &arena);
    assert(ok && count == 2);
    assert(strcmp(fns[0].name, "Math_square") == 0);
    assert(strcmp(fns[1].name, "test_ns") == 0);
    assert(fns[1].code_size > 0);

    dump_binary("build/test_cpp_ns.bin", fns[1].code, fns[1].code_size);

    arena_free(&arena);
    printf("PASSED\n");
}

static void test_cpp_extern_c(void) {
    printf("Running %-48s ... ", __func__);
    Arena arena;
    arena_init(&arena, 64 * 1024);

    const char *src =
        "extern \"C\" {\n"
        "    int c_abi_func() {\n"
        "        return 777;\n"
        "    }\n"
        "}\n";

    CompiledFunction *fns = NULL;
    size_t count = 0;
    bool ok = compile_source(src, &fns, &count, &arena);
    assert(ok && count == 1);
    assert(strcmp(fns[0].name, "c_abi_func") == 0);
    assert(fns[0].code_size > 0);

    dump_binary("build/test_cpp_extern_c.bin", fns[0].code, fns[0].code_size);

    arena_free(&arena);
    printf("PASSED\n");
}

int main(void) {
    printf("========================================================\n");
    printf(" Phase 13 Tests: Embedded C++ (EC++) Support\n");
    printf("========================================================\n");

    test_cpp_class_and_methods();
    test_cpp_references();
    test_cpp_constructors();
    test_cpp_namespaces();
    test_cpp_extern_c();

    printf("========================================================\n");
    printf(" All Phase 13 C++ tests passed successfully!\n");
    printf("========================================================\n");
    return 0;
}

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdint.h>

#include "arena.h"
#include "lexer.h"

static int g_tests_run = 0;
static int g_tests_passed = 0;

#define TEST(name) static void name(void)
#define RUN_TEST(name) do { \
    g_tests_run++; \
    printf("Running %-40s ... ", #name); \
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

// ============================================================================
// Arena Tests
// ============================================================================

TEST(test_arena_basic_allocation) {
    Arena a;
    ASSERT(arena_init(&a, 1024));
    ASSERT(a.capacity == 1024);
    ASSERT(a.offset == 0);
    ASSERT(a.buffer != NULL);

    // Test 8-byte alignment on allocations of odd sizes
    void *p1 = arena_alloc(&a, 1);
    ASSERT(p1 != NULL);
    ASSERT(((uintptr_t)p1 % ARENA_ALIGNMENT) == 0);

    void *p2 = arena_alloc(&a, 3);
    ASSERT(p2 != NULL);
    ASSERT(((uintptr_t)p2 % ARENA_ALIGNMENT) == 0);
    ASSERT((uint8_t *)p2 >= (uint8_t *)p1 + 1);

    void *p3 = arena_alloc(&a, 17);
    ASSERT(p3 != NULL);
    ASSERT(((uintptr_t)p3 % ARENA_ALIGNMENT) == 0);

    // Verify non-overlapping writes
    memset(p1, 0xAA, 1);
    memset(p2, 0xBB, 3);
    memset(p3, 0xCC, 17);

    ASSERT(*(uint8_t *)p1 == 0xAA);
    ASSERT(*(uint8_t *)p2 == 0xBB);
    ASSERT(*(uint8_t *)p3 == 0xCC);

    arena_free(&a);
    ASSERT(a.buffer == NULL);
    ASSERT(a.capacity == 0);
    ASSERT(a.offset == 0);
}

TEST(test_arena_zero_and_strings) {
    Arena a;
    ASSERT(arena_init(&a, 512));

    uint32_t *zeros = (uint32_t *)arena_alloc_zero(&a, 4 * sizeof(uint32_t));
    ASSERT(zeros != NULL);
    for (int i = 0; i < 4; i++) {
        ASSERT(zeros[i] == 0);
    }

    const char *orig = "Xtensa LX7 ESP32";
    char *dup = arena_strdup(&a, orig);
    ASSERT(dup != NULL);
    ASSERT(strcmp(dup, orig) == 0);
    ASSERT(dup != orig);

    char *ndup = arena_strndup(&a, orig, 6);
    ASSERT(ndup != NULL);
    ASSERT(strcmp(ndup, "Xtensa") == 0);

    arena_free(&a);
}

TEST(test_arena_reset_and_capacity_limits) {
    Arena a;
    ASSERT(arena_init(&a, 64));

    void *p1 = arena_alloc(&a, 32);
    ASSERT(p1 != NULL);
    ASSERT(a.offset == 32);

    void *p2 = arena_alloc(&a, 32);
    ASSERT(p2 != NULL);

    // Arena is now full; subsequent allocation must return NULL
    void *p3 = arena_alloc(&a, 8);
    ASSERT(p3 == NULL);

    // Reset arena and reuse
    arena_reset(&a);
    ASSERT(a.offset == 0);

    void *p4 = arena_alloc(&a, 48);
    ASSERT(p4 != NULL);
    ASSERT(p4 == p1); // Reuses the buffer from start

    arena_free(&a);
}

// ============================================================================
// Lexer Tests
// ============================================================================

TEST(test_lexer_sample_c_function) {
    const char *src =
        "int main() {\n"
        "    int a = 40 + 2;\n"
        "    return a;\n"
        "}\n";

    Lexer l;
    lexer_init(&l, src, NULL);
    Token tok;

    // 1. int
    ASSERT(lexer_next_token(&l, &tok));
    ASSERT(tok.type == TOK_INT);
    ASSERT(tok.line == 1);
    ASSERT(tok.col == 1);

    // 2. main
    ASSERT(lexer_next_token(&l, &tok));
    ASSERT(tok.type == TOK_IDENT);
    ASSERT(token_ident_equals(&tok, "main"));
    ASSERT(tok.line == 1);
    ASSERT(tok.col == 5);

    // 3. (
    ASSERT(lexer_next_token(&l, &tok));
    ASSERT(tok.type == TOK_LPAREN);
    ASSERT(tok.line == 1);
    ASSERT(tok.col == 9);

    // 4. )
    ASSERT(lexer_next_token(&l, &tok));
    ASSERT(tok.type == TOK_RPAREN);
    ASSERT(tok.line == 1);
    ASSERT(tok.col == 10);

    // 5. {
    ASSERT(lexer_next_token(&l, &tok));
    ASSERT(tok.type == TOK_LBRACE);
    ASSERT(tok.line == 1);
    ASSERT(tok.col == 12);

    // 6. int
    ASSERT(lexer_next_token(&l, &tok));
    ASSERT(tok.type == TOK_INT);
    ASSERT(tok.line == 2);
    ASSERT(tok.col == 5);

    // 7. a
    ASSERT(lexer_next_token(&l, &tok));
    ASSERT(tok.type == TOK_IDENT);
    ASSERT(token_ident_equals(&tok, "a"));
    ASSERT(tok.line == 2);
    ASSERT(tok.col == 9);

    // 8. =
    ASSERT(lexer_next_token(&l, &tok));
    ASSERT(tok.type == TOK_ASSIGN);
    ASSERT(tok.line == 2);
    ASSERT(tok.col == 11);

    // 9. 40
    ASSERT(lexer_next_token(&l, &tok));
    ASSERT(tok.type == TOK_INT_LIT);
    ASSERT(tok.int_val == 40);
    ASSERT(tok.line == 2);
    ASSERT(tok.col == 13);

    // 10. +
    ASSERT(lexer_next_token(&l, &tok));
    ASSERT(tok.type == TOK_PLUS);
    ASSERT(tok.line == 2);
    ASSERT(tok.col == 16);

    // 11. 2
    ASSERT(lexer_next_token(&l, &tok));
    ASSERT(tok.type == TOK_INT_LIT);
    ASSERT(tok.int_val == 2);
    ASSERT(tok.line == 2);
    ASSERT(tok.col == 18);

    // 12. ;
    ASSERT(lexer_next_token(&l, &tok));
    ASSERT(tok.type == TOK_SEMICOLON);
    ASSERT(tok.line == 2);
    ASSERT(tok.col == 19);

    // 13. return
    ASSERT(lexer_next_token(&l, &tok));
    ASSERT(tok.type == TOK_RETURN);
    ASSERT(tok.line == 3);
    ASSERT(tok.col == 5);

    // 14. a
    ASSERT(lexer_next_token(&l, &tok));
    ASSERT(tok.type == TOK_IDENT);
    ASSERT(token_ident_equals(&tok, "a"));
    ASSERT(tok.line == 3);
    ASSERT(tok.col == 12);

    // 15. ;
    ASSERT(lexer_next_token(&l, &tok));
    ASSERT(tok.type == TOK_SEMICOLON);
    ASSERT(tok.line == 3);
    ASSERT(tok.col == 13);

    // 16. }
    ASSERT(lexer_next_token(&l, &tok));
    ASSERT(tok.type == TOK_RBRACE);
    ASSERT(tok.line == 4);
    ASSERT(tok.col == 1);

    // 17. EOF
    ASSERT(!lexer_next_token(&l, &tok));
    ASSERT(tok.type == TOK_EOF);
}

TEST(test_lexer_operators_and_comments) {
    const char *src =
        "// Single-line comment\n"
        "/* Multi-line\n"
        "   comment */\n"
        "== != <= >= << >> < > + - * / % & | ^ ~ ! = ; , [ ]";

    Lexer l;
    lexer_init(&l, src, NULL);
    Token tok;

    TokenType expected[] = {
        TOK_EQ, TOK_NEQ, TOK_LE, TOK_GE, TOK_LSHIFT, TOK_RSHIFT,
        TOK_LT, TOK_GT, TOK_PLUS, TOK_MINUS, TOK_STAR, TOK_SLASH,
        TOK_PERCENT, TOK_AMP, TOK_PIPE, TOK_CARET, TOK_TILDE,
        TOK_EXCL, TOK_ASSIGN, TOK_SEMICOLON, TOK_COMMA,
        TOK_LBRACKET, TOK_RBRACKET
    };

    size_t count = sizeof(expected) / sizeof(expected[0]);
    for (size_t i = 0; i < count; i++) {
        ASSERT(lexer_next_token(&l, &tok));
        if (tok.type != expected[i]) {
            fprintf(stderr, "Expected %s, got %s (index %zu)\n",
                    token_type_name(expected[i]),
                    token_type_name(tok.type),
                    i);
            exit(1);
        }
    }

    ASSERT(!lexer_next_token(&l, &tok));
    ASSERT(tok.type == TOK_EOF);
}

TEST(test_lexer_number_formats) {
    const char *src = "0 42 0x2A 0XFF 0b1010 077";

    Lexer l;
    lexer_init(&l, src, NULL);
    Token tok;

    int64_t expected_vals[] = { 0, 42, 42, 255, 10, 63 };
    size_t count = sizeof(expected_vals) / sizeof(expected_vals[0]);

    for (size_t i = 0; i < count; i++) {
        ASSERT(lexer_next_token(&l, &tok));
        ASSERT(tok.type == TOK_INT_LIT);
        ASSERT(tok.int_val == expected_vals[i]);
    }

    ASSERT(!lexer_next_token(&l, &tok));
    ASSERT(tok.type == TOK_EOF);
}

TEST(test_lexer_keywords_and_identifiers) {
    const char *src = "int return void if else while for integer return_val _var123";

    Lexer l;
    lexer_init(&l, src, NULL);
    Token tok;

    TokenType expected_types[] = {
        TOK_INT, TOK_RETURN, TOK_VOID, TOK_IF, TOK_ELSE, TOK_WHILE, TOK_FOR,
        TOK_IDENT, TOK_IDENT, TOK_IDENT
    };

    const char *expected_names[] = {
        "int", "return", "void", "if", "else", "while", "for",
        "integer", "return_val", "_var123"
    };

    size_t count = sizeof(expected_types) / sizeof(expected_types[0]);
    for (size_t i = 0; i < count; i++) {
        ASSERT(lexer_next_token(&l, &tok));
        ASSERT(tok.type == expected_types[i]);
        ASSERT(token_ident_equals(&tok, expected_names[i]));
    }

    ASSERT(!lexer_next_token(&l, &tok));
    ASSERT(tok.type == TOK_EOF);
}

// ============================================================================
// Main
// ============================================================================

int main(void) {
    printf("========================================\n");
    printf(" Phase 1 Unit Tests: Arena & Lexer\n");
    printf("========================================\n");

    RUN_TEST(test_arena_basic_allocation);
    RUN_TEST(test_arena_zero_and_strings);
    RUN_TEST(test_arena_reset_and_capacity_limits);
    RUN_TEST(test_lexer_sample_c_function);
    RUN_TEST(test_lexer_operators_and_comments);
    RUN_TEST(test_lexer_number_formats);
    RUN_TEST(test_lexer_keywords_and_identifiers);

    printf("========================================\n");
    printf(" All %d tests passed successfully!\n", g_tests_passed);
    printf("========================================\n");
    return 0;
}

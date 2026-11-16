#ifndef PARSER_H
#define PARSER_H

#include "lexer.h"
#include "xtensa_emitter.h"
#include "symbol_table.h"
#include "arena.h"

typedef struct {
    const char *name;
    uint8_t *code;
    size_t code_size;
    RelocEntry *relocs;
    size_t reloc_count;
} CompiledFunction;

typedef enum {
    CTX_LOOP,
    CTX_SWITCH
} ContextType;

typedef struct SwitchCase {
    int32_t val;
    size_t body_pos;
} SwitchCase;

typedef struct BreakContext {
    ContextType type;
    size_t continue_target_offset;      // Offset to jump to on continue (CTX_LOOP only)
    size_t *continue_fixup_offsets;
    size_t continue_fixup_count;
    size_t continue_fixup_capacity;

    size_t *break_fixup_offsets;
    size_t break_count;
    size_t break_capacity;

    // For CTX_SWITCH
    int discriminant_slot;
    SwitchCase *cases;
    size_t case_count;
    size_t case_capacity;
    size_t default_pos;
    bool has_default;

    struct BreakContext *parent;
} BreakContext;

typedef struct {
    Lexer *lexer;
    Emitter *emitter;
    Arena *arena;
    SymbolTable symtab;
    Token current_token;

    // Type of the most recently evaluated expression
    DataType last_expr_type;

    // Return jump patches for current function
    size_t *return_patches;
    size_t return_patch_count;
    size_t return_patch_capacity;

    // Active break/continue context stack (loops and switches)
    BreakContext *current_break_ctx;

    // Expression register depth
    int expr_depth;

    // Diagnostics
    bool has_error;
    char error_msg[256];
} Parser;

/**
 * Initialize parser with lexer, emitter, and memory arena.
 */
void parser_init(Parser *p, Lexer *l, Emitter *e, Arena *a);

/**
 * Compile a single expression into accumulator register a2.
 */
bool compile_expression(Parser *p);

/**
 * Compile a statement (declaration, assignment, if, while, return, block).
 */
bool compile_statement(Parser *p);

/**
 * Compile a single top-level function or skip prototype.
 */
bool compile_function(Parser *p, CompiledFunction *out_fn);

/**
 * Compile all top-level functions in the translation unit.
 */
bool compile_translation_unit(Parser *p, CompiledFunction **out_fns, size_t *out_count);

#endif // PARSER_H

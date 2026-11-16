#include "parser.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static const Reg SCRATCH_REGS[] = { XTENSA_A8, XTENSA_A9, XTENSA_A10 };
#define MAX_SCRATCH_REGS 3

static void advance_token(Parser *p) {
    lexer_next_token(p->lexer, &p->current_token);
}

static bool match(Parser *p, TokenType type) {
    if (p->current_token.type == type) {
        advance_token(p);
        return true;
    }
    p->has_error = true;
    snprintf(p->error_msg, sizeof(p->error_msg),
             "Line %u, Col %u: Expected %s, got %s",
             p->current_token.line, p->current_token.col,
             token_type_name(type), token_type_name(p->current_token.type));
    return false;
}

static bool ensure_return_patch_capacity(Parser *p, size_t needed) {
    if (!p || !p->arena) return false;
    if (p->return_patch_count + needed <= p->return_patch_capacity) {
        return true;
    }
    size_t new_cap = p->return_patch_capacity ? p->return_patch_capacity * 2 : 16;
    while (new_cap < p->return_patch_count + needed) {
        new_cap *= 2;
    }
    size_t *new_patches = (size_t *)arena_alloc(p->arena, new_cap * sizeof(size_t));
    if (!new_patches) return false;
    if (p->return_patches && p->return_patch_count > 0) {
        memcpy(new_patches, p->return_patches, p->return_patch_count * sizeof(size_t));
    }
    p->return_patches = new_patches;
    p->return_patch_capacity = new_cap;
    return true;
}

static void add_return_patch(Parser *p, size_t inst_pos) {
    if (ensure_return_patch_capacity(p, 1)) {
        p->return_patches[p->return_patch_count++] = inst_pos;
    }
}

static void enter_break_context(Parser *p, BreakContext *ctx, ContextType type, size_t continue_target) {
    ctx->type = type;
    ctx->continue_target_offset = continue_target;
    ctx->continue_fixup_offsets = NULL;
    ctx->continue_fixup_count = 0;
    ctx->continue_fixup_capacity = 0;

    ctx->break_fixup_offsets = NULL;
    ctx->break_count = 0;
    ctx->break_capacity = 0;

    ctx->discriminant_slot = 0;
    ctx->cases = NULL;
    ctx->case_count = 0;
    ctx->case_capacity = 0;
    ctx->default_pos = 0;
    ctx->has_default = false;

    ctx->parent = p->current_break_ctx;
    p->current_break_ctx = ctx;
}

static void exit_break_context(Parser *p, size_t exit_target) {
    if (!p->current_break_ctx) return;
    BreakContext *ctx = p->current_break_ctx;
    for (size_t i = 0; i < ctx->break_count; i++) {
        emit_patch_branch(p->emitter, ctx->break_fixup_offsets[i], exit_target);
    }
    p->current_break_ctx = ctx->parent;
}

static void add_break_fixup(Parser *p, size_t inst_pos) {
    if (!p->current_break_ctx) return;
    BreakContext *ctx = p->current_break_ctx;
    if (ctx->break_count >= ctx->break_capacity) {
        size_t new_cap = ctx->break_capacity == 0 ? 4 : ctx->break_capacity * 2;
        size_t *new_arr = (size_t *)arena_alloc(p->arena, new_cap * sizeof(size_t));
        if (ctx->break_count > 0 && ctx->break_fixup_offsets) {
            memcpy(new_arr, ctx->break_fixup_offsets, ctx->break_count * sizeof(size_t));
        }
        ctx->break_fixup_offsets = new_arr;
        ctx->break_capacity = new_cap;
    }
    ctx->break_fixup_offsets[ctx->break_count++] = inst_pos;
}

static void add_continue_fixup(Parser *p, BreakContext *loop, size_t inst_pos) {
    if (!loop) return;
    if (loop->continue_fixup_count >= loop->continue_fixup_capacity) {
        size_t new_cap = loop->continue_fixup_capacity == 0 ? 4 : loop->continue_fixup_capacity * 2;
        size_t *new_arr = (size_t *)arena_alloc(p->arena, new_cap * sizeof(size_t));
        if (loop->continue_fixup_count > 0 && loop->continue_fixup_offsets) {
            memcpy(new_arr, loop->continue_fixup_offsets, loop->continue_fixup_count * sizeof(size_t));
        }
        loop->continue_fixup_offsets = new_arr;
        loop->continue_fixup_capacity = new_cap;
    }
    loop->continue_fixup_offsets[loop->continue_fixup_count++] = inst_pos;
}

static void patch_continue_fixups(Parser *p, BreakContext *loop, size_t continue_target) {
    if (!loop) return;
    for (size_t i = 0; i < loop->continue_fixup_count; i++) {
        emit_patch_branch(p->emitter, loop->continue_fixup_offsets[i], continue_target);
    }
}

static BreakContext *find_active_loop(Parser *p) {
    BreakContext *curr = p->current_break_ctx;
    while (curr) {
        if (curr->type == CTX_LOOP) {
            return curr;
        }
        curr = curr->parent;
    }
    return NULL;
}

static BreakContext *find_active_switch(Parser *p) {
    BreakContext *curr = p->current_break_ctx;
    while (curr) {
        if (curr->type == CTX_SWITCH) {
            return curr;
        }
        curr = curr->parent;
    }
    return NULL;
}

static void switch_add_case(Parser *p, BreakContext *sw, int32_t val, size_t body_pos) {
    if (!sw) return;
    if (sw->case_count >= sw->case_capacity) {
        size_t new_cap = sw->case_capacity == 0 ? 8 : sw->case_capacity * 2;
        SwitchCase *new_arr = (SwitchCase *)arena_alloc(p->arena, new_cap * sizeof(SwitchCase));
        if (sw->case_count > 0 && sw->cases) {
            memcpy(new_arr, sw->cases, sw->case_count * sizeof(SwitchCase));
        }
        sw->cases = new_arr;
        sw->case_capacity = new_cap;
    }
    sw->cases[sw->case_count].val = val;
    sw->cases[sw->case_count].body_pos = body_pos;
    sw->case_count++;
}

void parser_init(Parser *p, Lexer *l, Emitter *e, Arena *a) {
    if (!p) return;
    p->lexer = l;
    p->emitter = e;
    p->arena = a;
    symtab_init(&p->symtab, a);
    p->last_expr_type = type_int();
    p->return_patches = NULL;
    p->return_patch_count = 0;
    p->return_patch_capacity = 0;
    p->current_break_ctx = NULL;
    p->expr_depth = 0;
    p->has_error = false;
    p->error_msg[0] = '\0';

    // Prime the first token
    advance_token(p);
}

static void save_left_operand(Parser *p) {
    if (p->expr_depth < MAX_SCRATCH_REGS) {
        emit_mov_n(p->emitter, SCRATCH_REGS[p->expr_depth], XTENSA_A2);
    } else {
        int spill_offset = p->symtab.current_stack_offset + (p->expr_depth * 4);
        emit_s32i(p->emitter, XTENSA_A2, XTENSA_A1, spill_offset);
    }
    p->expr_depth++;
}

static Reg restore_left_operand(Parser *p) {
    p->expr_depth--;
    if (p->expr_depth < MAX_SCRATCH_REGS) {
        return SCRATCH_REGS[p->expr_depth];
    } else {
        int spill_offset = p->symtab.current_stack_offset + (p->expr_depth * 4);
        emit_l32i(p->emitter, XTENSA_A8, XTENSA_A1, spill_offset);
        return XTENSA_A8;
    }
}

static void skip_attributes(Parser *p) {
    while (p->current_token.type == TOK_ATTRIBUTE ||
           p->current_token.type == TOK_DECLSPEC ||
           p->current_token.type == TOK_EXTENSION) {
        if (p->current_token.type == TOK_EXTENSION) {
            advance_token(p);
            continue;
        }
        advance_token(p); // consume __attribute__ / __declspec
        if (p->current_token.type == TOK_LPAREN) {
            int depth = 0;
            while (!p->has_error && p->current_token.type != TOK_EOF) {
                if (p->current_token.type == TOK_LPAREN) depth++;
                else if (p->current_token.type == TOK_RPAREN) {
                    depth--;
                    if (depth == 0) {
                        advance_token(p);
                        break;
                    }
                }
                advance_token(p);
            }
        }
    }
}

static bool is_type_start(Parser *p) {
    TokenType t = p->current_token.type;
    if (t == TOK_INT || t == TOK_CHAR || t == TOK_VOID || t == TOK_BOOL ||
        t == TOK_SHORT || t == TOK_LONG || t == TOK_SIGNED || t == TOK_UNSIGNED ||
        t == TOK_CONST || t == TOK_VOLATILE || t == TOK_RESTRICT || t == TOK_INLINE ||
        t == TOK_STRUCT || t == TOK_CLASS || t == TOK_UNION || t == TOK_ENUM ||
        t == TOK_FLOAT || t == TOK_DOUBLE || t == TOK_TYPEOF ||
        t == TOK_ATTRIBUTE || t == TOK_DECLSPEC || t == TOK_EXTENSION) {
        return true;
    }
    if (t == TOK_IDENT) {
        if (symtab_lookup_typedef(&p->symtab, p->current_token.start, p->current_token.length) != NULL) return true;
        if (symtab_lookup_struct(&p->symtab, p->current_token.start, p->current_token.length) != NULL) return true;
        return false;
    }
    return false;
}

// Helper to parse types: int, char, void, bool, short, long, float, double, struct, class, union, enum, typedef, qualifiers, pointer stars, references
static bool parse_type(Parser *p, DataType *out_type) {
    skip_attributes(p);

    bool is_vol = false;
    bool is_const = false;
    bool is_unsigned = false;
    bool has_qualifier = false;

    while (true) {
        if (p->current_token.type == TOK_VOLATILE) {
            is_vol = true;
            has_qualifier = true;
            advance_token(p);
        } else if (p->current_token.type == TOK_CONST) {
            is_const = true;
            has_qualifier = true;
            advance_token(p);
        } else if (p->current_token.type == TOK_RESTRICT || p->current_token.type == TOK_INLINE) {
            has_qualifier = true;
            advance_token(p);
        } else if (p->current_token.type == TOK_UNSIGNED) {
            is_unsigned = true;
            has_qualifier = true;
            advance_token(p);
        } else if (p->current_token.type == TOK_SIGNED) {
            has_qualifier = true;
            advance_token(p);
        } else if (p->current_token.type == TOK_ATTRIBUTE || p->current_token.type == TOK_DECLSPEC || p->current_token.type == TOK_EXTENSION) {
            skip_attributes(p);
        } else {
            break;
        }
    }

    BaseType base = BASE_TYPE_INT;
    StructDef *s_def = NULL;
    bool has_base = false;

    if (p->current_token.type == TOK_INT) {
        base = BASE_TYPE_INT;
        has_base = true;
        advance_token(p);
    } else if (p->current_token.type == TOK_CHAR) {
        base = BASE_TYPE_CHAR;
        has_base = true;
        advance_token(p);
    } else if (p->current_token.type == TOK_BOOL) {
        base = BASE_TYPE_BOOL;
        has_base = true;
        advance_token(p);
    } else if (p->current_token.type == TOK_FLOAT) {
        base = BASE_TYPE_FLOAT;
        has_base = true;
        advance_token(p);
    } else if (p->current_token.type == TOK_DOUBLE) {
        base = BASE_TYPE_DOUBLE;
        has_base = true;
        advance_token(p);
    } else if (p->current_token.type == TOK_SHORT) {
        base = BASE_TYPE_SHORT;
        has_base = true;
        advance_token(p);
        if (p->current_token.type == TOK_INT) advance_token(p);
    } else if (p->current_token.type == TOK_LONG) {
        base = BASE_TYPE_LONG;
        has_base = true;
        advance_token(p);
        if (p->current_token.type == TOK_LONG) advance_token(p); // long long
        if (p->current_token.type == TOK_INT) advance_token(p);
    } else if (p->current_token.type == TOK_VOID) {
        base = BASE_TYPE_VOID;
        has_base = true;
        advance_token(p);
    } else if (p->current_token.type == TOK_STRUCT || p->current_token.type == TOK_CLASS || p->current_token.type == TOK_UNION) {
        bool is_class_kw = (p->current_token.type == TOK_CLASS);
        bool is_union_kw = (p->current_token.type == TOK_UNION);
        base = is_union_kw ? BASE_TYPE_UNION : BASE_TYPE_STRUCT;
        has_base = true;
        advance_token(p); // consume 'struct', 'class', or 'union'
        skip_attributes(p);
        if (p->current_token.type == TOK_IDENT) {
            const char *tag = p->current_token.start;
            size_t tag_len = p->current_token.length;
            advance_token(p); // consume tag name

            s_def = symtab_lookup_struct(&p->symtab, tag, tag_len);
            if (!s_def) {
                s_def = symtab_define_struct(&p->symtab, tag, tag_len);
            }
            s_def->is_class = is_class_kw;
            s_def->is_union = is_union_kw;
        }
        skip_attributes(p);
        if (p->current_token.type == TOK_LBRACE) {
            advance_token(p); // consume '{'
            if (!s_def) {
                s_def = symtab_define_struct(&p->symtab, "", 0);
                s_def->is_class = is_class_kw;
                s_def->is_union = is_union_kw;
            }
            bool current_is_public = !is_class_kw;
            while (p->current_token.type != TOK_RBRACE && !p->has_error && p->current_token.type != TOK_EOF) {
                skip_attributes(p);
                if (p->current_token.type == TOK_PUBLIC) {
                    advance_token(p);
                    if (!match(p, TOK_COLON)) return false;
                    current_is_public = true;
                    continue;
                }
                if (p->current_token.type == TOK_PRIVATE || p->current_token.type == TOK_PROTECTED) {
                    advance_token(p);
                    if (!match(p, TOK_COLON)) return false;
                    current_is_public = false;
                    continue;
                }

                DataType mtype;
                if (!parse_type(p, &mtype)) return false;
                const char *mname = p->current_token.start;
                size_t mlen = p->current_token.length;
                if (!match(p, TOK_IDENT)) return false;

                // Check for member method: type name(params...)
                if (p->current_token.type == TOK_LPAREN) {
                    advance_token(p); // consume '('
                    if (p->current_token.type != TOK_RPAREN && p->current_token.type != TOK_VOID) {
                        while (true) {
                            DataType pt;
                            if (!parse_type(p, &pt)) return false;
                            if (p->current_token.type == TOK_IDENT) advance_token(p);
                            if (p->current_token.type == TOK_COMMA) advance_token(p);
                            else break;
                        }
                    } else if (p->current_token.type == TOK_VOID) {
                        advance_token(p);
                    }
                    if (!match(p, TOK_RPAREN)) return false;

                    MethodDef mdef;
                    memset(&mdef, 0, sizeof(mdef));
                    mdef.name = arena_strndup(p->arena, mname, mlen);
                    mdef.name_len = mlen;
                    mdef.mangled_name = mangle_method_name(p->arena, s_def->name, mdef.name);
                    mdef.return_type = mtype;
                    mdef.is_public = current_is_public;
                    struct_add_method(s_def, p->arena, &mdef);

                    if (p->current_token.type == TOK_LBRACE) {
                        int depth = 1;
                        advance_token(p); // consume '{'
                        while (depth > 0 && p->current_token.type != TOK_EOF) {
                            if (p->current_token.type == TOK_LBRACE) depth++;
                            else if (p->current_token.type == TOK_RBRACE) depth--;
                            advance_token(p);
                        }
                    } else {
                        if (!match(p, TOK_SEMICOLON)) return false;
                    }
                    continue;
                }

                // Check for bitfield: unsigned int field : width;
                int bit_width = 0;
                if (p->current_token.type == TOK_COLON) {
                    advance_token(p); // consume ':'
                    if (p->current_token.type == TOK_INT_LIT) {
                        bit_width = (int)p->current_token.int_val;
                        advance_token(p);
                    }
                }

                if (bit_width > 0) {
                    struct_add_bitfield(s_def, p->arena, mname, mlen, mtype, bit_width);
                } else {
                    struct_add_member(s_def, p->arena, mname, mlen, mtype);
                }
                s_def->members[s_def->member_count - 1].is_public = current_is_public;
                skip_attributes(p);
                if (!match(p, TOK_SEMICOLON)) return false;
            }
            if (!match(p, TOK_RBRACE)) return false;
            skip_attributes(p);
            struct_finalize_layout(s_def);
        }
    } else if (p->current_token.type == TOK_ENUM) {
        base = BASE_TYPE_ENUM;
        has_base = true;
        advance_token(p); // consume 'enum'
        if (p->current_token.type == TOK_IDENT) {
            advance_token(p); // consume enum tag
        }
        if (p->current_token.type == TOK_LBRACE) {
            advance_token(p); // consume '{'
            int32_t val = 0;
            while (p->current_token.type != TOK_RBRACE && !p->has_error && p->current_token.type != TOK_EOF) {
                if (p->current_token.type != TOK_IDENT) {
                    p->has_error = true;
                    snprintf(p->error_msg, sizeof(p->error_msg), "Expected enum member identifier");
                    return false;
                }
                const char *ename = p->current_token.start;
                size_t elen = p->current_token.length;
                advance_token(p);

                if (p->current_token.type == TOK_ASSIGN) {
                    advance_token(p); // consume '='
                    bool neg = false;
                    if (p->current_token.type == TOK_MINUS) {
                        neg = true;
                        advance_token(p);
                    }
                    if (p->current_token.type != TOK_INT_LIT) {
                        p->has_error = true;
                        snprintf(p->error_msg, sizeof(p->error_msg), "Expected integer constant in enum definition");
                        return false;
                    }
                    val = (int32_t)p->current_token.int_val;
                    if (neg) val = -val;
                    advance_token(p);
                }

                symtab_define_enum_constant(&p->symtab, ename, elen, val);
                val++;

                if (p->current_token.type == TOK_COMMA) {
                    advance_token(p);
                } else {
                    break;
                }
            }
            if (!match(p, TOK_RBRACE)) return false;
        }
    } else if (p->current_token.type == TOK_TYPEOF) {
        advance_token(p); // consume typeof
        if (!match(p, TOK_LPAREN)) return false;
        if (is_type_start(p)) {
            if (!parse_type(p, out_type)) return false;
        } else {
            if (p->current_token.type == TOK_IDENT) {
                Symbol *s = symtab_lookup(&p->symtab, p->current_token.start, p->current_token.length);
                if (s && out_type) *out_type = s->type;
                advance_token(p);
            }
        }
        if (!match(p, TOK_RPAREN)) return false;
        return true;
    } else if (p->current_token.type == TOK_IDENT) {
        const DataType *tdef = symtab_lookup_typedef(&p->symtab, p->current_token.start, p->current_token.length);
        if (tdef) {
            advance_token(p); // consume typedef name
            if (out_type) *out_type = *tdef;
            return true;
        }
        StructDef *s_lookup = symtab_lookup_struct(&p->symtab, p->current_token.start, p->current_token.length);
        if (s_lookup) {
            base = s_lookup->is_union ? BASE_TYPE_UNION : BASE_TYPE_STRUCT;
            has_base = true;
            s_def = s_lookup;
            advance_token(p); // consume struct/class/union tag name
        }
    }

    if (!has_base) {
        if (has_qualifier) {
            // Default `unsigned` -> `unsigned int`
            base = BASE_TYPE_INT;
            has_base = true;
        } else {
            p->has_error = true;
            snprintf(p->error_msg, sizeof(p->error_msg),
                     "Line %u, Col %u: Expected type specifier",
                     p->current_token.line, p->current_token.col);
            return false;
        }
    }

    int ptr_depth = 0;
    while (p->current_token.type == TOK_STAR || p->current_token.type == TOK_CONST ||
           p->current_token.type == TOK_VOLATILE || p->current_token.type == TOK_RESTRICT ||
           p->current_token.type == TOK_ATTRIBUTE) {
        if (p->current_token.type == TOK_STAR) {
            ptr_depth++;
        } else if (p->current_token.type == TOK_VOLATILE) {
            is_vol = true;
        } else if (p->current_token.type == TOK_CONST) {
            is_const = true;
        } else if (p->current_token.type == TOK_ATTRIBUTE) {
            skip_attributes(p);
            continue;
        }
        advance_token(p);
    }

    skip_attributes(p);
    bool is_ref = false;
    if (p->current_token.type == TOK_AMP) {
        is_ref = true;
        advance_token(p); // consume '&'
    }
    skip_attributes(p);

    if (out_type) {
        if (base == BASE_TYPE_STRUCT || base == BASE_TYPE_UNION) {
            *out_type = make_struct_type(s_def, ptr_depth, is_vol);
        } else {
            *out_type = make_type(base, ptr_depth, is_vol);
        }
        out_type->is_const = is_const;
        out_type->is_unsigned = is_unsigned;
        out_type->is_reference = is_ref;
        out_type->is_class = (s_def && s_def->is_class);
    }
    return true;
}

// Forward declarations for recursive descent expressions
static bool compile_expr_unary(Parser *p);
static bool compile_expr_multiplicative(Parser *p);
static bool compile_expr_additive(Parser *p);
static bool compile_expr_shift(Parser *p);
static bool compile_expr_relational(Parser *p);
static bool compile_expr_equality(Parser *p);
static bool compile_expr_bitwise_and(Parser *p);
static bool compile_expr_bitwise_xor(Parser *p);
static bool compile_expr_bitwise_or(Parser *p);

static bool compile_expr_primary(Parser *p) {
    if (p->has_error) return false;

    if (p->current_token.type == TOK_INT_LIT) {
        int64_t val = p->current_token.int_val;
        advance_token(p);
        emit_load_imm(p->emitter, XTENSA_A2, val);
        p->last_expr_type = type_int();
        return true;
    }

    if (p->current_token.type == TOK_STRING_LIT) {
        const char *s = p->current_token.string_val;
        size_t slen = p->current_token.string_len;
        advance_token(p);
        size_t str_id = emit_add_string(p->emitter, s, slen);
        emit_load_string_addr(p->emitter, XTENSA_A2, str_id);
        p->last_expr_type = make_type(BASE_TYPE_CHAR, 1, false);
        return true;
    }

    if (p->current_token.type == TOK_SIZEOF) {
        advance_token(p); // consume 'sizeof'
        if (!match(p, TOK_LPAREN)) return false;
        DataType t;
        if (!parse_type(p, &t)) return false;
        if (!match(p, TOK_RPAREN)) return false;
        int sz = type_size(t);
        emit_load_imm(p->emitter, XTENSA_A2, sz);
        p->last_expr_type = type_int();
        return true;
    }

    if (p->current_token.type == TOK_THIS) {
        advance_token(p);
        Symbol *sym = symtab_lookup(&p->symtab, "this", 4);
        if (!sym) {
            p->has_error = true;
            snprintf(p->error_msg, sizeof(p->error_msg),
                     "Line %u, Col %u: 'this' used outside of member function",
                     p->current_token.line, p->current_token.col);
            return false;
        }
        emit_l32i(p->emitter, XTENSA_A2, XTENSA_A1, sym->stack_offset);
        p->last_expr_type = sym->type;
        return true;
    }

    if (p->current_token.type == TOK_IDENT) {
        Token next_tok;
        Lexer lookahead = *p->lexer;
        lexer_next_token(&lookahead, &next_tok);

        if (next_tok.type == TOK_COLON_COLON) {
            const char *prefix = p->current_token.start;
            size_t prefix_len = p->current_token.length;
            advance_token(p); // consume prefix
            advance_token(p); // consume '::'

            if (p->current_token.type != TOK_IDENT) {
                p->has_error = true;
                snprintf(p->error_msg, sizeof(p->error_msg), "Expected identifier after '::'");
                return false;
            }
            const char *sym_name = p->current_token.start;
            size_t sym_len = p->current_token.length;
            advance_token(p); // consume sym_name

            char scoped_name[256];
            snprintf(scoped_name, sizeof(scoped_name), "%.*s_%.*s", (int)prefix_len, prefix, (int)sym_len, sym_name);
            const char *mangled = arena_strndup(p->arena, scoped_name, strlen(scoped_name));

            if (p->current_token.type == TOK_LPAREN) {
                advance_token(p); // consume '('
                size_t num_args = 0;
                if (p->current_token.type != TOK_RPAREN && p->current_token.type != TOK_VOID) {
                    while (true) {
                        if (!compile_expression(p)) return false;
                        save_left_operand(p);
                        num_args++;
                        if (p->current_token.type == TOK_COMMA) advance_token(p);
                        else break;
                    }
                } else if (p->current_token.type == TOK_VOID) {
                    advance_token(p);
                }
                if (!match(p, TOK_RPAREN)) return false;

                if (num_args > 0) {
                    for (int i = (int)num_args - 1; i >= 0; i--) {
                        Reg saved = restore_left_operand(p);
                        Reg target = (Reg)(XTENSA_A2 + i);
                        if (saved != target) {
                            emit_mov_n(p->emitter, target, saved);
                        }
                    }
                }

                size_t call_pos = p->emitter->code_size;
                emit_call0(p->emitter, 0);
                emit_add_reloc(p->emitter, call_pos, mangled, strlen(mangled), R_XTENSA_SLOT0_OP, 0);
                p->last_expr_type = type_int();
                return true;
            }
        }

        if (next_tok.type == TOK_LPAREN) {
            // Function Call: ident(arg0, arg1, ...)
            const char *func_name = p->current_token.start;
            size_t func_len = p->current_token.length;
            advance_token(p); // consume function name
            advance_token(p); // consume '('

            // Built-in: __builtin_expect(expr, val)
            if (func_len == 16 && memcmp(func_name, "__builtin_expect", 16) == 0) {
                if (!compile_expression(p)) return false;
                if (p->current_token.type == TOK_COMMA) {
                    advance_token(p);
                    if (!compile_expression(p)) return false;
                }
                if (!match(p, TOK_RPAREN)) return false;
                return true;
            }

            // Built-in: __builtin_bswap32(x)
            if (func_len == 17 && memcmp(func_name, "__builtin_bswap32", 17) == 0) {
                if (!compile_expression(p)) return false;
                if (!match(p, TOK_RPAREN)) return false;
                // Byte swap a2
                emit_srli(p->emitter, XTENSA_A8, XTENSA_A2, 24);
                emit_srli(p->emitter, XTENSA_A9, XTENSA_A2, 8);
                emit_load_imm(p->emitter, XTENSA_A11, 0xFF00);
                emit_and(p->emitter, XTENSA_A9, XTENSA_A9, XTENSA_A11);
                emit_slli(p->emitter, XTENSA_A10, XTENSA_A2, 8);
                emit_load_imm(p->emitter, XTENSA_A11, 0xFF0000);
                emit_and(p->emitter, XTENSA_A10, XTENSA_A10, XTENSA_A11);
                emit_slli(p->emitter, XTENSA_A2, XTENSA_A2, 24);
                emit_or(p->emitter, XTENSA_A2, XTENSA_A2, XTENSA_A8);
                emit_or(p->emitter, XTENSA_A2, XTENSA_A2, XTENSA_A9);
                emit_or(p->emitter, XTENSA_A2, XTENSA_A2, XTENSA_A10);
                p->last_expr_type = type_int();
                return true;
            }

            // Check if calling an in-scope function pointer local variable
            Symbol *fn_sym = symtab_lookup(&p->symtab, func_name, func_len);
            if (fn_sym && (fn_sym->type.is_func_ptr || fn_sym->type.pointer_depth > 0)) {
                emit_l32i(p->emitter, XTENSA_A8, XTENSA_A1, fn_sym->stack_offset);
                save_left_operand(p); // save function target address

                size_t num_args = 0;
                if (p->current_token.type != TOK_RPAREN && p->current_token.type != TOK_VOID) {
                    while (true) {
                        if (!compile_expression(p)) return false;
                        save_left_operand(p);
                        num_args++;
                        if (p->current_token.type == TOK_COMMA) advance_token(p);
                        else break;
                    }
                } else if (p->current_token.type == TOK_VOID) {
                    advance_token(p);
                }
                if (!match(p, TOK_RPAREN)) return false;

                if (num_args > 0) {
                    for (int i = (int)num_args - 1; i >= 0; i--) {
                        Reg saved = restore_left_operand(p);
                        Reg target = (Reg)(XTENSA_A2 + i);
                        if (saved != target) emit_mov_n(p->emitter, target, saved);
                    }
                }
                Reg fn_ptr_reg = restore_left_operand(p);
                emit_callx0(p->emitter, fn_ptr_reg);
                p->last_expr_type = type_int();
                return true;
            }

            size_t num_args = 0;
            if (p->current_token.type != TOK_RPAREN && p->current_token.type != TOK_VOID) {
                while (true) {
                    if (!compile_expression(p)) return false;
                    save_left_operand(p);
                    num_args++;
                    if (p->current_token.type == TOK_COMMA) {
                        advance_token(p);
                    } else {
                        break;
                    }
                }
            } else if (p->current_token.type == TOK_VOID) {
                advance_token(p);
            }
            if (!match(p, TOK_RPAREN)) return false;

            // Restore/move arguments into registers a2..a7
            if (num_args > 0) {
                for (int i = (int)num_args - 1; i >= 0; i--) {
                    Reg saved = restore_left_operand(p);
                    Reg target = (Reg)(XTENSA_A2 + i);
                    if (saved != target) {
                        emit_mov_n(p->emitter, target, saved);
                    }
                }
            }

            size_t call_pos = p->emitter->code_size;
            emit_call0(p->emitter, 0);
            emit_add_reloc(p->emitter, call_pos, func_name, func_len, R_XTENSA_SLOT0_OP, 0);
            p->last_expr_type = type_int();
            return true;
        }

        const char *name = p->current_token.start;
        size_t len = p->current_token.length;

        // Check if identifier is an enum constant
        const EnumConstant *ec = symtab_lookup_enum_constant(&p->symtab, name, len);
        if (ec) {
            advance_token(p);
            emit_load_imm(p->emitter, XTENSA_A2, ec->val);
            p->last_expr_type = type_int();
            return true;
        }

        Symbol *sym = symtab_lookup(&p->symtab, name, len);
        if (!sym) {
            // Function address literal for callbacks / function pointers
            advance_token(p);
            emit_load_literal(p->emitter, XTENSA_A2, 0);
            size_t lit_idx = p->emitter->literal_count - 1;
            emit_add_reloc(p->emitter, 4 + lit_idx * 4, name, len, R_XTENSA_32, 0);
            DataType fn_ptr_t = type_int();
            fn_ptr_t.is_func_ptr = true;
            p->last_expr_type = fn_ptr_t;
            return true;
        }
        advance_token(p);

        if (sym->type.is_reference) {
            // Load target address stored in reference
            emit_l32i(p->emitter, XTENSA_A2, XTENSA_A1, sym->stack_offset);
            // Dereference value
            DataType target_type = sym->type;
            target_type.is_reference = false;
            if (target_type.base == BASE_TYPE_CHAR && target_type.pointer_depth == 0) {
                emit_l8ui(p->emitter, XTENSA_A2, XTENSA_A2, 0);
            } else {
                emit_l32i(p->emitter, XTENSA_A2, XTENSA_A2, 0);
            }
            p->last_expr_type = target_type;
            return true;
        }

        if (sym->type.array_size > 0) {
            // Array decays to pointer: compute address into a2
            if (sym->stack_offset >= -128 && sym->stack_offset <= 127) {
                emit_addi(p->emitter, XTENSA_A2, XTENSA_A1, (int8_t)sym->stack_offset);
            } else {
                emit_load_imm(p->emitter, XTENSA_A11, sym->stack_offset);
                emit_add(p->emitter, XTENSA_A2, XTENSA_A1, XTENSA_A11);
            }
            DataType decayed = sym->type;
            decayed.array_size = 0;
            decayed.pointer_depth++;
            p->last_expr_type = decayed;
        } else if (sym->type.base == BASE_TYPE_STRUCT && sym->type.pointer_depth == 0) {
            // Struct value decays to base stack address in a2
            if (sym->stack_offset >= -128 && sym->stack_offset <= 127) {
                emit_addi(p->emitter, XTENSA_A2, XTENSA_A1, (int8_t)sym->stack_offset);
            } else {
                emit_load_imm(p->emitter, XTENSA_A11, sym->stack_offset);
                emit_add(p->emitter, XTENSA_A2, XTENSA_A1, XTENSA_A11);
            }
            p->last_expr_type = sym->type;
        } else {
            if (sym->type.base == BASE_TYPE_CHAR && sym->type.pointer_depth == 0) {
                emit_l8ui(p->emitter, XTENSA_A2, XTENSA_A1, sym->stack_offset);
            } else {
                emit_l32i(p->emitter, XTENSA_A2, XTENSA_A1, sym->stack_offset);
            }
            p->last_expr_type = sym->type;
        }
        return true;
    }

    if (p->current_token.type == TOK_LPAREN) {
        // Look ahead to check for GCC Statement Expressions: ({ ... }) or Type Casts
        Token next_tok;
        Lexer lookahead = *p->lexer;
        lexer_next_token(&lookahead, &next_tok);

        if (next_tok.type == TOK_LBRACE) {
            advance_token(p); // consume '('
            advance_token(p); // consume '{'
            symtab_enter_scope(&p->symtab);
            while (p->current_token.type != TOK_RBRACE && !p->has_error && p->current_token.type != TOK_EOF) {
                if (!compile_statement(p)) return false;
            }
            symtab_exit_scope(&p->symtab);
            if (!match(p, TOK_RBRACE)) return false;
            if (!match(p, TOK_RPAREN)) return false;
            p->last_expr_type = type_int();
            return true;
        }

        bool is_cast = (next_tok.type == TOK_INT || next_tok.type == TOK_CHAR ||
                        next_tok.type == TOK_VOID || next_tok.type == TOK_BOOL ||
                        next_tok.type == TOK_SHORT || next_tok.type == TOK_LONG ||
                        next_tok.type == TOK_FLOAT || next_tok.type == TOK_DOUBLE ||
                        next_tok.type == TOK_SIGNED || next_tok.type == TOK_UNSIGNED ||
                        next_tok.type == TOK_CONST || next_tok.type == TOK_VOLATILE ||
                        next_tok.type == TOK_RESTRICT || next_tok.type == TOK_STRUCT ||
                        next_tok.type == TOK_UNION || next_tok.type == TOK_ENUM ||
                        (next_tok.type == TOK_IDENT && symtab_lookup_typedef(&p->symtab, next_tok.start, next_tok.length)));
        if (is_cast) {
            advance_token(p); // consume '('
            DataType cast_type;
            if (!parse_type(p, &cast_type)) return false;
            if (!match(p, TOK_RPAREN)) return false;
            if (!compile_expr_unary(p)) return false;
            p->last_expr_type = cast_type;
            return true;
        }

        advance_token(p); // consume '('
        if (!compile_expression(p)) return false;
        return match(p, TOK_RPAREN);
    }

    p->has_error = true;
    snprintf(p->error_msg, sizeof(p->error_msg),
             "Line %u, Col %u: Unexpected token in primary expression '%s'",
             p->current_token.line, p->current_token.col,
             token_type_name(p->current_token.type));
    return false;
}

static bool compile_expr_postfix(Parser *p) {
    if (!compile_expr_primary(p)) return false;

    while (p->current_token.type == TOK_LBRACKET ||
           p->current_token.type == TOK_DOT ||
           p->current_token.type == TOK_ARROW) {

        if (p->current_token.type == TOK_LBRACKET) {
            advance_token(p); // consume '['
            DataType base_type = p->last_expr_type;
            DataType elem_type = type_deref(base_type);

            save_left_operand(p); // Save base address in a2
            if (!compile_expression(p)) return false; // Compute index in a2
            if (!match(p, TOK_RBRACKET)) return false; // consume ']'

            Reg base_reg = restore_left_operand(p); // Restore base address

            // Scale index in a2
            if (elem_type.pointer_depth > 0 || elem_type.base == BASE_TYPE_INT || elem_type.base == BASE_TYPE_STRUCT) {
                int sz = type_size(elem_type);
                if (sz == 4) {
                    emit_add_n(p->emitter, XTENSA_A2, XTENSA_A2, XTENSA_A2);
                    emit_add_n(p->emitter, XTENSA_A2, XTENSA_A2, XTENSA_A2);
                } else if (sz != 1) {
                    emit_load_imm(p->emitter, XTENSA_A11, sz);
                    emit_mull(p->emitter, XTENSA_A2, XTENSA_A2, XTENSA_A11);
                }
            }

            // Add base + scaled_index -> a2
            emit_add(p->emitter, XTENSA_A2, base_reg, XTENSA_A2);

            if (elem_type.base == BASE_TYPE_STRUCT && elem_type.pointer_depth == 0) {
                // Address of struct element is already in a2
            } else if (elem_type.array_size > 0) {
                // Address of sub-array is already in a2
            } else {
                if (elem_type.base == BASE_TYPE_CHAR && elem_type.pointer_depth == 0) {
                    emit_l8ui(p->emitter, XTENSA_A2, XTENSA_A2, 0);
                } else {
                    emit_l32i(p->emitter, XTENSA_A2, XTENSA_A2, 0);
                }
            }

            p->last_expr_type = elem_type;
        } else if (p->current_token.type == TOK_DOT || p->current_token.type == TOK_ARROW) {
            bool is_arrow = (p->current_token.type == TOK_ARROW);
            advance_token(p); // consume '.' or '->'
            if (p->current_token.type != TOK_IDENT) {
                p->has_error = true;
                snprintf(p->error_msg, sizeof(p->error_msg),
                         "Line %u, Col %u: Expected member name after '%s'",
                         p->current_token.line, p->current_token.col, is_arrow ? "->" : ".");
                return false;
            }
            const char *mname = p->current_token.start;
            size_t mlen = p->current_token.length;
            advance_token(p); // consume member name

            StructDef *s_def = p->last_expr_type.struct_def;

            // Check if this is a method call: obj.method(args...) or ptr->method(args...)
            if (p->current_token.type == TOK_LPAREN) {
                advance_token(p); // consume '('
                // 'this' pointer is in a2
                save_left_operand(p); // save 'this'

                size_t num_args = 1;
                if (p->current_token.type != TOK_RPAREN && p->current_token.type != TOK_VOID) {
                    while (true) {
                        if (!compile_expression(p)) return false;
                        save_left_operand(p);
                        num_args++;
                        if (p->current_token.type == TOK_COMMA) advance_token(p);
                        else break;
                    }
                } else if (p->current_token.type == TOK_VOID) {
                    advance_token(p);
                }
                if (!match(p, TOK_RPAREN)) return false;

                if (num_args > 0) {
                    for (int i = (int)num_args - 1; i >= 0; i--) {
                        Reg saved = restore_left_operand(p);
                        Reg target = (Reg)(XTENSA_A2 + i);
                        if (saved != target) {
                            emit_mov_n(p->emitter, target, saved);
                        }
                    }
                }

                char *mangled = mangle_method_name(p->arena, s_def ? s_def->name : "", arena_strndup(p->arena, mname, mlen));
                size_t call_pos = p->emitter->code_size;
                emit_call0(p->emitter, 0);
                emit_add_reloc(p->emitter, call_pos, mangled, strlen(mangled), R_XTENSA_SLOT0_OP, 0);
                p->last_expr_type = type_int();
                continue;
            }

            if (!s_def) {
                p->has_error = true;
                snprintf(p->error_msg, sizeof(p->error_msg),
                         "Line %u: Member access applied to non-struct type", p->current_token.line);
                return false;
            }

            const StructMember *m = struct_lookup_member(s_def, mname, mlen);
            if (!m) {
                p->has_error = true;
                snprintf(p->error_msg, sizeof(p->error_msg),
                         "Line %u: Struct/Class '%s' has no member named '%.*s'",
                         p->current_token.line, s_def ? s_def->name : "", (int)mlen, mname);
                return false;
            }

            // Effective address: a2 = a2 + m->offset
            if (m->offset > 0) {
                if (m->offset >= -128 && m->offset <= 127) {
                    emit_addi(p->emitter, XTENSA_A2, XTENSA_A2, (int8_t)m->offset);
                } else {
                    emit_load_imm(p->emitter, XTENSA_A11, m->offset);
                    emit_add(p->emitter, XTENSA_A2, XTENSA_A2, XTENSA_A11);
                }
            }

            if (m->is_bitfield) {
                emit_l32i(p->emitter, XTENSA_A2, XTENSA_A2, 0);
                if (m->bit_offset > 0) {
                    emit_srli(p->emitter, XTENSA_A2, XTENSA_A2, m->bit_offset);
                }
                uint32_t mask = (1U << m->bit_width) - 1;
                emit_load_imm(p->emitter, XTENSA_A11, mask);
                emit_and(p->emitter, XTENSA_A2, XTENSA_A2, XTENSA_A11);
            } else if (m->type.base == BASE_TYPE_STRUCT && m->type.pointer_depth == 0) {
                // Sub-struct address is in a2
            } else if (m->type.array_size > 0) {
                // Array address is in a2
            } else {
                if (m->type.base == BASE_TYPE_CHAR && m->type.pointer_depth == 0) {
                    emit_l8ui(p->emitter, XTENSA_A2, XTENSA_A2, 0);
                } else {
                    emit_l32i(p->emitter, XTENSA_A2, XTENSA_A2, 0);
                }
            }
            p->last_expr_type = m->type;
        }
    }
    return true;
}

static bool compile_expr_unary(Parser *p) {
    // Dereference: *<expr>
    if (p->current_token.type == TOK_STAR) {
        advance_token(p); // consume '*'
        if (!compile_expr_unary(p)) return false;
        DataType target_type = type_deref(p->last_expr_type);
        if (target_type.base == BASE_TYPE_CHAR && target_type.pointer_depth == 0) {
            emit_l8ui(p->emitter, XTENSA_A2, XTENSA_A2, 0);
        } else {
            emit_l32i(p->emitter, XTENSA_A2, XTENSA_A2, 0);
        }
        p->last_expr_type = target_type;
        return true;
    }

    // Address-of: &<var>
    if (p->current_token.type == TOK_AMP) {
        advance_token(p); // consume '&'
        if (p->current_token.type != TOK_IDENT) {
            p->has_error = true;
            snprintf(p->error_msg, sizeof(p->error_msg),
                     "Line %u, Col %u: Expected identifier after '&'",
                     p->current_token.line, p->current_token.col);
            return false;
        }
        const char *name = p->current_token.start;
        size_t len = p->current_token.length;
        Symbol *sym = symtab_lookup(&p->symtab, name, len);
        if (!sym) {
            p->has_error = true;
            snprintf(p->error_msg, sizeof(p->error_msg),
                     "Line %u, Col %u: Undeclared variable '%.*s'",
                     p->current_token.line, p->current_token.col, (int)len, name);
            return false;
        }
        advance_token(p);
        // Compute address on stack: a2 = a1 + stack_offset
        if (sym->stack_offset >= -128 && sym->stack_offset <= 127) {
            emit_addi(p->emitter, XTENSA_A2, XTENSA_A1, (int8_t)sym->stack_offset);
        } else {
            emit_load_imm(p->emitter, XTENSA_A11, sym->stack_offset);
            emit_add(p->emitter, XTENSA_A2, XTENSA_A1, XTENSA_A11);
        }
        p->last_expr_type = type_ptr_to(sym->type);
        return true;
    }

    if (p->current_token.type == TOK_MINUS) {
        advance_token(p);
        if (!compile_expr_unary(p)) return false;
        emit_neg(p->emitter, XTENSA_A2, XTENSA_A2);
        p->last_expr_type = type_int();
        return true;
    }

    if (p->current_token.type == TOK_TILDE) {
        advance_token(p);
        if (!compile_expr_unary(p)) return false;
        emit_movi(p->emitter, XTENSA_A11, -1);
        emit_xor(p->emitter, XTENSA_A2, XTENSA_A2, XTENSA_A11);
        p->last_expr_type = type_int();
        return true;
    }

    if (p->current_token.type == TOK_EXCL) {
        advance_token(p);
        if (!compile_expr_unary(p)) return false;
        emit_mov_n(p->emitter, XTENSA_A11, XTENSA_A2);
        emit_movi(p->emitter, XTENSA_A2, 0);
        emit_movi(p->emitter, XTENSA_A8, 1);
        emit_moveqz(p->emitter, XTENSA_A2, XTENSA_A8, XTENSA_A11);
        p->last_expr_type = type_int();
        return true;
    }

    if (p->current_token.type == TOK_PLUS) {
        advance_token(p);
        return compile_expr_unary(p);
    }

    return compile_expr_postfix(p);
}

static bool compile_expr_multiplicative(Parser *p) {
    if (!compile_expr_unary(p)) return false;
    DataType left_type = p->last_expr_type;

    while (p->current_token.type == TOK_STAR ||
           p->current_token.type == TOK_SLASH ||
           p->current_token.type == TOK_PERCENT) {
        TokenType op = p->current_token.type;
        advance_token(p);

        save_left_operand(p);
        if (!compile_expr_unary(p)) return false;
        Reg left = restore_left_operand(p);

        if (op == TOK_STAR) {
            emit_mull(p->emitter, XTENSA_A2, left, XTENSA_A2);
        } else if (op == TOK_SLASH) {
            emit_quos(p->emitter, XTENSA_A2, left, XTENSA_A2);
        } else if (op == TOK_PERCENT) {
            emit_rems(p->emitter, XTENSA_A2, left, XTENSA_A2);
        }
        p->last_expr_type = left_type;
    }
    return true;
}

static bool compile_expr_additive(Parser *p) {
    if (!compile_expr_multiplicative(p)) return false;
    DataType left_type = p->last_expr_type;

    while (p->current_token.type == TOK_PLUS ||
           p->current_token.type == TOK_MINUS) {
        TokenType op = p->current_token.type;
        advance_token(p);

        save_left_operand(p);
        if (!compile_expr_multiplicative(p)) return false;
        Reg left = restore_left_operand(p);

        if (op == TOK_PLUS) {
            emit_add(p->emitter, XTENSA_A2, left, XTENSA_A2);
        } else if (op == TOK_MINUS) {
            emit_sub(p->emitter, XTENSA_A2, left, XTENSA_A2);
        }
        p->last_expr_type = left_type;
    }
    return true;
}

static bool compile_expr_shift(Parser *p) {
    if (!compile_expr_additive(p)) return false;

    while (p->current_token.type == TOK_LSHIFT ||
           p->current_token.type == TOK_RSHIFT) {
        TokenType op = p->current_token.type;
        advance_token(p);

        save_left_operand(p);
        if (!compile_expr_additive(p)) return false;
        Reg left = restore_left_operand(p);

        if (op == TOK_LSHIFT) {
            emit_ssl(p->emitter, XTENSA_A2);
            emit_sll(p->emitter, XTENSA_A2, left);
        } else if (op == TOK_RSHIFT) {
            emit_ssr(p->emitter, XTENSA_A2);
            emit_sra(p->emitter, XTENSA_A2, left);
        }
        p->last_expr_type = type_int();
    }
    return true;
}

static bool compile_expr_relational(Parser *p) {
    if (!compile_expr_shift(p)) return false;

    while (p->current_token.type == TOK_LT ||
           p->current_token.type == TOK_LE ||
           p->current_token.type == TOK_GT ||
           p->current_token.type == TOK_GE) {
        TokenType op = p->current_token.type;
        advance_token(p);

        save_left_operand(p);
        if (!compile_expr_shift(p)) return false;
        Reg left = restore_left_operand(p);

        if (op == TOK_LT) {
            emit_sub(p->emitter, left, left, XTENSA_A2);
            emit_movi(p->emitter, XTENSA_A2, 0);
            emit_movi(p->emitter, XTENSA_A11, 1);
            emit_movltz(p->emitter, XTENSA_A2, XTENSA_A11, left);
        } else if (op == TOK_GE) {
            emit_sub(p->emitter, left, left, XTENSA_A2);
            emit_movi(p->emitter, XTENSA_A2, 0);
            emit_movi(p->emitter, XTENSA_A11, 1);
            emit_movgez(p->emitter, XTENSA_A2, XTENSA_A11, left);
        } else if (op == TOK_GT) {
            emit_sub(p->emitter, left, XTENSA_A2, left);
            emit_movi(p->emitter, XTENSA_A2, 0);
            emit_movi(p->emitter, XTENSA_A11, 1);
            emit_movltz(p->emitter, XTENSA_A2, XTENSA_A11, left);
        } else if (op == TOK_LE) {
            emit_sub(p->emitter, left, XTENSA_A2, left);
            emit_movi(p->emitter, XTENSA_A2, 0);
            emit_movi(p->emitter, XTENSA_A11, 1);
            emit_movgez(p->emitter, XTENSA_A2, XTENSA_A11, left);
        }
        p->last_expr_type = type_int();
    }
    return true;
}

static bool compile_expr_equality(Parser *p) {
    if (!compile_expr_relational(p)) return false;

    while (p->current_token.type == TOK_EQ ||
           p->current_token.type == TOK_NEQ) {
        TokenType op = p->current_token.type;
        advance_token(p);

        save_left_operand(p);
        if (!compile_expr_relational(p)) return false;
        Reg left = restore_left_operand(p);

        if (op == TOK_EQ) {
            emit_sub(p->emitter, left, left, XTENSA_A2);
            emit_movi(p->emitter, XTENSA_A2, 0);
            emit_movi(p->emitter, XTENSA_A11, 1);
            emit_moveqz(p->emitter, XTENSA_A2, XTENSA_A11, left);
        } else if (op == TOK_NEQ) {
            emit_sub(p->emitter, left, left, XTENSA_A2);
            emit_movi(p->emitter, XTENSA_A2, 0);
            emit_movi(p->emitter, XTENSA_A11, 1);
            emit_movnez(p->emitter, XTENSA_A2, XTENSA_A11, left);
        }
        p->last_expr_type = type_int();
    }
    return true;
}

static bool compile_expr_bitwise_and(Parser *p) {
    if (!compile_expr_equality(p)) return false;

    while (p->current_token.type == TOK_AMP) {
        advance_token(p);

        save_left_operand(p);
        if (!compile_expr_equality(p)) return false;
        Reg left = restore_left_operand(p);

        emit_and(p->emitter, XTENSA_A2, left, XTENSA_A2);
        p->last_expr_type = type_int();
    }
    return true;
}

static bool compile_expr_bitwise_xor(Parser *p) {
    if (!compile_expr_bitwise_and(p)) return false;

    while (p->current_token.type == TOK_CARET) {
        advance_token(p);

        save_left_operand(p);
        if (!compile_expr_bitwise_and(p)) return false;
        Reg left = restore_left_operand(p);

        emit_xor(p->emitter, XTENSA_A2, left, XTENSA_A2);
        p->last_expr_type = type_int();
    }
    return true;
}

static bool compile_expr_bitwise_or(Parser *p) {
    if (!compile_expr_bitwise_xor(p)) return false;

    while (p->current_token.type == TOK_PIPE) {
        advance_token(p);

        save_left_operand(p);
        if (!compile_expr_bitwise_xor(p)) return false;
        Reg left = restore_left_operand(p);

        emit_or(p->emitter, XTENSA_A2, left, XTENSA_A2);
        p->last_expr_type = type_int();
    }
    return true;
}

static bool compile_expr_logical_and(Parser *p) {
    if (!compile_expr_bitwise_or(p)) return false;

    if (p->current_token.type == TOK_AND_AND) {
        size_t false_patches[32];
        size_t false_count = 0;

        while (p->current_token.type == TOK_AND_AND) {
            advance_token(p); // consume '&&'

            if (false_count < 32) {
                false_patches[false_count++] = p->emitter->code_size;
            }
            emit_beqz(p->emitter, XTENSA_A2, 0);

            if (!compile_expr_bitwise_or(p)) return false;
        }

        // Test right operand
        if (false_count < 32) {
            false_patches[false_count++] = p->emitter->code_size;
        }
        emit_beqz(p->emitter, XTENSA_A2, 0);

        // True path: result = 1
        emit_movi(p->emitter, XTENSA_A2, 1);
        size_t jump_over_false = p->emitter->code_size;
        emit_j(p->emitter, 0);

        // False path: result = 0
        size_t false_target = p->emitter->code_size;
        for (size_t i = 0; i < false_count; i++) {
            emit_patch_branch(p->emitter, false_patches[i], false_target);
        }
        emit_movi(p->emitter, XTENSA_A2, 0);

        size_t end_target = p->emitter->code_size;
        emit_patch_branch(p->emitter, jump_over_false, end_target);
        p->last_expr_type = type_int();
    }
    return true;
}

static bool compile_expr_logical_or(Parser *p) {
    if (!compile_expr_logical_and(p)) return false;

    if (p->current_token.type == TOK_PIPE_PIPE) {
        size_t true_patches[32];
        size_t true_count = 0;

        while (p->current_token.type == TOK_PIPE_PIPE) {
            advance_token(p); // consume '||'

            if (true_count < 32) {
                true_patches[true_count++] = p->emitter->code_size;
            }
            emit_bnez(p->emitter, XTENSA_A2, 0);

            if (!compile_expr_logical_and(p)) return false;
        }

        // Test right operand
        if (true_count < 32) {
            true_patches[true_count++] = p->emitter->code_size;
        }
        emit_bnez(p->emitter, XTENSA_A2, 0);

        // False path: result = 0
        emit_movi(p->emitter, XTENSA_A2, 0);
        size_t jump_over_true = p->emitter->code_size;
        emit_j(p->emitter, 0);

        // True path: result = 1
        size_t true_target = p->emitter->code_size;
        for (size_t i = 0; i < true_count; i++) {
            emit_patch_branch(p->emitter, true_patches[i], true_target);
        }
        emit_movi(p->emitter, XTENSA_A2, 1);

        size_t end_target = p->emitter->code_size;
        emit_patch_branch(p->emitter, jump_over_true, end_target);
        p->last_expr_type = type_int();
    }
    return true;
}

static bool compile_expr_ternary(Parser *p) {
    if (!compile_expr_logical_or(p)) return false;

    if (p->current_token.type == TOK_QUESTION) {
        advance_token(p); // consume '?'

        size_t branch_to_false_pos = p->emitter->code_size;
        emit_beqz(p->emitter, XTENSA_A2, 0);

        // True branch: evaluate expression into a2
        if (!compile_expression(p)) return false;
        DataType true_type = p->last_expr_type;

        size_t jump_over_false_pos = p->emitter->code_size;
        emit_j(p->emitter, 0);

        size_t false_start_pos = p->emitter->code_size;
        emit_patch_branch(p->emitter, branch_to_false_pos, false_start_pos);

        if (!match(p, TOK_COLON)) return false;

        // False branch: evaluate ternary expression (right-associative) into a2
        if (!compile_expr_ternary(p)) return false;
        DataType false_type = p->last_expr_type;

        size_t end_pos = p->emitter->code_size;
        emit_patch_branch(p->emitter, jump_over_false_pos, end_pos);

        p->last_expr_type = (true_type.pointer_depth > false_type.pointer_depth) ? true_type : false_type;
    }
    return true;
}

bool compile_expression(Parser *p) {
    return compile_expr_ternary(p);
}

// ============================================================================
// Statements
// ============================================================================

static bool is_compound_assign(TokenType t) {
    return t == TOK_PLUS_ASSIGN || t == TOK_MINUS_ASSIGN ||
           t == TOK_STAR_ASSIGN || t == TOK_SLASH_ASSIGN || t == TOK_PERCENT_ASSIGN ||
           t == TOK_AMP_ASSIGN || t == TOK_PIPE_ASSIGN || t == TOK_CARET_ASSIGN ||
           t == TOK_LSHIFT_ASSIGN || t == TOK_RSHIFT_ASSIGN;
}

static void emit_binary_op(Emitter *e, TokenType op, Reg dst, Reg left, Reg right) {
    switch (op) {
        case TOK_PLUS_ASSIGN:
        case TOK_PLUS:
            emit_add(e, dst, left, right);
            break;
        case TOK_MINUS_ASSIGN:
        case TOK_MINUS:
            emit_sub(e, dst, left, right);
            break;
        case TOK_STAR_ASSIGN:
        case TOK_STAR:
            emit_mull(e, dst, left, right);
            break;
        case TOK_SLASH_ASSIGN:
        case TOK_SLASH:
            emit_quos(e, dst, left, right);
            break;
        case TOK_PERCENT_ASSIGN:
        case TOK_PERCENT:
            emit_rems(e, dst, left, right);
            break;
        case TOK_AMP_ASSIGN:
        case TOK_AMP:
            emit_and(e, dst, left, right);
            break;
        case TOK_PIPE_ASSIGN:
        case TOK_PIPE:
            emit_or(e, dst, left, right);
            break;
        case TOK_CARET_ASSIGN:
        case TOK_CARET:
            emit_xor(e, dst, left, right);
            break;
        case TOK_LSHIFT_ASSIGN:
        case TOK_LSHIFT:
            emit_ssl(e, right);
            emit_sll(e, dst, left);
            break;
        case TOK_RSHIFT_ASSIGN:
        case TOK_RSHIFT:
            emit_ssr(e, right);
            emit_sra(e, dst, left);
            break;
        default:
            break;
    }
}

static bool compile_assignment_or_expr(Parser *p, bool consume_semicolon) {
    if (p->has_error) return false;

    // 1. Prefix Increment / Decrement: ++x; or --x;
    if (p->current_token.type == TOK_PLUS_PLUS || p->current_token.type == TOK_MINUS_MINUS) {
        bool is_inc = (p->current_token.type == TOK_PLUS_PLUS);
        advance_token(p); // consume '++' or '--'

        if (p->current_token.type != TOK_IDENT) {
            p->has_error = true;
            snprintf(p->error_msg, sizeof(p->error_msg),
                     "Line %u, Col %u: Expected identifier after '%s'",
                     p->current_token.line, p->current_token.col, is_inc ? "++" : "--");
            return false;
        }

        const char *name = p->current_token.start;
        size_t len = p->current_token.length;
        Symbol *sym = symtab_lookup(&p->symtab, name, len);
        if (!sym) {
            p->has_error = true;
            snprintf(p->error_msg, sizeof(p->error_msg),
                     "Line %u, Col %u: Undeclared variable '%.*s'",
                     p->current_token.line, p->current_token.col, (int)len, name);
            return false;
        }
        advance_token(p); // consume identifier

        if (sym->type.base == BASE_TYPE_CHAR && sym->type.pointer_depth == 0) {
            emit_l8ui(p->emitter, XTENSA_A2, XTENSA_A1, sym->stack_offset);
        } else {
            emit_l32i(p->emitter, XTENSA_A2, XTENSA_A1, sym->stack_offset);
        }

        int step = 1;
        if (sym->type.pointer_depth > 0) {
            step = type_size(type_deref(sym->type));
        }

        if (is_inc) {
            emit_addi(p->emitter, XTENSA_A2, XTENSA_A2, (int8_t)step);
        } else {
            emit_addi(p->emitter, XTENSA_A2, XTENSA_A2, (int8_t)(-step));
        }

        if (sym->type.base == BASE_TYPE_CHAR && sym->type.pointer_depth == 0) {
            emit_s8i(p->emitter, XTENSA_A2, XTENSA_A1, sym->stack_offset);
        } else {
            emit_s32i(p->emitter, XTENSA_A2, XTENSA_A1, sym->stack_offset);
        }

        if (consume_semicolon) return match(p, TOK_SEMICOLON);
        return true;
    }

    // 2. Pointer dereference assignment: *<expr> = <expr> or *<expr> += <expr>
    if (p->current_token.type == TOK_STAR) {
        advance_token(p); // consume '*'
        if (!compile_expr_unary(p)) return false;
        DataType target_type = type_deref(p->last_expr_type);

        save_left_operand(p); // save target address

        if (p->current_token.type == TOK_ASSIGN) {
            advance_token(p); // consume '='
            if (!compile_expression(p)) return false; // compute RHS value into a2

            Reg addr_reg = restore_left_operand(p); // restore target address
            if (target_type.base == BASE_TYPE_CHAR && target_type.pointer_depth == 0) {
                emit_s8i(p->emitter, XTENSA_A2, addr_reg, 0);
            } else {
                emit_s32i(p->emitter, XTENSA_A2, addr_reg, 0);
            }
            if (consume_semicolon) return match(p, TOK_SEMICOLON);
            return true;
        } else if (is_compound_assign(p->current_token.type)) {
            TokenType op = p->current_token.type;
            advance_token(p);

            // Load old value from target address into a2
            Reg addr_reg = restore_left_operand(p);
            save_left_operand(p); // re-save target address

            if (target_type.base == BASE_TYPE_CHAR && target_type.pointer_depth == 0) {
                emit_l8ui(p->emitter, XTENSA_A2, addr_reg, 0);
            } else {
                emit_l32i(p->emitter, XTENSA_A2, addr_reg, 0);
            }

            save_left_operand(p); // save old value
            if (!compile_expression(p)) return false; // compute RHS value into a2

            Reg old_val = restore_left_operand(p);
            Reg target_addr = restore_left_operand(p);

            emit_binary_op(p->emitter, op, XTENSA_A2, old_val, XTENSA_A2);

            if (target_type.base == BASE_TYPE_CHAR && target_type.pointer_depth == 0) {
                emit_s8i(p->emitter, XTENSA_A2, target_addr, 0);
            } else {
                emit_s32i(p->emitter, XTENSA_A2, target_addr, 0);
            }
            if (consume_semicolon) return match(p, TOK_SEMICOLON);
            return true;
        } else {
            restore_left_operand(p);
            if (consume_semicolon) return match(p, TOK_SEMICOLON);
            return true;
        }
    }

    // 3. Identifier / this assignment, compound assignment, or postfix inc/dec
    if (p->current_token.type == TOK_IDENT || p->current_token.type == TOK_THIS) {
        bool is_this_kw = (p->current_token.type == TOK_THIS);
        Token next_tok;
        Lexer lookahead = *p->lexer;
        lexer_next_token(&lookahead, &next_tok);

        if (next_tok.type == TOK_ASSIGN || is_compound_assign(next_tok.type) ||
            next_tok.type == TOK_PLUS_PLUS || next_tok.type == TOK_MINUS_MINUS ||
            next_tok.type == TOK_LBRACKET || next_tok.type == TOK_DOT || next_tok.type == TOK_ARROW) {

            const char *name = is_this_kw ? "this" : p->current_token.start;
            size_t len = is_this_kw ? 4 : p->current_token.length;
            Symbol *sym = symtab_lookup(&p->symtab, name, len);
            if (!sym) {
                p->has_error = true;
                snprintf(p->error_msg, sizeof(p->error_msg),
                         "Line %u, Col %u: Undeclared variable '%.*s'",
                         p->current_token.line, p->current_token.col, (int)len, name);
                return false;
            }
            advance_token(p); // consume identifier / this

            // 3a. Simple assignment: ident = <expr>
            if (p->current_token.type == TOK_ASSIGN) {
                advance_token(p); // consume '='
                if (sym->type.is_reference) {
                    emit_l32i(p->emitter, XTENSA_A2, XTENSA_A1, sym->stack_offset); // load target addr
                    save_left_operand(p); // save target addr
                    if (!compile_expression(p)) return false; // RHS value in a2
                    Reg target_addr = restore_left_operand(p);
                    if (sym->type.base == BASE_TYPE_CHAR && sym->type.pointer_depth == 0) {
                        emit_s8i(p->emitter, XTENSA_A2, target_addr, 0);
                    } else {
                        emit_s32i(p->emitter, XTENSA_A2, target_addr, 0);
                    }
                    if (consume_semicolon) return match(p, TOK_SEMICOLON);
                    return true;
                }
                if (!compile_expression(p)) return false;
                if (sym->type.base == BASE_TYPE_CHAR && sym->type.pointer_depth == 0) {
                    emit_s8i(p->emitter, XTENSA_A2, XTENSA_A1, sym->stack_offset);
                } else {
                    emit_s32i(p->emitter, XTENSA_A2, XTENSA_A1, sym->stack_offset);
                }
                if (consume_semicolon) return match(p, TOK_SEMICOLON);
                return true;
            }

            // 3b. Simple compound assignment: ident += <expr> etc.
            if (is_compound_assign(p->current_token.type)) {
                TokenType op = p->current_token.type;
                advance_token(p);

                // Load variable current value
                if (sym->type.base == BASE_TYPE_CHAR && sym->type.pointer_depth == 0) {
                    emit_l8ui(p->emitter, XTENSA_A2, XTENSA_A1, sym->stack_offset);
                } else {
                    emit_l32i(p->emitter, XTENSA_A2, XTENSA_A1, sym->stack_offset);
                }

                save_left_operand(p); // save old value
                if (!compile_expression(p)) return false; // RHS in a2
                Reg old_val = restore_left_operand(p);

                emit_binary_op(p->emitter, op, XTENSA_A2, old_val, XTENSA_A2);

                if (sym->type.base == BASE_TYPE_CHAR && sym->type.pointer_depth == 0) {
                    emit_s8i(p->emitter, XTENSA_A2, XTENSA_A1, sym->stack_offset);
                } else {
                    emit_s32i(p->emitter, XTENSA_A2, XTENSA_A1, sym->stack_offset);
                }
                if (consume_semicolon) return match(p, TOK_SEMICOLON);
                return true;
            }

            // 3c. Simple postfix increment / decrement: ident++ or ident--
            if (p->current_token.type == TOK_PLUS_PLUS || p->current_token.type == TOK_MINUS_MINUS) {
                bool is_inc = (p->current_token.type == TOK_PLUS_PLUS);
                advance_token(p);

                if (sym->type.base == BASE_TYPE_CHAR && sym->type.pointer_depth == 0) {
                    emit_l8ui(p->emitter, XTENSA_A2, XTENSA_A1, sym->stack_offset);
                } else {
                    emit_l32i(p->emitter, XTENSA_A2, XTENSA_A1, sym->stack_offset);
                }

                int step = 1;
                if (sym->type.pointer_depth > 0) {
                    step = type_size(type_deref(sym->type));
                }

                if (is_inc) {
                    emit_addi(p->emitter, XTENSA_A2, XTENSA_A2, (int8_t)step);
                } else {
                    emit_addi(p->emitter, XTENSA_A2, XTENSA_A2, (int8_t)(-step));
                }

                if (sym->type.base == BASE_TYPE_CHAR && sym->type.pointer_depth == 0) {
                    emit_s8i(p->emitter, XTENSA_A2, XTENSA_A1, sym->stack_offset);
                } else {
                    emit_s32i(p->emitter, XTENSA_A2, XTENSA_A1, sym->stack_offset);
                }
                if (consume_semicolon) return match(p, TOK_SEMICOLON);
                return true;
            }

            // 3d. Postfix l-value target (. / -> / [ ])
            DataType curr_type = sym->type;
            if (curr_type.array_size > 0 || (curr_type.base == BASE_TYPE_STRUCT && curr_type.pointer_depth == 0)) {
                if (sym->stack_offset >= -128 && sym->stack_offset <= 127) {
                    emit_addi(p->emitter, XTENSA_A2, XTENSA_A1, (int8_t)sym->stack_offset);
                } else {
                    emit_load_imm(p->emitter, XTENSA_A11, sym->stack_offset);
                    emit_add(p->emitter, XTENSA_A2, XTENSA_A1, XTENSA_A11);
                }
            } else {
                emit_l32i(p->emitter, XTENSA_A2, XTENSA_A1, sym->stack_offset);
            }

            const StructMember *last_member = NULL;
            while (p->current_token.type == TOK_LBRACKET ||
                   p->current_token.type == TOK_DOT ||
                   p->current_token.type == TOK_ARROW) {

                if (p->current_token.type == TOK_LBRACKET) {
                    last_member = NULL;
                    advance_token(p); // consume '['
                    DataType elem_type = type_deref(curr_type);

                    save_left_operand(p); // save base address
                    if (!compile_expression(p)) return false; // compute index in a2
                    if (!match(p, TOK_RBRACKET)) return false; // consume ']'
                    Reg base_reg = restore_left_operand(p);

                    // Scale index
                    if (elem_type.pointer_depth > 0 || elem_type.base == BASE_TYPE_INT || elem_type.base == BASE_TYPE_STRUCT) {
                        int sz = type_size(elem_type);
                        if (sz == 4) {
                            emit_add_n(p->emitter, XTENSA_A2, XTENSA_A2, XTENSA_A2);
                            emit_add_n(p->emitter, XTENSA_A2, XTENSA_A2, XTENSA_A2);
                        } else if (sz != 1) {
                            emit_load_imm(p->emitter, XTENSA_A11, sz);
                            emit_mull(p->emitter, XTENSA_A2, XTENSA_A2, XTENSA_A11);
                        }
                    }

                    emit_add(p->emitter, XTENSA_A2, base_reg, XTENSA_A2);
                    curr_type = elem_type;
                } else if (p->current_token.type == TOK_DOT) {
                    advance_token(p); // consume '.'
                    if (p->current_token.type != TOK_IDENT) {
                        p->has_error = true;
                        snprintf(p->error_msg, sizeof(p->error_msg), "Expected member name after '.'");
                        return false;
                    }
                    const char *mname = p->current_token.start;
                    size_t mlen = p->current_token.length;
                    advance_token(p);

                    if (curr_type.base != BASE_TYPE_STRUCT && curr_type.base != BASE_TYPE_UNION) {
                        p->has_error = true;
                        snprintf(p->error_msg, sizeof(p->error_msg), "'.' applied to non-struct type");
                        return false;
                    }
                    const StructMember *m = struct_lookup_member(curr_type.struct_def, mname, mlen);
                    if (!m) {
                        p->has_error = true;
                        snprintf(p->error_msg, sizeof(p->error_msg), "No such member '%.*s'", (int)mlen, mname);
                        return false;
                    }
                    if (m->offset > 0) {
                        if (m->offset >= -128 && m->offset <= 127) {
                            emit_addi(p->emitter, XTENSA_A2, XTENSA_A2, (int8_t)m->offset);
                        } else {
                            emit_load_imm(p->emitter, XTENSA_A11, m->offset);
                            emit_add(p->emitter, XTENSA_A2, XTENSA_A2, XTENSA_A11);
                        }
                    }
                    last_member = m;
                    curr_type = m->type;
                } else if (p->current_token.type == TOK_ARROW) {
                    advance_token(p); // consume '->'
                    if (p->current_token.type != TOK_IDENT) {
                        p->has_error = true;
                        snprintf(p->error_msg, sizeof(p->error_msg), "Expected member name after '->'");
                        return false;
                    }
                    const char *mname = p->current_token.start;
                    size_t mlen = p->current_token.length;
                    advance_token(p);

                    if (curr_type.pointer_depth == 0 || (curr_type.base != BASE_TYPE_STRUCT && curr_type.base != BASE_TYPE_UNION)) {
                        p->has_error = true;
                        snprintf(p->error_msg, sizeof(p->error_msg), "'->' applied to non-struct-pointer type");
                        return false;
                    }
                    const StructMember *m = struct_lookup_member(curr_type.struct_def, mname, mlen);
                    if (!m) {
                        p->has_error = true;
                        snprintf(p->error_msg, sizeof(p->error_msg), "No such member '%.*s'", (int)mlen, mname);
                        return false;
                    }
                    if (m->offset > 0) {
                        if (m->offset >= -128 && m->offset <= 127) {
                            emit_addi(p->emitter, XTENSA_A2, XTENSA_A2, (int8_t)m->offset);
                        } else {
                            emit_load_imm(p->emitter, XTENSA_A11, m->offset);
                            emit_add(p->emitter, XTENSA_A2, XTENSA_A2, XTENSA_A11);
                        }
                    }
                    last_member = m;
                    curr_type = m->type;
                }
            }

            if (p->current_token.type == TOK_ASSIGN) {
                save_left_operand(p); // save target address
                advance_token(p); // consume '='
                if (!compile_expression(p)) return false; // compute RHS value in a2

                if (last_member && last_member->is_bitfield) {
                    uint32_t mask = (1U << last_member->bit_width) - 1;
                    emit_load_imm(p->emitter, XTENSA_A11, mask);
                    emit_and(p->emitter, XTENSA_A9, XTENSA_A2, XTENSA_A11);
                    if (last_member->bit_offset > 0) {
                        emit_slli(p->emitter, XTENSA_A9, XTENSA_A9, last_member->bit_offset);
                    }

                    Reg addr_reg = restore_left_operand(p); // restore target address

                    // Load current 32-bit word from memory
                    emit_l32i(p->emitter, XTENSA_A11, addr_reg, 0);
                    uint32_t inv_mask = ~(mask << last_member->bit_offset);
                    emit_load_imm(p->emitter, XTENSA_A10, inv_mask);
                    emit_and(p->emitter, XTENSA_A11, XTENSA_A11, XTENSA_A10);

                    emit_or(p->emitter, XTENSA_A2, XTENSA_A11, XTENSA_A9);
                    emit_s32i(p->emitter, XTENSA_A2, addr_reg, 0);
                } else {
                    Reg addr_reg = restore_left_operand(p); // restore target address
                    if (curr_type.base == BASE_TYPE_CHAR && curr_type.pointer_depth == 0) {
                        emit_s8i(p->emitter, XTENSA_A2, addr_reg, 0);
                    } else {
                        emit_s32i(p->emitter, XTENSA_A2, addr_reg, 0);
                    }
                }
                if (consume_semicolon) return match(p, TOK_SEMICOLON);
                return true;
            } else if (is_compound_assign(p->current_token.type)) {
                TokenType op = p->current_token.type;
                advance_token(p);

                save_left_operand(p); // save target address
                Reg addr_reg = restore_left_operand(p);
                save_left_operand(p); // re-save target address

                if (curr_type.base == BASE_TYPE_CHAR && curr_type.pointer_depth == 0) {
                    emit_l8ui(p->emitter, XTENSA_A2, addr_reg, 0);
                } else {
                    emit_l32i(p->emitter, XTENSA_A2, addr_reg, 0);
                }

                save_left_operand(p); // save old value
                if (!compile_expression(p)) return false; // compute RHS value in a2
                Reg old_val = restore_left_operand(p);
                Reg target_addr = restore_left_operand(p);

                emit_binary_op(p->emitter, op, XTENSA_A2, old_val, XTENSA_A2);

                if (curr_type.base == BASE_TYPE_CHAR && curr_type.pointer_depth == 0) {
                    emit_s8i(p->emitter, XTENSA_A2, target_addr, 0);
                } else {
                    emit_s32i(p->emitter, XTENSA_A2, target_addr, 0);
                }
                if (consume_semicolon) return match(p, TOK_SEMICOLON);
                return true;
            } else if (p->current_token.type == TOK_PLUS_PLUS || p->current_token.type == TOK_MINUS_MINUS) {
                bool is_inc = (p->current_token.type == TOK_PLUS_PLUS);
                advance_token(p);

                save_left_operand(p); // save target address
                Reg addr_reg = restore_left_operand(p);
                save_left_operand(p); // re-save target address

                if (curr_type.base == BASE_TYPE_CHAR && curr_type.pointer_depth == 0) {
                    emit_l8ui(p->emitter, XTENSA_A2, addr_reg, 0);
                } else {
                    emit_l32i(p->emitter, XTENSA_A2, addr_reg, 0);
                }

                int step = 1;
                if (curr_type.pointer_depth > 0) {
                    step = type_size(type_deref(curr_type));
                }

                if (is_inc) {
                    emit_addi(p->emitter, XTENSA_A2, XTENSA_A2, (int8_t)step);
                } else {
                    emit_addi(p->emitter, XTENSA_A2, XTENSA_A2, (int8_t)(-step));
                }

                Reg target_addr = restore_left_operand(p);
                if (curr_type.base == BASE_TYPE_CHAR && curr_type.pointer_depth == 0) {
                    emit_s8i(p->emitter, XTENSA_A2, target_addr, 0);
                } else {
                    emit_s32i(p->emitter, XTENSA_A2, target_addr, 0);
                }
                if (consume_semicolon) return match(p, TOK_SEMICOLON);
                return true;
            }
        }
    }

    if (!compile_expression(p)) return false;
    if (consume_semicolon) {
        return match(p, TOK_SEMICOLON);
    }
    return true;
}

bool compile_statement(Parser *p) {
    if (p->has_error) return false;

    // Typedef definition inside statement: typedef <type> <name>; or typedef <type> (*<name>)(<params>);
    if (p->current_token.type == TOK_TYPEDEF) {
        advance_token(p); // consume 'typedef'
        DataType t;
        if (!parse_type(p, &t)) return false;

        if (p->current_token.type == TOK_LPAREN) {
            Token next_tok;
            Lexer lookahead = *p->lexer;
            lexer_next_token(&lookahead, &next_tok);
            if (next_tok.type == TOK_STAR) {
                advance_token(p); // consume '('
                advance_token(p); // consume '*'
                if (p->current_token.type != TOK_IDENT) return false;
                const char *tname = p->current_token.start;
                size_t tlen = p->current_token.length;
                advance_token(p); // consume ident
                if (!match(p, TOK_RPAREN)) return false;
                if (!match(p, TOK_LPAREN)) return false;
                while (p->current_token.type != TOK_RPAREN && !p->has_error && p->current_token.type != TOK_EOF) {
                    DataType pt;
                    if (!parse_type(p, &pt)) return false;
                    if (p->current_token.type == TOK_IDENT) advance_token(p);
                    if (p->current_token.type == TOK_COMMA) advance_token(p);
                    else break;
                }
                if (!match(p, TOK_RPAREN)) return false;
                DataType fn_t = t;
                fn_t.is_func_ptr = true;
                fn_t.pointer_depth = 1;
                symtab_define_typedef(&p->symtab, tname, tlen, fn_t);
                return match(p, TOK_SEMICOLON);
            }
        }

        if (p->current_token.type != TOK_IDENT) {
            p->has_error = true;
            snprintf(p->error_msg, sizeof(p->error_msg),
                     "Line %u, Col %u: Expected identifier in typedef",
                     p->current_token.line, p->current_token.col);
            return false;
        }
        const char *tname = p->current_token.start;
        size_t tlen = p->current_token.length;
        advance_token(p);
        symtab_define_typedef(&p->symtab, tname, tlen, t);
        return match(p, TOK_SEMICOLON);
    }

    // 1. Variable / Array / Struct / Function Pointer Declaration
    if (is_type_start(p)) {
        DataType var_type;
        if (!parse_type(p, &var_type)) return false;

        // If declaration ended with semicolon (e.g. `enum Tag { ... };` or `struct Tag;`)
        if (p->current_token.type == TOK_SEMICOLON) {
            advance_token(p);
            return true;
        }

        // Function Pointer variable: Type (*name)(Params) = ...;
        if (p->current_token.type == TOK_LPAREN) {
            Token next_tok;
            Lexer lookahead = *p->lexer;
            lexer_next_token(&lookahead, &next_tok);
            if (next_tok.type == TOK_STAR) {
                advance_token(p); // consume '('
                advance_token(p); // consume '*'
                if (p->current_token.type != TOK_IDENT) return false;
                const char *fn_ptr_name = p->current_token.start;
                size_t fn_ptr_len = p->current_token.length;
                advance_token(p); // consume ident
                if (!match(p, TOK_RPAREN)) return false;
                if (!match(p, TOK_LPAREN)) return false;
                while (p->current_token.type != TOK_RPAREN && !p->has_error && p->current_token.type != TOK_EOF) {
                    DataType pt;
                    if (!parse_type(p, &pt)) return false;
                    if (p->current_token.type == TOK_IDENT) advance_token(p);
                    if (p->current_token.type == TOK_COMMA) advance_token(p);
                    else break;
                }
                if (!match(p, TOK_RPAREN)) return false;

                DataType fn_type = var_type;
                fn_type.is_func_ptr = true;
                fn_type.pointer_depth = 1;
                Symbol *sym = symtab_alloc_local(&p->symtab, fn_ptr_name, fn_ptr_len, fn_type);
                if (p->current_token.type == TOK_ASSIGN) {
                    advance_token(p); // consume '='
                    if (!compile_expression(p)) return false;
                    emit_s32i(p->emitter, XTENSA_A2, XTENSA_A1, sym->stack_offset);
                }
                return match(p, TOK_SEMICOLON);
            }
        }

        if (p->current_token.type != TOK_IDENT) {
            p->has_error = true;
            snprintf(p->error_msg, sizeof(p->error_msg),
                     "Line %u, Col %u: Expected variable identifier in declaration",
                     p->current_token.line, p->current_token.col);
            return false;
        }

        const char *name = p->current_token.start;
        size_t len = p->current_token.length;
        advance_token(p);

        // Check for array declaration: int arr[N];
        if (p->current_token.type == TOK_LBRACKET) {
            advance_token(p); // consume '['
            int arr_size = 0;
            if (p->current_token.type == TOK_INT_LIT) {
                arr_size = (int)p->current_token.int_val;
                advance_token(p);
            }
            if (!match(p, TOK_RBRACKET)) return false;

            // Optional array brace initializer: int arr[4] = { 1, 2, 3, 4 }; or designated [idx] = val
            if (p->current_token.type == TOK_ASSIGN) {
                advance_token(p); // consume '='
                if (p->current_token.type == TOK_LBRACE) {
                    advance_token(p); // consume '{'
                    size_t init_count = 0;
                    DataType elem_type = var_type;
                    int elem_sz = type_size(elem_type);

                    if (arr_size <= 0) arr_size = 64;
                    var_type.array_size = arr_size;
                    Symbol *sym = symtab_alloc_local(&p->symtab, name, len, var_type);

                    while (p->current_token.type != TOK_RBRACE && !p->has_error && p->current_token.type != TOK_EOF) {
                        size_t target_idx = init_count;
                        if (p->current_token.type == TOK_LBRACKET) {
                            advance_token(p); // consume '['
                            if (p->current_token.type == TOK_INT_LIT) {
                                target_idx = (size_t)p->current_token.int_val;
                                advance_token(p);
                            }
                            if (!match(p, TOK_RBRACKET)) return false;
                            if (p->current_token.type == TOK_ASSIGN) advance_token(p);
                        }
                        if (!compile_expression(p)) return false;
                        int offset = sym->stack_offset + (int)(target_idx * elem_sz);
                        if (elem_sz == 1) {
                            emit_s8i(p->emitter, XTENSA_A2, XTENSA_A1, offset);
                        } else {
                            emit_s32i(p->emitter, XTENSA_A2, XTENSA_A1, offset);
                        }
                        init_count = target_idx + 1;
                        if (p->current_token.type == TOK_COMMA) {
                            advance_token(p);
                        } else {
                            break;
                        }
                    }
                    if (!match(p, TOK_RBRACE)) return false;

                    // Zero-fill remaining elements
                    if (init_count < (size_t)arr_size) {
                        emit_movi(p->emitter, XTENSA_A2, 0);
                        for (size_t i = init_count; i < (size_t)arr_size; i++) {
                            int offset = sym->stack_offset + (int)(i * elem_sz);
                            if (elem_sz == 1) {
                                emit_s8i(p->emitter, XTENSA_A2, XTENSA_A1, offset);
                            } else {
                                emit_s32i(p->emitter, XTENSA_A2, XTENSA_A1, offset);
                            }
                        }
                    }
                    return match(p, TOK_SEMICOLON);
                }
            }

            if (arr_size <= 0) arr_size = 1;
            var_type.array_size = arr_size;
            symtab_alloc_local(&p->symtab, name, len, var_type);
            return match(p, TOK_SEMICOLON);
        }

        // Allocate scalar or struct local variable on stack frame
        Symbol *sym = symtab_alloc_local(&p->symtab, name, len, var_type);
        if (!sym) {
            p->has_error = true;
            snprintf(p->error_msg, sizeof(p->error_msg), "Out of memory in symbol table");
            return false;
        }

        // Optional reference initializer: Type &ref = var;
        if (var_type.is_reference) {
            if (!match(p, TOK_ASSIGN)) return false;
            if (p->current_token.type == TOK_IDENT) {
                Symbol *target_sym = symtab_lookup(&p->symtab, p->current_token.start, p->current_token.length);
                if (!target_sym) {
                    p->has_error = true;
                    snprintf(p->error_msg, sizeof(p->error_msg),
                             "Line %u, Col %u: Undeclared target for reference",
                             p->current_token.line, p->current_token.col);
                    return false;
                }
                advance_token(p);
                if (target_sym->stack_offset >= -128 && target_sym->stack_offset <= 127) {
                    emit_addi(p->emitter, XTENSA_A2, XTENSA_A1, (int8_t)target_sym->stack_offset);
                } else {
                    emit_load_imm(p->emitter, XTENSA_A11, target_sym->stack_offset);
                    emit_add(p->emitter, XTENSA_A2, XTENSA_A1, XTENSA_A11);
                }
                emit_s32i(p->emitter, XTENSA_A2, XTENSA_A1, sym->stack_offset);
                return match(p, TOK_SEMICOLON);
            }
        }

        // Optional constructor initializer: ClassName obj(arg1, arg2);
        if (var_type.base == BASE_TYPE_STRUCT && p->current_token.type == TOK_LPAREN) {
            advance_token(p); // consume '('
            if (sym->stack_offset >= -128 && sym->stack_offset <= 127) {
                emit_addi(p->emitter, XTENSA_A2, XTENSA_A1, (int8_t)sym->stack_offset);
            } else {
                emit_load_imm(p->emitter, XTENSA_A11, sym->stack_offset);
                emit_add(p->emitter, XTENSA_A2, XTENSA_A1, XTENSA_A11);
            }
            save_left_operand(p); // save 'this'

            size_t num_args = 1;
            if (p->current_token.type != TOK_RPAREN && p->current_token.type != TOK_VOID) {
                while (true) {
                    if (!compile_expression(p)) return false;
                    save_left_operand(p);
                    num_args++;
                    if (p->current_token.type == TOK_COMMA) advance_token(p);
                    else break;
                }
            } else if (p->current_token.type == TOK_VOID) {
                advance_token(p);
            }
            if (!match(p, TOK_RPAREN)) return false;

            for (int i = (int)num_args - 1; i >= 0; i--) {
                Reg saved = restore_left_operand(p);
                Reg target = (Reg)(XTENSA_A2 + i);
                if (saved != target) {
                    emit_mov_n(p->emitter, target, saved);
                }
            }

            StructDef *s = var_type.struct_def;
            char *ctor_mangled = mangle_method_name(p->arena, s ? s->name : "", s ? s->name : "");
            size_t call_pos = p->emitter->code_size;
            emit_call0(p->emitter, 0);
            emit_add_reloc(p->emitter, call_pos, ctor_mangled, strlen(ctor_mangled), R_XTENSA_SLOT0_OP, 0);
            return match(p, TOK_SEMICOLON);
        }

        // Optional initializer: [type] <var> = <expr>; or [type] <var> = { <m0>, <m1> }; or designated { .field = val }
        if (p->current_token.type == TOK_ASSIGN) {
            advance_token(p); // consume '='
            if (p->current_token.type == TOK_LBRACE && (var_type.base == BASE_TYPE_STRUCT || var_type.base == BASE_TYPE_UNION) && var_type.struct_def) {
                advance_token(p); // consume '{'
                StructDef *s = var_type.struct_def;
                size_t m_idx = 0;
                while (p->current_token.type != TOK_RBRACE && !p->has_error && p->current_token.type != TOK_EOF) {
                    const StructMember *target_m = NULL;
                    if (p->current_token.type == TOK_DOT) {
                        advance_token(p); // consume '.'
                        if (p->current_token.type == TOK_IDENT) {
                            target_m = struct_lookup_member(s, p->current_token.start, p->current_token.length);
                            advance_token(p);
                        }
                        if (p->current_token.type == TOK_ASSIGN) advance_token(p);
                    } else if (m_idx < s->member_count) {
                        target_m = &s->members[m_idx++];
                    }
                    if (!target_m) {
                        p->has_error = true;
                        snprintf(p->error_msg, sizeof(p->error_msg), "Invalid struct member initializer");
                        return false;
                    }
                    if (!compile_expression(p)) return false;
                    int m_off = sym->stack_offset + target_m->offset;
                    int m_sz = type_size(target_m->type);
                    if (target_m->is_bitfield) {
                        uint32_t mask = (1U << target_m->bit_width) - 1;
                        emit_load_imm(p->emitter, XTENSA_A11, mask);
                        emit_and(p->emitter, XTENSA_A2, XTENSA_A2, XTENSA_A11);
                        if (target_m->bit_offset > 0) {
                            emit_slli(p->emitter, XTENSA_A2, XTENSA_A2, target_m->bit_offset);
                        }
                        save_left_operand(p);
                        emit_l32i(p->emitter, XTENSA_A11, XTENSA_A1, m_off);
                        uint32_t inv_mask = ~(mask << target_m->bit_offset);
                        emit_load_imm(p->emitter, XTENSA_A10, inv_mask);
                        emit_and(p->emitter, XTENSA_A11, XTENSA_A11, XTENSA_A10);
                        Reg new_b = restore_left_operand(p);
                        emit_or(p->emitter, XTENSA_A2, XTENSA_A11, new_b);
                        emit_s32i(p->emitter, XTENSA_A2, XTENSA_A1, m_off);
                    } else if (m_sz == 1) {
                        emit_s8i(p->emitter, XTENSA_A2, XTENSA_A1, m_off);
                    } else {
                        emit_s32i(p->emitter, XTENSA_A2, XTENSA_A1, m_off);
                    }
                    if (p->current_token.type == TOK_COMMA) {
                        advance_token(p);
                    } else {
                        break;
                    }
                }
                if (!match(p, TOK_RBRACE)) return false;
            } else {
                if (!compile_expression(p)) return false;

                if (var_type.base == BASE_TYPE_CHAR && var_type.pointer_depth == 0) {
                    emit_s8i(p->emitter, XTENSA_A2, XTENSA_A1, sym->stack_offset);
                } else {
                    emit_s32i(p->emitter, XTENSA_A2, XTENSA_A1, sym->stack_offset);
                }
            }
        }

        return match(p, TOK_SEMICOLON);
    }

    // 2. Block statement: { <stmt>* }
    if (p->current_token.type == TOK_LBRACE) {
        advance_token(p); // consume '{'
        symtab_enter_scope(&p->symtab);
        while (p->current_token.type != TOK_RBRACE && !p->has_error && p->current_token.type != TOK_EOF) {
            if (!compile_statement(p)) return false;
        }
        symtab_exit_scope(&p->symtab);
        return match(p, TOK_RBRACE);
    }

    // 3. Return statement: return [<expr>];
    if (p->current_token.type == TOK_RETURN) {
        advance_token(p); // consume 'return'
        if (p->current_token.type != TOK_SEMICOLON) {
            if (!compile_expression(p)) return false;
        }
        size_t jump_pos = p->emitter->code_size;
        emit_j(p->emitter, 0); // placeholder jump to epilogue
        add_return_patch(p, jump_pos);
        return match(p, TOK_SEMICOLON);
    }

    // 4. If statement: if (<cond>) <then_stmt> [else <else_stmt>]
    if (p->current_token.type == TOK_IF) {
        advance_token(p); // consume 'if'
        if (!match(p, TOK_LPAREN)) return false;
        if (!compile_expression(p)) return false;
        if (!match(p, TOK_RPAREN)) return false;

        size_t branch_to_else_pos = p->emitter->code_size;
        emit_beqz(p->emitter, XTENSA_A2, 0); // placeholder branch

        if (!compile_statement(p)) return false;

        if (p->current_token.type == TOK_ELSE) {
            advance_token(p); // consume 'else'
            size_t jump_over_else_pos = p->emitter->code_size;
            emit_j(p->emitter, 0);

            size_t else_start = p->emitter->code_size;
            emit_patch_branch(p->emitter, branch_to_else_pos, else_start);

            if (!compile_statement(p)) return false;

            size_t after_else = p->emitter->code_size;
            emit_patch_branch(p->emitter, jump_over_else_pos, after_else);
        } else {
            size_t after_if = p->emitter->code_size;
            emit_patch_branch(p->emitter, branch_to_else_pos, after_if);
        }
        return true;
    }

    // 5. While loop: while (<cond>) <stmt>
    if (p->current_token.type == TOK_WHILE) {
        advance_token(p); // consume 'while'
        if (!match(p, TOK_LPAREN)) return false;

        size_t cond_offset = p->emitter->code_size;

        if (!compile_expression(p)) return false;
        if (!match(p, TOK_RPAREN)) return false;

        size_t exit_branch_pos = p->emitter->code_size;
        emit_beqz(p->emitter, XTENSA_A2, 0);

        BreakContext loop_ctx;
        enter_break_context(p, &loop_ctx, CTX_LOOP, cond_offset);

        if (!compile_statement(p)) return false;

        size_t loop_back_pos = p->emitter->code_size;
        emit_j(p->emitter, 0);
        emit_patch_branch(p->emitter, loop_back_pos, cond_offset);

        size_t loop_end = p->emitter->code_size;
        emit_patch_branch(p->emitter, exit_branch_pos, loop_end);

        exit_break_context(p, loop_end);
        return true;
    }

    // 6. For loop: for ([init]; [cond]; [step]) <stmt>
    if (p->current_token.type == TOK_FOR) {
        advance_token(p); // consume 'for'
        if (!match(p, TOK_LPAREN)) return false;

        symtab_enter_scope(&p->symtab);

        // 1. Init clause
        if (p->current_token.type == TOK_INT ||
            p->current_token.type == TOK_CHAR ||
            p->current_token.type == TOK_VOID ||
            p->current_token.type == TOK_VOLATILE ||
            p->current_token.type == TOK_STRUCT) {
            if (!compile_statement(p)) return false;
        } else if (p->current_token.type != TOK_SEMICOLON) {
            if (!compile_assignment_or_expr(p, true)) return false;
        } else {
            advance_token(p); // consume ';'
        }

        // 2. Condition clause
        size_t cond_offset = p->emitter->code_size;
        size_t exit_branch_pos = 0;
        bool has_cond = false;

        if (p->current_token.type != TOK_SEMICOLON) {
            has_cond = true;
            if (!compile_expression(p)) return false;
            exit_branch_pos = p->emitter->code_size;
            emit_beqz(p->emitter, XTENSA_A2, 0);
        }
        if (!match(p, TOK_SEMICOLON)) return false;

        // 3. Step clause & Jump to body
        size_t continue_target = cond_offset;
        size_t jump_to_body_pos = 0;

        if (p->current_token.type != TOK_RPAREN) {
            jump_to_body_pos = p->emitter->code_size;
            emit_j(p->emitter, 0); // Jump over step block to body on first iteration

            size_t step_offset = p->emitter->code_size;
            continue_target = step_offset;

            if (!compile_assignment_or_expr(p, false)) return false;

            size_t step_jump_cond = p->emitter->code_size;
            emit_j(p->emitter, 0);
            emit_patch_branch(p->emitter, step_jump_cond, cond_offset);

            size_t body_offset = p->emitter->code_size;
            emit_patch_branch(p->emitter, jump_to_body_pos, body_offset);
        }
        if (!match(p, TOK_RPAREN)) return false;

        // 4. Body
        BreakContext loop_ctx;
        enter_break_context(p, &loop_ctx, CTX_LOOP, continue_target);

        if (!compile_statement(p)) return false;

        size_t loop_back_pos = p->emitter->code_size;
        emit_j(p->emitter, 0);
        emit_patch_branch(p->emitter, loop_back_pos, continue_target);

        // 5. Loop Exit
        size_t loop_end = p->emitter->code_size;
        if (has_cond) {
            emit_patch_branch(p->emitter, exit_branch_pos, loop_end);
        }
        exit_break_context(p, loop_end);
        symtab_exit_scope(&p->symtab);
        return true;
    }

    // 7. Do-While loop: do <stmt> while (<cond>);
    if (p->current_token.type == TOK_DO) {
        advance_token(p); // consume 'do'
        size_t body_pos = p->emitter->code_size;

        BreakContext loop_ctx;
        enter_break_context(p, &loop_ctx, CTX_LOOP, 0);

        if (!compile_statement(p)) return false;

        size_t cond_offset = p->emitter->code_size;
        loop_ctx.continue_target_offset = cond_offset;
        patch_continue_fixups(p, &loop_ctx, cond_offset);

        if (!match(p, TOK_WHILE)) return false;
        if (!match(p, TOK_LPAREN)) return false;
        if (!compile_expression(p)) return false;
        if (!match(p, TOK_RPAREN)) return false;
        if (!match(p, TOK_SEMICOLON)) return false;

        size_t back_pos = p->emitter->code_size;
        emit_bnez(p->emitter, XTENSA_A2, 0);
        emit_patch_branch(p->emitter, back_pos, body_pos);

        size_t loop_end = p->emitter->code_size;
        exit_break_context(p, loop_end);
        return true;
    }

    // 8. Switch statement: switch (<expr>) <stmt>
    if (p->current_token.type == TOK_SWITCH) {
        advance_token(p); // consume 'switch'
        if (!match(p, TOK_LPAREN)) return false;
        if (!compile_expression(p)) return false;
        if (!match(p, TOK_RPAREN)) return false;

        // Save switch discriminant to stack slot
        Symbol *disc_sym = symtab_alloc_local(&p->symtab, "", 0, type_int());
        emit_s32i(p->emitter, XTENSA_A2, XTENSA_A1, disc_sym->stack_offset);

        // Jump over switch body to dispatch table
        size_t jump_to_dispatch_pos = p->emitter->code_size;
        emit_j(p->emitter, 0);

        BreakContext switch_ctx;
        enter_break_context(p, &switch_ctx, CTX_SWITCH, 0);
        switch_ctx.discriminant_slot = disc_sym->stack_offset;

        if (!compile_statement(p)) return false;

        // Jump over dispatch block to switch exit
        size_t jump_over_dispatch_pos = p->emitter->code_size;
        emit_j(p->emitter, 0);

        // Dispatch Block
        size_t dispatch_pos = p->emitter->code_size;
        emit_patch_branch(p->emitter, jump_to_dispatch_pos, dispatch_pos);

        for (size_t i = 0; i < switch_ctx.case_count; i++) {
            emit_l32i(p->emitter, XTENSA_A2, XTENSA_A1, switch_ctx.discriminant_slot);
            emit_load_imm(p->emitter, XTENSA_A11, switch_ctx.cases[i].val);
            size_t case_match_pos = p->emitter->code_size;
            emit_beq(p->emitter, XTENSA_A2, XTENSA_A11, 0);
            emit_patch_branch(p->emitter, case_match_pos, switch_ctx.cases[i].body_pos);
        }

        if (switch_ctx.has_default) {
            size_t default_jump_pos = p->emitter->code_size;
            emit_j(p->emitter, 0);
            emit_patch_branch(p->emitter, default_jump_pos, switch_ctx.default_pos);
        }

        size_t exit_pos = p->emitter->code_size;
        emit_patch_branch(p->emitter, jump_over_dispatch_pos, exit_pos);
        exit_break_context(p, exit_pos);
        return true;
    }

    // 9. Case label: case <const>:
    if (p->current_token.type == TOK_CASE) {
        advance_token(p); // consume 'case'
        BreakContext *sw = find_active_switch(p);
        if (!sw) {
            p->has_error = true;
            snprintf(p->error_msg, sizeof(p->error_msg),
                     "Line %u, Col %u: 'case' label not within a switch statement",
                     p->current_token.line, p->current_token.col);
            return false;
        }

        bool neg = false;
        if (p->current_token.type == TOK_MINUS) {
            neg = true;
            advance_token(p);
        }
        if (p->current_token.type != TOK_INT_LIT) {
            p->has_error = true;
            snprintf(p->error_msg, sizeof(p->error_msg),
                     "Line %u, Col %u: Expected integer constant after 'case'",
                     p->current_token.line, p->current_token.col);
            return false;
        }
        int32_t val = (int32_t)p->current_token.int_val;
        if (neg) val = -val;
        advance_token(p);

        if (!match(p, TOK_COLON)) return false;

        switch_add_case(p, sw, val, p->emitter->code_size);
        return compile_statement(p);
    }

    // 10. Default label: default:
    if (p->current_token.type == TOK_DEFAULT) {
        advance_token(p); // consume 'default'
        BreakContext *sw = find_active_switch(p);
        if (!sw) {
            p->has_error = true;
            snprintf(p->error_msg, sizeof(p->error_msg),
                     "Line %u, Col %u: 'default' label not within a switch statement",
                     p->current_token.line, p->current_token.col);
            return false;
        }
        if (!match(p, TOK_COLON)) return false;

        sw->default_pos = p->emitter->code_size;
        sw->has_default = true;
        return compile_statement(p);
    }

    // 11. Break statement: break;
    if (p->current_token.type == TOK_BREAK) {
        advance_token(p); // consume 'break'
        if (!p->current_break_ctx) {
            p->has_error = true;
            snprintf(p->error_msg, sizeof(p->error_msg),
                     "Line %u, Col %u: 'break' statement not within a loop or switch",
                     p->current_token.line, p->current_token.col);
            return false;
        }
        size_t jump_pos = p->emitter->code_size;
        emit_j(p->emitter, 0); // Placeholder jump to context exit
        add_break_fixup(p, jump_pos);
        return match(p, TOK_SEMICOLON);
    }

    // 12. Continue statement: continue;
    if (p->current_token.type == TOK_CONTINUE) {
        advance_token(p); // consume 'continue'
        BreakContext *loop = find_active_loop(p);
        if (!loop) {
            p->has_error = true;
            snprintf(p->error_msg, sizeof(p->error_msg),
                     "Line %u, Col %u: 'continue' statement not within a loop",
                     p->current_token.line, p->current_token.col);
            return false;
        }
        size_t jump_pos = p->emitter->code_size;
        emit_j(p->emitter, 0);
        if (loop->continue_target_offset != 0) {
            emit_patch_branch(p->emitter, jump_pos, loop->continue_target_offset);
        } else {
            add_continue_fixup(p, loop, jump_pos);
        }
        return match(p, TOK_SEMICOLON);
    }

    // 13. Inline Assembly: __asm__ volatile ("inst" : outputs : inputs : clobbers);
    if (p->current_token.type == TOK_ASM) {
        advance_token(p); // consume '__asm__'
        if (p->current_token.type == TOK_VOLATILE || p->current_token.type == TOK_CONST) {
            advance_token(p);
        }
        if (!match(p, TOK_LPAREN)) return false;
        while (p->current_token.type == TOK_STRING_LIT) {
            const char *asm_str = p->current_token.string_val;
            advance_token(p);

            // Recognize core Xtensa hardware instructions
            if (strstr(asm_str, "memw") != NULL) {
                emit_memw(p->emitter);
            } else if (strstr(asm_str, "isync") != NULL) {
                emit_isync(p->emitter);
            } else if (strstr(asm_str, "esync") != NULL) {
                emit_esync(p->emitter);
            } else if (strstr(asm_str, "nop") != NULL) {
                emit_nop_n(p->emitter);
            } else if (strstr(asm_str, "rsil") != NULL) {
                emit_rsil(p->emitter, XTENSA_A2, 3);
            }
        }
        // Skip any outputs, inputs, clobbers until closing ')'
        int depth = 1;
        while (depth > 0 && !p->has_error && p->current_token.type != TOK_EOF) {
            if (p->current_token.type == TOK_LPAREN) depth++;
            else if (p->current_token.type == TOK_RPAREN) {
                depth--;
                if (depth == 0) {
                    advance_token(p);
                    break;
                }
            }
            advance_token(p);
        }
        return match(p, TOK_SEMICOLON);
    }

    // 14. Empty statement
    if (p->current_token.type == TOK_SEMICOLON) {
        advance_token(p);
        return true;
    }

    // 15. Assignment or Expression statement
    return compile_assignment_or_expr(p, true);
}

// ============================================================================
// Function & Translation Unit Compilation
// ============================================================================

bool compile_function(Parser *p, CompiledFunction *out_fn) {
    if (p->has_error) return false;
    skip_attributes(p);

    DataType ret_type;
    if (!parse_type(p, &ret_type)) return false;

    skip_attributes(p);

    // If declaration is just a type definition ending with semicolon (e.g. struct Tag { ... }; or enum Tag { ... };)
    if (p->current_token.type == TOK_SEMICOLON) {
        advance_token(p);
        if (out_fn) {
            out_fn->name = "";
            out_fn->code = NULL;
            out_fn->code_size = 0;
            out_fn->relocs = NULL;
            out_fn->reloc_count = 0;
        }
        return true;
    }

    if (p->current_token.type != TOK_IDENT) {
        p->has_error = true;
        snprintf(p->error_msg, sizeof(p->error_msg),
                 "Line %u, Col %u: Expected function name identifier",
                 p->current_token.line, p->current_token.col);
        return false;
    }

    bool is_method = false;
    const char *class_tag = NULL;
    size_t class_tag_len = 0;

    Token lookahead_tok;
    Lexer lookahead = *p->lexer;
    lexer_next_token(&lookahead, &lookahead_tok);
    if (lookahead_tok.type == TOK_COLON_COLON) {
        is_method = true;
        class_tag = p->current_token.start;
        class_tag_len = p->current_token.length;
        advance_token(p); // consume class name
        advance_token(p); // consume '::'
    }

    if (p->current_token.type != TOK_IDENT) {
        p->has_error = true;
        snprintf(p->error_msg, sizeof(p->error_msg),
                 "Line %u, Col %u: Expected function name identifier",
                 p->current_token.line, p->current_token.col);
        return false;
    }

    const char *func_name_raw = p->current_token.start;
    size_t func_name_len = p->current_token.length;
    advance_token(p); // consume function name

    const char *func_name;
    if (is_method) {
        char *m = mangle_method_name(p->arena, arena_strndup(p->arena, class_tag, class_tag_len),
                                     arena_strndup(p->arena, func_name_raw, func_name_len));
        func_name = m;
    } else {
        func_name = arena_strndup(p->arena, func_name_raw, func_name_len);
    }

    // Reset per-function emitter & symbol table state
    emit_reset(p->emitter);
    p->symtab.head = NULL;
    p->symtab.current_scope = 0;
    p->symtab.current_stack_offset = 0;
    p->return_patch_count = 0;
    p->current_break_ctx = NULL;
    p->expr_depth = 0;

    size_t param_count = 0;
    if (is_method) {
        StructDef *s_owner = symtab_lookup_struct(&p->symtab, class_tag, class_tag_len);
        DataType this_type = make_struct_type(s_owner, 1, false);
        symtab_alloc_local(&p->symtab, "this", 4, this_type);
        param_count++;
    }

    // Parameter list: (int a, char *b, ...)
    if (!match(p, TOK_LPAREN)) return false;

    if (p->current_token.type != TOK_RPAREN && p->current_token.type != TOK_VOID) {
        while (true) {
            if (p->current_token.type == TOK_ELLIPSIS) {
                advance_token(p); // consume '...'
                break;
            }

            DataType ptype;
            if (!parse_type(p, &ptype)) return false;

            if (p->current_token.type != TOK_IDENT) {
                p->has_error = true;
                snprintf(p->error_msg, sizeof(p->error_msg),
                 "Line %u, Col %u: Expected parameter identifier",
                 p->current_token.line, p->current_token.col);
                return false;
            }

            const char *pname = p->current_token.start;
            size_t plen = p->current_token.length;
            advance_token(p);

            symtab_alloc_local(&p->symtab, pname, plen, ptype);
            param_count++;

            if (p->current_token.type == TOK_COMMA) {
                advance_token(p);
            } else {
                break;
            }
        }
    } else if (p->current_token.type == TOK_VOID) {
        advance_token(p);
    }
    if (!match(p, TOK_RPAREN)) return false;

    // If followed by semicolon, this is a prototype declaration!
    if (p->current_token.type == TOK_SEMICOLON) {
        advance_token(p); // consume ';'
        if (out_fn) {
            out_fn->name = func_name;
            out_fn->code = NULL;
            out_fn->code_size = 0;
            out_fn->relocs = NULL;
            out_fn->reloc_count = 0;
        }
        return true;
    }

    // Reserve space for prologue:
    // 1. addi a1, a1, -frame_size (3 bytes)
    // 2. s32i a0, a1, frame_size - 4 (3 bytes)
    // 3. Spills for params a2..a7 (param_count * 3 bytes)
    size_t prologue_pos = p->emitter->code_size;
    emit_addi(p->emitter, XTENSA_A1, XTENSA_A1, 0); // Placeholder
    emit_s32i(p->emitter, XTENSA_A0, XTENSA_A1, 0); // Placeholder

    for (size_t i = 0; i < param_count && i < 6; i++) {
        emit_s32i(p->emitter, (Reg)(XTENSA_A2 + i), XTENSA_A1, (uint32_t)(i * 4));
    }

    // Function body: { ... }
    if (p->current_token.type != TOK_LBRACE) {
        p->has_error = true;
        snprintf(p->error_msg, sizeof(p->error_msg),
                 "Line %u, Col %u: Expected '{' at function body",
                 p->current_token.line, p->current_token.col);
        return false;
    }
    advance_token(p); // consume '{'

    while (p->current_token.type != TOK_RBRACE && p->current_token.type != TOK_EOF) {
        if (!compile_statement(p)) return false;
    }

    if (!match(p, TOK_RBRACE)) return false; // consume '}'

    // Patch function return statements
    size_t epilogue_pos = p->emitter->code_size;
    for (size_t i = 0; i < p->return_patch_count; i++) {
        size_t patch_pos = p->return_patches[i];
        emit_patch_branch(p->emitter, patch_pos, epilogue_pos);
    }

    // Frame size calculation (reserve 4 bytes for saved a0)
    int total_local_bytes = p->symtab.current_stack_offset + (MAX_SCRATCH_REGS * 4) + 4;
    int frame_size = (total_local_bytes + 15) & ~15; // 16-byte aligned frame

    // Patch Prologue:
    if (frame_size <= 128) {
        uint8_t *b = p->emitter->code + prologue_pos;
        int8_t neg_frame = (int8_t)(-frame_size);
        b[0] = 0x12;
        b[1] = 0xC1;
        b[2] = (uint8_t)neg_frame;

        uint32_t a0_spill_offset = (uint32_t)(frame_size - 4);
        uint32_t imm8 = a0_spill_offset / 4;
        uint8_t *s = p->emitter->code + prologue_pos + 3;
        s[0] = 0x02;
        s[1] = 0x61;
        s[2] = (uint8_t)imm8;
    }

    // Emit Epilogue:
    emit_l32i(p->emitter, XTENSA_A0, XTENSA_A1, frame_size - 4);
    emit_addi(p->emitter, XTENSA_A1, XTENSA_A1, (int8_t)frame_size);
    emit_ret_n(p->emitter);

    // Finalize binary and flush literal pool
    size_t final_size = 0;
    uint8_t *final_code = emit_finalize(p->emitter, &final_size);

    if (out_fn) {
        out_fn->name = func_name;
        out_fn->code = final_code;
        out_fn->code_size = final_size;
        out_fn->reloc_count = p->emitter->reloc_count;
        if (p->emitter->reloc_count > 0) {
            out_fn->relocs = (RelocEntry *)arena_alloc(p->arena, p->emitter->reloc_count * sizeof(RelocEntry));
            if (out_fn->relocs) {
                memcpy(out_fn->relocs, p->emitter->relocs, p->emitter->reloc_count * sizeof(RelocEntry));
            }
        } else {
            out_fn->relocs = NULL;
        }
    }
    return true;
}

bool compile_translation_unit(Parser *p, CompiledFunction **out_fns, size_t *out_count) {
    if (!p) return false;

    size_t cap = 8;
    size_t count = 0;
    CompiledFunction *fns = (CompiledFunction *)arena_alloc(p->arena, cap * sizeof(CompiledFunction));
    if (!fns) return false;

    while (p->current_token.type != TOK_EOF && !p->has_error) {
        if (p->current_token.type == TOK_SEMICOLON) {
            advance_token(p);
            continue;
        }

        // Top-level extern "C" { ... }
        if (p->current_token.type == TOK_EXTERN) {
            advance_token(p); // consume 'extern'
            if (p->current_token.type == TOK_STRING_LIT) {
                advance_token(p); // consume "C"
                if (p->current_token.type == TOK_LBRACE) {
                    advance_token(p); // consume '{'
                    while (p->current_token.type != TOK_RBRACE && !p->has_error && p->current_token.type != TOK_EOF) {
                        if (count >= cap) {
                            size_t new_cap = cap * 2;
                            CompiledFunction *new_fns = (CompiledFunction *)arena_alloc(p->arena, new_cap * sizeof(CompiledFunction));
                            if (!new_fns) return false;
                            memcpy(new_fns, fns, count * sizeof(CompiledFunction));
                            fns = new_fns;
                            cap = new_cap;
                        }
                        if (!compile_function(p, &fns[count])) return false;
                        if (fns[count].code_size > 0) count++;
                    }
                    if (!match(p, TOK_RBRACE)) return false;
                    continue;
                }
            }
        }

        // Top-level namespace Name { ... }
        if (p->current_token.type == TOK_NAMESPACE) {
            advance_token(p); // consume 'namespace'
            const char *ns_name = p->current_token.start;
            size_t ns_len = p->current_token.length;
            if (!match(p, TOK_IDENT)) return false;
            if (!match(p, TOK_LBRACE)) return false;

            while (p->current_token.type != TOK_RBRACE && !p->has_error && p->current_token.type != TOK_EOF) {
                if (count >= cap) {
                    size_t new_cap = cap * 2;
                    CompiledFunction *new_fns = (CompiledFunction *)arena_alloc(p->arena, new_cap * sizeof(CompiledFunction));
                    if (!new_fns) return false;
                    memcpy(new_fns, fns, count * sizeof(CompiledFunction));
                    fns = new_fns;
                    cap = new_cap;
                }
                if (!compile_function(p, &fns[count])) return false;
                if (fns[count].code_size > 0) {
                    char scoped_name[256];
                    snprintf(scoped_name, sizeof(scoped_name), "%.*s_%s", (int)ns_len, ns_name, fns[count].name);
                    fns[count].name = arena_strndup(p->arena, scoped_name, strlen(scoped_name));
                    count++;
                }
            }
            if (!match(p, TOK_RBRACE)) return false;
            continue;
        }

        // Top-level typedef: typedef <type> <name>;
        if (p->current_token.type == TOK_TYPEDEF) {
            advance_token(p); // consume 'typedef'
            DataType t;
            if (!parse_type(p, &t)) return false;
            if (p->current_token.type != TOK_IDENT) {
                p->has_error = true;
                snprintf(p->error_msg, sizeof(p->error_msg),
                         "Line %u, Col %u: Expected identifier in typedef",
                         p->current_token.line, p->current_token.col);
                return false;
            }
            const char *tname = p->current_token.start;
            size_t tlen = p->current_token.length;
            advance_token(p);
            symtab_define_typedef(&p->symtab, tname, tlen, t);
            if (!match(p, TOK_SEMICOLON)) return false;
            continue;
        }

        // Top-level enum definition: enum Tag { ... };
        if (p->current_token.type == TOK_ENUM) {
            DataType t;
            if (!parse_type(p, &t)) return false;
            if (p->current_token.type == TOK_SEMICOLON) {
                advance_token(p);
                continue;
            }
        }

        // Top-level class / struct / union definition: class/struct/union Tag { ... };
        if (p->current_token.type == TOK_STRUCT || p->current_token.type == TOK_CLASS || p->current_token.type == TOK_UNION) {
            Token next_tok;
            Lexer lookahead = *p->lexer;
            lexer_next_token(&lookahead, &next_tok);
            if (next_tok.type == TOK_IDENT) {
                Token third_tok;
                lexer_next_token(&lookahead, &third_tok);
                if (third_tok.type == TOK_LBRACE) {
                    DataType t;
                    if (!parse_type(p, &t)) return false;
                    if (p->current_token.type == TOK_SEMICOLON) {
                        advance_token(p);
                    }
                    continue;
                }
            }
        }

        if (count >= cap) {
            size_t new_cap = cap * 2;
            CompiledFunction *new_fns = (CompiledFunction *)arena_alloc(p->arena, new_cap * sizeof(CompiledFunction));
            if (!new_fns) return false;
            memcpy(new_fns, fns, count * sizeof(CompiledFunction));
            fns = new_fns;
            cap = new_cap;
        }

        if (!compile_function(p, &fns[count])) {
            return false;
        }
        if (fns[count].code_size > 0) {
            count++;
        }
    }

    if (out_fns) *out_fns = fns;
    if (out_count) *out_count = count;
    return !p->has_error;
}

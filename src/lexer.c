#define _GNU_SOURCE
#include "lexer.h"
#include <ctype.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

void lexer_init(Lexer *l, const char *source, Arena *arena) {
    if (!l) return;
    memset(l, 0, sizeof(*l));
    l->arena = arena;
    l->stack_ptr = 0;
    l->stack[0].src = source ? source : "";
    l->stack[0].pos = 0;
    l->stack[0].line = 1;
    l->stack[0].col = 1;
    l->stack[0].filename = "main.c";
    l->cond_depth = 0;

    // Define standard C99, C++ & architecture predefined macros
    lexer_define_macro(l, "__STDC__", 8, "1", 1);
    lexer_define_macro(l, "__STDC_VERSION__", 16, "199901L", 7);
    lexer_define_macro(l, "__cplusplus", 11, "199711L", 7);
    lexer_define_macro(l, "__xtensa__", 10, "1", 1);
    lexer_define_macro(l, "__XTENSA_CALL0_ABI__", 20, "1", 1);
}

void lexer_add_include_path(Lexer *l, const char *path) {
    if (!l || !path) return;
    if (l->include_path_count >= l->include_path_capacity) {
        size_t new_cap = l->include_path_capacity == 0 ? 4 : l->include_path_capacity * 2;
        const char **new_arr;
        if (l->arena) {
            new_arr = (const char **)arena_alloc(l->arena, new_cap * sizeof(const char *));
            if (l->include_paths && l->include_path_count > 0) {
                memcpy((void *)new_arr, l->include_paths, l->include_path_count * sizeof(const char *));
            }
        } else {
            new_arr = (const char **)realloc(l->include_paths, new_cap * sizeof(const char *));
        }
        l->include_paths = new_arr;
        l->include_path_capacity = new_cap;
    }
    l->include_paths[l->include_path_count++] = path;
}

void lexer_define_macro(Lexer *l, const char *name, size_t name_len, const char *val, size_t val_len) {
    if (!l || !name || name_len == 0) return;

    // Replace if already exists
    Macro *m = lexer_lookup_macro(l, name, name_len);
    if (m) {
        if (l->arena) {
            char *v = (char *)arena_alloc(l->arena, val_len + 1);
            memcpy(v, val, val_len);
            v[val_len] = '\0';
            m->replacement = v;
        } else {
            char *v = (char *)malloc(val_len + 1);
            memcpy(v, val, val_len);
            v[val_len] = '\0';
            m->replacement = v;
        }
        m->repl_len = val_len;
        return;
    }

    if (l->arena) {
        m = (Macro *)arena_alloc(l->arena, sizeof(Macro));
        char *n = (char *)arena_alloc(l->arena, name_len + 1);
        memcpy(n, name, name_len);
        n[name_len] = '\0';
        m->name = n;

        char *v = (char *)arena_alloc(l->arena, val_len + 1);
        memcpy(v, val, val_len);
        v[val_len] = '\0';
        m->replacement = v;
    } else {
        m = (Macro *)malloc(sizeof(Macro));
        char *n = (char *)malloc(name_len + 1);
        memcpy(n, name, name_len);
        n[name_len] = '\0';
        m->name = n;

        char *v = (char *)malloc(val_len + 1);
        memcpy(v, val, val_len);
        v[val_len] = '\0';
        m->replacement = v;
    }
    m->name_len = name_len;
    m->repl_len = val_len;
    m->next = l->macros;
    l->macros = m;
}

void lexer_undef_macro(Lexer *l, const char *name, size_t name_len) {
    if (!l || !name || name_len == 0) return;

    Macro **curr = &l->macros;
    while (*curr) {
        if ((*curr)->name_len == name_len && memcmp((*curr)->name, name, name_len) == 0) {
            *curr = (*curr)->next;
            return;
        }
        curr = &((*curr)->next);
    }
}

Macro *lexer_lookup_macro(const Lexer *l, const char *name, size_t name_len) {
    if (!l || !name) return NULL;
    for (Macro *m = l->macros; m != NULL; m = m->next) {
        if (m->name_len == name_len && memcmp(m->name, name, name_len) == 0) {
            return m;
        }
    }
    return NULL;
}

static inline SourceBuffer *current_buf(Lexer *l) {
    return &l->stack[l->stack_ptr];
}

static inline char peek(Lexer *l) {
    while (l->stack_ptr >= 0) {
        SourceBuffer *b = &l->stack[l->stack_ptr];
        char c = b->src[b->pos];
        if (c != '\0') return c;
        if (l->stack_ptr > 0) {
            l->stack_ptr--;
            continue;
        }
        return '\0';
    }
    return '\0';
}

static inline char peek_at(Lexer *l, size_t offset) {
    if (l->stack_ptr < 0) return '\0';
    SourceBuffer *b = &l->stack[l->stack_ptr];
    return b->src[b->pos + offset];
}

static inline char advance(Lexer *l) {
    while (l->stack_ptr >= 0) {
        SourceBuffer *b = &l->stack[l->stack_ptr];
        char c = b->src[b->pos];
        if (c != '\0') {
            b->pos++;
            if (c == '\n') {
                b->line++;
                b->col = 1;
            } else {
                b->col++;
            }
            return c;
        }
        if (l->stack_ptr > 0) {
            l->stack_ptr--;
            continue;
        }
        return '\0';
    }
    return '\0';
}

static bool is_cond_active(const Lexer *l) {
    if (l->cond_depth == 0) return true;
    return l->cond_stack[l->cond_depth - 1].active;
}

static void handle_preprocessor_directive(Lexer *l) {
    advance(l); // consume '#'
    while (peek(l) == ' ' || peek(l) == '\t') {
        advance(l);
    }

    char dir[32];
    size_t dir_len = 0;
    while (isalpha((unsigned char)peek(l)) && dir_len < sizeof(dir) - 1) {
        dir[dir_len++] = advance(l);
    }
    dir[dir_len] = '\0';

    bool parent_active = (l->cond_depth == 0) || l->cond_stack[l->cond_depth - 1].active;

    if (strcmp(dir, "ifdef") == 0) {
        while (peek(l) == ' ' || peek(l) == '\t') advance(l);
        SourceBuffer *b = current_buf(l);
        const char *name_start = b->src + b->pos;
        while (isalnum((unsigned char)peek(l)) || peek(l) == '_') advance(l);
        size_t name_len = (size_t)((b->src + b->pos) - name_start);

        bool defined = (name_len > 0) && (lexer_lookup_macro(l, name_start, name_len) != NULL);
        if (l->cond_depth < MAX_COND_DEPTH) {
            bool active = parent_active && defined;
            l->cond_stack[l->cond_depth].active = active;
            l->cond_stack[l->cond_depth].taken = active;
            l->cond_depth++;
        }
        while (peek(l) != '\0' && peek(l) != '\n') advance(l);
        return;
    }

    if (strcmp(dir, "ifndef") == 0) {
        while (peek(l) == ' ' || peek(l) == '\t') advance(l);
        SourceBuffer *b = current_buf(l);
        const char *name_start = b->src + b->pos;
        while (isalnum((unsigned char)peek(l)) || peek(l) == '_') advance(l);
        size_t name_len = (size_t)((b->src + b->pos) - name_start);

        bool defined = (name_len > 0) && (lexer_lookup_macro(l, name_start, name_len) != NULL);
        if (l->cond_depth < MAX_COND_DEPTH) {
            bool active = parent_active && !defined;
            l->cond_stack[l->cond_depth].active = active;
            l->cond_stack[l->cond_depth].taken = active;
            l->cond_depth++;
        }
        while (peek(l) != '\0' && peek(l) != '\n') advance(l);
        return;
    }

    if (strcmp(dir, "elif") == 0) {
        while (peek(l) == ' ' || peek(l) == '\t') advance(l);
        SourceBuffer *b = current_buf(l);
        const char *name_start = b->src + b->pos;
        while (isalnum((unsigned char)peek(l)) || peek(l) == '_') advance(l);
        size_t name_len = (size_t)((b->src + b->pos) - name_start);

        if (l->cond_depth > 0) {
            bool grand_parent_active = (l->cond_depth <= 1) || l->cond_stack[l->cond_depth - 2].active;
            bool defined = (name_len > 0) && (lexer_lookup_macro(l, name_start, name_len) != NULL);
            if (!l->cond_stack[l->cond_depth - 1].taken && grand_parent_active && defined) {
                l->cond_stack[l->cond_depth - 1].active = true;
                l->cond_stack[l->cond_depth - 1].taken = true;
            } else {
                l->cond_stack[l->cond_depth - 1].active = false;
            }
        }
        while (peek(l) != '\0' && peek(l) != '\n') advance(l);
        return;
    }

    if (strcmp(dir, "else") == 0) {
        if (l->cond_depth > 0) {
            bool grand_parent_active = (l->cond_depth <= 1) || l->cond_stack[l->cond_depth - 2].active;
            if (!l->cond_stack[l->cond_depth - 1].taken && grand_parent_active) {
                l->cond_stack[l->cond_depth - 1].active = true;
                l->cond_stack[l->cond_depth - 1].taken = true;
            } else {
                l->cond_stack[l->cond_depth - 1].active = false;
            }
        }
        while (peek(l) != '\0' && peek(l) != '\n') advance(l);
        return;
    }

    if (strcmp(dir, "endif") == 0) {
        if (l->cond_depth > 0) {
            l->cond_depth--;
        }
        while (peek(l) != '\0' && peek(l) != '\n') advance(l);
        return;
    }

    // Directives only processed if conditional block is active
    if (!parent_active) {
        while (peek(l) != '\0' && peek(l) != '\n') advance(l);
        return;
    }

    if (strcmp(dir, "undef") == 0) {
        while (peek(l) == ' ' || peek(l) == '\t') advance(l);
        SourceBuffer *b = current_buf(l);
        const char *name_start = b->src + b->pos;
        while (isalnum((unsigned char)peek(l)) || peek(l) == '_') advance(l);
        size_t name_len = (size_t)((b->src + b->pos) - name_start);
        if (name_len > 0) {
            lexer_undef_macro(l, name_start, name_len);
        }
        while (peek(l) != '\0' && peek(l) != '\n') advance(l);
        return;
    }

    if (strcmp(dir, "define") == 0) {
        while (peek(l) == ' ' || peek(l) == '\t') advance(l);

        SourceBuffer *b = current_buf(l);
        const char *name_start = b->src + b->pos;
        while (isalnum((unsigned char)peek(l)) || peek(l) == '_') {
            advance(l);
        }
        size_t name_len = (size_t)((b->src + b->pos) - name_start);

        while (peek(l) == ' ' || peek(l) == '\t') advance(l);

        const char *val_start = b->src + b->pos;
        while (peek(l) != '\0' && peek(l) != '\n' && peek(l) != '\r') {
            if (peek(l) == '/' && peek_at(l, 1) == '/') break;
            advance(l);
        }
        const char *val_end = b->src + b->pos;
        while (val_end > val_start && (val_end[-1] == ' ' || val_end[-1] == '\t')) {
            val_end--;
        }
        size_t val_len = (size_t)(val_end - val_start);

        if (name_len > 0) {
            lexer_define_macro(l, name_start, name_len, val_start, val_len);
        }
    } else if (strcmp(dir, "include") == 0) {
        while (peek(l) == ' ' || peek(l) == '\t') advance(l);

        char quote = peek(l);
        if (quote == '"' || quote == '<') {
            char end_quote = (quote == '"') ? '"' : '>';
            advance(l); // consume opening quote

            char inc_path[512];
            size_t inc_len = 0;
            while (peek(l) != end_quote && peek(l) != '\0' && peek(l) != '\n' && inc_len < sizeof(inc_path) - 1) {
                inc_path[inc_len++] = advance(l);
            }
            inc_path[inc_len] = '\0';
            if (peek(l) == end_quote) {
                advance(l); // consume closing quote
            }

            // Try opening file directly, relative to current file dir, or via include search paths
            FILE *f = fopen(inc_path, "rb");
            if (!f && current_buf(l)->filename) {
                const char *cur_file = current_buf(l)->filename;
                const char *last_slash = strrchr(cur_file, '/');
                if (!last_slash) last_slash = strrchr(cur_file, '\\');
                if (last_slash) {
                    size_t dir_len = (size_t)(last_slash - cur_file);
                    char rel[1024];
                    snprintf(rel, sizeof(rel), "%.*s/%s", (int)dir_len, cur_file, inc_path);
                    f = fopen(rel, "rb");
                }
            }
            if (!f) {
                // Check built-in include/ directory
                char builtin_path[1024];
                snprintf(builtin_path, sizeof(builtin_path), "include/%s", inc_path);
                f = fopen(builtin_path, "rb");
            }
            if (!f && l->include_paths) {
                for (size_t i = 0; i < l->include_path_count; i++) {
                    char full[1024];
                    snprintf(full, sizeof(full), "%s/%s", l->include_paths[i], inc_path);
                    f = fopen(full, "rb");
                    if (f) break;
                }
            }

            if (f) {
                fseek(f, 0, SEEK_END);
                long sz = ftell(f);
                fseek(f, 0, SEEK_SET);
                if (sz >= 0) {
                    char *content;
                    if (l->arena) {
                        content = (char *)arena_alloc(l->arena, (size_t)sz + 1);
                    } else {
                        content = (char *)malloc((size_t)sz + 1);
                    }
                    if (content) {
                        size_t read_bytes = fread(content, 1, (size_t)sz, f);
                        content[read_bytes] = '\0';

                        if (l->stack_ptr + 1 < MAX_INCLUDE_DEPTH) {
                            l->stack_ptr++;
                            SourceBuffer *nb = &l->stack[l->stack_ptr];
                            nb->src = content;
                            nb->pos = 0;
                            nb->line = 1;
                            nb->col = 1;
                            nb->filename = l->arena ? arena_strdup(l->arena, inc_path) : strdup(inc_path);
                        }
                    }
                }
                fclose(f);
            }
        }
    } else {
        // Skip unknown preprocessor directive (e.g. #pragma)
        while (peek(l) != '\0' && peek(l) != '\n') {
            advance(l);
        }
    }
}

static void skip_whitespace_and_comments(Lexer *l) {
    while (true) {
        char c = peek(l);
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            advance(l);
        } else if (c == '/' && peek_at(l, 1) == '/') {
            advance(l);
            advance(l);
            while (peek(l) != '\0' && peek(l) != '\n') {
                advance(l);
            }
        } else if (c == '/' && peek_at(l, 1) == '*') {
            advance(l);
            advance(l);
            while (peek(l) != '\0') {
                if (peek(l) == '*' && peek_at(l, 1) == '/') {
                    advance(l);
                    advance(l);
                    break;
                }
                advance(l);
            }
        } else if (c == '#') {
            handle_preprocessor_directive(l);
        } else if (!is_cond_active(l)) {
            // In an inactive conditional branch: skip until next directive or EOF
            advance(l);
        } else {
            break;
        }
    }
}

static TokenType classify_ident(const char *str, size_t len) {
    switch (len) {
        case 2:
            if (memcmp(str, "if", 2) == 0) return TOK_IF;
            if (memcmp(str, "do", 2) == 0) return TOK_DO;
            break;
        case 3:
            if (memcmp(str, "int", 3) == 0) return TOK_INT;
            if (memcmp(str, "for", 3) == 0) return TOK_FOR;
            if (memcmp(str, "new", 3) == 0) return TOK_NEW;
            if (memcmp(str, "asm", 3) == 0) return TOK_ASM;
            break;
        case 4:
            if (memcmp(str, "char", 4) == 0) return TOK_CHAR;
            if (memcmp(str, "void", 4) == 0) return TOK_VOID;
            if (memcmp(str, "else", 4) == 0) return TOK_ELSE;
            if (memcmp(str, "case", 4) == 0) return TOK_CASE;
            if (memcmp(str, "enum", 4) == 0) return TOK_ENUM;
            if (memcmp(str, "long", 4) == 0) return TOK_LONG;
            if (memcmp(str, "bool", 4) == 0) return TOK_BOOL;
            if (memcmp(str, "this", 4) == 0) return TOK_THIS;
            break;
        case 5:
            if (memcmp(str, "_Bool", 5) == 0) return TOK_BOOL;
            if (memcmp(str, "while", 5) == 0) return TOK_WHILE;
            if (memcmp(str, "break", 5) == 0) return TOK_BREAK;
            if (memcmp(str, "const", 5) == 0) return TOK_CONST;
            if (memcmp(str, "short", 5) == 0) return TOK_SHORT;
            if (memcmp(str, "class", 5) == 0) return TOK_CLASS;
            if (memcmp(str, "union", 5) == 0) return TOK_UNION;
            if (memcmp(str, "float", 5) == 0) return TOK_FLOAT;
            if (memcmp(str, "__asm", 5) == 0) return TOK_ASM;
            break;
        case 6:
            if (memcmp(str, "return", 6) == 0) return TOK_RETURN;
            if (memcmp(str, "struct", 6) == 0) return TOK_STRUCT;
            if (memcmp(str, "sizeof", 6) == 0) return TOK_SIZEOF;
            if (memcmp(str, "switch", 6) == 0) return TOK_SWITCH;
            if (memcmp(str, "signed", 6) == 0) return TOK_SIGNED;
            if (memcmp(str, "inline", 6) == 0) return TOK_INLINE;
            if (memcmp(str, "public", 6) == 0) return TOK_PUBLIC;
            if (memcmp(str, "extern", 6) == 0) return TOK_EXTERN;
            if (memcmp(str, "delete", 6) == 0) return TOK_DELETE;
            if (memcmp(str, "double", 6) == 0) return TOK_DOUBLE;
            if (memcmp(str, "typeof", 6) == 0) return TOK_TYPEOF;
            if (memcmp(str, "__asm__", 7) == 0) return TOK_ASM;
            break;
        case 7:
            if (memcmp(str, "default", 7) == 0) return TOK_DEFAULT;
            if (memcmp(str, "typedef", 7) == 0) return TOK_TYPEDEF;
            if (memcmp(str, "private", 7) == 0) return TOK_PRIVATE;
            if (memcmp(str, "__asm__", 7) == 0) return TOK_ASM;
            break;
        case 8:
            if (memcmp(str, "volatile", 8) == 0) return TOK_VOLATILE;
            if (memcmp(str, "continue", 8) == 0) return TOK_CONTINUE;
            if (memcmp(str, "restrict", 8) == 0) return TOK_RESTRICT;
            if (memcmp(str, "unsigned", 8) == 0) return TOK_UNSIGNED;
            if (memcmp(str, "operator", 8) == 0) return TOK_OPERATOR;
            if (memcmp(str, "__typeof", 8) == 0) return TOK_TYPEOF;
            break;
        case 9:
            if (memcmp(str, "protected", 9) == 0) return TOK_PROTECTED;
            if (memcmp(str, "namespace", 9) == 0) return TOK_NAMESPACE;
            break;
        case 10:
            if (memcmp(str, "__typeof__", 10) == 0) return TOK_TYPEOF;
            if (memcmp(str, "__declspec", 10) == 0) return TOK_DECLSPEC;
            break;
        case 11:
            if (memcmp(str, "__attribute", 11) == 0) return TOK_ATTRIBUTE;
            break;
        case 13:
            if (memcmp(str, "__attribute__", 13) == 0) return TOK_ATTRIBUTE;
            if (memcmp(str, "__extension__", 13) == 0) return TOK_EXTENSION;
            break;
        default:
            break;
    }
    return TOK_IDENT;
}

static void scan_number(Lexer *l, Token *tok) {
    SourceBuffer *b = current_buf(l);
    const char *start = b->src + b->pos;
    int base = 10;

    if (peek(l) == '0' && (peek_at(l, 1) == 'x' || peek_at(l, 1) == 'X')) {
        base = 16;
        advance(l);
        advance(l);
        while (isxdigit((unsigned char)peek(l))) {
            advance(l);
        }
    } else if (peek(l) == '0' && (peek_at(l, 1) == 'b' || peek_at(l, 1) == 'B')) {
        base = 2;
        advance(l);
        advance(l);
        while (peek(l) == '0' || peek(l) == '1') {
            advance(l);
        }
    } else if (peek(l) == '0' && isdigit((unsigned char)peek_at(l, 1))) {
        base = 8;
        advance(l);
        while (peek(l) >= '0' && peek(l) <= '7') {
            advance(l);
        }
    } else {
        while (isdigit((unsigned char)peek(l))) {
            advance(l);
        }
    }

    tok->type = TOK_INT_LIT;
    tok->length = (size_t)((b->src + b->pos) - start);

    if (base == 2) {
        tok->int_val = strtoll(start + 2, NULL, 2);
    } else {
        tok->int_val = strtoll(start, NULL, base);
    }

    // Skip integer suffixes: U, L, UL, ULL, LL
    while (peek(l) == 'u' || peek(l) == 'U' || peek(l) == 'l' || peek(l) == 'L') {
        advance(l);
    }
}

static void scan_string_literal(Lexer *l, Token *tok) {
    advance(l); // consume opening quote
    SourceBuffer *b = current_buf(l);
    const char *start = b->src + b->pos;

    char *buf = NULL;
    size_t cap = 64;
    size_t len = 0;

    if (l->arena) {
        buf = (char *)arena_alloc(l->arena, cap);
    } else {
        buf = (char *)malloc(cap);
    }

    while (peek(l) != '"' && peek(l) != '\0') {
        char c = advance(l);
        if (c == '\\') {
            char esc = advance(l);
            switch (esc) {
                case 'n': c = '\n'; break;
                case 't': c = '\t'; break;
                case 'r': c = '\r'; break;
                case '0': c = '\0'; break;
                case '\\': c = '\\'; break;
                case '\'': c = '\''; break;
                case '"': c = '"'; break;
                case 'x': {
                    char hex[3] = {0};
                    if (isxdigit((unsigned char)peek(l))) hex[0] = advance(l);
                    if (isxdigit((unsigned char)peek(l))) hex[1] = advance(l);
                    c = (char)strtol(hex, NULL, 16);
                    break;
                }
                default: c = esc; break;
            }
        }

        if (len + 2 >= cap) {
            size_t new_cap = cap * 2;
            char *new_buf;
            if (l->arena) {
                new_buf = (char *)arena_alloc(l->arena, new_cap);
                memcpy(new_buf, buf, len);
            } else {
                new_buf = (char *)realloc(buf, new_cap);
            }
            buf = new_buf;
            cap = new_cap;
        }
        buf[len++] = c;
    }

    if (peek(l) == '"') {
        advance(l); // consume closing quote
    }

    buf[len] = '\0';
    tok->type = TOK_STRING_LIT;
    tok->length = (size_t)((b->src + b->pos) - start);
    tok->string_val = buf;
    tok->string_len = len;
}

static void scan_char_literal(Lexer *l, Token *tok) {
    advance(l); // consume opening single quote
    char c = advance(l);
    if (c == '\\') {
        char esc = advance(l);
        switch (esc) {
            case 'n': c = '\n'; break;
            case 't': c = '\t'; break;
            case 'r': c = '\r'; break;
            case '0': c = '\0'; break;
            case '\\': c = '\\'; break;
            case '\'': c = '\''; break;
            case '"': c = '"'; break;
            case 'x': {
                char hex[3] = {0};
                if (isxdigit((unsigned char)peek(l))) hex[0] = advance(l);
                if (isxdigit((unsigned char)peek(l))) hex[1] = advance(l);
                c = (char)strtol(hex, NULL, 16);
                break;
            }
            default: c = esc; break;
        }
    }
    if (peek(l) == '\'') {
        advance(l); // consume closing quote
    }
    tok->type = TOK_INT_LIT;
    tok->int_val = (int64_t)(unsigned char)c;
    tok->length = 3;
}

bool lexer_next_token(Lexer *l, Token *tok) {
    if (!l || !tok) return false;

    skip_whitespace_and_comments(l);

    SourceBuffer *b = current_buf(l);
    tok->line = b->line;
    tok->col = b->col;
    tok->start = b->src + b->pos;
    tok->length = 0;
    tok->int_val = 0;
    tok->string_val = NULL;
    tok->string_len = 0;

    char c = peek(l);
    if (c == '\0') {
        tok->type = TOK_EOF;
        return false;
    }

    if (isdigit((unsigned char)c)) {
        scan_number(l, tok);
        return true;
    }

    if (c == '"') {
        scan_string_literal(l, tok);
        return true;
    }

    if (c == '\'') {
        scan_char_literal(l, tok);
        return true;
    }

    if (isalpha((unsigned char)c) || c == '_') {
        const char *start = b->src + b->pos;
        while (isalnum((unsigned char)peek(l)) || peek(l) == '_') {
            advance(l);
        }
        size_t len = (size_t)((b->src + b->pos) - start);
        tok->length = len;

        // Check if this identifier is a defined macro
        Macro *m = lexer_lookup_macro(l, start, len);
        if (m) {
            // Push macro replacement onto buffer stack
            if (l->stack_ptr + 1 < MAX_INCLUDE_DEPTH) {
                l->stack_ptr++;
                SourceBuffer *nb = &l->stack[l->stack_ptr];
                nb->src = m->replacement;
                nb->pos = 0;
                nb->line = tok->line;
                nb->col = tok->col;
                nb->filename = b->filename;
                return lexer_next_token(l, tok);
            }
        }

        tok->type = classify_ident(start, len);
        return true;
    }

    // Operators & Punctuation
    advance(l);
    tok->length = 1;

    switch (c) {
        case '+':
            if (peek(l) == '+') {
                advance(l);
                tok->type = TOK_PLUS_PLUS;
                tok->length = 2;
            } else if (peek(l) == '=') {
                advance(l);
                tok->type = TOK_PLUS_ASSIGN;
                tok->length = 2;
            } else {
                tok->type = TOK_PLUS;
            }
            return true;
        case '-':
            if (peek(l) == '-') {
                advance(l);
                tok->type = TOK_MINUS_MINUS;
                tok->length = 2;
            } else if (peek(l) == '=') {
                advance(l);
                tok->type = TOK_MINUS_ASSIGN;
                tok->length = 2;
            } else if (peek(l) == '>') {
                advance(l);
                tok->type = TOK_ARROW;
                tok->length = 2;
            } else {
                tok->type = TOK_MINUS;
            }
            return true;
        case '.':
            if (peek(l) == '.' && peek_at(l, 1) == '.') {
                advance(l);
                advance(l);
                tok->type = TOK_ELLIPSIS;
                tok->length = 3;
            } else {
                tok->type = TOK_DOT;
            }
            return true;
        case '*':
            if (peek(l) == '=') {
                advance(l);
                tok->type = TOK_STAR_ASSIGN;
                tok->length = 2;
            } else {
                tok->type = TOK_STAR;
            }
            return true;
        case '/':
            if (peek(l) == '=') {
                advance(l);
                tok->type = TOK_SLASH_ASSIGN;
                tok->length = 2;
            } else {
                tok->type = TOK_SLASH;
            }
            return true;
        case '%':
            if (peek(l) == '=') {
                advance(l);
                tok->type = TOK_PERCENT_ASSIGN;
                tok->length = 2;
            } else {
                tok->type = TOK_PERCENT;
            }
            return true;
        case '(':
            tok->type = TOK_LPAREN;
            return true;
        case ')':
            tok->type = TOK_RPAREN;
            return true;
        case '{':
            tok->type = TOK_LBRACE;
            return true;
        case '}':
            tok->type = TOK_RBRACE;
            return true;
        case '[':
            tok->type = TOK_LBRACKET;
            return true;
        case ']':
            tok->type = TOK_RBRACKET;
            return true;
        case ';':
            tok->type = TOK_SEMICOLON;
            return true;
        case ',':
            tok->type = TOK_COMMA;
            return true;
        case '~':
            tok->type = TOK_TILDE;
            return true;
        case '^':
            if (peek(l) == '=') {
                advance(l);
                tok->type = TOK_CARET_ASSIGN;
                tok->length = 2;
            } else {
                tok->type = TOK_CARET;
            }
            return true;
        case '?':
            tok->type = TOK_QUESTION;
            return true;
        case ':':
            if (peek(l) == ':') {
                advance(l);
                tok->type = TOK_COLON_COLON;
                tok->length = 2;
            } else {
                tok->type = TOK_COLON;
            }
            return true;

        case '=':
            if (peek(l) == '=') {
                advance(l);
                tok->type = TOK_EQ;
                tok->length = 2;
            } else {
                tok->type = TOK_ASSIGN;
            }
            return true;

        case '!':
            if (peek(l) == '=') {
                advance(l);
                tok->type = TOK_NEQ;
                tok->length = 2;
            } else {
                tok->type = TOK_EXCL;
            }
            return true;

        case '<':
            if (peek(l) == '<') {
                advance(l);
                if (peek(l) == '=') {
                    advance(l);
                    tok->type = TOK_LSHIFT_ASSIGN;
                    tok->length = 3;
                } else {
                    tok->type = TOK_LSHIFT;
                    tok->length = 2;
                }
            } else if (peek(l) == '=') {
                advance(l);
                tok->type = TOK_LE;
                tok->length = 2;
            } else {
                tok->type = TOK_LT;
            }
            return true;

        case '>':
            if (peek(l) == '>') {
                advance(l);
                if (peek(l) == '=') {
                    advance(l);
                    tok->type = TOK_RSHIFT_ASSIGN;
                    tok->length = 3;
                } else {
                    tok->type = TOK_RSHIFT;
                    tok->length = 2;
                }
            } else if (peek(l) == '=') {
                advance(l);
                tok->type = TOK_GE;
                tok->length = 2;
            } else {
                tok->type = TOK_GT;
            }
            return true;

        case '&':
            if (peek(l) == '&') {
                advance(l);
                tok->type = TOK_AND_AND;
                tok->length = 2;
            } else if (peek(l) == '=') {
                advance(l);
                tok->type = TOK_AMP_ASSIGN;
                tok->length = 2;
            } else {
                tok->type = TOK_AMP;
            }
            return true;

        case '|':
            if (peek(l) == '|') {
                advance(l);
                tok->type = TOK_PIPE_PIPE;
                tok->length = 2;
            } else if (peek(l) == '=') {
                advance(l);
                tok->type = TOK_PIPE_ASSIGN;
                tok->length = 2;
            } else {
                tok->type = TOK_PIPE;
            }
            return true;

        default:
            tok->type = TOK_ERROR;
            return false;
    }
}

const char *token_type_name(TokenType type) {
    switch (type) {
        case TOK_EOF: return "EOF";
        case TOK_ERROR: return "ERROR";
        case TOK_IDENT: return "IDENT";
        case TOK_INT_LIT: return "INT_LIT";
        case TOK_STRING_LIT: return "STRING_LIT";
        case TOK_INT: return "int";
        case TOK_CHAR: return "char";
        case TOK_VOID: return "void";
        case TOK_BOOL: return "_Bool";
        case TOK_SHORT: return "short";
        case TOK_LONG: return "long";
        case TOK_SIGNED: return "signed";
        case TOK_UNSIGNED: return "unsigned";
        case TOK_CONST: return "const";
        case TOK_VOLATILE: return "volatile";
        case TOK_RESTRICT: return "restrict";
        case TOK_INLINE: return "inline";
        case TOK_STRUCT: return "struct";
        case TOK_ENUM: return "enum";
        case TOK_TYPEDEF: return "typedef";
        case TOK_SIZEOF: return "sizeof";
        case TOK_RETURN: return "return";
        case TOK_IF: return "if";
        case TOK_ELSE: return "else";
        case TOK_WHILE: return "while";
        case TOK_FOR: return "for";
        case TOK_DO: return "do";
        case TOK_BREAK: return "break";
        case TOK_CONTINUE: return "continue";
        case TOK_SWITCH: return "switch";
        case TOK_CASE: return "case";
        case TOK_DEFAULT: return "default";
        case TOK_CLASS: return "class";
        case TOK_PUBLIC: return "public";
        case TOK_PRIVATE: return "private";
        case TOK_PROTECTED: return "protected";
        case TOK_THIS: return "this";
        case TOK_NAMESPACE: return "namespace";
        case TOK_OPERATOR: return "operator";
        case TOK_EXTERN: return "extern";
        case TOK_NEW: return "new";
        case TOK_DELETE: return "delete";
        case TOK_UNION: return "union";
        case TOK_FLOAT: return "float";
        case TOK_DOUBLE: return "double";
        case TOK_ATTRIBUTE: return "__attribute__";
        case TOK_DECLSPEC: return "__declspec";
        case TOK_EXTENSION: return "__extension__";
        case TOK_TYPEOF: return "typeof";
        case TOK_ASM: return "__asm__";
        case TOK_COLON_COLON: return "::";
        case TOK_PLUS: return "+";
        case TOK_MINUS: return "-";
        case TOK_STAR: return "*";
        case TOK_SLASH: return "/";
        case TOK_PERCENT: return "%";
        case TOK_ASSIGN: return "=";
        case TOK_EQ: return "==";
        case TOK_NEQ: return "!=";
        case TOK_LT: return "<";
        case TOK_GT: return ">";
        case TOK_LE: return "<=";
        case TOK_GE: return ">=";
        case TOK_AMP: return "&";
        case TOK_PIPE: return "|";
        case TOK_CARET: return "^";
        case TOK_TILDE: return "~";
        case TOK_EXCL: return "!";
        case TOK_LSHIFT: return "<<";
        case TOK_RSHIFT: return ">>";
        case TOK_ARROW: return "->";
        case TOK_DOT: return ".";
        case TOK_ELLIPSIS: return "...";
        case TOK_QUESTION: return "?";
        case TOK_COLON: return ":";
        case TOK_AND_AND: return "&&";
        case TOK_PIPE_PIPE: return "||";
        case TOK_PLUS_PLUS: return "++";
        case TOK_MINUS_MINUS: return "--";
        case TOK_PLUS_ASSIGN: return "+=";
        case TOK_MINUS_ASSIGN: return "-=";
        case TOK_STAR_ASSIGN: return "*=";
        case TOK_SLASH_ASSIGN: return "/=";
        case TOK_PERCENT_ASSIGN: return "%=";
        case TOK_AMP_ASSIGN: return "&=";
        case TOK_PIPE_ASSIGN: return "|=";
        case TOK_CARET_ASSIGN: return "^=";
        case TOK_LSHIFT_ASSIGN: return "<<=";
        case TOK_RSHIFT_ASSIGN: return ">>=";
        case TOK_LPAREN: return "(";
        case TOK_RPAREN: return ")";
        case TOK_LBRACE: return "{";
        case TOK_RBRACE: return "}";
        case TOK_LBRACKET: return "[";
        case TOK_RBRACKET: return "]";
        case TOK_SEMICOLON: return ";";
        case TOK_COMMA: return ",";
    }
    return "UNKNOWN";
}

bool token_ident_equals(const Token *tok, const char *str) {
    if (!tok || !str) return false;
    size_t len = strlen(str);
    return tok->length == len && memcmp(tok->start, str, len) == 0;
}

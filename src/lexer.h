#ifndef LEXER_H
#define LEXER_H

#include "arena.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

typedef enum {
    TOK_EOF = 0,
    TOK_ERROR,

    // Identifiers & Literals
    TOK_IDENT,
    TOK_INT_LIT,
    TOK_STRING_LIT,

    // Keywords
    TOK_INT,
    TOK_CHAR,
    TOK_VOID,
    TOK_BOOL,           // _Bool or bool
    TOK_SHORT,          // short
    TOK_LONG,           // long
    TOK_SIGNED,         // signed
    TOK_UNSIGNED,       // unsigned
    TOK_CONST,          // const
    TOK_VOLATILE,       // volatile
    TOK_RESTRICT,       // restrict
    TOK_INLINE,         // inline
    TOK_STRUCT,         // struct
    TOK_ENUM,           // enum
    TOK_TYPEDEF,        // typedef
    TOK_SIZEOF,         // sizeof
    TOK_RETURN,         // return
    TOK_IF,             // if
    TOK_ELSE,           // else
    TOK_WHILE,          // while
    TOK_FOR,            // for
    TOK_DO,             // do
    TOK_BREAK,          // break
    TOK_CONTINUE,       // continue
    TOK_SWITCH,         // switch
    TOK_CASE,           // case
    TOK_DEFAULT,        // default
    TOK_CLASS,          // class
    TOK_PUBLIC,         // public
    TOK_PRIVATE,        // private
    TOK_PROTECTED,      // protected
    TOK_THIS,           // this
    TOK_NAMESPACE,      // namespace
    TOK_OPERATOR,       // operator
    TOK_EXTERN,         // extern
    TOK_NEW,            // new
    TOK_DELETE,         // delete
    TOK_UNION,          // union
    TOK_FLOAT,          // float
    TOK_DOUBLE,         // double
    TOK_ATTRIBUTE,      // __attribute__ / __attribute
    TOK_DECLSPEC,       // __declspec
    TOK_EXTENSION,      // __extension__
    TOK_TYPEOF,         // typeof / __typeof__ / __typeof
    TOK_ASM,            // __asm__ / __asm / asm

    // Operators
    TOK_COLON_COLON,    // ::
    TOK_PLUS,           // +
    TOK_MINUS,          // -
    TOK_STAR,           // *
    TOK_SLASH,          // /
    TOK_PERCENT,        // %
    TOK_ASSIGN,         // =
    TOK_EQ,             // ==
    TOK_NEQ,            // !=
    TOK_LT,             // <
    TOK_GT,             // >
    TOK_LE,             // <=
    TOK_GE,             // >=
    TOK_AMP,            // &
    TOK_PIPE,           // |
    TOK_CARET,          // ^
    TOK_TILDE,          // ~
    TOK_EXCL,           // !
    TOK_LSHIFT,         // <<
    TOK_RSHIFT,         // >>
    TOK_ARROW,          // ->
    TOK_DOT,            // .
    TOK_ELLIPSIS,       // ...
    TOK_QUESTION,       // ?
    TOK_COLON,          // :

    // Logical & Increment / Decrement
    TOK_AND_AND,        // &&
    TOK_PIPE_PIPE,      // ||
    TOK_PLUS_PLUS,      // ++
    TOK_MINUS_MINUS,    // --

    // Compound Assignments
    TOK_PLUS_ASSIGN,    // +=
    TOK_MINUS_ASSIGN,   // -=
    TOK_STAR_ASSIGN,    // *=
    TOK_SLASH_ASSIGN,   // /=
    TOK_PERCENT_ASSIGN, // %=
    TOK_AMP_ASSIGN,     // &=
    TOK_PIPE_ASSIGN,    // |=
    TOK_CARET_ASSIGN,   // ^=
    TOK_LSHIFT_ASSIGN,  // <<=
    TOK_RSHIFT_ASSIGN,  // >>=

    // Punctuation
    TOK_LPAREN,         // (
    TOK_RPAREN,         // )
    TOK_LBRACE,         // {
    TOK_RBRACE,         // }
    TOK_LBRACKET,       // [
    TOK_RBRACKET,       // ]
    TOK_SEMICOLON,      // ;
    TOK_COMMA,          // ,
} TokenType;

typedef struct {
    TokenType type;
    const char *start;      // Pointer to token start in source string
    size_t length;          // Token character length in source
    int64_t int_val;        // Parsed integer value (for TOK_INT_LIT)
    const char *string_val; // Parsed string value without quotes (for TOK_STRING_LIT)
    size_t string_len;      // Parsed string byte length (excluding null terminator)
    uint32_t line;          // 1-based source line
    uint32_t col;           // 1-based source column
} Token;

typedef struct Macro {
    const char *name;
    size_t name_len;
    const char *replacement;
    size_t repl_len;
    struct Macro *next;
} Macro;

typedef struct {
    const char *src;
    size_t pos;
    uint32_t line;
    uint32_t col;
    const char *filename;
} SourceBuffer;

#define MAX_INCLUDE_DEPTH 8
#define MAX_COND_DEPTH 32

typedef struct {
    bool active;
    bool taken;
} CondFrame;

typedef struct {
    SourceBuffer stack[MAX_INCLUDE_DEPTH];
    int stack_ptr;
    Arena *arena;
    Macro *macros;
    const char **include_paths;
    size_t include_path_count;
    size_t include_path_capacity;
    CondFrame cond_stack[MAX_COND_DEPTH];
    int cond_depth;
} Lexer;

/**
 * Initialize a lexer for a null-terminated source string with an optional arena.
 */
void lexer_init(Lexer *l, const char *source, Arena *arena);

/**
 * Add an include directory path to the lexer search path.
 */
void lexer_add_include_path(Lexer *l, const char *path);

/**
 * Define an object-like preprocessor macro: #define NAME replacement
 */
void lexer_define_macro(Lexer *l, const char *name, size_t name_len, const char *val, size_t val_len);

/**
 * Undefine a macro: #undef NAME
 */
void lexer_undef_macro(Lexer *l, const char *name, size_t name_len);

/**
 * Lookup a defined macro by name.
 */
Macro *lexer_lookup_macro(const Lexer *l, const char *name, size_t name_len);

/**
 * Fetch the next token from the stream (expanding macros and includes transparently).
 */
bool lexer_next_token(Lexer *l, Token *tok);

/**
 * Return a human-readable name for a given token type.
 */
const char *token_type_name(TokenType type);

/**
 * Helper to check if a token identifier matches a C string.
 */
bool token_ident_equals(const Token *tok, const char *str);

#endif // LEXER_H

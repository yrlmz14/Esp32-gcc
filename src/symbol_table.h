#ifndef SYMBOL_TABLE_H
#define SYMBOL_TABLE_H

#include "arena.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

typedef enum {
    BASE_TYPE_INT = 0,
    BASE_TYPE_CHAR,
    BASE_TYPE_VOID,
    BASE_TYPE_STRUCT,
    BASE_TYPE_UNION,
    BASE_TYPE_BOOL,
    BASE_TYPE_SHORT,
    BASE_TYPE_LONG,
    BASE_TYPE_FLOAT,
    BASE_TYPE_DOUBLE,
    BASE_TYPE_ENUM,
    BASE_TYPE_FUNCTION_POINTER,
} BaseType;

struct StructDef;

typedef struct {
    BaseType base;
    int pointer_depth; // 0 = value, 1 = ptr, 2 = ptr-to-ptr
    int array_size;    // 0 = scalar / pointer, >0 = fixed-size array
    bool is_volatile;
    bool is_const;
    bool is_unsigned;
    bool is_reference; // C++ reference (Type &)
    bool is_class;     // true if defined with 'class'
    bool is_func_ptr;  // true if function pointer
    struct StructDef *struct_def; // valid when base == BASE_TYPE_STRUCT or BASE_TYPE_UNION
} DataType;

typedef struct StructMember {
    const char *name;
    size_t name_len;
    DataType type;
    int offset;        // Byte offset from base of struct
    int bit_offset;    // Bit offset within word (0..31) for bitfields
    int bit_width;     // Width in bits (0 if normal member, >0 if bitfield)
    bool is_bitfield;
    bool is_public;
} StructMember;

typedef struct MethodDef {
    const char *name;
    size_t name_len;
    const char *mangled_name;
    DataType return_type;
    DataType *param_types;
    const char **param_names;
    size_t param_count;
    bool is_constructor;
    bool is_destructor;
    bool is_const;
    bool is_public;
} MethodDef;

typedef struct StructDef {
    const char *name;
    size_t name_len;
    bool is_class;
    bool is_union;
    int current_bit_offset;
    StructMember *members;
    size_t member_count;
    size_t member_capacity;
    MethodDef *methods;
    size_t method_count;
    size_t method_capacity;
    int total_size;    // Total size in bytes (padded to alignment)
    int alignment;     // Struct alignment (typically 4 bytes)
    struct StructDef *next;
} StructDef;

typedef struct TypedefEntry {
    const char *name;
    size_t name_len;
    DataType type;
    struct TypedefEntry *next;
} TypedefEntry;

typedef struct EnumConstant {
    const char *name;
    size_t name_len;
    int32_t val;
    struct EnumConstant *next;
} EnumConstant;

static inline DataType make_type(BaseType base, int ptr_depth, bool is_vol) {
    DataType t;
    t.base = base;
    t.pointer_depth = ptr_depth;
    t.array_size = 0;
    t.is_volatile = is_vol;
    t.is_const = false;
    t.is_unsigned = false;
    t.struct_def = NULL;
    return t;
}

static inline DataType make_struct_type(StructDef *s, int ptr_depth, bool is_vol) {
    DataType t;
    t.base = BASE_TYPE_STRUCT;
    t.pointer_depth = ptr_depth;
    t.array_size = 0;
    t.is_volatile = is_vol;
    t.is_const = false;
    t.is_unsigned = false;
    t.struct_def = s;
    return t;
}

static inline DataType make_array_type(BaseType base, int array_size, bool is_vol) {
    DataType t;
    t.base = base;
    t.pointer_depth = 0;
    t.array_size = array_size;
    t.is_volatile = is_vol;
    t.is_const = false;
    t.is_unsigned = false;
    t.struct_def = NULL;
    return t;
}

static inline DataType type_int(void) {
    return make_type(BASE_TYPE_INT, 0, false);
}

static inline DataType type_char(void) {
    return make_type(BASE_TYPE_CHAR, 0, false);
}

static inline DataType type_void(void) {
    return make_type(BASE_TYPE_VOID, 0, false);
}

static inline DataType type_bool(void) {
    return make_type(BASE_TYPE_BOOL, 0, false);
}

static inline DataType type_short(void) {
    return make_type(BASE_TYPE_SHORT, 0, false);
}

static inline DataType type_long(void) {
    return make_type(BASE_TYPE_LONG, 0, false);
}

static inline DataType type_ptr_to(DataType t) {
    t.pointer_depth++;
    return t;
}

static inline DataType type_deref(DataType t) {
    if (t.array_size > 0) {
        t.array_size = 0;
        return t;
    }
    if (t.pointer_depth > 0) {
        t.pointer_depth--;
    }
    return t;
}

static inline DataType make_reference_type(DataType t) {
    t.is_reference = true;
    return t;
}

int type_size(DataType t);
int type_align(DataType t);

typedef struct Symbol {
    const char *name;
    size_t name_len;
    DataType type;
    int stack_offset;        // Offset relative to SP (a1)
    int scope_level;         // Scope nesting level (0 = global/func params, 1+ = blocks)
    struct Symbol *next;     // Linked list pointer
} Symbol;

typedef struct {
    Arena *arena;
    Symbol *head;
    StructDef *structs;
    TypedefEntry *typedefs;
    EnumConstant *enum_constants;
    int current_scope;
    int current_stack_offset; // Running stack size needed for locals in function
} SymbolTable;

void symtab_init(SymbolTable *st, Arena *arena);
void symtab_enter_scope(SymbolTable *st);
void symtab_exit_scope(SymbolTable *st);
Symbol *symtab_add(SymbolTable *st, const char *name, size_t name_len, DataType type, int stack_offset);
Symbol *symtab_lookup(const SymbolTable *st, const char *name, size_t name_len);
Symbol *symtab_alloc_local(SymbolTable *st, const char *name, size_t name_len, DataType type);

StructDef *symtab_define_struct(SymbolTable *st, const char *name, size_t name_len);
StructDef *symtab_lookup_struct(const SymbolTable *st, const char *name, size_t name_len);
bool struct_add_member(StructDef *s, Arena *arena, const char *name, size_t name_len, DataType type);
bool struct_add_bitfield(StructDef *s, Arena *arena, const char *name, size_t name_len, DataType type, int bit_width);
void struct_finalize_layout(StructDef *s);
const StructMember *struct_lookup_member(const StructDef *s, const char *name, size_t name_len);

bool struct_add_method(StructDef *s, Arena *arena, const MethodDef *m);
const MethodDef *struct_lookup_method(const StructDef *s, const char *name, size_t name_len);
char *mangle_method_name(Arena *arena, const char *class_name, const char *method_name);
char *mangle_function_name(Arena *arena, const char *func_name);

bool symtab_define_typedef(SymbolTable *st, const char *name, size_t name_len, DataType type);
const DataType *symtab_lookup_typedef(const SymbolTable *st, const char *name, size_t name_len);

bool symtab_define_enum_constant(SymbolTable *st, const char *name, size_t name_len, int32_t val);
const EnumConstant *symtab_lookup_enum_constant(const SymbolTable *st, const char *name, size_t name_len);

#endif // SYMBOL_TABLE_H

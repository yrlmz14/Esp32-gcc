#include "symbol_table.h"
#include <string.h>
#include <stdio.h>

int type_size(DataType t) {
    if (t.pointer_depth > 0 || t.is_func_ptr) return 4;
    if (t.array_size > 0) {
        DataType elem = t;
        elem.array_size = 0;
        return t.array_size * type_size(elem);
    }
    switch (t.base) {
        case BASE_TYPE_BOOL: return 1;
        case BASE_TYPE_CHAR: return 1;
        case BASE_TYPE_SHORT: return 2;
        case BASE_TYPE_INT: return 4;
        case BASE_TYPE_LONG: return 4;
        case BASE_TYPE_FLOAT: return 4;
        case BASE_TYPE_DOUBLE: return 8;
        case BASE_TYPE_ENUM: return 4;
        case BASE_TYPE_FUNCTION_POINTER: return 4;
        case BASE_TYPE_VOID: return 1;
        case BASE_TYPE_STRUCT:
        case BASE_TYPE_UNION:
            return t.struct_def ? t.struct_def->total_size : 4;
    }
    return 4;
}

int type_align(DataType t) {
    if (t.pointer_depth > 0 || t.is_func_ptr) return 4;
    if (t.array_size > 0) {
        DataType elem = t;
        elem.array_size = 0;
        return type_align(elem);
    }
    switch (t.base) {
        case BASE_TYPE_BOOL: return 1;
        case BASE_TYPE_CHAR: return 1;
        case BASE_TYPE_SHORT: return 2;
        case BASE_TYPE_INT: return 4;
        case BASE_TYPE_LONG: return 4;
        case BASE_TYPE_FLOAT: return 4;
        case BASE_TYPE_DOUBLE: return 4;
        case BASE_TYPE_ENUM: return 4;
        case BASE_TYPE_FUNCTION_POINTER: return 4;
        case BASE_TYPE_VOID: return 1;
        case BASE_TYPE_STRUCT:
        case BASE_TYPE_UNION:
            return t.struct_def ? t.struct_def->alignment : 4;
    }
    return 4;
}

void symtab_init(SymbolTable *st, Arena *arena) {
    if (!st) return;
    st->arena = arena;
    st->head = NULL;
    st->structs = NULL;
    st->typedefs = NULL;
    st->enum_constants = NULL;
    st->current_scope = 0;
    st->current_stack_offset = 0;
}

void symtab_enter_scope(SymbolTable *st) {
    if (st) {
        st->current_scope++;
    }
}

void symtab_exit_scope(SymbolTable *st) {
    if (!st || st->current_scope == 0) return;
    int target_scope = st->current_scope;
    st->current_scope--;

    // Pop all symbols that belong to target_scope
    while (st->head && st->head->scope_level == target_scope) {
        st->head = st->head->next;
    }
}

Symbol *symtab_add(SymbolTable *st, const char *name, size_t name_len, DataType type, int stack_offset) {
    if (!st || !st->arena || !name) return NULL;

    Symbol *sym = (Symbol *)arena_alloc(st->arena, sizeof(Symbol));
    if (!sym) return NULL;

    sym->name = arena_strndup(st->arena, name, name_len);
    sym->name_len = name_len;
    sym->type = type;
    sym->stack_offset = stack_offset;
    sym->scope_level = st->current_scope;
    sym->next = st->head;
    st->head = sym;

    return sym;
}

Symbol *symtab_lookup(const SymbolTable *st, const char *name, size_t name_len) {
    if (!st || !name) return NULL;

    Symbol *curr = st->head;
    while (curr) {
        if (curr->name_len == name_len && memcmp(curr->name, name, name_len) == 0) {
            return curr;
        }
        curr = curr->next;
    }
    return NULL;
}

Symbol *symtab_alloc_local(SymbolTable *st, const char *name, size_t name_len, DataType type) {
    if (!st) return NULL;
    int offset = st->current_stack_offset;
    int size = type_size(type);
    size = (size + 3) & ~3; // 4-byte aligned stack allocation
    st->current_stack_offset += size;
    return symtab_add(st, name, name_len, type, offset);
}

StructDef *symtab_define_struct(SymbolTable *st, const char *name, size_t name_len) {
    if (!st || !st->arena || !name) return NULL;

    // Check if struct already defined
    StructDef *existing = symtab_lookup_struct(st, name, name_len);
    if (existing) return existing;

    StructDef *s = (StructDef *)arena_alloc(st->arena, sizeof(StructDef));
    if (!s) return NULL;

    s->name = arena_strndup(st->arena, name, name_len);
    s->name_len = name_len;
    s->members = NULL;
    s->member_count = 0;
    s->member_capacity = 0;
    s->total_size = 0;
    s->alignment = 4;
    s->next = st->structs;
    st->structs = s;

    return s;
}

StructDef *symtab_lookup_struct(const SymbolTable *st, const char *name, size_t name_len) {
    if (!st || !name) return NULL;

    StructDef *curr = st->structs;
    while (curr) {
        if (curr->name_len == name_len && memcmp(curr->name, name, name_len) == 0) {
            return curr;
        }
        curr = curr->next;
    }
    return NULL;
}

bool struct_add_member(StructDef *s, Arena *arena, const char *name, size_t name_len, DataType type) {
    if (!s || !arena || !name) return false;

    if (s->member_count >= s->member_capacity) {
        size_t new_cap = s->member_capacity == 0 ? 4 : s->member_capacity * 2;
        StructMember *new_arr = (StructMember *)arena_alloc(arena, new_cap * sizeof(StructMember));
        if (!new_arr) return false;
        if (s->members && s->member_count > 0) {
            memcpy(new_arr, s->members, s->member_count * sizeof(StructMember));
        }
        s->members = new_arr;
        s->member_capacity = new_cap;
    }

    int sz = type_size(type);
    int al = type_align(type);
    if (al > s->alignment) s->alignment = al;

    if (s->is_union) {
        StructMember *m = &s->members[s->member_count++];
        m->name = arena_strndup(arena, name, name_len);
        m->name_len = name_len;
        m->type = type;
        m->offset = 0;
        m->bit_offset = 0;
        m->bit_width = 0;
        m->is_bitfield = false;
        m->is_public = true;
        if (sz > s->total_size) s->total_size = sz;
        return true;
    }

    // Flush any pending bitfield bits to a 4-byte word boundary
    if (s->current_bit_offset > 0) {
        s->total_size += 4;
        s->current_bit_offset = 0;
    }

    // Align member offset
    s->total_size = (s->total_size + al - 1) & ~(al - 1);

    StructMember *m = &s->members[s->member_count++];
    m->name = arena_strndup(arena, name, name_len);
    m->name_len = name_len;
    m->type = type;
    m->offset = s->total_size;
    m->bit_offset = 0;
    m->bit_width = 0;
    m->is_bitfield = false;
    m->is_public = true;

    s->total_size += sz;
    return true;
}

bool struct_add_bitfield(StructDef *s, Arena *arena, const char *name, size_t name_len, DataType type, int bit_width) {
    if (!s || !arena || !name) return false;
    if (bit_width <= 0) bit_width = 1;
    if (bit_width > 32) bit_width = 32;

    if (s->member_count >= s->member_capacity) {
        size_t new_cap = s->member_capacity == 0 ? 4 : s->member_capacity * 2;
        StructMember *new_arr = (StructMember *)arena_alloc(arena, new_cap * sizeof(StructMember));
        if (!new_arr) return false;
        if (s->members && s->member_count > 0) {
            memcpy(new_arr, s->members, s->member_count * sizeof(StructMember));
        }
        s->members = new_arr;
        s->member_capacity = new_cap;
    }

    if (s->alignment < 4) s->alignment = 4;

    // Check if bitfield fits into the current word
    if (s->current_bit_offset + bit_width > 32) {
        s->total_size += 4;
        s->current_bit_offset = 0;
    }

    // Ensure 4-byte alignment
    s->total_size = (s->total_size + 3) & ~3;

    StructMember *m = &s->members[s->member_count++];
    m->name = arena_strndup(arena, name, name_len);
    m->name_len = name_len;
    m->type = type;
    m->offset = s->total_size;
    m->bit_offset = s->current_bit_offset;
    m->bit_width = bit_width;
    m->is_bitfield = true;
    m->is_public = true;

    s->current_bit_offset += bit_width;
    if (s->current_bit_offset == 32) {
        s->total_size += 4;
        s->current_bit_offset = 0;
    }
    return true;
}

void struct_finalize_layout(StructDef *s) {
    if (!s) return;
    if (s->current_bit_offset > 0) {
        s->total_size += 4;
        s->current_bit_offset = 0;
    }
    if (s->alignment > 0) {
        s->total_size = (s->total_size + s->alignment - 1) & ~(s->alignment - 1);
    }
    if (s->total_size == 0) {
        s->total_size = 4; // Minimum 4 bytes
    }
}

const StructMember *struct_lookup_member(const StructDef *s, const char *name, size_t name_len) {
    if (!s || !name) return NULL;

    for (size_t i = 0; i < s->member_count; i++) {
        if (s->members[i].name_len == name_len &&
            memcmp(s->members[i].name, name, name_len) == 0) {
            return &s->members[i];
        }
    }
    return NULL;
}

bool symtab_define_typedef(SymbolTable *st, const char *name, size_t name_len, DataType type) {
    if (!st || !st->arena || !name) return false;

    TypedefEntry *e = (TypedefEntry *)arena_alloc(st->arena, sizeof(TypedefEntry));
    if (!e) return false;

    e->name = arena_strndup(st->arena, name, name_len);
    e->name_len = name_len;
    e->type = type;
    e->next = st->typedefs;
    st->typedefs = e;
    return true;
}

const DataType *symtab_lookup_typedef(const SymbolTable *st, const char *name, size_t name_len) {
    if (!st || !name) return NULL;

    for (const TypedefEntry *e = st->typedefs; e != NULL; e = e->next) {
        if (e->name_len == name_len && memcmp(e->name, name, name_len) == 0) {
            return &e->type;
        }
    }
    return NULL;
}

bool symtab_define_enum_constant(SymbolTable *st, const char *name, size_t name_len, int32_t val) {
    if (!st || !st->arena || !name) return false;

    EnumConstant *c = (EnumConstant *)arena_alloc(st->arena, sizeof(EnumConstant));
    if (!c) return false;

    c->name = arena_strndup(st->arena, name, name_len);
    c->name_len = name_len;
    c->val = val;
    c->next = st->enum_constants;
    st->enum_constants = c;
    return true;
}

const EnumConstant *symtab_lookup_enum_constant(const SymbolTable *st, const char *name, size_t name_len) {
    if (!st || !name) return NULL;

    for (const EnumConstant *c = st->enum_constants; c != NULL; c = c->next) {
        if (c->name_len == name_len && memcmp(c->name, name, name_len) == 0) {
            return c;
        }
    }
    return NULL;
}

bool struct_add_method(StructDef *s, Arena *arena, const MethodDef *m) {
    if (!s || !arena || !m) return false;
    if (s->method_count >= s->method_capacity) {
        size_t new_cap = s->method_capacity == 0 ? 4 : s->method_capacity * 2;
        MethodDef *new_methods = (MethodDef *)arena_alloc(arena, new_cap * sizeof(MethodDef));
        if (!new_methods) return false;
        if (s->methods && s->method_count > 0) {
            memcpy(new_methods, s->methods, s->method_count * sizeof(MethodDef));
        }
        s->methods = new_methods;
        s->method_capacity = new_cap;
    }
    s->methods[s->method_count++] = *m;
    return true;
}

const MethodDef *struct_lookup_method(const StructDef *s, const char *name, size_t name_len) {
    if (!s || !name) return NULL;
    for (size_t i = 0; i < s->method_count; i++) {
        if (s->methods[i].name_len == name_len &&
            memcmp(s->methods[i].name, name, name_len) == 0) {
            return &s->methods[i];
        }
    }
    return NULL;
}

char *mangle_method_name(Arena *arena, const char *class_name, const char *method_name) {
    if (!arena || !class_name || !method_name) return "";
    size_t clen = strlen(class_name);
    size_t mlen = strlen(method_name);
    char buf[256];
    snprintf(buf, sizeof(buf), "_ZN%u%s%u%sE", (unsigned)clen, class_name, (unsigned)mlen, method_name);
    return arena_strndup(arena, buf, strlen(buf));
}

char *mangle_function_name(Arena *arena, const char *func_name) {
    if (!arena || !func_name) return "";
    return arena_strndup(arena, func_name, strlen(func_name));
}

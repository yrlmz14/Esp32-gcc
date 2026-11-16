#include "xtensa_emitter.h"
#include <string.h>

// ============================================================================
// Internal Helpers
// ============================================================================

static bool ensure_code_capacity(Emitter *e, size_t needed) {
    if (e->code_size + needed <= e->code_capacity) {
        return true;
    }
    size_t new_cap = e->code_capacity * 2;
    if (new_cap < e->code_size + needed) {
        new_cap = e->code_size + needed + 128;
    }
    uint8_t *new_buf = (uint8_t *)arena_alloc(e->arena, new_cap);
    if (!new_buf) {
        return false;
    }
    if (e->code_size > 0) {
        memcpy(new_buf, e->code, e->code_size);
    }
    e->code = new_buf;
    e->code_capacity = new_cap;
    return true;
}

static bool ensure_literal_capacity(Emitter *e, size_t needed) {
    if (e->literal_count + needed <= e->literal_capacity) {
        return true;
    }
    size_t new_cap = e->literal_capacity * 2;
    if (new_cap < e->literal_count + needed) {
        new_cap = e->literal_count + needed + 16;
    }
    int32_t *new_lits = (int32_t *)arena_alloc(e->arena, new_cap * sizeof(int32_t));
    if (!new_lits) {
        return false;
    }
    if (e->literal_count > 0) {
        memcpy(new_lits, e->literals, e->literal_count * sizeof(int32_t));
    }
    e->literals = new_lits;
    e->literal_capacity = new_cap;
    return true;
}

static bool ensure_fixup_capacity(Emitter *e, size_t needed) {
    if (e->fixup_count + needed <= e->fixup_capacity) {
        return true;
    }
    size_t new_cap = e->fixup_capacity * 2;
    if (new_cap < e->fixup_count + needed) {
        new_cap = e->fixup_count + needed + 16;
    }
    LiteralFixup *new_fixups = (LiteralFixup *)arena_alloc(e->arena, new_cap * sizeof(LiteralFixup));
    if (!new_fixups) {
        return false;
    }
    if (e->fixup_count > 0) {
        memcpy(new_fixups, e->fixups, e->fixup_count * sizeof(LiteralFixup));
    }
    e->fixups = new_fixups;
    e->fixup_capacity = new_cap;
    return true;
}

static bool ensure_string_capacity(Emitter *e, size_t needed) {
    if (e->string_count + needed <= e->string_capacity) {
        return true;
    }
    size_t new_cap = e->string_capacity * 2;
    if (new_cap < e->string_count + needed) {
        new_cap = e->string_count + needed + 8;
    }
    StringLiteral *new_strs = (StringLiteral *)arena_alloc(e->arena, new_cap * sizeof(StringLiteral));
    if (!new_strs) return false;
    if (e->string_count > 0) {
        memcpy(new_strs, e->strings, e->string_count * sizeof(StringLiteral));
    }
    e->strings = new_strs;
    e->string_capacity = new_cap;
    return true;
}

static bool ensure_str_fixup_capacity(Emitter *e, size_t needed) {
    if (e->str_fixup_count + needed <= e->str_fixup_capacity) {
        return true;
    }
    size_t new_cap = e->str_fixup_capacity * 2;
    if (new_cap < e->str_fixup_count + needed) {
        new_cap = e->str_fixup_count + needed + 8;
    }
    StringFixup *new_fixups = (StringFixup *)arena_alloc(e->arena, new_cap * sizeof(StringFixup));
    if (!new_fixups) return false;
    if (e->str_fixup_count > 0) {
        memcpy(new_fixups, e->str_fixups, e->str_fixup_count * sizeof(StringFixup));
    }
    e->str_fixups = new_fixups;
    e->str_fixup_capacity = new_cap;
    return true;
}

static bool ensure_reloc_capacity(Emitter *e, size_t needed) {
    if (e->reloc_count + needed <= e->reloc_capacity) {
        return true;
    }
    size_t new_cap = e->reloc_capacity * 2;
    if (new_cap < e->reloc_count + needed) {
        new_cap = e->reloc_count + needed + 8;
    }
    RelocEntry *new_relocs = (RelocEntry *)arena_alloc(e->arena, new_cap * sizeof(RelocEntry));
    if (!new_relocs) return false;
    if (e->reloc_count > 0) {
        memcpy(new_relocs, e->relocs, e->reloc_count * sizeof(RelocEntry));
    }
    e->relocs = new_relocs;
    e->reloc_capacity = new_cap;
    return true;
}

void emit_add_reloc(Emitter *e, size_t inst_offset, const char *sym_name, size_t sym_len, uint32_t type, int32_t addend) {
    if (!ensure_reloc_capacity(e, 1)) return;
    RelocEntry *r = &e->relocs[e->reloc_count++];
    r->inst_offset = inst_offset;
    r->sym_name = arena_strndup(e->arena, sym_name, sym_len);
    r->sym_name_len = sym_len;
    r->type = type;
    r->addend = addend;
}

static inline void emit_byte(Emitter *e, uint8_t byte) {
    if (ensure_code_capacity(e, 1)) {
        e->code[e->code_size++] = byte;
    }
}

static inline void emit_u16_le(Emitter *e, uint16_t val) {
    if (ensure_code_capacity(e, 2)) {
        e->code[e->code_size++] = (uint8_t)(val & 0xFF);
        e->code[e->code_size++] = (uint8_t)((val >> 8) & 0xFF);
    }
}

static inline void emit_u24_le(Emitter *e, uint32_t val) {
    if (ensure_code_capacity(e, 3)) {
        e->code[e->code_size++] = (uint8_t)(val & 0xFF);
        e->code[e->code_size++] = (uint8_t)((val >> 8) & 0xFF);
        e->code[e->code_size++] = (uint8_t)((val >> 16) & 0xFF);
    }
}

// ============================================================================
// Lifecycle Management
// ============================================================================

bool emit_init(Emitter *e, Arena *arena, size_t initial_capacity) {
    if (!e || !arena) {
        return false;
    }
    if (initial_capacity < 64) {
        initial_capacity = 64;
    }
    e->arena = arena;
    e->code = (uint8_t *)arena_alloc(arena, initial_capacity);
    if (!e->code) {
        return false;
    }
    e->code_size = 0;
    e->code_capacity = initial_capacity;

    e->literal_capacity = 4;
    e->literals = (int32_t *)arena_alloc(arena, e->literal_capacity * sizeof(int32_t));
    e->literal_count = 0;

    e->fixup_capacity = 4;
    e->fixups = (LiteralFixup *)arena_alloc(arena, e->fixup_capacity * sizeof(LiteralFixup));
    e->fixup_count = 0;

    e->string_capacity = 2;
    e->strings = (StringLiteral *)arena_alloc(arena, e->string_capacity * sizeof(StringLiteral));
    e->string_count = 0;
    e->string_bytes_total = 0;

    e->str_fixup_capacity = 2;
    e->str_fixups = (StringFixup *)arena_alloc(arena, e->str_fixup_capacity * sizeof(StringFixup));
    e->str_fixup_count = 0;

    e->reloc_capacity = 2;
    e->relocs = (RelocEntry *)arena_alloc(arena, e->reloc_capacity * sizeof(RelocEntry));
    e->reloc_count = 0;

    return (e->literals != NULL && e->fixups != NULL && e->strings != NULL && e->str_fixups != NULL && e->relocs != NULL);
}

void emit_reset(Emitter *e) {
    if (e) {
        e->code_size = 0;
        e->literal_count = 0;
        e->fixup_count = 0;
        e->string_count = 0;
        e->string_bytes_total = 0;
        e->str_fixup_count = 0;
        e->reloc_count = 0;
    }
}

// ============================================================================
// 16-Bit Density Instructions
// ============================================================================

void emit_ret_n(Emitter *e) {
    emit_u16_le(e, 0xF00D);
}

void emit_nop_n(Emitter *e) {
    emit_u16_le(e, 0xF03D);
}

void emit_mov_n(Emitter *e, Reg dst, Reg src) {
    uint16_t inst = 0x000D | ((dst & 0xF) << 4) | ((src & 0xF) << 8);
    emit_u16_le(e, inst);
}

void emit_add_n(Emitter *e, Reg dst, Reg s1, Reg s2) {
    uint16_t inst = 0x000A | ((s2 & 0xF) << 4) | ((s1 & 0xF) << 8) | ((dst & 0xF) << 12);
    emit_u16_le(e, inst);
}

void emit_addi_n(Emitter *e, Reg dst, Reg src, int imm) {
    uint8_t simm4 = (imm == -1) ? 0 : (uint8_t)(imm & 0xF);
    uint16_t inst = 0x000B | ((simm4 & 0xF) << 4) | ((src & 0xF) << 8) | ((dst & 0xF) << 12);
    emit_u16_le(e, inst);
}

void emit_movi_n(Emitter *e, Reg dst, int imm) {
    uint8_t uimm7 = (uint8_t)(imm & 0x7F);
    uint8_t imm7_lo = uimm7 & 0x0F;
    uint8_t imm7_hi = (uimm7 >> 4) & 0x07;
    uint16_t inst = 0x000C | (imm7_hi << 4) | ((dst & 0xF) << 8) | (imm7_lo << 12);
    emit_u16_le(e, inst);
}

void emit_l32i_n(Emitter *e, Reg dst, Reg base, uint32_t offset_bytes) {
    uint8_t imm4_offset_words = (uint8_t)((offset_bytes / 4) & 0xF);
    uint16_t inst = 0x0008 | ((dst & 0xF) << 4) | ((base & 0xF) << 8) | ((imm4_offset_words & 0xF) << 12);
    emit_u16_le(e, inst);
}

void emit_s32i_n(Emitter *e, Reg src, Reg base, uint32_t offset_bytes) {
    uint8_t imm4_offset_words = (uint8_t)((offset_bytes / 4) & 0xF);
    uint16_t inst = 0x0009 | ((src & 0xF) << 4) | ((base & 0xF) << 8) | ((imm4_offset_words & 0xF) << 12);
    emit_u16_le(e, inst);
}

// ============================================================================
// 24-Bit Standard Format Instructions
// ============================================================================

void emit_ret(Emitter *e) {
    emit_u24_le(e, 0x000080);
}

void emit_nop(Emitter *e) {
    emit_u24_le(e, 0x0020F0);
}

void emit_add(Emitter *e, Reg dst, Reg s1, Reg s2) {
    uint32_t inst = 0x800000 | ((s2 & 0xF) << 4) | ((s1 & 0xF) << 8) | ((dst & 0xF) << 12);
    emit_u24_le(e, inst);
}

void emit_sub(Emitter *e, Reg dst, Reg s1, Reg s2) {
    uint32_t inst = 0xC00000 | ((s2 & 0xF) << 4) | ((s1 & 0xF) << 8) | ((dst & 0xF) << 12);
    emit_u24_le(e, inst);
}

void emit_mull(Emitter *e, Reg dst, Reg s1, Reg s2) {
    uint32_t inst = 0x820000 | ((s2 & 0xF) << 4) | ((s1 & 0xF) << 8) | ((dst & 0xF) << 12);
    emit_u24_le(e, inst);
}

void emit_quos(Emitter *e, Reg dst, Reg s1, Reg s2) {
    uint32_t inst = 0xD20000 | ((s2 & 0xF) << 4) | ((s1 & 0xF) << 8) | ((dst & 0xF) << 12);
    emit_u24_le(e, inst);
}

void emit_rems(Emitter *e, Reg dst, Reg s1, Reg s2) {
    uint32_t inst = 0xF20000 | ((s2 & 0xF) << 4) | ((s1 & 0xF) << 8) | ((dst & 0xF) << 12);
    emit_u24_le(e, inst);
}

void emit_and(Emitter *e, Reg dst, Reg s1, Reg s2) {
    uint32_t inst = 0x100000 | ((s2 & 0xF) << 4) | ((s1 & 0xF) << 8) | ((dst & 0xF) << 12);
    emit_u24_le(e, inst);
}

void emit_or(Emitter *e, Reg dst, Reg s1, Reg s2) {
    uint32_t inst = 0x200000 | ((s2 & 0xF) << 4) | ((s1 & 0xF) << 8) | ((dst & 0xF) << 12);
    emit_u24_le(e, inst);
}

void emit_xor(Emitter *e, Reg dst, Reg s1, Reg s2) {
    uint32_t inst = 0x300000 | ((s2 & 0xF) << 4) | ((s1 & 0xF) << 8) | ((dst & 0xF) << 12);
    emit_u24_le(e, inst);
}

void emit_neg(Emitter *e, Reg dst, Reg src) {
    uint32_t inst = 0x600000 | ((src & 0xF) << 4) | ((dst & 0xF) << 12);
    emit_u24_le(e, inst);
}

void emit_addi(Emitter *e, Reg dst, Reg src, int8_t imm8) {
    uint32_t inst = 0x00C002 | ((dst & 0xF) << 4) | ((src & 0xF) << 8) | (((uint32_t)(uint8_t)imm8) << 16);
    emit_u24_le(e, inst);
}

void emit_movi(Emitter *e, Reg dst, int32_t imm12) {
    uint32_t uimm = (uint32_t)(imm12 & 0xFFF);
    uint32_t imm_lo = uimm & 0xFF;
    uint32_t imm_hi = (uimm >> 8) & 0x0F;
    uint32_t inst = 0x00A002 | ((dst & 0xF) << 4) | (imm_hi << 8) | (imm_lo << 16);
    emit_u24_le(e, inst);
}

void emit_l32i(Emitter *e, Reg dst, Reg base, uint32_t offset_bytes) {
    uint32_t word_offset = (offset_bytes / 4) & 0xFF;
    uint32_t inst = 0x002002 | ((dst & 0xF) << 4) | ((base & 0xF) << 8) | (word_offset << 16);
    emit_u24_le(e, inst);
}

void emit_s32i(Emitter *e, Reg src, Reg base, uint32_t offset_bytes) {
    uint32_t word_offset = (offset_bytes / 4) & 0xFF;
    uint32_t inst = 0x006002 | ((src & 0xF) << 4) | ((base & 0xF) << 8) | (word_offset << 16);
    emit_u24_le(e, inst);
}

void emit_l8ui(Emitter *e, Reg dst, Reg base, uint32_t offset_bytes) {
    uint32_t inst = 0x000002 | ((dst & 0xF) << 4) | ((base & 0xF) << 8) | (((uint32_t)(offset_bytes & 0xFF)) << 16);
    emit_u24_le(e, inst);
}

void emit_s8i(Emitter *e, Reg src, Reg base, uint32_t offset_bytes) {
    uint32_t inst = 0x004002 | ((src & 0xF) << 4) | ((base & 0xF) << 8) | (((uint32_t)(offset_bytes & 0xFF)) << 16);
    emit_u24_le(e, inst);
}

void emit_l32r(Emitter *e, Reg dst, int32_t word_offset_neg) {
    uint32_t uimm16 = (uint32_t)(word_offset_neg & 0xFFFF);
    uint32_t inst = 0x000001 | ((dst & 0xF) << 4) | (uimm16 << 8);
    emit_u24_le(e, inst);
}

void emit_ssl(Emitter *e, Reg src) {
    uint32_t inst = 0x401000 | ((src & 0xF) << 8);
    emit_u24_le(e, inst);
}

void emit_sll(Emitter *e, Reg dst, Reg src) {
    uint32_t inst = 0xA10000 | ((src & 0xF) << 8) | ((dst & 0xF) << 12);
    emit_u24_le(e, inst);
}

void emit_ssr(Emitter *e, Reg src) {
    uint32_t inst = 0x400000 | ((src & 0xF) << 8);
    emit_u24_le(e, inst);
}

void emit_sra(Emitter *e, Reg dst, Reg src) {
    uint32_t inst = 0xB10000 | ((src & 0xF) << 4) | ((dst & 0xF) << 12);
    emit_u24_le(e, inst);
}

void emit_srl(Emitter *e, Reg dst, Reg src) {
    uint32_t inst = 0x910000 | ((src & 0xF) << 4) | ((dst & 0xF) << 12);
    emit_u24_le(e, inst);
}

void emit_moveqz(Emitter *e, Reg dst, Reg src, Reg test) {
    uint32_t inst = 0x830000 | ((test & 0xF) << 4) | ((src & 0xF) << 8) | ((dst & 0xF) << 12);
    emit_u24_le(e, inst);
}

void emit_movnez(Emitter *e, Reg dst, Reg src, Reg test) {
    uint32_t inst = 0x930000 | ((test & 0xF) << 4) | ((src & 0xF) << 8) | ((dst & 0xF) << 12);
    emit_u24_le(e, inst);
}

void emit_movltz(Emitter *e, Reg dst, Reg src, Reg test) {
    uint32_t inst = 0xA30000 | ((test & 0xF) << 4) | ((src & 0xF) << 8) | ((dst & 0xF) << 12);
    emit_u24_le(e, inst);
}

void emit_movgez(Emitter *e, Reg dst, Reg src, Reg test) {
    uint32_t inst = 0xB30000 | ((test & 0xF) << 4) | ((src & 0xF) << 8) | ((dst & 0xF) << 12);
    emit_u24_le(e, inst);
}

// ============================================================================
// CALL0 Control Flow & Branches
// ============================================================================

void emit_call0(Emitter *e, int32_t offset) {
    int32_t word_offset = (offset - 4) / 4;
    uint32_t uoff = (uint32_t)(word_offset & 0x3FFFF);
    uint32_t inst = 0x05 | (uoff << 6);
    emit_u24_le(e, inst);
}

void emit_callx0(Emitter *e, Reg target) {
    uint32_t inst = 0x0000C0 | ((target & 0xF) << 8);
    emit_u24_le(e, inst);
}

void emit_jx(Emitter *e, Reg target) {
    uint32_t inst = 0x0000A0 | ((target & 0xF) << 8);
    emit_u24_le(e, inst);
}

void emit_j(Emitter *e, int32_t offset) {
    int32_t diff = offset - 4;
    uint32_t uoff = (uint32_t)(diff & 0x3FFFF);
    uint32_t inst = 0x06 | (uoff << 6);
    emit_u24_le(e, inst);
}

void emit_beqz(Emitter *e, Reg s, int32_t offset) {
    int32_t diff = offset - 4;
    uint32_t uimm = (uint32_t)(diff & 0xFFF);
    uint32_t imm_lo = uimm & 0x0F;
    uint32_t imm_hi = (uimm >> 4) & 0xFF;
    uint32_t inst = 0x000016 | ((s & 0xF) << 8) | (imm_lo << 12) | (imm_hi << 16);
    emit_u24_le(e, inst);
}

void emit_bnez(Emitter *e, Reg s, int32_t offset) {
    int32_t diff = offset - 4;
    uint32_t uimm = (uint32_t)(diff & 0xFFF);
    uint32_t imm_lo = uimm & 0x0F;
    uint32_t imm_hi = (uimm >> 4) & 0xFF;
    uint32_t inst = 0x000056 | ((s & 0xF) << 8) | (imm_lo << 12) | (imm_hi << 16);
    emit_u24_le(e, inst);
}

void emit_beq(Emitter *e, Reg s, Reg t, int32_t offset) {
    int32_t diff = offset - 4;
    uint8_t imm8 = (uint8_t)(diff & 0xFF);
    uint32_t inst = 0x001007 | ((s & 0xF) << 8) | ((t & 0xF) << 4) | (((uint32_t)imm8) << 16);
    emit_u24_le(e, inst);
}

void emit_bne(Emitter *e, Reg s, Reg t, int32_t offset) {
    int32_t diff = offset - 4;
    uint8_t imm8 = (uint8_t)(diff & 0xFF);
    uint32_t inst = 0x009007 | ((s & 0xF) << 8) | ((t & 0xF) << 4) | (((uint32_t)imm8) << 16);
    emit_u24_le(e, inst);
}

void emit_bge(Emitter *e, Reg s, Reg t, int32_t offset) {
    int32_t diff = offset - 4;
    uint8_t imm8 = (uint8_t)(diff & 0xFF);
    uint32_t inst = 0x00A007 | ((s & 0xF) << 8) | ((t & 0xF) << 4) | (((uint32_t)imm8) << 16);
    emit_u24_le(e, inst);
}

void emit_blt(Emitter *e, Reg s, Reg t, int32_t offset) {
    int32_t diff = offset - 4;
    uint8_t imm8 = (uint8_t)(diff & 0xFF);
    uint32_t inst = 0x002007 | ((s & 0xF) << 8) | ((t & 0xF) << 4) | (((uint32_t)imm8) << 16);
    emit_u24_le(e, inst);
}

void emit_patch_branch(Emitter *e, size_t inst_offset, size_t target_offset) {
    if (inst_offset + 3 > e->code_size) {
        return;
    }
    int32_t diff = (int32_t)target_offset - (int32_t)inst_offset - 4;
    uint8_t b0 = e->code[inst_offset];

    if ((b0 & 0x3F) == 0x06) {
        // J instruction
        uint32_t uoff = (uint32_t)(diff & 0x3FFFF);
        uint32_t inst = 0x06 | (uoff << 6);
        e->code[inst_offset + 0] = (uint8_t)(inst & 0xFF);
        e->code[inst_offset + 1] = (uint8_t)((inst >> 8) & 0xFF);
        e->code[inst_offset + 2] = (uint8_t)((inst >> 16) & 0xFF);
    } else if ((b0 & 0x0F) == 0x06) {
        // BRI12 instruction (BEQZ / BNEZ / etc.)
        uint32_t uimm = (uint32_t)(diff & 0xFFF);
        e->code[inst_offset + 1] = (uint8_t)((e->code[inst_offset + 1] & 0x0F) | ((uimm & 0x0F) << 4));
        e->code[inst_offset + 2] = (uint8_t)((uimm >> 4) & 0xFF);
    } else if ((b0 & 0x0F) == 0x07) {
        // RRI8 instruction (BEQ / BNE / BGE / BLT)
        e->code[inst_offset + 2] = (uint8_t)(diff & 0xFF);
    }
}

// ============================================================================
// Constant Loading & Literal Pool
// ============================================================================

size_t emit_add_literal(Emitter *e, int32_t val) {
    for (size_t i = 0; i < e->literal_count; i++) {
        if (e->literals[i] == val) {
            return i;
        }
    }
    if (ensure_literal_capacity(e, 1)) {
        size_t idx = e->literal_count++;
        e->literals[idx] = val;
        return idx;
    }
    return 0;
}

void emit_load_literal(Emitter *e, Reg dst, int32_t val) {
    size_t lit_idx = emit_add_literal(e, val);
    size_t inst_pos = e->code_size;

    if (ensure_fixup_capacity(e, 1)) {
        e->fixups[e->fixup_count].inst_offset = inst_pos;
        e->fixups[e->fixup_count].literal_index = lit_idx;
        e->fixup_count++;
    }

    emit_l32r(e, dst, 0); // Temporary offset, patched in emit_finalize
}

void emit_load_imm(Emitter *e, Reg dst, int32_t val) {
    if (val >= -2048 && val <= 2047) {
        emit_movi(e, dst, val);
    } else {
        emit_load_literal(e, dst, val);
    }
}

// ============================================================================
// String Literals
// ============================================================================

size_t emit_add_string(Emitter *e, const char *data, size_t len) {
    if (!e || !data) return 0;

    // Check if duplicate string exists
    for (size_t i = 0; i < e->string_count; i++) {
        if (e->strings[i].len == len && memcmp(e->strings[i].data, data, len) == 0) {
            return i;
        }
    }

    if (ensure_string_capacity(e, 1)) {
        size_t idx = e->string_count++;
        char *copy = (char *)arena_alloc(e->arena, len + 1);
        if (copy) {
            memcpy(copy, data, len);
            copy[len] = '\0';
        }
        e->strings[idx].data = copy;
        e->strings[idx].len = len;
        e->strings[idx].pool_offset = e->string_bytes_total;
        e->string_bytes_total += (len + 1); // include null terminator
        return idx;
    }
    return 0;
}

void emit_load_string_addr(Emitter *e, Reg dst, size_t str_idx) {
    // Pad code_size to multiple of 4 using valid NOPs
    size_t rem = e->code_size % 4;
    if (rem == 1) {
        emit_nop(e);       // +3 bytes -> multiple of 4
    } else if (rem == 2) {
        emit_nop_n(e);     // +2 bytes -> multiple of 4
    } else if (rem == 3) {
        emit_nop_n(e);     // +2 bytes
        emit_nop(e);       // +3 bytes -> +5 bytes -> multiple of 4
    }

    size_t call0_pos = e->code_size; // Guaranteed multiple of 4

    // Emit CALL0 with word_offset = 1 (target is call0_pos + 8)
    uint32_t inst = 0x05 | (1 << 6);
    emit_u24_le(e, inst);

    // Pad 5 bytes to reach call0_pos + 8
    emit_nop_n(e); // 2 bytes
    emit_nop(e);   // 3 bytes

    size_t fixup_pos = e->code_size; // Exactly call0_pos + 8

    if (ensure_str_fixup_capacity(e, 1)) {
        e->str_fixups[e->str_fixup_count].inst_offset = fixup_pos;
        e->str_fixups[e->str_fixup_count].a0_offset = call0_pos + 3;
        e->str_fixups[e->str_fixup_count].string_index = str_idx;
        e->str_fixup_count++;
    }

    emit_movi(e, XTENSA_A8, 0); // 3-byte placeholder for offset
    emit_add(e, dst, XTENSA_A0, XTENSA_A8);
}

// ============================================================================
// CALL0 Frame Helpers
// ============================================================================

void emit_prologue(Emitter *e, uint32_t frame_size) {
    uint32_t aligned_frame = (frame_size + 15) & ~15;
    if (aligned_frame > 0) {
        emit_addi(e, XTENSA_A1, XTENSA_A1, -(int8_t)aligned_frame);
        emit_s32i(e, XTENSA_A0, XTENSA_A1, aligned_frame - 4);
    }
}

void emit_epilogue(Emitter *e, uint32_t frame_size) {
    uint32_t aligned_frame = (frame_size + 15) & ~15;
    if (aligned_frame > 0) {
        emit_l32i(e, XTENSA_A0, XTENSA_A1, aligned_frame - 4);
        emit_addi(e, XTENSA_A1, XTENSA_A1, (int8_t)aligned_frame);
    }
    emit_ret_n(e);
}

void emit_entry(Emitter *e, uint32_t frame_size) {
    uint32_t imm12 = (frame_size + 7) / 8;
    uint32_t inst = 0x000136 | ((imm12 & 0xFFF) << 12);
    emit_u24_le(e, inst);
}

void emit_retw_n(Emitter *e) {
    emit_u16_le(e, 0xF01D);
}

void emit_memw(Emitter *e) {
    emit_u24_le(e, 0x00C020);
}

void emit_isync(Emitter *e) {
    emit_u24_le(e, 0x002000);
}

void emit_esync(Emitter *e) {
    emit_u24_le(e, 0x002020);
}

void emit_rsil(Emitter *e, Reg dst, uint8_t level) {
    uint32_t inst = 0x006000 | ((dst & 0xF) << 4) | ((level & 0xF) << 8);
    emit_u24_le(e, inst);
}

void emit_slli(Emitter *e, Reg dst, Reg src, uint8_t sa) {
    if (sa == 0) {
        if (dst != src) emit_mov_n(e, dst, src);
        return;
    }
    emit_movi(e, XTENSA_A11, sa & 31);
    emit_ssl(e, XTENSA_A11);
    emit_sll(e, dst, src);
}

void emit_srli(Emitter *e, Reg dst, Reg src, uint8_t sa) {
    if (sa == 0) {
        if (dst != src) emit_mov_n(e, dst, src);
        return;
    }
    emit_movi(e, XTENSA_A11, sa & 31);
    emit_ssr(e, XTENSA_A11);
    emit_srl(e, dst, src);
}

void emit_srai(Emitter *e, Reg dst, Reg src, uint8_t sa) {
    if (sa == 0) {
        if (dst != src) emit_mov_n(e, dst, src);
        return;
    }
    emit_movi(e, XTENSA_A11, sa & 31);
    emit_ssr(e, XTENSA_A11);
    emit_sra(e, dst, src);
}

void emit_optimize_peephole(Emitter *e) {
    if (!e || e->code_size < 6) return;

    for (size_t i = 0; i + 5 < e->code_size; ) {
        uint8_t b0 = e->code[i];
        uint8_t b1 = e->code[i + 1];
        uint8_t b2 = e->code[i + 2];

        // 1. Redundant store-then-load: s32i a2, a1, imm8 followed by l32i a2, a1, imm8
        if (b0 == 0x02 && b1 == 0x61) {
            uint8_t imm8 = b2;
            if (i + 5 < e->code_size && e->code[i + 3] == 0x02 && e->code[i + 4] == 0x21 && e->code[i + 5] == imm8) {
                // Replace redundant l32i with NOP
                e->code[i + 3] = 0xF0;
                e->code[i + 4] = 0x20;
                e->code[i + 5] = 0x00;
                i += 6;
                continue;
            }
        }

        // 2. Redundant addi aX, aX, 0 -> NOP
        if (b0 == 0x02 && (b1 & 0xF0) == 0xC0 && b2 == 0x00) {
            uint8_t dst = (b1 >> 4) & 0x0F;
            uint8_t src = (b0 >> 4) & 0x0F;
            if (dst == src) {
                e->code[i] = 0xF0;
                e->code[i + 1] = 0x20;
                e->code[i + 2] = 0x00;
                i += 3;
                continue;
            }
        }

        i++;
    }
}

uint8_t *emit_finalize(Emitter *e, size_t *out_size) {
    if (!e || !e->arena) {
        if (out_size) *out_size = 0;
        return NULL;
    }

    emit_optimize_peephole(e);

    if (e->literal_count == 0 && e->string_count == 0) {
        uint8_t *out = (uint8_t *)arena_alloc(e->arena, e->code_size);
        if (out && e->code_size > 0) {
            memcpy(out, e->code, e->code_size);
        }
        if (out_size) *out_size = e->code_size;
        return out;
    }

    // When literals or strings are present, place a jump at offset 0 (aligned to 4 bytes)
    // jumping over the literal + string pool into the function prologue.
    size_t jump_bytes = 4;
    size_t lit_bytes = e->literal_count * 4;
    size_t str_bytes = (e->string_bytes_total + 3) & ~3; // 4-byte aligned
    size_t pool_bytes = lit_bytes + str_bytes;
    size_t code_offset = jump_bytes + pool_bytes;
    size_t total_size = code_offset + e->code_size;

    uint8_t *out = (uint8_t *)arena_alloc(e->arena, total_size);
    if (!out) {
        if (out_size) *out_size = 0;
        return NULL;
    }
    memset(out, 0, total_size);

    // 1. Emit jump over literal pool: target is code_offset, inst PC is 0.
    int32_t jump_diff = (int32_t)code_offset - 4;
    uint32_t uoff = (uint32_t)(jump_diff & 0x3FFFF);
    uint32_t jump_inst = 0x06 | (uoff << 6);
    out[0] = (uint8_t)(jump_inst & 0xFF);
    out[1] = (uint8_t)((jump_inst >> 8) & 0xFF);
    out[2] = (uint8_t)((jump_inst >> 16) & 0xFF);
    out[3] = 0x00; // pad to 4-byte align literal pool

    // 2. Write literals in pool at offset 4
    for (size_t i = 0; i < e->literal_count; i++) {
        uint32_t val = (uint32_t)e->literals[i];
        out[4 + i * 4 + 0] = (uint8_t)(val & 0xFF);
        out[4 + i * 4 + 1] = (uint8_t)((val >> 8) & 0xFF);
        out[4 + i * 4 + 2] = (uint8_t)((val >> 16) & 0xFF);
        out[4 + i * 4 + 3] = (uint8_t)((val >> 24) & 0xFF);
    }

    // 3. Write strings in pool at offset 4 + lit_bytes
    size_t str_base = 4 + lit_bytes;
    for (size_t i = 0; i < e->string_count; i++) {
        size_t s_off = str_base + e->strings[i].pool_offset;
        if (e->strings[i].data && e->strings[i].len > 0) {
            memcpy(out + s_off, e->strings[i].data, e->strings[i].len);
        }
        out[s_off + e->strings[i].len] = '\0';
    }

    // 4. Copy code after pool
    if (e->code_size > 0) {
        memcpy(out + code_offset, e->code, e->code_size);
    }

    // 5. Apply L32R fixups
    for (size_t f = 0; f < e->fixup_count; f++) {
        size_t inst_pos = code_offset + e->fixups[f].inst_offset;
        size_t target_addr = 4 + e->fixups[f].literal_index * 4;
        size_t base_addr = (inst_pos + 3) & ~3;
        int32_t word_disp = ((int32_t)target_addr - (int32_t)base_addr) / 4;
        uint16_t imm16 = (uint16_t)(word_disp & 0xFFFF);

        out[inst_pos + 1] = (uint8_t)(imm16 & 0xFF);
        out[inst_pos + 2] = (uint8_t)((imm16 >> 8) & 0xFF);
    }

    // 6. Apply String Fixups
    for (size_t f = 0; f < e->str_fixup_count; f++) {
        size_t str_idx = e->str_fixups[f].string_index;
        size_t target_str_addr = str_base + e->strings[str_idx].pool_offset;
        size_t inst_pos = code_offset + e->str_fixups[f].inst_offset;
        size_t a0_val = code_offset + e->str_fixups[f].a0_offset;
        int32_t diff = (int32_t)target_str_addr - (int32_t)a0_val;

        uint32_t uimm = (uint32_t)(diff & 0xFFF);
        uint32_t imm_lo = uimm & 0xFF;
        uint32_t imm_hi = (uimm >> 8) & 0x0F;
        uint32_t inst = 0x00A002 | (XTENSA_A8 << 4) | (imm_hi << 8) | (imm_lo << 16);

        out[inst_pos + 0] = (uint8_t)(inst & 0xFF);
        out[inst_pos + 1] = (uint8_t)((inst >> 8) & 0xFF);
        out[inst_pos + 2] = (uint8_t)((inst >> 16) & 0xFF);
    }

    // 7. Adjust Relocations (only code-relative opcodes move with code_offset)
    for (size_t r = 0; r < e->reloc_count; r++) {
        if (e->relocs[r].type == R_XTENSA_SLOT0_OP) {
            e->relocs[r].inst_offset += code_offset;
        }
    }

    if (out_size) *out_size = total_size;
    return out;
}

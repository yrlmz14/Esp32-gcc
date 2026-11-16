#ifndef XTENSA_EMITTER_H
#define XTENSA_EMITTER_H

#include "arena.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/**
 * Strict CALL0 ABI Register Definitions (Xtensa LX6/LX7)
 */
typedef enum {
    XTENSA_A0  = 0,   // Return Address (caller-saved)
    XTENSA_A1  = 1,   // Stack Pointer (16-byte aligned frame)
    XTENSA_A2  = 2,   // Return Value & Function Arg 0
    XTENSA_A3  = 3,   // Function Arg 1
    XTENSA_A4  = 4,   // Function Arg 2
    XTENSA_A5  = 5,   // Function Arg 3
    XTENSA_A6  = 6,   // Function Arg 4
    XTENSA_A7  = 7,   // Function Arg 5
    XTENSA_A8  = 8,   // Temp / Scratch
    XTENSA_A9  = 9,   // Temp / Scratch
    XTENSA_A10 = 10,  // Temp / Scratch
    XTENSA_A11 = 11,  // Temp / Scratch
    XTENSA_A12 = 12,  // Callee-saved
    XTENSA_A13 = 13,  // Callee-saved
    XTENSA_A14 = 14,  // Callee-saved
    XTENSA_A15 = 15,  // Callee-saved
} Reg;

#define R_XTENSA_NONE         0
#define R_XTENSA_32           1
#define R_XTENSA_SLOT0_OP     20

typedef struct {
    size_t inst_offset;       // Byte offset of the L32R instruction in code buffer
    size_t literal_index;     // Index into emitter's literal table
} LiteralFixup;

typedef struct {
    const char *data;
    size_t len;               // Length excluding null terminator
    size_t pool_offset;       // Offset in string pool
} StringLiteral;

typedef struct {
    size_t inst_offset;       // Offset of movi instruction
    size_t a0_offset;         // Offset of a0 return address in code buffer
    size_t string_index;      // Index into strings array
} StringFixup;

typedef struct {
    size_t inst_offset;
    const char *sym_name;
    size_t sym_name_len;
    uint32_t type;
    int32_t addend;
} RelocEntry;

typedef struct {
    Arena *arena;
    uint8_t *code;
    size_t code_size;
    size_t code_capacity;

    // Literal Pool
    int32_t *literals;
    size_t literal_count;
    size_t literal_capacity;

    // Fixups for L32R
    LiteralFixup *fixups;
    size_t fixup_count;
    size_t fixup_capacity;

    // String Pool & Fixups
    StringLiteral *strings;
    size_t string_count;
    size_t string_capacity;
    size_t string_bytes_total;

    StringFixup *str_fixups;
    size_t str_fixup_count;
    size_t str_fixup_capacity;

    // Relocations (e.g. for CALL0 to external symbols)
    RelocEntry *relocs;
    size_t reloc_count;
    size_t reloc_capacity;
} Emitter;

void emit_add_reloc(Emitter *e, size_t inst_offset, const char *sym_name, size_t sym_len, uint32_t type, int32_t addend);

// Lifecycle
bool emit_init(Emitter *e, Arena *arena, size_t initial_capacity);
void emit_reset(Emitter *e);

// 16-bit Density Instructions (.N)
void emit_ret_n(Emitter *e);
void emit_nop_n(Emitter *e);
void emit_mov_n(Emitter *e, Reg dst, Reg src);
void emit_add_n(Emitter *e, Reg dst, Reg src1, Reg src2);
void emit_addi_n(Emitter *e, Reg dst, Reg src, int imm); // imm in range -1..15, excluding 0
void emit_movi_n(Emitter *e, Reg dst, int imm);         // imm in range -32..95
void emit_l32i_n(Emitter *e, Reg dst, Reg base, uint32_t offset_bytes); // offset: 0..60, multiple of 4
void emit_s32i_n(Emitter *e, Reg src, Reg base, uint32_t offset_bytes); // offset: 0..60, multiple of 4

// 24-bit Standard Arithmetic & Logical Instructions
void emit_ret(Emitter *e);
void emit_nop(Emitter *e);
void emit_add(Emitter *e, Reg dst, Reg src1, Reg src2);
void emit_sub(Emitter *e, Reg dst, Reg src1, Reg src2);
void emit_mull(Emitter *e, Reg dst, Reg src1, Reg src2);
void emit_quos(Emitter *e, Reg dst, Reg src1, Reg src2); // Signed divide
void emit_rems(Emitter *e, Reg dst, Reg src1, Reg src2); // Signed remainder/modulo
void emit_and(Emitter *e, Reg dst, Reg src1, Reg src2);
void emit_or(Emitter *e, Reg dst, Reg src1, Reg src2);
void emit_xor(Emitter *e, Reg dst, Reg src1, Reg src2);
void emit_neg(Emitter *e, Reg dst, Reg src);
void emit_addi(Emitter *e, Reg dst, Reg src, int8_t imm); // signed 8-bit: -128..127
void emit_movi(Emitter *e, Reg dst, int32_t imm);         // signed 12-bit: -2048..2047
void emit_l32i(Emitter *e, Reg dst, Reg base, uint32_t offset_bytes); // offset: 0..1020, multiple of 4
void emit_s32i(Emitter *e, Reg src, Reg base, uint32_t offset_bytes); // offset: 0..1020, multiple of 4
void emit_l8ui(Emitter *e, Reg dst, Reg base, uint32_t offset_bytes); // offset: 0..255
void emit_s8i(Emitter *e, Reg src, Reg base, uint32_t offset_bytes);  // offset: 0..255
void emit_l32r(Emitter *e, Reg dst, int32_t pc_rel_offset_words);

// Shifts
void emit_ssl(Emitter *e, Reg shift);
void emit_sll(Emitter *e, Reg dst, Reg src);
void emit_ssr(Emitter *e, Reg shift);
void emit_sra(Emitter *e, Reg dst, Reg src);
void emit_srl(Emitter *e, Reg dst, Reg src);

// Conditional Moves
void emit_moveqz(Emitter *e, Reg dst, Reg src, Reg cond);
void emit_movnez(Emitter *e, Reg dst, Reg src, Reg cond);
void emit_movltz(Emitter *e, Reg dst, Reg src, Reg cond);
void emit_movgez(Emitter *e, Reg dst, Reg src, Reg cond);

// Branches & Jumps
void emit_jx(Emitter *e, Reg target);
void emit_call0(Emitter *e, int32_t pc_rel_offset_bytes);
void emit_callx0(Emitter *e, Reg target);
void emit_j(Emitter *e, int32_t pc_rel_offset_bytes);
void emit_beqz(Emitter *e, Reg src, int32_t pc_rel_offset_bytes);
void emit_bnez(Emitter *e, Reg src, int32_t pc_rel_offset_bytes);
void emit_beq(Emitter *e, Reg s, Reg t, int32_t pc_rel_offset_bytes);
void emit_bne(Emitter *e, Reg s, Reg t, int32_t pc_rel_offset_bytes);
void emit_bge(Emitter *e, Reg s, Reg t, int32_t pc_rel_offset_bytes);
void emit_blt(Emitter *e, Reg s, Reg t, int32_t pc_rel_offset_bytes);

// Branch & Jump Backpatching
void emit_patch_branch(Emitter *e, size_t inst_offset, size_t target_offset);

// Constant Loading & Literal Pool Helpers
size_t emit_add_literal(Emitter *e, int32_t val);
void emit_load_literal(Emitter *e, Reg dst, int32_t val);
void emit_load_imm(Emitter *e, Reg dst, int32_t val);

// String Literals
size_t emit_add_string(Emitter *e, const char *data, size_t len);
void emit_load_string_addr(Emitter *e, Reg dst, size_t str_idx);

// Function Prologue / Epilogue Helpers (CALL0 & Windowed)
void emit_prologue(Emitter *e, uint32_t frame_size);
void emit_epilogue(Emitter *e, uint32_t frame_size);
void emit_entry(Emitter *e, uint32_t frame_size);
void emit_retw_n(Emitter *e);

// Hardware Assembly & Synchronization
void emit_memw(Emitter *e);
void emit_isync(Emitter *e);
void emit_esync(Emitter *e);
void emit_rsil(Emitter *e, Reg dst, uint8_t level);

// Immediate Shifts
void emit_slli(Emitter *e, Reg dst, Reg src, uint8_t sa);
void emit_srli(Emitter *e, Reg dst, Reg src, uint8_t sa);
void emit_srai(Emitter *e, Reg dst, Reg src, uint8_t sa);

// Peephole Optimizer
void emit_optimize_peephole(Emitter *e);

// Finalize complete function binary (prepends 4-byte aligned literal & string pool if present, applies fixups)
uint8_t *emit_finalize(Emitter *e, size_t *out_size);

#endif // XTENSA_EMITTER_H

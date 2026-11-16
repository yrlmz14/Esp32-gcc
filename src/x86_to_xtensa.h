/**
 * @file x86_to_xtensa.h
 * @brief Zero-dependency x86-32 to Xtensa LX7 (ESP32-S3 Call0 ABI) Binary Recompiler.
 *
 * Provides instruction decoding, IR construction, branch-flag fusion,
 * complex addressing mode decomposition, and native Xtensa machine code emission.
 */

#ifndef X86_TO_XTENSA_H
#define X86_TO_XTENSA_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================= */
/* 1. Register Mapping Architecture                                          */
/* ========================================================================= */

/**
 * Standard x86 32-bit General-Purpose Registers (encoded as 0..7).
 */
typedef enum {
    X86_REG_EAX = 0,
    X86_REG_ECX = 1,
    X86_REG_EDX = 2,
    X86_REG_EBX = 3,
    X86_REG_ESP = 4,
    X86_REG_EBP = 5,
    X86_REG_ESI = 6,
    X86_REG_EDI = 7,
    X86_REG_NONE = -1
} X86Reg;

/**
 * Xtensa LX7 (ESP32-S3) CALL0 ABI Register Definitions.
 *
 * Register Assignment:
 *   a0:      Return Address
 *   a1:      Host Stack Pointer (16-byte aligned)
 *   a2..a9:  x86 Registers (EAX, ECX, EDX, EBX, ESP, EBP, ESI, EDI)
 *   a10..a15: Dedicated Scratchpads (address calculation, flags, trampoline)
 */
typedef enum {
    XT_A0  = 0,   /* Return Address (CALL0 convention) */
    XT_A1  = 1,   /* Host Stack Pointer */
    XT_A2  = 2,   /* x86 EAX / Primary Return Value / Host Function Arg 0 */
    XT_A3  = 3,   /* x86 ECX / Host Function Arg 1 */
    XT_A4  = 4,   /* x86 EDX / Host Function Arg 2 */
    XT_A5  = 5,   /* x86 EBX / Host Function Arg 3 */
    XT_A6  = 6,   /* x86 ESP / Host Function Arg 4 */
    XT_A7  = 7,   /* x86 EBP / Host Function Arg 5 */
    XT_A8  = 8,   /* x86 ESI */
    XT_A9  = 9,   /* x86 EDI */
    XT_A10 = 10,  /* Scratchpad: Address calculation & Temp 0 */
    XT_A11 = 11,  /* Scratchpad: SIB scale shift & Temp 1 */
    XT_A12 = 12,  /* Scratchpad: Flag LHS comparison & Temp 2 */
    XT_A13 = 13,  /* Scratchpad: Flag RHS comparison & Temp 3 */
    XT_A14 = 14,  /* Scratchpad: IAT Call Trampoline target & Temp 4 */
    XT_A15 = 15   /* Scratchpad: Host stack frame saving & Temp 5 */
} XtensaReg;

/**
 * @brief Map an x86 register to its assigned Xtensa register.
 */
static inline XtensaReg x86_to_xtensa_reg(X86Reg r) {
    switch (r) {
        case X86_REG_EAX: return XT_A2;
        case X86_REG_ECX: return XT_A3;
        case X86_REG_EDX: return XT_A4;
        case X86_REG_EBX: return XT_A5;
        case X86_REG_ESP: return XT_A6;
        case X86_REG_EBP: return XT_A7;
        case X86_REG_ESI: return XT_A8;
        case X86_REG_EDI: return XT_A9;
        default:          return XT_A10;
    }
}

/* ========================================================================= */
/* 2. Win32 Import Address Table (IAT) Symbol Mapping                        */
/* ========================================================================= */

/**
 * Record describing a Win32 DLL import target for trampoline emission.
 */
typedef struct {
    uint32_t    iat_addr;        /* Virtual address of the IAT slot (e.g. 0x00405000) */
    const char *func_name;       /* Function name (e.g. "MessageBoxA", "ExitProcess") */
    uintptr_t   host_func_ptr;   /* Host C function pointer in ESP32 firmware */
    uint8_t     param_count;     /* Number of 32-bit parameters pushed in stdcall */
} Win32ImportSymbol;

typedef struct {
    const Win32ImportSymbol *symbols;
    size_t count;
} Win32SymbolMap;

/* ========================================================================= */
/* 3. Intermediate Representation (IR) Architecture                          */
/* ========================================================================= */

typedef enum {
    IR_OPND_NONE,
    IR_OPND_REG,
    IR_OPND_IMM,
    IR_OPND_MEM   /* [base + index * scale + disp] */
} IROpndKind;

typedef struct {
    IROpndKind kind;
    X86Reg     reg;
    int32_t    imm;
    X86Reg     base;
    X86Reg     index;
    uint8_t    scale;    /* 1, 2, 4, 8 */
    int32_t    disp;     /* 32-bit displacement */
} IROperand;

typedef enum {
    IR_NOP = 0,
    IR_MOV,
    IR_ADD,
    IR_SUB,
    IR_AND,
    IR_OR,
    IR_XOR,
    IR_INC,
    IR_DEC,
    IR_CMP,
    IR_TEST,
    IR_JMP,
    IR_JCC,
    IR_PUSH,
    IR_POP,
    IR_CALL_IAT,
    IR_CALL_DIRECT,
    IR_RET
} IROpcode;

/* x86 Condition Codes */
typedef enum {
    COND_O   = 0x0, /* Overflow */
    COND_NO  = 0x1, /* Not Overflow */
    COND_B   = 0x2, /* Below / Carry (unsigned <) */
    COND_AE  = 0x3, /* Above or Equal (unsigned >=) */
    COND_E   = 0x4, /* Equal / Zero (JE / JZ) */
    COND_NE  = 0x5, /* Not Equal / Not Zero (JNE / JNZ) */
    COND_BE  = 0x6, /* Below or Equal (unsigned <=) */
    COND_A   = 0x7, /* Above (unsigned >) */
    COND_S   = 0x8, /* Sign */
    COND_NS  = 0x9, /* Not Sign */
    COND_L   = 0xC, /* Less (signed <) */
    COND_GE  = 0xD, /* Greater or Equal (signed >=) */
    COND_LE  = 0xE, /* Less or Equal (signed <=) */
    COND_G   = 0xF  /* Greater (signed >) */
} IRCond;

typedef struct {
    IROpcode   op;
    IROperand  dst;
    IROperand  src;
    IRCond     cond;
    int32_t    target_x86_off;    /* Jump target byte offset in original x86 stream */
    bool       fused;             /* Fused into compare-and-branch */
    const Win32ImportSymbol *iat_sym;
    uint32_t   x86_offset;        /* Original instruction offset */
    uint32_t   x86_len;           /* Original instruction length */
} IRInst;

/* ========================================================================= */
/* 4. Core Public API                                                        */
/* ========================================================================= */

/**
 * @brief Translates an x86-32 machine code stream into native Xtensa LX7 instructions.
 *
 * @param x86_code       Pointer to input buffer containing x86-32 machine code bytes
 * @param length         Length of input x86 code in bytes
 * @param out_xtensa_buf Pointer to destination buffer for native Xtensa machine code
 * @param sym_map        Optional Win32 symbol map for IAT call resolution (can be NULL)
 *
 * @return Total size of native Xtensa machine code emitted in bytes (0 on failure).
 */
size_t translate_x86_to_xtensa(const uint8_t *x86_code,
                               size_t length,
                               uint8_t *out_xtensa_buf,
                               const Win32SymbolMap *sym_map);

/**
 * @brief Disassemble emitted Xtensa bytecode to stdout (for debugging & verification).
 *
 * @param code Pointer to native Xtensa machine code
 * @param size Length of code in bytes
 */
void xtensa_disassemble_dump(const uint8_t *code, size_t size);

#ifdef __cplusplus
}
#endif

#endif /* X86_TO_XTENSA_H */

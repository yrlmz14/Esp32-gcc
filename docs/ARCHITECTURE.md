# Compiler Architecture & Internals Specification

## 1. System Pipeline

```
 Source Code (.c / .cpp)
          │
          ▼
┌──────────────────┐
│   Lexer / CPP    │ ── Token stream & Macro Table (#define, #include, #ifdef)
└──────────────────┘
          │
          ▼
┌──────────────────┐
│  Parser & AST    │ ── Recursive descent expression parser & type checker
└──────────────────┘
          │
          ▼
┌──────────────────┐
│  Xtensa Emitter  │ ── Emits CALL0 machine instructions & manages scratch stack
└──────────────────┘
          │
          ▼
┌──────────────────┐
│Peephole Optimizer│ ── Multi-pass redundant load/store & identity op elimination
└──────────────────┘
          │
          ▼
┌──────────────────┐
│    ELF Writer    │ ── Direct ELF32 relocatable binary generator (.text, .rela.text)
└──────────────────┘
          │
          ▼
 Native ELF32 Relocatable Object (.o)
```

---

## 2. Memory & Allocator Architecture (`src/arena.c`)

- `tiny-xtensa-cc` enforces a **100% deterministic memory model**:
  - Allocated in chunks of `Arena` memory (default `16 MB` per compilation session).
  - No OS dynamic heap allocations (`malloc`, `free`, `realloc`) during tokenization, parsing, code generation, or ELF serialization.
  - All symbols, AST nodes, string tables, and byte buffers are freed simultaneously upon translation unit completion via `arena_free()`.

---

## 3. Register Conventions & Calling Convention (Xtensa LX6/LX7 CALL0 ABI)

| Register | CALL0 ABI Role | Managed By |
|---|---|---|
| `a0` | Return Address | Saved in prologue at `a1 + frame_size - 4`, restored in epilogue |
| `a1` | Stack Pointer / Frame Pointer | 16-byte aligned frame decrement in prologue |
| `a2` | Return Value / Argument 0 / `this` pointer | Primary expression result register |
| `a3`..`a7` | Arguments 1 through 5 | Function parameters |
| `a8`..`a15` | Caller-Saved Scratch Registers | Managed by parser scratch depth (`save_left_operand`) |

### Scratch Register Management:
- During nested expression evaluation (e.g. `(a + b) * (c - d)`), partial results in `a2` are saved via `save_left_operand()`:
  - Depths 0 to 5 use hardware registers `a8`..`a13`.
  - Additional depths spill to dedicated stack frame slots.
  - Paired `restore_left_operand()` restores the operands in strict LIFO order.

---

## 4. Machine Instruction Encoder (`src/xtensa_emitter.c`)

### Density 16-Bit Instructions (`.N`):
- `emit_ret_n`: `0xF00D`
- `emit_nop_n`: `0xF03D`
- `emit_mov_n`: `0x000D | (dst << 4) | (src << 8)`
- `emit_add_n`: `0x000A | (s2 << 4) | (s1 << 8) | (dst << 12)`
- `emit_l32i_n`: `0x0008 | (dst << 4) | (base << 8) | (word_offset << 12)`
- `emit_s32i_n`: `0x0009 | (src << 4) | (base << 8) | (word_offset << 12)`

### Standard 24-Bit Instructions:
- Arithmetic: `emit_add`, `emit_sub`, `emit_mull`, `emit_quos`, `emit_rems`, `emit_neg`
- Bitwise: `emit_and`, `emit_or`, `emit_xor`
- Memory: `emit_l32i`, `emit_s32i`, `emit_l8ui`, `emit_s8i`, `emit_l32r`
- Control Flow: `emit_call0`, `emit_callx0`, `emit_j`, `emit_jx`, `emit_beqz`, `emit_bnez`, `emit_beq`, `emit_bne`, `emit_bge`, `emit_blt`
- Synchronization: `emit_memw` (`0x00C020`), `emit_isync` (`0x002000`), `emit_esync` (`0x002020`), `emit_rsil` (`0x006000`)
- Shifts: `emit_ssl`, `emit_ssr`, `emit_sll`, `emit_srl`, `emit_sra`, `emit_slli`, `emit_srli`, `emit_srai`

---

## 5. Direct ELF32 Relocatable Object Writer (`src/elf_writer.c`)

The binary writer generates a fully conformant ELF32 binary:

1. **Header (`Elf32_Ehdr`)**:
   - `e_ident`: `[0x7f, 'E', 'L', 'F', 1, 1, 1, 0, ...]` (32-bit, Little Endian)
   - `e_type`: `ET_REL` (1)
   - `e_machine`: `EM_XTENSA` (94 / `0x005E`)
   - `e_ehsize`: 52 bytes, `e_shentsize`: 40 bytes
2. **Sections Generated**:
   - `[0] SHN_UNDEF`: Null section
   - `[1] .text`: `SHT_PROGBITS`, `SHF_ALLOC | SHF_EXECINSTR`
   - `[2] .rodata`: `SHT_PROGBITS`, `SHF_ALLOC`
   - `[3] .rela.text`: `SHT_RELA`, `SHF_INFO_LINK` (relocation table)
   - `[4] .symtab`: `SHT_SYMTAB` (symbol table with local symbols first)
   - `[5] .strtab`: `SHT_STRTAB` (symbol string table)
   - `[6] .shstrtab`: `SHT_STRTAB` (section header string table)
3. **Relocation Types Supported**:
   - `R_XTENSA_SLOT0_OP` (20): CALL0 / L32R instruction displacements.
   - `R_XTENSA_32` (1): 32-bit word literal references.

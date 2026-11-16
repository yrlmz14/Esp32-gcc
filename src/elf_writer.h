#ifndef ELF_WRITER_H
#define ELF_WRITER_H

#include "arena.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define EI_NIDENT       16
#define ELFMAG0         0x7f
#define ELFMAG1         'E'
#define ELFMAG2         'L'
#define ELFMAG3         'F'

#define ELFCLASS32      1
#define ELFDATA2LSB     1
#define EV_CURRENT      1
#define ELFOSABI_NONE   0

#define ET_REL          1
#define EM_XTENSA       94   // 0x005E

#define SHT_NULL        0
#define SHT_PROGBITS    1
#define SHT_SYMTAB      2
#define SHT_STRTAB      3
#define SHT_RELA        4

#define SHF_WRITE       0x1
#define SHF_ALLOC       0x2
#define SHF_EXECINSTR   0x4
#define SHF_INFO_LINK   0x40

#define STN_UNDEF       0
#define SHN_UNDEF       0
#define SHN_ABS         0xfff1

#define STB_LOCAL       0
#define STB_GLOBAL      1

#define STT_NOTYPE      0
#define STT_OBJECT      1
#define STT_FUNC        2
#define STT_SECTION     3
#define STT_FILE        4

#define R_XTENSA_NONE         0
#define R_XTENSA_32           1
#define R_XTENSA_SLOT0_OP     20

#define ELF32_ST_INFO(bind, type) (((bind) << 4) + ((type) & 0xf))
#define ELF32_R_INFO(sym, type)   (((sym) << 8) + ((type) & 0xff))

typedef struct {
    unsigned char e_ident[EI_NIDENT];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint32_t e_entry;
    uint32_t e_phoff;
    uint32_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} __attribute__((packed)) Elf32_Ehdr;

typedef struct {
    uint32_t sh_name;
    uint32_t sh_type;
    uint32_t sh_flags;
    uint32_t sh_addr;
    uint32_t sh_offset;
    uint32_t sh_size;
    uint32_t sh_link;
    uint32_t sh_info;
    uint32_t sh_addralign;
    uint32_t sh_entsize;
} __attribute__((packed)) Elf32_Shdr;

typedef struct {
    uint32_t st_name;
    uint32_t st_value;
    uint32_t st_size;
    uint8_t  st_info;
    uint8_t  st_other;
    uint16_t st_shndx;
} __attribute__((packed)) Elf32_Sym;

typedef struct {
    uint32_t r_offset;
    uint32_t r_info;
    int32_t  r_addend;
} __attribute__((packed)) Elf32_Rela;

typedef struct {
    size_t inst_offset;
    const char *sym_name;
    uint32_t type;
    int32_t addend;
} ElfReloc;

typedef struct {
    char *data;
    size_t size;
    size_t capacity;
} StrtabBuilder;

typedef struct {
    Arena *arena;

    // Sections
    const uint8_t *text_data;
    size_t text_len;

    const uint8_t *rodata_data;
    size_t rodata_len;

    // Symbol table
    Elf32_Sym *syms;
    const char **sym_names;
    size_t sym_count;
    size_t sym_capacity;

    // Relocations
    ElfReloc *relocs;
    size_t reloc_count;
    size_t reloc_capacity;

    // String tables
    StrtabBuilder strtab;
    StrtabBuilder shstrtab;
} ElfWriter;

/**
 * Initialize ELF32 Relocatable Object Writer with arena.
 */
void elf_writer_init(ElfWriter *w, Arena *arena);

/**
 * Set .text section raw bytes.
 */
void elf_writer_set_text(ElfWriter *w, const uint8_t *text_data, size_t text_len);

/**
 * Set .rodata section raw bytes.
 */
void elf_writer_set_rodata(ElfWriter *w, const uint8_t *rodata_data, size_t rodata_len);

/**
 * Add a symbol to .symtab and .strtab.
 */
void elf_writer_add_symbol(ElfWriter *w, const char *name, uint32_t value, uint32_t size, uint8_t type, uint8_t binding, uint16_t shndx);

/**
 * Add a relocation for .text section.
 */
void elf_writer_add_reloc(ElfWriter *w, size_t inst_offset, const char *sym_name, uint32_t type, int32_t addend);

/**
 * Build and emit the complete in-memory ELF32 relocatable object binary.
 */
uint8_t *elf_writer_emit(ElfWriter *w, size_t *out_size);

/**
 * Serialize ELF32 object to disk at filepath.
 */
bool elf_writer_write_file(ElfWriter *w, const char *filepath);

#endif // ELF_WRITER_H

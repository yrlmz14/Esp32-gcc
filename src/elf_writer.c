#include "elf_writer.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

// ============================================================================
// String Table Builder
// ============================================================================

static void strtab_init(StrtabBuilder *b, Arena *arena) {
    b->capacity = 64;
    b->data = (char *)arena_alloc(arena, b->capacity);
    if (b->data) {
        b->data[0] = '\0';
        b->size = 1;
    } else {
        b->size = 0;
        b->capacity = 0;
    }
}

static uint32_t strtab_add(StrtabBuilder *b, Arena *arena, const char *str) {
    if (!str || !*str) return 0;

    // Check if already present
    size_t len = strlen(str);
    for (size_t i = 1; i + len < b->size; i++) {
        if (b->data[i - 1] == '\0' && strcmp(&b->data[i], str) == 0) {
            return (uint32_t)i;
        }
    }

    // Ensure capacity
    if (b->size + len + 1 > b->capacity) {
        size_t new_cap = b->capacity ? b->capacity * 2 : 128;
        while (new_cap < b->size + len + 1) {
            new_cap *= 2;
        }
        char *new_data = (char *)arena_alloc(arena, new_cap);
        if (!new_data) return 0;
        if (b->size > 0) {
            memcpy(new_data, b->data, b->size);
        }
        b->data = new_data;
        b->capacity = new_cap;
    }

    uint32_t offset = (uint32_t)b->size;
    memcpy(b->data + b->size, str, len);
    b->data[b->size + len] = '\0';
    b->size += len + 1;
    return offset;
}

// ============================================================================
// ELF Writer Core Implementation
// ============================================================================

void elf_writer_init(ElfWriter *w, Arena *arena) {
    if (!w || !arena) return;
    w->arena = arena;
    w->text_data = NULL;
    w->text_len = 0;
    w->rodata_data = NULL;
    w->rodata_len = 0;

    strtab_init(&w->strtab, arena);
    strtab_init(&w->shstrtab, arena);

    w->sym_capacity = 16;
    w->syms = (Elf32_Sym *)arena_alloc(arena, w->sym_capacity * sizeof(Elf32_Sym));
    w->sym_names = (const char **)arena_alloc(arena, w->sym_capacity * sizeof(const char *));
    w->sym_count = 0;

    w->reloc_capacity = 8;
    w->relocs = (ElfReloc *)arena_alloc(arena, w->reloc_capacity * sizeof(ElfReloc));
    w->reloc_count = 0;

    // Add STN_UNDEF (Symbol 0 is always undef/null)
    if (w->syms) {
        memset(&w->syms[0], 0, sizeof(Elf32_Sym));
        w->sym_names[0] = "";
        w->sym_count = 1;
    }
}

void elf_writer_set_text(ElfWriter *w, const uint8_t *text_data, size_t text_len) {
    if (w) {
        w->text_data = text_data;
        w->text_len = text_len;
    }
}

void elf_writer_set_rodata(ElfWriter *w, const uint8_t *rodata_data, size_t rodata_len) {
    if (w) {
        w->rodata_data = rodata_data;
        w->rodata_len = rodata_len;
    }
}

void elf_writer_add_symbol(ElfWriter *w, const char *name, uint32_t value, uint32_t size, uint8_t type, uint8_t binding, uint16_t shndx) {
    if (!w || !w->arena) return;

    if (w->sym_count + 1 > w->sym_capacity) {
        size_t new_cap = w->sym_capacity * 2;
        Elf32_Sym *new_syms = (Elf32_Sym *)arena_alloc(w->arena, new_cap * sizeof(Elf32_Sym));
        const char **new_names = (const char **)arena_alloc(w->arena, new_cap * sizeof(const char *));
        if (!new_syms || !new_names) return;
        memcpy(new_syms, w->syms, w->sym_count * sizeof(Elf32_Sym));
        memcpy(new_names, w->sym_names, w->sym_count * sizeof(const char *));
        w->syms = new_syms;
        w->sym_names = new_names;
        w->sym_capacity = new_cap;
    }

    uint32_t name_offset = strtab_add(&w->strtab, w->arena, name);
    Elf32_Sym *sym = &w->syms[w->sym_count];
    w->sym_names[w->sym_count] = arena_strdup(w->arena, name);
    w->sym_count++;

    sym->st_name = name_offset;
    sym->st_value = value;
    sym->st_size = size;
    sym->st_info = ELF32_ST_INFO(binding, type);
    sym->st_other = 0;
    sym->st_shndx = shndx;
}

void elf_writer_add_reloc(ElfWriter *w, size_t inst_offset, const char *sym_name, uint32_t type, int32_t addend) {
    if (!w || !w->arena) return;

    if (w->reloc_count + 1 > w->reloc_capacity) {
        size_t new_cap = w->reloc_capacity * 2;
        ElfReloc *new_relocs = (ElfReloc *)arena_alloc(w->arena, new_cap * sizeof(ElfReloc));
        if (!new_relocs) return;
        memcpy(new_relocs, w->relocs, w->reloc_count * sizeof(ElfReloc));
        w->relocs = new_relocs;
        w->reloc_capacity = new_cap;
    }

    ElfReloc *r = &w->relocs[w->reloc_count++];
    r->inst_offset = inst_offset;
    r->sym_name = arena_strdup(w->arena, sym_name);
    r->type = type;
    r->addend = addend;
}

uint8_t *elf_writer_emit(ElfWriter *w, size_t *out_size) {
    if (!w || !w->arena) {
        if (out_size) *out_size = 0;
        return NULL;
    }

    // Ensure all referenced symbols exist in symbol table
    for (size_t r = 0; r < w->reloc_count; r++) {
        const char *rname = w->relocs[r].sym_name;
        bool found = false;
        for (size_t s = 1; s < w->sym_count; s++) {
            if (strcmp(w->sym_names[s], rname) == 0) {
                found = true;
                break;
            }
        }
        if (!found) {
            elf_writer_add_symbol(w, rname, 0, 0, STT_NOTYPE, STB_GLOBAL, SHN_UNDEF);
        }
    }

    // 1. Populate Section Header String Table (.shstrtab)
    uint32_t sh_name_text = strtab_add(&w->shstrtab, w->arena, ".text");
    uint32_t sh_name_rodata = strtab_add(&w->shstrtab, w->arena, ".rodata");
    uint32_t sh_name_rela_text = (w->reloc_count > 0) ? strtab_add(&w->shstrtab, w->arena, ".rela.text") : 0;
    uint32_t sh_name_symtab = strtab_add(&w->shstrtab, w->arena, ".symtab");
    uint32_t sh_name_strtab = strtab_add(&w->shstrtab, w->arena, ".strtab");
    uint32_t sh_name_shstrtab = strtab_add(&w->shstrtab, w->arena, ".shstrtab");

    // 2. Separate/sort symbols: STB_LOCAL first, STB_GLOBAL second
    Elf32_Sym *sorted_syms = (Elf32_Sym *)arena_alloc(w->arena, w->sym_count * sizeof(Elf32_Sym));
    const char **sorted_names = (const char **)arena_alloc(w->arena, w->sym_count * sizeof(const char *));
    if (!sorted_syms || !sorted_names) {
        if (out_size) *out_size = 0;
        return NULL;
    }

    size_t sorted_count = 0;
    // Copy UNDEF (sym 0)
    sorted_syms[sorted_count] = w->syms[0];
    sorted_names[sorted_count] = w->sym_names[0];
    sorted_count++;

    // Copy locals
    for (size_t i = 1; i < w->sym_count; i++) {
        if ((w->syms[i].st_info >> 4) == STB_LOCAL) {
            sorted_syms[sorted_count] = w->syms[i];
            sorted_names[sorted_count] = w->sym_names[i];
            sorted_count++;
        }
    }
    size_t first_global_idx = sorted_count;

    // Copy globals
    for (size_t i = 1; i < w->sym_count; i++) {
        if ((w->syms[i].st_info >> 4) != STB_LOCAL) {
            sorted_syms[sorted_count] = w->syms[i];
            sorted_names[sorted_count] = w->sym_names[i];
            sorted_count++;
        }
    }

    // Map relocations to sorted symbol table indices
    Elf32_Rela *rela_entries = NULL;
    if (w->reloc_count > 0) {
        rela_entries = (Elf32_Rela *)arena_alloc(w->arena, w->reloc_count * sizeof(Elf32_Rela));
        for (size_t r = 0; r < w->reloc_count; r++) {
            const char *rname = w->relocs[r].sym_name;
            uint32_t sym_idx = 0;
            for (size_t s = 1; s < sorted_count; s++) {
                if (strcmp(sorted_names[s], rname) == 0) {
                    sym_idx = (uint32_t)s;
                    break;
                }
            }
            rela_entries[r].r_offset = (uint32_t)w->relocs[r].inst_offset;
            rela_entries[r].r_info = ELF32_R_INFO(sym_idx, w->relocs[r].type);
            rela_entries[r].r_addend = w->relocs[r].addend;
        }
    }

    // 3. Compute section offsets and sizes
    size_t offset = sizeof(Elf32_Ehdr);

    // .text
    size_t text_offset = (offset + 3) & ~3;
    offset = text_offset + w->text_len;

    // .rodata
    size_t rodata_offset = (offset + 3) & ~3;
    offset = rodata_offset + w->rodata_len;

    // .rela.text
    size_t rela_text_offset = 0;
    size_t rela_text_size = 0;
    if (w->reloc_count > 0) {
        rela_text_offset = (offset + 3) & ~3;
        rela_text_size = w->reloc_count * sizeof(Elf32_Rela);
        offset = rela_text_offset + rela_text_size;
    }

    // .symtab
    size_t symtab_offset = (offset + 3) & ~3;
    size_t symtab_size = sorted_count * sizeof(Elf32_Sym);
    offset = symtab_offset + symtab_size;

    // .strtab
    size_t strtab_offset = offset;
    offset = strtab_offset + w->strtab.size;

    // .shstrtab
    size_t shstrtab_offset = offset;
    offset = shstrtab_offset + w->shstrtab.size;

    // Section Header Table
    size_t sh_offset = (offset + 3) & ~3;
    size_t num_sections = (w->reloc_count > 0) ? 7 : 6;
    size_t total_size = sh_offset + num_sections * sizeof(Elf32_Shdr);

    // Allocate total ELF file buffer
    uint8_t *out = (uint8_t *)arena_alloc(w->arena, total_size);
    if (!out) {
        if (out_size) *out_size = 0;
        return NULL;
    }
    memset(out, 0, total_size);

    // Section index assignment:
    // 0: NULL
    // 1: .text
    // 2: .rodata
    // If relocs:
    // 3: .rela.text, 4: .symtab, 5: .strtab, 6: .shstrtab
    // Else:
    // 3: .symtab, 4: .strtab, 5: .shstrtab
    uint16_t idx_rela = (w->reloc_count > 0) ? 3 : 0;
    uint16_t idx_symtab = (w->reloc_count > 0) ? 4 : 3;
    uint16_t idx_strtab = (w->reloc_count > 0) ? 5 : 4;
    uint16_t idx_shstrtab = (w->reloc_count > 0) ? 6 : 5;

    // 4. Fill ELF Header
    Elf32_Ehdr *ehdr = (Elf32_Ehdr *)out;
    ehdr->e_ident[0] = ELFMAG0;
    ehdr->e_ident[1] = ELFMAG1;
    ehdr->e_ident[2] = ELFMAG2;
    ehdr->e_ident[3] = ELFMAG3;
    ehdr->e_ident[4] = ELFCLASS32;
    ehdr->e_ident[5] = ELFDATA2LSB;
    ehdr->e_ident[6] = EV_CURRENT;
    ehdr->e_ident[7] = ELFOSABI_NONE;
    ehdr->e_type = ET_REL;
    ehdr->e_machine = EM_XTENSA;
    ehdr->e_version = EV_CURRENT;
    ehdr->e_entry = 0;
    ehdr->e_phoff = 0;
    ehdr->e_shoff = (uint32_t)sh_offset;
    ehdr->e_flags = 0x00000000;
    ehdr->e_ehsize = (uint16_t)sizeof(Elf32_Ehdr);
    ehdr->e_phentsize = 0;
    ehdr->e_phnum = 0;
    ehdr->e_shentsize = (uint16_t)sizeof(Elf32_Shdr);
    ehdr->e_shnum = (uint16_t)num_sections;
    ehdr->e_shstrndx = idx_shstrtab;

    // 5. Write Section Payload Data
    if (w->text_len > 0 && w->text_data) {
        memcpy(out + text_offset, w->text_data, w->text_len);
    }
    if (w->rodata_len > 0 && w->rodata_data) {
        memcpy(out + rodata_offset, w->rodata_data, w->rodata_len);
    }
    if (rela_text_size > 0 && rela_entries) {
        memcpy(out + rela_text_offset, rela_entries, rela_text_size);
    }
    if (symtab_size > 0) {
        memcpy(out + symtab_offset, sorted_syms, symtab_size);
    }
    if (w->strtab.size > 0 && w->strtab.data) {
        memcpy(out + strtab_offset, w->strtab.data, w->strtab.size);
    }
    if (w->shstrtab.size > 0 && w->shstrtab.data) {
        memcpy(out + shstrtab_offset, w->shstrtab.data, w->shstrtab.size);
    }

    // 6. Fill Section Header Table
    Elf32_Shdr *shdrs = (Elf32_Shdr *)(out + sh_offset);

    // Section 0: NULL
    memset(&shdrs[0], 0, sizeof(Elf32_Shdr));

    // Section 1: .text
    shdrs[1].sh_name = sh_name_text;
    shdrs[1].sh_type = SHT_PROGBITS;
    shdrs[1].sh_flags = SHF_ALLOC | SHF_EXECINSTR;
    shdrs[1].sh_addr = 0;
    shdrs[1].sh_offset = (uint32_t)text_offset;
    shdrs[1].sh_size = (uint32_t)w->text_len;
    shdrs[1].sh_link = 0;
    shdrs[1].sh_info = 0;
    shdrs[1].sh_addralign = 4;
    shdrs[1].sh_entsize = 0;

    // Section 2: .rodata
    shdrs[2].sh_name = sh_name_rodata;
    shdrs[2].sh_type = SHT_PROGBITS;
    shdrs[2].sh_flags = SHF_ALLOC;
    shdrs[2].sh_addr = 0;
    shdrs[2].sh_offset = (uint32_t)rodata_offset;
    shdrs[2].sh_size = (uint32_t)w->rodata_len;
    shdrs[2].sh_link = 0;
    shdrs[2].sh_info = 0;
    shdrs[2].sh_addralign = 4;
    shdrs[2].sh_entsize = 0;

    if (w->reloc_count > 0) {
        // Section 3: .rela.text
        shdrs[idx_rela].sh_name = sh_name_rela_text;
        shdrs[idx_rela].sh_type = SHT_RELA;
        shdrs[idx_rela].sh_flags = SHF_INFO_LINK;
        shdrs[idx_rela].sh_addr = 0;
        shdrs[idx_rela].sh_offset = (uint32_t)rela_text_offset;
        shdrs[idx_rela].sh_size = (uint32_t)rela_text_size;
        shdrs[idx_rela].sh_link = idx_symtab; // Link to symtab
        shdrs[idx_rela].sh_info = 1;          // Section to which relocation applies (.text)
        shdrs[idx_rela].sh_addralign = 4;
        shdrs[idx_rela].sh_entsize = sizeof(Elf32_Rela);
    }

    // Section: .symtab
    shdrs[idx_symtab].sh_name = sh_name_symtab;
    shdrs[idx_symtab].sh_type = SHT_SYMTAB;
    shdrs[idx_symtab].sh_flags = 0;
    shdrs[idx_symtab].sh_addr = 0;
    shdrs[idx_symtab].sh_offset = (uint32_t)symtab_offset;
    shdrs[idx_symtab].sh_size = (uint32_t)symtab_size;
    shdrs[idx_symtab].sh_link = idx_strtab;              // Link to string table
    shdrs[idx_symtab].sh_info = (uint32_t)first_global_idx; // One greater than last local symbol index
    shdrs[idx_symtab].sh_addralign = 4;
    shdrs[idx_symtab].sh_entsize = sizeof(Elf32_Sym);

    // Section: .strtab
    shdrs[idx_strtab].sh_name = sh_name_strtab;
    shdrs[idx_strtab].sh_type = SHT_STRTAB;
    shdrs[idx_strtab].sh_flags = 0;
    shdrs[idx_strtab].sh_addr = 0;
    shdrs[idx_strtab].sh_offset = (uint32_t)strtab_offset;
    shdrs[idx_strtab].sh_size = (uint32_t)w->strtab.size;
    shdrs[idx_strtab].sh_link = 0;
    shdrs[idx_strtab].sh_info = 0;
    shdrs[idx_strtab].sh_addralign = 1;
    shdrs[idx_strtab].sh_entsize = 0;

    // Section: .shstrtab
    shdrs[idx_shstrtab].sh_name = sh_name_shstrtab;
    shdrs[idx_shstrtab].sh_type = SHT_STRTAB;
    shdrs[idx_shstrtab].sh_flags = 0;
    shdrs[idx_shstrtab].sh_addr = 0;
    shdrs[idx_shstrtab].sh_offset = (uint32_t)shstrtab_offset;
    shdrs[idx_shstrtab].sh_size = (uint32_t)w->shstrtab.size;
    shdrs[idx_shstrtab].sh_link = 0;
    shdrs[idx_shstrtab].sh_info = 0;
    shdrs[idx_shstrtab].sh_addralign = 1;
    shdrs[idx_shstrtab].sh_entsize = 0;

    if (out_size) {
        *out_size = total_size;
    }
    return out;
}

bool elf_writer_write_file(ElfWriter *w, const char *filepath) {
    if (!w || !filepath) return false;
    size_t out_size = 0;
    uint8_t *elf_data = elf_writer_emit(w, &out_size);
    if (!elf_data || out_size == 0) return false;

    FILE *f = fopen(filepath, "wb");
    if (!f) return false;

    size_t written = fwrite(elf_data, 1, out_size, f);
    fclose(f);
    return (written == out_size);
}

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>

#include "arena.h"
#include "lexer.h"
#include "xtensa_emitter.h"
#include "symbol_table.h"
#include "parser.h"
#include "elf_writer.h"

#define ARENA_SIZE (4 * 1024 * 1024) // 4MB contiguous arena

static void print_usage(const char *prog_name) {
    printf("Tiny Xtensa C Compiler (CALL0 ABI for ESP32 LX6/LX7)\n");
    printf("Usage:\n");
    printf("  %s [options] <input.c>\n", prog_name);
    printf("  %s [options] -c \"<c_code>\"\n\n", prog_name);
    printf("Options:\n");
    printf("  -o <output.o|output.bin>  Set output file (default: a.out.o)\n");
    printf("  -c <c_code>               Compile direct C source string\n");
    printf("  -d, --disasm              Disassemble emitted binary/ELF via objdump\n");
    printf("  -v, --version             Display compiler version\n");
    printf("  -h, --help                Display this help message\n");
}

static const char *get_objdump_path(void) {
    const char *env = getenv("OBJDUMP");
    if (env && access(env, X_OK) == 0) return env;

    if (system("which xtensa-esp32-elf-objdump > /dev/null 2>&1") == 0) {
        return "xtensa-esp32-elf-objdump";
    }

    const char *candidates[] = {
        "/home/okan/.espressif/tools/xtensa-esp-elf/esp-14.2.0_20241119/xtensa-esp-elf/bin/xtensa-esp32-elf-objdump",
        "/home/okan/.espressif/tools/xtensa-esp32-elf/esp-12.2.0_20230208/xtensa-esp32-elf/bin/xtensa-esp32-elf-objdump",
    };
    for (size_t i = 0; i < sizeof(candidates)/sizeof(candidates[0]); i++) {
        if (access(candidates[i], X_OK) == 0) return candidates[i];
    }
    return "xtensa-esp32-elf-objdump";
}

static char *read_file_to_arena(Arena *arena, const char *filepath) {
    FILE *f = fopen(filepath, "rb");
    if (!f) {
        fprintf(stderr, "Error: Failed to open input file '%s'\n", filepath);
        return NULL;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size < 0) {
        fclose(f);
        fprintf(stderr, "Error: Failed to determine file size '%s'\n", filepath);
        return NULL;
    }

    char *buffer = (char *)arena_alloc(arena, (size_t)size + 1);
    if (!buffer) {
        fclose(f);
        fprintf(stderr, "Error: Memory allocation failed for file buffer\n");
        return NULL;
    }

    size_t read_bytes = fread(buffer, 1, (size_t)size, f);
    fclose(f);

    buffer[read_bytes] = '\0';
    return buffer;
}

static bool str_ends_with(const char *str, const char *suffix) {
    if (!str || !suffix) return false;
    size_t str_len = strlen(str);
    size_t suf_len = strlen(suffix);
    if (suf_len > str_len) return false;
    return (strcmp(str + str_len - suf_len, suffix) == 0);
}

int main(int argc, char **argv) {
    const char *input_file = NULL;
    const char *inline_code = NULL;
    const char *output_file = "a.out.o";
    bool do_disasm = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--version") == 0) {
            printf("tiny-xtensa-cc v1.0.0 (Target: Xtensa LX6/LX7 CALL0 ABI)\n");
            return 0;
        } else if (strcmp(argv[i], "-o") == 0) {
            if (i + 1 < argc) {
                output_file = argv[++i];
            } else {
                fprintf(stderr, "Error: Option '-o' requires an argument\n");
                return 1;
            }
        } else if (strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "-s") == 0 || strcmp(argv[i], "-e") == 0) {
            if (i + 1 < argc) {
                inline_code = argv[++i];
            } else {
                fprintf(stderr, "Error: Option '%s' requires a code string\n", argv[i]);
                return 1;
            }
        } else if (strncmp(argv[i], "-I", 2) == 0) {
            const char *inc_dir = argv[i] + 2;
            if (*inc_dir == '\0' && i + 1 < argc) {
                inc_dir = argv[++i];
            }
            // Will add to lexer after lexer_init
        } else if (strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "--disasm") == 0) {
            do_disasm = true;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "Error: Unknown option '%s'\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        } else {
            input_file = argv[i];
        }
    }

    if (!input_file && !inline_code) {
        fprintf(stderr, "Error: No input file or code string provided\n");
        print_usage(argv[0]);
        return 1;
    }

    Arena arena;
    if (!arena_init(&arena, ARENA_SIZE)) {
        fprintf(stderr, "Error: Failed to initialize compiler arena\n");
        return 1;
    }

    const char *source_text = NULL;
    if (inline_code) {
        source_text = inline_code;
    } else {
        source_text = read_file_to_arena(&arena, input_file);
        if (!source_text) {
            arena_free(&arena);
            return 1;
        }
    }

    Lexer lexer;
    lexer_init(&lexer, source_text, &arena);
    if (input_file) {
        lexer.stack[0].filename = arena_strdup(&arena, input_file);
    }
    lexer_add_include_path(&lexer, ".");
    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "-I", 2) == 0) {
            const char *inc_dir = argv[i] + 2;
            if (*inc_dir == '\0' && i + 1 < argc) {
                inc_dir = argv[++i];
            }
            lexer_add_include_path(&lexer, inc_dir);
        }
    }

    Emitter emitter;
    if (!emit_init(&emitter, &arena, 4096)) {
        fprintf(stderr, "Error: Failed to initialize emitter\n");
        arena_free(&arena);
        return 1;
    }

    Parser parser;
    parser_init(&parser, &lexer, &emitter, &arena);

    CompiledFunction *functions = NULL;
    size_t function_count = 0;

    if (!compile_translation_unit(&parser, &functions, &function_count)) {
        fprintf(stderr, "Compilation failed:\n%s\n", parser.error_msg);
        arena_free(&arena);
        return 1;
    }

    if (function_count == 0) {
        fprintf(stderr, "Warning: No functions compiled\n");
    }

    bool is_elf_output = str_ends_with(output_file, ".o") || !str_ends_with(output_file, ".bin");

    if (is_elf_output) {
        // Build ELF32 Relocatable Object
        ElfWriter writer;
        elf_writer_init(&writer, &arena);

        // Add File Symbol
        const char *fname = input_file ? input_file : "input.c";
        elf_writer_add_symbol(&writer, fname, 0, 0, STT_FILE, STB_LOCAL, SHN_ABS);

        // Add Section Symbols
        elf_writer_add_symbol(&writer, "", 0, 0, STT_SECTION, STB_LOCAL, 1);
        elf_writer_add_symbol(&writer, "", 0, 0, STT_SECTION, STB_LOCAL, 2);

        // Calculate total text length
        size_t total_text_len = 0;
        for (size_t i = 0; i < function_count; i++) {
            total_text_len += (functions[i].code_size + 3) & ~3;
        }

        uint8_t *text_buf = (uint8_t *)arena_alloc(&arena, total_text_len ? total_text_len : 4);
        if (text_buf && total_text_len > 0) {
            memset(text_buf, 0, total_text_len);
        }

        size_t curr_pos = 0;
        for (size_t i = 0; i < function_count; i++) {
            if (functions[i].code_size > 0) {
                size_t aligned_pos = (curr_pos + 3) & ~3;
                if (text_buf) {
                    memcpy(text_buf + aligned_pos, functions[i].code, functions[i].code_size);
                }
                elf_writer_add_symbol(&writer, functions[i].name, (uint32_t)aligned_pos, (uint32_t)functions[i].code_size, STT_FUNC, STB_GLOBAL, 1);

                // Add relocations for this function
                for (size_t r = 0; r < functions[i].reloc_count; r++) {
                    elf_writer_add_reloc(&writer, aligned_pos + functions[i].relocs[r].inst_offset,
                                         functions[i].relocs[r].sym_name,
                                         functions[i].relocs[r].type,
                                         functions[i].relocs[r].addend);
                }

                curr_pos = aligned_pos + functions[i].code_size;
            }
        }

        elf_writer_set_text(&writer, text_buf, total_text_len);

        if (!elf_writer_write_file(&writer, output_file)) {
            fprintf(stderr, "Error: Failed to write ELF object '%s'\n", output_file);
            arena_free(&arena);
            return 1;
        }

        size_t elf_size = 0;
        elf_writer_emit(&writer, &elf_size);
        printf("Compiled %zu function(s) -> %s (ELF32 Relocatable Object, %zu bytes)\n",
               function_count, output_file, elf_size);

        if (do_disasm) {
            printf("\n--- Disassembly (%s) ---\n", output_file);
            char cmd[1024];
            snprintf(cmd, sizeof(cmd),
                     "%s -d %s 2>&1",
                     get_objdump_path(), output_file);
            int sys_ret = system(cmd);
            (void)sys_ret;
        }
    } else {
        // Write raw binary
        FILE *out = fopen(output_file, "wb");
        if (!out) {
            fprintf(stderr, "Error: Failed to open output file '%s' for writing\n", output_file);
            arena_free(&arena);
            return 1;
        }

        size_t total_written = 0;
        for (size_t i = 0; i < function_count; i++) {
            if (functions[i].code_size > 0) {
                size_t written = fwrite(functions[i].code, 1, functions[i].code_size, out);
                total_written += written;
            }
        }
        fclose(out);

        printf("Compiled %zu function(s) -> %s (%zu bytes)\n",
               function_count, output_file, total_written);

        if (do_disasm && total_written > 0) {
            printf("\n--- Disassembly (%s) ---\n", output_file);
            char cmd[1024];
            snprintf(cmd, sizeof(cmd),
                     "%s -D -b binary -m xtensa %s 2>&1",
                     get_objdump_path(), output_file);
            int sys_ret = system(cmd);
            (void)sys_ret;
        }
    }

    arena_free(&arena);
    return 0;
}

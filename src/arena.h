#ifndef ARENA_H
#define ARENA_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define ARENA_ALIGNMENT 8

typedef struct Arena {
    uint8_t *buffer;
    size_t capacity;
    size_t offset;
} Arena;

/**
 * Initialize a bump memory arena with a fixed capacity.
 * Returns true on success, false if malloc fails or capacity is 0.
 */
bool arena_init(Arena *a, size_t capacity);

/**
 * Allocate `size` bytes from the arena with 8-byte alignment.
 * Returns aligned pointer, or NULL if capacity is exceeded.
 */
void *arena_alloc(Arena *a, size_t size);

/**
 * Allocate `size` bytes zero-initialized from the arena.
 */
void *arena_alloc_zero(Arena *a, size_t size);

/**
 * Duplicate a string of length `n` into the arena, null-terminating it.
 */
char *arena_strndup(Arena *a, const char *s, size_t n);

/**
 * Duplicate a null-terminated string into the arena.
 */
char *arena_strdup(Arena *a, const char *s);

/**
 * Reset the arena allocation offset back to 0 without freeing memory.
 */
void arena_reset(Arena *a);

/**
 * Free the backing memory of the arena and zero its state.
 */
void arena_free(Arena *a);

#endif // ARENA_H

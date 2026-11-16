#include "arena.h"
#include <stdlib.h>
#include <string.h>

#define ALIGN_UP(n, align) (((n) + ((align) - 1)) & ~((align) - 1))

bool arena_init(Arena *a, size_t capacity) {
    if (!a || capacity == 0) {
        return false;
    }
    a->buffer = (uint8_t *)malloc(capacity);
    if (!a->buffer) {
        a->capacity = 0;
        a->offset = 0;
        return false;
    }
    a->capacity = capacity;
    a->offset = 0;
    return true;
}

void *arena_alloc(Arena *a, size_t size) {
    if (!a || !a->buffer || size == 0) {
        return NULL;
    }

    size_t aligned_offset = ALIGN_UP(a->offset, ARENA_ALIGNMENT);

    // Overflow check
    if (aligned_offset > a->capacity || size > a->capacity - aligned_offset) {
        return NULL;
    }

    void *ptr = (void *)(a->buffer + aligned_offset);
    a->offset = aligned_offset + size;
    return ptr;
}

void *arena_alloc_zero(Arena *a, size_t size) {
    void *ptr = arena_alloc(a, size);
    if (ptr) {
        memset(ptr, 0, size);
    }
    return ptr;
}

char *arena_strndup(Arena *a, const char *s, size_t n) {
    if (!a || !s) {
        return NULL;
    }
    char *copy = (char *)arena_alloc(a, n + 1);
    if (!copy) {
        return NULL;
    }
    memcpy(copy, s, n);
    copy[n] = '\0';
    return copy;
}

char *arena_strdup(Arena *a, const char *s) {
    if (!s) {
        return NULL;
    }
    return arena_strndup(a, s, strlen(s));
}

void arena_reset(Arena *a) {
    if (a) {
        a->offset = 0;
    }
}

void arena_free(Arena *a) {
    if (a) {
        if (a->buffer) {
            free(a->buffer);
            a->buffer = NULL;
        }
        a->capacity = 0;
        a->offset = 0;
    }
}

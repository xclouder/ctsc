#include "ctsc/arena.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct CtscArenaBlock {
    struct CtscArenaBlock* next;
    size_t                 capacity;
    size_t                 used;
    unsigned char          data[];
};

static CtscArenaBlock* block_new(size_t capacity) {
    CtscArenaBlock* b = (CtscArenaBlock*)malloc(sizeof(CtscArenaBlock) + capacity);
    if (!b) { CTSC_PANIC("arena: out of memory (%zu bytes)", capacity); }
    b->next = NULL;
    b->capacity = capacity;
    b->used = 0;
    return b;
}

void ctsc_arena_init(CtscArena* a, size_t default_block_size) {
    a->head = NULL;
    a->default_block_size = default_block_size ? default_block_size : (64 * 1024);
    a->total_allocated = 0;
}

void ctsc_arena_free(CtscArena* a) {
    CtscArenaBlock* b = a->head;
    while (b) {
        CtscArenaBlock* n = b->next;
        free(b);
        b = n;
    }
    a->head = NULL;
    a->total_allocated = 0;
}

static size_t align_up(size_t v, size_t a) {
    return (v + (a - 1)) & ~(a - 1);
}

void* ctsc_arena_alloc_aligned(CtscArena* a, size_t size, size_t align) {
    if (size == 0) { size = 1; }
    CtscArenaBlock* b = a->head;
    if (b) {
        size_t aligned = align_up(b->used, align);
        if (aligned + size <= b->capacity) {
            void* p = b->data + aligned;
            b->used = aligned + size;
            a->total_allocated += size;
            return p;
        }
    }
    size_t cap = a->default_block_size;
    if (size + align > cap) { cap = size + align; }
    CtscArenaBlock* nb = block_new(cap);
    nb->next = a->head;
    a->head = nb;
    size_t aligned = align_up(0, align);
    nb->used = aligned + size;
    a->total_allocated += size;
    return nb->data + aligned;
}

void* ctsc_arena_alloc(CtscArena* a, size_t size) {
    return ctsc_arena_alloc_aligned(a, size, sizeof(void*));
}

void* ctsc_arena_calloc(CtscArena* a, size_t count, size_t elem_size) {
    size_t total = count * elem_size;
    void* p = ctsc_arena_alloc(a, total);
    memset(p, 0, total);
    return p;
}

char* ctsc_arena_strdup(CtscArena* a, const char* s) {
    return ctsc_arena_strndup(a, s, strlen(s));
}

char* ctsc_arena_strndup(CtscArena* a, const char* s, size_t n) {
    char* p = (char*)ctsc_arena_alloc_aligned(a, n + 1, 1);
    memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

CTSC_NORETURN void ctsc_panic(const char* file, int line, const char* fmt, ...) {
    fprintf(stderr, "[ctsc panic] %s:%d: ", file, line);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
    abort();
}

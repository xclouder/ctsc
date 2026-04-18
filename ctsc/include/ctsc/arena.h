#ifndef CTSC_ARENA_H
#define CTSC_ARENA_H

#include "common.h"

typedef struct CtscArenaBlock CtscArenaBlock;

typedef struct CtscArena {
    CtscArenaBlock* head;
    size_t          default_block_size;
    size_t          total_allocated;
} CtscArena;

void  ctsc_arena_init(CtscArena* a, size_t default_block_size);
void  ctsc_arena_free(CtscArena* a);
void* ctsc_arena_alloc(CtscArena* a, size_t size);
void* ctsc_arena_alloc_aligned(CtscArena* a, size_t size, size_t align);
void* ctsc_arena_calloc(CtscArena* a, size_t count, size_t elem_size);
char* ctsc_arena_strdup(CtscArena* a, const char* s);
char* ctsc_arena_strndup(CtscArena* a, const char* s, size_t n);

#endif

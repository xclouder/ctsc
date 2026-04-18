#include "ctsc/arena.h"
#include <stdio.h>
#include <string.h>

#define EXPECT(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failed++; } } while (0)

int test_arena(void) {
    int failed = 0;
    CtscArena a; ctsc_arena_init(&a, 128);
    int* p1 = (int*)ctsc_arena_alloc(&a, sizeof(int) * 4);
    EXPECT(p1 != NULL);
    for (int i = 0; i < 4; ++i) { p1[i] = i; }
    char* s = ctsc_arena_strdup(&a, "hello");
    EXPECT(s != NULL);
    EXPECT(strcmp(s, "hello") == 0);
    void* big = ctsc_arena_alloc(&a, 4096);
    EXPECT(big != NULL);
    ctsc_arena_free(&a);
    return failed;
}

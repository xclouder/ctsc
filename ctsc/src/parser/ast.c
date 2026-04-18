#include "ctsc/ast.h"
#include "ctsc/arena.h"

#include <string.h>

CtscNode* ctsc_node_new(CtscArena* a, CtscSyntaxKind kind, int pos, int end) {
    CtscNode* n = (CtscNode*)ctsc_arena_calloc(a, 1, sizeof(CtscNode));
    n->kind = kind;
    n->pos = pos;
    n->end = end;
    return n;
}

void ctsc_node_array_init(CtscNodeArray* arr) {
    arr->items = NULL;
    arr->len = 0;
    arr->cap = 0;
}

void ctsc_node_array_push(CtscNodeArray* arr, CtscArena* a, CtscNode* n) {
    if (arr->len + 1 > arr->cap) {
        size_t ncap = arr->cap ? arr->cap * 2 : 4;
        CtscNode** nb = (CtscNode**)ctsc_arena_alloc(a, ncap * sizeof(CtscNode*));
        if (arr->items) { memcpy(nb, arr->items, arr->len * sizeof(CtscNode*)); }
        arr->items = nb;
        arr->cap = ncap;
    }
    arr->items[arr->len++] = n;
}

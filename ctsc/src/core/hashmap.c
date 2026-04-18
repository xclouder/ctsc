#include "ctsc/hashmap.h"

#include <stdlib.h>
#include <string.h>

#define INITIAL_CAPACITY 16

static void grow(CtscHashMap* m, size_t new_cap);

void ctsc_hashmap_init(CtscHashMap* m) {
    m->slots = NULL;
    m->capacity = 0;
    m->count = 0;
    m->tombstones = 0;
}

void ctsc_hashmap_free(CtscHashMap* m) {
    free(m->slots);
    m->slots = NULL;
    m->capacity = 0;
    m->count = 0;
    m->tombstones = 0;
}

static size_t find_slot(const CtscHashMapSlot* slots, size_t cap, CtscStringRef key, size_t hash) {
    size_t mask = cap - 1;
    size_t i = hash & mask;
    size_t first_tomb = (size_t)-1;
    for (;;) {
        const CtscHashMapSlot* s = &slots[i];
        if (!s->used && !s->tombstone) {
            return (first_tomb != (size_t)-1) ? first_tomb : i;
        }
        if (s->tombstone) {
            if (first_tomb == (size_t)-1) { first_tomb = i; }
        } else if (s->hash == hash && ctsc_str_eq(s->key, key)) {
            return i;
        }
        i = (i + 1) & mask;
    }
}

static void grow(CtscHashMap* m, size_t new_cap) {
    CtscHashMapSlot* new_slots = (CtscHashMapSlot*)calloc(new_cap, sizeof(CtscHashMapSlot));
    if (!new_slots) { CTSC_PANIC("hashmap: oom %zu", new_cap); }
    for (size_t i = 0; i < m->capacity; ++i) {
        CtscHashMapSlot* s = &m->slots[i];
        if (!s->used || s->tombstone) { continue; }
        size_t j = find_slot(new_slots, new_cap, s->key, s->hash);
        new_slots[j] = *s;
        new_slots[j].tombstone = false;
    }
    free(m->slots);
    m->slots = new_slots;
    m->capacity = new_cap;
    m->tombstones = 0;
}

void ctsc_hashmap_set(CtscHashMap* m, CtscStringRef key, void* value) {
    if (m->capacity == 0) { grow(m, INITIAL_CAPACITY); }
    if ((m->count + m->tombstones) * 4 >= m->capacity * 3) {
        grow(m, m->capacity * 2);
    }
    size_t hash = ctsc_str_hash(key);
    size_t i = find_slot(m->slots, m->capacity, key, hash);
    CtscHashMapSlot* s = &m->slots[i];
    if (!s->used) {
        s->used = true;
        s->tombstone = false;
        s->key = key;
        s->hash = hash;
        m->count++;
    }
    s->value = value;
}

void* ctsc_hashmap_get(const CtscHashMap* m, CtscStringRef key) {
    if (m->capacity == 0) { return NULL; }
    size_t hash = ctsc_str_hash(key);
    size_t i = find_slot(m->slots, m->capacity, key, hash);
    const CtscHashMapSlot* s = &m->slots[i];
    if (s->used && !s->tombstone) { return s->value; }
    return NULL;
}

bool ctsc_hashmap_has(const CtscHashMap* m, CtscStringRef key) {
    if (m->capacity == 0) { return false; }
    size_t hash = ctsc_str_hash(key);
    size_t i = find_slot(m->slots, m->capacity, key, hash);
    const CtscHashMapSlot* s = &m->slots[i];
    return s->used && !s->tombstone;
}

void ctsc_hashmap_remove(CtscHashMap* m, CtscStringRef key) {
    if (m->capacity == 0) { return; }
    size_t hash = ctsc_str_hash(key);
    size_t i = find_slot(m->slots, m->capacity, key, hash);
    CtscHashMapSlot* s = &m->slots[i];
    if (s->used && !s->tombstone) {
        s->tombstone = true;
        m->count--;
        m->tombstones++;
    }
}

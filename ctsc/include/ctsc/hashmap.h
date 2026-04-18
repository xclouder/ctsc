#ifndef CTSC_HASHMAP_H
#define CTSC_HASHMAP_H

#include "common.h"
#include "string_ref.h"

typedef struct {
    CtscStringRef key;
    void*         value;
    size_t        hash;
    bool          used;
    bool          tombstone;
} CtscHashMapSlot;

typedef struct {
    CtscHashMapSlot* slots;
    size_t           capacity;
    size_t           count;
    size_t           tombstones;
} CtscHashMap;

void  ctsc_hashmap_init(CtscHashMap* m);
void  ctsc_hashmap_free(CtscHashMap* m);
void  ctsc_hashmap_set(CtscHashMap* m, CtscStringRef key, void* value);
void* ctsc_hashmap_get(const CtscHashMap* m, CtscStringRef key);
bool  ctsc_hashmap_has(const CtscHashMap* m, CtscStringRef key);
void  ctsc_hashmap_remove(CtscHashMap* m, CtscStringRef key);

#endif

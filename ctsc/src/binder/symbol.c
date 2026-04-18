#include "ctsc/symbol.h"
#include "ctsc/arena.h"

#include <string.h>

void ctsc_symbol_table_init(CtscSymbolTable* t) {
    t->items = NULL;
    t->len = 0;
    t->cap = 0;
}

static bool utf16_equals(const uint16_t* a, size_t alen, const uint16_t* b, size_t blen) {
    if (alen != blen) return false;
    if (alen == 0) return true;
    return memcmp(a, b, alen * sizeof(uint16_t)) == 0;
}

CtscSymbol* ctsc_symbol_table_find(const CtscSymbolTable* t, const uint16_t* name, size_t name_len) {
    for (size_t i = 0; i < t->len; ++i) {
        CtscSymbol* s = t->items[i];
        if (utf16_equals(s->name, s->name_len, name, name_len)) return s;
    }
    return NULL;
}

static void table_grow(CtscSymbolTable* t, CtscArena* a) {
    size_t ncap = t->cap ? t->cap * 2 : 4;
    CtscSymbol** nb = (CtscSymbol**)ctsc_arena_alloc(a, ncap * sizeof(CtscSymbol*));
    if (t->items) memcpy(nb, t->items, t->len * sizeof(CtscSymbol*));
    t->items = nb;
    t->cap = ncap;
}

CtscSymbol* ctsc_symbol_table_get_or_create(CtscSymbolTable* t, CtscArena* a,
                                            const uint16_t* name, size_t name_len) {
    CtscSymbol* existing = ctsc_symbol_table_find(t, name, name_len);
    if (existing) return existing;
    if (t->len + 1 > t->cap) table_grow(t, a);
    CtscSymbol* s = (CtscSymbol*)ctsc_arena_calloc(a, 1, sizeof(CtscSymbol));
    s->name = name;
    s->name_len = name_len;
    t->items[t->len++] = s;
    return s;
}

void ctsc_symbol_add_declaration(CtscSymbol* s, CtscArena* a, CtscNode* decl) {
    if (s->decls_len + 1 > s->decls_cap) {
        size_t ncap = s->decls_cap ? s->decls_cap * 2 : 2;
        CtscNode** nb = (CtscNode**)ctsc_arena_alloc(a, ncap * sizeof(CtscNode*));
        if (s->decls) memcpy(nb, s->decls, s->decls_len * sizeof(CtscNode*));
        s->decls = nb;
        s->decls_cap = ncap;
    }
    s->decls[s->decls_len++] = decl;
}

/* Atomic ts.SymbolFlags names, mirrored from
 * upstream/TypeScript/src/compiler/types.ts ~5942. Only single-bit flags
 * appear here; oracle-binder.ts filters out composite aliases. Extend this
 * table in lockstep with CtscSymbolFlag. */
const char* ctsc_symbol_flag_name(CtscSymbolFlag f) {
    switch (f) {
        case CTSC_SYMBOL_FLAG_FunctionScopedVariable: return "FunctionScopedVariable";
        case CTSC_SYMBOL_FLAG_BlockScopedVariable:    return "BlockScopedVariable";
        case CTSC_SYMBOL_FLAG_Property:               return "Property";
        case CTSC_SYMBOL_FLAG_EnumMember:             return "EnumMember";
        case CTSC_SYMBOL_FLAG_Function:               return "Function";
        case CTSC_SYMBOL_FLAG_Class:                  return "Class";
        case CTSC_SYMBOL_FLAG_Interface:              return "Interface";
        case CTSC_SYMBOL_FLAG_ConstEnum:              return "ConstEnum";
        case CTSC_SYMBOL_FLAG_RegularEnum:            return "RegularEnum";
        case CTSC_SYMBOL_FLAG_ValueModule:            return "ValueModule";
        case CTSC_SYMBOL_FLAG_NamespaceModule:        return "NamespaceModule";
        case CTSC_SYMBOL_FLAG_TypeLiteral:            return "TypeLiteral";
        case CTSC_SYMBOL_FLAG_ObjectLiteral:          return "ObjectLiteral";
        case CTSC_SYMBOL_FLAG_Method:                 return "Method";
        case CTSC_SYMBOL_FLAG_Constructor:            return "Constructor";
        case CTSC_SYMBOL_FLAG_GetAccessor:            return "GetAccessor";
        case CTSC_SYMBOL_FLAG_SetAccessor:            return "SetAccessor";
        case CTSC_SYMBOL_FLAG_Signature:              return "Signature";
        case CTSC_SYMBOL_FLAG_TypeParameter:          return "TypeParameter";
        case CTSC_SYMBOL_FLAG_TypeAlias:              return "TypeAlias";
        case CTSC_SYMBOL_FLAG_ExportValue:            return "ExportValue";
        case CTSC_SYMBOL_FLAG_Alias:                  return "Alias";
        case CTSC_SYMBOL_FLAG_Prototype:              return "Prototype";
        case CTSC_SYMBOL_FLAG_ExportStar:             return "ExportStar";
        case CTSC_SYMBOL_FLAG_Optional:               return "Optional";
        case CTSC_SYMBOL_FLAG_Transient:              return "Transient";
        case CTSC_SYMBOL_FLAG_Assignment:             return "Assignment";
        case CTSC_SYMBOL_FLAG_ModuleExports:          return "ModuleExports";
        default:                                      return NULL;
    }
}

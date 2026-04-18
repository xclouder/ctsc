#ifndef CTSC_SYMBOL_H
#define CTSC_SYMBOL_H

#include "common.h"
#include "ast.h"

/*
 * Mirrors a growing subset of ts.SymbolFlags from
 * upstream/TypeScript/src/compiler/types.ts (~5942 `const enum SymbolFlags`).
 *
 * Only *atomic* (single-bit) flags are represented — composite aliases like
 * Value/Variable/Type/Namespace are intentionally omitted because
 * harness/src/oracle-binder.ts (atomicSymbolFlags / flagsNames) filters to
 * single-bit flags when serialising.
 *
 * Bit positions mirror tsc's numeric values so ctsc_symbol_emit_flag_names()
 * can iterate in the same ascending-by-value order that the oracle uses.
 * Add new flags here when a fixture demands them.
 */
typedef enum {
    CTSC_SYMBOL_FLAG_NONE                   = 0,
    CTSC_SYMBOL_FLAG_FunctionScopedVariable = 1 << 0,
    CTSC_SYMBOL_FLAG_BlockScopedVariable    = 1 << 1,
    CTSC_SYMBOL_FLAG_Property               = 1 << 2,
    CTSC_SYMBOL_FLAG_EnumMember             = 1 << 3,
    CTSC_SYMBOL_FLAG_Function               = 1 << 4,
    CTSC_SYMBOL_FLAG_Class                  = 1 << 5,
    CTSC_SYMBOL_FLAG_Interface              = 1 << 6,
    CTSC_SYMBOL_FLAG_ConstEnum              = 1 << 7,
    CTSC_SYMBOL_FLAG_RegularEnum            = 1 << 8,
    CTSC_SYMBOL_FLAG_ValueModule            = 1 << 9,
    CTSC_SYMBOL_FLAG_NamespaceModule        = 1 << 10,
    CTSC_SYMBOL_FLAG_TypeLiteral            = 1 << 11,
    CTSC_SYMBOL_FLAG_ObjectLiteral          = 1 << 12,
    CTSC_SYMBOL_FLAG_Method                 = 1 << 13,
    CTSC_SYMBOL_FLAG_Constructor            = 1 << 14,
    CTSC_SYMBOL_FLAG_GetAccessor            = 1 << 15,
    CTSC_SYMBOL_FLAG_SetAccessor            = 1 << 16,
    CTSC_SYMBOL_FLAG_Signature              = 1 << 17,
    CTSC_SYMBOL_FLAG_TypeParameter          = 1 << 18,
    CTSC_SYMBOL_FLAG_TypeAlias              = 1 << 19,
    CTSC_SYMBOL_FLAG_ExportValue            = 1 << 20,
    CTSC_SYMBOL_FLAG_Alias                  = 1 << 21,
    CTSC_SYMBOL_FLAG_Prototype              = 1 << 22,
    CTSC_SYMBOL_FLAG_ExportStar             = 1 << 23,
    CTSC_SYMBOL_FLAG_Optional               = 1 << 24,
    CTSC_SYMBOL_FLAG_Transient              = 1 << 25,
    CTSC_SYMBOL_FLAG_Assignment             = 1 << 26,
    CTSC_SYMBOL_FLAG_ModuleExports          = 1 << 27
} CtscSymbolFlag;

typedef unsigned int CtscSymbolFlags;

typedef struct CtscSymbol {
    /* Symbol name as UTF-16 code units. Owned by the arena or the source
     * UTF-16 buffer (for identifiers backed by scanner-owned memory). */
    const uint16_t* name;
    size_t          name_len;
    CtscSymbolFlags flags;
    /* Declaration AST nodes (insertion order matches tsc's binder.ts
     * addDeclarationToSymbol). */
    CtscNode**      decls;
    size_t          decls_len;
    size_t          decls_cap;
} CtscSymbol;

typedef struct CtscSymbolTable {
    CtscSymbol** items;
    size_t       len;
    size_t       cap;
} CtscSymbolTable;

struct CtscArena;

void        ctsc_symbol_table_init(CtscSymbolTable* t);
CtscSymbol* ctsc_symbol_table_find(const CtscSymbolTable* t, const uint16_t* name, size_t name_len);
CtscSymbol* ctsc_symbol_table_get_or_create(CtscSymbolTable* t, struct CtscArena* a,
                                            const uint16_t* name, size_t name_len);
void        ctsc_symbol_add_declaration(CtscSymbol* s, struct CtscArena* a, CtscNode* decl);

/* Canonical ts.SymbolFlags name for an atomic flag value, or NULL. */
const char* ctsc_symbol_flag_name(CtscSymbolFlag f);

#endif

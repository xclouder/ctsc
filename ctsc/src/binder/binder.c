#include "ctsc/binder.h"
#include "ctsc/arena.h"
#include "ctsc/json_writer.h"
#include "ctsc/parser.h"
#include "ctsc/scanner.h"

#include <string.h>

/*
 * Phase 3 binder — minimal DFS walk that mirrors
 * upstream/TypeScript/src/compiler/binder.ts (bindSourceFile ~571 /
 * bindContainer ~953 / declareSymbol ~315). We currently recognise only the
 * smallest set of declarations needed by the active fixtures; new AST kinds
 * are handled in bind_node() as the harness unlocks them.
 *
 * The result is a flat, pre-order list of CtscScope entries (one per
 * container that owns a `locals` SymbolTable). That matches the traversal
 * shape of harness/src/oracle-binder.ts buildBindingsJson, which walks tsc's
 * SourceFile with ts.forEachChild and records every node whose `locals` is
 * defined.
 */

static void scopes_push(CtscBindResult* r, CtscArena* a, CtscScope* s) {
    if (r->scopes_len + 1 > r->scopes_cap) {
        size_t ncap = r->scopes_cap ? r->scopes_cap * 2 : 4;
        CtscScope** nb = (CtscScope**)ctsc_arena_alloc(a, ncap * sizeof(CtscScope*));
        if (r->scopes) memcpy(nb, r->scopes, r->scopes_len * sizeof(CtscScope*));
        r->scopes = nb;
        r->scopes_cap = ncap;
    }
    r->scopes[r->scopes_len++] = s;
}

static CtscScope* scope_new(CtscArena* a, const CtscNode* node) {
    CtscScope* s = (CtscScope*)ctsc_arena_calloc(a, 1, sizeof(CtscScope));
    s->node = node;
    ctsc_symbol_table_init(&s->locals);
    return s;
}

/*
 * Depth-first pre-order walk.
 *
 *   `container_scope`   — nearest IsContainer (SourceFile / function-like /
 *                         module) whose locals receive FunctionScopedVariable
 *                         (`var`) declarations and named FunctionDeclarations
 *                         (non-strict mode). Mirrors binder.ts `container`.
 *   `block_scope`       — nearest IsBlockScopedContainer (block-scope
 *                         container / ForStatement / CatchClause / CaseBlock
 *                         / ...). Receives BlockScopedVariable (`let`/`const`)
 *                         declarations. Mirrors binder.ts `blockScopeContainer`.
 *
 * For top-level code both point at the SourceFile. Inside a FunctionDeclaration
 * the function itself is the container AND its body is not a separate block-
 * scope container (see binder.ts getContainerFlags ~3882: a Block whose parent
 * is function-like returns ContainerFlags.None). Any deeper Block becomes a
 * new block-scope container.
 */
static void bind_node(CtscBindResult* r, CtscArena* a,
                      CtscScope* container_scope, CtscScope* block_scope,
                      const CtscNode* node);

static void bind_children(CtscBindResult* r, CtscArena* a,
                          CtscScope* container_scope, CtscScope* block_scope,
                          const CtscNodeArray* arr) {
    for (size_t i = 0; i < arr->len; ++i) bind_node(r, a, container_scope, block_scope, arr->items[i]);
}

/*
 * Declare `node` as a symbol named `name` with `flags` in `scope->locals`.
 * Mirrors upstream/TypeScript/src/compiler/binder.ts declareSymbol (~749) in
 * the minimal "first declaration wins, subsequent declarations are appended"
 * shape we currently need. We do NOT yet implement excludes / merge-conflict
 * diagnostics; the active fixtures only declare each name once per scope.
 */
static void declare_symbol_in_scope(CtscArena* a, CtscScope* scope,
                                    const CtscNode* name_ident,
                                    CtscSymbolFlags flags,
                                    const CtscNode* decl) {
    if (!scope || !name_ident) return;
    if (name_ident->kind != CTSC_SK_Identifier) return;
    const uint16_t* nm = name_ident->data.identifier.text;
    size_t nl = name_ident->data.identifier.text_len;
    if (!nm || nl == 0) return;
    CtscSymbol* sym = ctsc_symbol_table_get_or_create(&scope->locals, a, nm, nl);
    sym->flags |= flags;
    ctsc_symbol_add_declaration(sym, a, (CtscNode*)decl);
}

static void declare_binding_pattern_recursive(CtscArena* a, CtscScope* scope, CtscSymbolFlags flags,
                                              const CtscNode* pattern);
static void declare_binding_element_recursive(CtscArena* a, CtscScope* scope, CtscSymbolFlags flags,
                                              const CtscNode* be_node) {
    if (!be_node || be_node->kind != CTSC_SK_BindingElement) return;
    const CtscBindingElementData* bed = &be_node->data.bindingElement;
    if (bed->has_dotdotdot) {
        const CtscNode* nm = bed->name;
        if (nm && (nm->kind == CTSC_SK_ObjectBindingPattern || nm->kind == CTSC_SK_ArrayBindingPattern)) {
            declare_binding_pattern_recursive(a, scope, flags, nm);
        } else if (nm && nm->kind == CTSC_SK_Identifier) {
            declare_symbol_in_scope(a, scope, nm, flags, be_node);
        }
        return;
    }
    const CtscNode* nm = bed->name;
    if (!nm) return;
    if (nm->kind == CTSC_SK_ObjectBindingPattern || nm->kind == CTSC_SK_ArrayBindingPattern) {
        declare_binding_pattern_recursive(a, scope, flags, nm);
    } else if (nm->kind == CTSC_SK_Identifier) {
        declare_symbol_in_scope(a, scope, nm, flags, be_node);
    }
}

static void declare_binding_pattern_recursive(CtscArena* a, CtscScope* scope, CtscSymbolFlags flags,
                                              const CtscNode* pattern) {
    if (!pattern) return;
    if (pattern->kind != CTSC_SK_ObjectBindingPattern && pattern->kind != CTSC_SK_ArrayBindingPattern) return;
    const CtscNodeArray* el = &pattern->data.bindingPattern.elements;
    for (size_t i = 0; i < el->len; i++) {
        CtscNode* e = el->items[i];
        if (!e) continue;
        if (e->kind == CTSC_SK_OmittedExpression) continue;
        if (e->kind == CTSC_SK_BindingElement) declare_binding_element_recursive(a, scope, flags, e);
    }
}

/*
 * VariableDeclaration / Parameter name: Identifier or binding pattern.
 * Inner identifiers use BindingElement as the declaration node (binder.ts
 * bindVariableDeclarationOrBindingElement ~3648).
 */
static void declare_variable_like_name(CtscArena* a, CtscScope* scope, CtscSymbolFlags sym_flag,
                                       const CtscNode* name_node, const CtscNode* owner_decl) {
    if (!name_node) return;
    if (name_node->kind == CTSC_SK_Identifier) {
        declare_symbol_in_scope(a, scope, name_node, sym_flag, owner_decl);
    } else if (name_node->kind == CTSC_SK_ObjectBindingPattern
               || name_node->kind == CTSC_SK_ArrayBindingPattern) {
        declare_binding_pattern_recursive(a, scope, sym_flag, name_node);
    }
}

static void bind_node(CtscBindResult* r, CtscArena* a,
                      CtscScope* container_scope, CtscScope* block_scope,
                      const CtscNode* node) {
    if (!node) return;
    switch (node->kind) {
        case CTSC_SK_SourceFile: {
            /* The SourceFile is simultaneously the outermost container and
             * block-scope container (binder.ts getContainerFlags ~3839:
             * IsContainer | HasLocals | IsBlockScopedContainer for scripts). */
            CtscScope* s = scope_new(a, node);
            scopes_push(r, a, s);
            bind_children(r, a, s, s, &node->data.sourceFile.statements);
            break;
        }
        case CTSC_SK_FunctionDeclaration: {
            /* Mirrors binder.ts bindFunctionDeclaration (~3709): in non-strict
             * mode, declare the function name as SymbolFlags.Function in the
             * enclosing container. ctsc does not yet track strict mode / ES
             * modules, so we always follow the non-strict branch — the active
             * binder fixtures are plain scripts without "use strict". */
            declare_symbol_in_scope(a, container_scope,
                                    node->data.functionDeclaration.name,
                                    CTSC_SYMBOL_FLAG_Function, node);
            /* The function itself is a container with its own `locals` in tsc
             * (bindContainer for ContainerFlags.IsContainer | HasLocals |
             * IsFunctionLike). It is also its own blockScopeContainer:
             * binder.ts getContainerFlags (~3882) returns ContainerFlags.None
             * for a Block whose parent is function-like, so let/const in the
             * body land in the function's locals, not a new block scope. */
            CtscScope* fn_scope = scope_new(a, node);
            scopes_push(r, a, fn_scope);
            const CtscNodeArray* params = &node->data.functionDeclaration.parameters;
            for (size_t i = 0; i < params->len; ++i) {
                bind_node(r, a, fn_scope, fn_scope, params->items[i]);
            }
            const CtscNode* body = node->data.functionDeclaration.body;
            if (body) {
                /* Visit the body Block's statements directly — the body is
                 * NOT its own block-scope container (see comment above). Any
                 * nested Block inside it will create its own block scope via
                 * the CTSC_SK_Block case below. */
                if (body->kind == CTSC_SK_Block) {
                    bind_children(r, a, fn_scope, fn_scope, &body->data.block.statements);
                } else {
                    bind_node(r, a, fn_scope, fn_scope, body);
                }
            }
            break;
        }
        case CTSC_SK_MethodDeclaration: {
            /* Mirrors binder.ts bindMethodDeclaration / bindFunctionLikeDeclaration
             * (~3720+): method is a function-like container; parameters live in its
             * locals (same Block/body rule as FunctionDeclaration). */
            CtscScope* m_scope = scope_new(a, node);
            scopes_push(r, a, m_scope);
            const CtscMethodDeclarationData* md = &node->data.methodDeclaration;
            const CtscNodeArray* params = &md->parameters;
            for (size_t i = 0; i < params->len; ++i) {
                bind_node(r, a, m_scope, m_scope, params->items[i]);
            }
            const CtscNode* body = md->body;
            if (body) {
                if (body->kind == CTSC_SK_Block) {
                    bind_children(r, a, m_scope, m_scope, &body->data.block.statements);
                } else {
                    bind_node(r, a, m_scope, m_scope, body);
                }
            }
            break;
        }
        case CTSC_SK_FunctionExpression: {
            /*
             * Mirrors bindFunctionExpression (binder.ts ~3715): container with
             * parameter locals. A named function expression's binding identifier
             * lives in the function's own locals (not the enclosing container).
             */
            const CtscFunctionDeclarationData* fd = &node->data.functionDeclaration;
            CtscScope* fn_scope = scope_new(a, node);
            scopes_push(r, a, fn_scope);
            if (fd->name) {
                declare_symbol_in_scope(a, fn_scope, fd->name, CTSC_SYMBOL_FLAG_Function, node);
            }
            const CtscNodeArray* params = &fd->parameters;
            for (size_t i = 0; i < params->len; ++i) {
                bind_node(r, a, fn_scope, fn_scope, params->items[i]);
            }
            const CtscNode* body = fd->body;
            if (body) {
                if (body->kind == CTSC_SK_Block) {
                    bind_children(r, a, fn_scope, fn_scope, &body->data.block.statements);
                } else {
                    bind_node(r, a, fn_scope, fn_scope, body);
                }
            }
            break;
        }
        case CTSC_SK_Parameter: {
            /* Mirrors binder.ts bindParameter (~3684): identifier-named
             * parameters become SymbolFlags.FunctionScopedVariable in the
             * containing function's locals. Binding patterns declare each
             * inner name (BindingElement as decl) per bindVariableDeclarationOrBindingElement
             * (~3648). */
            declare_variable_like_name(a, container_scope, CTSC_SYMBOL_FLAG_FunctionScopedVariable,
                                       node->data.parameter.name, node);
            break;
        }
        case CTSC_SK_Block: {
            /* A Block reached here is NOT directly inside a function-like
             * (that case is handled inline by CTSC_SK_FunctionDeclaration,
             * which skips re-entering the body Block). Per binder.ts
             * getContainerFlags (~3882) such a Block becomes
             * ContainerFlags.IsBlockScopedContainer | HasLocals — its own
             * block-scope container. In tsc `locals` is populated lazily by
             * bindBlockScopedDeclaration (~2437); we mirror that by always
             * pushing a scope here and filtering empty Block scopes at
             * emission time (see emit_scope). */
            CtscScope* blk_scope = scope_new(a, node);
            scopes_push(r, a, blk_scope);
            bind_children(r, a, container_scope, blk_scope, &node->data.block.statements);
            break;
        }
        case CTSC_SK_SwitchStatement:
            bind_node(r, a, container_scope, block_scope,
                      node->data.switchStatement.expression);
            bind_node(r, a, container_scope, block_scope,
                      node->data.switchStatement.caseBlock);
            break;
        case CTSC_SK_CaseBlock:
            bind_children(r, a, container_scope, block_scope,
                          &node->data.caseBlock.clauses);
            break;
        case CTSC_SK_CaseClause:
            bind_node(r, a, container_scope, block_scope,
                      node->data.caseClause.expression);
            bind_children(r, a, container_scope, block_scope,
                          &node->data.caseClause.statements);
            break;
        case CTSC_SK_DefaultClause:
            bind_children(r, a, container_scope, block_scope,
                          &node->data.defaultClause.statements);
            break;
        case CTSC_SK_ClassDeclaration: {
            /* Mirrors binder.ts bindClassDeclaration (~3720): class name is a
             * BlockLike / container-local value symbol (script top-level uses
             * the SourceFile as container). Members are bound for locals
             * (method parameters, etc.) like bindChildren on the class body. */
            const CtscNode* name = node->data.classDeclaration.name;
            if (name) {
                declare_symbol_in_scope(a, container_scope, name, CTSC_SYMBOL_FLAG_Class, node);
            }
            bind_children(r, a, container_scope, block_scope, &node->data.classDeclaration.members);
            break;
        }
        case CTSC_SK_InterfaceDeclaration: {
            /* Mirrors binder.ts bindInterfaceDeclaration (~3726): interface name
             * is a type symbol in the enclosing container (parser.ts shares
             * CtscClassDeclarationData shape with InterfaceDeclaration). */
            const CtscNode* name = node->data.classDeclaration.name;
            if (name) {
                declare_symbol_in_scope(a, container_scope, name, CTSC_SYMBOL_FLAG_Interface, node);
            }
            break;
        }
        case CTSC_SK_TypeAliasDeclaration: {
            /* Mirrors binder.ts bindTypeAliasDeclaration (~3732): alias name is
             * SymbolFlags.TypeAlias in the enclosing container. */
            const CtscNode* name = node->data.typeAliasDeclaration.name;
            if (name) {
                declare_symbol_in_scope(a, container_scope, name, CTSC_SYMBOL_FLAG_TypeAlias, node);
            }
            break;
        }
        case CTSC_SK_EnumDeclaration: {
            /* Mirrors binder.ts bindEnumDeclaration (~3642): block-scoped
             * SymbolFlags.RegularEnum (non-const enums). ctsc does not parse
             * const enums yet. */
            const CtscNode* name = node->data.enumDeclaration.name;
            if (name) {
                declare_symbol_in_scope(a, block_scope, name, CTSC_SYMBOL_FLAG_RegularEnum, node);
            }
            break;
        }
        case CTSC_SK_VariableStatement: {
            /* Mirrors binder.ts bindVariableDeclarationOrBindingElement (~3648):
             * each VariableDeclaration contributes a symbol. `let`/`const`
             * use SymbolFlags.BlockScopedVariable and go to the nearest
             * block-scope container (`block_scope`); `var` uses
             * SymbolFlags.FunctionScopedVariable and goes to the nearest
             * function/SourceFile container (`container_scope`). */
            const CtscNode* list = node->data.variableStatement.declarationList;
            if (!list || list->kind != CTSC_SK_VariableDeclarationList) break;
            int vflags = list->data.variableDeclarationList.flags;
            /* Parser encodes: bit0 = Let, bit1 = Const, else Var. */
            bool is_block_scoped = (vflags & 0x3) != 0;
            CtscSymbolFlags sym_flag = is_block_scoped
                ? CTSC_SYMBOL_FLAG_BlockScopedVariable
                : CTSC_SYMBOL_FLAG_FunctionScopedVariable;
            CtscScope* target = is_block_scoped ? block_scope : container_scope;
            const CtscNodeArray* decls = &list->data.variableDeclarationList.declarations;
            for (size_t i = 0; i < decls->len; ++i) {
                const CtscNode* d = decls->items[i];
                if (!d || d->kind != CTSC_SK_VariableDeclaration) continue;
                declare_variable_like_name(a, target, sym_flag, d->data.variableDeclaration.name, d);
            }
            /* Initializers can contain function-like containers (e.g. arrow functions)
             * whose parameters must be bound (binder.ts bind ~3648 + bindChildren). */
            for (size_t i = 0; i < decls->len; ++i) {
                const CtscNode* d = decls->items[i];
                if (!d || d->kind != CTSC_SK_VariableDeclaration) continue;
                const CtscNode* init = d->data.variableDeclaration.initializer;
                if (init) bind_node(r, a, container_scope, block_scope, init);
            }
            break;
        }
        case CTSC_SK_ArrowFunction: {
            /* Mirrors bindFunctionDeclaration / bind ~3709: arrow is a container with
             * parameter locals (binder.ts bindArrowFunction ~3765). */
            CtscScope* fn_scope = scope_new(a, node);
            scopes_push(r, a, fn_scope);
            const CtscNodeArray* params = &node->data.arrowFunction.parameters;
            for (size_t i = 0; i < params->len; ++i) {
                bind_node(r, a, fn_scope, fn_scope, params->items[i]);
            }
            const CtscNode* body = node->data.arrowFunction.body;
            if (body) {
                if (body->kind == CTSC_SK_Block) {
                    bind_children(r, a, fn_scope, fn_scope, &body->data.block.statements);
                } else {
                    bind_node(r, a, fn_scope, fn_scope, body);
                }
            }
            break;
        }
        case CTSC_SK_ForStatement: {
            /* binder.ts getContainerFlags (~3876): ForStatement is IsBlockScopedContainer |
             * HasLocals; bindForStatement (~1541) walks initializer, condition, body,
             * incrementor. */
            CtscScope* for_scope = scope_new(a, node);
            scopes_push(r, a, for_scope);
            const CtscForStatementData* fs = &node->data.forStatement;
            if (fs->initializer) {
                if (fs->initializer->kind == CTSC_SK_VariableDeclarationList) {
                    const CtscNode* list = fs->initializer;
                    int vflags = list->data.variableDeclarationList.flags;
                    bool is_block_scoped = (vflags & 0x3) != 0;
                    CtscSymbolFlags sym_flag = is_block_scoped ? CTSC_SYMBOL_FLAG_BlockScopedVariable
                                                               : CTSC_SYMBOL_FLAG_FunctionScopedVariable;
                    CtscScope* target = is_block_scoped ? for_scope : container_scope;
                    const CtscNodeArray* decls = &list->data.variableDeclarationList.declarations;
                    for (size_t i = 0; i < decls->len; ++i) {
                        const CtscNode* d = decls->items[i];
                        if (!d || d->kind != CTSC_SK_VariableDeclaration) continue;
                        declare_variable_like_name(a, target, sym_flag, d->data.variableDeclaration.name, d);
                    }
                    for (size_t i = 0; i < decls->len; ++i) {
                        const CtscNode* d = decls->items[i];
                        if (!d || d->kind != CTSC_SK_VariableDeclaration) continue;
                        const CtscNode* init = d->data.variableDeclaration.initializer;
                        if (init) bind_node(r, a, container_scope, for_scope, init);
                    }
                } else {
                    bind_node(r, a, container_scope, for_scope, fs->initializer);
                }
            }
            if (fs->condition) bind_node(r, a, container_scope, for_scope, fs->condition);
            if (fs->statement) bind_node(r, a, container_scope, for_scope, fs->statement);
            if (fs->incrementor) bind_node(r, a, container_scope, for_scope, fs->incrementor);
            break;
        }
        default:
            /* Other kinds do not introduce scopes or declarations yet. */
            break;
    }
}

CtscBindResult* ctsc_bind(const CtscNode* sourceFile, CtscArena* arena) {
    CtscBindResult* r = (CtscBindResult*)ctsc_arena_calloc(arena, 1, sizeof(CtscBindResult));
    bind_node(r, arena, NULL, NULL, sourceFile);
    return r;
}

/* ----- JSON serialisation -------------------------------------------------
 *
 * Output shape: see harness/src/oracle-binder.ts buildBindingsJson.
 *
 *   { "scopes": [ {kind, pos, end, symbols:[{name, flags, decls}]} ... ],
 *     "diagnostics": [] }
 */

static int cmp_symbol_name_utf16(const CtscSymbol* a, const CtscSymbol* b) {
    size_t n = a->name_len < b->name_len ? a->name_len : b->name_len;
    for (size_t i = 0; i < n; ++i) {
        if (a->name[i] != b->name[i]) {
            return (a->name[i] < b->name[i]) ? -1 : 1;
        }
    }
    if (a->name_len == b->name_len) return 0;
    return a->name_len < b->name_len ? -1 : 1;
}

static void sort_symbols_by_name(CtscSymbol** items, size_t n) {
    /* Insertion sort: symbol counts per scope are tiny for the fixtures we
     * expect; avoids pulling in qsort / a comparator context. Mirrors the
     * Unicode code-point order that oracle-binder.ts uses
     * (a.name < b.name ? -1 : ...). */
    for (size_t i = 1; i < n; ++i) {
        CtscSymbol* cur = items[i];
        size_t j = i;
        while (j > 0 && cmp_symbol_name_utf16(items[j - 1], cur) > 0) {
            items[j] = items[j - 1];
            --j;
        }
        items[j] = cur;
    }
}

static void emit_flags(CtscJson* j, CtscSymbolFlags flags) {
    ctsc_json_key(j, "flags");
    ctsc_json_begin_arr(j);
    /* Iterate bits in ascending numeric order, matching oracle-binder.ts
     * atomicSymbolFlags() which sorts by ts.SymbolFlags value. */
    for (unsigned bit = 0; bit < 32; ++bit) {
        CtscSymbolFlag f = (CtscSymbolFlag)(1u << bit);
        if (!(flags & f)) continue;
        const char* name = ctsc_symbol_flag_name(f);
        if (!name) continue;
        ctsc_json_cstr(j, name);
    }
    ctsc_json_end_arr(j);
}

static void emit_decls(CtscJson* j, CtscSymbol* s) {
    ctsc_json_key(j, "decls");
    ctsc_json_begin_arr(j);
    for (size_t i = 0; i < s->decls_len; ++i) {
        const CtscNode* d = s->decls[i];
        ctsc_json_begin_obj(j);
        ctsc_json_key(j, "kind"); ctsc_json_cstr(j, ctsc_syntax_kind_name(d->kind));
        ctsc_json_key(j, "pos");  ctsc_json_int(j, d->pos);
        ctsc_json_key(j, "end");  ctsc_json_int(j, d->end);
        ctsc_json_end_obj(j);
    }
    ctsc_json_end_arr(j);
}

static void emit_symbol(CtscJson* j, CtscSymbol* s) {
    ctsc_json_begin_obj(j);
    ctsc_json_key(j, "name");
    ctsc_json_str_utf16(j, s->name, s->name_len);
    emit_flags(j, s->flags);
    emit_decls(j, s);
    ctsc_json_end_obj(j);
}

static void emit_scope(CtscJson* j, CtscArena* a, const CtscScope* sc) {
    ctsc_json_begin_obj(j);
    ctsc_json_key(j, "kind"); ctsc_json_cstr(j, ctsc_syntax_kind_name(sc->node->kind));
    ctsc_json_key(j, "pos");  ctsc_json_int(j, sc->node->pos);
    ctsc_json_key(j, "end");  ctsc_json_int(j, sc->node->end);
    ctsc_json_key(j, "symbols");
    ctsc_json_begin_arr(j);
    /* Sort a shallow copy so the binder's insertion order is preserved for
     * internal consumers (future checker) while the JSON emits sorted. */
    if (sc->locals.len > 0) {
        CtscSymbol** copy = (CtscSymbol**)ctsc_arena_alloc(a, sc->locals.len * sizeof(CtscSymbol*));
        memcpy(copy, sc->locals.items, sc->locals.len * sizeof(CtscSymbol*));
        sort_symbols_by_name(copy, sc->locals.len);
        for (size_t i = 0; i < sc->locals.len; ++i) emit_symbol(j, copy[i]);
    }
    ctsc_json_end_arr(j);
    ctsc_json_end_obj(j);
}

void ctsc_bindings_dump_json(const CtscNode* sourceFile, CtscBuffer* out, bool pretty) {
    CtscArena a; ctsc_arena_init(&a, 16 * 1024);
    CtscBindResult* r = ctsc_bind(sourceFile, &a);

    CtscJson j; ctsc_json_init(&j, out, pretty);
    ctsc_json_begin_obj(&j);
    ctsc_json_key(&j, "scopes");
    ctsc_json_begin_arr(&j);
    for (size_t i = 0; i < r->scopes_len; ++i) {
        const CtscScope* sc = r->scopes[i];
        /* Mirror tsc's lazy `locals` on IsBlockScopedContainer nodes (see
         * binder.ts bindBlockScopedDeclaration ~2450 `if (!blockScopeContainer
         * .locals) blockScopeContainer.locals = createSymbolTable();`). The
         * oracle (oracle-binder.ts) only emits scopes where `node.locals !==
         * undefined`, so a Block with no block-scoped decls must NOT appear.
         * Containers with eagerly-allocated locals (SourceFile, FunctionDecl,
         * ModuleDecl, ...) always emit, even when empty. */
        if (sc->node && sc->node->kind == CTSC_SK_Block && sc->locals.len == 0) continue;
        emit_scope(&j, &a, sc);
    }
    ctsc_json_end_arr(&j);
    ctsc_json_key(&j, "diagnostics");
    ctsc_json_begin_arr(&j);
    ctsc_json_end_arr(&j);
    ctsc_json_end_obj(&j);

    ctsc_arena_free(&a);
}

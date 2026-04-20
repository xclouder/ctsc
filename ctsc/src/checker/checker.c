#include "ctsc/checker.h"
#include "ctsc/arena.h"
#include "ctsc/scanner.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/*
 * Phase 4 (M4.0) checker — pragmatic, AST-walk implementation.
 *
 * Shared responsibilities:
 *   - Types channel: record a CtscCheckTypeEntry for each interesting named
 *     declaration (VariableDeclaration / ParameterDeclaration /
 *     FunctionDeclaration / MethodDeclaration / PropertyDeclaration /
 *     PropertySignature) so ctsc_check_dump_types_json produces the same
 *     shape as oracle-checker-types.ts.
 *   - Diag channel: emit TS2304 ("Cannot find name '...'") for each bare
 *     Identifier in reference position whose name doesn't resolve against
 *     the binder-built scope chain.
 *
 * The walker below is intentionally a tiny, switch-driven visitor that only
 * looks at the node fields relevant to M4.0. It is the canonical place the
 * agent loop grows when fixtures demand new constructs.
 */

/* ----- output accumulators ----- */

static void diag_push(CtscCheckResult* r, CtscArena* a, CtscCheckDiagnostic d) {
    if (r->diagnostics_len + 1 > r->diagnostics_cap) {
        size_t ncap = r->diagnostics_cap ? r->diagnostics_cap * 2 : 4;
        CtscCheckDiagnostic* nb = (CtscCheckDiagnostic*)ctsc_arena_alloc(a, ncap * sizeof(CtscCheckDiagnostic));
        if (r->diagnostics) memcpy(nb, r->diagnostics, r->diagnostics_len * sizeof(CtscCheckDiagnostic));
        r->diagnostics = nb;
        r->diagnostics_cap = ncap;
    }
    r->diagnostics[r->diagnostics_len++] = d;
}

static void entry_push(CtscCheckResult* r, CtscArena* a, CtscCheckTypeEntry e) {
    if (r->entries_len + 1 > r->entries_cap) {
        size_t ncap = r->entries_cap ? r->entries_cap * 2 : 4;
        CtscCheckTypeEntry* nb = (CtscCheckTypeEntry*)ctsc_arena_alloc(a, ncap * sizeof(CtscCheckTypeEntry));
        if (r->entries) memcpy(nb, r->entries, r->entries_len * sizeof(CtscCheckTypeEntry));
        r->entries = nb;
        r->entries_cap = ncap;
    }
    r->entries[r->entries_len++] = e;
}

/* ----- scope stack (node→scope) ----- */

typedef struct {
    CtscArena*      arena;
    CtscBindResult* binding;
    CtscScope**     stack;
    size_t          depth;
    size_t          cap;
} ScopeStack;

static CtscScope* find_scope_for_node(CtscBindResult* b, const CtscNode* n) {
    for (size_t i = 0; i < b->scopes_len; ++i) {
        if (b->scopes[i]->node == n) return b->scopes[i];
    }
    return NULL;
}

static void scope_push(ScopeStack* ss, CtscScope* s) {
    if (ss->depth + 1 > ss->cap) {
        size_t ncap = ss->cap ? ss->cap * 2 : 8;
        CtscScope** nb = (CtscScope**)ctsc_arena_alloc(ss->arena, ncap * sizeof(CtscScope*));
        if (ss->stack) memcpy(nb, ss->stack, ss->depth * sizeof(CtscScope*));
        ss->stack = nb;
        ss->cap = ncap;
    }
    ss->stack[ss->depth++] = s;
}

static void scope_pop(ScopeStack* ss) {
    if (ss->depth > 0) ss->depth--;
}

static CtscSymbol* resolve_name(ScopeStack* ss, const uint16_t* name, size_t name_len) {
    for (size_t i = ss->depth; i > 0; --i) {
        CtscScope* sc = ss->stack[i - 1];
        CtscSymbol* s = ctsc_symbol_table_find(&sc->locals, name, name_len);
        if (s) return s;
    }
    return NULL;
}

/* ----- type inference helpers ----- */

static CtscType* type_of_type_node(CtscTypeRegistry* reg, const CtscNode* type_node) {
    /* Maps TypeNode → CtscType for the M4.0 subset. Unknown types collapse
     * to `any` so downstream formatters never emit garbage. */
    if (!type_node) return reg->t_any;
    switch (type_node->kind) {
        case CTSC_SK_NumberKeyword:    return reg->t_number;
        case CTSC_SK_StringKeyword:    return reg->t_string;
        case CTSC_SK_BooleanKeyword:   return reg->t_boolean;
        case CTSC_SK_VoidKeyword:      return reg->t_void;
        case CTSC_SK_UndefinedKeyword: return reg->t_undefined;
        case CTSC_SK_NullKeyword:      return reg->t_null;
        case CTSC_SK_NeverKeyword:     return reg->t_never;
        case CTSC_SK_UnknownKeyword:   return reg->t_unknown;
        case CTSC_SK_AnyKeyword:       return reg->t_any;
        case CTSC_SK_ObjectKeyword:    return reg->t_object;
        case CTSC_SK_SymbolKeyword:    return reg->t_symbol;
        default:
            /* TypeReference / generics / etc.: not handled in M4.0. */
            return reg->t_any;
    }
}

static CtscType* type_of_expression(CtscTypeRegistry* reg, const CtscNode* expr);

static CtscType* type_of_expression(CtscTypeRegistry* reg, const CtscNode* expr) {
    if (!expr) return reg->t_any;
    switch (expr->kind) {
        case CTSC_SK_NumericLiteral: {
            /*
             * ctsc stores NumericLiteral's text as UTF-16 code units; convert
             * to a double for the literal type. All current curriculum cases
             * are simple integers, so a plain strtod with an ASCII copy is
             * sufficient.
             */
            const CtscNumericLiteralData* d = &expr->data.numericLiteral;
            char buf[64];
            size_t n = d->text_len < sizeof(buf) - 1 ? d->text_len : sizeof(buf) - 1;
            for (size_t i = 0; i < n; ++i) buf[i] = (char)d->text[i];
            buf[n] = '\0';
            double v = strtod(buf, NULL);
            return ctsc_type_number_literal(reg, v);
        }
        case CTSC_SK_StringLiteral: {
            const CtscStringLiteralData* d = &expr->data.stringLiteral;
            return ctsc_type_string_literal(reg, d->value ? d->value : d->text,
                                                   d->value ? d->value_len : d->text_len);
        }
        case CTSC_SK_NoSubstitutionTemplateLiteral: {
            const CtscTemplateLiteralLikeData* d = &expr->data.templateLiteralLike;
            return ctsc_type_string_literal(reg, d->text, d->text_len);
        }
        case CTSC_SK_TrueKeyword:  return reg->t_true;
        case CTSC_SK_FalseKeyword: return reg->t_false;
        case CTSC_SK_NullKeyword:  return reg->t_null;
        case CTSC_SK_UndefinedKeyword: return reg->t_undefined;
        case CTSC_SK_BigIntLiteral: {
            const CtscNumericLiteralData* d = &expr->data.numericLiteral;
            /* Scanner strips the trailing `n`; we pass the remaining digits. */
            return ctsc_type_bigint_literal(reg, d->text, d->text_len);
        }
        default:
            /* All other expressions → any, for M4.0. */
            return reg->t_any;
    }
}

/* Emit either a widened (for let/var) or non-widened (for const) initializer type. */
static CtscType* var_decl_type(CtscTypeRegistry* reg, const CtscNode* decl, bool is_const) {
    const CtscVariableDeclarationData* d = &decl->data.variableDeclaration;
    if (d->type) return type_of_type_node(reg, d->type);
    CtscType* init_t = type_of_expression(reg, d->initializer);
    if (!init_t) return reg->t_any;
    return is_const ? init_t : ctsc_type_widen(reg, init_t);
}

static const char* syntax_kind_cstr(CtscSyntaxKind k) { return ctsc_syntax_kind_name(k); }

/* ----- walker ----- */

typedef struct {
    CtscCheckResult* r;
    CtscArena*       arena;
    ScopeStack       scopes;
} Walk;

static void visit(Walk* w, const CtscNode* n);

static void walk_children_nodearray(Walk* w, const CtscNodeArray* arr) {
    for (size_t i = 0; i < arr->len; ++i) visit(w, arr->items[i]);
}

static void check_identifier_reference(Walk* w, const CtscNode* id) {
    if (!id || id->kind != CTSC_SK_Identifier) return;
    const uint16_t* nm = id->data.identifier.text;
    size_t nl = id->data.identifier.text_len;
    if (!nm || nl == 0) return;
    CtscSymbol* sym = resolve_name(&w->scopes, nm, nl);
    if (sym) return;
    /*
     * Produce the same message text tsc emits for TS2304:
     *   "Cannot find name '<ident>'."
     * oracle uses ts.flattenDiagnosticMessageText which collapses `chain`
     * messages to newline-joined strings; for TS2304 the message is a single
     * line so the output is identical.
     */
    CtscBuffer msg; ctsc_buf_init(&msg);
    ctsc_buf_append_cstr(&msg, "Cannot find name '");
    for (size_t i = 0; i < nl; ++i) {
        uint16_t u = nm[i];
        if (u < 0x80) { ctsc_buf_append_char(&msg, (char)u); }
        else if (u < 0x800) {
            char b[2];
            b[0] = (char)(0xC0 | (u >> 6));
            b[1] = (char)(0x80 | (u & 0x3F));
            ctsc_buf_append(&msg, b, 2);
        } else {
            char b[3];
            b[0] = (char)(0xE0 | (u >> 12));
            b[1] = (char)(0x80 | ((u >> 6) & 0x3F));
            b[2] = (char)(0x80 | (u & 0x3F));
            ctsc_buf_append(&msg, b, 3);
        }
    }
    ctsc_buf_append_cstr(&msg, "'.");
    /* Move the message text into the arena so it outlives the local buffer. */
    char* msg_arena = (char*)ctsc_arena_alloc(w->arena, msg.len + 1);
    memcpy(msg_arena, msg.data, msg.len);
    msg_arena[msg.len] = '\0';
    ctsc_buf_free(&msg);

    CtscCheckDiagnostic d = {0};
    d.code = 2304;
    d.category = "Error";
    /* tsc diagnostics use the token's actual start (post-leading-trivia),
     * not node.pos (= full_start). For a bare Identifier there is no
     * trailing trivia inside the node, so start = end - text_len in UTF-16
     * units, which matches ts.TextSpan produced by createDiagnosticForNode. */
    d.length = (int)id->data.identifier.text_len;
    d.start = id->end - d.length;
    d.message = msg_arena;
    diag_push(w->r, w->arena, d);
}

static void push_entry_for_name(Walk* w, const CtscNode* decl, const CtscNode* name_id, CtscType* type) {
    if (!name_id || name_id->kind != CTSC_SK_Identifier) return;
    if (!name_id->data.identifier.text || name_id->data.identifier.text_len == 0) return;
    CtscCheckTypeEntry e = {0};
    e.name = name_id->data.identifier.text;
    e.name_len = name_id->data.identifier.text_len;
    e.decl_kind_name = syntax_kind_cstr(decl->kind);
    e.pos = name_id->pos;
    e.end = name_id->end;
    e.type = type ? type : w->r->registry.t_any;
    entry_push(w->r, w->arena, e);
}

static bool is_const_decl_list(const CtscNode* list) {
    if (!list || list->kind != CTSC_SK_VariableDeclarationList) return false;
    int flags = list->data.variableDeclarationList.flags;
    /* bit1 = Const */
    return (flags & 0x2) != 0;
}

static void open_scope_if_container(Walk* w, const CtscNode* n) {
    /* binding is stored in w->scopes.binding. */
    CtscScope* s = find_scope_for_node(w->scopes.binding, n);
    if (s) scope_push(&w->scopes, s);
}

static void close_scope_if_container(Walk* w, const CtscNode* n) {
    CtscScope* s = find_scope_for_node(w->scopes.binding, n);
    if (s) scope_pop(&w->scopes);
}

static void visit_variable_statement(Walk* w, const CtscNode* n) {
    const CtscVariableStatementData* vs = &n->data.variableStatement;
    const CtscNode* list = vs->declarationList;
    if (!list || list->kind != CTSC_SK_VariableDeclarationList) return;
    bool is_const = is_const_decl_list(list);
    const CtscNodeArray* decls = &list->data.variableDeclarationList.declarations;
    for (size_t i = 0; i < decls->len; ++i) {
        const CtscNode* d = decls->items[i];
        if (!d || d->kind != CTSC_SK_VariableDeclaration) continue;
        CtscType* t = var_decl_type(&w->r->registry, d, is_const);
        push_entry_for_name(w, d, d->data.variableDeclaration.name, t);
        /* Still walk initializer so identifier references inside get checked. */
        if (d->data.variableDeclaration.initializer) {
            visit(w, d->data.variableDeclaration.initializer);
        }
    }
}

static CtscType* param_type(CtscTypeRegistry* reg, const CtscNode* param) {
    const CtscParameterData* p = &param->data.parameter;
    if (p->type) return type_of_type_node(reg, p->type);
    return reg->t_any;
}

static void emit_function_signature_string(CtscBuffer* out,
                                           const CtscNodeArray* params,
                                           CtscTypeRegistry* reg,
                                           const CtscNode* return_type_node) {
    /*
     * tsc's typeToString on a FunctionType uses the printer with signature
     * display rules; at default flags the output form is:
     *   (p0: T0, p1: T1, ...) => R
     * with parameter names preserved. Rest parameters would prefix `...`;
     * we re-emit them when CtscParameterData.has_dot_dot_dot is true.
     */
    ctsc_buf_append_char(out, '(');
    for (size_t i = 0; i < params->len; ++i) {
        const CtscNode* p = params->items[i];
        if (i > 0) ctsc_buf_append_cstr(out, ", ");
        if (p->data.parameter.has_dot_dot_dot) ctsc_buf_append_cstr(out, "...");
        const CtscNode* name = p->data.parameter.name;
        if (name && name->kind == CTSC_SK_Identifier) {
            const CtscIdentifierData* id = &name->data.identifier;
            for (size_t k = 0; k < id->text_len; ++k) {
                uint16_t u = id->text[k];
                if (u < 0x80) ctsc_buf_append_char(out, (char)u);
            }
        } else {
            ctsc_buf_append_cstr(out, "arg");
        }
        ctsc_buf_append_cstr(out, ": ");
        CtscType* t = param_type(reg, p);
        ctsc_type_to_string(t, out);
    }
    ctsc_buf_append_cstr(out, ") => ");
    if (return_type_node) {
        CtscType* rt = type_of_type_node(reg, return_type_node);
        ctsc_type_to_string(rt, out);
    } else {
        /* No explicit return annotation; M4.0 punts to void unless the body
         * has a return expression, in which case we fallback to `any`. */
        ctsc_buf_append_cstr(out, "void");
    }
}

static void push_entry_with_string(Walk* w, const CtscNode* decl, const CtscNode* name_id,
                                   const char* type_str, size_t type_str_len) {
    if (!name_id || name_id->kind != CTSC_SK_Identifier) return;
    if (!name_id->data.identifier.text || name_id->data.identifier.text_len == 0) return;
    CtscCheckTypeEntry e = {0};
    e.name = name_id->data.identifier.text;
    e.name_len = name_id->data.identifier.text_len;
    e.decl_kind_name = syntax_kind_cstr(decl->kind);
    e.pos = name_id->pos;
    e.end = name_id->end;
    e.type_string = type_str;
    e.type_string_len = type_str_len;
    entry_push(w->r, w->arena, e);
}

static void visit_function_declaration(Walk* w, const CtscNode* n) {
    const CtscFunctionDeclarationData* f = &n->data.functionDeclaration;
    /* Emit a types-channel entry for the function name if present. M4.0 does
     * not model function types structurally — it pre-formats the string the
     * oracle expects (see emit_function_signature_string comment). */
    if (f->name && f->name->kind == CTSC_SK_Identifier) {
        CtscBuffer sig; ctsc_buf_init(&sig);
        emit_function_signature_string(&sig, &f->parameters, &w->r->registry, NULL);
        char* s = (char*)ctsc_arena_alloc(w->arena, sig.len);
        memcpy(s, sig.data, sig.len);
        size_t slen = sig.len;
        ctsc_buf_free(&sig);
        push_entry_with_string(w, n, f->name, s, slen);
    }

    open_scope_if_container(w, n);
    for (size_t i = 0; i < f->parameters.len; ++i) {
        const CtscNode* p = f->parameters.items[i];
        if (!p || p->kind != CTSC_SK_Parameter) continue;
        push_entry_for_name(w, p, p->data.parameter.name, param_type(&w->r->registry, p));
        if (p->data.parameter.initializer) visit(w, p->data.parameter.initializer);
    }
    if (f->body) visit(w, f->body);
    close_scope_if_container(w, n);
}

/* Dispatch visitor: handles both scope management and reference checking. */
static void visit(Walk* w, const CtscNode* n) {
    if (!n) return;
    switch (n->kind) {
        case CTSC_SK_SourceFile:
            open_scope_if_container(w, n);
            walk_children_nodearray(w, &n->data.sourceFile.statements);
            close_scope_if_container(w, n);
            return;

        case CTSC_SK_Block:
            open_scope_if_container(w, n);
            walk_children_nodearray(w, &n->data.block.statements);
            close_scope_if_container(w, n);
            return;

        case CTSC_SK_VariableStatement:
            visit_variable_statement(w, n);
            return;

        case CTSC_SK_FunctionDeclaration:
            visit_function_declaration(w, n);
            return;

        case CTSC_SK_ExpressionStatement:
            visit(w, n->data.expressionStatement.expression);
            return;

        case CTSC_SK_ReturnStatement:
            if (n->data.returnStatement.expression) visit(w, n->data.returnStatement.expression);
            return;

        case CTSC_SK_IfStatement:
            visit(w, n->data.ifStatement.expression);
            visit(w, n->data.ifStatement.thenStatement);
            visit(w, n->data.ifStatement.elseStatement);
            return;

        case CTSC_SK_WhileStatement:
            visit(w, n->data.whileStatement.expression);
            visit(w, n->data.whileStatement.statement);
            return;

        /* Identifier references: TS2304 detection. */
        case CTSC_SK_Identifier:
            check_identifier_reference(w, n);
            return;

        case CTSC_SK_BinaryExpression:
            visit(w, n->data.binaryExpression.left);
            visit(w, n->data.binaryExpression.right);
            return;

        case CTSC_SK_CallExpression:
            visit(w, n->data.callExpression.expression);
            for (size_t i = 0; i < n->data.callExpression.arguments.len; ++i) {
                visit(w, n->data.callExpression.arguments.items[i]);
            }
            return;

        case CTSC_SK_PropertyAccessExpression:
            /* Only the LHS is a reference; the `.name` is a property, not a
             * symbol lookup. */
            visit(w, n->data.propertyAccessExpression.expression);
            return;

        case CTSC_SK_ElementAccessExpression:
            visit(w, n->data.elementAccessExpression.expression);
            visit(w, n->data.elementAccessExpression.argumentExpression);
            return;

        case CTSC_SK_ParenthesizedExpression:
            visit(w, n->data.parenthesizedExpression.expression);
            return;

        case CTSC_SK_PrefixUnaryExpression:
            visit(w, n->data.prefixUnaryExpression.operand);
            return;

        case CTSC_SK_PostfixUnaryExpression:
            visit(w, n->data.postfixUnaryExpression.operand);
            return;

        case CTSC_SK_ConditionalExpression:
            visit(w, n->data.conditionalExpression.condition);
            visit(w, n->data.conditionalExpression.whenTrue);
            visit(w, n->data.conditionalExpression.whenFalse);
            return;

        default:
            /* Leaves and unhandled kinds: nothing to do for M4.0. */
            return;
    }
}

CtscCheckResult* ctsc_check(const CtscNode* sourceFile, CtscBindResult* binding, CtscArena* arena) {
    CtscCheckResult* r = (CtscCheckResult*)ctsc_arena_calloc(arena, 1, sizeof(CtscCheckResult));
    ctsc_type_registry_init(&r->registry, arena);

    Walk w;
    memset(&w, 0, sizeof(w));
    w.r = r;
    w.arena = arena;
    w.scopes.arena = arena;
    w.scopes.binding = binding;
    visit(&w, sourceFile);
    return r;
}

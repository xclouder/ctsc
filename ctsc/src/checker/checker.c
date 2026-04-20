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
 *   - TS2322 when an annotated VariableDeclaration's initializer is not
 *     assignable to the annotation (error on the binding name), mirroring
 *     checker.ts checkVariableDeclaration / checkTypeAssignableTo
 *     (~22686, Diagnostics.Type_0_is_not_assignable_to_type_1).
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

typedef struct {
    CtscCheckResult* r;
    CtscArena*       arena;
    ScopeStack       scopes;
    const uint16_t* source_utf16;
    size_t           source_utf16_len;
} Walk;

/* ----- type inference helpers ----- */

static bool ctsc_is_ws_u16(uint16_t c) {
    return c == (uint16_t)' ' || c == (uint16_t)'\t' || c == (uint16_t)'\n' || c == (uint16_t)'\r';
}

/*
 * Strip one trailing empty `[]` from UTF-16 [pos, *end_io). Mirrors parser.ts
 * postfix element types when brackets contain no type (parsePostfixTypeOrHigher
 * ~4716).
 */
static bool consume_trailing_empty_array_suffix(const uint16_t* src, int pos, int* end_io) {
    int e = *end_io;
    if (e <= pos) return false;
    while (e > pos && ctsc_is_ws_u16(src[e - 1])) e--;
    if (e <= pos || src[e - 1] != (uint16_t)']') return false;
    int close_br = e - 1;
    int i = close_br - 1;
    while (i >= pos && ctsc_is_ws_u16(src[i])) i--;
    if (i < pos || src[i] != (uint16_t)'[') return false;
    for (int k = i + 1; k < close_br; k++) {
        if (!ctsc_is_ws_u16(src[k])) return false;
    }
    *end_io = i;
    return true;
}

static bool type_node_has_postfix_empty_array(const uint16_t* src, size_t src_len, const CtscNode* type_node) {
    if (!type_node || !src || type_node->pos < 0 || type_node->end < 0) return false;
    if (type_node->end > (int)src_len) return false;
    int ee = type_node->end;
    return consume_trailing_empty_array_suffix(src, type_node->pos, &ee);
}

static int count_trailing_empty_array_suffix_pairs(const uint16_t* src, int pos, int end) {
    int n = 0;
    int e = end;
    while (consume_trailing_empty_array_suffix(src, pos, &e)) n++;
    return n;
}

static void append_keyword_intrinsic_name_to_buf(CtscBuffer* out, CtscSyntaxKind k) {
    switch (k) {
        case CTSC_SK_AnyKeyword:       ctsc_buf_append_cstr(out, "any"); return;
        case CTSC_SK_UnknownKeyword:   ctsc_buf_append_cstr(out, "unknown"); return;
        case CTSC_SK_StringKeyword:    ctsc_buf_append_cstr(out, "string"); return;
        case CTSC_SK_NumberKeyword:    ctsc_buf_append_cstr(out, "number"); return;
        case CTSC_SK_BooleanKeyword:   ctsc_buf_append_cstr(out, "boolean"); return;
        case CTSC_SK_VoidKeyword:      ctsc_buf_append_cstr(out, "void"); return;
        case CTSC_SK_UndefinedKeyword: ctsc_buf_append_cstr(out, "undefined"); return;
        case CTSC_SK_NullKeyword:      ctsc_buf_append_cstr(out, "null"); return;
        case CTSC_SK_NeverKeyword:     ctsc_buf_append_cstr(out, "never"); return;
        case CTSC_SK_ObjectKeyword:    ctsc_buf_append_cstr(out, "object"); return;
        case CTSC_SK_SymbolKeyword:    ctsc_buf_append_cstr(out, "symbol"); return;
        default:                       ctsc_buf_append_cstr(out, "any"); return;
    }
}

static void append_utf16_ascii_identifier(CtscBuffer* out, const uint16_t* t, size_t n) {
    if (!t) return;
    for (size_t i = 0; i < n; ++i) {
        uint16_t u = t[i];
        if (u < 0x80) ctsc_buf_append_char(out, (char)u);
    }
}

/*
 * Text used inside synthetic function signatures `(a: T) => R`. tsc keeps
 * postfix `T[]` spelling here even though getTypeAtLocation on `a` is `{}`
 * under noLib (oracle-checker-types.ts vs checker typeToString paths).
 */
static void append_type_for_signature(CtscBuffer* out, const CtscNode* type_node,
                                     const uint16_t* src, size_t src_len) {
    if (!type_node) { ctsc_buf_append_cstr(out, "any"); return; }
    if (!src || type_node->pos < 0 || type_node->end < 0 || type_node->end > (int)src_len) {
        ctsc_buf_append_cstr(out, "any");
        return;
    }
    switch (type_node->kind) {
        case CTSC_SK_AnyKeyword:
        case CTSC_SK_UnknownKeyword:
        case CTSC_SK_StringKeyword:
        case CTSC_SK_NumberKeyword:
        case CTSC_SK_BooleanKeyword:
        case CTSC_SK_VoidKeyword:
        case CTSC_SK_UndefinedKeyword:
        case CTSC_SK_NullKeyword:
        case CTSC_SK_NeverKeyword:
        case CTSC_SK_ObjectKeyword:
        case CTSC_SK_SymbolKeyword: {
            int n_br = count_trailing_empty_array_suffix_pairs(src, type_node->pos, type_node->end);
            append_keyword_intrinsic_name_to_buf(out, type_node->kind);
            for (int i = 0; i < n_br; ++i) ctsc_buf_append_cstr(out, "[]");
            return;
        }
        case CTSC_SK_TypeReference: {
            const CtscTypeReferenceData* tr = &type_node->data.typeReference;
            int inner_end = 0;
            if (tr->typeName && tr->typeName->kind == CTSC_SK_Identifier) {
                const CtscIdentifierData* id = &tr->typeName->data.identifier;
                append_utf16_ascii_identifier(out, id->text, id->text_len);
                inner_end = tr->typeName->end;
            } else {
                ctsc_buf_append_cstr(out, "any");
                return;
            }
            if (tr->has_type_arguments && tr->type_arguments.len > 0) {
                ctsc_buf_append_char(out, '<');
                for (size_t ai = 0; ai < tr->type_arguments.len; ++ai) {
                    if (ai > 0) ctsc_buf_append_cstr(out, ", ");
                    append_type_for_signature(out, tr->type_arguments.items[ai], src, src_len);
                }
                ctsc_buf_append_char(out, '>');
                {
                    CtscNode* last = tr->type_arguments.items[tr->type_arguments.len - 1];
                    if (last) inner_end = last->end;
                }
            }
            {
                int n_br = count_trailing_empty_array_suffix_pairs(src, inner_end, type_node->end);
                for (int i = 0; i < n_br; ++i) ctsc_buf_append_cstr(out, "[]");
            }
            return;
        }
        default:
            ctsc_buf_append_cstr(out, "any");
            return;
    }
}

static double numeric_literal_to_double(const CtscNumericLiteralData* d);

static CtscType* type_of_type_node(CtscTypeRegistry* reg, const uint16_t* src, size_t src_len,
                                  const CtscNode* type_node) {
    /* Maps TypeNode → CtscType for the M4.0 subset. Unknown types collapse
     * to `any` so downstream formatters never emit garbage. */
    if (!type_node) return reg->t_any;
    if (type_node_has_postfix_empty_array(src, src_len, type_node)) {
        return reg->t_empty_object;
    }
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
        case CTSC_SK_StringLiteral: {
            const CtscStringLiteralData* d = &type_node->data.stringLiteral;
            return ctsc_type_string_literal(reg, d->value ? d->value : d->text,
                                                 d->value ? d->value_len : d->text_len);
        }
        case CTSC_SK_NumericLiteral: {
            double v = numeric_literal_to_double(&type_node->data.numericLiteral);
            return ctsc_type_number_literal(reg, v);
        }
        default:
            /* TypeReference / generics / etc.: not handled in M4.0. */
            return reg->t_any;
    }
}

static CtscType* type_of_expression(CtscTypeRegistry* reg, const CtscNode* expr);

/* Operand classification for binary `+` / ordering (checker.ts ~40837-40848). */
static bool type_is_number_like(const CtscType* t) {
    return t && (t->kind == CTSC_TYPE_NUMBER || t->kind == CTSC_TYPE_NUMBER_LITERAL);
}

static bool type_is_bigint_like(const CtscType* t) {
    return t && (t->kind == CTSC_TYPE_BIGINT || t->kind == CTSC_TYPE_BIGINT_LITERAL);
}

static bool type_is_string_like(const CtscType* t) {
    return t && (t->kind == CTSC_TYPE_STRING || t->kind == CTSC_TYPE_STRING_LITERAL);
}

/*
 * Numeric literal text → double for NumberLiteralType. Mirrors checker.ts
 * getNumberLiteralType(parseFloat(text)) / checkPrefixUnaryExpression
 * (~39990-39996) when the operand is a NumericLiteral.
 */
static double numeric_literal_to_double(const CtscNumericLiteralData* d) {
    char buf[64];
    size_t n = d->text_len < sizeof(buf) - 1 ? d->text_len : sizeof(buf) - 1;
    for (size_t i = 0; i < n; ++i) buf[i] = (char)d->text[i];
    buf[n] = '\0';
    return strtod(buf, NULL);
}

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
            double v = numeric_literal_to_double(&expr->data.numericLiteral);
            return ctsc_type_number_literal(reg, v);
        }
        case CTSC_SK_StringLiteral: {
            const CtscStringLiteralData* d = &expr->data.stringLiteral;
            return ctsc_type_string_literal(reg, d->value ? d->value : d->text,
                                                   d->value ? d->value_len : d->text_len);
        }
        case CTSC_SK_NoSubstitutionTemplateLiteral: {
            const CtscTemplateLiteralLikeData* d = &expr->data.templateLiteralLike;
            return ctsc_type_string_literal(reg, d->value ? d->value : d->text,
                                                 d->value ? d->value_len : d->text_len);
        }
        case CTSC_SK_TrueKeyword:  return reg->t_true;
        case CTSC_SK_FalseKeyword: return reg->t_false;
        case CTSC_SK_NullKeyword:  return reg->t_null;
        case CTSC_SK_UndefinedKeyword: return reg->t_undefined;
        case CTSC_SK_BigIntLiteral: {
            const CtscNumericLiteralData* d = &expr->data.numericLiteral;
            /* Token text is the full lexeme (`42n`); literal type uses digits only
             * (type_registry appends `n` for typeToString — checker.ts BigIntLiteralType). */
            size_t len = d->text_len;
            if (len > 0 && d->text[len - 1] == (uint16_t)'n') {
                len--;
            }
            return ctsc_type_bigint_literal(reg, d->text, len);
        }
        case CTSC_SK_PrefixUnaryExpression: {
            const CtscNode* op = expr->data.prefixUnaryExpression.operand;
            CtscSyntaxKind oper = expr->data.prefixUnaryExpression.operator_kind;
            /*
             * checker.ts checkPrefixUnaryExpression (~39990-39997): for
             * NumericLiteral operand, unary +/- yields a fresh number literal
             * type from -(operand.text) / +(operand.text) as in ECMAScript.
             */
            if (op && op->kind == CTSC_SK_NumericLiteral) {
                double v = numeric_literal_to_double(&op->data.numericLiteral);
                if (oper == CTSC_SK_MinusToken) {
                    return ctsc_type_number_literal(reg, -v);
                }
                if (oper == CTSC_SK_PlusToken) {
                    return ctsc_type_number_literal(reg, v);
                }
            }
            return reg->t_any;
        }
        case CTSC_SK_ArrayLiteralExpression:
            /*
             * Harness oracle uses noLib; without Array, checkArrayLiteral's
             * element union collapses so typeToString is `{}` (checker.ts
             * checkArrayLiteral ~33329-33404, createArrayType).
             */
            return reg->t_empty_object;
        case CTSC_SK_BinaryExpression: {
            const CtscBinaryExpressionData* b = &expr->data.binaryExpression;
            CtscType* lt = type_of_expression(reg, b->left);
            CtscType* rt = type_of_expression(reg, b->right);
            switch (b->operator_kind) {
                case CTSC_SK_PlusToken:
                    /*
                     * checkBinaryLikeExpressionWorker PlusToken branch
                     * (checker.ts ~40825-40877).
                     */
                    if (type_is_number_like(lt) && type_is_number_like(rt)) {
                        return reg->t_number;
                    }
                    if (type_is_bigint_like(lt) && type_is_bigint_like(rt)) {
                        return reg->t_bigint;
                    }
                    if (type_is_string_like(lt) || type_is_string_like(rt)) {
                        return reg->t_string;
                    }
                    if ((lt && lt->kind == CTSC_TYPE_ANY) || (rt && rt->kind == CTSC_TYPE_ANY)) {
                        return reg->t_any;
                    }
                    return reg->t_any;
                case CTSC_SK_LessThanToken:
                case CTSC_SK_GreaterThanToken:
                case CTSC_SK_LessThanEqualsToken:
                case CTSC_SK_GreaterThanEqualsToken:
                    /* ~40878-40895: relational operators → boolean. */
                    return reg->t_boolean;
                case CTSC_SK_EqualsEqualsToken:
                case CTSC_SK_ExclamationEqualsToken:
                case CTSC_SK_EqualsEqualsEqualsToken:
                case CTSC_SK_ExclamationEqualsEqualsToken:
                    /* ~40896-40915: equality → boolean. */
                    return reg->t_boolean;
                default:
                    return reg->t_any;
            }
        }
        default:
            /* All other expressions → any, for M4.0. */
            return reg->t_any;
    }
}

/*
 * Structural assignability for the M4.0 subset. Mirrors the legacy
 * strictNullChecks-off behaviour of the harness oracle (noLib / default
 * compiler options in harness/src/oracle-checker-diag.ts).
 */
static bool utf16_type_text_equal(const uint16_t* a, size_t alen, const uint16_t* b, size_t blen) {
    if (alen != blen) return false;
    if (alen == 0) return true;
    if (!a || !b) return a == b;
    return memcmp(a, b, alen * sizeof(uint16_t)) == 0;
}

static bool is_assignable_to(CtscTypeRegistry* reg, CtscType* source, CtscType* target) {
    if (!source || !target) return true;
    if (source->kind == CTSC_TYPE_ANY || target->kind == CTSC_TYPE_ANY) return true;
    if (source->kind == CTSC_TYPE_NEVER) return true;
    if (target->kind == CTSC_TYPE_UNKNOWN_T) return true;
    if (source->kind == CTSC_TYPE_UNKNOWN_T) {
        return target->kind == CTSC_TYPE_ANY || target->kind == CTSC_TYPE_UNKNOWN_T;
    }
    if (target->kind == CTSC_TYPE_NEVER) return source->kind == CTSC_TYPE_NEVER;
    if (source->kind == CTSC_TYPE_NULL || source->kind == CTSC_TYPE_UNDEFINED) return true;
    /*
     * String literal types are only mutually assignable when identical
     * (checker.ts isSimpleTypeRelatedTo ~22230-22240; structural check on
     * literal values, not widened `string`).
     */
    if (source->kind == CTSC_TYPE_STRING_LITERAL && target->kind == CTSC_TYPE_STRING_LITERAL) {
        return utf16_type_text_equal(source->text, source->text_len, target->text, target->text_len);
    }
    /*
     * Number literal types: assignable only when values match (checker.ts
     * isSimpleTypeRelatedTo ~22240-22245; literal-to-literal is not covered by
     * the NumberLike→Number widen rule alone).
     */
    if (source->kind == CTSC_TYPE_NUMBER_LITERAL && target->kind == CTSC_TYPE_NUMBER_LITERAL) {
        return source->number_value == target->number_value;
    }
    {
        CtscType* sw = ctsc_type_widen(reg, source);
        CtscType* tw = ctsc_type_widen(reg, target);
        return sw->kind == tw->kind;
    }
}

static void emit_ts2322_on_binding_name(CtscCheckResult* r, CtscArena* arena, CtscTypeRegistry* reg,
                                         const CtscNode* name_id, CtscType* source, CtscType* target) {
    if (!name_id || name_id->kind != CTSC_SK_Identifier) return;
    const bool both_str_lit = source && target
        && source->kind == CTSC_TYPE_STRING_LITERAL
        && target->kind == CTSC_TYPE_STRING_LITERAL;
    const bool both_num_lit = source && target
        && source->kind == CTSC_TYPE_NUMBER_LITERAL
        && target->kind == CTSC_TYPE_NUMBER_LITERAL;
    CtscType* src_m = (both_str_lit || both_num_lit) ? source : ctsc_type_widen(reg, source);
    CtscType* tgt_m = (both_str_lit || both_num_lit) ? target : ctsc_type_widen(reg, target);
    CtscBuffer msg;
    ctsc_buf_init(&msg);
    ctsc_buf_append_cstr(&msg, "Type '");
    ctsc_type_to_string(src_m, &msg);
    ctsc_buf_append_cstr(&msg, "' is not assignable to type '");
    ctsc_type_to_string(tgt_m, &msg);
    ctsc_buf_append_cstr(&msg, "'.");
    char* msg_arena = (char*)ctsc_arena_alloc(arena, msg.len + 1);
    memcpy(msg_arena, msg.data, msg.len);
    msg_arena[msg.len] = '\0';
    ctsc_buf_free(&msg);

    CtscCheckDiagnostic d = {0};
    d.code = 2322;
    d.category = "Error";
    d.length = (int)name_id->data.identifier.text_len;
    d.start = name_id->end - d.length;
    d.message = msg_arena;
    diag_push(r, arena, d);
}

/*
 * Mirrors checker.ts getWidenedTypeWithContext (~26021) when strictNullChecks
 * is off: nullWideningType / undefinedWideningType carry RequiresWidening and
 * TypeFlags.Nullable, so the nullable branch yields anyType (~26027-26028).
 * ctsc models the harness default (no strictNullChecks) only for M4.0.
 */
static CtscType* widen_nullish_when_not_strict_null(CtscTypeRegistry* reg, CtscType* t) {
    if (!t) return reg->t_any;
    if (t->kind == CTSC_TYPE_NULL || t->kind == CTSC_TYPE_UNDEFINED) return reg->t_any;
    return t;
}

/*
 * Inferred variable type: getWidenedLiteralTypeForInitializer (~41455) then
 * widenTypeForVariableLikeDeclaration → getWidenedType (~12480-12495).
 * const keeps literal types except null/undefined widening (above).
 */
static CtscType* var_decl_type(Walk* w, const CtscNode* decl, bool is_const) {
    const CtscVariableDeclarationData* d = &decl->data.variableDeclaration;
    if (d->type) return type_of_type_node(&w->r->registry, w->source_utf16, w->source_utf16_len, d->type);
    CtscType* init_t = type_of_expression(&w->r->registry, d->initializer);
    if (!init_t) return w->r->registry.t_any;
    CtscType* after_literal = is_const ? init_t : ctsc_type_widen(&w->r->registry, init_t);
    return widen_nullish_when_not_strict_null(&w->r->registry, after_literal);
}

static const char* syntax_kind_cstr(CtscSyntaxKind k) { return ctsc_syntax_kind_name(k); }

/* ----- walker ----- */

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
        CtscType* t = var_decl_type(w, d, is_const);
        push_entry_for_name(w, d, d->data.variableDeclaration.name, t);
        const CtscVariableDeclarationData* vd = &d->data.variableDeclaration;
        if (vd->type && vd->initializer) {
            CtscType* ann = type_of_type_node(&w->r->registry, w->source_utf16, w->source_utf16_len, vd->type);
            CtscType* init_t = type_of_expression(&w->r->registry, vd->initializer);
            if (!is_assignable_to(&w->r->registry, init_t, ann)) {
                emit_ts2322_on_binding_name(w->r, w->arena, &w->r->registry, vd->name, init_t, ann);
            }
        }
        /* Still walk initializer so identifier references inside get checked. */
        if (d->data.variableDeclaration.initializer) {
            visit(w, d->data.variableDeclaration.initializer);
        }
    }
}

static CtscType* param_type(Walk* w, const CtscNode* param) {
    const CtscParameterData* p = &param->data.parameter;
    if (p->type) return type_of_type_node(&w->r->registry, w->source_utf16, w->source_utf16_len, p->type);
    return w->r->registry.t_any;
}

/*
 * Aggregate ReturnStatement expression types inside a function body, mirroring
 * checker.ts getReturnTypeFromBody (~39195-39250) + getWidenedType (~39276-39277).
 * Nested function / arrow / class bodies are not descended into so inner
 * returns do not affect the outer signature.
 */
typedef struct {
    CtscTypeRegistry* reg;
    CtscArena*        arena;
    CtscType**        items;
    size_t            len;
    size_t            cap;
} RetCollector;

static void ret_collector_push(RetCollector* c, CtscType* t) {
    if (!t) return;
    if (c->len + 1 > c->cap) {
        size_t ncap = c->cap ? c->cap * 2 : 8;
        CtscType** nb = (CtscType**)ctsc_arena_alloc(c->arena, ncap * sizeof(CtscType*));
        if (c->items) memcpy(nb, c->items, c->len * sizeof(CtscType*));
        c->items = nb;
        c->cap = ncap;
    }
    c->items[c->len++] = t;
}

static void collect_return_expression_types(RetCollector* c, const CtscNode* n);

static void collect_return_types_from_case_block(RetCollector* c, const CtscNode* case_block) {
    if (!case_block || case_block->kind != CTSC_SK_CaseBlock) return;
    const CtscNodeArray* clauses = &case_block->data.caseBlock.clauses;
    for (size_t i = 0; i < clauses->len; ++i) {
        const CtscNode* cl = clauses->items[i];
        if (!cl) continue;
        if (cl->kind == CTSC_SK_CaseClause) {
            const CtscNodeArray* ss = &cl->data.caseClause.statements;
            for (size_t j = 0; j < ss->len; ++j) {
                collect_return_expression_types(c, ss->items[j]);
            }
        } else if (cl->kind == CTSC_SK_DefaultClause) {
            const CtscNodeArray* ss = &cl->data.defaultClause.statements;
            for (size_t j = 0; j < ss->len; ++j) {
                collect_return_expression_types(c, ss->items[j]);
            }
        }
    }
}

static void collect_return_expression_types(RetCollector* c, const CtscNode* n) {
    if (!n) return;
    switch (n->kind) {
        case CTSC_SK_Block: {
            const CtscNodeArray* stmts = &n->data.block.statements;
            for (size_t i = 0; i < stmts->len; ++i) {
                collect_return_expression_types(c, stmts->items[i]);
            }
            return;
        }
        case CTSC_SK_ReturnStatement:
            if (n->data.returnStatement.expression) {
                ret_collector_push(c, type_of_expression(c->reg, n->data.returnStatement.expression));
            }
            return;
        case CTSC_SK_FunctionDeclaration:
        case CTSC_SK_FunctionExpression:
        case CTSC_SK_ArrowFunction:
        case CTSC_SK_ClassDeclaration:
            return;
        case CTSC_SK_IfStatement:
            collect_return_expression_types(c, n->data.ifStatement.thenStatement);
            collect_return_expression_types(c, n->data.ifStatement.elseStatement);
            return;
        case CTSC_SK_WhileStatement:
            collect_return_expression_types(c, n->data.whileStatement.statement);
            return;
        case CTSC_SK_DoStatement:
            collect_return_expression_types(c, n->data.doStatement.statement);
            return;
        case CTSC_SK_ForStatement:
            collect_return_expression_types(c, n->data.forStatement.statement);
            return;
        case CTSC_SK_ForInStatement:
        case CTSC_SK_ForOfStatement:
            collect_return_expression_types(c, n->data.forInOrOfStatement.statement);
            return;
        case CTSC_SK_TryStatement: {
            const CtscTryStatementData* tr = &n->data.tryStatement;
            collect_return_expression_types(c, tr->tryBlock);
            if (tr->catchClause && tr->catchClause->kind == CTSC_SK_CatchClause) {
                collect_return_expression_types(c, tr->catchClause->data.catchClause.block);
            }
            collect_return_expression_types(c, tr->finallyBlock);
            return;
        }
        case CTSC_SK_SwitchStatement:
            collect_return_types_from_case_block(c, n->data.switchStatement.caseBlock);
            return;
        case CTSC_SK_LabeledStatement:
            collect_return_expression_types(c, n->data.labeledStatement.statement);
            return;
        case CTSC_SK_WithStatement:
            collect_return_expression_types(c, n->data.withStatement.statement);
            return;
        default:
            /* e.g. VariableStatement / ExpressionStatement: no statement-level
             * ReturnStatement descendants that belong to this function. */
            return;
    }
}

static CtscType* widen_inferred_return_types(CtscTypeRegistry* reg, CtscArena* arena,
                                            CtscType** raw, size_t n) {
    if (n == 0) return reg->t_void;
    CtscType** uniq = (CtscType**)ctsc_arena_alloc(arena, n * sizeof(CtscType*));
    size_t ulen = 0;
    for (size_t i = 0; i < n; ++i) {
        CtscType* w = widen_nullish_when_not_strict_null(reg, ctsc_type_widen(reg, raw[i]));
        bool seen = false;
        for (size_t j = 0; j < ulen; ++j) {
            if (uniq[j] == w) {
                seen = true;
                break;
            }
        }
        if (!seen) uniq[ulen++] = w;
    }
    if (ulen == 0) return reg->t_void;
    if (ulen == 1) return uniq[0];
    {
        CtscType* u = ctsc_type_new(reg, CTSC_TYPE_UNION);
        u->union_members = (CtscType**)ctsc_arena_alloc(arena, ulen * sizeof(CtscType*));
        memcpy(u->union_members, uniq, ulen * sizeof(CtscType*));
        u->union_members_len = ulen;
        return u;
    }
}

static CtscType* infer_return_type_for_function_body(CtscTypeRegistry* reg, CtscArena* arena,
                                                     const CtscNode* body) {
    if (!body || body->kind != CTSC_SK_Block) return reg->t_void;
    RetCollector c;
    memset(&c, 0, sizeof(c));
    c.reg = reg;
    c.arena = arena;
    const CtscNodeArray* stmts = &body->data.block.statements;
    for (size_t i = 0; i < stmts->len; ++i) {
        collect_return_expression_types(&c, stmts->items[i]);
    }
    return widen_inferred_return_types(reg, arena, c.items, c.len);
}

static void emit_function_signature_string(CtscBuffer* out,
                                           const CtscNodeArray* params,
                                           CtscTypeRegistry* reg,
                                           const CtscNode* return_type_node,
                                           CtscType* inferred_return_type,
                                           const uint16_t* src, size_t src_len) {
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
        if (p->data.parameter.type) {
            append_type_for_signature(out, p->data.parameter.type, src, src_len);
        } else {
            ctsc_type_to_string(reg->t_any, out);
        }
    }
    ctsc_buf_append_cstr(out, ") => ");
    if (return_type_node) {
        append_type_for_signature(out, return_type_node, src, src_len);
    } else if (inferred_return_type) {
        ctsc_type_to_string(inferred_return_type, out);
    } else {
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
        CtscType* inferred_ret = infer_return_type_for_function_body(&w->r->registry, w->arena, f->body);
        CtscBuffer sig; ctsc_buf_init(&sig);
        emit_function_signature_string(&sig, &f->parameters, &w->r->registry, NULL, inferred_ret,
                                       w->source_utf16, w->source_utf16_len);
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
        push_entry_for_name(w, p, p->data.parameter.name, param_type(w, p));
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
            w->source_utf16 = n->data.sourceFile.text_utf16;
            w->source_utf16_len = n->data.sourceFile.text_utf16_len;
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

        case CTSC_SK_ArrayLiteralExpression:
            walk_children_nodearray(w, &n->data.arrayLiteralExpression.elements);
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

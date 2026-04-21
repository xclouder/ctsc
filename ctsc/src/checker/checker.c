#include "ctsc/checker.h"
#include "ctsc/arena.h"
#include "ctsc/scanner.h"

#include <limits.h>
#include <stdint.h>
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
 *     assignable to the annotation. When the initializer is an object
 *     literal checked against an interface / type-literal shape, the span
 *     and message target the first incompatible property initializer
 *     (checker.ts checkPropertyAssignment ~41503, checkVariableDeclaration
 *     ~45282); otherwise the error is on the binding name.
 *   - TS2345 when a call argument is not assignable to the corresponding
 *     parameter of a resolved FunctionDeclaration, mirroring checker.ts
 *     getSignatureApplicabilityError (~36181,
 *     Diagnostics.Argument_of_type_0_is_not_assignable_to_parameter_of_type_1).
 *   - TS2554 when a call supplies fewer or more arguments than the callee's
 *     arity allows (checker.ts getArgumentArityError ~36410-36494,
 *     Diagnostics.Expected_0_arguments_but_got_1). Too few: span on the
 *     callee via getDiagnosticSpanForCallNode ~36359-36362; too many: span on
 *     excess argument expressions ~36479-36493.
 *   - TS2339 when a property access reads a name that is not on an inferred
 *     anonymous object literal type (checker.ts checkPropertyAccessExpression
 *     ~34948-34968, Diagnostics.Property_0_does_not_exist_on_type_1; error
 *     span on the Identifier after `.`).
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
    /*
     * VariableDeclaration* → whether its VariableDeclarationList was const.
     * Populated before inferring types in each VariableStatement so
     * `const a = 1, b = a` can resolve `a` with the correct const/let widening
     * (checker.ts getTypeOfSymbol → getTypeOfVariableOrParameterOrPropertyWorker
     * ~12537, getWidenedTypeForVariableLikeDeclaration ~12451).
     */
    const CtscNode** var_decl_const_keys;
    bool*            var_decl_const_vals;
    size_t           var_decl_const_len;
    size_t           var_decl_const_cap;
    /*
     * BindingElement* → inferred type for object/array destructuring (checker.ts
     * getTypeFromBindingElement / checkBindingElement ~45289, getTypeFromBindingPattern ~12433).
     */
    const CtscNode** binding_elem_type_keys;
    CtscType**       binding_elem_type_vals;
    size_t           binding_elem_type_len;
    size_t           binding_elem_type_cap;
    /*
     * typeof-narrowing for `if (typeof id === "string" | "number" | "boolean")` / else branch
     * (checker.ts narrowTypeByTypeFacts / getNarrowedType ~29865+, ~29871, ~38000+).
     */
    CtscSymbol* narrow_typeof_eq_string_sym;
    CtscType*   narrow_typeof_eq_string_type;
    /*
     * `if (id === "lit")` / else (checker.ts narrowTypeByEquality ~29752-29787).
     */
    CtscSymbol* narrow_eq_literal_sym;
    CtscType*   narrow_eq_literal_type;
} Walk;

static void var_decl_const_register(Walk* w, const CtscNode* decl, bool is_const) {
    if (!w || !decl) return;
    if (w->var_decl_const_len + 1 > w->var_decl_const_cap) {
        size_t ncap = w->var_decl_const_cap ? w->var_decl_const_cap * 2 : 16;
        const CtscNode** nk = (const CtscNode**)ctsc_arena_alloc(w->arena, ncap * sizeof(CtscNode*));
        bool* nv = (bool*)ctsc_arena_alloc(w->arena, ncap * sizeof(bool));
        if (w->var_decl_const_keys) {
            memcpy(nk, w->var_decl_const_keys, w->var_decl_const_len * sizeof(CtscNode*));
            memcpy(nv, w->var_decl_const_vals, w->var_decl_const_len * sizeof(bool));
        }
        w->var_decl_const_keys = nk;
        w->var_decl_const_vals = nv;
        w->var_decl_const_cap = ncap;
    }
    w->var_decl_const_keys[w->var_decl_const_len] = decl;
    w->var_decl_const_vals[w->var_decl_const_len] = is_const;
    w->var_decl_const_len++;
}

static bool var_decl_is_const(Walk* w, const CtscNode* decl) {
    if (!w || !decl) return false;
    for (size_t i = 0; i < w->var_decl_const_len; ++i) {
        if (w->var_decl_const_keys[i] == decl) return w->var_decl_const_vals[i];
    }
    return false;
}

static void binding_elem_type_register(Walk* w, const CtscNode* be, CtscType* t) {
    if (!w || !be) return;
    if (w->binding_elem_type_len + 1 > w->binding_elem_type_cap) {
        size_t ncap = w->binding_elem_type_cap ? w->binding_elem_type_cap * 2 : 16;
        const CtscNode** nk = (const CtscNode**)ctsc_arena_alloc(w->arena, ncap * sizeof(CtscNode*));
        CtscType** nv = (CtscType**)ctsc_arena_alloc(w->arena, ncap * sizeof(CtscType*));
        if (w->binding_elem_type_keys) {
            memcpy(nk, w->binding_elem_type_keys, w->binding_elem_type_len * sizeof(CtscNode*));
            memcpy(nv, w->binding_elem_type_vals, w->binding_elem_type_len * sizeof(CtscType*));
        }
        w->binding_elem_type_keys = nk;
        w->binding_elem_type_vals = nv;
        w->binding_elem_type_cap = ncap;
    }
    w->binding_elem_type_keys[w->binding_elem_type_len] = be;
    w->binding_elem_type_vals[w->binding_elem_type_len] = t ? t : w->r->registry.t_any;
    w->binding_elem_type_len++;
}

static CtscType* binding_elem_type_lookup(Walk* w, const CtscNode* be) {
    if (!w || !be) return NULL;
    for (size_t i = 0; i < w->binding_elem_type_len; ++i) {
        if (w->binding_elem_type_keys[i] == be) return w->binding_elem_type_vals[i];
    }
    return NULL;
}

/* ----- type inference helpers ----- */

static int u16_span_trim_bounds(const uint16_t* s, int pos, int end, int* out_end);

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

static void append_trimmed_type_span_for_signature(CtscBuffer* out, const uint16_t* src, int pos, int end);
static double numeric_literal_to_double(const CtscNumericLiteralData* d);

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
            } else if (!tr->typeName) {
                append_trimmed_type_span_for_signature(out, src, type_node->pos, type_node->end);
                return;
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
        /*
         * LiteralTypeNode variants (parser.ts parseLiteralTypeNode ~4610):
         * string / numeric / bigint / boolean literals used as type
         * annotations stringify back to themselves under typeToString
         * (checker.ts typeToString on LiteralType ~6820+). Without this
         * the overload signature emitter loses the `"a"` in
         * `(x: "a"): number;` and regresses to `(x: any)`.
         */
        case CTSC_SK_StringLiteral: {
            const CtscStringLiteralData* d = &type_node->data.stringLiteral;
            const uint16_t* t = d->value ? d->value : d->text;
            size_t n = d->value ? d->value_len : d->text_len;
            ctsc_buf_append_char(out, '"');
            for (size_t i = 0; i < n; ++i) {
                uint16_t u = t[i];
                if (u == '"')  { ctsc_buf_append(out, "\\\"", 2); continue; }
                if (u == '\\') { ctsc_buf_append(out, "\\\\", 2); continue; }
                if (u == '\n') { ctsc_buf_append(out, "\\n",  2); continue; }
                if (u == '\r') { ctsc_buf_append(out, "\\r",  2); continue; }
                if (u == '\t') { ctsc_buf_append(out, "\\t",  2); continue; }
                if (u < 0x20) {
                    char buf[8];
                    int m = snprintf(buf, sizeof(buf), "\\u%04x", (unsigned)u);
                    if (m > 0) ctsc_buf_append(out, buf, (size_t)m);
                    continue;
                }
                if (u < 0x80) { ctsc_buf_append_char(out, (char)u); continue; }
                if (u < 0x800) {
                    char b[2];
                    b[0] = (char)(0xC0 | (u >> 6));
                    b[1] = (char)(0x80 | (u & 0x3F));
                    ctsc_buf_append(out, b, 2);
                } else {
                    char b[3];
                    b[0] = (char)(0xE0 | (u >> 12));
                    b[1] = (char)(0x80 | ((u >> 6) & 0x3F));
                    b[2] = (char)(0x80 | (u & 0x3F));
                    ctsc_buf_append(out, b, 3);
                }
            }
            ctsc_buf_append_char(out, '"');
            return;
        }
        case CTSC_SK_NumericLiteral: {
            const CtscNumericLiteralData* d = &type_node->data.numericLiteral;
            double v = numeric_literal_to_double(d);
            if ((double)(long long)v == v) {
                char buf[32];
                int m = snprintf(buf, sizeof(buf), "%lld", (long long)v);
                if (m > 0) ctsc_buf_append(out, buf, (size_t)m);
            } else {
                char buf[64];
                int m = snprintf(buf, sizeof(buf), "%g", v);
                if (m > 0) ctsc_buf_append(out, buf, (size_t)m);
            }
            return;
        }
        case CTSC_SK_BigIntLiteral: {
            const CtscNumericLiteralData* d = &type_node->data.numericLiteral;
            size_t len = d->text_len;
            if (len > 0 && d->text[len - 1] == (uint16_t)'n') len--;
            for (size_t i = 0; i < len; ++i) {
                uint16_t u = d->text[i];
                if (u < 0x80) ctsc_buf_append_char(out, (char)u);
            }
            ctsc_buf_append_char(out, 'n');
            return;
        }
        case CTSC_SK_TrueKeyword:  ctsc_buf_append_cstr(out, "true");  return;
        case CTSC_SK_FalseKeyword: ctsc_buf_append_cstr(out, "false"); return;
        default:
            ctsc_buf_append_cstr(out, "any");
            return;
    }
}

/*
 * Parser fallback for complex type annotations (e.g. `number | string`) yields a
 * TypeReference with no typeName (parser.c parse_type_node ~571). Mirror
 * checker.ts getTypeFromTypeNode (~15000+) by recovering a CtscType from the
 * UTF-16 span between pos/end.
 */
static int u16_span_trim_bounds(const uint16_t* s, int pos, int end, int* out_end) {
    while (pos < end && ctsc_is_ws_u16(s[pos])) pos++;
    while (end > pos && ctsc_is_ws_u16(s[end - 1])) end--;
    *out_end = end;
    return pos;
}

/*
 * Fallback TypeReference nodes (parser consume_type_via_fallback_scan ~2870 for
 * `string | number`) carry only pos/end. Mirror checker.ts typeToString on the
 * recovered span (checker.ts ~15000+ / signature writer ~6202).
 */
static void append_trimmed_type_span_for_signature(CtscBuffer* out, const uint16_t* src, int pos, int end) {
    if (!src || pos < 0 || end < 0 || pos >= end) {
        ctsc_buf_append_cstr(out, "any");
        return;
    }
    int te;
    int a = u16_span_trim_bounds(src, pos, end, &te);
    if (a >= te) {
        ctsc_buf_append_cstr(out, "any");
        return;
    }
    for (int i = a; i < te; ++i) {
        uint16_t u = src[i];
        if (u < 0x80) {
            ctsc_buf_append_char(out, (char)u);
        } else if (u < 0x800) {
            char b[2];
            b[0] = (char)(0xC0 | (u >> 6));
            b[1] = (char)(0x80 | (u & 0x3F));
            ctsc_buf_append(out, b, 2);
        } else {
            char b[3];
            b[0] = (char)(0xE0 | (u >> 12));
            b[1] = (char)(0x80 | ((u >> 6) & 0x3F));
            b[2] = (char)(0x80 | (u & 0x3F));
            ctsc_buf_append(out, b, 3);
        }
    }
}

static bool u16_span_eq_ascii(const uint16_t* s, int a, int b, const char* lit) {
    size_t L = strlen(lit);
    if (b - a != (int)L) return false;
    for (size_t i = 0; i < L; i++) {
        if (s[a + (int)i] != (uint16_t)(unsigned char)lit[i]) return false;
    }
    return true;
}

static CtscType* string_literal_type_from_annotation_quotes(CtscTypeRegistry* reg, const uint16_t* s, int a, int b);

static CtscType* primitive_type_from_trimmed_span(CtscTypeRegistry* reg,
                                                  const uint16_t* s, int a, int b) {
    if (a >= b) return NULL;
    if (u16_span_eq_ascii(s, a, b, "undefined")) return reg->t_undefined;
    if (u16_span_eq_ascii(s, a, b, "boolean")) return reg->t_boolean;
    if (u16_span_eq_ascii(s, a, b, "bigint")) return reg->t_bigint;
    if (u16_span_eq_ascii(s, a, b, "number")) return reg->t_number;
    if (u16_span_eq_ascii(s, a, b, "string")) return reg->t_string;
    if (u16_span_eq_ascii(s, a, b, "symbol")) return reg->t_symbol;
    if (u16_span_eq_ascii(s, a, b, "object")) return reg->t_object;
    if (u16_span_eq_ascii(s, a, b, "unknown")) return reg->t_unknown;
    if (u16_span_eq_ascii(s, a, b, "never")) return reg->t_never;
    if (u16_span_eq_ascii(s, a, b, "any")) return reg->t_any;
    if (u16_span_eq_ascii(s, a, b, "void")) return reg->t_void;
    if (u16_span_eq_ascii(s, a, b, "null")) return reg->t_null;
    /*
     * Literal type node `"..."` / `'...'` recovered from UTF-16 span (checker.ts
     * parseLiteralTypeNode ~4610-4612; getTypeFromTypeNode for string literal).
     */
    {
        CtscType* lit = string_literal_type_from_annotation_quotes(reg, s, a, b);
        if (lit) return lit;
    }
    return NULL;
}

/*
 * Parse a string-literal type token from a trimmed annotation span; NULL if not
 * a quote-balanced literal type.
 */
static CtscType* string_literal_type_from_annotation_quotes(CtscTypeRegistry* reg, const uint16_t* s, int a, int b) {
    if (!reg || !s || b - a < 2) return NULL;
    uint16_t q = s[a];
    if ((q != (uint16_t)'"' && q != (uint16_t)'\'') || s[b - 1] != q) return NULL;
    int inner_end = b - 1; /* index of closing quote */
    if (inner_end == a + 1) {
        return ctsc_type_string_literal(reg, NULL, 0);
    }
    uint16_t* dst = (uint16_t*)ctsc_arena_alloc(reg->arena, (size_t)(inner_end - (a + 1)) * sizeof(uint16_t));
    size_t w = 0;
    int i = a + 1;
    while (i < inner_end) {
        if (s[i] == (uint16_t)'\\' && i + 1 < inner_end) {
            uint16_t nxt = s[i + 1];
            if (nxt == (uint16_t)'"' || nxt == (uint16_t)'\''
                || nxt == (uint16_t)'\\') {
                dst[w++] = nxt;
                i += 2;
                continue;
            }
            if (nxt == (uint16_t)'n') {
                dst[w++] = (uint16_t)'\n';
                i += 2;
                continue;
            }
            if (nxt == (uint16_t)'r') {
                dst[w++] = (uint16_t)'\r';
                i += 2;
                continue;
            }
            if (nxt == (uint16_t)'t') {
                dst[w++] = (uint16_t)'\t';
                i += 2;
                continue;
            }
        }
        dst[w++] = s[i++];
    }
    return ctsc_type_string_literal(reg, dst, w);
}

static bool u16_is_ident_start_ascii(uint16_t c) {
    return (c >= (uint16_t)'a' && c <= (uint16_t)'z')
        || (c >= (uint16_t)'A' && c <= (uint16_t)'Z')
        || c == (uint16_t)'_' || c == (uint16_t)'$';
}

static bool u16_is_ident_part_ascii(uint16_t c) {
    return u16_is_ident_start_ascii(c) || (c >= (uint16_t)'0' && c <= (uint16_t)'9');
}

/*
 * Single identifier type reference from a trimmed span (`A` in `A & B`;
 * checker.ts getTypeFromTypeNode for TypeReference ~15000+).
 */
static CtscType* named_type_reference_from_trimmed_span(CtscTypeRegistry* reg, const uint16_t* s, int a, int b) {
    if (!reg || !s || a >= b) return NULL;
    if (!u16_is_ident_start_ascii(s[a])) return NULL;
    int i = a + 1;
    while (i < b && u16_is_ident_part_ascii(s[i])) i++;
    if (i != b) return NULL;
    return ctsc_type_reference(reg, s + a, (size_t)(b - a));
}

static CtscType* type_from_annotation_fallback_span(CtscTypeRegistry* reg, const uint16_t* src, size_t src_len,
                                                    int pos, int end);

/*
 * Comma-separated type list inside `[` ... `]` (tuple) or similar, respecting
 * nesting and string literals (mirrors delimiter logic in
 * type_from_annotation_fallback_span).
 */
static bool split_type_annotation_element_list(const uint16_t* s, int inner_a, int inner_b, int* el_starts,
                                              int* el_ends, size_t* out_n, size_t max_n) {
    int bd = 0, brd = 0, pd = 0, ad = 0;
    bool in_dbl = false, in_sgl = false;
    int seg0 = inner_a;
    size_t n = 0;
    for (int i = inner_a; i < inner_b; ++i) {
        uint16_t c = s[i];
        if (in_dbl) {
            if (c == (uint16_t)'\\' && i + 1 < inner_b) {
                i++;
                continue;
            }
            if (c == (uint16_t)'"') in_dbl = false;
            continue;
        }
        if (in_sgl) {
            if (c == (uint16_t)'\\' && i + 1 < inner_b) {
                i++;
                continue;
            }
            if (c == (uint16_t)'\'') in_sgl = false;
            continue;
        }
        if (c == (uint16_t)'"') {
            in_dbl = true;
            continue;
        }
        if (c == (uint16_t)'\'') {
            in_sgl = true;
            continue;
        }

        if (c == (uint16_t)'{') bd++;
        else if (c == (uint16_t)'}' && bd > 0) bd--;
        else if (c == (uint16_t)'[') brd++;
        else if (c == (uint16_t)']' && brd > 0) brd--;
        else if (c == (uint16_t)'(') pd++;
        else if (c == (uint16_t)')' && pd > 0) pd--;
        else if (c == (uint16_t)'<') ad++;
        else if (c == (uint16_t)'>' && ad > 0) ad--;
        else if (c == (uint16_t)',' && bd == 0 && brd == 0 && pd == 0 && ad == 0) {
            if (n >= max_n) return false;
            int te2 = inner_b;
            int ss = u16_span_trim_bounds(s, seg0, i, &te2);
            el_starts[n] = ss;
            el_ends[n] = te2;
            n++;
            seg0 = i + 1;
        }
    }
    if (n >= max_n) return false;
    int te2 = inner_b;
    int ss = u16_span_trim_bounds(s, seg0, inner_b, &te2);
    el_starts[n] = ss;
    el_ends[n] = te2;
    n++;
    *out_n = n;
    return true;
}

/*
 * Tuple type node text `[T0, T1, ...]` recovered from a parser fallback span
 * (checker.ts getTypeFromArrayOrTupleTypeNode ~17824-17840).
 */
static CtscType* tuple_type_from_annotation_span(CtscTypeRegistry* reg, const uint16_t* s, size_t src_len, int a,
                                                int b) {
    int te = b;
    a = u16_span_trim_bounds(s, a, b, &te);
    b = te;
    if (!s || a < 0 || b < 0 || a > (int)src_len || b > (int)src_len || a >= b) return NULL;
    if (s[a] != (uint16_t)'[') return NULL;

    int depth = 0;
    bool in_dbl = false, in_sgl = false;
    int close_i = -1;
    for (int i = a; i < b; ++i) {
        uint16_t c = s[i];
        if (in_dbl) {
            if (c == (uint16_t)'\\' && i + 1 < b) {
                i++;
                continue;
            }
            if (c == (uint16_t)'"') in_dbl = false;
            continue;
        }
        if (in_sgl) {
            if (c == (uint16_t)'\\' && i + 1 < b) {
                i++;
                continue;
            }
            if (c == (uint16_t)'\'') in_sgl = false;
            continue;
        }
        if (c == (uint16_t)'"') {
            in_dbl = true;
            continue;
        }
        if (c == (uint16_t)'\'') {
            in_sgl = true;
            continue;
        }

        if (c == (uint16_t)'[') depth++;
        else if (c == (uint16_t)']') {
            depth--;
            if (depth == 0) {
                close_i = i;
                break;
            }
        }
    }
    if (close_i < 0 || close_i != b - 1) return NULL;

    int inner_a = a + 1;
    int inner_b = close_i;
    int ia = u16_span_trim_bounds(s, inner_a, inner_b, &te);
    int ib = te;
    if (ia >= ib) return ctsc_type_tuple(reg, NULL, 0);

    int el_st[32], el_en[32];
    size_t nel = 0;
    if (!split_type_annotation_element_list(s, ia, ib, el_st, el_en, &nel, 32)) return NULL;

    CtscType* elems_storage[32];
    for (size_t ei = 0; ei < nel; ++ei) {
        if (el_st[ei] >= el_en[ei]) return NULL;
        elems_storage[ei] = type_from_annotation_fallback_span(reg, s, src_len, el_st[ei], el_en[ei]);
    }
    return ctsc_type_tuple(reg, elems_storage, nel);
}

/*
 * One side of `|` after outer union split: `T0 & T1 & ...` where `&` binds
 * tighter than `|` (checker.ts parseIntersectionType / getIntersectionType ~14510+).
 */
static CtscType* type_from_annotation_intersection_side(CtscTypeRegistry* reg, const uint16_t* src, size_t src_len,
                                                        int pos, int end) {
    if (!src || pos < 0 || end < 0 || pos > (int)src_len || end > (int)src_len || pos >= end) {
        return reg->t_any;
    }
    int te = end;
    pos = u16_span_trim_bounds(src, pos, end, &te);
    end = te;
    if (pos >= end) return reg->t_any;

    int span_starts[32];
    int span_ends[32];
    size_t nsp = 0;

    int bd = 0, brd = 0, pd = 0, ad = 0;
    bool in_dbl = false, in_sgl = false;
    int seg0 = pos;

    for (int i = pos; i < end; ++i) {
        uint16_t c = src[i];
        if (in_dbl) {
            if (c == (uint16_t)'\\' && i + 1 < end) {
                i++;
                continue;
            }
            if (c == (uint16_t)'"') in_dbl = false;
            continue;
        }
        if (in_sgl) {
            if (c == (uint16_t)'\\' && i + 1 < end) {
                i++;
                continue;
            }
            if (c == (uint16_t)'\'') in_sgl = false;
            continue;
        }
        if (c == (uint16_t)'"') {
            in_dbl = true;
            continue;
        }
        if (c == (uint16_t)'\'') {
            in_sgl = true;
            continue;
        }

        if (c == (uint16_t)'{') bd++;
        else if (c == (uint16_t)'}' && bd > 0) bd--;
        else if (c == (uint16_t)'[') brd++;
        else if (c == (uint16_t)']' && brd > 0) brd--;
        else if (c == (uint16_t)'(') pd++;
        else if (c == (uint16_t)')' && pd > 0) pd--;
        else if (c == (uint16_t)'<') ad++;
        else if (c == (uint16_t)'>' && ad > 0) ad--;

        if (c == (uint16_t)'&' && bd == 0 && brd == 0 && pd == 0 && ad == 0) {
            if (nsp >= sizeof(span_starts) / sizeof(span_starts[0])) return reg->t_any;
            int ss = u16_span_trim_bounds(src, seg0, i, &te);
            span_starts[nsp] = ss;
            span_ends[nsp] = te;
            nsp++;
            seg0 = i + 1;
        }
    }
    if (nsp >= sizeof(span_starts) / sizeof(span_starts[0])) return reg->t_any;
    {
        int ss = u16_span_trim_bounds(src, seg0, end, &te);
        span_starts[nsp] = ss;
        span_ends[nsp] = te;
        nsp++;
    }

    CtscType* parts[32];
    size_t np = 0;
    for (size_t si = 0; si < nsp; ++si) {
        CtscType* p = tuple_type_from_annotation_span(reg, src, src_len, span_starts[si], span_ends[si]);
        if (!p) p = primitive_type_from_trimmed_span(reg, src, span_starts[si], span_ends[si]);
        if (!p) p = named_type_reference_from_trimmed_span(reg, src, span_starts[si], span_ends[si]);
        if (!p) return reg->t_any;
        parts[np++] = p;
    }

    size_t w = 0;
    for (size_t i = 0; i < np; ++i) {
        if (w == 0 || parts[w - 1] != parts[i]) parts[w++] = parts[i];
    }
    np = w;

    if (np == 0) return reg->t_any;
    if (np == 1) return parts[0];
    return ctsc_type_intersection(reg, parts, np);
}

/*
 * Intrinsic-only unions are ordered for typeToString the same way tsc orders
 * union constituents (checker.ts getUnionType ~14780+ / typeToString).
 */
static int intrinsic_union_emit_rank(const CtscType* t) {
    switch (t->kind) {
        case CTSC_TYPE_STRING:      return 10;
        case CTSC_TYPE_NUMBER:      return 20;
        case CTSC_TYPE_BOOLEAN:     return 30;
        case CTSC_TYPE_BIGINT:      return 40;
        case CTSC_TYPE_SYMBOL:      return 50;
        case CTSC_TYPE_UNDEFINED:   return 60;
        case CTSC_TYPE_NULL:        return 70;
        case CTSC_TYPE_VOID:        return 80;
        case CTSC_TYPE_OBJECT:      return 90;
        case CTSC_TYPE_NEVER:       return 100;
        case CTSC_TYPE_UNKNOWN_T:   return 110;
        case CTSC_TYPE_ANY:         return 120;
        default:                    return 9999;
    }
}

static bool union_is_all_sortable_intrinsics(const CtscType* u) {
    if (!u || u->kind != CTSC_TYPE_UNION) return false;
    for (size_t i = 0; i < u->union_members_len; ++i) {
        if (intrinsic_union_emit_rank(u->union_members[i]) >= 9999) return false;
    }
    return u->union_members_len > 1;
}

static void sort_intrinsic_union_members(CtscType* u) {
    if (!union_is_all_sortable_intrinsics(u)) return;
    for (size_t i = 1; i < u->union_members_len; ++i) {
        CtscType* key = u->union_members[i];
        int rk = intrinsic_union_emit_rank(key);
        size_t j = i;
        while (j > 0 && intrinsic_union_emit_rank(u->union_members[j - 1]) > rk) {
            u->union_members[j] = u->union_members[j - 1];
            j--;
        }
        u->union_members[j] = key;
    }
}

static CtscType* type_from_annotation_fallback_span(CtscTypeRegistry* reg, const uint16_t* src, size_t src_len,
                                                    int pos, int end) {
    if (!src || pos < 0 || end < 0 || pos > (int)src_len || end > (int)src_len || pos >= end) {
        return reg->t_any;
    }
    int te = end;
    pos = u16_span_trim_bounds(src, pos, end, &te);
    end = te;
    if (pos >= end) return reg->t_any;

    int span_starts[32];
    int span_ends[32];
    size_t nsp = 0;

    int bd = 0, brd = 0, pd = 0, ad = 0;
    bool in_dbl = false, in_sgl = false;
    int seg0 = pos;

    for (int i = pos; i < end; ++i) {
        uint16_t c = src[i];
        if (in_dbl) {
            if (c == (uint16_t)'\\' && i + 1 < end) {
                i++;
                continue;
            }
            if (c == (uint16_t)'"') in_dbl = false;
            continue;
        }
        if (in_sgl) {
            if (c == (uint16_t)'\\' && i + 1 < end) {
                i++;
                continue;
            }
            if (c == (uint16_t)'\'') in_sgl = false;
            continue;
        }
        if (c == (uint16_t)'"') {
            in_dbl = true;
            continue;
        }
        if (c == (uint16_t)'\'') {
            in_sgl = true;
            continue;
        }

        if (c == (uint16_t)'{') bd++;
        else if (c == (uint16_t)'}' && bd > 0) bd--;
        else if (c == (uint16_t)'[') brd++;
        else if (c == (uint16_t)']' && brd > 0) brd--;
        else if (c == (uint16_t)'(') pd++;
        else if (c == (uint16_t)')' && pd > 0) pd--;
        else if (c == (uint16_t)'<') ad++;
        else if (c == (uint16_t)'>' && ad > 0) ad--;

        if (c == (uint16_t)'|' && bd == 0 && brd == 0 && pd == 0 && ad == 0) {
            if (nsp >= sizeof(span_starts) / sizeof(span_starts[0])) return reg->t_any;
            int ss = u16_span_trim_bounds(src, seg0, i, &te);
            span_starts[nsp] = ss;
            span_ends[nsp] = te;
            nsp++;
            seg0 = i + 1;
        }
    }
    if (nsp >= sizeof(span_starts) / sizeof(span_starts[0])) return reg->t_any;
    {
        int ss = u16_span_trim_bounds(src, seg0, end, &te);
        span_starts[nsp] = ss;
        span_ends[nsp] = te;
        nsp++;
    }

    CtscType* parts[32];
    size_t np = 0;
    for (size_t si = 0; si < nsp; ++si) {
        CtscType* p = type_from_annotation_intersection_side(reg, src, src_len, span_starts[si], span_ends[si]);
        if (!p) return reg->t_any;
        parts[np++] = p;
    }

    size_t w = 0;
    for (size_t i = 0; i < np; ++i) {
        if (w == 0 || parts[w - 1] != parts[i]) parts[w++] = parts[i];
    }
    np = w;

    if (np == 0) return reg->t_any;
    if (np == 1) return parts[0];

    CtscType* u = ctsc_type_new(reg, CTSC_TYPE_UNION);
    u->union_members = (CtscType**)ctsc_arena_alloc(reg->arena, np * sizeof(CtscType*));
    memcpy(u->union_members, parts, np * sizeof(CtscType*));
    u->union_members_len = np;
    sort_intrinsic_union_members(u);
    return u;
}

/*
 * Else-branch type after `typeof v === "string"` is truthy in the other branch:
 * exclude `string` and string-literal constituents (checker.ts getNarrowedType
 * / narrowTypeByTypeFacts ~38000+).
 */
static CtscType* narrow_typeof_string_compare_else_type(CtscTypeRegistry* reg, CtscType* t) {
    if (!reg || !t) return reg->t_any;
    if (t == reg->t_string) return reg->t_never;
    if (t->kind != CTSC_TYPE_UNION) return t;
    CtscType* buf[32];
    size_t n = 0;
    for (size_t i = 0; i < t->union_members_len && n < 32; ++i) {
        CtscType* m = t->union_members[i];
        if (!m) continue;
        if (m == reg->t_string) continue;
        if (m->kind == CTSC_TYPE_STRING_LITERAL) continue;
        buf[n++] = m;
    }
    if (n == 0) return reg->t_never;
    if (n == 1) return buf[0];
    CtscType* u = ctsc_type_new(reg, CTSC_TYPE_UNION);
    u->union_members = (CtscType**)ctsc_arena_alloc(reg->arena, n * sizeof(CtscType*));
    memcpy(u->union_members, buf, n * sizeof(CtscType*));
    u->union_members_len = n;
    sort_intrinsic_union_members(u);
    return u;
}

/*
 * Else-branch type after `typeof v === "number"` in the truthy branch
 * (checker.ts narrowTypeByTypeFacts / TypeofEQNumber ~29867, ~29884+).
 */
static CtscType* narrow_typeof_number_compare_else_type(CtscTypeRegistry* reg, CtscType* t) {
    if (!reg || !t) return reg->t_any;
    if (t == reg->t_number) return reg->t_never;
    if (t->kind != CTSC_TYPE_UNION) return t;
    CtscType* buf[32];
    size_t n = 0;
    for (size_t i = 0; i < t->union_members_len && n < 32; ++i) {
        CtscType* m = t->union_members[i];
        if (!m) continue;
        if (m == reg->t_number) continue;
        if (m->kind == CTSC_TYPE_NUMBER_LITERAL) continue;
        buf[n++] = m;
    }
    if (n == 0) return reg->t_never;
    if (n == 1) return buf[0];
    CtscType* u = ctsc_type_new(reg, CTSC_TYPE_UNION);
    u->union_members = (CtscType**)ctsc_arena_alloc(reg->arena, n * sizeof(CtscType*));
    memcpy(u->union_members, buf, n * sizeof(CtscType*));
    u->union_members_len = n;
    sort_intrinsic_union_members(u);
    return u;
}

/*
 * Else-branch type after `typeof v === "boolean"` in the truthy branch
 * (checker.ts narrowTypeByTypeFacts / TypeofEQBoolean ~29871, ~29884+).
 */
static CtscType* narrow_typeof_boolean_compare_else_type(CtscTypeRegistry* reg, CtscType* t) {
    if (!reg || !t) return reg->t_any;
    if (t == reg->t_boolean) return reg->t_never;
    if (t->kind != CTSC_TYPE_UNION) return t;
    CtscType* buf[32];
    size_t n = 0;
    for (size_t i = 0; i < t->union_members_len && n < 32; ++i) {
        CtscType* m = t->union_members[i];
        if (!m) continue;
        if (m == reg->t_boolean) continue;
        if (m->kind == CTSC_TYPE_BOOLEAN_LITERAL) continue;
        buf[n++] = m;
    }
    if (n == 0) return reg->t_never;
    if (n == 1) return buf[0];
    CtscType* u = ctsc_type_new(reg, CTSC_TYPE_UNION);
    u->union_members = (CtscType**)ctsc_arena_alloc(reg->arena, n * sizeof(CtscType*));
    memcpy(u->union_members, buf, n * sizeof(CtscType*));
    u->union_members_len = n;
    sort_intrinsic_union_members(u);
    return u;
}

static bool string_literal_value_eq(const uint16_t* at, size_t alen, const uint16_t* bt, size_t blen) {
    if (alen != blen) return false;
    if (alen == 0) return true;
    if (!at || !bt) return at == bt;
    return memcmp(at, bt, alen * sizeof(uint16_t)) == 0;
}

/*
 * False branch of `id === "lit"` (checker.ts narrowTypeByEquality ~29784-29786 when the
 * witness is a string literal unit type).
 */
static CtscType* narrow_string_literal_eq_else_type(CtscTypeRegistry* reg, CtscType* declared, const uint16_t* excl,
                                                   size_t excl_len) {
    if (!reg || !declared) return reg ? reg->t_any : NULL;
    if (declared->kind == CTSC_TYPE_STRING) return reg->t_string;
    if (declared->kind == CTSC_TYPE_ANY) return reg->t_any;
    if (declared->kind == CTSC_TYPE_STRING_LITERAL) {
        if (string_literal_value_eq(declared->text, declared->text_len, excl, excl_len)) return reg->t_never;
        return declared;
    }
    if (declared->kind != CTSC_TYPE_UNION) return declared;
    CtscType* buf[32];
    size_t n = 0;
    for (size_t i = 0; i < declared->union_members_len && n < 32; ++i) {
        CtscType* m = declared->union_members[i];
        if (!m) continue;
        if (m->kind == CTSC_TYPE_STRING_LITERAL
            && string_literal_value_eq(m->text, m->text_len, excl, excl_len))
            continue;
        buf[n++] = m;
    }
    if (n == 0) return reg->t_never;
    if (n == 1) return buf[0];
    CtscType* u = ctsc_type_new(reg, CTSC_TYPE_UNION);
    u->union_members = (CtscType**)ctsc_arena_alloc(reg->arena, n * sizeof(CtscType*));
    memcpy(u->union_members, buf, n * sizeof(CtscType*));
    u->union_members_len = n;
    return u;
}

/*
 * TypeLiteral nodes carry only pos/end (parser.c parse_type_literal ~2810); the
 * checker recovers PropertySignature types from the UTF-16 span as tsc does via
 * getTypeFromTypeNode (checker.ts ~15000+).
 */

/*
 * First `{` at depth 0 outside `<...>` type parameters (checker.ts interface
 * body after heritage / type params). Used when InterfaceDeclaration has no
 * TypeElement children (parser.ts brace-balanced skip).
 */
static int find_interface_body_open_brace(const uint16_t* src, size_t src_len, int pos, int end) {
    if (!src || pos < 0 || end > (int)src_len || pos >= end) return -1;
    int angle = 0;
    bool in_dbl = false, in_sgl = false;
    for (int i = pos; i < end; ++i) {
        uint16_t c = src[i];
        if (in_dbl) {
            if (c == (uint16_t)'\\' && i + 1 < end) {
                i++;
                continue;
            }
            if (c == (uint16_t)'"') in_dbl = false;
            continue;
        }
        if (in_sgl) {
            if (c == (uint16_t)'\\' && i + 1 < end) {
                i++;
                continue;
            }
            if (c == (uint16_t)'\'') in_sgl = false;
            continue;
        }
        if (c == (uint16_t)'"') {
            in_dbl = true;
            continue;
        }
        if (c == (uint16_t)'\'') {
            in_sgl = true;
            continue;
        }
        if (c == (uint16_t)'<') {
            angle++;
            continue;
        }
        if (c == (uint16_t)'>' && angle > 0) {
            angle--;
            continue;
        }
        if (c == (uint16_t)'{' && angle == 0) return i;
    }
    return -1;
}

static int type_literal_find_matching_brace(const uint16_t* s, size_t src_len, int open_pos, int limit) {
    if (!s || open_pos < 0 || open_pos >= (int)src_len || s[open_pos] != (uint16_t)'{') return -1;
    if (limit > (int)src_len) limit = (int)src_len;
    int depth = 0;
    bool in_dbl = false, in_sgl = false;
    for (int i = open_pos; i < limit; ++i) {
        uint16_t c = s[i];
        if (in_dbl) {
            if (c == (uint16_t)'\\' && i + 1 < limit) {
                i++;
                continue;
            }
            if (c == (uint16_t)'"') in_dbl = false;
            continue;
        }
        if (in_sgl) {
            if (c == (uint16_t)'\\' && i + 1 < limit) {
                i++;
                continue;
            }
            if (c == (uint16_t)'\'') in_sgl = false;
            continue;
        }
        if (c == (uint16_t)'"') {
            in_dbl = true;
            continue;
        }
        if (c == (uint16_t)'\'') {
            in_sgl = true;
            continue;
        }
        if (c == (uint16_t)'{') depth++;
        else if (c == (uint16_t)'}') {
            depth--;
            if (depth == 0) return i;
        }
    }
    return -1;
}

/*
 * End of the type in `Name : Type` inside a TypeLiteral body [body_start, body_end).
 * Stops at a top-level `,` or at body_end.
 */
static int type_literal_scan_member_type_end(const uint16_t* s, int type_start, int body_end) {
    int bd = 0, brd = 0, pd = 0, ad = 0;
    bool in_dbl = false, in_sgl = false;
    for (int i = type_start; i < body_end; ++i) {
        uint16_t c = s[i];
        if (in_dbl) {
            if (c == (uint16_t)'\\' && i + 1 < body_end) {
                i++;
                continue;
            }
            if (c == (uint16_t)'"') in_dbl = false;
            continue;
        }
        if (in_sgl) {
            if (c == (uint16_t)'\\' && i + 1 < body_end) {
                i++;
                continue;
            }
            if (c == (uint16_t)'\'') in_sgl = false;
            continue;
        }
        if (c == (uint16_t)'"') {
            in_dbl = true;
            continue;
        }
        if (c == (uint16_t)'\'') {
            in_sgl = true;
            continue;
        }
        if (c == (uint16_t)'{') bd++;
        else if (c == (uint16_t)'}' && bd > 0) bd--;
        else if (c == (uint16_t)'[') brd++;
        else if (c == (uint16_t)']' && brd > 0) brd--;
        else if (c == (uint16_t)'(') pd++;
        else if (c == (uint16_t)')' && pd > 0) pd--;
        else if (c == (uint16_t)'<') ad++;
        else if (c == (uint16_t)'>' && ad > 0) ad--;
        if (c == (uint16_t)';' && bd == 0 && brd == 0 && pd == 0 && ad == 0) return i;
        if (c == (uint16_t)',' && bd == 0 && brd == 0 && pd == 0 && ad == 0) return i;
    }
    return body_end;
}

typedef struct {
    int          name_pos;
    int          name_end;
    const uint16_t* name_ptr;
    size_t       name_len;
    CtscType*    value_type;
    bool         optional;
} TypeLiteralMember;

/*
 * Index signature `[key: string]: T` parsed from a type literal span
 * (checker.ts IndexSignatureDeclaration / getTypeFromTypeNode).
 */
typedef struct {
    bool has_string_index;
    int          param_name_pos;
    int          param_name_end;
    const uint16_t* param_name_ptr;
    size_t       param_name_len;
    CtscType*    key_type;
    CtscType*    value_type;
} TypeLiteralStringIndexInfo;

/*
 * `readonly` before PropertySignature / MethodSignature (parser.ts parsePropertySignature
 * ~4435-4445; token is ReadonlyKeyword).
 */
static bool u16_keyword_readonly_at(const uint16_t* src, int i, int inner_end) {
    static const uint16_t kw[] = { (uint16_t)'r', (uint16_t)'e', (uint16_t)'a', (uint16_t)'d',
                                   (uint16_t)'o', (uint16_t)'n', (uint16_t)'l', (uint16_t)'y' };
    if (i < 0 || i + 8 > inner_end) return false;
    for (int k = 0; k < 8; k++) {
        if (src[i + k] != kw[k]) return false;
    }
    if (i + 8 < inner_end && u16_is_ident_part_ascii(src[i + 8])) return false;
    return true;
}

/* Returns NULL → caller uses t_any. */
static CtscType* type_from_type_literal_span(CtscTypeRegistry* reg, const uint16_t* src,
                                            size_t src_len, int pos, int end,
                                            TypeLiteralMember* members_buf, size_t members_cap, size_t* out_n,
                                            TypeLiteralStringIndexInfo* out_idx) {
    *out_n = 0;
    if (out_idx) memset(out_idx, 0, sizeof(*out_idx));
    if (!src || pos < 0 || end < 0 || pos >= (int)src_len || end > (int)src_len || pos >= end) return NULL;
    /*
     * TypeLiteral.node.pos is token full_start (may include trivia before `{`);
     * mirror getTypeFromTypeNode by locating the opening brace.
     */
    int brace_open = pos;
    while (brace_open < end && ctsc_is_ws_u16(src[brace_open])) brace_open++;
    if (brace_open >= end || src[brace_open] != (uint16_t)'{') return NULL;
    int close = type_literal_find_matching_brace(src, src_len, brace_open, end);
    if (close < 0 || close >= end) return NULL;
    int inner_start = brace_open + 1;
    int inner_end = close; /* exclusive; inner text is [inner_start, inner_end) */
    int i = inner_start;
    size_t count = 0;
    CtscType* string_index_value = NULL;
    while (i < inner_end) {
        /*
         * Property / type member names use Identifier nodes whose .pos is the
         * token full start (scanner.getTokenFullStart), i.e. leading trivia
         * before the identifier text is included (mirrors parser.ts createIdentifier
         * ~2648–2657 and getNodePos ~2180–2181).
         */
        int name_token_full_start = i;
        while (i < inner_end && ctsc_is_ws_u16(src[i])) i++;
        if (i >= inner_end) break;

        /* Index signature: [ param : keyType ] : valueType (checker.ts parseIndexSignature ~4400+). */
        if (src[i] == (uint16_t)'[') {
            int bracket_open = i;
            i++;
            while (i < inner_end && ctsc_is_ws_u16(src[i])) i++;
            int param_name_start = i;
            if (!u16_is_ident_start_ascii(src[i])) {
                *out_n = 0;
                return NULL;
            }
            i++;
            while (i < inner_end && u16_is_ident_part_ascii(src[i])) i++;
            int param_name_end = i;
            if (param_name_start >= param_name_end) {
                *out_n = 0;
                return NULL;
            }
            while (i < inner_end && ctsc_is_ws_u16(src[i])) i++;
            if (i >= inner_end || src[i] != (uint16_t)':') {
                *out_n = 0;
                return NULL;
            }
            i++;
            while (i < inner_end && ctsc_is_ws_u16(src[i])) i++;
            int key_type_start = i;
            int j = bracket_open + 1;
            int depth = 1;
            for (; j < inner_end; ++j) {
                if (src[j] == (uint16_t)'[') depth++;
                else if (src[j] == (uint16_t)']') {
                    depth--;
                    if (depth == 0) break;
                }
            }
            if (depth != 0) {
                *out_n = 0;
                return NULL;
            }
            int key_type_end = j;
            CtscType* key_ty = type_from_annotation_fallback_span(reg, src, src_len, key_type_start, key_type_end);
            i = j + 1;
            while (i < inner_end && ctsc_is_ws_u16(src[i])) i++;
            if (i >= inner_end || src[i] != (uint16_t)':') {
                *out_n = 0;
                return NULL;
            }
            i++;
            while (i < inner_end && ctsc_is_ws_u16(src[i])) i++;
            int val_type_start = i;
            int val_type_end = type_literal_scan_member_type_end(src, i, inner_end);
            CtscType* val_ty = type_from_annotation_fallback_span(reg, src, src_len, val_type_start, val_type_end);
            i = val_type_end;
            if (key_ty && key_ty->kind == CTSC_TYPE_STRING) {
                string_index_value = val_ty;
                if (out_idx) {
                    out_idx->has_string_index = true;
                    out_idx->param_name_pos = param_name_start;
                    out_idx->param_name_end = param_name_end;
                    out_idx->param_name_ptr = src + param_name_start;
                    out_idx->param_name_len = (size_t)(param_name_end - param_name_start);
                    out_idx->key_type = key_ty;
                    out_idx->value_type = val_ty;
                }
            }
            while (i < inner_end && ctsc_is_ws_u16(src[i])) i++;
            if (i < inner_end && src[i] == (uint16_t)';') i++;
            if (i < inner_end && src[i] == (uint16_t)',') i++;
            continue;
        }

        if (u16_keyword_readonly_at(src, i, inner_end)) {
            i += 8;
            /* Identifier full start for the property name (trivia before name, e.g. space before `x`). */
            name_token_full_start = i;
            while (i < inner_end && ctsc_is_ws_u16(src[i])) i++;
        }

        int name_start = i;
        if (!u16_is_ident_start_ascii(src[i])) {
            *out_n = 0;
            return NULL;
        }
        i++;
        while (i < inner_end && u16_is_ident_part_ascii(src[i])) i++;
        int name_end = i;
        if (name_start >= name_end) {
            *out_n = 0;
            return NULL;
        }
        bool member_optional = false;
        while (i < inner_end && ctsc_is_ws_u16(src[i])) i++;
        /*
         * Optional PropertySignature / MethodSignature: Name `?` Type (parser.ts
         * parsePropertyOrMethodSignature ~4268–4271, parseOptionalToken(QuestionToken)).
         */
        if (i < inner_end && src[i] == (uint16_t)'?') {
            member_optional = true;
            i++;
            while (i < inner_end && ctsc_is_ws_u16(src[i])) i++;
        }
        if (i >= inner_end || src[i] != (uint16_t)':') {
            *out_n = 0;
            return NULL;
        }
        i++; /* colon */
        while (i < inner_end && ctsc_is_ws_u16(src[i])) i++;
        int type_start = i;
        int type_end = type_literal_scan_member_type_end(src, i, inner_end);
        CtscType* pt = type_from_annotation_fallback_span(reg, src, src_len, type_start, type_end);
        if (count >= members_cap) {
            *out_n = 0;
            return NULL;
        }
        members_buf[count].name_pos = name_token_full_start;
        members_buf[count].name_end = name_end;
        members_buf[count].name_ptr = src + name_start;
        members_buf[count].name_len = (size_t)(name_end - name_start);
        members_buf[count].value_type = pt;
        members_buf[count].optional = member_optional;
        count++;
        i = type_end;
        while (i < inner_end && ctsc_is_ws_u16(src[i])) i++;
        if (i < inner_end && src[i] == (uint16_t)';') i++;
        /*
         * Do not skip trivia after `;` / before the next member: the next
         * PropertySignature name Identifier uses token full_start (scanner),
         * so pos includes that leading trivia (mirrors tsc; CRLF fixtures).
         */
        if (i < inner_end && src[i] == (uint16_t)',') i++;
    }
    *out_n = count;
    if (count == 0 && !string_index_value) return reg->t_empty_object;
    if (count == 0 && string_index_value) {
        CtscType* only_idx = ctsc_type_object_literal(reg, NULL, 0);
        only_idx->object_string_index_value_type = string_index_value;
        return only_idx;
    }
    CtscObjectProperty* props = (CtscObjectProperty*)ctsc_arena_alloc(reg->arena, count * sizeof(CtscObjectProperty));
    for (size_t k = 0; k < count; ++k) {
        props[k].name = members_buf[k].name_ptr;
        props[k].name_len = members_buf[k].name_len;
        props[k].value_type = members_buf[k].value_type;
        props[k].optional = members_buf[k].optional;
    }
    CtscType* lit = ctsc_type_object_literal(reg, props, count);
    if (string_index_value) lit->object_string_index_value_type = string_index_value;
    return lit;
}

/*
 * Maps TypeNode → CtscType for the M4.0 subset. Unknown types collapse to
 * `any` so downstream formatters never emit garbage.
 *
 * For Identifier type references, mirrors checker.ts getTypeFromTypeNode /
 * resolveAlias / getDeclaredTypeOfTypeAlias (~9659+): a local `type N = T`
 * yields the same CtscType as `T`. For unions / object literals from an
 * alias, type.aliasSymbol is preserved in tsc so typeToString prints the
 * alias name (e.g. `SN` for `type SN = string | number`) while `type N = number`
 * still stringifies as `number` (~6916-6923).
 */
static CtscType* type_of_type_node(Walk* w, CtscTypeRegistry* reg, const uint16_t* src, size_t src_len,
                                  const CtscNode* type_node, int alias_depth) {
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
        case CTSC_SK_TypeReference: {
            const CtscTypeReferenceData* tr = &type_node->data.typeReference;
            if (tr->typeName && tr->typeName->kind == CTSC_SK_Identifier) {
                const CtscIdentifierData* id = &tr->typeName->data.identifier;
                if (w && alias_depth < 32) {
                    CtscSymbol* sym = resolve_name(&w->scopes, id->text, id->text_len);
                    if (sym && (sym->flags & CTSC_SYMBOL_FLAG_TypeAlias) && sym->decls_len > 0) {
                        CtscNode* decl0 = sym->decls[0];
                        if (decl0 && decl0->kind == CTSC_SK_TypeAliasDeclaration) {
                            CtscNode* alias_ty = decl0->data.typeAliasDeclaration.type;
                            if (alias_ty) {
                                CtscType* expanded =
                                    type_of_type_node(w, reg, src, src_len, alias_ty, alias_depth + 1);
                                if (expanded
                                    && (expanded->kind == CTSC_TYPE_UNION
                                        || expanded->kind == CTSC_TYPE_INTERSECTION
                                        || expanded->kind == CTSC_TYPE_OBJECT_LITERAL)) {
                                    expanded->alias_symbol_name = id->text;
                                    expanded->alias_symbol_name_len = id->text_len;
                                }
                                return expanded;
                            }
                        }
                    }
                }
                /*
                 * Preserve type arguments for generic class / interface references
                 * (checker.ts getTypeFromClassOrInterfaceReference ~16990:
                 * createTypeReference(type, typeArguments)). Without this, a
                 * `Box<number>` annotation would stringify back as just `Box`
                 * and property access would never substitute type parameters.
                 */
                if (tr->has_type_arguments && tr->type_arguments.len > 0 && tr->type_arguments.len <= 32) {
                    size_t na = tr->type_arguments.len;
                    CtscType* args_buf[32];
                    for (size_t ai = 0; ai < na; ++ai) {
                        CtscNode* tnode = tr->type_arguments.items[ai];
                        args_buf[ai] = type_of_type_node(w, reg, src, src_len, tnode, alias_depth + 1);
                        if (!args_buf[ai]) args_buf[ai] = reg->t_any;
                    }
                    CtscType** slot = (CtscType**)ctsc_arena_alloc(reg->arena, na * sizeof(CtscType*));
                    memcpy(slot, args_buf, na * sizeof(CtscType*));
                    return ctsc_type_reference_with_type_args(reg, id->text, id->text_len, slot, na);
                }
                return ctsc_type_reference(reg, id->text, id->text_len);
            }
            if (tr->typeName) {
                return reg->t_any;
            }
            return type_from_annotation_fallback_span(reg, src, src_len, type_node->pos, type_node->end);
        }
        case CTSC_SK_TypeLiteral: {
            /* No TypeElement children in the AST yet — recover from source span. */
            TypeLiteralMember tmp[32];
            size_t n = 0;
            CtscType* t =
                type_from_type_literal_span(reg, src, src_len, type_node->pos, type_node->end, tmp,
                                            sizeof(tmp) / sizeof(tmp[0]), &n, NULL);
            return t ? t : reg->t_any;
        }
        default:
            /* Generics / etc.: not handled in M4.0. */
            return reg->t_any;
    }
}

static CtscType* type_of_expression(Walk* w, CtscTypeRegistry* reg, const CtscNode* expr);
static CtscType* type_of_object_literal_expression(Walk* w, CtscTypeRegistry* reg, const CtscNode* expr);
static CtscType* type_of_property_of_object_type(Walk* w, CtscTypeRegistry* reg, CtscType* object_type,
                                                 const uint16_t* prop_name, size_t prop_len);
static CtscType* type_of_namespace_member(Walk* w, CtscTypeRegistry* reg, CtscSymbol* ns_sym,
                                                      const uint16_t* prop_name, size_t prop_len);
static const CtscNode* function_declaration_in_namespace(CtscSymbol* ns_sym,
                                                         const uint16_t* prop_name, size_t prop_len);
static bool is_const_decl_list(const CtscNode* list);
static CtscType* variable_declaration_type(Walk* w, const CtscNode* decl, bool is_const);
static CtscType* class_property_declaration_type(Walk* w, const CtscNode* decl);
static CtscType* property_type_from_class_declaration(Walk* w, const CtscNode* class_node,
                                                      const uint16_t* prop_name, size_t prop_len,
                                                      unsigned depth, bool static_side);
static CtscType* return_type_of_method_declaration(Walk* w, const CtscNode* method_node);
static CtscType* return_type_of_get_accessor_declaration(Walk* w, const CtscNode* getter_node);
static CtscType* method_return_type_from_class_declaration(Walk* w, const CtscNode* class_node,
                                                           const uint16_t* prop_name, size_t prop_len,
                                                           unsigned depth, bool static_side);
static CtscType* return_type_of_function_declaration(Walk* w, const CtscNode* fn_decl);
static const CtscNode* symbol_function_declaration_decl(CtscSymbol* sym);
static const CtscNode* symbol_resolve_overload_for_call(Walk* w, CtscSymbol* sym,
                                                         const CtscCallExpressionData* ce);
static CtscType* type_of_symbol_value(Walk* w, CtscSymbol* sym);
static CtscType* param_type(Walk* w, const CtscNode* param);
static CtscType* infer_return_type_for_function_body(Walk* w, CtscTypeRegistry* reg, CtscArena* arena,
                                                     const CtscNode* body, const CtscNode* scope_container);
static CtscType* generic_call_instantiated_return_type(Walk* w, CtscTypeRegistry* reg, const CtscNode* fn_decl,
                                                      const CtscCallExpressionData* ce);
static CtscType* instantiate_type_params_in_type(Walk* w, CtscTypeRegistry* reg, CtscType* t,
                                                 const CtscNodeArray* type_params, CtscType** type_arg_values,
                                                 size_t type_arg_count, unsigned depth);
static CtscType* resolve_type_reference_to_object_literal(Walk* w, CtscTypeRegistry* reg, CtscType* ref);
static CtscType* string_index_value_type_of_resolved_object(Walk* w, CtscTypeRegistry* reg, CtscType* object_type);
static CtscType* maybe_instantiate_class_member_type(Walk* w, CtscTypeRegistry* reg, const CtscNode* class_node,
                                                     CtscType* member_type, CtscType** instance_args,
                                                     size_t instance_arg_count);
static bool identifier_is_class_constructor_name(const CtscNode* name_id);
static const CtscNode* constructor_method_declaration(const CtscNode* class_decl);
static bool constructor_parameter_matches_class_type_parameter(const CtscNode* class_decl, const CtscNode* param,
                                                               size_t tp_index);
static bool inferred_return_type_equal(CtscArena* arena, const CtscType* a, const CtscType* b);
static CtscType* inferred_generic_class_instance_from_new(Walk* w, CtscTypeRegistry* reg,
                                                          const CtscNode* class_decl,
                                                          const CtscIdentifierData* class_name_id,
                                                          const CtscNewExpressionData* ne);

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

/*
 * True branch / false branch collapse when identical after subtype-style
 * literal comparison (checker.ts getUnionType + UnionReduction.Subtype for
 * the simple literal cases exercised by M4.0).
 */
static bool conditional_branch_types_identical(const CtscType* a, const CtscType* b) {
    if (a == b) return true;
    if (!a || !b) return false;
    if (a->kind != b->kind) return false;
    switch (a->kind) {
        case CTSC_TYPE_NUMBER_LITERAL:
            return a->number_value == b->number_value;
        case CTSC_TYPE_STRING_LITERAL:
        case CTSC_TYPE_BIGINT_LITERAL: {
            if (a->text_len != b->text_len) return false;
            if (a->text_len == 0) return true;
            if (!a->text || !b->text) return a->text == b->text;
            return memcmp(a->text, b->text, a->text_len * sizeof(uint16_t)) == 0;
        }
        case CTSC_TYPE_BOOLEAN_LITERAL:
            return a->boolean_value == b->boolean_value;
        case CTSC_TYPE_REFERENCE: {
            if (a->text_len != b->text_len) return false;
            if (a->text_len > 0) {
                if (!a->text || !b->text) return a->text == b->text;
                if (memcmp(a->text, b->text, a->text_len * sizeof(uint16_t)) != 0) return false;
            }
            if (a->reference_type_args_len != b->reference_type_args_len) return false;
            for (size_t i = 0; i < a->reference_type_args_len; ++i) {
                if (!conditional_branch_types_identical(a->reference_type_args[i], b->reference_type_args[i]))
                    return false;
            }
            return true;
        }
        case CTSC_TYPE_CLASS_CONSTRUCTOR: {
            if (a->text_len != b->text_len) return false;
            if (a->text_len == 0) return true;
            if (!a->text || !b->text) return a->text == b->text;
            return memcmp(a->text, b->text, a->text_len * sizeof(uint16_t)) == 0;
        }
        case CTSC_TYPE_ENUM_MEMBER_LITERAL: {
            if (a->enum_parent_name_len != b->enum_parent_name_len
                || a->enum_member_name_len != b->enum_member_name_len) {
                return false;
            }
            if (a->enum_parent_name_len > 0) {
                if (!a->enum_parent_name || !b->enum_parent_name) return a->enum_parent_name == b->enum_parent_name;
                if (memcmp(a->enum_parent_name, b->enum_parent_name,
                           a->enum_parent_name_len * sizeof(uint16_t))
                    != 0) {
                    return false;
                }
            }
            if (a->enum_member_name_len > 0) {
                if (!a->enum_member_name || !b->enum_member_name) return a->enum_member_name == b->enum_member_name;
                return memcmp(a->enum_member_name, b->enum_member_name,
                              a->enum_member_name_len * sizeof(uint16_t))
                    == 0;
            }
            return true;
        }
        default:
            return false;
    }
}

/* checkConditionalExpression → getUnionType([type1, type2], ...) (~41268-41273). */
static CtscType* union_conditional_branch_types(CtscTypeRegistry* reg, CtscType* t1, CtscType* t2) {
    if (!t1 || !t2) return reg->t_any;
    if (t1->kind == CTSC_TYPE_ANY || t2->kind == CTSC_TYPE_ANY) return reg->t_any;
    if (conditional_branch_types_identical(t1, t2)) return t1;
    {
        CtscType* u = ctsc_type_new(reg, CTSC_TYPE_UNION);
        u->union_members = (CtscType**)ctsc_arena_alloc(reg->arena, 2 * sizeof(CtscType*));
        u->union_members[0] = t1;
        u->union_members[1] = t2;
        u->union_members_len = 2;
        return u;
    }
}

/*
 * Numeric index for tuple element access when the argument is a numeric literal
 * (checker.ts getPropertyTypeForIndexType ~19262-19279: tuple + numeric literal
 * name → getTupleElementTypeOutOfStartCount).
 */
static bool tuple_index_from_access_argument(const CtscNode* arg, size_t* out_index) {
    if (!arg || !out_index) return false;
    if (arg->kind == CTSC_SK_NumericLiteral) {
        double v = numeric_literal_to_double(&arg->data.numericLiteral);
        if (v < 0 || v > (double)SIZE_MAX) return false;
        size_t idx = (size_t)v;
        if ((double)idx != v) return false;
        *out_index = idx;
        return true;
    }
    if (arg->kind == CTSC_SK_PrefixUnaryExpression) {
        const CtscPrefixUnaryExpressionData* pu = &arg->data.prefixUnaryExpression;
        if (pu->operator_kind == CTSC_SK_PlusToken && pu->operand) {
            return tuple_index_from_access_argument(pu->operand, out_index);
        }
    }
    return false;
}

/*
 * Instance type of a class declaration (`new C()` result type). Mirrors
 * checker.ts resolveNewExpression (~37131) + checkCallExpression NewExpression
 * branch (~37826): construct signature return type is the class instance type,
 * typeToString is the class name. Generic classes without explicit `<...>` infer
 * type arguments from constructor parameters (inferTypeArguments ~35827).
 */
static CtscType* type_of_new_expression(Walk* w, CtscTypeRegistry* reg, const CtscNode* expr) {
    const CtscNewExpressionData* ne = &expr->data.newExpression;
    const CtscNode* callee = ne->expression;
    if (!callee || !w) return reg->t_any;
    if (callee->kind != CTSC_SK_Identifier) return reg->t_any;
    const uint16_t* nm = callee->data.identifier.text;
    size_t nl = callee->data.identifier.text_len;
    CtscSymbol* sym = resolve_name(&w->scopes, nm, nl);
    if (!sym) return reg->t_any;
    for (size_t i = 0; i < sym->decls_len; ++i) {
        CtscNode* d = sym->decls[i];
        if (d && d->kind == CTSC_SK_ClassDeclaration) {
            const CtscClassDeclarationData* cd = &d->data.classDeclaration;
            const CtscNode* cname = cd->name;
            if (cname && cname->kind == CTSC_SK_Identifier) {
                const CtscIdentifierData* id = &cname->data.identifier;
                if (ne->has_type_arguments && ne->type_arguments.len > 0
                    && cd->type_parameters.len == ne->type_arguments.len
                    && cd->type_parameters.len <= 32) {
                    CtscType* args_buf[32];
                    size_t na = ne->type_arguments.len;
                    for (size_t ai = 0; ai < na; ++ai) {
                        CtscNode* tnode = ne->type_arguments.items[ai];
                        args_buf[ai] =
                            type_of_type_node(w, reg, w->source_utf16, w->source_utf16_len, tnode, 0);
                        if (!args_buf[ai]) args_buf[ai] = reg->t_any;
                    }
                    CtscType** slot = (CtscType**)ctsc_arena_alloc(w->arena, na * sizeof(CtscType*));
                    memcpy(slot, args_buf, na * sizeof(CtscType*));
                    return ctsc_type_reference_with_type_args(reg, id->text, id->text_len, slot, na);
                }
                if (cd->type_parameters.len > 0 && cd->type_parameters.len <= 32) {
                    CtscType* inferred_inst =
                        inferred_generic_class_instance_from_new(w, reg, d, id, ne);
                    if (inferred_inst) return inferred_inst;
                }
                return ctsc_type_reference(reg, id->text, id->text_len);
            }
            break;
        }
    }
    return reg->t_any;
}

static CtscType* type_of_expression(Walk* w, CtscTypeRegistry* reg, const CtscNode* expr) {
    if (!expr) return reg->t_any;
    switch (expr->kind) {
        case CTSC_SK_Identifier: {
            /*
             * getTypeOfSymbol / checkIdentifier (checker.ts): reference to a
             * declared value reads the symbol's type (not `any`).
             */
            if (!w) return reg->t_any;
            const uint16_t* nm = expr->data.identifier.text;
            size_t nl = expr->data.identifier.text_len;
            if (!nm || nl == 0) return reg->t_any;
            CtscSymbol* sym = resolve_name(&w->scopes, nm, nl);
            if (!sym) return reg->t_any;
            if (w->narrow_typeof_eq_string_sym && sym == w->narrow_typeof_eq_string_sym
                && w->narrow_typeof_eq_string_type) {
                return w->narrow_typeof_eq_string_type;
            }
            if (w->narrow_eq_literal_sym && sym == w->narrow_eq_literal_sym && w->narrow_eq_literal_type) {
                return w->narrow_eq_literal_type;
            }
            return type_of_symbol_value(w, sym);
        }
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
        case CTSC_SK_ObjectLiteralExpression:
            /*
             * checkObjectLiteral (~33527) + checkPropertyAssignment (~41503) /
             * checkExpressionForMutableLocation (~41496): property initializers
             * use widened literal types (e.g. `1` → number).
             */
            return type_of_object_literal_expression(w, reg, expr);
        case CTSC_SK_ParenthesizedExpression:
            return type_of_expression(w, reg, expr->data.parenthesizedExpression.expression);
        case CTSC_SK_ConditionalExpression: {
            const CtscConditionalExpressionData* ce = &expr->data.conditionalExpression;
            CtscType* type_when_true = type_of_expression(w, reg, ce->whenTrue);
            CtscType* type_when_false = type_of_expression(w, reg, ce->whenFalse);
            return union_conditional_branch_types(reg, type_when_true, type_when_false);
        }
        case CTSC_SK_BinaryExpression: {
            const CtscBinaryExpressionData* b = &expr->data.binaryExpression;
            CtscType* lt = type_of_expression(w, reg, b->left);
            CtscType* rt = type_of_expression(w, reg, b->right);
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
        case CTSC_SK_NewExpression:
            return type_of_new_expression(w, reg, expr);
        case CTSC_SK_CallExpression: {
            /*
             * checkCallExpression → getReturnTypeOfSignature (checker.ts ~37810-37878).
             * M4.0: direct calls to a FunctionDeclaration or to a class method via
             * PropertyAccessExpression (e.g. `new C().m()`).
             */
            const CtscCallExpressionData* ce = &expr->data.callExpression;
            const CtscNode* callee = ce->expression;
            if (callee && callee->kind == CTSC_SK_PropertyAccessExpression && w) {
                const CtscPropertyAccessExpressionData* pa = &callee->data.propertyAccessExpression;
                /*
                 * Namespace-qualified call `N.fn(args)` (checker.ts
                 * resolveEntityName ~34822-34842 → getResolvedSignature on the
                 * function's symbol): when the LHS identifier resolves to a
                 * ValueModule / NamespaceModule, look up the declared function
                 * in the module body and return its return type. Mirrors the
                 * PropertyAccessExpression namespace branch below (~1867).
                 */
                if (pa->expression && pa->expression->kind == CTSC_SK_Identifier && pa->name
                    && pa->name->kind == CTSC_SK_Identifier) {
                    const CtscIdentifierData* lhs = &pa->expression->data.identifier;
                    CtscSymbol* lhs_sym = resolve_name(&w->scopes, lhs->text, lhs->text_len);
                    if (lhs_sym
                        && (lhs_sym->flags & (CTSC_SYMBOL_FLAG_ValueModule
                                              | CTSC_SYMBOL_FLAG_NamespaceModule))) {
                        const CtscIdentifierData* mid = &pa->name->data.identifier;
                        const CtscNode* fn_decl =
                            function_declaration_in_namespace(lhs_sym, mid->text, mid->text_len);
                        if (fn_decl) {
                            CtscType* gen = generic_call_instantiated_return_type(w, reg, fn_decl, ce);
                            if (gen) return gen;
                            CtscType* rt = return_type_of_function_declaration(w, fn_decl);
                            if (rt) return rt;
                            return reg->t_any;
                        }
                    }
                }
                CtscType* obj_t = type_of_expression(w, reg, pa->expression);
                bool static_member = (obj_t && obj_t->kind == CTSC_TYPE_CLASS_CONSTRUCTOR);
                if (obj_t && (obj_t->kind == CTSC_TYPE_REFERENCE || static_member) && pa->name
                    && pa->name->kind == CTSC_SK_Identifier) {
                    const CtscIdentifierData* id = &pa->name->data.identifier;
                    CtscSymbol* sym = resolve_name(&w->scopes, obj_t->text, obj_t->text_len);
                    if (sym) {
                        for (size_t i = 0; i < sym->decls_len; ++i) {
                            CtscNode* d = sym->decls[i];
                            if (d && d->kind == CTSC_SK_ClassDeclaration) {
                                CtscType* rt = method_return_type_from_class_declaration(w, d, id->text,
                                                                                       id->text_len, 0,
                                                                                       static_member);
                                if (rt) return rt;
                            }
                        }
                    }
                }
                return reg->t_any;
            }
            if (callee && callee->kind == CTSC_SK_Identifier && w) {
                const uint16_t* nm = callee->data.identifier.text;
                size_t nl = callee->data.identifier.text_len;
                if (nm && nl > 0) {
                    CtscSymbol* sym = resolve_name(&w->scopes, nm, nl);
                    if (sym) {
                        /* Overload resolution (checker.ts resolveCall /
                         * chooseOverload ~36750-36900): pick the first
                         * signature whose parameters accept the arguments.
                         * Falls back to the first declaration's return type
                         * so single-signature paths and no-match paths keep
                         * behaving as before. */
                        const CtscNode* fn_decl = symbol_resolve_overload_for_call(w, sym, ce);
                        if (!fn_decl) fn_decl = symbol_function_declaration_decl(sym);
                        if (fn_decl) {
                            CtscType* gen = generic_call_instantiated_return_type(w, reg, fn_decl, ce);
                            if (gen) return gen;
                            CtscType* rt = return_type_of_function_declaration(w, fn_decl);
                            if (rt) return rt;
                        }
                    }
                }
                return reg->t_any;
            }
            return reg->t_any;
        }
        case CTSC_SK_PropertyAccessExpression: {
            const CtscPropertyAccessExpressionData* pa = &expr->data.propertyAccessExpression;
            if (!pa->name || pa->name->kind != CTSC_SK_Identifier) return reg->t_any;
            const CtscIdentifierData* id = &pa->name->data.identifier;
            /*
             * Namespace qualified access `N.member` (checker.ts
             * checkQualifiedName / resolveEntityName ~34822-34842): when the
             * LHS is an Identifier that resolves to a ModuleDeclaration
             * (SymbolFlags.NamespaceModule), look the member up directly in
             * the module body's statements. This short-circuits the object-
             * type path because a namespace is not represented as a
             * CtscType and its members aren't on a CtscType.object_properties.
             */
            if (w && pa->expression && pa->expression->kind == CTSC_SK_Identifier) {
                const CtscIdentifierData* lhs = &pa->expression->data.identifier;
                CtscSymbol* lhs_sym = resolve_name(&w->scopes, lhs->text, lhs->text_len);
                if (lhs_sym
                    && (lhs_sym->flags & (CTSC_SYMBOL_FLAG_ValueModule
                                          | CTSC_SYMBOL_FLAG_NamespaceModule))) {
                    CtscType* mt = type_of_namespace_member(w, reg, lhs_sym, id->text, id->text_len);
                    if (mt) return mt;
                }
            }
            CtscType* obj_t = type_of_expression(w, reg, pa->expression);
            return type_of_property_of_object_type(w, reg, obj_t, id->text, id->text_len);
        }
        case CTSC_SK_ElementAccessExpression: {
            const CtscElementAccessExpressionData* ea = &expr->data.elementAccessExpression;
            CtscType* obj_t = type_of_expression(w, reg, ea->expression);
            if (obj_t && obj_t->kind == CTSC_TYPE_TUPLE) {
                size_t idx;
                if (!tuple_index_from_access_argument(ea->argumentExpression, &idx)) return reg->t_any;
                if (idx >= obj_t->tuple_elements_len) return reg->t_any;
                CtscType* el = obj_t->tuple_elements[idx];
                return el ? el : reg->t_any;
            }
            /*
             * String index signature: getPropertyTypeForIndexType (checker.ts ~19262-19279)
             * when the argument is a string literal / template.
             */
            if (w && ea->argumentExpression) {
                bool string_like_arg = ea->argumentExpression->kind == CTSC_SK_StringLiteral
                    || ea->argumentExpression->kind == CTSC_SK_NoSubstitutionTemplateLiteral;
                if (string_like_arg) {
                    CtscType* idx_val = string_index_value_type_of_resolved_object(w, reg, obj_t);
                    if (idx_val) return idx_val;
                }
            }
            return reg->t_any;
        }
        case CTSC_SK_AsExpression: {
            /* checkAssertionWorker → getTypeFromTypeNode(type) (checker.ts ~38110-38123). */
            const CtscAsExpressionData* ae = &expr->data.asExpression;
            CtscNode* tnode = ae->type;
            if (!tnode || !w) return reg->t_any;
            return type_of_type_node(w, reg, w->source_utf16, w->source_utf16_len, tnode, 0);
        }
        case CTSC_SK_TypeAssertionExpression: {
            const CtscTypeAssertionExpressionData* ta = &expr->data.typeAssertionExpression;
            CtscNode* tnode = ta->type;
            if (!tnode || !w) return reg->t_any;
            return type_of_type_node(w, reg, w->source_utf16, w->source_utf16_len, tnode, 0);
        }
        default:
            /* All other expressions → any, for M4.0. */
            return reg->t_any;
    }
}

/*
 * Property names for OBJECT_LITERAL types: Identifier only until fixtures need
 * string/numeric literal keys (typeToString parity).
 */
static bool object_literal_property_name_utf16(const CtscNode* name_node,
                                               const uint16_t** name_out, size_t* len_out) {
    if (!name_node || name_node->kind != CTSC_SK_Identifier) return false;
    const CtscIdentifierData* id = &name_node->data.identifier;
    *name_out = id->text;
    *len_out = id->text_len;
    return true;
}

static CtscType* type_of_object_literal_expression(Walk* w, CtscTypeRegistry* reg, const CtscNode* expr) {
    const CtscObjectLiteralExpressionData* ol = &expr->data.objectLiteralExpression;
    if (ol->properties.len == 0) return reg->t_empty_object;

    CtscObjectProperty* props = (CtscObjectProperty*)ctsc_arena_alloc(
        reg->arena, ol->properties.len * sizeof(CtscObjectProperty));
    size_t count = 0;
    for (size_t i = 0; i < ol->properties.len; ++i) {
        CtscNode* prop = ol->properties.items[i];
        if (!prop || prop->kind != CTSC_SK_PropertyAssignment) return reg->t_any;
        const CtscPropertyAssignmentData* pa = &prop->data.propertyAssignment;
        const uint16_t* pname;
        size_t plen;
        if (!object_literal_property_name_utf16(pa->name, &pname, &plen)) return reg->t_any;
        if (!pa->initializer) return reg->t_any;
        CtscType* vt = type_of_expression(w, reg, pa->initializer);
        vt = ctsc_type_widen(reg, vt);
        props[count].name = pname;
        props[count].name_len = plen;
        props[count].value_type = vt;
        props[count].optional = false;
        count++;
    }
    return ctsc_type_object_literal(reg, props, count);
}

/*
 * checker.ts getTypeOfPropertyOfType on an enum object type (~11575+):
 * property read yields the unique enum literal type `EnumName.member`
 * (EnumLiteralType / typeToString).
 */
static CtscType* type_of_enum_member(Walk* w, CtscTypeRegistry* reg, const uint16_t* enum_name_utf16,
                                     size_t enum_name_len, const uint16_t* prop_name, size_t prop_len) {
    if (!w || !reg || !enum_name_utf16 || enum_name_len == 0 || !prop_name || prop_len == 0) return reg->t_any;
    CtscSymbol* sym = resolve_name(&w->scopes, enum_name_utf16, enum_name_len);
    if (!sym) return reg->t_any;
    for (size_t di = 0; di < sym->decls_len; ++di) {
        CtscNode* ed = sym->decls[di];
        if (!ed || ed->kind != CTSC_SK_EnumDeclaration) continue;
        const CtscNodeArray* members = &ed->data.enumDeclaration.members;
        for (size_t mi = 0; mi < members->len; ++mi) {
            CtscNode* m = members->items[mi];
            if (!m || (m->kind != CTSC_SK_EnumMember && m->kind != CTSC_SK_PropertyAssignment)) continue;
            const CtscNode* mname = m->data.propertyAssignment.name;
            if (!mname || mname->kind != CTSC_SK_Identifier) continue;
            const CtscIdentifierData* mid = &mname->data.identifier;
            if (mid->text_len != prop_len || !mid->text) continue;
            if (memcmp(mid->text, prop_name, prop_len * sizeof(uint16_t)) != 0) continue;
            return ctsc_type_enum_member_literal(reg, enum_name_utf16, enum_name_len, mid->text, mid->text_len);
        }
    }
    return reg->t_any;
}

/*
 * checker.ts getTypeOfPropertyOfType (~11575): anonymous object literal
 * properties and class instance members (resolve class symbol → declared
 * PropertyDeclaration type, including members from `extends` heritage).
 */
static CtscType* type_of_property_of_object_type(Walk* w, CtscTypeRegistry* reg, CtscType* object_type,
                                                 const uint16_t* prop_name, size_t prop_len) {
    if (!reg || !object_type || !prop_name || prop_len == 0) return reg->t_any;
    if (object_type->kind == CTSC_TYPE_INTERSECTION && w) {
        CtscType* found[8];
        size_t nf = 0;
        for (size_t ci = 0; ci < object_type->intersection_members_len && nf < 8; ++ci) {
            CtscType* t = type_of_property_of_object_type(w, reg, object_type->intersection_members[ci], prop_name,
                                                         prop_len);
            if (t && t != reg->t_any) found[nf++] = t;
        }
        if (nf == 0) return reg->t_any;
        if (nf == 1) return found[0];
        bool all_same = true;
        for (size_t i = 1; i < nf; ++i) {
            if (!inferred_return_type_equal(w->arena, found[0], found[i])) {
                all_same = false;
                break;
            }
        }
        if (all_same) return found[0];
        return ctsc_type_intersection(reg, found, nf);
    }
    if (object_type->kind == CTSC_TYPE_ENUM_VALUE && w) {
        return type_of_enum_member(w, reg, object_type->text, object_type->text_len, prop_name, prop_len);
    }
    if (object_type->kind == CTSC_TYPE_OBJECT_LITERAL) {
        for (size_t i = 0; i < object_type->object_properties_len; ++i) {
            CtscObjectProperty* p = &object_type->object_properties[i];
            if (p->name_len == prop_len && p->name
                && memcmp(p->name, prop_name, prop_len * sizeof(uint16_t)) == 0) {
                return p->value_type ? p->value_type : reg->t_any;
            }
        }
        return reg->t_any;
    }
    if (object_type->kind == CTSC_TYPE_REFERENCE && w) {
        CtscSymbol* sym = resolve_name(&w->scopes, object_type->text, object_type->text_len);
        if (sym) {
            for (size_t i = 0; i < sym->decls_len; ++i) {
                CtscNode* d = sym->decls[i];
                if (d && d->kind == CTSC_SK_InterfaceDeclaration) {
                    CtscType* lit = resolve_type_reference_to_object_literal(w, reg, object_type);
                    if (lit) {
                        CtscType* pt = type_of_property_of_object_type(w, reg, lit, prop_name, prop_len);
                        if (pt) {
                            /*
                             * Substitute the interface's local type parameters with the
                             * reference's type arguments (checker.ts
                             * getTypeFromClassOrInterfaceReference ~16990 +
                             * getPropertyTypeForIndexType ~19262 path on a generic
                             * interface instance).
                             */
                            pt = maybe_instantiate_class_member_type(w, reg, d, pt,
                                                                     object_type->reference_type_args,
                                                                     object_type->reference_type_args_len);
                            return pt;
                        }
                    }
                }
                if (d && d->kind == CTSC_SK_ClassDeclaration) {
                    CtscType* pt =
                        property_type_from_class_declaration(w, d, prop_name, prop_len, 0, false);
                    if (pt) {
                        pt = maybe_instantiate_class_member_type(w, reg, d, pt, object_type->reference_type_args,
                                                                 object_type->reference_type_args_len);
                    }
                    if (pt) return pt;
                }
            }
        }
    }
    if (object_type->kind == CTSC_TYPE_CLASS_CONSTRUCTOR && w) {
        CtscSymbol* sym = resolve_name(&w->scopes, object_type->text, object_type->text_len);
        if (sym) {
            for (size_t i = 0; i < sym->decls_len; ++i) {
                CtscNode* d = sym->decls[i];
                if (d && d->kind == CTSC_SK_ClassDeclaration) {
                    CtscType* pt =
                        property_type_from_class_declaration(w, d, prop_name, prop_len, 0, true);
                    if (pt) {
                        pt = maybe_instantiate_class_member_type(w, reg, d, pt, object_type->reference_type_args,
                                                                 object_type->reference_type_args_len);
                    }
                    if (pt) return pt;
                }
            }
        }
    }
    return reg->t_any;
}

/*
 * Resolve `N.<prop>` by scanning the ModuleDeclaration body(s) attached to
 * the namespace symbol. Mirrors checker.ts getSymbol on
 * `symbol.exports` for a NamespaceModule (~2520-2560, resolveEntityName
 * ~34822-34842); our M5.0 slice walks the AST directly because the binder
 * does not yet maintain per-namespace export tables.
 *
 * Iterates each ModuleDeclaration declaration of the symbol, unwraps
 * ModuleBlock bodies (or nested ModuleDeclaration bodies for dotted-name
 * namespaces — currently not exercised here but cheap to support), and
 * looks for a VariableDeclaration whose name matches `prop_name`. Returns
 * the declared type (annotation preferred; widened initializer otherwise),
 * or NULL when nothing matches so the caller can fall back.
 */
static CtscType* type_of_namespace_member(Walk* w, CtscTypeRegistry* reg, CtscSymbol* ns_sym,
                                          const uint16_t* prop_name, size_t prop_len) {
    if (!w || !reg || !ns_sym || !prop_name || prop_len == 0) return NULL;
    for (size_t di = 0; di < ns_sym->decls_len; ++di) {
        CtscNode* d = ns_sym->decls[di];
        if (!d || d->kind != CTSC_SK_ModuleDeclaration) continue;
        const CtscNode* body = d->data.moduleDeclaration.body;
        /* Unwrap dotted-name namespaces (`namespace a.b {}` parses as nested
         * ModuleDeclarations); stop at the innermost ModuleBlock. */
        while (body && body->kind == CTSC_SK_ModuleDeclaration) {
            body = body->data.moduleDeclaration.body;
        }
        if (!body || body->kind != CTSC_SK_ModuleBlock) continue;
        const CtscNodeArray* stmts = &body->data.moduleBlock.statements;
        for (size_t si = 0; si < stmts->len; ++si) {
            const CtscNode* s = stmts->items[si];
            if (!s) continue;
            if (s->kind != CTSC_SK_VariableStatement) continue;
            const CtscNode* list = s->data.variableStatement.declarationList;
            if (!list || list->kind != CTSC_SK_VariableDeclarationList) continue;
            bool is_const = is_const_decl_list(list);
            const CtscNodeArray* vds = &list->data.variableDeclarationList.declarations;
            for (size_t vi = 0; vi < vds->len; ++vi) {
                const CtscNode* vd = vds->items[vi];
                if (!vd || vd->kind != CTSC_SK_VariableDeclaration) continue;
                const CtscNode* nm = vd->data.variableDeclaration.name;
                if (!nm || nm->kind != CTSC_SK_Identifier) continue;
                const CtscIdentifierData* nid = &nm->data.identifier;
                if (nid->text_len != prop_len
                    || memcmp(nid->text, prop_name, prop_len * sizeof(uint16_t)) != 0) {
                    continue;
                }
                return variable_declaration_type(w, vd, is_const);
            }
        }
    }
    return NULL;
}

/*
 * Locate a `function <name>(...)` declaration inside a namespace body.
 * Mirrors the namespace-member resolution path of checker.ts
 * resolveEntityName (~34822-34842) / getSymbol on symbol.exports (~2520-2560)
 * for the M5.0 slice: walks each ModuleDeclaration → ModuleBlock of
 * `ns_sym`, descending through nested ModuleDeclarations (dotted-name
 * namespaces), and returns the first FunctionDeclaration whose name
 * matches. `declare function` inside the namespace counts.
 *
 * Used by callers (e.g. type_of_expression CallExpression branch) that
 * need the signature's return type; namespaces aren't modeled as
 * CtscTypes so we resolve via AST scan instead of a property lookup.
 */
static const CtscNode* function_declaration_in_namespace(CtscSymbol* ns_sym,
                                                         const uint16_t* prop_name, size_t prop_len) {
    if (!ns_sym || !prop_name || prop_len == 0) return NULL;
    for (size_t di = 0; di < ns_sym->decls_len; ++di) {
        CtscNode* d = ns_sym->decls[di];
        if (!d || d->kind != CTSC_SK_ModuleDeclaration) continue;
        const CtscNode* body = d->data.moduleDeclaration.body;
        while (body && body->kind == CTSC_SK_ModuleDeclaration) {
            body = body->data.moduleDeclaration.body;
        }
        if (!body || body->kind != CTSC_SK_ModuleBlock) continue;
        const CtscNodeArray* stmts = &body->data.moduleBlock.statements;
        for (size_t si = 0; si < stmts->len; ++si) {
            const CtscNode* s = stmts->items[si];
            if (!s || s->kind != CTSC_SK_FunctionDeclaration) continue;
            const CtscNode* nm = s->data.functionDeclaration.name;
            if (!nm || nm->kind != CTSC_SK_Identifier) continue;
            const CtscIdentifierData* nid = &nm->data.identifier;
            if (nid->text_len != prop_len
                || memcmp(nid->text, prop_name, prop_len * sizeof(uint16_t)) != 0) {
                continue;
            }
            return s;
        }
    }
    return NULL;
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

/*
 * Substitute class type parameters (checker.ts instantiateType ~19000+,
 * getTypeOfPropertyOfType on generic class instance).
 */
static CtscType* instantiate_type_params_in_type(Walk* w, CtscTypeRegistry* reg, CtscType* t,
                                                 const CtscNodeArray* type_params, CtscType** type_arg_values,
                                                 size_t type_arg_count, unsigned depth) {
    (void)w;
    if (!t || !type_params || !type_arg_values || type_arg_count == 0 || depth > 48u) return t;

    if (t->kind == CTSC_TYPE_REFERENCE) {
        for (size_t i = 0; i < type_params->len && i < type_arg_count; ++i) {
            const CtscNode* tp = type_params->items[i];
            if (!tp || tp->kind != CTSC_SK_TypeParameter) continue;
            const CtscNode* tname = tp->data.typeParameter.name;
            if (!tname || tname->kind != CTSC_SK_Identifier) continue;
            const CtscIdentifierData* tid = &tname->data.identifier;
            if (utf16_type_text_equal(t->text, t->text_len, tid->text, tid->text_len)) {
                return type_arg_values[i] ? type_arg_values[i] : reg->t_any;
            }
        }
        if (t->reference_type_args_len > 0 && t->reference_type_args) {
            CtscType** new_args =
                (CtscType**)ctsc_arena_alloc(reg->arena, t->reference_type_args_len * sizeof(CtscType*));
            bool changed = false;
            for (size_t j = 0; j < t->reference_type_args_len; ++j) {
                new_args[j] = instantiate_type_params_in_type(w, reg, t->reference_type_args[j], type_params,
                                                              type_arg_values, type_arg_count, depth + 1);
                if (new_args[j] != t->reference_type_args[j]) changed = true;
            }
            if (!changed) return t;
            return ctsc_type_reference_with_type_args(reg, t->text, t->text_len, new_args,
                                                      t->reference_type_args_len);
        }
        return t;
    }
    if (t->kind == CTSC_TYPE_UNION) {
        CtscType** new_mem = (CtscType**)ctsc_arena_alloc(reg->arena, t->union_members_len * sizeof(CtscType*));
        bool changed = false;
        for (size_t i = 0; i < t->union_members_len; ++i) {
            new_mem[i] = instantiate_type_params_in_type(w, reg, t->union_members[i], type_params, type_arg_values,
                                                         type_arg_count, depth + 1);
            if (new_mem[i] != t->union_members[i]) changed = true;
        }
        if (!changed) return t;
        CtscType* u = ctsc_type_new(reg, CTSC_TYPE_UNION);
        u->union_members = new_mem;
        u->union_members_len = t->union_members_len;
        return u;
    }
    if (t->kind == CTSC_TYPE_INTERSECTION) {
        CtscType** new_mem =
            (CtscType**)ctsc_arena_alloc(reg->arena, t->intersection_members_len * sizeof(CtscType*));
        bool changed = false;
        for (size_t i = 0; i < t->intersection_members_len; ++i) {
            new_mem[i] = instantiate_type_params_in_type(w, reg, t->intersection_members[i], type_params,
                                                         type_arg_values, type_arg_count, depth + 1);
            if (new_mem[i] != t->intersection_members[i]) changed = true;
        }
        if (!changed) return t;
        return ctsc_type_intersection(reg, new_mem, t->intersection_members_len);
    }
    if (t->kind == CTSC_TYPE_OBJECT_LITERAL) {
        CtscObjectProperty* new_props =
            (CtscObjectProperty*)ctsc_arena_alloc(reg->arena, t->object_properties_len * sizeof(CtscObjectProperty));
        bool changed = false;
        for (size_t i = 0; i < t->object_properties_len; ++i) {
            new_props[i] = t->object_properties[i];
            new_props[i].value_type =
                instantiate_type_params_in_type(w, reg, t->object_properties[i].value_type, type_params,
                                                type_arg_values, type_arg_count, depth + 1);
            if (new_props[i].value_type != t->object_properties[i].value_type) changed = true;
        }
        CtscType* new_idx = NULL;
        if (t->object_string_index_value_type) {
            new_idx = instantiate_type_params_in_type(w, reg, t->object_string_index_value_type, type_params,
                                                      type_arg_values, type_arg_count, depth + 1);
            if (new_idx != t->object_string_index_value_type) changed = true;
        }
        if (!changed) return t;
        CtscType* o = ctsc_type_object_literal(reg, new_props, t->object_properties_len);
        o->object_string_index_value_type = new_idx;
        return o;
    }
    if (t->kind == CTSC_TYPE_TUPLE) {
        CtscType** new_el = (CtscType**)ctsc_arena_alloc(reg->arena, t->tuple_elements_len * sizeof(CtscType*));
        bool changed = false;
        for (size_t i = 0; i < t->tuple_elements_len; ++i) {
            new_el[i] = instantiate_type_params_in_type(w, reg, t->tuple_elements[i], type_params, type_arg_values,
                                                        type_arg_count, depth + 1);
            if (new_el[i] != t->tuple_elements[i]) changed = true;
        }
        if (!changed) return t;
        return ctsc_type_tuple(reg, new_el, t->tuple_elements_len);
    }
    return t;
}

static CtscType* maybe_instantiate_class_member_type(Walk* w, CtscTypeRegistry* reg, const CtscNode* class_node,
                                                     CtscType* member_type, CtscType** instance_args,
                                                     size_t instance_arg_count) {
    (void)w;
    if (!class_node || !member_type) return member_type;
    if (class_node->kind != CTSC_SK_ClassDeclaration
        && class_node->kind != CTSC_SK_InterfaceDeclaration) {
        return member_type;
    }
    const CtscNodeArray* tps = &class_node->data.classDeclaration.type_parameters;
    if (!instance_args || instance_arg_count == 0 || tps->len != instance_arg_count) return member_type;
    return instantiate_type_params_in_type(w, reg, member_type, tps, instance_args, instance_arg_count, 0);
}

static bool object_literal_shape_is_empty(CtscTypeRegistry* reg, CtscType* t) {
    if (!t || t == reg->t_empty_object) return true;
    if (t->kind == CTSC_TYPE_EMPTY_OBJECT) return true;
    if (t->kind == CTSC_TYPE_OBJECT_LITERAL && t->object_properties_len == 0 && !t->object_string_index_value_type) {
        return true;
    }
    return false;
}

/*
 * Structural merge for interface `extends` (checker.ts resolveBaseTypesOfInterface
 * ~13400-13428 + member resolution feeding getTypeOfPropertyOfType ~11575).
 * Later `overlay` properties replace same-named properties from `base`.
 */
static CtscType* merge_object_literal_shapes(CtscTypeRegistry* reg, CtscType* base, CtscType* overlay) {
    if (!reg) return NULL;
    if (object_literal_shape_is_empty(reg, base)) {
        if (object_literal_shape_is_empty(reg, overlay)) return reg->t_empty_object;
        return overlay;
    }
    if (object_literal_shape_is_empty(reg, overlay)) return base;
    if (base->kind != CTSC_TYPE_OBJECT_LITERAL || overlay->kind != CTSC_TYPE_OBJECT_LITERAL) return overlay;

    size_t cap = base->object_properties_len + overlay->object_properties_len;
    CtscObjectProperty* merged = (CtscObjectProperty*)ctsc_arena_alloc(reg->arena, cap * sizeof(CtscObjectProperty));
    size_t out = 0;
    for (size_t i = 0; i < base->object_properties_len; ++i) {
        merged[out++] = base->object_properties[i];
    }
    for (size_t j = 0; j < overlay->object_properties_len; ++j) {
        const CtscObjectProperty* op = &overlay->object_properties[j];
        bool replaced = false;
        for (size_t k = 0; k < out; ++k) {
            CtscObjectProperty* mp = &merged[k];
            if (mp->name_len == op->name_len && mp->name && op->name
                && memcmp(mp->name, op->name, op->name_len * sizeof(uint16_t)) == 0) {
                mp->value_type = op->value_type;
                mp->optional = op->optional;
                replaced = true;
                break;
            }
        }
        if (!replaced) {
            merged[out].name = op->name;
            merged[out].name_len = op->name_len;
            merged[out].value_type = op->value_type;
            merged[out].optional = op->optional;
            out++;
        }
    }
    CtscType* lit = ctsc_type_object_literal(reg, merged, out);
    CtscType* idx = overlay->object_string_index_value_type ? overlay->object_string_index_value_type
                                                             : base->object_string_index_value_type;
    if (idx) lit->object_string_index_value_type = idx;
    return lit;
}

/*
 * Interface declaration → anonymous object literal type, including `extends`
 * heritage (checker.ts resolveBaseTypesOfInterface ~13400, resolveDeclaredMembers
 * ~13753+).
 */
static CtscType* resolve_interface_declaration_to_object_literal(Walk* w, CtscTypeRegistry* reg, const CtscNode* idecl,
                                                                 unsigned depth) {
    if (!w || !reg || !idecl || idecl->kind != CTSC_SK_InterfaceDeclaration) return NULL;
    if (depth > 64) return NULL;

    CtscType* merged_bases = reg->t_empty_object;
    const CtscNodeArray* hcs = &idecl->data.classDeclaration.heritage_clauses;
    for (size_t hi = 0; hi < hcs->len; ++hi) {
        CtscNode* hc = hcs->items[hi];
        if (!hc || hc->kind != CTSC_SK_HeritageClause) continue;
        if (hc->data.heritageClause.token != CTSC_SK_ExtendsKeyword) continue;
        const CtscNodeArray* ext_types = &hc->data.heritageClause.types;
        for (size_t ti = 0; ti < ext_types->len; ++ti) {
            CtscNode* ewta = ext_types->items[ti];
            if (!ewta || ewta->kind != CTSC_SK_ExpressionWithTypeArguments) continue;
            const CtscNode* base_expr = ewta->data.expressionWithTypeArguments.expression;
            if (!base_expr || base_expr->kind != CTSC_SK_Identifier) continue;
            const CtscIdentifierData* bid = &base_expr->data.identifier;
            CtscSymbol* base_sym = resolve_name(&w->scopes, bid->text, bid->text_len);
            if (!base_sym) continue;
            for (size_t di = 0; di < base_sym->decls_len; ++di) {
                CtscNode* bd = base_sym->decls[di];
                if (!bd || bd->kind != CTSC_SK_InterfaceDeclaration) continue;
                CtscType* base_shape = resolve_interface_declaration_to_object_literal(w, reg, bd, depth + 1);
                if (base_shape) merged_bases = merge_object_literal_shapes(reg, merged_bases, base_shape);
            }
        }
    }

    int bo = find_interface_body_open_brace(w->source_utf16, w->source_utf16_len, idecl->pos, idecl->end);
    if (bo < 0) {
        if (object_literal_shape_is_empty(reg, merged_bases)) return NULL;
        return merged_bases;
    }
    TypeLiteralMember tmp[32];
    size_t n = 0;
    CtscType* own = type_from_type_literal_span(reg, w->source_utf16, w->source_utf16_len, bo, idecl->end, tmp,
                                                sizeof(tmp) / sizeof(tmp[0]), &n, NULL);
    if (!own) own = reg->t_empty_object;
    return merge_object_literal_shapes(reg, merged_bases, own);
}

/*
 * Resolve an identifier TypeReference (interface instance type) to the
 * structural object type of its declaration (checker.ts getDeclaredTypeOfClassOrInterface
 * ~13463+ / resolveBaseTypesOfInterface ~13400). Used for assignability of object
 * literals to interface types and property access on interface references.
 */
static CtscType* resolve_type_reference_to_object_literal(Walk* w, CtscTypeRegistry* reg, CtscType* ref) {
    if (!w || !reg || !ref || ref->kind != CTSC_TYPE_REFERENCE) return NULL;
    CtscSymbol* sym = resolve_name(&w->scopes, ref->text, ref->text_len);
    if (!sym) return NULL;
    /* Declaration merging (checker.ts mergeSymbol ~3567 +
     * getDeclaredTypeOfClassOrInterface ~13463): every InterfaceDeclaration
     * sharing the same name contributes members. tsc materialises one
     * InterfaceType whose resolvedMembers is the union of each declaration's
     * own members plus its base types. Here we approximate by calling the
     * per-declaration resolver and concatenating the shapes. Later
     * declarations override earlier ones on conflicting member names, which
     * matches tsc's "last write wins" behaviour for declaration merging
     * (important for lib.d.ts augmentations). */
    CtscType* merged = NULL;
    for (size_t i = 0; i < sym->decls_len; ++i) {
        CtscNode* d = sym->decls[i];
        if (!d || d->kind != CTSC_SK_InterfaceDeclaration) continue;
        CtscType* t = resolve_interface_declaration_to_object_literal(w, reg, d, 0);
        if (!t) continue;
        if (!merged) {
            merged = t;
        } else {
            merged = merge_object_literal_shapes(reg, merged, t);
        }
    }
    return merged;
}

/*
 * Value type of a string index signature on an object type or interface instance
 * (checker.ts getIndexTypeOfType ~16559, getPropertyTypeForIndexType ~19262).
 */
static CtscType* string_index_value_type_of_resolved_object(Walk* w, CtscTypeRegistry* reg, CtscType* object_type) {
    if (!reg || !object_type) return NULL;
    if (object_type->kind == CTSC_TYPE_OBJECT_LITERAL && object_type->object_string_index_value_type) {
        return object_type->object_string_index_value_type;
    }
    if (object_type->kind == CTSC_TYPE_REFERENCE && w) {
        CtscType* expanded = resolve_type_reference_to_object_literal(w, reg, object_type);
        if (expanded && expanded->kind == CTSC_TYPE_OBJECT_LITERAL && expanded->object_string_index_value_type) {
            return expanded->object_string_index_value_type;
        }
    }
    return NULL;
}

static char* ts2322_message_text(CtscArena* arena, CtscTypeRegistry* reg, CtscType* source, CtscType* target) {
    const bool both_str_lit = source && target && source->kind == CTSC_TYPE_STRING_LITERAL
        && target->kind == CTSC_TYPE_STRING_LITERAL;
    const bool both_num_lit = source && target && source->kind == CTSC_TYPE_NUMBER_LITERAL
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
    return msg_arena;
}

/*
 * Diagnostic span for TS2322 on an arbitrary expression (checker.ts
 * createDiagnosticForNode / getErrorSpanForNode on StringLiteral: the
 * highlight covers the decoded string text, not the quotes).
 * StringLiteral nodes record pos = token full_start (can include leading
 * trivia); the lexeme pointer `text` is anchored at the opening quote, so
 * use (text - source_utf16) + 1 for the first decoded character.
 */
static bool ts2322_error_span_for_expression(const CtscNode* expr, const uint16_t* source_utf16, int* out_start,
                                             int* out_len) {
    if (!expr || expr->pos < 0 || expr->end < expr->pos || !out_start || !out_len) return false;
    if (expr->kind == CTSC_SK_Identifier) {
        size_t tl = expr->data.identifier.text_len;
        if (tl == 0 || expr->end < (int)tl) return false;
        *out_len = (int)tl;
        *out_start = expr->end - *out_len;
        return *out_start >= 0;
    }
    if (expr->kind == CTSC_SK_StringLiteral) {
        const CtscStringLiteralData* d = &expr->data.stringLiteral;
        if (d->value_len > 0 && d->text && source_utf16) {
            *out_start = (int)((d->text + 1) - source_utf16);
            *out_len = (int)d->value_len;
            return *out_len > 0 && *out_start >= 0 && *out_start + *out_len <= expr->end;
        }
    }
    if (expr->kind == CTSC_SK_NumericLiteral) {
        const CtscNumericLiteralData* d = &expr->data.numericLiteral;
        size_t tl = d->text_len;
        if (tl > 0 && expr->end >= (int)tl) {
            *out_len = (int)tl;
            *out_start = expr->end - *out_len;
            return *out_start >= 0;
        }
    }
    *out_start = expr->pos;
    *out_len = expr->end - expr->pos;
    return *out_len > 0;
}

static void emit_ts2322_on_expression(CtscCheckResult* r, CtscArena* arena, CtscTypeRegistry* reg, const CtscNode* expr,
                                      const uint16_t* source_utf16, CtscType* source, CtscType* target) {
    if (!expr) return;
    char* msg_arena = ts2322_message_text(arena, reg, source, target);
    int start = 0, len = 0;
    if (!ts2322_error_span_for_expression(expr, source_utf16, &start, &len)) {
        return;
    }
    CtscCheckDiagnostic d = {0};
    d.code = 2322;
    d.category = "Error";
    d.start = start;
    d.length = len;
    d.message = msg_arena;
    diag_push(r, arena, d);
}

static const CtscNode* property_assignment_initializer_for_name(const CtscNode* obj_lit, const uint16_t* prop_name,
                                                                size_t prop_len) {
    if (!obj_lit || obj_lit->kind != CTSC_SK_ObjectLiteralExpression || !prop_name || prop_len == 0) return NULL;
    const CtscObjectLiteralExpressionData* ol = &obj_lit->data.objectLiteralExpression;
    for (size_t i = 0; i < ol->properties.len; ++i) {
        CtscNode* prop = ol->properties.items[i];
        if (!prop || prop->kind != CTSC_SK_PropertyAssignment) continue;
        const CtscNode* pname = prop->data.propertyAssignment.name;
        if (!pname || pname->kind != CTSC_SK_Identifier) continue;
        if (pname->data.identifier.text_len != prop_len) continue;
        if (memcmp(pname->data.identifier.text, prop_name, prop_len * sizeof(uint16_t)) != 0) continue;
        return prop->data.propertyAssignment.initializer;
    }
    return NULL;
}

/* Same lookup, but returns the property NAME identifier so the diagnostic
 * spans the key (what tsc highlights for TS2322 on object-literal property
 * mismatches, matching checker.ts checkPropertyAssignment). */
static const CtscNode* property_assignment_name_for_name(const CtscNode* obj_lit, const uint16_t* prop_name,
                                                         size_t prop_len) {
    if (!obj_lit || obj_lit->kind != CTSC_SK_ObjectLiteralExpression || !prop_name || prop_len == 0) return NULL;
    const CtscObjectLiteralExpressionData* ol = &obj_lit->data.objectLiteralExpression;
    for (size_t i = 0; i < ol->properties.len; ++i) {
        CtscNode* prop = ol->properties.items[i];
        if (!prop || prop->kind != CTSC_SK_PropertyAssignment) continue;
        const CtscNode* pname = prop->data.propertyAssignment.name;
        if (!pname || pname->kind != CTSC_SK_Identifier) continue;
        if (pname->data.identifier.text_len != prop_len) continue;
        if (memcmp(pname->data.identifier.text, prop_name, prop_len * sizeof(uint16_t)) != 0) continue;
        return pname;
    }
    return NULL;
}

static bool is_assignable_to(Walk* w, CtscTypeRegistry* reg, CtscType* source, CtscType* target);
static void emit_ts2322_on_binding_name(CtscCheckResult* r, CtscArena* arena, CtscTypeRegistry* reg,
                                         const CtscNode* name_id, CtscType* source, CtscType* target);

/*
 * When an object literal fails assignability to an interface / object type,
 * report TS2322 on the first property whose value is incompatible (mirrors
 * checker.ts flow through checkPropertyAssignment rather than the whole
 * initializer object).
 */
static bool try_emit_ts2322_object_literal_structural_mismatch(Walk* w, const CtscNode* obj_lit, CtscType* source_obj,
                                                               CtscType* target_struct) {
    if (!w || !obj_lit || obj_lit->kind != CTSC_SK_ObjectLiteralExpression) return false;
    if (!source_obj || source_obj->kind != CTSC_TYPE_OBJECT_LITERAL) return false;
    if (!target_struct || target_struct->kind != CTSC_TYPE_OBJECT_LITERAL) return false;
    for (size_t ti = 0; ti < target_struct->object_properties_len; ti++) {
        const CtscObjectProperty* tp = &target_struct->object_properties[ti];
        CtscType* sp_val = NULL;
        for (size_t si = 0; si < source_obj->object_properties_len; si++) {
            const CtscObjectProperty* sp = &source_obj->object_properties[si];
            if (sp->name_len == tp->name_len && utf16_type_text_equal(sp->name, sp->name_len, tp->name, tp->name_len)) {
                sp_val = sp->value_type;
                break;
            }
        }
        if (!sp_val) continue;
        if (!is_assignable_to(w, &w->r->registry, sp_val, tp->value_type)) {
            /* tsc (checker.ts checkPropertyAssignment) places TS2322 on the
             * property NAME, not the initializer expression. The message still
             * describes source vs target value types, but the span covers the
             * key so IDE squiggles land on the property that was wrong. */
            const CtscNode* name_id = property_assignment_name_for_name(obj_lit, tp->name, tp->name_len);
            if (name_id) {
                emit_ts2322_on_binding_name(w->r, w->arena, &w->r->registry, name_id, sp_val, tp->value_type);
                return true;
            }
            const CtscNode* init = property_assignment_initializer_for_name(obj_lit, tp->name, tp->name_len);
            if (!init) return false;
            emit_ts2322_on_expression(w->r, w->arena, &w->r->registry, init, w->source_utf16, sp_val, tp->value_type);
            return true;
        }
    }
    return false;
}

static bool is_assignable_to(Walk* w, CtscTypeRegistry* reg, CtscType* source, CtscType* target) {
    if (!source || !target) return true;
    /*
     * `const x: T = {}` gives the initializer type CTSC_TYPE_EMPTY_OBJECT
     * (object literal with no properties). Assignable to an object type iff
     * every target property is optional (checker.ts excess / missing checks
     * ~13500+).
     */
    if (source->kind == CTSC_TYPE_EMPTY_OBJECT) {
        if (target->kind == CTSC_TYPE_OBJECT_LITERAL) {
            for (size_t ti = 0; ti < target->object_properties_len; ti++) {
                if (!target->object_properties[ti].optional) return false;
            }
            return true;
        }
        if (target->kind == CTSC_TYPE_REFERENCE && w) {
            CtscType* expanded = resolve_type_reference_to_object_literal(w, reg, target);
            if (expanded && expanded->kind == CTSC_TYPE_OBJECT_LITERAL) {
                return is_assignable_to(w, reg, source, expanded);
            }
        }
        if (target->kind == CTSC_TYPE_INTERSECTION && w) {
            for (size_t i = 0; i < target->intersection_members_len; ++i) {
                if (!is_assignable_to(w, reg, source, target->intersection_members[i])) return false;
            }
            return true;
        }
    }
    if (source->kind == CTSC_TYPE_OBJECT_LITERAL && target->kind == CTSC_TYPE_REFERENCE && w) {
        CtscType* expanded = resolve_type_reference_to_object_literal(w, reg, target);
        if (expanded && expanded->kind == CTSC_TYPE_OBJECT_LITERAL) {
            return is_assignable_to(w, reg, source, expanded);
        }
    }
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
    if (target->kind == CTSC_TYPE_INTERSECTION && w) {
        for (size_t i = 0; i < target->intersection_members_len; ++i) {
            if (!is_assignable_to(w, reg, source, target->intersection_members[i])) return false;
        }
        return true;
    }
    if (target->kind == CTSC_TYPE_UNION) {
        /* isTypeRelatedTo / union target: assignable if assignable to a member
         * (checker.ts ~22000+). */
        for (size_t i = 0; i < target->union_members_len; ++i) {
            if (is_assignable_to(w, reg, source, target->union_members[i])) return true;
        }
        return false;
    }
    if (source->kind == CTSC_TYPE_OBJECT_LITERAL && target->kind == CTSC_TYPE_OBJECT_LITERAL) {
        /*
         * Structural assignability for anonymous object types (checker.ts
         * checkTypeRelatedTo for object types ~22000+): each property in the
         * target must be present in the source with an assignable type.
         */
        for (size_t ti = 0; ti < target->object_properties_len; ti++) {
            const CtscObjectProperty* tp = &target->object_properties[ti];
            CtscType* sp_val = NULL;
            for (size_t si = 0; si < source->object_properties_len; si++) {
                const CtscObjectProperty* sp = &source->object_properties[si];
                if (sp->name_len == tp->name_len
                    && utf16_type_text_equal(sp->name, sp->name_len, tp->name, tp->name_len)) {
                    sp_val = sp->value_type;
                    break;
                }
            }
            if (!sp_val) {
                if (tp->optional) continue;
                return false;
            }
            if (!is_assignable_to(w, reg, sp_val, tp->value_type)) return false;
        }
        if (target->object_string_index_value_type) {
            for (size_t si = 0; si < source->object_properties_len; si++) {
                const CtscObjectProperty* sp = &source->object_properties[si];
                bool matched_named = false;
                for (size_t ti = 0; ti < target->object_properties_len; ti++) {
                    const CtscObjectProperty* tp = &target->object_properties[ti];
                    if (sp->name_len == tp->name_len
                        && utf16_type_text_equal(sp->name, sp->name_len, tp->name, tp->name_len)) {
                        matched_named = true;
                        break;
                    }
                }
                if (!matched_named
                    && !is_assignable_to(w, reg, sp->value_type, target->object_string_index_value_type)) {
                    return false;
                }
            }
        }
        return true;
    }
    if (source->kind == CTSC_TYPE_TUPLE && target->kind == CTSC_TYPE_TUPLE) {
        if (source->tuple_elements_len != target->tuple_elements_len) return false;
        for (size_t ti = 0; ti < target->tuple_elements_len; ti++) {
            if (!is_assignable_to(w, reg, source->tuple_elements[ti], target->tuple_elements[ti])) return false;
        }
        return true;
    }
    if (source->kind == CTSC_TYPE_REFERENCE && target->kind == CTSC_TYPE_REFERENCE) {
        if (!utf16_type_text_equal(source->text, source->text_len, target->text, target->text_len)) return false;
        if (source->reference_type_args_len != target->reference_type_args_len) return false;
        for (size_t i = 0; i < source->reference_type_args_len; ++i) {
            CtscType* sa = source->reference_type_args[i];
            CtscType* ta = target->reference_type_args[i];
            if (!is_assignable_to(w, reg, sa, ta)) return false;
        }
        return true;
    }
    if (source->kind == CTSC_TYPE_CLASS_CONSTRUCTOR && target->kind == CTSC_TYPE_CLASS_CONSTRUCTOR) {
        return utf16_type_text_equal(source->text, source->text_len, target->text, target->text_len);
    }
    if (source->kind == CTSC_TYPE_ENUM_MEMBER_LITERAL && target->kind == CTSC_TYPE_ENUM_MEMBER_LITERAL) {
        return utf16_type_text_equal(source->enum_parent_name, source->enum_parent_name_len, target->enum_parent_name,
                                     target->enum_parent_name_len)
            && utf16_type_text_equal(source->enum_member_name, source->enum_member_name_len, target->enum_member_name,
                                     target->enum_member_name_len);
    }
    if (source->kind == CTSC_TYPE_ENUM_MEMBER_LITERAL) {
        if (target->kind == CTSC_TYPE_ENUM_VALUE
            && utf16_type_text_equal(source->enum_parent_name, source->enum_parent_name_len, target->text,
                                     target->text_len)) {
            return true;
        }
        if (target->kind == CTSC_TYPE_REFERENCE
            && utf16_type_text_equal(source->enum_parent_name, source->enum_parent_name_len, target->text,
                                     target->text_len)) {
            return true;
        }
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
    char* msg_arena = ts2322_message_text(arena, reg, source, target);

    CtscCheckDiagnostic d = {0};
    d.code = 2322;
    d.category = "Error";
    d.length = (int)name_id->data.identifier.text_len;
    d.start = name_id->end - d.length;
    d.message = msg_arena;
    diag_push(r, arena, d);
}

static void emit_ts2345_on_argument(CtscCheckResult* r, CtscArena* arena, CtscTypeRegistry* reg,
                                    const CtscNode* arg, CtscType* source, CtscType* target) {
    if (!arg || arg->pos < 0 || arg->end < arg->pos) return;
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
    ctsc_buf_append_cstr(&msg, "Argument of type '");
    ctsc_type_to_string(src_m, &msg);
    ctsc_buf_append_cstr(&msg, "' is not assignable to parameter of type '");
    ctsc_type_to_string(tgt_m, &msg);
    ctsc_buf_append_cstr(&msg, "'.");
    char* msg_arena = (char*)ctsc_arena_alloc(arena, msg.len + 1);
    memcpy(msg_arena, msg.data, msg.len);
    msg_arena[msg.len] = '\0';
    ctsc_buf_free(&msg);

    CtscCheckDiagnostic d = {0};
    d.code = 2345;
    d.category = "Error";
    d.start = arg->pos;
    d.length = arg->end - arg->pos;
    d.message = msg_arena;
    diag_push(r, arena, d);
}

/*
 * Property missing on anonymous object type (checker.ts checkPropertyAccessExpression
 * when getPropertyOfType yields no symbol and no index signature ~34948-34968;
 * Diagnostics.Property_0_does_not_exist_on_type_1, code 2339).
 */
static void emit_ts2339_missing_property(CtscCheckResult* r, CtscArena* arena, CtscTypeRegistry* reg,
                                         const CtscNode* prop_name_node, const uint16_t* prop_utf16,
                                         size_t prop_utf16_len, CtscType* left_type) {
    if (!r || !arena || !reg || !prop_name_node || prop_name_node->kind != CTSC_SK_Identifier) return;
    if (prop_name_node->pos < 0 || prop_name_node->end < prop_name_node->pos) return;
    CtscBuffer msg;
    ctsc_buf_init(&msg);
    ctsc_buf_append_cstr(&msg, "Property '");
    append_utf16_ascii_identifier(&msg, prop_utf16, prop_utf16_len);
    ctsc_buf_append_cstr(&msg, "' does not exist on type '");
    ctsc_type_to_string(left_type, &msg);
    ctsc_buf_append_cstr(&msg, "'.");
    char* msg_arena = (char*)ctsc_arena_alloc(arena, msg.len + 1);
    memcpy(msg_arena, msg.data, msg.len);
    msg_arena[msg.len] = '\0';
    ctsc_buf_free(&msg);

    CtscCheckDiagnostic d = {0};
    d.code = 2339;
    d.category = "Error";
    d.start = prop_name_node->pos;
    d.length = prop_name_node->end - prop_name_node->pos;
    d.message = msg_arena;
    diag_push(r, arena, d);
}

/*
 * Minimum argument count for a FunctionDeclaration parameter list.
 * Mirrors checker.ts getMinArgumentCount / signature.minArgumentCount for the
 * simple cases we model: parameters after the first rest are ignored; a rest
 * parameter does not consume a required slot; parameters with default
 * initializers are optional from the right.
 */
static int function_declaration_min_arguments(const CtscNode* fn_decl) {
    if (!fn_decl || fn_decl->kind != CTSC_SK_FunctionDeclaration) return 0;
    const CtscNodeArray* params = &fn_decl->data.functionDeclaration.parameters;
    if (params->len == 0) return 0;
    int min = (int)params->len;
    for (int i = (int)params->len - 1; i >= 0; --i) {
        const CtscNode* p = params->items[i];
        if (!p || p->kind != CTSC_SK_Parameter) {
            min = i + 1;
            break;
        }
        const CtscParameterData* pd = &p->data.parameter;
        if (pd->has_dot_dot_dot) {
            min = i;
            continue;
        }
        if (pd->initializer || pd->is_optional) {
            min = i;
            continue;
        }
        min = i + 1;
        break;
    }
    return min;
}

/*
 * Maximum argument count for a FunctionDeclaration (no rest): parameter list
 * length. Rest parameters absorb extra arguments, so max is unbounded
 * (INT_MAX). Mirrors checker.ts getParameterCount (~38559-38567) for the
 * non-tuple-rest cases we model.
 */
static int function_declaration_max_arguments(const CtscNode* fn_decl) {
    if (!fn_decl || fn_decl->kind != CTSC_SK_FunctionDeclaration) return 0;
    const CtscNodeArray* params = &fn_decl->data.functionDeclaration.parameters;
    if (params->len == 0) return 0;
    const CtscNode* last = params->items[params->len - 1];
    if (last && last->kind == CTSC_SK_Parameter && last->data.parameter.has_dot_dot_dot) {
        return INT_MAX;
    }
    return (int)params->len;
}

/*
 * Span for TS2554/arity on CallExpression: same node as getDiagnosticSpanForCallNode
 * (checker.ts ~36359-36362) — the callee expression, or the property name for
 * `obj.m()`. M4.0 only supplies the Identifier callee path.
 */
static bool call_arity_error_span(const CtscNode* callee, int* out_start, int* out_len) {
    if (!callee || callee->end < callee->pos) return false;
    if (callee->kind == CTSC_SK_Identifier) {
        /* Mirrors getErrorSpanForNode on Identifier: narrow to the name text,
         * not parser Node.pos (full_start may include leading trivia; same
         * adjustment as emit_ts2322_on_binding_name ~827-828). */
        size_t tl = callee->data.identifier.text_len;
        if (tl == 0 || callee->end < (int)tl) return false;
        *out_len = (int)tl;
        *out_start = callee->end - *out_len;
        return *out_start >= 0;
    }
    if (callee->kind == CTSC_SK_PropertyAccessExpression) {
        const CtscNode* name = callee->data.propertyAccessExpression.name;
        if (name && name->kind == CTSC_SK_Identifier) {
            size_t tl = name->data.identifier.text_len;
            if (tl == 0 || name->end < (int)tl) return false;
            *out_len = (int)tl;
            *out_start = name->end - *out_len;
            return *out_start >= 0;
        }
    }
    return false;
}

static void emit_ts2554_arity(CtscCheckResult* r, CtscArena* arena, int start, int length,
                              int expected, int got) {
    CtscBuffer msg;
    ctsc_buf_init(&msg);
    ctsc_buf_append_cstr(&msg, "Expected ");
    {
        char nbuf[32];
        snprintf(nbuf, sizeof(nbuf), "%d", expected);
        ctsc_buf_append_cstr(&msg, nbuf);
    }
    ctsc_buf_append_cstr(&msg, " arguments, but got ");
    {
        char nbuf[32];
        snprintf(nbuf, sizeof(nbuf), "%d", got);
        ctsc_buf_append_cstr(&msg, nbuf);
    }
    ctsc_buf_append_cstr(&msg, ".");
    char* msg_arena = (char*)ctsc_arena_alloc(arena, msg.len + 1);
    memcpy(msg_arena, msg.data, msg.len);
    msg_arena[msg.len] = '\0';
    ctsc_buf_free(&msg);

    CtscCheckDiagnostic d = {0};
    d.code = 2554;
    d.category = "Error";
    d.start = start;
    d.length = length;
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
 * Harness oracle uses ts.TypeChecker.getTypeAtLocation with default compiler
 * options (strictNullChecks off). Unions lose null/undefined constituents when
 * other members remain (checker.ts getWidenedType ~26021+); if only nullish
 * members remain, collapse to null when both appear (observed: null | undefined
 * parameter → "null").
 */
static CtscType* widen_union_remove_nullish_for_types_dump(CtscTypeRegistry* reg, CtscType* t) {
    if (!reg || !t) return reg ? reg->t_any : NULL;
    if (t->kind != CTSC_TYPE_UNION) return t;
    CtscType* buf[32];
    size_t n = 0;
    bool saw_null = false;
    bool saw_undef = false;
    for (size_t i = 0; i < t->union_members_len && n < 32; ++i) {
        CtscType* m = t->union_members[i];
        if (!m) continue;
        if (m == reg->t_null) {
            saw_null = true;
            continue;
        }
        if (m == reg->t_undefined) {
            saw_undef = true;
            continue;
        }
        buf[n++] = m;
    }
    /* No null / undefined constituents — keep the union object (alias_symbol_name etc.). */
    if (n == t->union_members_len) return t;
    if (n > 0) {
        if (n == 1) return buf[0];
        CtscType* u = ctsc_type_new(reg, CTSC_TYPE_UNION);
        u->union_members = (CtscType**)ctsc_arena_alloc(reg->arena, n * sizeof(CtscType*));
        memcpy(u->union_members, buf, n * sizeof(CtscType*));
        u->union_members_len = n;
        sort_intrinsic_union_members(u);
        return u;
    }
    if (saw_null && saw_undef) return reg->t_null;
    if (saw_null) return reg->t_null;
    if (saw_undef) return reg->t_undefined;
    return reg->t_any;
}

/*
 * Object destructuring: inferred type for one BindingElement from the
 * initializer object's property (checker.ts checkVariableDeclaration /
 * getTypeFromBindingPattern ~41400+).
 */
static CtscType* destructured_binding_element_type_from_prop(Walk* w, CtscType* prop_t, bool is_const) {
    if (!prop_t) return w->r->registry.t_any;
    CtscType* after_literal = is_const ? prop_t : ctsc_type_widen(&w->r->registry, prop_t);
    return widen_nullish_when_not_strict_null(&w->r->registry, after_literal);
}

static void register_object_binding_element_types(Walk* w, const CtscNode* be, CtscType* init_object_t, bool is_const);

static void register_object_binding_pattern_types_from_init(Walk* w, const CtscNode* obp, CtscType* init_t,
                                                            bool is_const) {
    if (!obp || obp->kind != CTSC_SK_ObjectBindingPattern || !init_t) return;
    const CtscNodeArray* ar = &obp->data.bindingPattern.elements;
    for (size_t i = 0; i < ar->len; i++) {
        CtscNode* el = ar->items[i];
        if (!el || el->kind != CTSC_SK_BindingElement) continue;
        register_object_binding_element_types(w, el, init_t, is_const);
    }
}

static void register_array_binding_element_types(Walk* w, const CtscNode* be, CtscType* init_t,
                                                 const CtscNode* initializer, size_t slot, bool is_const);

/*
 * One positional slot in array destructuring: infer from the initializer's array
 * literal element or a tuple type (checker.ts getTypeForVariableLikeDeclaration
 * / contextual tuple from binding pattern ~12444-12450).
 */
static CtscType* destructured_array_element_type_at_slot(Walk* w, const CtscNode* initializer,
                                                         CtscType* init_t, size_t slot, bool is_const) {
    if (initializer && initializer->kind == CTSC_SK_ArrayLiteralExpression) {
        const CtscArrayLiteralExpressionData* ar = &initializer->data.arrayLiteralExpression;
        if (slot < ar->elements.len) {
            CtscNode* el = ar->elements.items[slot];
            if (!el || el->kind == CTSC_SK_SpreadElement) return w->r->registry.t_any;
            CtscType* et = type_of_expression(w, &w->r->registry, el);
            et = ctsc_type_widen(&w->r->registry, et);
            return destructured_binding_element_type_from_prop(w, et, is_const);
        }
        return w->r->registry.t_any;
    }
    if (init_t && init_t->kind == CTSC_TYPE_TUPLE && slot < init_t->tuple_elements_len) {
        CtscType* et = init_t->tuple_elements[slot];
        return destructured_binding_element_type_from_prop(w, et, is_const);
    }
    return w->r->registry.t_any;
}

static void register_array_binding_pattern_types_from_init(Walk* w, const CtscNode* abp, CtscType* init_t,
                                                          const CtscNode* initializer, bool is_const) {
    if (!abp || abp->kind != CTSC_SK_ArrayBindingPattern) return;
    const CtscNodeArray* ar = &abp->data.bindingPattern.elements;
    size_t slot = 0;
    for (size_t i = 0; i < ar->len; i++) {
        CtscNode* el = ar->items[i];
        if (!el) continue;
        if (el->kind == CTSC_SK_OmittedExpression) {
            slot++;
            continue;
        }
        if (el->kind != CTSC_SK_BindingElement) continue;
        const CtscBindingElementData* bed = &el->data.bindingElement;
        if (bed->has_dotdotdot) {
            if (bed->name && bed->name->kind == CTSC_SK_Identifier) {
                binding_elem_type_register(w, el, w->r->registry.t_any);
            }
            break;
        }
        register_array_binding_element_types(w, el, init_t, initializer, slot, is_const);
        slot++;
    }
}

static void register_array_binding_element_types(Walk* w, const CtscNode* be, CtscType* init_t,
                                                 const CtscNode* initializer, size_t slot, bool is_const) {
    if (!be || be->kind != CTSC_SK_BindingElement) return;
    const CtscBindingElementData* bed = &be->data.bindingElement;
    if (bed->has_dotdotdot) return;
    CtscType* elem_t = destructured_array_element_type_at_slot(w, initializer, init_t, slot, is_const);
    const CtscNode* name = bed->name;
    if (!name) return;
    if (name->kind == CTSC_SK_Identifier) {
        binding_elem_type_register(w, be, elem_t);
    } else if (name->kind == CTSC_SK_ObjectBindingPattern) {
        register_object_binding_pattern_types_from_init(w, name, elem_t, is_const);
    } else if (name->kind == CTSC_SK_ArrayBindingPattern) {
        const CtscNode* inner_init = NULL;
        if (initializer && initializer->kind == CTSC_SK_ArrayLiteralExpression) {
            const CtscArrayLiteralExpressionData* ar = &initializer->data.arrayLiteralExpression;
            if (slot < ar->elements.len) inner_init = ar->elements.items[slot];
        }
        register_array_binding_pattern_types_from_init(w, name, elem_t, inner_init, is_const);
    }
}

static void register_object_binding_element_types(Walk* w, const CtscNode* be, CtscType* init_object_t,
                                                 bool is_const) {
    if (!be || be->kind != CTSC_SK_BindingElement || !init_object_t) return;
    const CtscBindingElementData* bed = &be->data.bindingElement;
    if (bed->has_dotdotdot) return;

    const uint16_t* prop_utf16 = NULL;
    size_t prop_len = 0;
    if (bed->propertyName) {
        if (!object_literal_property_name_utf16(bed->propertyName, &prop_utf16, &prop_len)) return;
    } else {
        if (!bed->name || bed->name->kind != CTSC_SK_Identifier) return;
        prop_utf16 = bed->name->data.identifier.text;
        prop_len = bed->name->data.identifier.text_len;
    }

    CtscType* prop_t =
        type_of_property_of_object_type(w, &w->r->registry, init_object_t, prop_utf16, prop_len);
    const CtscNode* name = bed->name;
    if (!name) return;
    if (name->kind == CTSC_SK_Identifier) {
        binding_elem_type_register(w, be, destructured_binding_element_type_from_prop(w, prop_t, is_const));
    } else if (name->kind == CTSC_SK_ObjectBindingPattern) {
        register_object_binding_pattern_types_from_init(w, name, prop_t, is_const);
    }
}

static const CtscNode* constructor_method_declaration(const CtscNode* class_decl) {
    if (!class_decl || class_decl->kind != CTSC_SK_ClassDeclaration) return NULL;
    const CtscNodeArray* members = &class_decl->data.classDeclaration.members;
    for (size_t i = 0; i < members->len; ++i) {
        CtscNode* m = members->items[i];
        if (!m || m->kind != CTSC_SK_MethodDeclaration) continue;
        const CtscNode* nm = m->data.methodDeclaration.name;
        if (identifier_is_class_constructor_name(nm)) return m;
    }
    return NULL;
}

static bool constructor_parameter_matches_class_type_parameter(const CtscNode* class_decl, const CtscNode* param,
                                                                 size_t tp_index) {
    if (!class_decl || class_decl->kind != CTSC_SK_ClassDeclaration || !param
        || param->kind != CTSC_SK_Parameter) {
        return false;
    }
    const CtscNodeArray* tps = &class_decl->data.classDeclaration.type_parameters;
    if (tp_index >= tps->len) return false;
    const CtscNode* tp = tps->items[tp_index];
    if (!tp || tp->kind != CTSC_SK_TypeParameter) return false;
    const CtscNode* tnm = tp->data.typeParameter.name;
    if (!tnm || tnm->kind != CTSC_SK_Identifier) return false;
    const CtscIdentifierData* tid = &tnm->data.identifier;

    const CtscNode* pty = param->data.parameter.type;
    if (!pty || pty->kind != CTSC_SK_TypeReference) return false;
    const CtscTypeReferenceData* tr = &pty->data.typeReference;
    if (!tr->typeName || tr->typeName->kind != CTSC_SK_Identifier) return false;
    const CtscIdentifierData* pid = &tr->typeName->data.identifier;
    return utf16_type_text_equal(pid->text, pid->text_len, tid->text, tid->text_len);
}

/*
 * Infer `Box<number>` from `new Box(1)` by matching constructor parameters typed
 * with class type parameters to `new` arguments (checker.ts inferTypeArguments
 * / resolveCall on construct signatures ~35827, ~37131).
 */
static CtscType* inferred_generic_class_instance_from_new(Walk* w, CtscTypeRegistry* reg,
                                                          const CtscNode* class_decl,
                                                          const CtscIdentifierData* class_name_id,
                                                          const CtscNewExpressionData* ne) {
    if (!w || !reg || !class_decl || class_decl->kind != CTSC_SK_ClassDeclaration || !class_name_id || !ne)
        return NULL;
    const CtscClassDeclarationData* cd = &class_decl->data.classDeclaration;
    size_t nt = cd->type_parameters.len;
    if (nt == 0 || nt > 32) return NULL;

    const CtscNode* ctor = constructor_method_declaration(class_decl);
    if (!ctor || ctor->kind != CTSC_SK_MethodDeclaration) return NULL;
    const CtscMethodDeclarationData* md = &ctor->data.methodDeclaration;
    if (!ne->has_arguments) return NULL;

    CtscType* inferred[32];
    for (size_t i = 0; i < nt; ++i) inferred[i] = NULL;

    for (size_t pi = 0; pi < md->parameters.len; ++pi) {
        const CtscNode* param = md->parameters.items[pi];
        if (!param || param->kind != CTSC_SK_Parameter) continue;
        for (size_t ti = 0; ti < nt; ++ti) {
            if (!constructor_parameter_matches_class_type_parameter(class_decl, param, ti)) continue;
            if (inferred[ti] != NULL) break;
            if (pi >= ne->arguments.len) break;
            CtscType* arg_t = type_of_expression(w, reg, ne->arguments.items[pi]);
            if (!arg_t) break;
            /* Widen literals for type-parameter inference (checker.ts inferTypesFromArguments ~35827). */
            inferred[ti] =
                widen_nullish_when_not_strict_null(reg, ctsc_type_widen(reg, arg_t));
            break;
        }
    }

    for (size_t i = 0; i < nt; ++i) {
        if (!inferred[i]) return NULL;
    }

    CtscType** slot = (CtscType**)ctsc_arena_alloc(w->arena, nt * sizeof(CtscType*));
    memcpy(slot, inferred, nt * sizeof(CtscType*));
    return ctsc_type_reference_with_type_args(reg, class_name_id->text, class_name_id->text_len, slot, nt);
}

/*
 * Inferred variable type: getWidenedLiteralTypeForInitializer (~41455) then
 * widenTypeForVariableLikeDeclaration → getWidenedType (~12480-12495).
 * const keeps literal types except null/undefined widening (above).
 */
static CtscType* variable_declaration_type(Walk* w, const CtscNode* decl, bool is_const) {
    const CtscVariableDeclarationData* d = &decl->data.variableDeclaration;
    if (d->type) return type_of_type_node(w, &w->r->registry, w->source_utf16, w->source_utf16_len, d->type, 0);
    CtscType* init_t = type_of_expression(w, &w->r->registry, d->initializer);
    if (!init_t) return w->r->registry.t_any;
    CtscType* after_literal = is_const ? init_t : ctsc_type_widen(&w->r->registry, init_t);
    return widen_nullish_when_not_strict_null(&w->r->registry, after_literal);
}

/*
 * Class field type: annotation or widened initializer (checker.ts
 * checkPropertyDeclaration / getTypeOfVariableOrParameterOrPropertyWorker
 * ~12537).
 */
static CtscType* class_property_declaration_type(Walk* w, const CtscNode* decl) {
    if (!decl || decl->kind != CTSC_SK_PropertyDeclaration) return w->r->registry.t_any;
    const CtscPropertyDeclarationData* d = &decl->data.propertyDeclaration;
    if (d->type) {
        return type_of_type_node(w, &w->r->registry, w->source_utf16, w->source_utf16_len, d->type, 0);
    }
    CtscType* init_t = type_of_expression(w, &w->r->registry, d->initializer);
    if (!init_t) return w->r->registry.t_any;
    return widen_nullish_when_not_strict_null(&w->r->registry, ctsc_type_widen(&w->r->registry, init_t));
}

/*
 * Mirrors parser / checker static vs instance member lists (checker.ts
 * getPropertyOfType on staticType vs instanceType ~11575, ~15893).
 */
static bool modifier_list_has_static(const CtscNodeArray* mods) {
    if (!mods) return false;
    for (size_t i = 0; i < mods->len; ++i) {
        CtscNode* m = mods->items[i];
        if (m && m->kind == CTSC_SK_StaticKeyword) return true;
    }
    return false;
}

static CtscType* property_type_from_class_declaration(Walk* w, const CtscNode* class_node,
                                                      const uint16_t* prop_name, size_t prop_len,
                                                      unsigned depth, bool static_side) {
    if (!class_node || class_node->kind != CTSC_SK_ClassDeclaration) return NULL;
    if (depth > 64) return NULL;
    const CtscNodeArray* members = &class_node->data.classDeclaration.members;
    for (size_t i = 0; i < members->len; ++i) {
        CtscNode* m = members->items[i];
        if (!m) continue;
        if (m->kind == CTSC_SK_PropertyDeclaration) {
            if (modifier_list_has_static(&m->data.propertyDeclaration.modifiers) != static_side) continue;
            const CtscNode* pname = m->data.propertyDeclaration.name;
            if (!pname || pname->kind != CTSC_SK_Identifier) continue;
            if (pname->data.identifier.text_len != prop_len) continue;
            if (memcmp(pname->data.identifier.text, prop_name, prop_len * sizeof(uint16_t)) != 0) continue;
            return class_property_declaration_type(w, m);
        }
        if (m->kind == CTSC_SK_GetAccessor) {
            if (modifier_list_has_static(&m->data.accessorDeclaration.modifiers) != static_side) continue;
            const CtscNode* pname = m->data.accessorDeclaration.name;
            if (!pname || pname->kind != CTSC_SK_Identifier) continue;
            if (pname->data.identifier.text_len != prop_len) continue;
            if (memcmp(pname->data.identifier.text, prop_name, prop_len * sizeof(uint16_t)) != 0) continue;
            return return_type_of_get_accessor_declaration(w, m);
        }
    }
    /*
     * Inherited members: walk `extends` heritage (instance and static both
     * inherit declarations from the base class symbol).
     */
    const CtscNodeArray* hcs = &class_node->data.classDeclaration.heritage_clauses;
    for (size_t hi = 0; hi < hcs->len; ++hi) {
        CtscNode* hc = hcs->items[hi];
        if (!hc || hc->kind != CTSC_SK_HeritageClause) continue;
        if (hc->data.heritageClause.token != CTSC_SK_ExtendsKeyword) continue;
        const CtscNodeArray* ext_types = &hc->data.heritageClause.types;
        for (size_t ti = 0; ti < ext_types->len; ++ti) {
            CtscNode* ewta = ext_types->items[ti];
            if (!ewta || ewta->kind != CTSC_SK_ExpressionWithTypeArguments) continue;
            const CtscNode* base_expr = ewta->data.expressionWithTypeArguments.expression;
            if (!base_expr || base_expr->kind != CTSC_SK_Identifier) continue;
            const CtscIdentifierData* bid = &base_expr->data.identifier;
            CtscSymbol* base_sym = resolve_name(&w->scopes, bid->text, bid->text_len);
            if (!base_sym) continue;
            for (size_t di = 0; di < base_sym->decls_len; ++di) {
                CtscNode* bd = base_sym->decls[di];
                if (!bd || bd->kind != CTSC_SK_ClassDeclaration) continue;
                CtscType* inherited =
                    property_type_from_class_declaration(w, bd, prop_name, prop_len, depth + 1, static_side);
                if (inherited) return inherited;
            }
        }
    }
    return NULL;
}

static const char* syntax_kind_cstr(CtscSyntaxKind k) { return ctsc_syntax_kind_name(k); }

/* ----- walker ----- */

static void visit(Walk* w, const CtscNode* n);
static void visit_class_declaration(Walk* w, const CtscNode* n);
static CtscType* infer_return_type_for_arrow_function(Walk* w, CtscTypeRegistry* reg, CtscArena* arena,
                                                     const CtscNode* arrow);
static void emit_function_signature_string(CtscBuffer* out, const CtscNodeArray* type_parameters,
                                           const CtscNodeArray* params, CtscTypeRegistry* reg,
                                           const CtscNode* return_type_node, CtscType* inferred_return_type,
                                           const uint16_t* src, size_t src_len);
static void push_entry_with_string(Walk* w, const CtscNode* decl, const CtscNode* name_id, const char* type_str,
                                   size_t type_str_len);

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
    e.type = widen_union_remove_nullish_for_types_dump(&w->r->registry, type ? type : w->r->registry.t_any);
    entry_push(w->r, w->arena, e);
}

static void push_property_signatures_from_type_literal(Walk* w, const CtscNode* type_literal) {
    if (!w || !type_literal || type_literal->kind != CTSC_SK_TypeLiteral) return;
    TypeLiteralMember tmp[32];
    size_t n = 0;
    TypeLiteralStringIndexInfo idx;
    (void)type_from_type_literal_span(&w->r->registry, w->source_utf16, w->source_utf16_len, type_literal->pos,
                                      type_literal->end, tmp, sizeof(tmp) / sizeof(tmp[0]), &n, &idx);
    if (idx.has_string_index && idx.param_name_ptr && idx.param_name_len > 0) {
        CtscCheckTypeEntry pe = {0};
        pe.name = idx.param_name_ptr;
        pe.name_len = idx.param_name_len;
        pe.decl_kind_name = syntax_kind_cstr(CTSC_SK_Parameter);
        pe.pos = idx.param_name_pos;
        pe.end = idx.param_name_end;
        pe.type =
            widen_union_remove_nullish_for_types_dump(&w->r->registry, idx.key_type ? idx.key_type : w->r->registry.t_any);
        entry_push(w->r, w->arena, pe);
    }
    for (size_t i = 0; i < n; i++) {
        CtscCheckTypeEntry e = {0};
        e.name = tmp[i].name_ptr;
        e.name_len = tmp[i].name_len;
        e.decl_kind_name = syntax_kind_cstr(CTSC_SK_PropertySignature);
        e.pos = tmp[i].name_pos;
        e.end = tmp[i].name_end;
        e.type = widen_union_remove_nullish_for_types_dump(&w->r->registry,
                                                             tmp[i].value_type ? tmp[i].value_type : w->r->registry.t_any);
        entry_push(w->r, w->arena, e);
    }
}

static void push_property_signatures_from_interface_declaration(Walk* w, const CtscNode* idecl) {
    if (!w || !idecl || idecl->kind != CTSC_SK_InterfaceDeclaration) return;
    int bo = find_interface_body_open_brace(w->source_utf16, w->source_utf16_len, idecl->pos, idecl->end);
    if (bo < 0) return;
    TypeLiteralMember tmp[32];
    size_t n = 0;
    TypeLiteralStringIndexInfo idx;
    (void)type_from_type_literal_span(&w->r->registry, w->source_utf16, w->source_utf16_len, bo, idecl->end, tmp,
                                      sizeof(tmp) / sizeof(tmp[0]), &n, &idx);
    if (idx.has_string_index && idx.param_name_ptr && idx.param_name_len > 0) {
        CtscCheckTypeEntry pe = {0};
        pe.name = idx.param_name_ptr;
        pe.name_len = idx.param_name_len;
        pe.decl_kind_name = syntax_kind_cstr(CTSC_SK_Parameter);
        pe.pos = idx.param_name_pos;
        pe.end = idx.param_name_end;
        pe.type =
            widen_union_remove_nullish_for_types_dump(&w->r->registry, idx.key_type ? idx.key_type : w->r->registry.t_any);
        entry_push(w->r, w->arena, pe);
    }
    for (size_t i = 0; i < n; i++) {
        CtscCheckTypeEntry e = {0};
        e.name = tmp[i].name_ptr;
        e.name_len = tmp[i].name_len;
        e.decl_kind_name = syntax_kind_cstr(CTSC_SK_PropertySignature);
        e.pos = tmp[i].name_pos;
        e.end = tmp[i].name_end;
        e.type = widen_union_remove_nullish_for_types_dump(&w->r->registry,
                                                             tmp[i].value_type ? tmp[i].value_type : w->r->registry.t_any);
        entry_push(w->r, w->arena, e);
    }
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

/*
 * Array literal checked against a tuple annotation (checker.ts checkTypeRelatedTo
 * for tuple targets ~22000+ / elementwise checks).
 */
static bool array_literal_assignable_to_tuple_type(Walk* w, const CtscNode* initializer, CtscType* tuple_ann) {
    if (!initializer || initializer->kind != CTSC_SK_ArrayLiteralExpression || !tuple_ann
        || tuple_ann->kind != CTSC_TYPE_TUPLE) {
        return false;
    }
    const CtscArrayLiteralExpressionData* ar = &initializer->data.arrayLiteralExpression;
    if (ar->elements.len != tuple_ann->tuple_elements_len) return false;
    for (size_t i = 0; i < ar->elements.len; i++) {
        CtscNode* el = ar->elements.items[i];
        if (!el || el->kind == CTSC_SK_SpreadElement) return false;
        CtscType* et = type_of_expression(w, &w->r->registry, el);
        if (!is_assignable_to(w, &w->r->registry, et, tuple_ann->tuple_elements[i])) return false;
    }
    return true;
}

/*
 * VariableDeclarationList from a VariableStatement or a C-style ForStatement
 * initializer. Mirrors checker visiting order with bindVariableDeclaration in
 * binder.ts bindVariableDeclaration (~3648) scoped to ForStatement as
 * block-scope container (binder.ts getContainerFlags ~3876).
 */
static void visit_variable_declaration_list(Walk* w, const CtscNode* list) {
    if (!list || list->kind != CTSC_SK_VariableDeclarationList) return;
    bool is_const = is_const_decl_list(list);
    const CtscNodeArray* decls = &list->data.variableDeclarationList.declarations;
    for (size_t i = 0; i < decls->len; ++i) {
        const CtscNode* d = decls->items[i];
        if (!d || d->kind != CTSC_SK_VariableDeclaration) continue;
        var_decl_const_register(w, d, is_const);
    }
    for (size_t i = 0; i < decls->len; ++i) {
        const CtscNode* d = decls->items[i];
        if (!d || d->kind != CTSC_SK_VariableDeclaration) continue;
        const CtscVariableDeclarationData* vd = &d->data.variableDeclaration;
        if (vd->name && vd->name->kind == CTSC_SK_Identifier && !vd->type && vd->initializer
            && vd->initializer->kind == CTSC_SK_ArrowFunction) {
            const CtscArrowFunctionData* af = &vd->initializer->data.arrowFunction;
            CtscType* inferred_ret =
                infer_return_type_for_arrow_function(w, &w->r->registry, w->arena, vd->initializer);
            CtscBuffer sig;
            ctsc_buf_init(&sig);
            emit_function_signature_string(&sig, &af->type_parameters, &af->parameters, &w->r->registry, NULL,
                                           inferred_ret, w->source_utf16, w->source_utf16_len);
            char* s = (char*)ctsc_arena_alloc(w->arena, sig.len);
            memcpy(s, sig.data, sig.len);
            size_t slen = sig.len;
            ctsc_buf_free(&sig);
            push_entry_with_string(w, d, vd->name, s, slen);
        } else if (vd->name && vd->name->kind == CTSC_SK_Identifier && !vd->type && vd->initializer
                   && vd->initializer->kind == CTSC_SK_FunctionExpression) {
            /*
             * FunctionExpression in a VariableDeclaration: same signature string as
             * ArrowFunction (checker.ts getReturnTypeFromBody ~39195 + getWidenedType
             * ~39276-39277; typeToString uses `=>` form for callable display).
             */
            const CtscFunctionDeclarationData* fe = &vd->initializer->data.functionDeclaration;
            CtscType* inferred_ret = NULL;
            if (!fe->type) {
                inferred_ret = infer_return_type_for_function_body(w, &w->r->registry, w->arena, fe->body,
                                                                   vd->initializer);
            }
            CtscBuffer sig;
            ctsc_buf_init(&sig);
            emit_function_signature_string(&sig, &fe->type_parameters, &fe->parameters, &w->r->registry, fe->type,
                                           inferred_ret, w->source_utf16, w->source_utf16_len);
            char* s = (char*)ctsc_arena_alloc(w->arena, sig.len);
            memcpy(s, sig.data, sig.len);
            size_t slen = sig.len;
            ctsc_buf_free(&sig);
            push_entry_with_string(w, d, vd->name, s, slen);
        } else {
            const CtscNode* nm = vd->name;
            if (nm && nm->kind == CTSC_SK_ObjectBindingPattern) {
                CtscType* init_t;
                if (vd->initializer) {
                    init_t = type_of_expression(w, &w->r->registry, vd->initializer);
                } else if (vd->type) {
                    init_t = type_of_type_node(w, &w->r->registry, w->source_utf16, w->source_utf16_len, vd->type, 0);
                } else {
                    init_t = w->r->registry.t_any;
                }
                register_object_binding_pattern_types_from_init(w, nm, init_t, is_const);
            }
            if (nm && nm->kind == CTSC_SK_ArrayBindingPattern) {
                CtscType* init_t;
                if (vd->initializer) {
                    init_t = type_of_expression(w, &w->r->registry, vd->initializer);
                } else if (vd->type) {
                    init_t = type_of_type_node(w, &w->r->registry, w->source_utf16, w->source_utf16_len, vd->type, 0);
                } else {
                    init_t = w->r->registry.t_any;
                }
                register_array_binding_pattern_types_from_init(w, nm, init_t, vd->initializer, is_const);
            }
            if (!nm || nm->kind == CTSC_SK_Identifier) {
                CtscType* t = variable_declaration_type(w, d, is_const);
                push_entry_for_name(w, d, nm, t);
            }
        }
        if (vd->type && vd->type->kind == CTSC_SK_TypeLiteral) {
            push_property_signatures_from_type_literal(w, vd->type);
        }
        if (vd->type && vd->initializer) {
            CtscType* ann = type_of_type_node(w, &w->r->registry, w->source_utf16, w->source_utf16_len, vd->type, 0);
            CtscType* init_t = type_of_expression(w, &w->r->registry, vd->initializer);
            bool assign_ok;
            if (ann->kind == CTSC_TYPE_TUPLE && vd->initializer->kind == CTSC_SK_ArrayLiteralExpression) {
                assign_ok = array_literal_assignable_to_tuple_type(w, vd->initializer, ann);
            } else {
                assign_ok = is_assignable_to(w, &w->r->registry, init_t, ann);
            }
            if (!assign_ok) {
                bool emitted = false;
                if (vd->initializer->kind == CTSC_SK_ObjectLiteralExpression && init_t
                    && init_t->kind == CTSC_TYPE_OBJECT_LITERAL) {
                    CtscType* ann_struct = NULL;
                    if (ann->kind == CTSC_TYPE_REFERENCE) {
                        ann_struct = resolve_type_reference_to_object_literal(w, &w->r->registry, ann);
                    } else if (ann->kind == CTSC_TYPE_OBJECT_LITERAL) {
                        ann_struct = ann;
                    }
                    if (ann_struct && ann_struct->kind == CTSC_TYPE_OBJECT_LITERAL) {
                        emitted = try_emit_ts2322_object_literal_structural_mismatch(w, vd->initializer, init_t,
                                                                                   ann_struct);
                    }
                }
                if (!emitted) {
                    emit_ts2322_on_binding_name(w->r, w->arena, &w->r->registry, vd->name, init_t, ann);
                }
            }
        }
        /* Still walk initializer so identifier references inside get checked. */
        if (d->data.variableDeclaration.initializer) {
            visit(w, d->data.variableDeclaration.initializer);
        }
    }
}

static void visit_variable_statement(Walk* w, const CtscNode* n) {
    visit_variable_declaration_list(w, n->data.variableStatement.declarationList);
}

static CtscType* param_type(Walk* w, const CtscNode* param) {
    const CtscParameterData* p = &param->data.parameter;
    if (p->type) return type_of_type_node(w, &w->r->registry, w->source_utf16, w->source_utf16_len, p->type, 0);
    return w->r->registry.t_any;
}

static CtscType* type_of_symbol_value(Walk* w, CtscSymbol* sym) {
    if (!sym || sym->decls_len == 0) return w->r->registry.t_any;
    for (size_t i = 0; i < sym->decls_len; ++i) {
        CtscNode* d = sym->decls[i];
        if (!d) continue;
        switch (d->kind) {
            case CTSC_SK_VariableDeclaration:
                return variable_declaration_type(w, d, var_decl_is_const(w, d));
            case CTSC_SK_BindingElement: {
                CtscType* bt = binding_elem_type_lookup(w, d);
                return bt ? bt : w->r->registry.t_any;
            }
            case CTSC_SK_Parameter:
                return param_type(w, d);
            case CTSC_SK_ClassDeclaration: {
                const CtscNode* cname = d->data.classDeclaration.name;
                if (cname && cname->kind == CTSC_SK_Identifier) {
                    const CtscIdentifierData* cid = &cname->data.identifier;
                    return ctsc_type_class_constructor(&w->r->registry, cid->text, cid->text_len);
                }
                break;
            }
            case CTSC_SK_EnumDeclaration: {
                const CtscNode* en = d->data.enumDeclaration.name;
                if (en && en->kind == CTSC_SK_Identifier) {
                    const CtscIdentifierData* eid = &en->data.identifier;
                    return ctsc_type_enum_value(&w->r->registry, eid->text, eid->text_len);
                }
                break;
            }
            default:
                break;
        }
    }
    return w->r->registry.t_any;
}

typedef enum {
    CTSC_TYPEOF_CMP_NONE = 0,
    CTSC_TYPEOF_CMP_STRING,
    CTSC_TYPEOF_CMP_NUMBER,
    CTSC_TYPEOF_CMP_BOOLEAN,
} CtscTypeofResultCompareKind;

static bool string_literal_is_typeof_result_keyword(const CtscNode* n, const char* kw, size_t kw_len) {
    if (!n || n->kind != CTSC_SK_StringLiteral || !kw) return false;
    const CtscStringLiteralData* d = &n->data.stringLiteral;
    const uint16_t* v = d->value ? d->value : d->text;
    size_t len = d->value ? d->value_len : d->text_len;
    if (len != kw_len) return false;
    for (size_t i = 0; i < kw_len; i++) {
        if (v[i] != (uint16_t)(unsigned char)kw[i]) return false;
    }
    return true;
}

static CtscTypeofResultCompareKind if_condition_typeof_ident_eq_primitive_string_compare(
    Walk* w, const CtscNode* cond, CtscSymbol** out_sym) {
    if (out_sym) *out_sym = NULL;
    if (!w || !cond || cond->kind != CTSC_SK_BinaryExpression) return CTSC_TYPEOF_CMP_NONE;
    const CtscBinaryExpressionData* b = &cond->data.binaryExpression;
    if (b->operator_kind != CTSC_SK_EqualsEqualsEqualsToken && b->operator_kind != CTSC_SK_EqualsEqualsToken) {
        return CTSC_TYPEOF_CMP_NONE;
    }
    const CtscNode* L = b->left;
    const CtscNode* R = b->right;
    static const char kstr[] = "string";
    static const char knum[] = "number";
    static const char kbool[] = "boolean";
    const CtscNode* typeof_node = NULL;
    const CtscNode* lit = NULL;
    CtscTypeofResultCompareKind kind = CTSC_TYPEOF_CMP_NONE;
    if (L && L->kind == CTSC_SK_TypeOfExpression) {
        typeof_node = L;
        lit = R;
    } else if (R && R->kind == CTSC_SK_TypeOfExpression) {
        typeof_node = R;
        lit = L;
    } else {
        return CTSC_TYPEOF_CMP_NONE;
    }
    if (string_literal_is_typeof_result_keyword(lit, kstr, sizeof(kstr) - 1)) {
        kind = CTSC_TYPEOF_CMP_STRING;
    } else if (string_literal_is_typeof_result_keyword(lit, knum, sizeof(knum) - 1)) {
        kind = CTSC_TYPEOF_CMP_NUMBER;
    } else if (string_literal_is_typeof_result_keyword(lit, kbool, sizeof(kbool) - 1)) {
        kind = CTSC_TYPEOF_CMP_BOOLEAN;
    } else {
        return CTSC_TYPEOF_CMP_NONE;
    }
    const CtscNode* inner = typeof_node->data.voidExpression.expression;
    if (!inner || inner->kind != CTSC_SK_Identifier) return CTSC_TYPEOF_CMP_NONE;
    CtscSymbol* sym =
        resolve_name(&w->scopes, inner->data.identifier.text, inner->data.identifier.text_len);
    if (out_sym) *out_sym = sym;
    return sym ? kind : CTSC_TYPEOF_CMP_NONE;
}

/*
 * `id === "lit"` / `"lit" === id` for control-flow narrowing (checker.ts
 * narrowTypeByEquality ~29752-29787).
 */
static bool if_condition_ident_eq_string_literal_compare(Walk* w, const CtscNode* cond, CtscSymbol** out_sym,
                                                        const uint16_t** out_lit_text, size_t* out_lit_len) {
    if (out_sym) *out_sym = NULL;
    if (out_lit_text) *out_lit_text = NULL;
    if (out_lit_len) *out_lit_len = 0;
    if (!w || !cond || cond->kind != CTSC_SK_BinaryExpression) return false;
    const CtscBinaryExpressionData* b = &cond->data.binaryExpression;
    if (b->operator_kind != CTSC_SK_EqualsEqualsEqualsToken && b->operator_kind != CTSC_SK_EqualsEqualsToken) {
        return false;
    }
    const CtscNode* id_node = NULL;
    const CtscNode* str_node = NULL;
    if (b->left && b->left->kind == CTSC_SK_Identifier && b->right && b->right->kind == CTSC_SK_StringLiteral) {
        id_node = b->left;
        str_node = b->right;
    } else if (b->right && b->right->kind == CTSC_SK_Identifier && b->left && b->left->kind == CTSC_SK_StringLiteral) {
        id_node = b->right;
        str_node = b->left;
    } else {
        return false;
    }
    CtscSymbol* sym =
        resolve_name(&w->scopes, id_node->data.identifier.text, id_node->data.identifier.text_len);
    if (!sym) return false;
    const CtscStringLiteralData* sd = &str_node->data.stringLiteral;
    const uint16_t* v = sd->value ? sd->value : sd->text;
    size_t vl = sd->value ? sd->value_len : sd->text_len;
    if (out_sym) *out_sym = sym;
    if (out_lit_text) *out_lit_text = v;
    if (out_lit_len) *out_lit_len = vl;
    return true;
}

/*
 * Aggregate ReturnStatement expression types inside a function body, mirroring
 * checker.ts getReturnTypeFromBody (~39195-39250): union the expression types,
 * then getWidenedType (~39276-39277) on the result (not per-return before union).
 * Nested function / arrow / class bodies are not descended into so inner
 * returns do not affect the outer signature.
 */
typedef struct {
    CtscTypeRegistry* reg;
    CtscArena*        arena;
    Walk*             w; /* NULL: ReturnStatement operand types treat Identifier as any */
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
                ret_collector_push(c, type_of_expression(c->w, c->reg, n->data.returnStatement.expression));
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

/*
 * Structural equality for deduping aggregated return expression types before
 * unioning. Mirrors getUnionType deduping distinct Type objects that represent
 * the same type (checker.ts ~39249-39250).
 */
static bool inferred_return_type_equal(CtscArena* arena, const CtscType* a, const CtscType* b) {
    if (a == b) return true;
    if (!a || !b) return false;
    if (a->kind != b->kind) return false;
    switch (a->kind) {
        case CTSC_TYPE_NUMBER_LITERAL:
            return a->number_value == b->number_value;
        case CTSC_TYPE_STRING_LITERAL:
        case CTSC_TYPE_BIGINT_LITERAL:
        case CTSC_TYPE_REFERENCE:
        case CTSC_TYPE_CLASS_CONSTRUCTOR:
            return utf16_type_text_equal(a->text, a->text_len, b->text, b->text_len);
        case CTSC_TYPE_BOOLEAN_LITERAL:
            return a->boolean_value == b->boolean_value;
        case CTSC_TYPE_UNION: {
            size_t n = a->union_members_len;
            if (n != b->union_members_len) return false;
            if (n == 0) return true;
            bool* used = (bool*)ctsc_arena_alloc(arena, n * sizeof(bool));
            if (!used) return false;
            memset(used, 0, n * sizeof(bool));
            for (size_t i = 0; i < n; ++i) {
                bool found = false;
                for (size_t j = 0; j < n; ++j) {
                    if (used[j]) continue;
                    if (inferred_return_type_equal(arena, a->union_members[i], b->union_members[j])) {
                        used[j] = true;
                        found = true;
                        break;
                    }
                }
                if (!found) return false;
            }
            return true;
        }
        case CTSC_TYPE_INTERSECTION: {
            size_t n = a->intersection_members_len;
            if (n != b->intersection_members_len) return false;
            if (n == 0) return true;
            for (size_t i = 0; i < n; ++i) {
                if (!inferred_return_type_equal(arena, a->intersection_members[i], b->intersection_members[i])) {
                    return false;
                }
            }
            return true;
        }
        case CTSC_TYPE_OBJECT_LITERAL: {
            if (a->object_properties_len != b->object_properties_len) return false;
            for (size_t i = 0; i < a->object_properties_len; ++i) {
                const CtscObjectProperty* ap = &a->object_properties[i];
                const CtscObjectProperty* bp = &b->object_properties[i];
                if (ap->name_len != bp->name_len
                    || !utf16_type_text_equal(ap->name, ap->name_len, bp->name, bp->name_len)) {
                    return false;
                }
                if (ap->optional != bp->optional) return false;
                if (!inferred_return_type_equal(arena, ap->value_type, bp->value_type)) return false;
            }
            return true;
        }
        case CTSC_TYPE_TUPLE: {
            if (a->tuple_elements_len != b->tuple_elements_len) return false;
            for (size_t i = 0; i < a->tuple_elements_len; ++i) {
                if (!inferred_return_type_equal(arena, a->tuple_elements[i], b->tuple_elements[i])) return false;
            }
            return true;
        }
        default:
            return true;
    }
}

/*
 * checker.ts getReturnTypeFromBody (~39249-39250): union return expression types,
 * then getWidenedType (~39276-39277) on the aggregate. Widening literals before
 * unioning would collapse e.g. `1` and `2` to `number`; tsc keeps `1 | 2`.
 */
static CtscType* widen_inferred_return_types(CtscTypeRegistry* reg, CtscArena* arena,
                                            CtscType** raw, size_t n) {
    if (n == 0) return reg->t_void;
    CtscType** uniq = (CtscType**)ctsc_arena_alloc(arena, n * sizeof(CtscType*));
    size_t ulen = 0;
    for (size_t i = 0; i < n; ++i) {
        CtscType* t = widen_nullish_when_not_strict_null(reg, raw[i]);
        if (!t) continue;
        bool seen = false;
        for (size_t j = 0; j < ulen; ++j) {
            if (inferred_return_type_equal(arena, uniq[j], t)) {
                seen = true;
                break;
            }
        }
        if (!seen) uniq[ulen++] = t;
    }
    if (ulen == 0) return reg->t_void;
    if (ulen == 1) return ctsc_type_widen(reg, uniq[0]);
    {
        CtscType* u = ctsc_type_new(reg, CTSC_TYPE_UNION);
        u->union_members = (CtscType**)ctsc_arena_alloc(arena, ulen * sizeof(CtscType*));
        memcpy(u->union_members, uniq, ulen * sizeof(CtscType*));
        u->union_members_len = ulen;
        return u;
    }
}

static CtscType* infer_return_type_for_function_body(Walk* w, CtscTypeRegistry* reg, CtscArena* arena,
                                                     const CtscNode* body, const CtscNode* scope_container) {
    if (!body || body->kind != CTSC_SK_Block) return reg->t_void;
    CtscScope* fn_scope = NULL;
    if (w && scope_container) {
        fn_scope = find_scope_for_node(w->scopes.binding, scope_container);
    }
    if (fn_scope) scope_push(&w->scopes, fn_scope);
    RetCollector c;
    memset(&c, 0, sizeof(c));
    c.reg = reg;
    c.arena = arena;
    c.w = w;
    const CtscNodeArray* stmts = &body->data.block.statements;
    for (size_t i = 0; i < stmts->len; ++i) {
        collect_return_expression_types(&c, stmts->items[i]);
    }
    if (fn_scope) scope_pop(&w->scopes);
    return widen_inferred_return_types(reg, arena, c.items, c.len);
}

/*
 * Inferred return type for an arrow's ConciseBody: Block uses the same return
 * aggregation as functions; expression body mirrors checker.ts
 * getReturnTypeFromBody (~39208-39210) then getWidenedType on the result
 * (~39276-39277) for display in the signature string.
 */
static CtscType* infer_return_type_for_arrow_function(Walk* w, CtscTypeRegistry* reg, CtscArena* arena,
                                                     const CtscNode* arrow) {
    if (!arrow || arrow->kind != CTSC_SK_ArrowFunction) return reg->t_void;
    const CtscNode* body = arrow->data.arrowFunction.body;
    if (!body) return reg->t_void;
    if (body->kind == CTSC_SK_Block) {
        return infer_return_type_for_function_body(w, reg, arena, body, arrow);
    }
    /* Concise body uses checkExpressionCached on the body (~39208-39210); parameters
     * must be in scope — push the binder scope before resolving identifiers. */
    CtscScope* arrow_scope = w ? find_scope_for_node(w->scopes.binding, arrow) : NULL;
    if (arrow_scope) scope_push(&w->scopes, arrow_scope);
    CtscType* t = type_of_expression(w, reg, body);
    if (arrow_scope) scope_pop(&w->scopes);
    if (!t) return reg->t_void;
    return widen_nullish_when_not_strict_null(reg, ctsc_type_widen(reg, t));
}

/*
 * Return type of a class MethodDeclaration for call-expression typing (checker.ts
 * getReturnTypeOfSignature / resolve call signature ~37851-37878).
 */
static CtscType* return_type_of_method_declaration(Walk* w, const CtscNode* m) {
    if (!m || m->kind != CTSC_SK_MethodDeclaration) return NULL;
    const CtscMethodDeclarationData* md = &m->data.methodDeclaration;
    if (md->type) {
        return type_of_type_node(w, &w->r->registry, w->source_utf16, w->source_utf16_len, md->type, 0);
    }
    if (md->body && md->body->kind == CTSC_SK_Block) {
        CtscType* t = infer_return_type_for_function_body(w, &w->r->registry, w->arena, md->body, m);
        return widen_nullish_when_not_strict_null(&w->r->registry, t);
    }
    return NULL;
}

/*
 * Getter return type: annotation first, else inferred from body (checker.ts
 * getTypeOfAccessors ~12706-12723: getAnnotatedAccessorType(getter) then
 * getReturnTypeFromBody(getter)).
 */
static CtscType* return_type_of_get_accessor_declaration(Walk* w, const CtscNode* m) {
    if (!m || m->kind != CTSC_SK_GetAccessor) return NULL;
    const CtscAccessorDeclarationData* ad = &m->data.accessorDeclaration;
    if (ad->type) {
        return type_of_type_node(w, &w->r->registry, w->source_utf16, w->source_utf16_len, ad->type, 0);
    }
    if (ad->body && ad->body->kind == CTSC_SK_Block) {
        CtscType* t = infer_return_type_for_function_body(w, &w->r->registry, w->arena, ad->body, m);
        return widen_nullish_when_not_strict_null(&w->r->registry, t);
    }
    return NULL;
}

/*
 * Instance method lookup by name, including `extends` (checker.ts getPropertyOfType
 * feeding signature resolution ~11575+).
 */
static CtscType* method_return_type_from_class_declaration(Walk* w, const CtscNode* class_node,
                                                           const uint16_t* prop_name, size_t prop_len,
                                                           unsigned depth, bool static_side) {
    if (!class_node || class_node->kind != CTSC_SK_ClassDeclaration) return NULL;
    if (depth > 64) return NULL;
    const CtscNodeArray* members = &class_node->data.classDeclaration.members;
    for (size_t i = 0; i < members->len; ++i) {
        CtscNode* mem = members->items[i];
        if (!mem || mem->kind != CTSC_SK_MethodDeclaration) continue;
        if (modifier_list_has_static(&mem->data.methodDeclaration.modifiers) != static_side) continue;
        const CtscNode* pname = mem->data.methodDeclaration.name;
        if (!pname || pname->kind != CTSC_SK_Identifier) continue;
        if (pname->data.identifier.text_len != prop_len) continue;
        if (memcmp(pname->data.identifier.text, prop_name, prop_len * sizeof(uint16_t)) != 0) continue;
        return return_type_of_method_declaration(w, mem);
    }
    const CtscNodeArray* hcs = &class_node->data.classDeclaration.heritage_clauses;
    for (size_t hi = 0; hi < hcs->len; ++hi) {
        CtscNode* hc = hcs->items[hi];
        if (!hc || hc->kind != CTSC_SK_HeritageClause) continue;
        if (hc->data.heritageClause.token != CTSC_SK_ExtendsKeyword) continue;
        const CtscNodeArray* ext_types = &hc->data.heritageClause.types;
        for (size_t ti = 0; ti < ext_types->len; ++ti) {
            CtscNode* ewta = ext_types->items[ti];
            if (!ewta || ewta->kind != CTSC_SK_ExpressionWithTypeArguments) continue;
            const CtscNode* base_expr = ewta->data.expressionWithTypeArguments.expression;
            if (!base_expr || base_expr->kind != CTSC_SK_Identifier) continue;
            const CtscIdentifierData* bid = &base_expr->data.identifier;
            CtscSymbol* base_sym = resolve_name(&w->scopes, bid->text, bid->text_len);
            if (!base_sym) continue;
            for (size_t di = 0; di < base_sym->decls_len; ++di) {
                CtscNode* bd = base_sym->decls[di];
                if (!bd || bd->kind != CTSC_SK_ClassDeclaration) continue;
                CtscType* inherited = method_return_type_from_class_declaration(w, bd, prop_name, prop_len,
                                                                                depth + 1, static_side);
                if (inherited) return inherited;
            }
        }
    }
    return NULL;
}

static CtscType* return_type_of_function_declaration(Walk* w, const CtscNode* fn) {
    if (!fn || fn->kind != CTSC_SK_FunctionDeclaration) return NULL;
    const CtscFunctionDeclarationData* f = &fn->data.functionDeclaration;
    /* Explicit return annotation wins (checker.ts getReturnTypeFromAnnotation
     * ~39185 path); ambient `declare function` has no body but always a
     * declared annotation (or implicit any in tsc, which we approximate as
     * NULL → caller falls back to t_any). */
    if (f->type) {
        return type_of_type_node(w, &w->r->registry, w->source_utf16, w->source_utf16_len, f->type, 0);
    }
    if (!f->body || f->body->kind != CTSC_SK_Block) return NULL;
    CtscType* t = infer_return_type_for_function_body(w, &w->r->registry, w->arena, f->body, fn);
    return widen_nullish_when_not_strict_null(&w->r->registry, t);
}

/*
 * True when `param`'s type annotation is a TypeReference whose name matches a
 * TypeParameter on `fn_decl` (checker.ts unconstrained type parameter assignability).
 */
static bool parameter_type_is_declared_type_parameter(const CtscNode* fn_decl, const CtscNode* param) {
    if (!fn_decl || fn_decl->kind != CTSC_SK_FunctionDeclaration) return false;
    if (!param || param->kind != CTSC_SK_Parameter) return false;
    const CtscNode* pty = param->data.parameter.type;
    if (!pty || pty->kind != CTSC_SK_TypeReference) return false;
    const CtscTypeReferenceData* tr = &pty->data.typeReference;
    if (!tr->typeName || tr->typeName->kind != CTSC_SK_Identifier) return false;
    const CtscIdentifierData* pid = &tr->typeName->data.identifier;
    const CtscNodeArray* tps = &fn_decl->data.functionDeclaration.type_parameters;
    for (size_t ti = 0; ti < tps->len; ++ti) {
        const CtscNode* tp = tps->items[ti];
        if (!tp || tp->kind != CTSC_SK_TypeParameter) continue;
        const CtscNode* tnm = tp->data.typeParameter.name;
        if (!tnm || tnm->kind != CTSC_SK_Identifier) continue;
        const CtscIdentifierData* tid = &tnm->data.identifier;
        if (utf16_type_text_equal(pid->text, pid->text_len, tid->text, tid->text_len)) return true;
    }
    return false;
}

/*
 * Generic call return instantiation: when the function body's inferred return
 * type is a type parameter (e.g. `return x` with `x: A`), map that parameter
 * to either explicit type arguments (`first<number, string>(...)`) or the
 * corresponding argument type (`first(1, "x")` → `1`).
 *
 * Mirrors checker.ts inferTypeArguments / getReturnTypeOfSignature (~35827+,
 * ~37810+).
 */
static CtscType* generic_call_instantiated_return_type(Walk* w, CtscTypeRegistry* reg, const CtscNode* fn_decl,
                                                      const CtscCallExpressionData* ce) {
    if (!w || !reg || !fn_decl || fn_decl->kind != CTSC_SK_FunctionDeclaration || !ce) return NULL;
    const CtscFunctionDeclarationData* fd = &fn_decl->data.functionDeclaration;
    if (fd->type_parameters.len < 1 || ce->arguments.len < 1 || fd->parameters.len < 1) return NULL;

    CtscType* inferred_ret =
        infer_return_type_for_function_body(w, reg, w->arena, fd->body, fn_decl);
    if (!inferred_ret || inferred_ret->kind != CTSC_TYPE_REFERENCE) return NULL;

    /* Return type must name one of this signature's type parameters (e.g. A in `: A`). */
    size_t ret_tp_index = (size_t)-1;
    for (size_t ti = 0; ti < fd->type_parameters.len; ++ti) {
        const CtscNode* tp = fd->type_parameters.items[ti];
        if (!tp || tp->kind != CTSC_SK_TypeParameter) continue;
        const CtscNode* tnm = tp->data.typeParameter.name;
        if (!tnm || tnm->kind != CTSC_SK_Identifier) continue;
        const CtscIdentifierData* tid = &tnm->data.identifier;
        if (utf16_type_text_equal(inferred_ret->text, inferred_ret->text_len, tid->text, tid->text_len)) {
            ret_tp_index = ti;
            break;
        }
    }
    if (ret_tp_index == (size_t)-1) return NULL;

    /*
     * Explicit `<T0, T1, ...>`: pick the type argument at the same index as
     * the return type parameter (`id<number>(42)` → number; `first<number,
     * string>(1,"x")` → number).
     */
    if (ce->has_type_arguments && ce->type_arguments.len == fd->type_parameters.len && w) {
        if (ret_tp_index < ce->type_arguments.len) {
            CtscType* explicit_t = type_of_type_node(w, reg, w->source_utf16, w->source_utf16_len,
                                                     ce->type_arguments.items[ret_tp_index], 0);
            if (explicit_t) return widen_nullish_when_not_strict_null(reg, explicit_t);
        }
        return NULL;
    }

    /* Infer from arguments: use the parameter whose type is that type parameter. */
    for (size_t i = 0; i < fd->parameters.len && i < ce->arguments.len; ++i) {
        const CtscNode* param = fd->parameters.items[i];
        if (!param || param->kind != CTSC_SK_Parameter) continue;
        if (!parameter_type_is_declared_type_parameter(fn_decl, param)) continue;
        const CtscNode* pty = param->data.parameter.type;
        if (!pty || pty->kind != CTSC_SK_TypeReference) continue;
        const CtscTypeReferenceData* tr = &pty->data.typeReference;
        if (!tr->typeName || tr->typeName->kind != CTSC_SK_Identifier) continue;
        const CtscIdentifierData* pid = &tr->typeName->data.identifier;
        if (!utf16_type_text_equal(pid->text, pid->text_len, inferred_ret->text, inferred_ret->text_len)) continue;
        CtscType* arg_t = type_of_expression(w, reg, ce->arguments.items[i]);
        if (!arg_t) return NULL;
        return widen_nullish_when_not_strict_null(reg, arg_t);
    }
    return NULL;
}

static void emit_type_parameters_for_signature(CtscBuffer* out, const CtscNodeArray* type_parameters) {
    if (!type_parameters || type_parameters->len == 0) return;
    ctsc_buf_append_char(out, '<');
    for (size_t i = 0; i < type_parameters->len; ++i) {
        if (i > 0) ctsc_buf_append_cstr(out, ", ");
        const CtscNode* tp = type_parameters->items[i];
        if (tp && tp->kind == CTSC_SK_TypeParameter) {
            const CtscNode* nm = tp->data.typeParameter.name;
            if (nm && nm->kind == CTSC_SK_Identifier) {
                const CtscIdentifierData* id = &nm->data.identifier;
                append_utf16_ascii_identifier(out, id->text, id->text_len);
            }
        }
    }
    ctsc_buf_append_char(out, '>');
}

static void emit_function_signature_string(CtscBuffer* out, const CtscNodeArray* type_parameters,
                                           const CtscNodeArray* params, CtscTypeRegistry* reg,
                                           const CtscNode* return_type_node, CtscType* inferred_return_type,
                                           const uint16_t* src, size_t src_len) {
    /*
     * tsc's typeToString on a FunctionType uses signatureToString (~6202) /
     * WriteTypeParametersInQualifiedName: optional `<T, U>` then `(p0: T0, ...)
     * => R` at default flags.
     */
    emit_type_parameters_for_signature(out, type_parameters);
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
        if (p->data.parameter.is_optional) ctsc_buf_append_char(out, '?');
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

/*
 * Count the FunctionDeclaration declarations attached to `sym`. Mirrors
 * checker.ts getSignaturesOfSymbol (~16700+): each FunctionDeclaration on
 * the symbol contributes a (possibly overload) signature. We count all
 * FunctionDeclaration decls irrespective of whether they have a body
 * because the "implementation signature" filter happens when selecting
 * which signatures are part of the *type*.
 */
static size_t symbol_function_declaration_count(CtscSymbol* sym) {
    if (!sym) return 0;
    size_t n = 0;
    for (size_t i = 0; i < sym->decls_len; ++i) {
        CtscNode* d = sym->decls[i];
        if (d && d->kind == CTSC_SK_FunctionDeclaration) ++n;
    }
    return n;
}

/*
 * Canonical "overload signatures" for an overloaded function symbol.
 *
 * tsc (checker.ts getSignaturesOfSymbol + isNodeDescendantOf / filter in
 * createAnonymousType for function symbols, ~16700+/~17300+): when a symbol
 * has one bodyless FunctionDeclaration followed by a bodied one, the bodied
 * one is the implementation and is NOT surfaced in the call signatures of
 * the type (it's `excluded`). When every FunctionDeclaration has a body
 * (no explicit overloads), each contributes a signature — but in that case
 * the symbol only has a single FunctionDeclaration anyway for a named
 * function (duplicate definitions would be a bind-time error we don't
 * model). So the rule we implement is:
 *
 *   - If there exists at least one bodyless FunctionDeclaration and at
 *     least one bodied FunctionDeclaration on the symbol, the overload
 *     signatures are the bodyless ones.
 *   - Otherwise (all bodyless, e.g. `declare function f(...)`, or all
 *     bodied, which is the single-declaration case), every
 *     FunctionDeclaration is a signature.
 *
 * Returns the count written into `out` (capacity `out_cap`).
 */
static size_t symbol_collect_overload_signature_decls(CtscSymbol* sym, const CtscNode** out, size_t out_cap) {
    if (!sym || !out || out_cap == 0) return 0;
    bool has_bodyless = false;
    bool has_bodied   = false;
    for (size_t i = 0; i < sym->decls_len; ++i) {
        CtscNode* d = sym->decls[i];
        if (!d || d->kind != CTSC_SK_FunctionDeclaration) continue;
        if (d->data.functionDeclaration.body) has_bodied = true; else has_bodyless = true;
    }
    bool skip_bodied = has_bodyless && has_bodied;
    size_t n = 0;
    for (size_t i = 0; i < sym->decls_len && n < out_cap; ++i) {
        CtscNode* d = sym->decls[i];
        if (!d || d->kind != CTSC_SK_FunctionDeclaration) continue;
        if (skip_bodied && d->data.functionDeclaration.body) continue;
        out[n++] = d;
    }
    return n;
}

/*
 * Emit the type-literal string for an overloaded function symbol:
 *   "{ (p0: T0, ...): R0; (p1: T1, ...): R1; ...; }"
 * Mirrors checker.ts createAnonymousTypeNode for resolved function types
 * with >1 call signatures (~7388+) combined with signatureToSignatureDeclarationHelper
 * (~8009) using SignatureDeclaration kind `CallSignature` which writes
 * "(params): R" (colon separator, no "=>"). The separator between call
 * signatures in a TypeLiteralNode is "; ".
 */
/*
 * Write a single call-signature string `(p0: T0, ...): R` for one
 * FunctionDeclaration. Shared by the overloaded-function type literal
 * emitter (`emit_overload_object_type_string`) and the TS2769 message
 * formatter (`emit_ts2769_no_overload_match`). Mirrors checker.ts
 * signatureToString ~6202 with SignatureKind.Call and the colon separator.
 */
static void emit_call_signature_decl_string(CtscBuffer* out, CtscTypeRegistry* reg, const CtscNode* fn,
                                             const uint16_t* src, size_t src_len) {
    if (!fn || fn->kind != CTSC_SK_FunctionDeclaration) return;
    const CtscFunctionDeclarationData* fd = &fn->data.functionDeclaration;
    emit_type_parameters_for_signature(out, &fd->type_parameters);
    ctsc_buf_append_char(out, '(');
    for (size_t pi = 0; pi < fd->parameters.len; ++pi) {
        const CtscNode* p = fd->parameters.items[pi];
        if (pi > 0) ctsc_buf_append_cstr(out, ", ");
        if (!p || p->kind != CTSC_SK_Parameter) continue;
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
        if (p->data.parameter.is_optional) ctsc_buf_append_char(out, '?');
        ctsc_buf_append_cstr(out, ": ");
        if (p->data.parameter.type) {
            append_type_for_signature(out, p->data.parameter.type, src, src_len);
        } else {
            ctsc_type_to_string(reg->t_any, out);
        }
    }
    ctsc_buf_append_cstr(out, "): ");
    if (fd->type) {
        append_type_for_signature(out, fd->type, src, src_len);
    } else {
        /* Bodyless with no return annotation would be `any` in ambient
         * contexts; for an implementation that slipped through we fall
         * back to `any` rather than recomputing inference. Overload
         * fixtures always annotate return types. */
        ctsc_buf_append_cstr(out, "any");
    }
}

static void emit_overload_object_type_string(CtscBuffer* out, CtscTypeRegistry* reg, const CtscNode** sigs,
                                              size_t sig_count, const uint16_t* src, size_t src_len) {
    ctsc_buf_append_cstr(out, "{ ");
    for (size_t i = 0; i < sig_count; ++i) {
        const CtscNode* fn = sigs[i];
        if (!fn || fn->kind != CTSC_SK_FunctionDeclaration) continue;
        emit_call_signature_decl_string(out, reg, fn, src, src_len);
        ctsc_buf_append_cstr(out, "; ");
    }
    ctsc_buf_append_char(out, '}');
}

static void visit_function_declaration(Walk* w, const CtscNode* n) {
    const CtscFunctionDeclarationData* f = &n->data.functionDeclaration;
    /* Emit a types-channel entry for the function name if present. M4.0 does
     * not model function types structurally — it pre-formats the string the
     * oracle expects (see emit_function_signature_string comment).
     *
     * For an overloaded function (multiple FunctionDeclaration decls on the
     * same symbol), tsc's getTypeAtLocation on ANY of the declaration's name
     * identifiers returns the same combined anonymous function type with one
     * call signature per overload. Mirrors checker.ts getTypeOfSymbol →
     * getTypeOfFuncClassEnumModule → anonymous type with resolved signatures
     * (~12600+/~17300+). */
    if (f->name && f->name->kind == CTSC_SK_Identifier) {
        CtscSymbol* sym = NULL;
        const CtscIdentifierData* nmid = &f->name->data.identifier;
        if (nmid->text && nmid->text_len > 0) {
            sym = resolve_name(&w->scopes, nmid->text, nmid->text_len);
        }
        size_t fn_decl_count = symbol_function_declaration_count(sym);
        if (fn_decl_count > 1) {
            const CtscNode* sigs[16];
            size_t sig_count = symbol_collect_overload_signature_decls(sym, sigs,
                                                                       sizeof(sigs) / sizeof(sigs[0]));
            if (sig_count > 1) {
                CtscBuffer sig; ctsc_buf_init(&sig);
                emit_overload_object_type_string(&sig, &w->r->registry, sigs, sig_count, w->source_utf16,
                                                 w->source_utf16_len);
                char* s = (char*)ctsc_arena_alloc(w->arena, sig.len);
                memcpy(s, sig.data, sig.len);
                size_t slen = sig.len;
                ctsc_buf_free(&sig);
                push_entry_with_string(w, n, f->name, s, slen);
                goto after_name_entry;
            }
        }
        /* Mirrors checker.ts getSignatureOfTypeNode / signatureToString: when
         * an explicit return-type annotation is present (including ambient
         * `declare function f(): T;` shapes), prefer it over the body-inferred
         * type. Matches MethodDeclaration handling above (~md->type branch). */
        CtscType* inferred_ret = NULL;
        if (!f->type) {
            inferred_ret = infer_return_type_for_function_body(w, &w->r->registry, w->arena, f->body, n);
        }
        CtscBuffer sig; ctsc_buf_init(&sig);
        emit_function_signature_string(&sig, &f->type_parameters, &f->parameters, &w->r->registry, f->type,
                                       inferred_ret, w->source_utf16, w->source_utf16_len);
        char* s = (char*)ctsc_arena_alloc(w->arena, sig.len);
        memcpy(s, sig.data, sig.len);
        size_t slen = sig.len;
        ctsc_buf_free(&sig);
        push_entry_with_string(w, n, f->name, s, slen);
    }
after_name_entry:

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

static const CtscNode* symbol_function_declaration_decl(CtscSymbol* sym) {
    if (!sym) return NULL;
    for (size_t i = 0; i < sym->decls_len; ++i) {
        CtscNode* d = sym->decls[i];
        if (d && d->kind == CTSC_SK_FunctionDeclaration) return d;
    }
    return NULL;
}

/*
 * Overload resolution for a CallExpression to a symbol with multiple
 * FunctionDeclaration signatures. Mirrors checker.ts resolveCall /
 * chooseOverload (~36750-36900): iterate candidate signatures (exactly
 * the bodyless overload signatures when any exist, else all decls) in
 * source order and return the first one whose arity admits `args->len`
 * and whose parameter types each accept the corresponding argument type.
 * `any` argument / parameter always matches (is_assignable_to already
 * short-circuits).
 *
 * Returns the best-matching FunctionDeclaration, or NULL if none
 * matches. Callers fall back to the first declaration so the rest of
 * the checker (TS2554 span etc.) still has something to report against.
 */
static const CtscNode* symbol_resolve_overload_for_call(Walk* w, CtscSymbol* sym, const CtscCallExpressionData* ce) {
    if (!w || !sym || !ce) return NULL;
    const CtscNode* sigs[16];
    size_t sig_count = symbol_collect_overload_signature_decls(sym, sigs, sizeof(sigs) / sizeof(sigs[0]));
    if (sig_count == 0) return NULL;
    const CtscNodeArray* args = &ce->arguments;
    for (size_t i = 0; i < sig_count; ++i) {
        const CtscNode* fn = sigs[i];
        int min = function_declaration_min_arguments(fn);
        int max = function_declaration_max_arguments(fn);
        if ((int)args->len < min) continue;
        if (max != INT_MAX && (int)args->len > max) continue;
        const CtscNodeArray* params = &fn->data.functionDeclaration.parameters;
        bool all_ok = true;
        for (size_t ai = 0; ai < args->len; ++ai) {
            if (ai >= params->len) {
                /* Excess args absorbed only by rest; max check above already
                 * rejected the non-rest case. */
                break;
            }
            const CtscNode* p = params->items[ai];
            if (!p || p->kind != CTSC_SK_Parameter) continue;
            if (parameter_type_is_declared_type_parameter(fn, p)) continue;
            CtscType* pt = param_type(w, p);
            CtscType* at = type_of_expression(w, &w->r->registry, args->items[ai]);
            if (!is_assignable_to(w, &w->r->registry, at, pt)) { all_ok = false; break; }
        }
        if (all_ok) return fn;
    }
    return NULL;
}

/*
 * Append one "Argument of type 'X' is not assignable to parameter of type 'Y'."
 * line (same text as TS2345) for the first mismatched argument of `fn` relative
 * to `args`. Returns true if a mismatch was found and appended. Mirrors
 * checker.ts getSignatureApplicabilityError (~36712) restricted to the single
 * top-level argument mismatch we model in M4.
 */
static bool append_overload_argument_error(CtscBuffer* out, Walk* w, const CtscNode* fn,
                                           const CtscNodeArray* args) {
    if (!fn || fn->kind != CTSC_SK_FunctionDeclaration) return false;
    const CtscNodeArray* params = &fn->data.functionDeclaration.parameters;
    for (size_t ai = 0; ai < args->len; ++ai) {
        if (ai >= params->len) break;
        const CtscNode* p = params->items[ai];
        if (!p || p->kind != CTSC_SK_Parameter) continue;
        if (parameter_type_is_declared_type_parameter(fn, p)) continue;
        CtscType* pt = param_type(w, p);
        CtscType* at = type_of_expression(w, &w->r->registry, args->items[ai]);
        if (is_assignable_to(w, &w->r->registry, at, pt)) continue;
        CtscTypeRegistry* reg = &w->r->registry;
        const bool both_str_lit = at && pt
            && at->kind == CTSC_TYPE_STRING_LITERAL
            && pt->kind == CTSC_TYPE_STRING_LITERAL;
        const bool both_num_lit = at && pt
            && at->kind == CTSC_TYPE_NUMBER_LITERAL
            && pt->kind == CTSC_TYPE_NUMBER_LITERAL;
        CtscType* src_m = (both_str_lit || both_num_lit) ? at : ctsc_type_widen(reg, at);
        CtscType* tgt_m = (both_str_lit || both_num_lit) ? pt : ctsc_type_widen(reg, pt);
        ctsc_buf_append_cstr(out, "Argument of type '");
        ctsc_type_to_string(src_m, out);
        ctsc_buf_append_cstr(out, "' is not assignable to parameter of type '");
        ctsc_type_to_string(tgt_m, out);
        ctsc_buf_append_cstr(out, "'.");
        return true;
    }
    return false;
}

/*
 * Emit a TS2769 "No overload matches this call" diagnostic when a call to an
 * overloaded function symbol fails every candidate signature for the same
 * arity. Mirrors checker.ts resolveCall branch at ~36704-36748 where the
 * candidatesForArgumentError list has length ≤ 3: each overload contributes a
 * nested "Overload i of N, '<sig>', gave the following error." chain whose
 * flattened form (ts.flattenDiagnosticMessageText) joins lines with "\n" +
 * "  " * depth.
 *
 * The span is the span common to all inner argument errors (checker.ts
 * ~36740-36742). For the single-argument-mismatch cases we model here that
 * is the argument node itself.
 */
static void emit_ts2769_no_overload_match(Walk* w, const CtscNode* call_node,
                                           const CtscNode** sigs, size_t sig_count,
                                           const CtscNodeArray* args) {
    if (!w || !call_node || !sigs || sig_count < 2) return;
    /* Span defaults to the first failing argument across all overloads (all
     * agree in the simple case). checker.ts ~36740-36742: if every inner diag
     * shares (start, length, file), reuse it. */
    int span_start = -1;
    int span_length = 0;
    for (size_t ai = 0; ai < args->len; ++ai) {
        const CtscNode* arg = args->items[ai];
        if (!arg || arg->pos < 0 || arg->end < arg->pos) continue;
        bool any_mismatch = false;
        for (size_t si = 0; si < sig_count; ++si) {
            const CtscNode* fn = sigs[si];
            if (!fn || fn->kind != CTSC_SK_FunctionDeclaration) continue;
            const CtscNodeArray* params = &fn->data.functionDeclaration.parameters;
            if (ai >= params->len) continue;
            const CtscNode* p = params->items[ai];
            if (!p || p->kind != CTSC_SK_Parameter) continue;
            if (parameter_type_is_declared_type_parameter(fn, p)) continue;
            CtscType* pt = param_type(w, p);
            CtscType* at = type_of_expression(w, &w->r->registry, arg);
            if (!is_assignable_to(w, &w->r->registry, at, pt)) { any_mismatch = true; break; }
        }
        if (any_mismatch) {
            span_start = arg->pos;
            span_length = arg->end - arg->pos;
            break;
        }
    }
    if (span_start < 0) return;

    CtscBuffer msg;
    ctsc_buf_init(&msg);
    ctsc_buf_append_cstr(&msg, "No overload matches this call.");
    size_t appended = 0;
    for (size_t i = 0; i < sig_count; ++i) {
        const CtscNode* fn = sigs[i];
        if (!fn || fn->kind != CTSC_SK_FunctionDeclaration) continue;
        CtscBuffer inner;
        ctsc_buf_init(&inner);
        bool has_err = append_overload_argument_error(&inner, w, fn, args);
        if (!has_err) { ctsc_buf_free(&inner); continue; }
        /* Child chain indent = 1 → "\n  ". */
        ctsc_buf_append_cstr(&msg, "\n  Overload ");
        char nbuf[32];
        int n = snprintf(nbuf, sizeof(nbuf), "%zu", i + 1);
        if (n > 0) ctsc_buf_append(&msg, nbuf, (size_t)n);
        ctsc_buf_append_cstr(&msg, " of ");
        n = snprintf(nbuf, sizeof(nbuf), "%zu", sig_count);
        if (n > 0) ctsc_buf_append(&msg, nbuf, (size_t)n);
        ctsc_buf_append_cstr(&msg, ", '");
        emit_call_signature_decl_string(&msg, &w->r->registry, fn, w->source_utf16, w->source_utf16_len);
        ctsc_buf_append_cstr(&msg, "', gave the following error.");
        /* Grandchild indent = 2 → "\n    ". */
        ctsc_buf_append_cstr(&msg, "\n    ");
        ctsc_buf_append(&msg, inner.data, inner.len);
        ctsc_buf_free(&inner);
        ++appended;
    }
    if (appended == 0) { ctsc_buf_free(&msg); return; }

    char* msg_arena = (char*)ctsc_arena_alloc(w->arena, msg.len + 1);
    memcpy(msg_arena, msg.data, msg.len);
    msg_arena[msg.len] = '\0';
    ctsc_buf_free(&msg);

    CtscCheckDiagnostic d = {0};
    d.code = 2769;
    d.category = "Error";
    d.start = span_start;
    d.length = span_length;
    d.message = msg_arena;
    diag_push(w->r, w->arena, d);
}

static void visit_call_expression(Walk* w, const CtscNode* n) {
    const CtscCallExpressionData* ce = &n->data.callExpression;
    const CtscNode* callee = ce->expression;
    const CtscNodeArray* args = &ce->arguments;

    visit(w, callee);

    const CtscNode* fn_decl = NULL;
    const CtscNode* overload_sigs[16];
    size_t overload_sig_count = 0;
    bool overload_no_match = false;
    if (callee && callee->kind == CTSC_SK_Identifier) {
        const uint16_t* nm = callee->data.identifier.text;
        size_t nl = callee->data.identifier.text_len;
        if (nm && nl > 0) {
            CtscSymbol* sym = resolve_name(&w->scopes, nm, nl);
            /* Prefer the resolved overload so TS2554 / TS2345 spans use the
             * signature that actually matched the call; if nothing matches
             * fall back to the first declaration. */
            fn_decl = symbol_resolve_overload_for_call(w, sym, ce);
            if (!fn_decl) {
                /* No overload matched. If there are ≥2 arity-compatible
                 * candidates, tsc reports a single TS2769 chain rather than
                 * a per-argument TS2345 against the fallback decl
                 * (checker.ts resolveCall ~36704-36748). */
                overload_sig_count = symbol_collect_overload_signature_decls(
                    sym, overload_sigs, sizeof(overload_sigs) / sizeof(overload_sigs[0]));
                if (overload_sig_count >= 2) {
                    size_t arity_ok = 0;
                    for (size_t i = 0; i < overload_sig_count; ++i) {
                        const CtscNode* fn = overload_sigs[i];
                        int min = function_declaration_min_arguments(fn);
                        int max = function_declaration_max_arguments(fn);
                        if ((int)args->len < min) continue;
                        if (max != INT_MAX && (int)args->len > max) continue;
                        ++arity_ok;
                    }
                    if (arity_ok >= 2) overload_no_match = true;
                }
                fn_decl = symbol_function_declaration_decl(sym);
            }
        }
    }

    if (overload_no_match) {
        emit_ts2769_no_overload_match(w, n, overload_sigs, overload_sig_count, args);
        for (size_t i = 0; i < args->len; ++i) {
            visit(w, args->items[i]);
        }
        return;
    }

    bool skip_param_assignability = false;
    if (fn_decl) {
        int min_args = function_declaration_min_arguments(fn_decl);
        int max_args = function_declaration_max_arguments(fn_decl);
        if ((int)args->len < min_args) {
            int es = 0, el = 0;
            if (call_arity_error_span(callee, &es, &el)) {
                emit_ts2554_arity(w->r, w->arena, es, el, min_args, (int)args->len);
            }
            skip_param_assignability = true;
        } else if (max_args != INT_MAX && (int)args->len > max_args) {
            /* Too many: span excess arguments (checker.ts ~36479-36493). */
            const CtscNode* first_excess = args->items[(size_t)max_args];
            const CtscNode* last_excess = args->items[args->len - 1];
            if (first_excess && last_excess && last_excess->end >= first_excess->pos) {
                int es = first_excess->pos;
                int el = last_excess->end - first_excess->pos;
                emit_ts2554_arity(w->r, w->arena, es, el, max_args, (int)args->len);
            }
        }
    }

    for (size_t i = 0; i < args->len; ++i) {
        const CtscNode* arg = args->items[i];
        if (!skip_param_assignability && fn_decl && arg) {
            const CtscNodeArray* params = &fn_decl->data.functionDeclaration.parameters;
            if (i < params->len) {
                const CtscNode* param = params->items[i];
                if (param && param->kind == CTSC_SK_Parameter) {
                    if (!parameter_type_is_declared_type_parameter(fn_decl, param)) {
                        CtscType* param_t = param_type(w, param);
                        CtscType* arg_t = type_of_expression(w, &w->r->registry, arg);
                        if (!is_assignable_to(w, &w->r->registry, arg_t, param_t)) {
                            emit_ts2345_on_argument(w->r, w->arena, &w->r->registry, arg, arg_t, param_t);
                        }
                    }
                }
            }
        }
        visit(w, arg);
    }
}

/*
 * Class `constructor(...)` is parsed as MethodDeclaration with the synthetic
 * name "constructor" (parser.ts parseClassElement ~8088). The types-channel
 * oracle (oracle-checker-types) does not emit a MethodDeclaration entry for
 * it—only constructor parameters—matching tsc's walk for this fixture
 * (checker/generics/06_generic_class.ts).
 */
static bool identifier_is_class_constructor_name(const CtscNode* name_id) {
    if (!name_id || name_id->kind != CTSC_SK_Identifier) return false;
    const uint16_t* t = name_id->data.identifier.text;
    size_t len = name_id->data.identifier.text_len;
    static const uint16_t ctor_utf16[] = {
        (uint16_t)'c', (uint16_t)'o', (uint16_t)'n', (uint16_t)'s', (uint16_t)'t',
        (uint16_t)'r', (uint16_t)'u', (uint16_t)'c', (uint16_t)'t', (uint16_t)'o', (uint16_t)'r'};
    if (len != sizeof(ctor_utf16) / sizeof(ctor_utf16[0])) return false;
    return memcmp(t, ctor_utf16, len * sizeof(uint16_t)) == 0;
}

static void check_property_access_expression(Walk* w, const CtscNode* n) {
    if (!w || !n || n->kind != CTSC_SK_PropertyAccessExpression) return;
    const CtscPropertyAccessExpressionData* pa = &n->data.propertyAccessExpression;
    const CtscNode* name = pa->name;
    if (!name || name->kind != CTSC_SK_Identifier) return;
    const CtscIdentifierData* id = &name->data.identifier;

    CtscType* obj_t = type_of_expression(w, &w->r->registry, pa->expression);
    if (!obj_t || obj_t->kind == CTSC_TYPE_ANY) return;
    if (obj_t->kind != CTSC_TYPE_OBJECT_LITERAL) return;

    for (size_t i = 0; i < obj_t->object_properties_len; ++i) {
        const CtscObjectProperty* p = &obj_t->object_properties[i];
        if (p->name_len == id->text_len && p->name && id->text
            && memcmp(p->name, id->text, id->text_len * sizeof(uint16_t)) == 0) {
            return;
        }
    }
    emit_ts2339_missing_property(w->r, w->arena, &w->r->registry, name, id->text, id->text_len, obj_t);
}

static void visit_class_declaration(Walk* w, const CtscNode* n) {
    const CtscClassDeclarationData* c = &n->data.classDeclaration;
    for (size_t i = 0; i < c->members.len; ++i) {
        CtscNode* m = c->members.items[i];
        if (!m) continue;
        if (m->kind == CTSC_SK_PropertyDeclaration) {
            const CtscNode* name_node = m->data.propertyDeclaration.name;
            if (!name_node || name_node->kind != CTSC_SK_Identifier) continue;
            CtscType* t = class_property_declaration_type(w, m);
            push_entry_for_name(w, m, name_node, t);
        } else if (m->kind == CTSC_SK_MethodDeclaration) {
            /*
             * checker.ts getTypeOfSymbol / typeToString on a method signature
             * (~12537+): oracle records MethodDeclaration name nodes with the
             * `() => Return` form (same as FunctionDeclaration entries).
             */
            const CtscMethodDeclarationData* md = &m->data.methodDeclaration;
            const CtscNode* name_node = md->name;
            if (!name_node || name_node->kind != CTSC_SK_Identifier) continue;
            if (!identifier_is_class_constructor_name(name_node)) {
                CtscType* inferred_ret =
                    infer_return_type_for_function_body(w, &w->r->registry, w->arena, md->body, m);
                CtscBuffer sig;
                ctsc_buf_init(&sig);
                emit_function_signature_string(&sig, &md->type_parameters, &md->parameters, &w->r->registry, md->type,
                                               inferred_ret, w->source_utf16, w->source_utf16_len);
                char* s = (char*)ctsc_arena_alloc(w->arena, sig.len);
                memcpy(s, sig.data, sig.len);
                size_t slen = sig.len;
                ctsc_buf_free(&sig);
                push_entry_with_string(w, m, name_node, s, slen);
            }
            /* Parameters: same types channel as FunctionDeclaration (checker.ts
             * getTypeOfVariableOrParameterOrPropertyWorker ~12554). */
            for (size_t pi = 0; pi < md->parameters.len; ++pi) {
                const CtscNode* p = md->parameters.items[pi];
                if (!p || p->kind != CTSC_SK_Parameter) continue;
                push_entry_for_name(w, p, p->data.parameter.name, param_type(w, p));
            }
        } else if (m->kind == CTSC_SK_SetAccessor) {
            /*
             * Setter value parameter: same types channel as method parameters
             * (checker.ts getTypeOfVariableOrParameterOrPropertyWorker ~12643
             * for SyntaxKind.Parameter).
             */
            const CtscAccessorDeclarationData* ad = &m->data.accessorDeclaration;
            for (size_t pi = 0; pi < ad->parameters.len; ++pi) {
                const CtscNode* p = ad->parameters.items[pi];
                if (!p || p->kind != CTSC_SK_Parameter) continue;
                push_entry_for_name(w, p, p->data.parameter.name, param_type(w, p));
            }
        }
    }
    for (size_t i = 0; i < c->members.len; ++i) {
        CtscNode* m = c->members.items[i];
        if (m && m->kind == CTSC_SK_PropertyDeclaration && m->data.propertyDeclaration.initializer) {
            visit(w, m->data.propertyDeclaration.initializer);
        } else if (m && m->kind == CTSC_SK_MethodDeclaration) {
            const CtscMethodDeclarationData* md = &m->data.methodDeclaration;
            open_scope_if_container(w, m);
            for (size_t pi = 0; pi < md->parameters.len; ++pi) {
                const CtscNode* p = md->parameters.items[pi];
                if (!p || p->kind != CTSC_SK_Parameter) continue;
                if (p->data.parameter.initializer) visit(w, p->data.parameter.initializer);
            }
            if (md->body) visit(w, md->body);
            close_scope_if_container(w, m);
        }
    }
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

        case CTSC_SK_FunctionExpression: {
            const CtscFunctionDeclarationData* f = &n->data.functionDeclaration;
            for (size_t i = 0; i < f->parameters.len; ++i) {
                const CtscNode* p = f->parameters.items[i];
                if (!p || p->kind != CTSC_SK_Parameter) continue;
                push_entry_for_name(w, p, p->data.parameter.name, param_type(w, p));
            }
            open_scope_if_container(w, n);
            for (size_t i = 0; i < f->parameters.len; ++i) {
                const CtscNode* p = f->parameters.items[i];
                if (!p || p->kind != CTSC_SK_Parameter) continue;
                if (p->data.parameter.initializer) visit(w, p->data.parameter.initializer);
            }
            if (f->body) visit(w, f->body);
            close_scope_if_container(w, n);
            return;
        }

        case CTSC_SK_ArrowFunction: {
            const CtscArrowFunctionData* af = &n->data.arrowFunction;
            for (size_t i = 0; i < af->parameters.len; ++i) {
                const CtscNode* p = af->parameters.items[i];
                if (!p || p->kind != CTSC_SK_Parameter) continue;
                push_entry_for_name(w, p, p->data.parameter.name, param_type(w, p));
            }
            open_scope_if_container(w, n);
            for (size_t i = 0; i < af->parameters.len; ++i) {
                const CtscNode* p = af->parameters.items[i];
                if (!p || p->kind != CTSC_SK_Parameter) continue;
                if (p->data.parameter.initializer) visit(w, p->data.parameter.initializer);
            }
            if (af->body) visit(w, af->body);
            close_scope_if_container(w, n);
            return;
        }

        case CTSC_SK_InterfaceDeclaration:
            push_property_signatures_from_interface_declaration(w, n);
            return;

        case CTSC_SK_ModuleDeclaration: {
            /*
             * Mirrors the oracle walk in harness/src/oracle-checker-types.ts:
             * ts.forEachChild descends into every ModuleDeclaration body, so
             * `declare namespace N { const x: number; }` emits an entry for
             * `x` with type `number` from checker.ts
             * getTypeOfVariableOrParameterOrPropertyWorker (~12537).
             *
             * Body is either a ModuleBlock (single-segment namespace) or a
             * nested ModuleDeclaration (dotted-name: `namespace a.b {}`).
             * We recurse through `visit` so nested modules also contribute
             * entries.
             */
            const CtscNode* body = n->data.moduleDeclaration.body;
            if (body) visit(w, body);
            return;
        }

        case CTSC_SK_ModuleBlock:
            walk_children_nodearray(w, &n->data.moduleBlock.statements);
            return;

        case CTSC_SK_TypeAliasDeclaration: {
            const CtscNode* alias_ty = n->data.typeAliasDeclaration.type;
            if (alias_ty && alias_ty->kind == CTSC_SK_TypeLiteral) {
                push_property_signatures_from_type_literal(w, alias_ty);
            }
            return;
        }

        case CTSC_SK_ClassDeclaration:
            visit_class_declaration(w, n);
            return;

        case CTSC_SK_ExpressionStatement:
            visit(w, n->data.expressionStatement.expression);
            return;

        case CTSC_SK_ReturnStatement:
            if (n->data.returnStatement.expression) visit(w, n->data.returnStatement.expression);
            return;

        case CTSC_SK_IfStatement: {
            const CtscIfStatementData* ifs = &n->data.ifStatement;
            visit(w, ifs->expression);
            CtscSymbol* lit_sym = NULL;
            const uint16_t* lit_txt = NULL;
            size_t lit_len = 0;
            if (if_condition_ident_eq_string_literal_compare(w, ifs->expression, &lit_sym, &lit_txt, &lit_len)) {
                CtscType* declared_lit = type_of_symbol_value(w, lit_sym);
                CtscType* then_lit = ctsc_type_string_literal(&w->r->registry, lit_txt, lit_len);
                w->narrow_eq_literal_sym = lit_sym;
                w->narrow_eq_literal_type = then_lit;
                visit(w, ifs->thenStatement);
                w->narrow_eq_literal_sym = NULL;
                w->narrow_eq_literal_type = NULL;
                w->narrow_eq_literal_sym = lit_sym;
                w->narrow_eq_literal_type =
                    narrow_string_literal_eq_else_type(&w->r->registry, declared_lit, lit_txt, lit_len);
                visit(w, ifs->elseStatement);
                w->narrow_eq_literal_sym = NULL;
                w->narrow_eq_literal_type = NULL;
            } else {
                CtscSymbol* narrow_sym = NULL;
                CtscTypeofResultCompareKind cmp =
                    if_condition_typeof_ident_eq_primitive_string_compare(w, ifs->expression, &narrow_sym);
                if (narrow_sym && cmp != CTSC_TYPEOF_CMP_NONE) {
                    CtscType* declared = type_of_symbol_value(w, narrow_sym);
                    CtscType* then_ty = NULL;
                    CtscType* (*else_fn)(CtscTypeRegistry*, CtscType*) = NULL;
                    if (cmp == CTSC_TYPEOF_CMP_STRING) {
                        then_ty = w->r->registry.t_string;
                        else_fn = narrow_typeof_string_compare_else_type;
                    } else if (cmp == CTSC_TYPEOF_CMP_NUMBER) {
                        then_ty = w->r->registry.t_number;
                        else_fn = narrow_typeof_number_compare_else_type;
                    } else {
                        then_ty = w->r->registry.t_boolean;
                        else_fn = narrow_typeof_boolean_compare_else_type;
                    }
                    w->narrow_typeof_eq_string_sym = narrow_sym;
                    w->narrow_typeof_eq_string_type = then_ty;
                    visit(w, ifs->thenStatement);
                    w->narrow_typeof_eq_string_sym = NULL;
                    w->narrow_typeof_eq_string_type = NULL;
                    w->narrow_typeof_eq_string_sym = narrow_sym;
                    w->narrow_typeof_eq_string_type = else_fn(&w->r->registry, declared);
                    visit(w, ifs->elseStatement);
                    w->narrow_typeof_eq_string_sym = NULL;
                    w->narrow_typeof_eq_string_type = NULL;
                } else {
                    visit(w, ifs->thenStatement);
                    visit(w, ifs->elseStatement);
                }
            }
            return;
        }

        case CTSC_SK_WhileStatement:
            visit(w, n->data.whileStatement.expression);
            visit(w, n->data.whileStatement.statement);
            return;

        case CTSC_SK_ForStatement: {
            const CtscForStatementData* fs = &n->data.forStatement;
            open_scope_if_container(w, n);
            if (fs->initializer) {
                if (fs->initializer->kind == CTSC_SK_VariableDeclarationList) {
                    visit_variable_declaration_list(w, fs->initializer);
                } else {
                    visit(w, fs->initializer);
                }
            }
            if (fs->condition) visit(w, fs->condition);
            if (fs->statement) visit(w, fs->statement);
            if (fs->incrementor) visit(w, fs->incrementor);
            close_scope_if_container(w, n);
            return;
        }

        /* Identifier references: TS2304 detection. */
        case CTSC_SK_Identifier:
            check_identifier_reference(w, n);
            return;

        case CTSC_SK_BinaryExpression:
            visit(w, n->data.binaryExpression.left);
            visit(w, n->data.binaryExpression.right);
            return;

        case CTSC_SK_CallExpression:
            visit_call_expression(w, n);
            return;

        case CTSC_SK_NewExpression: {
            const CtscNewExpressionData* ne = &n->data.newExpression;
            visit(w, ne->expression);
            if (ne->has_arguments) {
                walk_children_nodearray(w, &ne->arguments);
            }
            return;
        }

        case CTSC_SK_PropertyAccessExpression:
            /* Only the LHS is a reference; the `.name` is a property, not a
             * symbol lookup. */
            visit(w, n->data.propertyAccessExpression.expression);
            check_property_access_expression(w, n);
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

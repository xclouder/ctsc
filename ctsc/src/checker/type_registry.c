#include "ctsc/type.h"
#include "ctsc/arena.h"
#include "ctsc/buffer.h"
#include "ctsc/utf8.h"

#include <string.h>
#include <stdio.h>

/*
 * Arena-allocated type registry with primitive singletons (M4.0).
 *
 * Everything lives inside the caller-provided arena. Literal types are not
 * interned (two `42` literal types produce two distinct CtscType objects);
 * typeToString yields identical strings so this has zero observable effect
 * on the oracle contract. Interning becomes worthwhile once unions/
 * generics produce many shared structural nodes in M4.1+.
 */

static CtscType* primitive(CtscTypeRegistry* reg, CtscTypeKind k) {
    CtscType* t = (CtscType*)ctsc_arena_calloc(reg->arena, 1, sizeof(CtscType));
    t->kind = k;
    return t;
}

void ctsc_type_registry_init(CtscTypeRegistry* reg, CtscArena* arena) {
    memset(reg, 0, sizeof(*reg));
    reg->arena = arena;
    reg->t_any       = primitive(reg, CTSC_TYPE_ANY);
    reg->t_unknown   = primitive(reg, CTSC_TYPE_UNKNOWN_T);
    reg->t_void      = primitive(reg, CTSC_TYPE_VOID);
    reg->t_undefined = primitive(reg, CTSC_TYPE_UNDEFINED);
    reg->t_null      = primitive(reg, CTSC_TYPE_NULL);
    reg->t_never     = primitive(reg, CTSC_TYPE_NEVER);
    reg->t_number    = primitive(reg, CTSC_TYPE_NUMBER);
    reg->t_bigint    = primitive(reg, CTSC_TYPE_BIGINT);
    reg->t_string    = primitive(reg, CTSC_TYPE_STRING);
    reg->t_boolean   = primitive(reg, CTSC_TYPE_BOOLEAN);
    reg->t_symbol    = primitive(reg, CTSC_TYPE_SYMBOL);
    reg->t_object    = primitive(reg, CTSC_TYPE_OBJECT);
    reg->t_empty_object = primitive(reg, CTSC_TYPE_EMPTY_OBJECT);

    reg->t_true = (CtscType*)ctsc_arena_calloc(reg->arena, 1, sizeof(CtscType));
    reg->t_true->kind = CTSC_TYPE_BOOLEAN_LITERAL;
    reg->t_true->boolean_value = true;

    reg->t_false = (CtscType*)ctsc_arena_calloc(reg->arena, 1, sizeof(CtscType));
    reg->t_false->kind = CTSC_TYPE_BOOLEAN_LITERAL;
    reg->t_false->boolean_value = false;
}

CtscType* ctsc_type_new(CtscTypeRegistry* reg, CtscTypeKind kind) {
    CtscType* t = (CtscType*)ctsc_arena_calloc(reg->arena, 1, sizeof(CtscType));
    t->kind = kind;
    return t;
}

CtscType* ctsc_type_number_literal(CtscTypeRegistry* reg, double v) {
    CtscType* t = ctsc_type_new(reg, CTSC_TYPE_NUMBER_LITERAL);
    t->number_value = v;
    return t;
}

CtscType* ctsc_type_string_literal(CtscTypeRegistry* reg, const uint16_t* text, size_t len) {
    CtscType* t = ctsc_type_new(reg, CTSC_TYPE_STRING_LITERAL);
    t->text = text;
    t->text_len = len;
    return t;
}

CtscType* ctsc_type_bigint_literal(CtscTypeRegistry* reg, const uint16_t* text, size_t len) {
    CtscType* t = ctsc_type_new(reg, CTSC_TYPE_BIGINT_LITERAL);
    t->text = text;
    t->text_len = len;
    return t;
}

CtscType* ctsc_type_object_literal(CtscTypeRegistry* reg, CtscObjectProperty* props, size_t prop_count) {
    CtscType* t = ctsc_type_new(reg, CTSC_TYPE_OBJECT_LITERAL);
    t->object_properties = props;
    t->object_properties_len = prop_count;
    t->object_string_index_value_type = NULL;
    return t;
}

CtscType* ctsc_type_reference(CtscTypeRegistry* reg, const uint16_t* name, size_t name_len) {
    CtscType* t = ctsc_type_new(reg, CTSC_TYPE_REFERENCE);
    t->text = name;
    t->text_len = name_len;
    return t;
}

CtscType* ctsc_type_class_constructor(CtscTypeRegistry* reg, const uint16_t* name, size_t name_len) {
    CtscType* t = ctsc_type_new(reg, CTSC_TYPE_CLASS_CONSTRUCTOR);
    t->text = name;
    t->text_len = name_len;
    return t;
}

CtscType* ctsc_type_enum_value(CtscTypeRegistry* reg, const uint16_t* name, size_t name_len) {
    CtscType* t = ctsc_type_new(reg, CTSC_TYPE_ENUM_VALUE);
    t->text = name;
    t->text_len = name_len;
    return t;
}

CtscType* ctsc_type_enum_member_literal(CtscTypeRegistry* reg, const uint16_t* enum_name, size_t enum_name_len,
                                        const uint16_t* member_name, size_t member_name_len) {
    CtscType* t = ctsc_type_new(reg, CTSC_TYPE_ENUM_MEMBER_LITERAL);
    t->enum_parent_name = enum_name;
    t->enum_parent_name_len = enum_name_len;
    t->enum_member_name = member_name;
    t->enum_member_name_len = member_name_len;
    return t;
}

/*
 * Anonymous function type (checker.ts getTypeOfFuncClassEnumModule /
 * createAnonymousType for a function symbol; typeToString emits the single
 * call signature as `(params) => ret`). The signature string is
 * pre-formatted by the caller via emit_function_signature_string because
 * the M4.0 type model does not yet carry structural signature data.
 */
CtscType* ctsc_type_function(CtscTypeRegistry* reg, const char* signature_text, size_t signature_text_len,
                             const void* decl) {
    CtscType* t = ctsc_type_new(reg, CTSC_TYPE_FUNCTION);
    if (signature_text && signature_text_len > 0) {
        char* slot = (char*)ctsc_arena_alloc(reg->arena, signature_text_len);
        memcpy(slot, signature_text, signature_text_len);
        t->function_signature_text = slot;
        t->function_signature_text_len = signature_text_len;
    }
    t->function_decl = decl;
    return t;
}

CtscType* ctsc_type_reference_with_type_args(CtscTypeRegistry* reg, const uint16_t* name, size_t name_len,
                                               CtscType** args, size_t arg_count) {
    CtscType* t = ctsc_type_new(reg, CTSC_TYPE_REFERENCE);
    t->text = name;
    t->text_len = name_len;
    if (arg_count > 0 && args) {
        CtscType** slot = (CtscType**)ctsc_arena_alloc(reg->arena, arg_count * sizeof(CtscType*));
        memcpy(slot, args, arg_count * sizeof(CtscType*));
        t->reference_type_args = slot;
        t->reference_type_args_len = arg_count;
    }
    return t;
}

CtscType* ctsc_type_tuple(CtscTypeRegistry* reg, CtscType** elements, size_t element_count) {
    return ctsc_type_tuple_with_element_flags(reg, elements, NULL, element_count);
}

CtscType* ctsc_type_readonly_tuple_with_element_flags(CtscTypeRegistry* reg, CtscType** elements,
                                                      const uint8_t* element_flags, size_t element_count) {
    CtscType* t = ctsc_type_tuple_with_element_flags(reg, elements, element_flags, element_count);
    if (t) t->tuple_readonly = true;
    return t;
}

CtscType* ctsc_type_tuple_with_element_flags(CtscTypeRegistry* reg, CtscType** elements, const uint8_t* element_flags,
                                             size_t element_count) {
    return ctsc_type_tuple_with_element_flags_and_labels(reg, elements, element_flags, NULL, NULL, element_count,
                                                         false);
}

CtscType* ctsc_type_tuple_with_element_flags_and_labels(CtscTypeRegistry* reg, CtscType** elements,
                                                        const uint8_t* element_flags,
                                                        const uint16_t* const* labels, const size_t* label_lens,
                                                        size_t element_count, bool readonly_tuple) {
    CtscType* t = ctsc_type_new(reg, CTSC_TYPE_TUPLE);
    if (element_count > 0 && elements) {
        CtscType** slot = (CtscType**)ctsc_arena_alloc(reg->arena, element_count * sizeof(CtscType*));
        memcpy(slot, elements, element_count * sizeof(CtscType*));
        t->tuple_elements = slot;
        t->tuple_elements_len = element_count;
        if (element_flags) {
            /*
             * Only allocate the parallel flag array when at least one slot is
             * non-Required; tsc similarly elides elementFlags when all bits
             * are `Required` to keep TupleType payloads small (types.ts
             * TupleType ~6702; createTupleType ~17955 short-circuits for the
             * all-Required case). NULL is the canonical "all Required" shape.
             */
            bool any_non_required = false;
            for (size_t i = 0; i < element_count; ++i) {
                uint8_t f = element_flags[i];
                if (f && f != CTSC_TUPLE_ELEMENT_REQUIRED) { any_non_required = true; break; }
            }
            if (any_non_required) {
                uint8_t* fslot = (uint8_t*)ctsc_arena_alloc(reg->arena, element_count * sizeof(uint8_t));
                memcpy(fslot, element_flags, element_count * sizeof(uint8_t));
                t->tuple_element_flags = fslot;
            } else {
                t->tuple_element_flags = NULL;
            }
        } else {
            t->tuple_element_flags = NULL;
        }
        /*
         * Labels are elided when every slot is unlabeled, matching tsc's
         * optional `labeledElementDeclarations` on TupleType (types.ts
         * TupleType; set only when a tuple had NamedTupleMember nodes in
         * parser.ts ~4464-4477).
         */
        if (labels && label_lens) {
            bool any_labeled = false;
            for (size_t i = 0; i < element_count; ++i) {
                if (labels[i] && label_lens[i] > 0) { any_labeled = true; break; }
            }
            if (any_labeled) {
                const uint16_t** lslot =
                    (const uint16_t**)ctsc_arena_alloc(reg->arena, element_count * sizeof(const uint16_t*));
                size_t* nslot = (size_t*)ctsc_arena_alloc(reg->arena, element_count * sizeof(size_t));
                for (size_t i = 0; i < element_count; ++i) {
                    lslot[i] = labels[i];
                    nslot[i] = label_lens[i];
                }
                t->tuple_element_labels = lslot;
                t->tuple_element_label_lens = nslot;
            } else {
                t->tuple_element_labels = NULL;
                t->tuple_element_label_lens = NULL;
            }
        } else {
            t->tuple_element_labels = NULL;
            t->tuple_element_label_lens = NULL;
        }
    } else {
        t->tuple_elements = NULL;
        t->tuple_elements_len = 0;
        t->tuple_element_flags = NULL;
        t->tuple_element_labels = NULL;
        t->tuple_element_label_lens = NULL;
    }
    t->tuple_readonly = readonly_tuple;
    return t;
}

CtscType* ctsc_type_intersection(CtscTypeRegistry* reg, CtscType** members, size_t member_count) {
    if (!reg || member_count == 0 || !members) return reg ? reg->t_any : NULL;
    if (member_count == 1) return members[0];
    CtscType* t = ctsc_type_new(reg, CTSC_TYPE_INTERSECTION);
    CtscType** slot = (CtscType**)ctsc_arena_alloc(reg->arena, member_count * sizeof(CtscType*));
    memcpy(slot, members, member_count * sizeof(CtscType*));
    t->intersection_members = slot;
    t->intersection_members_len = member_count;
    return t;
}

CtscType* ctsc_type_index(CtscTypeRegistry* reg, CtscType* target) {
    if (!reg) return NULL;
    CtscType* t = ctsc_type_new(reg, CTSC_TYPE_INDEX);
    t->index_target = target ? target : reg->t_any;
    return t;
}

/*
 * Literal → base widening (types.ts getWidenedLiteralType ~35395):
 *   42 → number,  "hi" → string,  true/false → boolean,  42n → bigint.
 * Everything else returns unchanged.
 */
CtscType* ctsc_type_widen(CtscTypeRegistry* reg, const CtscType* t) {
    if (!t) return reg->t_any;
    switch (t->kind) {
        case CTSC_TYPE_NUMBER_LITERAL:  return reg->t_number;
        case CTSC_TYPE_STRING_LITERAL:  return reg->t_string;
        case CTSC_TYPE_BOOLEAN_LITERAL: return reg->t_boolean;
        case CTSC_TYPE_BIGINT_LITERAL:  return reg->t_bigint;
        case CTSC_TYPE_REFERENCE:
        case CTSC_TYPE_CLASS_CONSTRUCTOR:
        case CTSC_TYPE_ENUM_VALUE:
            return (CtscType*)t;
        case CTSC_TYPE_ENUM_MEMBER_LITERAL:
            if (t->enum_parent_name && t->enum_parent_name_len > 0) {
                return ctsc_type_reference(reg, t->enum_parent_name, t->enum_parent_name_len);
            }
            return reg->t_any;
        default: return (CtscType*)t;
    }
}

/* ---- typeToString ---- */

static void emit_number_literal(CtscBuffer* out, double v) {
    /*
     * tsc's pseudoBigIntToString / numberToString for NumberLiteralType uses
     * the same rules as ECMAScript Number.prototype.toString(10). For integer
     * values up to 2^53 this matches "%lld"; fractional values would need
     * more care (M4.1+). The initial curriculum only uses integer literals.
     */
    if ((double)(long long)v == v) {
        char buf[32];
        int n = snprintf(buf, sizeof(buf), "%lld", (long long)v);
        if (n > 0) ctsc_buf_append(out, buf, (size_t)n);
    } else {
        char buf[64];
        int n = snprintf(buf, sizeof(buf), "%g", v);
        if (n > 0) ctsc_buf_append(out, buf, (size_t)n);
    }
}

static void emit_utf16_escaped_for_string_literal(CtscBuffer* out, const uint16_t* text, size_t len) {
    /*
     * tsc's typeToString on a StringLiteralType emits `"<escaped>"` using the
     * default printer rules: wrap in double quotes, escape `"`, `\`, and
     * control chars. The curriculum only uses plain ASCII strings so we keep
     * the escaper minimal for now and grow it when a fixture needs more.
     */
    ctsc_buf_append_char(out, '"');
    for (size_t i = 0; i < len; ++i) {
        uint16_t u = text[i];
        if (u == '"')       { ctsc_buf_append(out, "\\\"", 2); continue; }
        if (u == '\\')      { ctsc_buf_append(out, "\\\\", 2); continue; }
        if (u == '\n')      { ctsc_buf_append(out, "\\n",  2); continue; }
        if (u == '\r')      { ctsc_buf_append(out, "\\r",  2); continue; }
        if (u == '\t')      { ctsc_buf_append(out, "\\t",  2); continue; }
        if (u < 0x20)       {
            char buf[8];
            int n = snprintf(buf, sizeof(buf), "\\u%04x", (unsigned)u);
            if (n > 0) ctsc_buf_append(out, buf, (size_t)n);
            continue;
        }
        if (u < 0x80) { ctsc_buf_append_char(out, (char)u); continue; }
        /*
         * Re-encode UTF-16 code units back to UTF-8 for the JSON emitter's
         * own escaping pass. For BMP code points we can just emit the UTF-8
         * 2/3-byte sequence directly.
         */
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
}

static void append_utf16_ascii_identifier_prop_name(CtscBuffer* out, const uint16_t* t, size_t n) {
    if (!t) return;
    for (size_t i = 0; i < n; ++i) {
        uint16_t u = t[i];
        if (u < 0x80) ctsc_buf_append_char(out, (char)u);
    }
}

void ctsc_type_to_string(const CtscType* t, CtscBuffer* out) {
    if (!t) { ctsc_buf_append_cstr(out, "any"); return; }
    switch (t->kind) {
        case CTSC_TYPE_ANY:         ctsc_buf_append_cstr(out, "any");       return;
        case CTSC_TYPE_UNKNOWN_T:   ctsc_buf_append_cstr(out, "unknown");   return;
        case CTSC_TYPE_VOID:        ctsc_buf_append_cstr(out, "void");      return;
        case CTSC_TYPE_UNDEFINED:   ctsc_buf_append_cstr(out, "undefined"); return;
        case CTSC_TYPE_NULL:        ctsc_buf_append_cstr(out, "null");      return;
        case CTSC_TYPE_NEVER:       ctsc_buf_append_cstr(out, "never");     return;
        case CTSC_TYPE_NUMBER:      ctsc_buf_append_cstr(out, "number");    return;
        case CTSC_TYPE_BIGINT:      ctsc_buf_append_cstr(out, "bigint");    return;
        case CTSC_TYPE_STRING:      ctsc_buf_append_cstr(out, "string");    return;
        case CTSC_TYPE_BOOLEAN:     ctsc_buf_append_cstr(out, "boolean");   return;
        case CTSC_TYPE_SYMBOL:      ctsc_buf_append_cstr(out, "symbol");    return;
        case CTSC_TYPE_OBJECT:      ctsc_buf_append_cstr(out, "object");    return;
        case CTSC_TYPE_EMPTY_OBJECT: ctsc_buf_append_cstr(out, "{}");       return;
        case CTSC_TYPE_NUMBER_LITERAL:  emit_number_literal(out, t->number_value); return;
        case CTSC_TYPE_STRING_LITERAL:  emit_utf16_escaped_for_string_literal(out, t->text, t->text_len); return;
        case CTSC_TYPE_BOOLEAN_LITERAL: ctsc_buf_append_cstr(out, t->boolean_value ? "true" : "false"); return;
        case CTSC_TYPE_BIGINT_LITERAL: {
            /* typeToString on BigIntLiteralType prints `${negative ? "-" : ""}${base10Value}n`.
             * We stored the source lexeme verbatim; just append it + "n".
             * BigInt literal text is guaranteed to be 7-bit ASCII. */
            for (size_t i = 0; i < t->text_len; ++i) {
                ctsc_buf_append_char(out, (char)t->text[i]);
            }
            ctsc_buf_append_char(out, 'n');
            return;
        }
        case CTSC_TYPE_UNION: {
            if (t->alias_symbol_name && t->alias_symbol_name_len > 0) {
                append_utf16_ascii_identifier_prop_name(out, t->alias_symbol_name, t->alias_symbol_name_len);
                return;
            }
            for (size_t i = 0; i < t->union_members_len; ++i) {
                if (i > 0) ctsc_buf_append_cstr(out, " | ");
                ctsc_type_to_string(t->union_members[i], out);
            }
            return;
        }
        case CTSC_TYPE_INTERSECTION: {
            for (size_t i = 0; i < t->intersection_members_len; ++i) {
                if (i > 0) ctsc_buf_append_cstr(out, " & ");
                ctsc_type_to_string(t->intersection_members[i], out);
            }
            return;
        }
        case CTSC_TYPE_OBJECT_LITERAL: {
            if (t->alias_symbol_name && t->alias_symbol_name_len > 0) {
                append_utf16_ascii_identifier_prop_name(out, t->alias_symbol_name, t->alias_symbol_name_len);
                return;
            }
            ctsc_buf_append_cstr(out, "{ ");
            for (size_t i = 0; i < t->object_properties_len; ++i) {
                if (i > 0) ctsc_buf_append_cstr(out, "; ");
                CtscObjectProperty* p = &t->object_properties[i];
                append_utf16_ascii_identifier_prop_name(out, p->name, p->name_len);
                if (p->optional) ctsc_buf_append_cstr(out, "?");
                ctsc_buf_append_cstr(out, ": ");
                ctsc_type_to_string(p->value_type, out);
            }
            ctsc_buf_append_cstr(out, "; }");
            return;
        }
        case CTSC_TYPE_TUPLE: {
            /*
             * Mirrors checker.ts typeToTypeNodeHelper for a tuple type
             * reference (~7432-7454). Per-element rendering is driven by
             * TupleType.elementFlags:
             *   Required → `T`
             *   Optional → `T?` (createOptionalTypeNode)
             *   Rest     → `...T[]` (createRestTypeNode(createArrayTypeNode(T)))
             *   Variadic → `...T`   (createRestTypeNode(T))
             *
             * Labeled elements (NamedTupleMember; checker.ts ~7442-7449) use
             * a different wrapping: the flags surface inside the member as
             * `[...]name[?]: T[? for rest]`, where the `[]` for Rest is
             * pulled inside the type (createArrayTypeNode), the `?` attaches
             * to the name (questionToken), and `...` prefixes the name:
             *   Required  → `name: T`
             *   Optional  → `name?: T`
             *   Rest      → `...name: T[]`
             *   Variadic  → `...name: T`
             * (emitter.ts emitNamedTupleMember ~2431-2438.)
             *
             * A readonly target wraps the TupleTypeNode in
             * TypeOperatorNode(ReadonlyKeyword) which the printer emits as
             * a `readonly ` prefix (checker.ts ~7458; types.ts TupleType.readonly).
             */
            if (t->tuple_readonly) ctsc_buf_append_cstr(out, "readonly ");
            ctsc_buf_append_char(out, '[');
            for (size_t i = 0; i < t->tuple_elements_len; ++i) {
                if (i > 0) ctsc_buf_append_cstr(out, ", ");
                uint8_t f = t->tuple_element_flags ? t->tuple_element_flags[i] : CTSC_TUPLE_ELEMENT_REQUIRED;
                bool is_rest     = (f & CTSC_TUPLE_ELEMENT_REST)     != 0;
                bool is_variadic = (f & CTSC_TUPLE_ELEMENT_VARIADIC) != 0;
                bool is_optional = (f & CTSC_TUPLE_ELEMENT_OPTIONAL) != 0;
                const uint16_t* label = t->tuple_element_labels ? t->tuple_element_labels[i] : NULL;
                size_t label_len = t->tuple_element_label_lens ? t->tuple_element_label_lens[i] : 0;
                if (label && label_len > 0) {
                    if (is_rest || is_variadic) ctsc_buf_append_cstr(out, "...");
                    append_utf16_ascii_identifier_prop_name(out, label, label_len);
                    if (is_optional) ctsc_buf_append_char(out, '?');
                    ctsc_buf_append_cstr(out, ": ");
                    ctsc_type_to_string(t->tuple_elements[i], out);
                    if (is_rest) ctsc_buf_append_cstr(out, "[]");
                } else {
                    if (is_rest || is_variadic) ctsc_buf_append_cstr(out, "...");
                    ctsc_type_to_string(t->tuple_elements[i], out);
                    if (is_rest)     ctsc_buf_append_cstr(out, "[]");
                    if (is_optional) ctsc_buf_append_char(out, '?');
                }
            }
            ctsc_buf_append_char(out, ']');
            return;
        }
        case CTSC_TYPE_REFERENCE:
            append_utf16_ascii_identifier_prop_name(out, t->text, t->text_len);
            if (t->reference_type_args_len > 0 && t->reference_type_args) {
                ctsc_buf_append_char(out, '<');
                for (size_t i = 0; i < t->reference_type_args_len; ++i) {
                    if (i > 0) ctsc_buf_append_cstr(out, ", ");
                    ctsc_type_to_string(t->reference_type_args[i], out);
                }
                ctsc_buf_append_char(out, '>');
            }
            return;
        case CTSC_TYPE_CLASS_CONSTRUCTOR:
            ctsc_buf_append_cstr(out, "typeof ");
            append_utf16_ascii_identifier_prop_name(out, t->text, t->text_len);
            return;
        case CTSC_TYPE_ENUM_VALUE:
            append_utf16_ascii_identifier_prop_name(out, t->text, t->text_len);
            return;
        case CTSC_TYPE_ENUM_MEMBER_LITERAL:
            append_utf16_ascii_identifier_prop_name(out, t->enum_parent_name, t->enum_parent_name_len);
            ctsc_buf_append_char(out, '.');
            append_utf16_ascii_identifier_prop_name(out, t->enum_member_name, t->enum_member_name_len);
            return;
        case CTSC_TYPE_FUNCTION:
            /*
             * Pre-formatted call signature string. Mirrors tsc's typeToString
             * on an anonymous object type with a single call signature
             * (checker.ts signatureToString ~7500+; emits `(p: T) => R`).
             */
            if (t->function_signature_text && t->function_signature_text_len > 0) {
                ctsc_buf_append(out, t->function_signature_text, t->function_signature_text_len);
            } else {
                ctsc_buf_append_cstr(out, "() => any");
            }
            return;
        case CTSC_TYPE_INDEX:
            /*
             * Mirrors checker.ts typeToTypeNodeHelper for IndexType: emits
             * TypeOperatorNode(KeyOfKeyword, operand) which the printer
             * stringifies as `keyof <operand>` (emitter.ts emitTypeOperator
             * ~2400+). Precedence: operand types that bind looser than
             * `keyof` would need parens; the M4.x-keyof slice only stores
             * TypeReference / primitive operands (never a union / function
             * type), so delegating straight through is correct for the
             * current fixture set.
             */
            ctsc_buf_append_cstr(out, "keyof ");
            ctsc_type_to_string(t->index_target, out);
            return;
        default:
            ctsc_buf_append_cstr(out, "any");
            return;
    }
}

#ifndef CTSC_TYPE_H
#define CTSC_TYPE_H

#include "common.h"
#include "buffer.h"

/*
 * Phase 4 (M4.0) type representation.
 *
 * Minimal subset of TypeScript's internal Type hierarchy, designed to grow.
 * Mirrors upstream/TypeScript/src/compiler/types.ts `TypeFlags` / `Type` in
 * spirit, but only the kinds currently needed by ctsc's checker. Additional
 * kinds (Object literal types, Tuple, Union variants, Generic TypeParameter,
 * Class/Interface declaration types, ...) will be added as M4.1+ fixtures
 * unlock them.
 *
 * Serialisation rule: ctsc_type_to_string() produces the exact string that
 * tsc's `TypeChecker.typeToString()` emits for the same type in default
 * formatting mode (with NoTruncation | UseFullyQualifiedType as used by
 * harness/src/oracle-checker-types.ts). Any divergence is an M4.x bug.
 */

typedef enum {
    CTSC_TYPE_UNKNOWN = 0,

    /* Primitive intrinsic types. Names mirror tsc's "intrinsicName" on the
     * IntrinsicType subclass (types.ts IntrinsicType). */
    CTSC_TYPE_ANY,
    CTSC_TYPE_UNKNOWN_T,   /* explicit unknown (vs CTSC_TYPE_UNKNOWN sentinel) */
    CTSC_TYPE_VOID,
    CTSC_TYPE_UNDEFINED,
    CTSC_TYPE_NULL,
    CTSC_TYPE_NEVER,
    CTSC_TYPE_NUMBER,
    CTSC_TYPE_BIGINT,
    CTSC_TYPE_STRING,
    CTSC_TYPE_BOOLEAN,
    CTSC_TYPE_SYMBOL,
    CTSC_TYPE_OBJECT,      /* the keyword/intrinsic `object` */
    /* Empty object type `{}` (TypeFlags.Object). With noLib, array literals
     * infer to this (checker.ts checkArrayLiteral → createArrayType ~33399). */
    CTSC_TYPE_EMPTY_OBJECT,

    /* Literal types (types.ts LiteralType). */
    CTSC_TYPE_NUMBER_LITERAL,
    CTSC_TYPE_STRING_LITERAL,
    CTSC_TYPE_BOOLEAN_LITERAL,   /* true/false */
    CTSC_TYPE_BIGINT_LITERAL,

    /* Union type (types.ts UnionType). Stored flattened / deduped. */
    CTSC_TYPE_UNION,

    /* Reserved for M4.1+ (kept here so switch tables compile without churn). */
    CTSC_TYPE_FUNCTION
} CtscTypeKind;

typedef struct CtscType CtscType;

struct CtscType {
    CtscTypeKind kind;

    /* Literal payload; only one set depending on kind. */
    double      number_value;      /* NUMBER_LITERAL */
    bool        boolean_value;     /* BOOLEAN_LITERAL */
    /* STRING_LITERAL / BIGINT_LITERAL text in UTF-16 (points into scanner
     * memory or arena, no ownership). text_len counts UTF-16 code units. */
    const uint16_t* text;
    size_t          text_len;

    /* Union payload. Members are unique, sorted in declaration order (tsc
     * preserves the order types entered the union in most cases). */
    CtscType**  union_members;
    size_t      union_members_len;
};

struct CtscArena;

typedef struct CtscTypeRegistry {
    struct CtscArena* arena;

    /* Primitive singletons; created lazily in ctsc_type_registry_init(). */
    CtscType* t_any;
    CtscType* t_unknown;
    CtscType* t_void;
    CtscType* t_undefined;
    CtscType* t_null;
    CtscType* t_never;
    CtscType* t_number;
    CtscType* t_bigint;
    CtscType* t_string;
    CtscType* t_boolean;
    CtscType* t_symbol;
    CtscType* t_object;
    CtscType* t_empty_object;
    CtscType* t_true;
    CtscType* t_false;
} CtscTypeRegistry;

/* Initialise registry + populate primitive singletons. */
void ctsc_type_registry_init(CtscTypeRegistry* reg, struct CtscArena* arena);

/* Arena-allocate a new CtscType of the requested kind with zeroed payload. */
CtscType* ctsc_type_new(CtscTypeRegistry* reg, CtscTypeKind kind);

/* Literal constructors (return cached/new CtscType in the arena). */
CtscType* ctsc_type_number_literal(CtscTypeRegistry* reg, double v);
CtscType* ctsc_type_string_literal(CtscTypeRegistry* reg, const uint16_t* text, size_t len);
CtscType* ctsc_type_bigint_literal(CtscTypeRegistry* reg, const uint16_t* text, size_t len);

/* Widening rules (types.ts getWidenedLiteralType): narrow literal -> base. */
CtscType* ctsc_type_widen(CtscTypeRegistry* reg, const CtscType* t);

/*
 * Format a CtscType the same way ts.TypeChecker.typeToString would. Appends
 * to `out` without a trailing newline. The output matches what the oracle
 * (harness/src/oracle-checker-types.ts) writes.
 */
void ctsc_type_to_string(const CtscType* t, CtscBuffer* out);

#endif

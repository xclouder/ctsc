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
 * kinds (Tuple, Generic TypeParameter, Class/Interface declaration types,
 * ...) will be added as M4.1+ fixtures unlock them.
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

    /*
     * Anonymous object literal type (checker.ts checkObjectLiteral ~33527).
     * typeToString matches `{ a: number; }` style (space after `{`, `; ` between
     * properties, trailing `; ` before `}`).
     */
    CTSC_TYPE_OBJECT_LITERAL,

    /*
     * Tuple type from a TupleTypeNode / `[T, U, ...]` annotation (checker.ts
     * getTypeFromArrayOrTupleTypeNode ~17824-17840).
     */
    CTSC_TYPE_TUPLE,

    /*
     * Nominal reference to a named class (instance type). typeToString is the
     * class identifier (checker.ts getTypeOfSymbol / class instance ~12537).
     * Payload uses `text` / `text_len` (UTF-16), same storage as literals.
     */
    CTSC_TYPE_REFERENCE,

    /*
     * Class constructor value (`class C {}` then identifier `C` in expression
     * position). typeToString is `typeof <Name>` (checker.ts typeToString on
     * TypeofType / typeof-narrowing of class symbols ~35000+). Property access
     * on this type resolves static members only (getTypeOfSymbol staticType).
     * Payload: `text` / `text_len` = class name (UTF-16), same as REFERENCE.
     */
    CTSC_TYPE_CLASS_CONSTRUCTOR,

    /* Reserved for M4.1+ (kept here so switch tables compile without churn). */
    CTSC_TYPE_FUNCTION
} CtscTypeKind;

typedef struct CtscType CtscType;

/* One named property on an OBJECT_LITERAL type (source order). */
typedef struct {
    const uint16_t* name;
    size_t          name_len;
    CtscType*       value_type;
    bool            optional; /* PropertySignature / type literal `name?: T` */
} CtscObjectProperty;

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

    /* OBJECT_LITERAL */
    CtscObjectProperty* object_properties;
    size_t                object_properties_len;

    /* TUPLE: element types in source order (arena-backed pointer array). */
    CtscType** tuple_elements;
    size_t     tuple_elements_len;

    /*
     * CTSC_TYPE_REFERENCE: optional type arguments for an instantiated generic
     * class (e.g. Box<number>). When reference_type_args_len is 0, typeToString
     * is the bare class name (checker.ts typeToString on generic instantiation).
     */
    CtscType** reference_type_args;
    size_t     reference_type_args_len;

    /*
     * Optional alias name for typeToString when a TypeReference resolved
     * through a local type alias (checker.ts typeToTypeNodeWorker ~6916:
     * type.aliasSymbol + !shouldExpandType → symbolToTypeNode).
     * Assignability still uses union_members / object_properties; only
     * ctsc_type_to_string prefers this when set.
     */
    const uint16_t* alias_symbol_name;
    size_t          alias_symbol_name_len;
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

/* Anonymous object literal type: `props` must live in `reg`'s arena. */
CtscType* ctsc_type_object_literal(CtscTypeRegistry* reg, CtscObjectProperty* props, size_t prop_count);

/* Class instance / nominal named type: typeToString is the identifier text. */
CtscType* ctsc_type_reference(CtscTypeRegistry* reg, const uint16_t* name, size_t name_len);

/* Class identifier in value position: typeToString `typeof Name`; static member lookup. */
CtscType* ctsc_type_class_constructor(CtscTypeRegistry* reg, const uint16_t* name, size_t name_len);

/*
 * Same as ctsc_type_reference but with type arguments (e.g. Box<number>).
 * `args` is copied into the arena; pointers must remain valid for the type's lifetime.
 */
CtscType* ctsc_type_reference_with_type_args(CtscTypeRegistry* reg, const uint16_t* name, size_t name_len,
                                               CtscType** args, size_t arg_count);

/* Tuple type: `elements` copied into the arena (may be NULL when count is 0). */
CtscType* ctsc_type_tuple(CtscTypeRegistry* reg, CtscType** elements, size_t element_count);

/* Widening rules (types.ts getWidenedLiteralType): narrow literal -> base. */
CtscType* ctsc_type_widen(CtscTypeRegistry* reg, const CtscType* t);

/*
 * Format a CtscType the same way ts.TypeChecker.typeToString would. Appends
 * to `out` without a trailing newline. The output matches what the oracle
 * (harness/src/oracle-checker-types.ts) writes.
 */
void ctsc_type_to_string(const CtscType* t, CtscBuffer* out);

#endif

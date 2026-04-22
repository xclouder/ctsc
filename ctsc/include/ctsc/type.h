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
     * Intersection type (types.ts IntersectionType). Constituents in source
     * order (`A & B`); checker.ts getTypeFromIntersectionTypeNode ~14510+.
     */
    CTSC_TYPE_INTERSECTION,

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

    /*
     * `enum E { ... }` value type: `E` in expression position (checker.ts
     * getTypeOfSymbol for regular enum ~12537+). typeToString is `E`;
     * property access yields CTSC_TYPE_ENUM_MEMBER_LITERAL.
     */
    CTSC_TYPE_ENUM_VALUE,

    /*
     * Unique enum member type (checker.ts EnumLiteralType); typeToString `E.M`.
     * Parent / member names are UTF-16 slices (typically Identifier text).
     */
    CTSC_TYPE_ENUM_MEMBER_LITERAL,

    /*
     * Anonymous function type with a single call signature.
     *
     * Built for `typeof fn` on a FunctionDeclaration symbol (checker.ts
     * getTypeOfSymbol → getTypeOfFuncClassEnumModule → createAnonymousType
     * with resolved call signatures; getTypeFromTypeQueryNode ~17410 flows
     * this type through `type X = typeof fn`). M4.0 stores a pre-formatted
     * signature string (e.g. `"(n: number) => number"`) + an optional
     * opaque pointer back to the FunctionDeclaration node so call
     * expressions on a value of this type can recover the return type
     * (checker.ts getReturnTypeOfSignature path).
     */
    CTSC_TYPE_FUNCTION,

    /*
     * Index type (the result of `keyof T`). Mirrors TypeScript's IndexType
     * (types.ts IndexType; checker.ts getIndexType ~16800+). Stores the
     * operand type in `index_target`. typeToString emits `keyof <target>`
     * (checker.ts typeToString / typeToTypeNodeHelper for IndexType emits
     * TypeOperatorNode(KeyOfKeyword)).
     *
     * M4.x-keyof only surfaces this type through a `keyof <TypeReference>`
     * annotation (parser.ts parseTypeOperator ~4752); full reduction to a
     * string-literal union of an object type's property names (checker.ts
     * getLiteralTypeFromPropertyNames ~16777) is not yet implemented —
     * formatting preserves the syntactic `keyof X` form, which is what tsc
     * emits for an un-distributed `keyof` of a generic / interface type.
     */
    CTSC_TYPE_INDEX
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

    /* INTERSECTION: same storage shape as union; distinct kind for typeToString. */
    CtscType**  intersection_members;
    size_t      intersection_members_len;

    /* OBJECT_LITERAL */
    CtscObjectProperty* object_properties;
    size_t                object_properties_len;
    /*
     * Optional string index signature `[k: string]: T` recovered from a type
     * literal / interface body span (checker.ts getIndexTypeOfType ~19200+).
     * When set, element access with a string literal argument yields this type.
     */
    CtscType* object_string_index_value_type;

    /* TUPLE: element types in source order (arena-backed pointer array). */
    CtscType** tuple_elements;
    size_t     tuple_elements_len;
    /*
     * Per-element flag bitmask parallel to tuple_elements (may be NULL when
     * every slot is Required). Mirrors TypeScript's TupleType.elementFlags
     * (types.ts ElementFlags ~6691): Required=1, Optional=2, Rest=4,
     * Variadic=8. Used by ctsc_type_to_string to re-emit `T?` for Optional
     * and `...T[]` for Rest (checker.ts typeToTypeNodeHelper ~7432-7454).
     */
    uint8_t*   tuple_element_flags;
    /*
     * Optional parallel arrays of per-element labels (names) for a
     * NamedTupleMember (parser.ts parseTupleElementNameOrTupleElementType
     * ~4464-4477). Mirrors TypeScript's TupleType.labeledElementDeclarations
     * (types.ts TupleType; checker.ts typeToTypeNodeHelper ~7437-7449 which
     * re-emits `name: T`, `name?: T`, and `...name: T[]` via
     * factory.createNamedTupleMember). `tuple_element_labels[i]` is NULL
     * for an unlabeled slot; the whole array is NULL when no slot is labeled.
     * Name text points into the source (UTF-16 code units), no ownership.
     */
    const uint16_t** tuple_element_labels;
    size_t*          tuple_element_label_lens;
    /*
     * Whether the tuple was written under a `readonly` TypeOperator (e.g.
     * `readonly [number, string]`). Mirrors TypeScript's TupleType.readonly
     * flag on the target (types.ts TupleType.readonly; checker.ts
     * getArrayOrTupleTargetType ~17745-17752 + typeToTypeNodeHelper
     * ~7457-7463 which wraps a TupleTypeNode in a
     * TypeOperatorNode(ReadonlyKeyword) when the target is readonly).
     */
    bool       tuple_readonly;

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

    /* CTSC_TYPE_ENUM_MEMBER_LITERAL only: `enum_parent`.`enum_member` */
    const uint16_t* enum_parent_name;
    size_t          enum_parent_name_len;
    const uint16_t* enum_member_name;
    size_t          enum_member_name_len;

    /*
     * CTSC_TYPE_FUNCTION only.
     *
     * `function_signature_text` is the pre-formatted UTF-8 signature string
     * (e.g. "(n: number) => number"), used by ctsc_type_to_string so the
     * types-channel dump reproduces tsc's typeToString output. The text
     * must live in an arena that outlives the type (ctsc_type_function
     * copies the caller's bytes into `reg->arena`).
     *
     * `function_decl` is an opaque pointer to the source FunctionDeclaration
     * node — declared `const void*` to keep this header decoupled from the
     * AST header. Call-expression handling in checker.c casts it back to
     * `const CtscNode*` to reach the parameters / return annotation so
     * `const r = f(1)` can read `f`'s return type.
     */
    const char* function_signature_text;
    size_t      function_signature_text_len;
    const void* function_decl;

    /*
     * CTSC_TYPE_INDEX only: operand of `keyof`. Mirrors TypeScript's
     * IndexType.type (types.ts IndexType; checker.ts createIndexType
     * ~16800+).
     */
    CtscType* index_target;
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

/* Regular / non-const enum object type (expression-side `E`). */
CtscType* ctsc_type_enum_value(CtscTypeRegistry* reg, const uint16_t* name, size_t name_len);

/* Enum member literal type `E.M` (checker.ts EnumLiteralType). */
CtscType* ctsc_type_enum_member_literal(CtscTypeRegistry* reg, const uint16_t* enum_name, size_t enum_name_len,
                                        const uint16_t* member_name, size_t member_name_len);

/*
 * Anonymous function type with one call signature.
 *
 * `signature_text` bytes (UTF-8) are copied into `reg->arena` so the caller
 * may free its local buffer after the call; `signature_text_len` counts
 * bytes (no trailing NUL required). `decl` is an opaque pointer to the
 * FunctionDeclaration node and is stored verbatim (not owned).
 */
CtscType* ctsc_type_function(CtscTypeRegistry* reg, const char* signature_text, size_t signature_text_len,
                             const void* decl);

/*
 * Same as ctsc_type_reference but with type arguments (e.g. Box<number>).
 * `args` is copied into the arena; pointers must remain valid for the type's lifetime.
 */
CtscType* ctsc_type_reference_with_type_args(CtscTypeRegistry* reg, const uint16_t* name, size_t name_len,
                                               CtscType** args, size_t arg_count);

/*
 * Per-element flag bits for a TUPLE type; mirrors types.ts ElementFlags
 * (~6691). Tuple constructors take an optional parallel `uint8_t*` array
 * where each entry is a bitmask of these values.
 */
#define CTSC_TUPLE_ELEMENT_REQUIRED  (1u << 0)
#define CTSC_TUPLE_ELEMENT_OPTIONAL  (1u << 1)
#define CTSC_TUPLE_ELEMENT_REST      (1u << 2)
#define CTSC_TUPLE_ELEMENT_VARIADIC  (1u << 3)

/* Tuple type: `elements` copied into the arena (may be NULL when count is 0). */
CtscType* ctsc_type_tuple(CtscTypeRegistry* reg, CtscType** elements, size_t element_count);

/*
 * Tuple type with a parallel per-element flag bitmask (mirrors TypeScript's
 * TupleType.elementFlags; checker.ts typeToTypeNodeHelper ~7432-7454).
 * `element_flags` may be NULL to mean "all Required" (equivalent to
 * ctsc_type_tuple). Both arrays are copied into the arena.
 */
CtscType* ctsc_type_tuple_with_element_flags(CtscTypeRegistry* reg, CtscType** elements, const uint8_t* element_flags,
                                             size_t element_count);

/*
 * Tuple type wrapped with `readonly` (e.g. `readonly [T, U]`). Mirrors
 * TupleType.readonly on the target (checker.ts getArrayOrTupleTargetType
 * ~17745-17752; typeToTypeNodeHelper ~7457-7463 wraps in TypeOperatorNode
 * ReadonlyKeyword). `element_flags` follows the same semantics as
 * ctsc_type_tuple_with_element_flags.
 */
CtscType* ctsc_type_readonly_tuple_with_element_flags(CtscTypeRegistry* reg, CtscType** elements,
                                                      const uint8_t* element_flags, size_t element_count);

/*
 * Tuple with optional per-element labels for NamedTupleMember nodes
 * (parser.ts parseTupleElementNameOrTupleElementType ~4464-4477; checker.ts
 * typeToTypeNodeHelper ~7437-7449 emits labels via createNamedTupleMember).
 * `labels` / `label_lens` may be NULL for no labels, or parallel arrays of
 * length `element_count` where entry `i` is NULL (no label) or a UTF-16
 * slice. Both label arrays + `element_flags` are copied into the arena.
 * `readonly_tuple` sets TupleType.readonly (checker.ts
 * getArrayOrTupleTargetType ~17745-17752).
 */
CtscType* ctsc_type_tuple_with_element_flags_and_labels(CtscTypeRegistry* reg, CtscType** elements,
                                                        const uint8_t* element_flags,
                                                        const uint16_t* const* labels, const size_t* label_lens,
                                                        size_t element_count, bool readonly_tuple);

/* Intersection: `members` copied into the arena; count must be >= 1. */
CtscType* ctsc_type_intersection(CtscTypeRegistry* reg, CtscType** members, size_t member_count);

/*
 * Index type (`keyof T`). Mirrors TypeScript's getIndexType (checker.ts
 * ~16800+). M4.x-keyof preserves the syntactic `keyof X` form;
 * ctsc_type_to_string emits `keyof <target>` by delegating to the target's
 * typeToString.
 */
CtscType* ctsc_type_index(CtscTypeRegistry* reg, CtscType* target);

/* Widening rules (types.ts getWidenedLiteralType): narrow literal -> base. */
CtscType* ctsc_type_widen(CtscTypeRegistry* reg, const CtscType* t);

/*
 * Format a CtscType the same way ts.TypeChecker.typeToString would. Appends
 * to `out` without a trailing newline. The output matches what the oracle
 * (harness/src/oracle-checker-types.ts) writes.
 */
void ctsc_type_to_string(const CtscType* t, CtscBuffer* out);

#endif

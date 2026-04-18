/*
 * Unicode identifier classification tables.
 *
 * Mirrors upstream/TypeScript/src/compiler/scanner.ts
 *   unicodeESNextIdentifierStart / unicodeESNextIdentifierPart (~342-344)
 *   unicodeES5IdentifierStart    / unicodeES5IdentifierPart    (~331-333)
 *   lookupInUnicodeMap (~358)
 *   isUnicodeIdentifierStart / isUnicodeIdentifierPart (~389 / ~395)
 *
 * The scanner picks ESNext vs ES5 via `languageVersion >= ScriptTarget.ES2015`.
 * Our harness oracle creates the TS scanner with ScriptTarget.Latest, so the
 * ESNext tables are the default path; we keep ES5 tables around for
 * completeness in case the driver later honours --target.
 */
#include "unicode_id.h"

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#include "unicode_id_tables.inl"

/*
 * Binary search over a sorted [lo, hi] range-pair map. Mirrors
 * upstream scanner.ts lookupInUnicodeMap (~358). The map always contains
 * an even number of entries: indices 0,2,4... are range lows, 1,3,5... are
 * (inclusive) range highs.
 */
static bool lookup_in_range_map(uint32_t code, const uint32_t* map, size_t n) {
    if (n == 0 || code < map[0]) { return false; }
    size_t lo = 0;
    size_t hi = n;
    while (lo + 1 < hi) {
        size_t mid = lo + (hi - lo) / 2;
        mid -= mid % 2;
        if (map[mid] <= code && code <= map[mid + 1]) {
            return true;
        }
        if (code < map[mid]) {
            hi = mid;
        } else {
            lo = mid + 2;
        }
    }
    return false;
}

bool ctsc_is_unicode_identifier_start(uint32_t code) {
    return lookup_in_range_map(code, unicode_esnext_id_start,
        sizeof(unicode_esnext_id_start) / sizeof(unicode_esnext_id_start[0]));
}

bool ctsc_is_unicode_identifier_part(uint32_t code) {
    return lookup_in_range_map(code, unicode_esnext_id_part,
        sizeof(unicode_esnext_id_part) / sizeof(unicode_esnext_id_part[0]));
}

#ifndef CTSC_SCANNER_UNICODE_ID_H
#define CTSC_SCANNER_UNICODE_ID_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Returns true iff `code` (a Unicode code point, 0..0x10FFFF) is an
 * IdentifierStart under the ESNext Unicode identifier tables used by tsc
 * when languageVersion >= ScriptTarget.ES2015. Mirrors
 * upstream/TypeScript/src/compiler/scanner.ts isUnicodeIdentifierStart.
 *
 * Callers should only invoke this for non-ASCII code points (> 127);
 * ASCII is handled by the fast path in scanner.c.
 */
bool ctsc_is_unicode_identifier_start(uint32_t code);

/*
 * Returns true iff `code` is an IdentifierPart under the ESNext tables.
 * See `ctsc_is_unicode_identifier_start` for the ASCII caveat.
 */
bool ctsc_is_unicode_identifier_part(uint32_t code);

#endif

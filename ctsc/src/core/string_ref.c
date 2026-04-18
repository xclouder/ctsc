#include "ctsc/string_ref.h"

#include <string.h>

bool ctsc_str_eq(CtscStringRef a, CtscStringRef b) {
    if (a.len != b.len) { return false; }
    if (a.len == 0) { return true; }
    return memcmp(a.data, b.data, a.len) == 0;
}

bool ctsc_str_eq_z(CtscStringRef a, const char* z) {
    size_t n = strlen(z);
    if (a.len != n) { return false; }
    return memcmp(a.data, z, n) == 0;
}

size_t ctsc_str_hash(CtscStringRef s) {
    size_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < s.len; ++i) {
        h ^= (unsigned char)s.data[i];
        h *= 1099511628211ULL;
    }
    return h;
}

#include "ctsc/string_ref.h"
#include <stdio.h>
#include <string.h>

#define EXPECT(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failed++; } } while (0)

int test_string_ref(void) {
    int failed = 0;
    CtscStringRef a = CTSC_STR_LIT("abc");
    CtscStringRef b = ctsc_str_z("abc");
    EXPECT(ctsc_str_eq(a, b));
    EXPECT(ctsc_str_eq_z(a, "abc"));
    EXPECT(!ctsc_str_eq_z(a, "abcd"));
    EXPECT(ctsc_str_hash(a) == ctsc_str_hash(b));
    return failed;
}

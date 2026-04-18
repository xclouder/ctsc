#include <stdio.h>

/* Declared by each unit test TU. */
int test_arena(void);
int test_string_ref(void);
int test_scanner(void);
int test_parser(void);
int test_binder(void);
int test_emitter(void);

int main(void) {
    int failed = 0;
    failed += test_arena();
    failed += test_string_ref();
    failed += test_scanner();
    failed += test_parser();
    failed += test_binder();
    failed += test_emitter();
    if (failed) {
        fprintf(stderr, "ctsc_tests: %d test(s) failed\n", failed);
        return 1;
    }
    printf("ctsc_tests: all passed\n");
    return 0;
}

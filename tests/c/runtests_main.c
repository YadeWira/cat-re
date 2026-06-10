/* Single main that runs all test groups. The test_*.c files
 * contain their own main()s as static, so we include them. */
#include <stdio.h>
extern int test_format_main(void);
extern int test_zip_main(void);
extern int test_jp2_main(void);
int main(void) {
    int r = 0;
    r |= test_format_main();
    r |= test_zip_main();
    r |= test_jp2_main();
    if (r == 0) printf("\nALL TESTS PASSED\n");
    return r;
}

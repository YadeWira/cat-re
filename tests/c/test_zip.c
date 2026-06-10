#include <unistd.h>
#include "cat.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>

/* Test the ZIP roundtrip via cat_write_file + cat_read_file */
static void test_zip_roundtrip(void) {
    /* Two temp files */
    char fn1[] = "/tmp/cat_t1_XXXXXX";
    char fn2[] = "/tmp/cat_t2_XXXXXX";
    int fd1 = mkstemp(fn1); assert(fd1 >= 0);
    int fd2 = mkstemp(fn2); assert(fd2 >= 0);
    write(fd1, "hello A", 7);
    write(fd2, "world B content", 15);
    close(fd1); close(fd2);
    /* Pack */
    char out_path[] = "/tmp/cat_zip_test_XXXXXX";
    int ofd = mkstemp(out_path); assert(ofd >= 0); close(ofd);
    const char *names[] = {fn1, fn2};
    cat_status s = cat_write_file(out_path, names, 2, (const uint8_t *)"data", 4);
    assert(s == CAT_OK);
    /* Read back */
    cat_header h; cat_file **files = NULL; size_t cnt = 0;
    s = cat_read_file(out_path, &h, &files, &cnt);
    assert(s == CAT_OK);
    assert(cnt == 2);
    /* The actual on-disk names are the temp paths, not what we want -
     * just verify content matches by size and a partial byte check. */
    int foundA = 0, foundB = 0;
    for (size_t i = 0; i < cnt; i++) {
        if (files[i]->size == 7 && memcmp(files[i]->data, "hello A", 7) == 0) foundA = 1;
        if (files[i]->size == 15 && memcmp(files[i]->data, "world B content", 15) == 0) foundB = 1;
    }
    assert(foundA); assert(foundB);
    cat_files_free(files, cnt);
    cat_header_free(&h);
    unlink(fn1); unlink(fn2); unlink(out_path);
    printf("test_zip_roundtrip: ok\n");
}

/* Test the raw roundtrip */
static void test_raw_roundtrip(void) {
    char fn[] = "/tmp/cat_raw_XXXXXX";
    int fd = mkstemp(fn); assert(fd >= 0);
    write(fd, "raw test data", 13);
    close(fd);
    char out_path[] = "/tmp/cat_raw_out_XXXXXX";
    int ofd = mkstemp(out_path); assert(ofd >= 0); close(ofd);
    const char *names[] = {fn};
    cat_status s = cat_write_file(out_path, names, 1, (const uint8_t *)"hello.txt", 9);
    assert(s == CAT_OK);
    cat_header h; cat_file **files = NULL; size_t cnt = 0;
    s = cat_read_file(out_path, &h, &files, &cnt);
    assert(s == CAT_OK);
    assert(cnt == 1);
    assert(files[0]->size == 13);
    assert(memcmp(files[0]->data, "raw test data", 13) == 0);
    /* Name should come from ext_header */
    assert(strcmp(files[0]->name, "hello.txt") == 0);
    cat_files_free(files, cnt);
    cat_header_free(&h);
    unlink(fn); unlink(out_path);
    printf("test_raw_roundtrip: ok\n");
}

int test_zip_main(void) {
    test_zip_roundtrip();
    test_raw_roundtrip();
    printf("\nALL ZIP TESTS PASSED\n");
    return 0;
}

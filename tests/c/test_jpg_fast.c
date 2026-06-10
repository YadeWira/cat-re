#include "cat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
static int test_count = 0, pass_count = 0, fail_count = 0;
static void test_one(const char *jpg_path) {
    test_count++;
    char msg[256] = {0};
    uint8_t *rgb = NULL; int w = 0, h = 0;
    cat_status s = cat_decode_jpg_to_rgb(jpg_path, &rgb, &w, &h);
    if (s != CAT_OK) { snprintf(msg, sizeof(msg), "decode=%d", s); goto fail; }
    uint8_t *jp2 = NULL; size_t jplen = 0;
    s = cat_jpg_to_jp2(jpg_path, &jp2, &jplen);
    if (s != CAT_OK) { snprintf(msg, sizeof(msg), "jp2enc=%d", s); free(rgb); goto fail; }
    s = cat_decode_jp2(jp2, jplen, "/tmp/cat_test.bmp");
    if (s != CAT_OK) { snprintf(msg, sizeof(msg), "jp2dec=%d", s); free(rgb); free(jp2); goto fail; }
    FILE *f = fopen("/tmp/cat_test.bmp", "rb");
    uint8_t hdr[26]; fread(hdr, 1, 26, f);
    fclose(f);
    int bmp_w = hdr[18] | (hdr[19]<<8) | (hdr[20]<<16) | (hdr[21]<<24);
    int bmp_h = hdr[22] | (hdr[23]<<8) | (hdr[24]<<16) | (hdr[25]<<24);
    if (bmp_w != w || bmp_h != h) {
        snprintf(msg, sizeof(msg), "dim %dx%d vs %dx%d", w, h, bmp_w, bmp_h);
        free(rgb); free(jp2); goto fail;
    }
    free(rgb); free(jp2);
    pass_count++;
    unlink("/tmp/cat_test.bmp");
    return;
fail:
    fprintf(stderr, "  [FAIL] %s: %s\n", jpg_path, msg);
    fail_count++;
    unlink("/tmp/cat_test.bmp");
}
int main(int argc, char **argv) {
    const char *dir = (argc > 1) ? argv[1] : "samples/jpg-files";
    DIR *d = opendir(dir);
    if (!d) { perror("opendir"); return 1; }
    struct dirent *de;
    int total = 0;
    while ((de = readdir(d))) {
        if (strstr(de->d_name, ".jpg") || strstr(de->d_name, ".JPG")) {
            char path[1024];
            snprintf(path, sizeof(path), "%s/%s", dir, de->d_name);
            test_one(path);
            total++;
        }
    }
    closedir(d);
    fprintf(stderr, "==== Results: %d/%d passed, %d failed ====\n",
            pass_count, total, fail_count);
    return (fail_count > 0) ? 1 : 0;
}

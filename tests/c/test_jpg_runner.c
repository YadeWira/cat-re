/* Test runner: process a directory of JPG files through the libcat
 * pipeline (lossless JPG -> RGB -> JP2 -> .qcf -> BMP). */
#include "cat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

static int test_count = 0, pass_count = 0, fail_count = 0;

static void test_one(const char *jpg_path) {
    test_count++;
    /* Decode JPG to RGB and re-encode as JP2 */
    uint8_t *rgb = NULL; int w = 0, h = 0;
    cat_status s = cat_decode_jpg_to_rgb(jpg_path, &rgb, &w, &h);
    if (s != CAT_OK) {
        fprintf(stderr, "  [FAIL] %s: decode failed: %d\n", jpg_path, s);
        fail_count++;
        return;
    }
    uint8_t *jp2 = NULL; size_t jplen = 0;
    s = cat_jpg_to_jp2(jpg_path, &jp2, &jplen);
    if (s != CAT_OK) {
        fprintf(stderr, "  [FAIL] %s: jpg->jp2 failed: %d\n", jpg_path, s);
        free(rgb); fail_count++;
        return;
    }
    /* Write to .qcf */
    cat_header qh = {0};
    qh.magic = CAT_MAGIC_QCF;
    qh.ext_hdr_size = 0;
    uint8_t hdr_buf[CAT_HDR_SIZE];
    cat_header_serialize(&qh, hdr_buf);
    char out_path[1024];
    snprintf(out_path, sizeof(out_path), "/tmp/cat_test_%d.qcf", test_count);
    FILE *f = fopen(out_path, "wb");
    fwrite(hdr_buf, 1, CAT_HDR_SIZE, f);
    fwrite(jp2, 1, jplen, f);
    fclose(f);
    /* Read back */
    cat_header h2 = {0}; cat_file **files = NULL; size_t count = 0;
    s = cat_read_file(out_path, &h2, &files, &count);
    if (s != CAT_OK) {
        fprintf(stderr, "  [FAIL] %s: readback failed: %d\n", jpg_path, s);
        free(rgb); free(jp2); fail_count++;
        return;
    }
    if (count != 1) {
        fprintf(stderr, "  [FAIL] %s: expected 1 file, got %zu\n", jpg_path, count);
        cat_files_free(files, count); cat_header_free(&h2);
        free(rgb); free(jp2); fail_count++;
        return;
    }
    /* The dispatch auto-decodes JP2 to BMP, so we get a BMP.
     * Verify it's a valid BMP of the right dimensions. */
    if (files[0]->size < 54 || files[0]->data[0] != 'B' || files[0]->data[1] != 'M') {
        fprintf(stderr, "  [FAIL] %s: not a BMP\n", jpg_path);
        cat_files_free(files, count); cat_header_free(&h2);
        free(rgb); free(jp2); fail_count++;
        return;
    }
    int bmp_w = files[0]->data[18] | (files[0]->data[19]<<8)
              | (files[0]->data[20]<<16) | (files[0]->data[21]<<24);
    int bmp_h = files[0]->data[22] | (files[0]->data[23]<<8)
              | (files[0]->data[24]<<16) | (files[0]->data[25]<<24);
    if (bmp_w != w || bmp_h != h) {
        fprintf(stderr, "  [FAIL] %s: dim mismatch %dx%d vs %dx%d\n",
                jpg_path, w, h, bmp_w, bmp_h);
        cat_files_free(files, count); cat_header_free(&h2);
        free(rgb); free(jp2); fail_count++;
        return;
    }
    /* Compare pixel data */
    int row_size = ((w * 3 + 3) / 4) * 4;
    uint8_t *bmp_pix = files[0]->data + 54;
    int pix_diff = 0;
    for (int y = 0; y < h; y++) {
        const uint8_t *rgb_row = rgb + (size_t)(h - 1 - y) * w * 3;
        const uint8_t *bmp_row = bmp_pix + (size_t)y * row_size;
        if (memcmp(rgb_row, bmp_row, (size_t)w * 3) != 0) pix_diff++;
    }
    if (pix_diff > 0) {
        fprintf(stderr, "  [FAIL] %s: %d pixel rows differ (lossy bug!)\n",
                jpg_path, pix_diff);
        cat_files_free(files, count); cat_header_free(&h2);
        free(rgb); free(jp2); fail_count++;
        return;
    }
    cat_files_free(files, count); cat_header_free(&h2);
    free(rgb); free(jp2);
    unlink(out_path);
    pass_count++;
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

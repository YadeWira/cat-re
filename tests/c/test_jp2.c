/* JP2 roundtrip via OpenJPEG. Uses a fixed synthetic 8x8 RGB BMP. */
#include "cat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>

/* Write a 24-bit BMP with WxH gradient */
static int write_test_bmp(const char *path, int w, int h) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    int row_size = ((w * 3 + 3) / 4) * 4;
    int data_size = row_size * h;
    int file_size = 54 + data_size;
    uint8_t hdr[54] = {0};
    hdr[0] = 'B'; hdr[1] = 'M';
    hdr[2] = file_size & 0xff; hdr[3] = (file_size>>8) & 0xff;
    hdr[4] = (file_size>>16) & 0xff; hdr[5] = (file_size>>24) & 0xff;
    hdr[10] = 54;
    hdr[14] = 40;
    hdr[18] = w & 0xff; hdr[19] = (w>>8) & 0xff;
    hdr[20] = (w>>16) & 0xff; hdr[21] = (w>>24) & 0xff;
    hdr[22] = h & 0xff; hdr[23] = (h>>8) & 0xff;
    hdr[24] = (h>>16) & 0xff; hdr[25] = (h>>24) & 0xff;
    hdr[26] = 1;
    hdr[28] = 24;
    hdr[34] = data_size & 0xff; hdr[35] = (data_size>>8) & 0xff;
    hdr[36] = (data_size>>16) & 0xff; hdr[37] = (data_size>>24) & 0xff;
    fwrite(hdr, 1, 54, f);
    for (int y = h - 1; y >= 0; y--) {
        for (int x = 0; x < w; x++) {
            uint8_t px[3] = { (uint8_t)(x * 8), (uint8_t)(y * 8), (uint8_t)((x + y) * 4) };
            fwrite(px, 1, 3, f);
        }
        for (int p = 0; p < row_size - w * 3; p++) fputc(0, f);
    }
    fclose(f);
    return 0;
}

static void test_jp2_roundtrip(void) {
    const char *bmp_in = "/tmp/cat_jp2_in.bmp";
    const char *bmp_out = "/tmp/cat_jp2_out.bmp";
    /* Create test input */
    assert(write_test_bmp(bmp_in, 32, 32) == 0);
    /* Encode */
    uint8_t *jp2 = NULL;
    size_t jplen = 0;
    cat_status s = cat_encode_jp2_from_bmp(bmp_in, &jp2, &jplen);
    assert(s == CAT_OK);
    assert(jp2 != NULL);
    assert(jplen > 0);
    printf("  encoded %zu bytes\n", jplen);
    /* Verify JP2 magic */
    assert(jplen >= 12);
    uint8_t jp2_box[12] = {0,0,0,0x0c,0x6a,0x50,0x20,0x20,0x0d,0x0a,0x87,0x0a};
    assert(memcmp(jp2, jp2_box, 12) == 0);
    /* Decode */
    s = cat_decode_jp2(jp2, jplen, bmp_out);
    assert(s == CAT_OK);
    /* Verify BMP dimensions in the output file */
    FILE *f = fopen(bmp_out, "rb");
    assert(f);
    uint8_t hdr[54];
    fread(hdr, 1, 54, f);
    fclose(f);
    int w = hdr[18] | (hdr[19]<<8);
    int h = hdr[22] | (hdr[23]<<8);
    assert(w == 32 && h == 32);
    free(jp2);
    unlink(bmp_in);
    unlink(bmp_out);
    printf("test_jp2_roundtrip: ok\n");
}

int test_jp2_main(void) {
    test_jp2_roundtrip();
    printf("\nALL JP2 TESTS PASSED\n");
    return 0;
}

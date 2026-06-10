/* JPEG decoder backend (uses libjpeg). Decodes a JPEG file to 24-bit
 * RGB raw pixels, and a convenience function that converts a JPEG
 * file to a JP2 codestream on disk.
 */
#include "cat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <jpeglib.h>
#include <setjmp.h>

struct jpg_error_mgr {
    struct jpeg_error_mgr pub;
    jmp_buf jmp;
};

static void jpg_error_exit(j_common_ptr cinfo) {
    struct jpg_error_mgr *e = (struct jpg_error_mgr *)cinfo->err;
    longjmp(e->jmp, 1);
}

cat_status cat_decode_jpg_to_rgb(const char *path,
                                 uint8_t **out_pixels,
                                 int *out_w, int *out_h) {
    FILE *f = fopen(path, "rb");
    if (!f) return CAT_ERR_IO;
    struct jpeg_decompress_struct cinfo;
    struct jpg_error_mgr jerr;
    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = jpg_error_exit;
    if (setjmp(jerr.jmp)) {
        jpeg_destroy_decompress(&cinfo);
        fclose(f);
        return CAT_ERR_FORMAT;
    }
    jpeg_create_decompress(&cinfo);
    jpeg_stdio_src(&cinfo, f);
    if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
        jpeg_destroy_decompress(&cinfo);
        fclose(f);
        return CAT_ERR_FORMAT;
    }
    cinfo.out_color_space = JCS_RGB;
    jpeg_start_decompress(&cinfo);
    int w = (int)cinfo.output_width;
    int h = (int)cinfo.output_height;
    size_t row_stride = (size_t)w * 3;
    uint8_t *pixels = (uint8_t *)malloc(row_stride * (size_t)h);
    if (!pixels) {
        jpeg_destroy_decompress(&cinfo);
        fclose(f);
        return CAT_ERR_NOMEM;
    }
    while (cinfo.output_scanline < cinfo.output_height) {
        uint8_t *row = pixels + (size_t)cinfo.output_scanline * row_stride;
        JSAMPROW r[1] = { row };
        jpeg_read_scanlines(&cinfo, r, 1);
    }
    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    fclose(f);
    *out_pixels = pixels;
    *out_w = w;
    *out_h = h;
    return CAT_OK;
}

/* Write 24-bit BMP (BGR rows, bottom-up) to a file. */
static cat_status write_bmp(const char *path, int w, int h, const uint8_t *rgb) {
    int row_size = ((w * 3 + 3) / 4) * 4;
    size_t data_size = (size_t)row_size * (size_t)h;
    size_t file_size = 54 + data_size;
    FILE *f = fopen(path, "wb");
    if (!f) return CAT_ERR_IO;
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
        const uint8_t *row = rgb + (size_t)y * w * 3;
        fwrite(row, 1, (size_t)w * 3, f);
        for (int p = 0; p < row_size - w * 3; p++) fputc(0, f);
    }
    fclose(f);
    return CAT_OK;
}

/* High-level: convert a JPEG file on disk to a JP2 codestream buffer
 * in memory. Uses libjpeg (decode) + libopenjp2 (encode). */
cat_status cat_jpg_to_jp2(const char *jpg_path, uint8_t **out, size_t *out_len) {
    uint8_t *rgb = NULL;
    int w = 0, h = 0;
    cat_status s = cat_decode_jpg_to_rgb(jpg_path, &rgb, &w, &h);
    if (s != CAT_OK) return s;
    char tmp_bmp[] = "/tmp/cat_jpg2bmp_XXXXXX";
    int fd = mkstemp(tmp_bmp);
    if (fd < 0) { free(rgb); return CAT_ERR_IO; }
    close(fd);
    s = write_bmp(tmp_bmp, w, h, rgb);
    free(rgb);
    if (s != CAT_OK) { unlink(tmp_bmp); return s; }
    uint8_t *jp2 = NULL;
    size_t jplen = 0;
    s = cat_encode_jp2_from_bmp(tmp_bmp, &jp2, &jplen);
    unlink(tmp_bmp);
    if (s != CAT_OK) return s;
    *out = jp2;
    *out_len = jplen;
    return CAT_OK;
}

/* JPEG2000 backend using libopenjp2. */
#include "cat.h"
#include <openjpeg.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>

static cat_status write_bmp(const char *path, int w, int h, int comps,
                            const uint8_t *pixels) {
    int row_size = ((w * comps + 3) / 4) * 4;
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
    hdr[28] = (uint8_t)(comps * 8);
    hdr[34] = data_size & 0xff; hdr[35] = (data_size>>8) & 0xff;
    hdr[36] = (data_size>>16) & 0xff; hdr[37] = (data_size>>24) & 0xff;
    fwrite(hdr, 1, 54, f);
    for (int y = h - 1; y >= 0; y--) {
        fwrite(pixels + (size_t)y * w * comps, 1, (size_t)w * comps, f);
        for (int p = 0; p < row_size - w * comps; p++) fputc(0, f);
    }
    fclose(f);
    return CAT_OK;
}

cat_status cat_decode_jp2(const void *payload, size_t plen, const char *out_path) {
    char tmp[] = "/tmp/cat_jp2_in_XXXXXX";
    int fd = mkstemp(tmp);
    if (fd < 0) return CAT_ERR_IO;
    if (write(fd, payload, plen) != (ssize_t)plen) { close(fd); unlink(tmp); return CAT_ERR_IO; }
    close(fd);
    /* OPJ_TRUE = read stream */
    opj_stream_t *st = opj_stream_create_default_file_stream(tmp, OPJ_TRUE);
    if (!st) { unlink(tmp); return CAT_ERR_OPENJPEG; }
    opj_codec_t *codec = opj_create_decompress(OPJ_CODEC_JP2);
    if (!codec) { opj_stream_destroy(st); unlink(tmp); return CAT_ERR_OPENJPEG; }
    opj_dparameters_t params;
    opj_set_default_decoder_parameters(&params);
    if (!opj_setup_decoder(codec, &params)) {
        opj_destroy_codec(codec); opj_stream_destroy(st); unlink(tmp); return CAT_ERR_OPENJPEG;
    }
    opj_image_t *img = NULL;
    if (!opj_read_header(st, codec, &img)) {
        opj_destroy_codec(codec); opj_stream_destroy(st); unlink(tmp); return CAT_ERR_OPENJPEG;
    }
    if (!opj_decode(codec, st, img)) {
        opj_destroy_codec(codec); opj_stream_destroy(st);
        if (img) opj_image_destroy(img); unlink(tmp); return CAT_ERR_OPENJPEG;
    }
    opj_destroy_codec(codec);
    opj_stream_destroy(st);
    unlink(tmp);
    int w = (int)(img->x1 - img->x0);
    int h = (int)(img->y1 - img->y0);
    int comps = img->numcomps;
    if (w <= 0 || h <= 0 || comps <= 0) {
        opj_image_destroy(img); return CAT_ERR_FORMAT;
    }
    int max_prec = 0;
    for (int c = 0; c < comps; c++) {
        if ((int)img->comps[c].prec > max_prec) max_prec = (int)img->comps[c].prec;
    }
    int bytes_per_sample = (max_prec > 8) ? 2 : 1;
    size_t row_bytes = (size_t)w * comps * bytes_per_sample;
    uint8_t *out = (uint8_t *)malloc(row_bytes * (size_t)h);
    if (!out) { opj_image_destroy(img); return CAT_ERR_NOMEM; }
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            for (int c = 0; c < comps; c++) {
                OPJ_INT32 v = img->comps[c].data[y * w + x];
                if (bytes_per_sample == 1) {
                    out[y * row_bytes + x * comps + c] = (uint8_t)(v & 0xff);
                } else {
                    out[y * row_bytes + (x * comps + c) * 2]     = (uint8_t)(v & 0xff);
                    out[y * row_bytes + (x * comps + c) * 2 + 1] = (uint8_t)((v >> 8) & 0xff);
                }
            }
        }
    }
    cat_status r = write_bmp(out_path, w, h, comps, out);
    free(out);
    opj_image_destroy(img);
    return r;
}

cat_status cat_encode_jp2_from_bmp(const char *in_path, uint8_t **out, size_t *out_len) {
    FILE *f = fopen(in_path, "rb");
    if (!f) return CAT_ERR_IO;
    uint8_t hdr[54];
    if (fread(hdr, 1, 54, f) != 54) { fclose(f); return CAT_ERR_FORMAT; }
    if (hdr[0] != 'B' || hdr[1] != 'M') { fclose(f); return CAT_ERR_FORMAT; }
    int data_off = hdr[10] | (hdr[11]<<8) | (hdr[12]<<16) | (hdr[13]<<24);
    int w = hdr[18] | (hdr[19]<<8) | (hdr[20]<<16) | (hdr[21]<<24);
    int h = hdr[22] | (hdr[23]<<8) | (hdr[24]<<16) | (hdr[25]<<24);
    int bpp = hdr[28] | (hdr[29]<<8);
    int comps = bpp / 8;
    if (comps != 1 && comps != 3) { fclose(f); return CAT_ERR_UNSUPPORTED; }
    fseek(f, data_off, SEEK_SET);
    int row_size = ((w * comps + 3) / 4) * 4;
    uint8_t *rows = (uint8_t *)malloc((size_t)row_size * (size_t)h);
    if (!rows) { fclose(f); return CAT_ERR_NOMEM; }
    if (fread(rows, 1, (size_t)row_size * (size_t)h, f) != (size_t)row_size * (size_t)h) {
        free(rows); fclose(f); return CAT_ERR_IO;
    }
    fclose(f);
    opj_cparameters_t params;
    opj_set_default_encoder_parameters(&params);
    params.cod_format = 0;            /* JP2 box */
    params.tcp_numlayers = 1;
    params.tcp_rates[0] = 0;         /* lossless */
    params.cp_disto_alloc = 1;
    params.irreversible = 0;         /* reversible 5-3 wavelet */
    opj_image_cmptparm_t cmpt[4] = {{0}};
    for (int c = 0; c < comps; c++) {
        cmpt[c].dx = 1; cmpt[c].dy = 1;
        cmpt[c].w = w; cmpt[c].h = h;
        cmpt[c].x0 = 0; cmpt[c].y0 = 0;
        cmpt[c].prec = 8;
        cmpt[c].sgnd = 0;
    }
    opj_image_t *img = opj_image_create(comps, cmpt, OPJ_CLRSPC_SRGB);
    if (!img) { free(rows); return CAT_ERR_NOMEM; }
    img->x0 = 0; img->y0 = 0; img->x1 = w; img->y1 = h;
    for (int c = 0; c < comps; c++) {
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                img->comps[c].data[(h - 1 - y) * w + x] =
                    rows[y * row_size + x * comps + c];
            }
        }
    }
    free(rows);
    char tmp[] = "/tmp/cat_jp2_out_XXXXXX";
    int fd = mkstemp(tmp);
    if (fd < 0) { opj_image_destroy(img); return CAT_ERR_IO; }
    close(fd);
    /* OPJ_FALSE = write stream */
    opj_stream_t *st = opj_stream_create_default_file_stream(tmp, OPJ_FALSE);
    if (!st) { opj_image_destroy(img); unlink(tmp); return CAT_ERR_OPENJPEG; }
    opj_codec_t *codec = opj_create_compress(OPJ_CODEC_JP2);
    if (!codec) { opj_stream_destroy(st); opj_image_destroy(img); unlink(tmp); return CAT_ERR_OPENJPEG; }
    if (!opj_setup_encoder(codec, &params, img)) {
        opj_destroy_codec(codec); opj_stream_destroy(st); opj_image_destroy(img); unlink(tmp);
        return CAT_ERR_OPENJPEG;
    }
    if (!opj_start_compress(codec, img, st) || !opj_encode(codec, st) || !opj_end_compress(codec, st)) {
        opj_destroy_codec(codec); opj_stream_destroy(st); opj_image_destroy(img); unlink(tmp);
        return CAT_ERR_OPENJPEG;
    }
    opj_destroy_codec(codec);
    opj_stream_destroy(st);
    opj_image_destroy(img);
    FILE *rf = fopen(tmp, "rb");
    if (!rf) { unlink(tmp); return CAT_ERR_IO; }
    fseek(rf, 0, SEEK_END);
    long sz = ftell(rf);
    fseek(rf, 0, SEEK_SET);
    uint8_t *buf = (uint8_t *)malloc((size_t)sz);
    if (!buf) { fclose(rf); unlink(tmp); return CAT_ERR_NOMEM; }
    fread(buf, 1, (size_t)sz, rf);
    fclose(rf);
    unlink(tmp);
    *out = buf;
    *out_len = (size_t)sz;
    return CAT_OK;
}

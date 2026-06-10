/* Top-level dispatch: read a .qcf file, route payload to a backend,
 * build a cat_file array. */
#include "cat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Forward declarations from backends */
cat_status cat_decode_raw(const void *p, size_t n,
                          const uint8_t *ext, size_t ext_size,
                          cat_file ***out, size_t *cnt);
cat_status cat_decode_zip(const void *p, size_t n,
                          cat_file ***out, size_t *cnt);
cat_status cat_decode_ole2(const void *p, size_t n,
                           cat_file ***out, size_t *cnt);
cat_status cat_encode_zip(const cat_file *files, size_t count,
                          uint8_t **out, size_t *out_len);
cat_status cat_decode_jp2(const void *p, size_t n, const char *out_path);

void cat_files_free(cat_file **files, size_t count) {
    if (!files) return;
    for (size_t i = 0; i < count; i++) {
        free(files[i]->name);
        free(files[i]->data);
        free(files[i]);
    }
    free(files);
}

cat_status cat_decode(const cat_header *h, const void *payload, size_t plen,
                      cat_file ***out_files, size_t *out_count) {
    cat_inner kind = cat_inner_detect(payload, plen);
    switch (kind) {
        case CAT_INNER_RAW:
            return cat_decode_raw(payload, plen,
                                  h->ext_header, h->ext_hdr_size,
                                  out_files, out_count);
        case CAT_INNER_ZIP:
            return cat_decode_zip(payload, plen, out_files, out_count);
        case CAT_INNER_OLE2:
            return cat_decode_ole2(payload, plen, out_files, out_count);
        case CAT_INNER_JP2: {
            char tmp[] = "/tmp/cat_jp2_out_XXXXXX.bmp";
            int fd = mkstemps(tmp, 4);
            if (fd < 0) return CAT_ERR_IO;
            close(fd);
            cat_status r = cat_decode_jp2(payload, plen, tmp);
            if (r != CAT_OK) return r;
            FILE *f = fopen(tmp, "rb");
            if (!f) { unlink(tmp); return CAT_ERR_IO; }
            fseek(f, 0, SEEK_END);
            long sz = ftell(f);
            fseek(f, 0, SEEK_SET);
            uint8_t *data = (uint8_t *)malloc(sz);
            if (!data) { fclose(f); unlink(tmp); return CAT_ERR_NOMEM; }
            fread(data, 1, sz, f);
            fclose(f);
            unlink(tmp);
            cat_file **arr = (cat_file **)calloc(2, sizeof(cat_file *));
            if (!arr) { free(data); return CAT_ERR_NOMEM; }
            cat_file *f0 = (cat_file *)calloc(1, sizeof(cat_file));
            if (!f0) { free(arr); free(data); return CAT_ERR_NOMEM; }
            f0->name = strdup("image.bmp");
            if (!f0->name) { free(f0); free(arr); free(data); return CAT_ERR_NOMEM; }
            f0->data = data;
            f0->size = (size_t)sz;
            arr[0] = f0;
            *out_files = arr;
            *out_count = 1;
            return CAT_OK;
        }
        default:
            return CAT_ERR_FORMAT;
    }
}

cat_status cat_read_file(const char *path,
                         cat_header *out_header,
                         cat_file ***out_files, size_t *out_count) {
    FILE *f = fopen(path, "rb");
    if (!f) return CAT_ERR_IO;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = (uint8_t *)malloc(sz);
    if (!buf) { fclose(f); return CAT_ERR_NOMEM; }
    if (fread(buf, 1, sz, f) != (size_t)sz) { free(buf); fclose(f); return CAT_ERR_IO; }
    fclose(f);
    if (cat_detect(buf, sz) == CAT_TYPE_ZIP_PASSTHROUGH) {
        memset(out_header, 0, sizeof(*out_header));
        out_header->magic = CAT_MAGIC_ZIP;
        cat_status r = cat_decode_zip(buf, sz, out_files, out_count);
        free(buf);
        return r;
    }
    cat_status r = cat_header_parse(buf, sz, out_header);
    if (r != CAT_OK) { free(buf); return r; }
    if (out_header->ext_hdr_size > 0) {
        if ((size_t)CAT_HDR_SIZE + out_header->ext_hdr_size > (size_t)sz) {
            cat_header_free(out_header); free(buf); return CAT_ERR_TRUNCATED;
        }
        out_header->ext_header = (uint8_t *)malloc(out_header->ext_hdr_size);
        if (!out_header->ext_header) { cat_header_free(out_header); free(buf); return CAT_ERR_NOMEM; }
        memcpy(out_header->ext_header, buf + CAT_HDR_SIZE, out_header->ext_hdr_size);
    }
    const uint8_t *payload = buf + CAT_HDR_SIZE + out_header->ext_hdr_size;
    size_t plen = sz - CAT_HDR_SIZE - out_header->ext_hdr_size;
    r = cat_decode(out_header, payload, plen, out_files, out_count);
    free(buf);
    return r;
}

cat_status cat_write_file(const char *path,
                          const char **names, size_t name_count,
                          const uint8_t *ext_header, size_t ext_size) {
    if (ext_size > 255) return CAT_ERR_OVERFLOW;
    if (!names || name_count == 0) return CAT_ERR_FORMAT;
    cat_file *files = (cat_file *)calloc(name_count, sizeof(cat_file));
    if (!files) return CAT_ERR_NOMEM;
    for (size_t i = 0; i < name_count; i++) {
        files[i].name = strdup(names[i]);
        FILE *f = fopen(names[i], "rb");
        if (!f) { for (size_t k = 0; k < i; k++) { free(files[k].name); free(files[k].data); } free(files); return CAT_ERR_IO; }
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        files[i].data = (uint8_t *)malloc(sz);
        files[i].size = sz;
        fread(files[i].data, 1, sz, f);
        fclose(f);
    }
    uint8_t *payload = NULL;
    size_t plen = 0;
    cat_status r;
    if (name_count == 1) {
        payload = (uint8_t *)malloc(files[0].size ? files[0].size : 1);
        if (!payload) { for (size_t k = 0; k < name_count; k++) { free(files[k].name); free(files[k].data); } free(files); return CAT_ERR_NOMEM; }
        memcpy(payload, files[0].data, files[0].size);
        plen = files[0].size;
        r = CAT_OK;
    } else {
        r = cat_encode_zip(files, name_count, &payload, &plen);
    }
    if (r != CAT_OK) { for (size_t k = 0; k < name_count; k++) { free(files[k].name); free(files[k].data); } free(files); if (payload) free(payload); return r; }
    cat_header h;
    memset(&h, 0, sizeof(h));
    h.magic = CAT_MAGIC_QCF;
    h.ext_hdr_size = (uint8_t)ext_size;
    if (ext_size > 0) {
        h.ext_header = (uint8_t *)malloc(ext_size);
        if (!h.ext_header) { free(payload); for (size_t k = 0; k < name_count; k++) { free(files[k].name); free(files[k].data); } free(files); return CAT_ERR_NOMEM; }
        memcpy(h.ext_header, ext_header, ext_size);
    }
    FILE *f = fopen(path, "wb");
    if (!f) { free(payload); cat_header_free(&h); for (size_t k = 0; k < name_count; k++) { free(files[k].name); free(files[k].data); } free(files); return CAT_ERR_IO; }
    uint8_t hdr[CAT_HDR_SIZE];
    cat_header_serialize(&h, hdr);
    fwrite(hdr, 1, CAT_HDR_SIZE, f);
    if (h.ext_hdr_size) fwrite(h.ext_header, 1, h.ext_hdr_size, f);
    fwrite(payload, 1, plen, f);
    fclose(f);
    cat_header_free(&h);
    free(payload);
    for (size_t k = 0; k < name_count; k++) { free(files[k].name); free(files[k].data); }
    free(files);
    return CAT_OK;
}

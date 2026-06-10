/* Raw passthrough backend. */
#include "cat.h"
#include <string.h>
#include <stdlib.h>

cat_status cat_decode_raw(const void *payload, size_t plen,
                          const uint8_t *ext_header, size_t ext_size,
                          cat_file ***out_files, size_t *out_count) {
    cat_file **arr = (cat_file **)calloc(1, sizeof(cat_file *));
    if (!arr) return CAT_ERR_NOMEM;
    cat_file *f = (cat_file *)calloc(1, sizeof(cat_file));
    if (!f) { free(arr); return CAT_ERR_NOMEM; }
    f->data = (uint8_t *)malloc(plen ? plen : 1);
    if (!f->data) { free(f); free(arr); return CAT_ERR_NOMEM; }
    if (plen) memcpy(f->data, payload, plen);
    f->size = plen;
    /* Name: from ext_header if printable, else "payload.bin" */
    f->name = (char *)malloc(64);
    if (!f->name) { free(f->data); free(f); free(arr); return CAT_ERR_NOMEM; }
    if (ext_header && ext_size > 0) {
        size_t n = ext_size < 60 ? ext_size : 60;
        int ok = 1;
        for (size_t i = 0; i < n; i++) {
            if (ext_header[i] < 0x20 || ext_header[i] > 0x7e) { ok = 0; break; }
        }
        if (ok) {
            memcpy(f->name, ext_header, n);
            f->name[n] = 0;
        } else {
            strcpy(f->name, "payload.bin");
        }
    } else {
        strcpy(f->name, "payload.bin");
    }
    arr[0] = f;
    *out_files = arr;
    *out_count = 1;
    return CAT_OK;
}

cat_status cat_encode_raw(const cat_file *files, size_t count, uint8_t **out, size_t *out_len) {
    if (count == 0) { *out = NULL; *out_len = 0; return CAT_OK; }
    if (count > 1) return CAT_ERR_UNSUPPORTED;
    uint8_t *buf = (uint8_t *)malloc(files[0].size ? files[0].size : 1);
    if (!buf) return CAT_ERR_NOMEM;
    if (files[0].size) memcpy(buf, files[0].data, files[0].size);
    *out = buf;
    *out_len = files[0].size;
    return CAT_OK;
}

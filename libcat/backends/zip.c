/* Hand-rolled minimal PKZIP reader/writer. Supports:
 *   - STORED (method 0)
 *   - DEFLATE (method 8, via zlib)
 */
#include "cat.h"
#include <zlib.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define LE16(p) ((uint16_t)((p)[0] | ((uint16_t)(p)[1] << 8)))
#define LE32(p) ((uint32_t)((p)[0] | ((uint32_t)(p)[1] << 8) | \
                ((uint32_t)(p)[2] << 16) | ((uint32_t)(p)[3] << 24)))
#define PUT16(p, v) do { (p)[0] = (v) & 0xff; (p)[1] = ((v)>>8) & 0xff; } while (0)
#define PUT32(p, v) do { (p)[0]=(v)&0xff; (p)[1]=((v)>>8)&0xff; (p)[2]=((v)>>16)&0xff; (p)[3]=((v)>>24)&0xff; } while (0)

static void dos_datetime(uint16_t *d, uint16_t *t) {
    *d = 0x0021; *t = 0;
}

static int find_eocd(const uint8_t *buf, size_t len,
                     size_t *eocd_off, uint16_t *nfiles) {
    if (len < 22) return 0;
    for (size_t i = len - 22; (long long)i >= 0 && (size_t)(i + 22) <= len; i--) {
        if (LE32(buf + i) == 0x06054b50) {
            *eocd_off = i;
            *nfiles = LE16(buf + i + 10);
            return 1;
        }
    }
    return 0;
}

cat_status cat_decode_zip(const void *payload, size_t plen,
                          cat_file ***out_files, size_t *out_count) {
    (void)plen;  /* silence unused warning */
    const uint8_t *buf = (const uint8_t *)payload;
    size_t eocd_off = 0;
    uint16_t nfiles = 0;
    if (!find_eocd(buf, plen, &eocd_off, &nfiles)) {
        return CAT_ERR_FORMAT;
    }
    size_t cd_off = LE32(buf + eocd_off + 16);
    cat_file **arr = (cat_file **)calloc(nfiles + 1, sizeof(cat_file *));
    if (!arr) return CAT_ERR_NOMEM;
    size_t got = 0;
    size_t p = cd_off;
    for (uint16_t i = 0; i < nfiles; i++) {
        if (p + 46 > plen) { cat_files_free(arr, got); return CAT_ERR_FORMAT; }
        if (LE32(buf + p) != 0x02014b50) { cat_files_free(arr, got); return CAT_ERR_FORMAT; }
        uint16_t method = LE16(buf + p + 10);
        uint32_t csize  = LE32(buf + p + 20);
        uint32_t usize  = LE32(buf + p + 24);
        uint16_t nlen   = LE16(buf + p + 28);
        uint16_t elen   = LE16(buf + p + 30);
        uint16_t clen   = LE16(buf + p + 32);
        uint32_t lh_off = LE32(buf + p + 42);
        if (p + 46 + nlen + elen + clen > plen) { cat_files_free(arr, got); return CAT_ERR_FORMAT; }
        const char *name = (const char *)buf + p + 46;
        if (lh_off + 30 + nlen > plen) { cat_files_free(arr, got); return CAT_ERR_FORMAT; }
        if (LE32(buf + lh_off) != 0x04034b50) { cat_files_free(arr, got); return CAT_ERR_FORMAT; }
        uint16_t lnlen   = LE16(buf + lh_off + 26);
        uint16_t lelen   = LE16(buf + lh_off + 28);
        const uint8_t *data = buf + lh_off + 30 + lnlen + lelen;
        if ((size_t)(data - buf) + csize > plen) { cat_files_free(arr, got); return CAT_ERR_FORMAT; }
        cat_file *f = (cat_file *)calloc(1, sizeof(cat_file));
        if (!f) { cat_files_free(arr, got); return CAT_ERR_NOMEM; }
        f->name = (char *)malloc(nlen + 1);
        if (!f->name) { free(f); cat_files_free(arr, got); return CAT_ERR_NOMEM; }
        memcpy(f->name, name, nlen); f->name[nlen] = 0;
        f->size = usize;
        f->data = (uint8_t *)malloc(usize ? usize : 1);
        if (!f->data) { free(f->name); free(f); cat_files_free(arr, got); return CAT_ERR_NOMEM; }
        if (method == 0) {
            if (csize != usize) { free(f->name); free(f->data); free(f); cat_files_free(arr, got); return CAT_ERR_FORMAT; }
            if (usize) memcpy(f->data, data, usize);
        } else if (method == 8) {
            z_stream zs; memset(&zs, 0, sizeof(zs));
            if (inflateInit2(&zs, -15) != Z_OK) { free(f->name); free(f->data); free(f); cat_files_free(arr, got); return CAT_ERR_ZLIB; }
            zs.next_in = (Bytef *)data; zs.avail_in = csize;
            zs.next_out = f->data;   zs.avail_out = usize;
            int rv = inflate(&zs, Z_FINISH);
            inflateEnd(&zs);
            if (rv != Z_STREAM_END) { free(f->name); free(f->data); free(f); cat_files_free(arr, got); return CAT_ERR_ZLIB; }
        } else {
            free(f->name); free(f->data); free(f);
            cat_files_free(arr, got); return CAT_ERR_UNSUPPORTED;
        }
        arr[got++] = f;
        p += 46 + nlen + elen + clen;
    }
    arr[got] = NULL;
    *out_files = arr;
    *out_count = got;
    return CAT_OK;
}

cat_status cat_encode_zip(const cat_file *files, size_t count,
                          uint8_t **out, size_t *out_len) {
    /* Compute total */
    size_t total = 0;
    for (size_t i = 0; i < count; i++) {
        total += 30 + strlen(files[i].name) + files[i].size;
    }
    size_t cd_off = total;
    size_t cd_size = 0;
    for (size_t i = 0; i < count; i++) {
        cd_size += 46 + strlen(files[i].name);
    }
    total += cd_size + 22;

    uint8_t *buf = (uint8_t *)malloc(total);
    if (!buf) return CAT_ERR_NOMEM;
    uint32_t crc_table[256];
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) c = (c & 1) ? 0xedb88320u ^ (c >> 1) : c >> 1;
        crc_table[i] = c;
    }
    size_t p = 0;
    for (size_t i = 0; i < count; i++) {
        size_t nlen = strlen(files[i].name);
        uint32_t crc = 0xffffffffu;
        for (size_t k = 0; k < files[i].size; k++) {
            crc = crc_table[(crc ^ files[i].data[k]) & 0xff] ^ (crc >> 8);
        }
        crc ^= 0xffffffffu;
        uint16_t d, t; dos_datetime(&d, &t);
        PUT32(buf + p, 0x04034b50); p += 4;
        PUT16(buf + p, 20); p += 2;
        PUT16(buf + p, 0);  p += 2;
        PUT16(buf + p, 0);  p += 2;
        PUT16(buf + p, t);  p += 2;
        PUT16(buf + p, d);  p += 2;
        PUT32(buf + p, crc); p += 4;
        PUT32(buf + p, (uint32_t)files[i].size); p += 4;
        PUT32(buf + p, (uint32_t)files[i].size); p += 4;
        PUT16(buf + p, (uint16_t)nlen); p += 2;
        PUT16(buf + p, 0);  p += 2;
        memcpy(buf + p, files[i].name, nlen); p += nlen;
        if (files[i].size) memcpy(buf + p, files[i].data, files[i].size);
        p += files[i].size;
    }
    for (size_t i = 0; i < count; i++) {
        size_t nlen = strlen(files[i].name);
        uint32_t crc = 0xffffffffu;
        for (size_t k = 0; k < files[i].size; k++) {
            crc = crc_table[(crc ^ files[i].data[k]) & 0xff] ^ (crc >> 8);
        }
        crc ^= 0xffffffffu;
        uint16_t d, t; dos_datetime(&d, &t);
        PUT32(buf + p, 0x02014b50); p += 4;
        PUT16(buf + p, 20); p += 2;
        PUT16(buf + p, 20); p += 2;
        PUT16(buf + p, 0);  p += 2;
        PUT16(buf + p, 0);  p += 2;
        PUT16(buf + p, t);  p += 2;
        PUT16(buf + p, d);  p += 2;
        PUT32(buf + p, crc); p += 4;
        PUT32(buf + p, (uint32_t)files[i].size); p += 4;
        PUT32(buf + p, (uint32_t)files[i].size); p += 4;
        PUT16(buf + p, (uint16_t)nlen); p += 2;
        PUT16(buf + p, 0);  p += 2;
        PUT16(buf + p, 0);  p += 2;
        PUT16(buf + p, 0);  p += 2;
        PUT16(buf + p, 0);  p += 2;
        PUT32(buf + p, 0);  p += 4;
        size_t off = 0;
        for (size_t j = 0; j < i; j++) off += 30 + strlen(files[j].name) + files[j].size;
        PUT32(buf + p, (uint32_t)off); p += 4;
        memcpy(buf + p, files[i].name, nlen); p += nlen;
    }
    PUT32(buf + p, 0x06054b50); p += 4;
    PUT16(buf + p, 0);  p += 2;
    PUT16(buf + p, 0);  p += 2;
    PUT16(buf + p, (uint16_t)count); p += 2;
    PUT16(buf + p, (uint16_t)count); p += 2;
    PUT32(buf + p, (uint32_t)cd_size); p += 4;
    PUT32(buf + p, (uint32_t)cd_off); p += 4;
    PUT16(buf + p, 0);  p += 2;
    *out = buf;
    *out_len = total;
    return CAT_OK;
}

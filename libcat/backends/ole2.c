/* OLE2 / MS-CFB (Compound File Binary) reader.
 *
 * Implements the bare minimum to enumerate streams in a .doc/.xls/.ppt
 * file (which is what CAT processed for Office documents).
 *
 * We do NOT decompress MS-OFFCRYP-compressed sectors (the deflate
 * variant with custom Huffman tables). For now we read the directory
 * structure and emit the raw, unprocessed stream bytes.
 *
 * Reference: [MS-CFB] Compound File Binary File Format.
 */
#include "cat.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <endian.h>

/* CFB Header layout (512 bytes total) */
typedef struct {
    uint8_t  sig[8];
    uint8_t  clsid[16];
    uint16_t minor;
    uint16_t major;
    uint16_t byte_order;     /* 0xFFFE = LE */
    uint16_t sector_shift;
    uint16_t mini_sector_shift;
    uint8_t  reserved[6];
    uint32_t num_dir_sectors;
    uint32_t num_fat_sectors;
    uint32_t first_dir_sector;
    uint32_t transaction_sig;
    uint32_t mini_stream_cutoff;
    uint32_t first_minifat_sector;
    uint32_t num_minifat_sectors;
    uint32_t first_difat_sector;
    uint32_t num_difat_sectors;
    uint32_t difat[109];
} __attribute__((packed)) cfb_header_t;

/* Directory entry (128 bytes) */
typedef struct {
    uint16_t name_len;
    uint16_t name[32];   /* UTF-16LE, max 32 chars */
    uint8_t  type;       /* 1=storage, 2=stream, 5=root */
    uint8_t  color;      /* 0=red, 1=black */
    uint32_t left_sib;
    uint32_t right_sib;
    uint32_t child;
    uint8_t  clsid[16];
    uint32_t state;
    uint64_t ctime;
    uint64_t mtime;
    uint32_t start_sector;
    uint64_t size;
} __attribute__((packed)) dir_entry_t;

#define SECTOR_SIZE(b)  (1u << (b)->sector_shift)
#define MINI_SECTOR_SIZE(b)  (1u << (b)->mini_sector_shift)

/* Sector N: byte offset = (N+1) * sector_size, capped at 512+ */
static size_t sector_offset(const cfb_header_t *h, uint32_t sec) {
    size_t ss = SECTOR_SIZE(h);
    return (size_t)(sec + 1) * ss;
}

/* Read N bytes at sector S of the file, applying FAT chain */
static int read_sector_chain(const uint8_t *buf, size_t len,
                             const cfb_header_t *h,
                             const uint32_t *fat,
                             uint32_t start_sector,
                             size_t want_bytes,
                             uint8_t *out) {
    size_t ss = SECTOR_SIZE(h);
    uint32_t cur = start_sector;
    size_t got = 0;
    while (cur < 0xFFFFFFFE && got < want_bytes) {
        if (cur >= (len - 1) / ss - 1) return -1;
        size_t off = (size_t)(cur + 1) * ss;
        size_t take = (want_bytes - got < ss) ? want_bytes - got : ss;
        if (off + take > len) return -1;
        memcpy(out + got, buf + off, take);
        got += take;
        cur = fat[cur];
    }
    return (int)got;
}

/* Walk the red-black tree of children of `start_id` and emit stream
 * paths to `files`. */
static int walk_dir(const uint8_t *buf, size_t len,
                    const cfb_header_t *h,
                    const uint32_t *fat,
                    uint32_t start_id,
                    char *name_buf, size_t name_len,
                    cat_file **arr, size_t *got) {
    if (start_id == 0xFFFFFFFF) return 0;
    if (start_id * 128 + 128 > len) return -1;
    const dir_entry_t *de = (const dir_entry_t *)(buf + start_id * 128);
    if (de->type == 2) {
        /* Stream: copy name (LE UTF-16 -> ASCII best-effort) */
        char *p = name_buf + name_len;
        *p = 0;
        for (int k = (int)de->name_len / 2 - 1; k >= 0; k--) {
            uint16_t c = de->name[k];
            if (c < 128) {
                *--p = (char)c;
            } else {
                *--p = '?';
            }
        }
        size_t p_len = strlen(p);
        if (p_len > 0) {
            /* Allocate cat_file */
            cat_file *f = (cat_file *)calloc(1, sizeof(cat_file));
            if (!f) return -1;
            f->name = strdup(p);
            if (!f->name) { free(f); return -1; }
            f->size = de->size;
            f->data = (uint8_t *)malloc(de->size ? de->size : 1);
            if (!f->data) { free(f->name); free(f); return -1; }
            if (de->size > 0) {
                int r = read_sector_chain(buf, len, h, fat,
                                          de->start_sector, de->size, f->data);
                if (r < 0) { free(f->name); free(f->data); free(f); return -1; }
            }
            arr[(*got)++] = f;
        }
    } else if (de->type == 1 || de->type == 5) {
        /* Storage/Root: recurse into children */
        uint32_t child = de->child;
        while (child != 0xFFFFFFFF) {
            if (walk_dir(buf, len, h, fat, child, name_buf,
                         name_len + (de->type == 5 ? 0 :
                              (de->type == 1 ? 0 : 0)), arr, got) < 0) {
                return -1;
            }
            const dir_entry_t *cd = (const dir_entry_t *)(buf + child * 128);
            child = cd->right_sib;
        }
    }
    return 0;
}

cat_status cat_decode_ole2(const void *payload, size_t plen,
                           cat_file ***out_files, size_t *out_count) {
    const uint8_t *buf = (const uint8_t *)payload;
    if (plen < 512) return CAT_ERR_FORMAT;
    cfb_header_t h;
    memcpy(&h, buf, sizeof(h));
    if (memcmp(h.sig, "\xd0\xcf\x11\xe0\xa1\xb1\x1a\xe1", 8) != 0) {
        return CAT_ERR_FORMAT;
    }
    /* Sector size: 1 << sector_shift, default 9 = 512 */
    if (h.sector_shift < 7 || h.sector_shift > 16) return CAT_ERR_FORMAT;
    size_t ss = SECTOR_SIZE(&h);
    if (h.first_dir_sector == 0xFFFFFFFE) return CAT_ERR_TRUNCATED;

    /* Read FAT */
    size_t fat_size = h.num_fat_sectors * ss;
    if (fat_size > plen) return CAT_ERR_FORMAT;
    uint32_t *fat = (uint32_t *)malloc(fat_size);
    if (!fat) return CAT_ERR_NOMEM;
    for (uint32_t i = 0; i < h.num_fat_sectors; i++) {
        size_t off = (h.difat[i] + 1) * ss;
        if (off + ss > plen) { free(fat); return CAT_ERR_FORMAT; }
        memcpy((uint8_t *)fat + i * ss, buf + off, ss);
    }
    /* Read directory chain */
    size_t dir_size = ss;  /* first dir sector is 1 sector; chain may be longer */
    if (h.num_dir_sectors > 0) dir_size = h.num_dir_sectors * ss;
    uint8_t *dir_buf = (uint8_t *)malloc(dir_size);
    if (!dir_buf) { free(fat); return CAT_ERR_NOMEM; }
    int r = read_sector_chain(buf, plen, &h, fat, h.first_dir_sector, dir_size, dir_buf);
    if (r < 0) { free(fat); free(dir_buf); return CAT_ERR_FORMAT; }
    /* Allocate up to 256 streams */
    cat_file **arr = (cat_file **)calloc(256, sizeof(cat_file *));
    if (!arr) { free(fat); free(dir_buf); return CAT_ERR_NOMEM; }
    size_t got = 0;
    char name_buf[1024] = "";
    /* Root entry is at index 0 */
    if (walk_dir(dir_buf, dir_size, &h, fat, 0, name_buf, 0, arr, &got) < 0) {
        cat_files_free(arr, got);
        free(fat); free(dir_buf);
        return CAT_ERR_FORMAT;
    }
    arr[got] = NULL;
    free(fat); free(dir_buf);
    *out_files = arr;
    *out_count = got;
    return CAT_OK;
}

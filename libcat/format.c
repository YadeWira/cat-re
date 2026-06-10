#include <stdlib.h>
/* .qcf format: parser and serializer. */
#include "cat.h"
#include <string.h>

cat_type cat_detect(const void *buf, size_t len) {
    if (len < 4) return CAT_TYPE_UNKNOWN;
    uint32_t m;
    memcpy(&m, buf, 4);
    if (m == CAT_MAGIC_QCF) return CAT_TYPE_QCF;
    if (m == CAT_MAGIC_QCM) return CAT_TYPE_QCM;
    if (m == CAT_MAGIC_ZIP) return CAT_TYPE_ZIP_PASSTHROUGH;
    return CAT_TYPE_UNKNOWN;
}

cat_status cat_header_parse(const uint8_t *buf, size_t len, cat_header *out) {
    if (len < CAT_HDR_SIZE) return CAT_ERR_TRUNCATED;
    uint32_t m;
    memcpy(&m, buf, 4);
    if (m != CAT_MAGIC_QCF && m != CAT_MAGIC_QCM && m != CAT_MAGIC_ZIP) {
        return CAT_ERR_BADMAGIC;
    }
    out->magic = m;
    memcpy(out->fields, buf + 4, CAT_FIELDS_SIZE);
    out->ext_hdr_size = buf[CAT_EXT_OFFSET];
    out->ext_header = NULL;
    return CAT_OK;
}

void cat_header_free(cat_header *h) {
    if (h->ext_header) {
        free(h->ext_header);
        h->ext_header = NULL;
    }
}

cat_status cat_header_serialize(const cat_header *h, uint8_t *out) {
    memcpy(out, &h->magic, 4);
    memcpy(out + 4, h->fields, CAT_FIELDS_SIZE);
    out[CAT_EXT_OFFSET] = h->ext_hdr_size;
    return CAT_OK;
}

cat_inner cat_inner_detect(const void *payload, size_t len) {
    if (len < 4) return CAT_INNER_RAW;
    const uint8_t *p = (const uint8_t *)payload;
    /* ZIP local header: PK\x03\x04 */
    if (p[0]=='P' && p[1]=='K' && p[2]==3 && p[3]==4) return CAT_INNER_ZIP;
    /* JP2 signature box: 00 00 00 0c 6a 50 20 20 0d 0a 87 0a */
    static const uint8_t jp2_box[12] = {0,0,0,0x0c,0x6a,0x50,0x20,0x20,0x0d,0x0a,0x87,0x0a};
    if (len >= 12 && memcmp(p, jp2_box, 12) == 0) return CAT_INNER_JP2;
    /* J2K codestream: FF 4F ... */
    if (p[0] == 0xff && p[1] == 0x4f) return CAT_INNER_JP2;
    /* OLE2 compound file: D0 CF 11 E0 A1 B1 1A E1 */
    static const uint8_t ole2_magic[8] = {0xd0,0xcf,0x11,0xe0,0xa1,0xb1,0x1a,0xe1};
    if (len >= 8 && memcmp(p, ole2_magic, 8) == 0) return CAT_INNER_OLE2;
    return CAT_INNER_RAW;
}

#include <stdlib.h>
#include "cat.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>

static void test_magic(void) {
    assert(CAT_MAGIC_QCF == 0x01464351u);
    assert(CAT_MAGIC_QCM == 0x014D4351u);
    assert(CAT_MAGIC_ZIP == 0x04034b50u);
    printf("test_magic: ok\n");
}

static void test_detect(void) {
    uint8_t buf_qcf[4] = {'Q','C','F',0x01};
    uint8_t buf_qcm[4] = {'Q','C','M',0x01};
    uint8_t buf_zip[4] = {'P','K',3,4};
    uint8_t buf_unk[4] = {'X','X','X','X'};
    assert(cat_detect(buf_qcf, 4) == CAT_TYPE_QCF);
    assert(cat_detect(buf_qcm, 4) == CAT_TYPE_QCM);
    assert(cat_detect(buf_zip, 4) == CAT_TYPE_ZIP_PASSTHROUGH);
    assert(cat_detect(buf_unk, 4) == CAT_TYPE_UNKNOWN);
    assert(cat_detect(buf_qcf, 2) == CAT_TYPE_UNKNOWN);
    printf("test_detect: ok\n");
}

static void test_header_parse(void) {
    uint8_t buf[CAT_HDR_SIZE];
    memcpy(buf, "QCF\x01", 4);
    memset(buf + 4, 0, CAT_FIELDS_SIZE);
    buf[CAT_EXT_OFFSET] = 5;
    cat_header h;
    assert(cat_header_parse(buf, sizeof(buf), &h) == CAT_OK);
    assert(h.magic == CAT_MAGIC_QCF);
    assert(h.ext_hdr_size == 5);
    assert(h.ext_header == NULL);
    /* try bad magic */
    memcpy(buf, "BAD!", 4);
    assert(cat_header_parse(buf, sizeof(buf), &h) == CAT_ERR_BADMAGIC);
    /* too short */
    assert(cat_header_parse(buf, 4, &h) == CAT_ERR_TRUNCATED);
    printf("test_header_parse: ok\n");
}

static void test_header_serialize(void) {
    cat_header h; memset(&h, 0, sizeof(h));
    h.magic = CAT_MAGIC_QCF;
    h.ext_hdr_size = 3;
    h.ext_header = (uint8_t *)"abc";
    h.fields[0] = 0x42;
    uint8_t out[CAT_HDR_SIZE];
    assert(cat_header_serialize(&h, out) == CAT_OK);
    assert(memcmp(out, "QCF\x01", 4) == 0);
    assert(out[4] == 0x42);
    assert(out[CAT_EXT_OFFSET] == 3);
    /* round-trip */
    cat_header h2;
    assert(cat_header_parse(out, sizeof(out), &h2) == CAT_OK);
    assert(h2.magic == CAT_MAGIC_QCF);
    assert(h2.ext_hdr_size == 3);
    assert(h2.fields[0] == 0x42);
    printf("test_header_serialize: ok\n");
}

static void test_inner_detect(void) {
    uint8_t jp2[12] = {0,0,0,0x0c,0x6a,0x50,0x20,0x20,0x0d,0x0a,0x87,0x0a};
    uint8_t j2k[4] = {0xff, 0x4f, 0xff, 0x51};
    uint8_t ole2[8] = {0xd0,0xcf,0x11,0xe0,0xa1,0xb1,0x1a,0xe1};
    uint8_t zip[4] = {'P','K',3,4};
    uint8_t raw[4] = {1,2,3,4};
    assert(cat_inner_detect(jp2, 12) == CAT_INNER_JP2);
    assert(cat_inner_detect(j2k, 4) == CAT_INNER_JP2);
    assert(cat_inner_detect(ole2, 8) == CAT_INNER_OLE2);
    assert(cat_inner_detect(zip, 4) == CAT_INNER_ZIP);
    assert(cat_inner_detect(raw, 4) == CAT_INNER_RAW);
    printf("test_inner_detect: ok\n");
}

int test_format_main(void) {
    test_magic();
    test_detect();
    test_header_parse();
    test_header_serialize();
    test_inner_detect();
    printf("\nALL TESTS PASSED\n");
    return 0;
}

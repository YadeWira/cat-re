/* libcat: read/write Choshuku (CAT) .qcf archives on POSIX systems.
 *
 * This is a reimplementation of the format reverse-engineered from
 * SOURCENEXT's Choshuku Professional v1.0.2. See docs/RE_notes.md
 * for the full RE.
 *
 * Build deps: zlib, libopenjp2 (for JPEG2000).
 *
 * License: MIT
 */
#ifndef CAT_H
#define CAT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Magic numbers (little-endian 32-bit) */
#define CAT_MAGIC_QCF  0x01464351u   /* 'Q','C','F',0x01 */
#define CAT_MAGIC_QCM  0x014D4351u   /* 'Q','C','M',0x01 */
#define CAT_MAGIC_ZIP  0x04034B50u   /* 'P','K',3,4 (ZIP local header) */

/* Header layout */
#define CAT_HDR_SIZE       28          /* fixed-size part (0x1C) */
#define CAT_FIELDS_SIZE    23          /* +0x04..+0x1A, opaque */
#define CAT_EXT_OFFSET     0x1B        /* byte that holds ext_hdr_size */

/* Result codes */
typedef enum {
    CAT_OK                  =  0,
    CAT_ERR_IO              = -1,   /* file/memory I/O failure */
    CAT_ERR_BADMAGIC        = -2,   /* not a QCF/QCM/ZIP file */
    CAT_ERR_TRUNCATED       = -3,   /* unexpected end of data */
    CAT_ERR_UNSUPPORTED     = -4,   /* feature not implemented */
    CAT_ERR_NOMEM           = -5,
    CAT_ERR_ZLIB            = -6,   /* zlib error */
    CAT_ERR_OPENJPEG        = -7,   /* libopenjp2 error */
    CAT_ERR_FORMAT          = -8,   /* malformed inner format */
    CAT_ERR_OVERFLOW        = -9,   /* ext_hdr_size > 255 */
} cat_status;

/* Detected file type */
typedef enum {
    CAT_TYPE_UNKNOWN = 0,
    CAT_TYPE_QCF,
    CAT_TYPE_QCM,
    CAT_TYPE_ZIP_PASSTHROUGH,
} cat_type;

/* Detected inner format */
typedef enum {
    CAT_INNER_UNKNOWN = 0,
    CAT_INNER_RAW,
    CAT_INNER_ZIP,
    CAT_INNER_JP2,
    CAT_INNER_OLE2,
} cat_inner;

/* A single file recovered from a .qcf payload */
typedef struct {
    char    *name;      /* logical name (allocated, caller frees) */
    uint8_t *data;      /* raw file bytes (allocated, caller frees) */
    size_t   size;      /* number of bytes */
} cat_file;

/* The fixed 28-byte header. The 23 middle bytes are opaque and may
 * carry type-specific metadata. */
typedef struct {
    uint32_t magic;             /* CAT_MAGIC_QCF / CAT_MAGIC_QCM / CAT_MAGIC_ZIP */
    uint8_t  fields[CAT_FIELDS_SIZE];
    uint8_t  ext_hdr_size;      /* 0..255 */
    uint8_t *ext_header;        /* ext_hdr_size bytes, or NULL */
} cat_header;

/* Detect a buffer's type by magic */
cat_type cat_detect(const void *buf, size_t len);

/* Parse a 28-byte fixed header from a buffer. ext_header is NOT
 * included; call cat_header_read_ext() to get it.
 *
 * `buf` must be at least CAT_HDR_SIZE bytes. */
cat_status cat_header_parse(const uint8_t *buf, size_t len, cat_header *out);

/* Free the ext_header buffer inside a cat_header */
void cat_header_free(cat_header *h);

/* Serialize the fixed header to a buffer of CAT_HDR_SIZE bytes. */
cat_status cat_header_serialize(const cat_header *h, uint8_t *out);

/* ------ Inner-format backends ---------------------------------------- */

/* Detect inner format from the first few bytes of a payload. */
cat_inner cat_inner_detect(const void *payload, size_t len);

/* Decode a payload. Returns a NULL-terminated array of cat_file* on
 * success (caller frees the array and each cat_file). The function
 * returns the number of files written into `out_files`, or a negative
 * status code. For raw payloads, the filename is taken from the
 * extended header (if any) or defaults to "payload.bin".
 */
cat_status cat_decode(const cat_header *h, const void *payload, size_t plen,
                      cat_file ***out_files, size_t *out_count);

/* Free an array returned by cat_decode. */
void cat_files_free(cat_file **files, size_t count);

/* ------ Convenience: read/write a whole file ------------------------- */

/* Read a .qcf file and produce the list of inner files.
 * On success, populates `*out_header` (caller frees via cat_header_free)
 * and `**out_files` / `*out_count`. */
cat_status cat_read_file(const char *path,
                         cat_header *out_header,
                         cat_file ***out_files, size_t *out_count);

/* Pack one or more files into a .qcf archive.
 * If `names` has one entry, its bytes are the payload directly.
 * If multiple, they are wrapped in a ZIP container.
 */
cat_status cat_write_file(const char *path,
                          const char **names, size_t name_count,
                          const uint8_t *ext_header, size_t ext_size);

#ifdef __cplusplus
}
#endif

#endif /* CAT_H */

/* ---- Raw codec entry points (no .qcf wrapper) ----------------------- */

/* Decode a JPEG2000 codestream (JP2 or J2K) to a BMP file. */
cat_status cat_decode_jp2(const void *payload, size_t plen, const char *out_path);

/* Encode a 24-bit BMP file to a JP2 codestream. */
cat_status cat_encode_jp2_from_bmp(const char *in_path, uint8_t **out, size_t *out_len);

/* ---- JPEG backend (uses libjpeg) ------------------------------------ */

/* Decode a JPEG file to 24-bit RGB raw pixels.
 * The output buffer is row-major, 3 bytes per pixel (R,G,B).
 * Caller frees with free(). */
cat_status cat_decode_jpg_to_rgb(const char *path,
                                 uint8_t **out_pixels,
                                 int *out_w, int *out_h);

/* High-level: convert a JPEG file on disk to a JP2 codestream buffer
 * in memory (lossless re-encode). Caller frees *out with free(). */
cat_status cat_jpg_to_jp2(const char *jpg_path, uint8_t **out, size_t *out_len);

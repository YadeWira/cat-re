/* cat-tool: CLI for the libcat .qcf library. */
#include "cat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *status_str(cat_status s) {
    switch (s) {
        case CAT_OK: return "ok";
        case CAT_ERR_IO: return "io error";
        case CAT_ERR_BADMAGIC: return "bad magic";
        case CAT_ERR_TRUNCATED: return "truncated";
        case CAT_ERR_UNSUPPORTED: return "unsupported";
        case CAT_ERR_NOMEM: return "out of memory";
        case CAT_ERR_ZLIB: return "zlib error";
        case CAT_ERR_OPENJPEG: return "openjpeg error";
        case CAT_ERR_FORMAT: return "format error";
        case CAT_ERR_OVERFLOW: return "overflow";
        default: return "?";
    }
}

static void cmd_info(const char *path) {
    cat_header h;
    cat_file **files = NULL;
    size_t count = 0;
    cat_status s = cat_read_file(path, &h, &files, &count);
    if (s != CAT_OK) { fprintf(stderr, "cat: %s: %s\n", path, status_str(s)); exit(1); }
    printf("file:       %s\n", path);
    printf("magic:      0x%08x\n", h.magic);
    if (h.magic == CAT_MAGIC_QCF) printf("detected:   qcf\n");
    else if (h.magic == CAT_MAGIC_QCM) printf("detected:   qcm\n");
    else if (h.magic == CAT_MAGIC_ZIP) printf("detected:   zip-passthrough\n");
    printf("ext_hdr:    %u byte(s)\n", h.ext_hdr_size);
    if (h.ext_hdr_size > 0) {
        printf("ext_hex:    ");
        for (uint8_t i = 0; i < h.ext_hdr_size; i++) printf("%02x", h.ext_header[i]);
        printf("\n");
    }
    printf("inner:      %s\n", cat_inner_detect(NULL, 0) == CAT_INNER_UNKNOWN ? "?" : "?");
    printf("files:      %zu\n", count);
    for (size_t i = 0; i < count; i++) {
        printf("  [%zu] %s (%zu bytes)\n", i, files[i]->name, files[i]->size);
    }
    cat_files_free(files, count);
    cat_header_free(&h);
}

static void cmd_extract(const char *path, const char *outdir) {
    cat_header h;
    cat_file **files = NULL;
    size_t count = 0;
    cat_status s = cat_read_file(path, &h, &files, &count);
    if (s != CAT_OK) { fprintf(stderr, "cat: %s: %s\n", path, status_str(s)); exit(1); }
    for (size_t i = 0; i < count; i++) {
        char fp[1024];
        /* Strip leading path components from the archive name */
        const char *bn = strrchr(files[i]->name, '/');
        bn = bn ? bn + 1 : files[i]->name;
        if (outdir) snprintf(fp, sizeof(fp), "%s/%s", outdir, bn);
        else snprintf(fp, sizeof(fp), "%s", bn);
        FILE *f = fopen(fp, "wb");
        if (!f) { fprintf(stderr, "cat: cannot open %s\n", fp); exit(1); }
        fwrite(files[i]->data, 1, files[i]->size, f);
        fclose(f);
        printf("  extracted %s (%zu bytes)\n", fp, files[i]->size);
    }
    cat_files_free(files, count);
    cat_header_free(&h);
}

static void cmd_pack(const char *out_path, char **inputs, int n, const char *ext_name) {
    cat_status s = cat_write_file(out_path, (const char **)inputs, n,
                                  (const uint8_t *)ext_name, ext_name ? strlen(ext_name) : 0);
    if (s != CAT_OK) { fprintf(stderr, "cat: pack failed: %s\n", status_str(s)); exit(1); }
    printf("wrote %s\n", out_path);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: cat-tool {info|extract|pack} ...\n");
        return 1;
    }
    if (strcmp(argv[1], "info") == 0) {
        if (argc != 3) { fprintf(stderr, "usage: cat-tool info <file>\n"); return 1; }
        cmd_info(argv[2]);
    } else if (strcmp(argv[1], "extract") == 0) {
        const char *out = ".";
        const char *in = NULL;
        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) { out = argv[++i]; }
            else { in = argv[i]; }
        }
        if (!in) { fprintf(stderr, "usage: cat-tool extract [-o dir] <file>\n"); return 1; }
        cmd_extract(in, out);
    } else if (strcmp(argv[1], "pack") == 0) {
        char *out_path = NULL;
        char *ext_name = "payload.bin";
        char **inputs = (char **)calloc(argc, sizeof(char *));
        int n = 0;
        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) { out_path = argv[++i]; }
            else if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) { ext_name = argv[++i]; }
            else inputs[n++] = argv[i];
        }
        if (!out_path || n == 0) {
            fprintf(stderr, "usage: cat-tool pack -o <out> [-n name] <inputs...>\n");
            return 1;
        }
        cmd_pack(out_path, inputs, n, ext_name);
        free(inputs);
    } else {
        fprintf(stderr, "unknown subcommand: %s\n", argv[1]);
        return 1;
    }
    return 0;
}

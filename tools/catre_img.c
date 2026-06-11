/* catre_img.c — image codec for CAT RE (the "super compression" path).
 *
 * Encode: load any image (stb_image: PNG/JPG/BMP/GIF/...) -> RGB ->
 *         JPEG2000 (OpenJPEG, 9/7 irreversible, MCT, 5 levels, 2 layers, LRCP)
 *         -> 26-byte IMGCMP wrapper + J2K codestream  (== engine's image member).
 * Decode: skip the 26-byte wrapper -> decode J2K (OpenJPEG) -> write a PNG.
 *
 * lQuality (0-100) -> JPEG2000 target rate (PCRD), matching the original engine's knob.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <strings.h>
#include <openjpeg.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#define STBI_ONLY_GIF
#define STBI_ONLY_TGA
#include "vendor/stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "vendor/stb_image_write.h"

int catre_is_image(const char *name){
    const char *d = strrchr(name, '.');
    if (!d) return 0;
    return !strcasecmp(d,".png")||!strcasecmp(d,".jpg")||!strcasecmp(d,".jpeg")||
           !strcasecmp(d,".bmp")||!strcasecmp(d,".gif")||!strcasecmp(d,".tga");
}

/* ---- growable memory stream for OpenJPEG output ---- */
typedef struct { uint8_t *p; size_t n, cap, pos; } MemBuf;
static OPJ_SIZE_T mem_write(void *buf, OPJ_SIZE_T n, void *ud){
    MemBuf *m=ud;
    if (m->n+n > m->cap){ m->cap=(m->n+n)*2+4096; m->p=realloc(m->p,m->cap); }
    memcpy(m->p+m->n, buf, n); m->n+=n; return n;
}
static OPJ_SIZE_T mem_read(void *buf, OPJ_SIZE_T n, void *ud){
    MemBuf *m=ud;
    if (m->pos >= m->n) return (OPJ_SIZE_T)-1;     /* EOF (pos>=size, no underflow) */
    size_t avail=m->n - m->pos;
    if (n>avail) n=avail; memcpy(buf, m->p+m->pos, n); m->pos+=n; return n;
}
/* skip/seek MUST clamp — OpenJPEG seeks/skips past EOF on some codestreams (e.g. the
 * original engine's Kakadu output); an unclamped pos overflows mem_read's avail. */
static OPJ_OFF_T mem_skip(OPJ_OFF_T n, void *ud){
    MemBuf *m=ud;
    if (n < 0){ size_t back=(size_t)(-n); OPJ_OFF_T moved=-(OPJ_OFF_T)(back>m->pos?m->pos:back);
                m->pos = back>m->pos ? 0 : m->pos-back; return moved; }
    size_t avail=m->n - m->pos;
    if ((size_t)n > avail){ m->pos=m->n; return (OPJ_OFF_T)avail; }
    m->pos += (size_t)n; return n;
}
static OPJ_BOOL mem_seek(OPJ_OFF_T n, void *ud){
    MemBuf *m=ud; if (n<0 || (size_t)n > m->n) return OPJ_FALSE; m->pos=(size_t)n; return OPJ_TRUE;
}
static void quiet(const char*msg,void*u){(void)msg;(void)u;}

/* Build the 26-byte IMGCMP wrapper (verified against real engine output). */
static void wrapper26(uint8_t *w, int width, int height, int quality){
    memset(w,0,26);
    w[0]=0x15;                              /* format/type tag (engine default) */
    w[2]=width&0xff;  w[3]=(width>>8)&0xff;
    w[4]=height&0xff; w[5]=(height>>8)&0xff;
    w[6]=24;                                /* bpp */
    w[16]=0x01;                             /* const 0x0001 */
    w[18]=0x00; w[19]=0x20;                 /* const 0x2000 */
    w[22]=quality&0xff; w[23]=(quality>>8)&0xff;   /* lQuality */
}

/* Encode image `data` (in memory) -> malloc'd (wrapper + J2K); sets *out_len. */
uint8_t *catre_encode_image(const uint8_t *data, size_t len, int quality, uint32_t *out_len){
    int w,h,ch;
    unsigned char *px = stbi_load_from_memory(data, (int)len, &w, &h, &ch, 3); /* force RGB */
    if (!px) return NULL;

    opj_image_cmptparm_t cmp[3];
    memset(cmp,0,sizeof cmp);
    for (int i=0;i<3;i++){ cmp[i].dx=1; cmp[i].dy=1; cmp[i].w=w; cmp[i].h=h;
                           cmp[i].x0=0; cmp[i].y0=0; cmp[i].prec=8; cmp[i].sgnd=0; }
    opj_image_t *img = opj_image_create(3, cmp, OPJ_CLRSPC_SRGB);
    if(!img){ stbi_image_free(px); return NULL; }
    img->x0=0; img->y0=0; img->x1=w; img->y1=h;
    for (int i=0;i<w*h;i++){
        img->comps[0].data[i]=px[i*3+0];
        img->comps[1].data[i]=px[i*3+1];
        img->comps[2].data[i]=px[i*3+2];
    }
    stbi_image_free(px);

    opj_cparameters_t cp; opj_set_default_encoder_parameters(&cp);
    cp.irreversible = 1;                 /* 9/7 wavelet (lossy, like the engine)   */
    cp.numresolution = 6;                /* 5 decomposition levels                 */
    cp.prog_order = OPJ_LRCP;
    cp.tcp_mct = 1;                      /* RGB->YCC color transform               */
    /* PSNR-targeted (content-adaptive) rate control — this is what the original
     * engine (Kakadu) does: it aims at a quality level, not a fixed bitrate, so the
     * output size tracks image content. Calibrated against the real engine's measured
     * quality->PSNR curve (q0->26.7dB .. q100->32.2dB, ~linear): PSNR = 26.7 + q*0.055.
     * A fixed-bpp target (the old approach) bloated smooth/large images up to ~8x. */
    int q = quality < 0 ? 0 : quality > 100 ? 100 : quality;
    cp.cp_fixed_quality = 1;
    cp.tcp_numlayers = 1;
    cp.tcp_distoratio[0] = (float)(26.7 + q * 0.055);   /* target PSNR in dB */

    opj_codec_t *cod = opj_create_compress(OPJ_CODEC_J2K);
    opj_set_warning_handler(cod, quiet, NULL);
    opj_set_error_handler(cod, quiet, NULL);
    opj_setup_encoder(cod, &cp, img);

    MemBuf mb = {0};
    opj_stream_t *st = opj_stream_default_create(OPJ_FALSE);   /* output stream */
    opj_stream_set_user_data(st, &mb, NULL);
    opj_stream_set_write_function(st, mem_write);
    opj_stream_set_skip_function(st, mem_skip);
    opj_stream_set_seek_function(st, mem_seek);

    uint8_t *result=NULL;
    if (opj_start_compress(cod,img,st) && opj_encode(cod,st) && opj_end_compress(cod,st)){
        *out_len = 26 + (uint32_t)mb.n;
        result = malloc(*out_len);
        wrapper26(result, w, h, quality);
        memcpy(result+26, mb.p, mb.n);
    }
    free(mb.p);
    opj_stream_destroy(st); opj_destroy_codec(cod); opj_image_destroy(img);
    return result;
}

/* Decode an image member payload (wrapper+J2K) -> write PNG at out_path. */
int catre_decode_image(const uint8_t *payload, uint32_t len, const char *out_path){
    if (len <= 26) return 0;
    MemBuf mb = { (uint8_t*)payload+26, len-26, len-26, 0 };
    opj_stream_t *st = opj_stream_default_create(OPJ_TRUE);    /* input stream */
    opj_stream_set_user_data(st, &mb, NULL);
    opj_stream_set_user_data_length(st, mb.n);
    opj_stream_set_read_function(st, mem_read);
    opj_stream_set_skip_function(st, mem_skip);
    opj_stream_set_seek_function(st, mem_seek);

    opj_codec_t *cod = opj_create_decompress(OPJ_CODEC_J2K);
    opj_set_warning_handler(cod, quiet, NULL);
    opj_set_error_handler(cod, quiet, NULL);
    opj_dparameters_t dp; opj_set_default_decoder_parameters(&dp);
    opj_image_t *img=NULL; int ok=0;
    if (opj_setup_decoder(cod,&dp) && opj_read_header(st,cod,&img) &&
        opj_decode(cod,st,img) && opj_end_decompress(cod,st)){
        int w=img->comps[0].w, h=img->comps[0].h, nc=img->numcomps;
        unsigned char *rgb=malloc((size_t)w*h*3);
        for (int i=0;i<w*h;i++) for(int c=0;c<3;c++){
            int v = img->comps[nc>=3?c:0].data[i];
            rgb[i*3+c] = v<0?0:v>255?255:v;
        }
        ok = stbi_write_png(out_path, w, h, 3, rgb, w*3);
        free(rgb);
    }
    if (img) opj_image_destroy(img);
    opj_destroy_codec(cod); opj_stream_destroy(st);
    return ok;
}

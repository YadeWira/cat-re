/* CAT RE v1.8 — native C archiver for Choshuku/CAT `.qcf` (QCM) files.
 *
 * Free, reverse-engineered reimplementation. Reads the real format
 * (single-file, multi-file, nested folders) and writes the DEFLATE path.
 * No dependency on the original Windows DLLs. Backend: zlib.
 *
 * Build: cc -O2 -o catre tools/catre.c -lz
 * Verified: the original Choshuku engine decompresses archives this writes.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/time.h>
#include <unistd.h>
#include <dirent.h>
#include <zlib.h>

/* ---------- portability: Windows (mingw) ---------- */
#ifdef _WIN32
#include <io.h>
/* mingw's mkdir() takes a single argument; drop the POSIX mode. */
#define mkdir(p, m) mkdir(p)
#ifndef STDERR_FILENO
#define STDERR_FILENO 2
#endif
#endif

#define VERSION "1.8"
#define MAGIC_QCM 0x014D4351u
#define MAGIC_QCF 0x01464351u
#define CODEC_DEFLATE 0
#define MAXMEM 4096

/* ---------- image codec (catre_img.c, OpenJPEG) ---------- */
int catre_is_image(const char *name);
uint8_t *catre_encode_image(const uint8_t *data, size_t len, int quality, uint32_t *out_len);
int catre_decode_image(const uint8_t *payload, uint32_t len, const char *out_path);
int catre_verify_image(const uint8_t *payload, uint32_t len);
long catre_find_codestream(const uint8_t *payload, uint32_t len);
#define CODEC_IMAGE 1
#define CODEC_OLE2  2
/* codecs we recognize but can't always decode (proprietary to the original engine) */
#define CODEC_OFFICE_PS 3   /* MSOC21 per-stream: multi-mode (see cmd_extract)      */
#define CODEC_LEAD      4   /* image sub-codec 0x09 — LEAD CMP/CMW, third-party      */
#define CODEC_IMAGE_X   5   /* image sub-codec that is not JPEG2000 (e.g. 0x02: GIF) */
#define CODEC_PDF       6   /* PdfProc: structural PDF payload, not zlib-of-file     */
#define CODEC_UNKNOWN   7
static const char *codec_name(uint32_t c);
static int codec_decodable(uint32_t c);
/* MSOC21 36-byte header tail (engine wants it present & non-zero; not content-validated) */
static const uint8_t MSOC_TAIL[14]={0xde,0xf9,0x0b,0x45,0x71,0x1b,0xe4,0x00,0x46,0xcb,0x1f,0xe3,0x34,0x00};

/* ---------- progress bar + timing ---------- */
static int g_progress = 1;   /* auto-disabled when stderr is not a TTY */
static int g_image = 1;      /* compress images as JPEG2000 (--store to disable) */
static int g_office = 1;     /* compress OLE2 docs as MSOC21 (--store to disable) */

static double now_sec(void){
    struct timeval tv; gettimeofday(&tv, NULL);
    return tv.tv_sec + tv.tv_usec * 1e-6;
}
static const char *human(double n, char *buf){
    const char *u[] = {"B","KB","MB","GB"}; int i=0;
    while (n >= 1024 && i < 3){ n/=1024; i++; }
    sprintf(buf, i==0 ? "%.0f %s" : "%.1f %s", n, u[i]); return buf;
}
/* in-place bar on stderr: "verb [████░░░] 57%  3/5  name" */
static void bar(const char *verb, size_t done, size_t total, int idx, int n, const char *name){
    if (!g_progress) return;
    const int W = 24;
    double f = total ? (double)done/total : 1.0; if (f > 1) f = 1;
    int fill = (int)(f * W);
    fprintf(stderr, "\r\033[K%s [", verb);
    for (int i=0;i<W;i++) fputs(i<fill ? "█" : "░", stderr);
    const char *nm = name ? name : "";
    if (strlen(nm) > 32) nm += strlen(nm) - 32;        /* tail of long paths */
    fprintf(stderr, "] %3.0f%%  %d/%d  %s", f*100, idx, n, nm);
    fflush(stderr);
}
static void bar_clear(void){ if (g_progress) fprintf(stderr, "\r\033[K"); }

/* ---------- little-endian helpers ---------- */
static uint32_t rd32(const uint8_t *p){ return p[0]|(p[1]<<8)|(p[2]<<16)|((uint32_t)p[3]<<24); }
static void wr32(uint8_t *p, uint32_t v){ p[0]=v; p[1]=v>>8; p[2]=v>>16; p[3]=v>>24; }

/* ---------- growable byte buffer ---------- */
typedef struct { uint8_t *p; size_t n, cap; } Buf;
static void bput(Buf *b, const void *d, size_t n){
    if (b->n+n > b->cap){ b->cap=(b->n+n)*2+1024; b->p=realloc(b->p,b->cap); }
    memcpy(b->p+b->n, d, n); b->n+=n;
}
static void bu8(Buf*b,uint8_t v){ bput(b,&v,1); }
static void bu32(Buf*b,uint32_t v){ uint8_t t[4]; wr32(t,v); bput(b,t,4); }

/* ---------- archive model ---------- */
typedef struct {
    char name[1024];        /* full path */
    uint32_t orig, comp, codec, dt, payoff, hdr;
    /* The member's stream as stored: QCF header + ext header + payload, without the
     * 4-byte chunk prefix. Copying these bytes verbatim is how `add` and `delete`
     * rewrite an archive without having to decode (or even understand) a member —
     * the engine's proprietary codecs survive the operation untouched. */
    uint32_t inner_off, inner_len;
} Member;

/* Folder records (type 0x00) carry no stream. They are the only way an EMPTY folder
 * exists in the archive, so they have to be carried across a rewrite. */
/* 4 MB of names: every instance is `static`, never a local. A DirList on the stack
 * overflows Windows' 1 MB default (and ASan caught it on Linux too) — the same trap
 * the MAXMEM arrays already avoid. */
#define MAXDIRS 4096
typedef struct { char name[MAXDIRS][1024]; int n; } DirList;
static void dirs_add(DirList *d, const char *name){
    if (!d || !*name) return;
    for (int i=0;i<d->n;i++) if (!strcmp(d->name[i],name)) return;
    if (d->n<MAXDIRS){ strncpy(d->name[d->n],name,sizeof d->name[0]-1); d->name[d->n][sizeof d->name[0]-1]=0; d->n++; }
}

static uint8_t *read_file(const char *path, size_t *len){
    FILE *f=fopen(path,"rb"); if(!f) return NULL;
    fseek(f,0,SEEK_END); long n=ftell(f); fseek(f,0,SEEK_SET);
    uint8_t *d=malloc(n>0?n:1);
    if(fread(d,1,n,f)!=(size_t)n){ fclose(f); free(d); return NULL; }
    fclose(f); *len=n; return d;
}

/* Classify a member's codec by inspecting its QCF header (+ payload start).
 * `qcf` = byte offset of the QCF magic. Recognizes codecs we can't decode too. */
/* Codec bytes, measured against the engine itself (see docs/QCF_FORMAT_SPEC.md §5):
 *   +0x18 = 1 -> image, 0 -> stream
 *   image sub-codec at +0x19: 0x01 = JPEG2000 (the only one with an FF4F codestream),
 *                             0x02 = non-J2K (GIF, paletted/grayscale PNG),
 *                             0x09 = non-J2K (TIFF, some PNG) — the LEAD path
 *   stream family at +0x1A:   0x04 = deflate, 0x02 = office (MSOC21), 0x05 = PDF (PdfProc)
 * Only JPEG2000 images, deflate and the decodable office modes are ours to read; the
 * rest must be IDENTIFIED (so `list` is honest) and skipped, never fed to a decoder. */
static int classify_codec(const uint8_t *d, size_t len, size_t qcf){
    if (qcf+0x1c>len) return CODEC_UNKNOWN;
    uint8_t c0=d[qcf+0x18], c1=d[qcf+0x19], c2=d[qcf+0x1a], ext=d[qcf+0x1b];
    size_t pay=qcf+0x1c+ext;
    if (c0==0x01){                                      /* image member */
        if (c1==0x01) return CODEC_IMAGE;               /*   JPEG2000                  */
        if (c1==0x09) return CODEC_LEAD;                /*   LEAD CMP/CMW              */
        return CODEC_IMAGE_X;                           /*   another engine image codec */
    }
    if (c2==0x05) return CODEC_PDF;                     /* PdfProc (structural payload) */
    if (pay+4<=len && d[pay]==0x32 && d[pay+1]==0x01){  /* MSOC21 office               */
        if (!memcmp(d+pay,"\x32\x01\x12\x00",4)) return CODEC_OLE2;   /* whole-file (decodable) */
        return CODEC_OFFICE_PS;                         /* per-stream (structural, opaque)        */
    }
    return CODEC_DEFLATE;
}

/* parse a QCM container; returns member count, fills mem[] (caps at MAXMEM).
 * Robust: if the stream walk can't locate the central directory (e.g. members use
 * the engine's proprietary per-stream office / LEAD codecs, or an image whose `comp`
 * is the codestream size only), it falls back to scanning for the "TOP" directory
 * record so the archive can still be LISTED and the decodable members extracted. */
static int qcm_read_ex(const uint8_t *d, size_t len, Member *mem, int maxm, uint32_t *cdir_out,
                       DirList *dirs_out){
    if (len<0x24 || rd32(d)!=MAGIC_QCM) return -1;
    /* walk streams: stream1 @ +0x08 (no prefix); others have a 4-byte size prefix */
    static struct { uint32_t so, hdr, comp, payoff, codec; } st[MAXMEM]; int ns=0;
    size_t off=8; int first=1;
    while (off+0x1c<=len){
        size_t hdr = first?off:off+4;
        if (hdr+0x1c>len || rd32(d+hdr)!=MAGIC_QCF) break;
        uint32_t comp=rd32(d+hdr+0x08); uint8_t ext=d[hdr+0x1b];
        size_t payoff=hdr+0x1c+ext;
        int codec=classify_codec(d,len,hdr);   /* distinguishes JP2/LEAD and office whole-file/per-stream */
        if (codec==CODEC_OLE2 && comp==0 && payoff+12<=len){
            comp = 36 + rd32(d+payoff+8);   /* MSOC21 office whole-file: 36B header + zlib(whole OLE2) */
        }
        if (ns<MAXMEM){ st[ns].so=hdr-4; st[ns].hdr=hdr; st[ns].comp=comp;
                        st[ns].payoff=payoff; st[ns].codec=codec; ns++; }
        off=payoff+comp; first=0;
    }
    /* central directory: trust the walk if it landed on the "TOP" record... */
    size_t cdir=off;
    int strict=1;
    int top_ok = (cdir+20+3<=len && d[cdir+17]==3 && !memcmp(d+cdir+20,"TOP",3));
    if (!top_ok){
        /* ...otherwise scan for the last "[03 00 00]TOP" record (proprietary-codec/image files) */
        long found=-1;
        for (size_t i=0; i+6<=len; i++)
            if (d[i]==3 && d[i+1]==0 && d[i+2]==0 && !memcmp(d+i+3,"TOP",3)) found=(long)i;
        if (found<17) return -1;
        cdir=(size_t)found-17; strict=0;
    }
    if(cdir_out)*cdir_out=cdir;
    size_t p=cdir+9+4+4; if (p+3>len) return -1; uint8_t tl=d[p]; p+=3;
    if (memcmp(d+p,"TOP",3)!=0) return -1;
    p+=tl;
    /* collect records (offset, parent, stream_off, type, orig, dt, name) */
    static struct { uint32_t off,parent,so,type,orig,dt; char name[256]; } rec[MAXMEM]; int nr=0;
    while (p+16<=len && nr<MAXMEM){
        uint32_t roff=p;
        uint32_t parent=rd32(d+p); p+=4;
        uint32_t so=rd32(d+p); p+=4;
        uint8_t type=d[p]; p+=1;
        uint32_t dt=rd32(d+p); p+=4;
        uint32_t orig=rd32(d+p); p+=4;
        uint8_t nl=d[p]; p+=1; p+=2;
        if (p+nl>len) break;
        /* strict (normal files): stop at the first record without a walked stream;
         * fallback mode: accept all (streams weren't walkable). */
        if (strict && type==0x02){ int found=0; for(int i=0;i<ns;i++) if(st[i].so==so){found=1;break;} if(!found) break; }
        rec[nr].off=roff; rec[nr].parent=parent; rec[nr].so=so; rec[nr].type=type;
        rec[nr].orig=orig; rec[nr].dt=dt;
        int cn=nl<255?nl:255; memcpy(rec[nr].name,d+p,cn); rec[nr].name[cn]=0;
        p+=nl; nr++;
    }
    /* resolve full paths (follow parent pointers up to TOP=cdir) and emit files */
    int m=0;
    for (int i=0;i<nr;i++){                      /* folders first: needed to keep empty ones */
        if (rec[i].type==0x02 || !dirs_out) continue;
        char path[1024]={0}; char tmp[1024];
        uint32_t cur=rec[i].off; int guard=0;
        while (guard++<64){
            int idx=-1; for(int j=0;j<nr;j++) if(rec[j].off==cur){idx=j;break;}
            if (idx<0) break;
            if (path[0]) { snprintf(tmp,sizeof tmp,"%s/%s",rec[idx].name,path); strcpy(path,tmp); }
            else strncpy(path,rec[idx].name,sizeof path-1);
            if (rec[idx].parent==cdir || rec[idx].parent==rec[idx].off) break;
            cur=rec[idx].parent;
        }
        dirs_add(dirs_out,path);
    }
    for (int i=0;i<nr && m<maxm;i++){
        if (rec[i].type!=0x02) continue;
        char path[1024]={0}; char tmp[1024];
        uint32_t cur=rec[i].off; int guard=0;
        while (guard++<64){
            int idx=-1; for(int j=0;j<nr;j++) if(rec[j].off==cur){idx=j;break;}
            if (idx<0) break;
            if (path[0]) { snprintf(tmp,sizeof tmp,"%s/%s",rec[idx].name,path); strcpy(path,tmp); }
            else strncpy(path,rec[idx].name,sizeof path-1);
            if (rec[idx].parent==cdir || rec[idx].parent==rec[idx].off) break;
            cur=rec[idx].parent;
        }
        strncpy(mem[m].name,path,sizeof mem[m].name-1);
        mem[m].orig=rec[i].orig; mem[m].dt=rec[i].dt;
        int si=-1; for(int j=0;j<ns;j++) if(st[j].so==rec[i].so){si=j;break;}
        if (si>=0){                                  /* walked stream: full info */
            mem[m].comp=st[si].comp; mem[m].codec=st[si].codec;
            mem[m].payoff=st[si].payoff; mem[m].hdr=st[si].hdr;
        } else {                                     /* fallback: derive from the QCF header */
            size_t qcf=(size_t)rec[i].so+4;
            if (qcf+0x1c>len) continue;
            uint8_t ext=d[qcf+0x1b];
            mem[m].hdr=(uint32_t)qcf; mem[m].payoff=(uint32_t)(qcf+0x1c+ext);
            mem[m].codec=classify_codec(d,len,qcf); mem[m].comp=0;  /* size unknown for opaque codecs */
        }
        m++;
    }
    /* stream extents: members sit back to back before the directory, so each one ends
     * where the next begins. That gives an exact length even for codecs whose header
     * does not record the packed size. */
    for (int i=0;i<m;i++){
        uint32_t start = mem[i].hdr;                 /* inner bytes start at the QCF header */
        uint32_t end = (uint32_t)cdir;
        for (int j=0;j<m;j++){
            uint32_t other = mem[j].hdr;             /* the next stream's prefix, if any */
            uint32_t other_start = (other>=4 && mem[j].hdr-4>=8) ? other-4 : other;
            if (other_start>start && other_start<end) end=other_start;
        }
        mem[i].inner_off = start;
        mem[i].inner_len = (end>start) ? end-start : 0;
        if (!mem[i].comp && mem[i].inner_len > 0x1c){   /* opaque codec: derive packed size */
            uint32_t ext = d[start+0x1b];
            if (mem[i].inner_len > 0x1c+ext) mem[i].comp = mem[i].inner_len - 0x1c - ext;
        }
    }
    return m;
}

static int qcm_read(const uint8_t *d, size_t len, Member *mem, int maxm, uint32_t *cdir_out){
    return qcm_read_ex(d,len,mem,maxm,cdir_out,NULL);
}

/* Does a zlib stream start here? CMF=0x78 (deflate, 32K window) and the FCHECK
 * rule from RFC 1950: the 16-bit CMF/FLG pair must be a multiple of 31. Cheap, and
 * it keeps the office-ps payload scan from trying to inflate at random offsets. */
static int zlib_hdr_at(const uint8_t *p){
    return p[0]==0x78 && ((p[0]<<8 | p[1]) % 31)==0;
}

/* Several of the engine's structural codecs (office per-stream, PdfProc) have a mode
 * that simply stores the WHOLE original file as one zlib stream inside their payload.
 * Measured: a .doc and a .pdf both come back byte-exact that way. Scan the payload for
 * a zlib header and accept the inflate only when it yields exactly `orig` bytes.
 * Returns a malloc'd buffer of `orig` bytes, or NULL when this member is in one of the
 * opaque modes. */
static uint8_t *try_wholefile_zlib(const uint8_t *d, size_t len, size_t payoff, uint32_t orig){
    for (size_t z=payoff; z+2<len; z++){
        if (!zlib_hdr_at(d+z)) continue;
        uLongf dn=orig; uint8_t *buf=malloc(dn?dn:1);
        if (uncompress(buf,&dn,d+z,(uLong)(len-z))==Z_OK && dn==orig) return buf;
        free(buf);
    }
    return NULL;
}

static uint8_t *inflate_mem(const uint8_t *src, uint32_t comp, uint32_t orig){
    uint8_t *out=malloc(orig>0?orig:1); uLongf dn=orig;
    if (uncompress(out,&dn,src,comp)!=Z_OK || dn!=orig){ free(out); return NULL; }
    return out;
}

/* ---------- DOS datetime ---------- */
static uint32_t now_dos(void){
    time_t tt=time(NULL); struct tm *t=localtime(&tt);
    int yr=t->tm_year+1900-1980; if(yr<0)yr=0;
    uint32_t date=(yr<<9)|((t->tm_mon+1)<<5)|t->tm_mday;
    uint32_t tim=(t->tm_hour<<11)|(t->tm_min<<5)|(t->tm_sec/2);
    return (date<<16)|tim;
}
static void dos_str(uint32_t dt, char *out){
    uint32_t date=dt>>16, tim=dt&0xffff;
    sprintf(out,"%04d-%02d-%02d %02d:%02d:%02d",
        ((date>>9)&0x7f)+1980,(date>>5)&0xf,date&0x1f,(tim>>11)&0x1f,(tim>>5)&0x3f,(tim&0x1f)*2);
}

/* ---------- compress: gather files (recurse dirs), build QCM ---------- */
typedef struct { char *name; uint8_t *data; size_t len; } InFile;

static void gather(const char *path, const char *base, InFile **list, int *n, int *cap,
                   DirList *dirs){
    struct stat sb; if (stat(path,&sb)!=0) return;
    if (S_ISDIR(sb.st_mode)){
        int before=*n;
        DIR *dp=opendir(path); if(!dp) return; struct dirent *e;
        while ((e=readdir(dp))){
            if (!strcmp(e->d_name,".")||!strcmp(e->d_name,"..")) continue;
            char sub[2048]; snprintf(sub,sizeof sub,"%s/%s",path,e->d_name);
            gather(sub,base,list,n,cap,dirs);
        }
        closedir(dp);
        if (*n==before && dirs){          /* nothing below it: an EMPTY folder, which only
                                           * a folder record can carry into the archive */
            const char *rel=path; size_t bl=strlen(base);
            if (!strncmp(path,base,bl)) rel=path+bl+(path[bl]=='/'?1:0);
            dirs_add(dirs,rel);
        }
    } else {
        size_t len; uint8_t *d=read_file(path,&len); if(!d) return;
        /* member name = path relative to base's parent */
        const char *rel=path; size_t bl=strlen(base);
        if (!strncmp(path,base,bl)) rel=path+bl+(path[bl]=='/'?1:0);
        if (*n>=*cap){ *cap=*cap*2+16; *list=realloc(*list,*cap*sizeof(InFile)); }
        (*list)[*n].name=strdup(rel); (*list)[*n].data=d; (*list)[*n].len=len; (*n)++;
    }
}

/* Build one member's stream (QCF header + ext byte + payload) from a file in memory.
 * Returns malloc'd bytes; *len_out is their length. This is the piece `compress` and
 * `add` share — the codec choice lives here and nowhere else. */
static uint8_t *build_member_stream(const char *name, const uint8_t *fd, size_t len,
                                    int q, uint32_t *len_out, int *codec_out, uint32_t *psize_out){
    uint8_t *payload=NULL; uint32_t psize=0, inner_csize=0, field04=0;
    int codec=CODEC_DEFLATE; uint8_t t4[4]={0x00,0x05,0x04,0x01};   /* deflate */
    int isole2 = len>=8 && !memcmp(fd,"\xd0\xcf\x11\xe0\xa1\xb1\x1a\xe1",8);

    /* DEFLATE baseline: cheap, never bloats. Serves as both the universal
     * fallback and the yardstick a specialized codec must beat. */
    uLongf cb=compressBound(len); uint8_t *defl=malloc(cb);
    compress2(defl,&cb,fd,len,9); uint32_t defl_size=(uint32_t)cb;

    /* Try JPEG2000 for images — but only keep it if it actually beats DEFLATE,
     * so a tiny/already-compressed image (e.g. a small GIF) never *grows*. */
    uint8_t *img=NULL; uint32_t imgsz=0;
    if (g_image && catre_is_image(name))
        img = catre_encode_image(fd, len, q, &imgsz);

    if (img && imgsz < defl_size){              /* JPEG2000 wins */
        payload=img; psize=imgsz; inner_csize=psize; field04=(uint32_t)len;
        codec=CODEC_IMAGE; t4[0]=0x01;t4[1]=0x01;t4[2]=0x04;t4[3]=0x01;
        free(defl);
    } else if (g_office && isole2){             /* MSOC21: 36-byte hdr + zlib(whole OLE2) */
        if (img) free(img);
        uint32_t zc=defl_size;                  /* zlib(whole OLE2) == the deflate baseline */
        psize=36+zc; payload=malloc(psize); uint8_t *p=payload;
        memcpy(p,"\x32\x01\x12\x00\x00\x00",6); p+=6; memcpy(p,"\x33\x02",2); p+=2;
        wr32(p,zc); p+=4; memset(p,0,6); p+=6;
        memcpy(p,"\x04\x0a\x00\x05",4); p+=4; memcpy(p,MSOC_TAIL,14); p+=14;
        memcpy(p,defl,zc); free(defl);
        codec=CODEC_OLE2; field04=(uint32_t)len; inner_csize=0;  /* office: +08 stays 0 */
        t4[0]=0x00;t4[1]=0x00;t4[2]=0x02;t4[3]=0x01;
    } else {                                    /* deflate (default / image fallback) */
        if (img) free(img);
        payload=defl; psize=defl_size; inner_csize=psize;
    }
    const char *bn=strrchr(name,'/');
    uint8_t ext=(uint8_t)(bn ? bn[1] : name[0]);   /* 1st char of basename */
    Buf inner={0};
    bu32(&inner,MAGIC_QCF); bu32(&inner,field04); bu32(&inner,inner_csize); bu32(&inner,0);
    bu32(&inner,0x0011001E);
    uint8_t tail[8]={0x01,0x00,0x04,0x00, t4[0],t4[1],t4[2],t4[3]};
    bput(&inner,tail,8);
    bu8(&inner,ext); bput(&inner,payload,psize);
    free(payload);
    *len_out=(uint32_t)inner.n;
    if (codec_out) *codec_out=codec;
    if (psize_out) *psize_out=psize;
    return inner.p;
}

/* A member as it will be written: metadata plus its stream bytes, which may have just
 * been built from a file OR copied verbatim out of an existing archive (that is how
 * `add`/`delete` keep members whose codec we cannot decode). */
typedef struct {
    char name[1024];
    uint32_t orig, dt;
    const uint8_t *inner; uint32_t inner_len;
    uint8_t *owned;                 /* non-NULL when we allocated `inner` */
} OutMember;

/* Assemble a QCM archive: header, member streams, then the central directory with
 * folder records (including empty folders) and file records wired to their parents. */
static int write_qcm(const char *out, OutMember *m, int n, DirList *extra_dirs){
    Buf out_b={0}; bu32(&out_b,MAGIC_QCM); bu32(&out_b,0);   /* [+04] patched below */
    uint32_t *so=malloc((n?n:1)*sizeof(uint32_t));
    uint32_t dt = n ? m[0].dt : now_dos();
    for (int i=0;i<n;i++){
        if (i==0){ so[0]=(uint32_t)out_b.n-4; bput(&out_b,m[i].inner,m[i].inner_len);
                   wr32(out_b.p+4,(uint32_t)out_b.n-4); }
        else { so[i]=(uint32_t)out_b.n; bu32(&out_b,4+m[i].inner_len);
               bput(&out_b,m[i].inner,m[i].inner_len); }
    }
    uint32_t cdir=(uint32_t)out_b.n;
    for (int z=0;z<9;z++) bu8(&out_b,0);
    bu32(&out_b,dt); bu32(&out_b,0);
    uint8_t toph[3]={3,0,0}; bput(&out_b,toph,3); bput(&out_b,"TOP",3);

    /* Record order mirrors the engine's own archives: the files of a folder first,
     * then each subfolder's record followed by its contents (depth first). Emitting
     * every folder up front builds the same tree and the engine reads it either way —
     * this just keeps our output shaped like the original's.
     * (Unrelated, but worth writing down: the engine's single-file DecompressFile API
     * returns E_INVALIDARG on an archive with no file at the ROOT, whatever wrote it,
     * because there is no single file for it to extract. Folder archives are the
     * shell's job in the original product.) */
    static DirList all; all.n=0;
    for (int i=0;i<n;i++){                       /* every prefix of every member path */
        char *nm=m[i].name;
        for (char *s2=nm; *s2; s2++) if (*s2=='/'){
            int L=s2-nm; char pre[1024]; if(L>=1024)L=1023; memcpy(pre,nm,L); pre[L]=0;
            dirs_add(&all,pre);
        }
    }
    if (extra_dirs) for (int i=0;i<extra_dirs->n;i++){
        dirs_add(&all,extra_dirs->name[i]);
        for (char *s2=extra_dirs->name[i]; *s2; s2++) if (*s2=='/'){   /* and its parents */
            int L=s2-extra_dirs->name[i]; char pre[1024]; if(L>=1024)L=1023;
            memcpy(pre,extra_dirs->name[i],L); pre[L]=0; dirs_add(&all,pre);
        }
    }

    /* iterative depth-first walk over (prefix, parent record offset) */
    typedef struct { char prefix[1024]; uint32_t parent; } Frame;
    static Frame stack[MAXDIRS]; int sp=0;
    static int dir_done[MAXDIRS];
    for (int i=0;i<all.n;i++) dir_done[i]=0;
    stack[sp].prefix[0]=0; stack[sp].parent=cdir; sp++;
    while (sp>0){
        Frame fr=stack[--sp];
        size_t plen=strlen(fr.prefix);
        for (int i=0;i<n;i++){                   /* files directly in this folder */
            const char *nm=m[i].name;
            const char *ls=strrchr(nm,'/');
            int in_here = plen ? (!strncmp(nm,fr.prefix,plen) && nm[plen]=='/' &&
                                  ls==nm+plen)
                               : (ls==NULL);
            if (!in_here) continue;
            const char *bn = ls ? ls+1 : nm;
            bu32(&out_b,fr.parent); bu32(&out_b,so[i]); bu8(&out_b,2); bu32(&out_b,m[i].dt);
            bu32(&out_b,m[i].orig);
            size_t nl=strlen(bn); bu8(&out_b,(uint8_t)nl); bu8(&out_b,0); bu8(&out_b,0);
            bput(&out_b,bn,nl);
        }
        /* subfolders of this folder, deepest pushed last so they come out in order */
        int children[MAXDIRS]; int nc=0;
        for (int i=0;i<all.n;i++){
            if (dir_done[i]) continue;
            const char *dn=all.name[i];
            const char *ls=strrchr(dn,'/');
            int in_here = plen ? (!strncmp(dn,fr.prefix,plen) && dn[plen]=='/' && ls==dn+plen)
                               : (ls==NULL);
            if (in_here && nc<MAXDIRS) children[nc++]=i;
        }
        for (int c=nc-1;c>=0;c--){
            int i=children[c]; dir_done[i]=1;
            const char *dn=all.name[i];
            const char *ls=strrchr(dn,'/'); const char *bn = ls ? ls+1 : dn;
            uint32_t myoff=(uint32_t)out_b.n;
            bu32(&out_b,fr.parent); bu32(&out_b,0); bu8(&out_b,0); bu32(&out_b,dt);  /* type=0 */
            bu32(&out_b,0); size_t nl=strlen(bn); bu8(&out_b,(uint8_t)nl); bu8(&out_b,0); bu8(&out_b,0);
            bput(&out_b,bn,nl);
            if (sp<MAXDIRS){ strncpy(stack[sp].prefix,dn,sizeof stack[sp].prefix-1);
                             stack[sp].prefix[sizeof stack[sp].prefix-1]=0;
                             stack[sp].parent=myoff; sp++; }
        }
    }
    FILE *f=fopen(out,"wb");
    if(!f){ bar_clear(); perror("open out"); free(so); free(out_b.p); return -1; }
    fwrite(out_b.p,1,out_b.n,f); fclose(f);
    int written=(int)out_b.n;
    free(so); free(out_b.p);
    return written;
}

static int cmd_compress(int argc, char **argv){
    const char *out=NULL; int q=100, verbose=0;
    InFile *files=NULL; int nf=0, cap=0;
    static DirList dirs; dirs.n=0;
    for (int i=0;i<argc;i++){
        if (!strcmp(argv[i],"-o")||!strcmp(argv[i],"--output")) out=argv[++i];
        else if (!strcmp(argv[i],"-q")||!strcmp(argv[i],"--quality")) q=atoi(argv[++i]);
        else if (!strcmp(argv[i],"-v")||!strcmp(argv[i],"--verbose")) verbose=1;
        else if (!strcmp(argv[i],"--no-progress")) g_progress=0;
        else if (!strcmp(argv[i],"-p")||!strcmp(argv[i],"--progress")) g_progress=2;
        else if (!strcmp(argv[i],"--store")) { g_image=0; g_office=0; }
        else if (!strcmp(argv[i],"--no-image")) g_image=0;
        else {
            struct stat sb; char parent[2048];
            if (stat(argv[i],&sb)==0 && S_ISDIR(sb.st_mode)){
                strncpy(parent,argv[i],sizeof parent-1); parent[sizeof parent-1]=0;
                char *s2=strrchr(parent,'/'); if(s2)*s2=0; else strcpy(parent,".");
                gather(argv[i],parent,&files,&nf,&cap,&dirs);
            } else {
                size_t len; uint8_t *d=read_file(argv[i],&len);
                if(d){ const char*bn=strrchr(argv[i],'/'); bn=bn?bn+1:argv[i];
                    if(nf>=cap){cap=cap*2+16;files=realloc(files,cap*sizeof(InFile));}
                    files[nf].name=strdup(bn); files[nf].data=d; files[nf].len=len; nf++; }
            }
        }
    }
    if (!out){ fprintf(stderr,"catre: -o/--output required\n"); return 2; }
    if (!nf && !dirs.n){ fprintf(stderr,"catre: no input files\n"); return 2; }

    uint32_t dt=now_dos();
    size_t total_in=0; for (int i=0;i<nf;i++) total_in+=files[i].len;
    double t0=now_sec(); size_t done=0;
    OutMember *om=calloc(nf?nf:1,sizeof(OutMember));
    for (int i=0;i<nf;i++){
        bar("Compressing", done, total_in, i, nf, files[i].name);
        int codec; uint32_t psize, ilen;
        uint8_t *inner=build_member_stream(files[i].name, files[i].data, files[i].len,
                                           q, &ilen, &codec, &psize);
        strncpy(om[i].name,files[i].name,sizeof om[i].name-1);
        om[i].orig=(uint32_t)files[i].len; om[i].dt=dt;
        om[i].inner=inner; om[i].inner_len=ilen; om[i].owned=inner;
        done+=files[i].len;
        if (verbose){ char a[16],b[16]; double r=files[i].len?100.0*psize/files[i].len:0;
            bar_clear();
            printf("  + %-32s %9s -> %9s (%.0f%%) %s\n", files[i].name,
                   human(files[i].len,a), human((double)psize,b), r,
                   codec==CODEC_IMAGE?"[jp2]":codec==CODEC_OLE2?"[office]":"[deflate]"); }
    }
    bar("Compressing", total_in, total_in, nf, nf, "done");
    int written=write_qcm(out,om,nf,&dirs);
    for (int i=0;i<nf;i++) free(om[i].owned);
    free(om);
    bar_clear();
    if (written<0) return 1;
    double dt_s=now_sec()-t0, ratio=total_in?100.0*written/total_in:0;
    char a[16],b[16],sp[16];
    printf("Created %s: %d file(s)%s, %s -> %s (%.1f%%), %.2fs, %s/s\n",
           out, nf, dirs.n?" + empty folder(s)":"", human((double)total_in,a),
           human((double)written,b), ratio, dt_s,
           human(dt_s>0?total_in/dt_s:total_in, sp));
    return 0;
}

/* ---------- add / delete: rewrite an archive, keeping members we can't decode ----------
 * Both work on the stored streams, never on decoded data, so a member in one of the
 * engine's proprietary codecs survives the operation byte-for-byte. */
static int cmd_add(int argc, char **argv){
    const char *arc=NULL; int q=100, verbose=0;
    InFile *files=NULL; int nf=0, cap=0;
    static DirList newdirs; newdirs.n=0;
    for (int i=0;i<argc;i++){
        if (!strcmp(argv[i],"-q")||!strcmp(argv[i],"--quality")) q=atoi(argv[++i]);
        else if (!strcmp(argv[i],"-v")||!strcmp(argv[i],"--verbose")) verbose=1;
        else if (!strcmp(argv[i],"--no-progress")) g_progress=0;
        else if (!strcmp(argv[i],"-p")||!strcmp(argv[i],"--progress")) g_progress=2;
        else if (!strcmp(argv[i],"--store")) { g_image=0; g_office=0; }
        else if (!strcmp(argv[i],"--no-image")) g_image=0;
        else if (!arc) arc=argv[i];
        else {
            struct stat sb; char parent[2048];
            if (stat(argv[i],&sb)==0 && S_ISDIR(sb.st_mode)){
                strncpy(parent,argv[i],sizeof parent-1); parent[sizeof parent-1]=0;
                char *s2=strrchr(parent,'/'); if(s2)*s2=0; else strcpy(parent,".");
                gather(argv[i],parent,&files,&nf,&cap,&newdirs);
            } else {
                size_t len; uint8_t *d=read_file(argv[i],&len);
                if(d){ const char*bn=strrchr(argv[i],'/'); bn=bn?bn+1:argv[i];
                    if(nf>=cap){cap=cap*2+16;files=realloc(files,cap*sizeof(InFile));}
                    files[nf].name=strdup(bn); files[nf].data=d; files[nf].len=len; nf++; }
            }
        }
    }
    if(!arc){ fprintf(stderr,"catre: archive required\n"); return 2; }
    if(!nf && !newdirs.n){ fprintf(stderr,"catre: nothing to add\n"); return 2; }

    size_t len; uint8_t *d=read_file(arc,&len); if(!d){ perror(arc); return 1; }
    static Member mem[MAXMEM]; static DirList olddirs;
    olddirs.n=0;
    int n=qcm_read_ex(d,len,mem,MAXMEM,NULL,&olddirs);
    if(n<0){ fprintf(stderr,"catre: not a valid .qcf\n"); free(d); return 1; }

    OutMember *om=calloc(n+nf,sizeof(OutMember)); int on=0;
    for (int i=0;i<n;i++){                       /* keep, unless the name is replaced */
        int replaced=0;
        for (int j=0;j<nf;j++) if(!strcmp(files[j].name,mem[i].name)) replaced=1;
        if (replaced) continue;
        strncpy(om[on].name,mem[i].name,sizeof om[on].name-1);
        om[on].orig=mem[i].orig; om[on].dt=mem[i].dt;
        om[on].inner=d+mem[i].inner_off; om[on].inner_len=mem[i].inner_len;
        on++;
    }
    uint32_t dt=now_dos();
    for (int j=0;j<nf;j++){
        bar("Adding", j, nf, j, nf, files[j].name);
        int codec; uint32_t psize, ilen;
        uint8_t *inner=build_member_stream(files[j].name,files[j].data,files[j].len,
                                           q,&ilen,&codec,&psize);
        strncpy(om[on].name,files[j].name,sizeof om[on].name-1);
        om[on].orig=(uint32_t)files[j].len; om[on].dt=dt;
        om[on].inner=inner; om[on].inner_len=ilen; om[on].owned=inner;
        if (verbose){ bar_clear(); printf("  + %s\n",files[j].name); }
        on++;
    }
    for (int i=0;i<newdirs.n;i++) dirs_add(&olddirs,newdirs.name[i]);
    int written=write_qcm(arc,om,on,&olddirs);
    for (int i=0;i<on;i++) free(om[i].owned);
    free(om); free(d);
    bar_clear();
    if (written<0) return 1;
    char a[16];
    printf("Updated %s: %d file(s) total (%s)\n", arc, on, human((double)written,a));
    return 0;
}

static int cmd_delete(int argc, char **argv){
    const char *arc=NULL; int verbose=0; const char *names[MAXMEM]; int nn=0;
    for (int i=0;i<argc;i++){
        if (!strcmp(argv[i],"-v")||!strcmp(argv[i],"--verbose")) verbose=1;
        else if (!strcmp(argv[i],"--no-progress")||!strcmp(argv[i],"-p")||!strcmp(argv[i],"--progress")) {}
        else if (!arc) arc=argv[i];
        else if (nn<MAXMEM) names[nn++]=argv[i];
    }
    if(!arc || !nn){ fprintf(stderr,"catre: usage: catre delete <archive> <member>...\n"); return 2; }
    size_t len; uint8_t *d=read_file(arc,&len); if(!d){ perror(arc); return 1; }
    static Member mem[MAXMEM]; static DirList dirs; dirs.n=0;
    int n=qcm_read_ex(d,len,mem,MAXMEM,NULL,&dirs);
    if(n<0){ fprintf(stderr,"catre: not a valid .qcf\n"); free(d); return 1; }

    OutMember *om=calloc(n?n:1,sizeof(OutMember)); int on=0, removed=0;
    for (int i=0;i<n;i++){
        int drop=0;
        for (int j=0;j<nn;j++){
            size_t L=strlen(names[j]);
            /* exact name, or everything under a folder the user named */
            if (!strcmp(mem[i].name,names[j]) ||
                (!strncmp(mem[i].name,names[j],L) && mem[i].name[L]=='/')) drop=1;
        }
        if (drop){ removed++; if(verbose) printf("  - %s\n",mem[i].name); continue; }
        strncpy(om[on].name,mem[i].name,sizeof om[on].name-1);
        om[on].orig=mem[i].orig; om[on].dt=mem[i].dt;
        om[on].inner=d+mem[i].inner_off; om[on].inner_len=mem[i].inner_len;
        on++;
    }
    if (!removed){ fprintf(stderr,"catre: no member matched\n"); free(om); free(d); return 1; }
    /* a folder the user deleted should not come back through the folder records */
    static DirList keep; keep.n=0;
    for (int i=0;i<dirs.n;i++){
        int drop=0;
        for (int j=0;j<nn;j++){
            size_t L=strlen(names[j]);
            if (!strcmp(dirs.name[i],names[j]) ||
                (!strncmp(dirs.name[i],names[j],L) && dirs.name[i][L]=='/')) drop=1;
        }
        if (!drop) dirs_add(&keep,dirs.name[i]);
    }
    int written=write_qcm(arc,om,on,&keep);
    free(om); free(d);
    if (written<0) return 1;
    char a[16];
    printf("Deleted %d member(s) from %s: %d left (%s)\n", removed, arc, on,
           human((double)written,a));
    return 0;
}

/* ---------- mkdir -p for a file path ---------- */
static void mkdirs(const char *path){
    char tmp[2048]; strncpy(tmp,path,sizeof tmp-1); tmp[sizeof tmp-1]=0;
    for (char *s=tmp+1; *s; s++) if (*s=='/'){ *s=0; mkdir(tmp,0755); *s='/'; }
}

static int cmd_extract(int argc, char **argv){
    const char *arc=NULL,*out="."; int verbose=0;
    const char *want[MAXMEM]; int nwant=0;          /* -m: extract only these members */
    for (int i=0;i<argc;i++){
        if (!strcmp(argv[i],"-o")||!strcmp(argv[i],"--output")) out=argv[++i];
        else if (!strcmp(argv[i],"-v")||!strcmp(argv[i],"--verbose")) verbose=1;
        else if (!strcmp(argv[i],"--no-progress")) g_progress=0;
        else if (!strcmp(argv[i],"-p")||!strcmp(argv[i],"--progress")) g_progress=2;
        else if (!strcmp(argv[i],"-m")||!strcmp(argv[i],"--members")){
            while (i+1<argc && argv[i+1][0]!='-' && nwant<MAXMEM) want[nwant++]=argv[++i];
        }
        else arc=argv[i];
    }
    if(!arc){ fprintf(stderr,"catre: archive required\n"); return 2; }
    size_t len; uint8_t *d=read_file(arc,&len); if(!d){ perror(arc); return 1; }
    static Member mem[MAXMEM]; static DirList dirs; dirs.n=0;
    int n=qcm_read_ex(d,len,mem,MAXMEM,NULL,&dirs);
    if (n<0){ fprintf(stderr,"catre: not a valid .qcf\n"); return 1; }
    mkdir(out,0755);
    /* recreate folder records first — an EMPTY folder exists only as a record, so
     * waiting for a file to mkdir its parents would silently drop it. */
    for (int i=0;i<dirs.n;i++){
        if (nwant){
            int hit=0;
            for (int j=0;j<nwant;j++){
                size_t L=strlen(want[j]);
                if (!strcmp(dirs.name[i],want[j]) ||
                    (!strncmp(dirs.name[i],want[j],L) && dirs.name[i][L]=='/')) hit=1;
            }
            if (!hit) continue;
        }
        char dpath[2300]; snprintf(dpath,sizeof dpath,"%s/%s/",out,dirs.name[i]);
        mkdirs(dpath);
    }
    size_t total_out=0; for(int i=0;i<n;i++) total_out+=mem[i].codec?mem[i].comp:mem[i].orig;
    double t0=now_sec(); size_t prog=0;
    int done=0;
    int skipped=0;
    for (int i=0;i<n;i++){
        if (nwant){                                  /* -m: a name, or a folder's contents */
            int hit=0;
            for (int j=0;j<nwant;j++){
                size_t L=strlen(want[j]);
                if (!strcmp(mem[i].name,want[j]) ||
                    (!strncmp(mem[i].name,want[j],L) && mem[i].name[L]=='/')) hit=1;
            }
            if (!hit) continue;
        }
        bar("Extracting", prog, total_out, i, n, mem[i].name);
        char target[2300];
        /* office-ps (per-stream) is not one format: the engine picks among several
         * modes per document. One of them stores the WHOLE original file as a single
         * zlib stream — that one is lossless and we decode it here. The other two (an
         * intermediate structural model, and the non-deflate sparse-XLS body) are
         * opaque, so those members are skipped. */
        if (mem[i].codec==CODEC_OFFICE_PS || mem[i].codec==CODEC_PDF){
            uint8_t *whole=try_wholefile_zlib(d,len,mem[i].payoff,mem[i].orig);
            if (whole){
                snprintf(target,sizeof target,"%s/%s",out,mem[i].name); mkdirs(target);
                FILE *f=fopen(target,"wb");
                if(f){ fwrite(whole,1,mem[i].orig,f); fclose(f); done++; prog+=mem[i].orig;
                       if(verbose){ bar_clear(); printf("  -> %-36s (%s whole-file)\n",
                                                        mem[i].name, codec_name(mem[i].codec)); } }
                free(whole); continue;
            }
            bar_clear();
            fprintf(stderr,"  SKIP %s: %s in a structural mode — needs the original "
                           "Choshuku engine (which does not restore it byte-exact either)\n",
                    mem[i].name, codec_name(mem[i].codec));
            skipped++; continue;
        }
        if (!codec_decodable(mem[i].codec)){     /* proprietary codec — needs the original engine */
            bar_clear();
            fprintf(stderr,"  SKIP %s: codec '%s' needs the original Choshuku engine "
                           "(%s) — cannot decode\n", mem[i].name, codec_name(mem[i].codec),
                    mem[i].codec==CODEC_LEAD ? "LEAD CMP/CMW, third-party" :
                    mem[i].codec==CODEC_IMAGE_X ? "engine image codec, not JPEG2000" :
                    mem[i].codec==CODEC_PDF ? "PdfProc structural payload" : "unsupported");
            skipped++; continue;
        }
        if (mem[i].codec==CODEC_IMAGE){          /* JPEG2000 -> decode to PNG */
            /* find the codestream (it is not always right after the 26-byte wrapper:
             * an alpha channel is stored before it). No codestream -> not JPEG2000. */
            if (catre_find_codestream(d+mem[i].payoff, (uint32_t)(len-mem[i].payoff)) < 0){
                bar_clear();
                fprintf(stderr,"  SKIP %s: image member without a JPEG2000 codestream "
                               "(engine image codec) — cannot decode\n", mem[i].name);
                skipped++; continue;
            }
            /* replace the member's extension with .png (don't append: file.png -> file.png, not file.png.png) */
            char stem[2048]; snprintf(stem,sizeof stem,"%s",mem[i].name);
            char *dot=strrchr(stem,'.'), *slash=strrchr(stem,'/');
            if (dot && (!slash || dot>slash)) *dot=0;     /* strip ext only if after the last '/' */
            snprintf(target,sizeof target,"%s/%s.png",out,stem);
            mkdirs(target);
            /* feed the whole remaining buffer: our `comp` includes the 26B wrapper but the
             * engine's `comp` is the codestream only — OpenJPEG stops at EOC, so over-feeding
             * is safe and avoids truncating engine codestreams. */
            if (catre_decode_image(d+mem[i].payoff, (uint32_t)(len-mem[i].payoff), target)){ done++; prog+=mem[i].comp;
                if(verbose){ bar_clear(); printf("  -> %-36s (decoded image)\n", target); } }
            else { bar_clear(); fprintf(stderr,"  FAILED decode: %s\n",mem[i].name); }
            continue;
        }
        snprintf(target,sizeof target,"%s/%s",out,mem[i].name);
        mkdirs(target);
        /* office (MSOC21) = 36-byte header + zlib(whole file); deflate = zlib directly */
        size_t zoff = mem[i].payoff + (mem[i].codec==CODEC_OLE2 ? 36 : 0);
        uint32_t zlen = mem[i].comp - (mem[i].codec==CODEC_OLE2 ? 36 : 0);
        uint8_t *data=inflate_mem(d+zoff,zlen,mem[i].orig); size_t dl=mem[i].orig;
        if(!data){ bar_clear(); fprintf(stderr,"  FAILED inflate: %s\n",mem[i].name); continue; }
        FILE *f=fopen(target,"wb"); if(f){ fwrite(data,1,dl,f); fclose(f); done++; prog+=dl;
            if(verbose){ char a[16]; bar_clear(); printf("  -> %-36s %9s\n", mem[i].name, human((double)dl,a)); } }
        free(data);
    }
    bar("Extracting", total_out, total_out, n, n, "done");
    bar_clear();
    double dt_s=now_sec()-t0; char a[16],sp[16];
    printf("Extracted %d file(s) to %s/ (%s, %.2fs, %s/s)%s\n", done, out,
           human((double)prog,a), dt_s, human(dt_s>0?prog/dt_s:prog, sp),
           skipped?" — some members skipped (proprietary codec)":"");
    free(d); return 0;
}

static const char *codec_name(uint32_t c){
    return c==CODEC_DEFLATE?"deflate":c==CODEC_IMAGE?"image-jp2":c==CODEC_OLE2?"office":
           c==CODEC_OFFICE_PS?"office-ps":c==CODEC_LEAD?"lead-cmp":
           c==CODEC_IMAGE_X?"image-x":c==CODEC_PDF?"pdf-proc":"unknown";
}
/* can we actually decode this codec, or does it need the original engine? */
static int codec_decodable(uint32_t c){ return c<=CODEC_OLE2; }

static int cmd_list(int argc, char **argv){
    const char *arc=NULL; int verbose=0;
    for (int i=0;i<argc;i++){
        if(!strcmp(argv[i],"-v")||!strcmp(argv[i],"--verbose")) verbose=1;
        else if(!strcmp(argv[i],"--no-progress")||!strcmp(argv[i],"-p")||!strcmp(argv[i],"--progress")) {}
        else arc=argv[i];
    }
    if(!arc){ fprintf(stderr,"catre: archive required\n"); return 2; }
    size_t len; uint8_t *d=read_file(arc,&len); if(!d){ perror(arc); return 1; }
    static Member mem[MAXMEM]; int n=qcm_read(d,len,mem,MAXMEM,NULL);
    if(n<0){ fprintf(stderr,"catre: not a valid .qcf\n"); return 1; }
    printf("Archive: %s  (%d file(s))\n",arc,n);
    if (verbose) printf("%11s %11s %7s  %-9s %-19s name\n","size","packed","ratio","codec","modified");
    for (int i=0;i<n;i++){
        if (verbose){ char dts[32]; dos_str(mem[i].dt,dts);
            if (!mem[i].comp){   /* opaque codec: the packed size isn't computable — don't print 0 */
                printf("%11u %11s %7s  %-9s %-19s %s\n",mem[i].orig,"?","?",
                       codec_name(mem[i].codec),dts,mem[i].name);
            } else {
                double r=mem[i].orig?100.0*mem[i].comp/mem[i].orig:0;
                printf("%11u %11u %6.1f%%  %-9s %-19s %s\n",mem[i].orig,mem[i].comp,r,codec_name(mem[i].codec),dts,mem[i].name);
            }
        } else printf("  %s\n",mem[i].name);
    }
    free(d); return 0;
}

static int cmd_info(int argc, char **argv){
    const char *arc=NULL;
    for (int i=0;i<argc;i++){
        if(!strcmp(argv[i],"--no-progress")||!strcmp(argv[i],"-p")||!strcmp(argv[i],"--progress")
           ||!strcmp(argv[i],"-v")||!strcmp(argv[i],"--verbose")) {}
        else arc=argv[i];
    }
    if(!arc){ fprintf(stderr,"catre: archive required\n"); return 2; }
    size_t len; uint8_t *d=read_file(arc,&len); if(!d){ perror(arc); return 1; }
    uint32_t cdir; static Member mem[MAXMEM]; int n=qcm_read(d,len,mem,MAXMEM,&cdir);
    if(n<0){ printf("%s: not a QCM/.qcf container\n",arc); free(d); return 1; }
    size_t total=0; for(int i=0;i<n;i++) total+=mem[i].orig;
    printf("file:           %s\n",arc);
    printf("format:         QCM container (Choshuku/CAT .qcf)\n");
    printf("archive size:   %zu bytes\n",len);
    printf("members:        %d\n",n);
    printf("central dir @:  0x%x\n",cdir);
    printf("uncompressed:   %zu bytes\n",total);
    printf("overall ratio:  %.1f%%\n",total?100.0*len/total:0);
    free(d); return 0;
}

static int cmd_test(int argc, char **argv){
    const char *arc=NULL; int verbose=0;
    for(int i=0;i<argc;i++){ if(!strcmp(argv[i],"-v"))verbose=1; else arc=argv[i]; }
    if(!arc){ fprintf(stderr,"catre: archive required\n"); return 2; }
    size_t len; uint8_t *d=read_file(arc,&len); if(!d){ perror(arc); return 1; }
    static Member mem[MAXMEM]; int n=qcm_read(d,len,mem,MAXMEM,NULL);
    if(n<0){ fprintf(stderr,"catre: not a valid .qcf\n"); return 1; }
    int ok=0,bad=0,skip=0;
    for(int i=0;i<n;i++){
        uint32_t c=mem[i].codec;
        /* Every member we claim to support is really DECODED here — reporting OK for
         * a member we never touched would make `test` useless (it did, before v1.5). */
        int good;
        if (c==CODEC_IMAGE){                    /* decode the J2K codestream, discard pixels */
            good = catre_verify_image(d+mem[i].payoff, (uint32_t)(len-mem[i].payoff));
        } else if (c==CODEC_OFFICE_PS || c==CODEC_PDF){   /* only the whole-file mode verifies */
            uint8_t *whole=try_wholefile_zlib(d,len,mem[i].payoff,mem[i].orig);
            good = whole!=NULL; free(whole);
            if (!good){                         /* structural/sparse mode: opaque, not verifiable */
                skip++; if(verbose)printf("  SKIP %s (%s — not verifiable)\n",mem[i].name,codec_name(c));
                continue;
            }
        } else if (!codec_decodable(c)){        /* proprietary: can't verify without the engine */
            skip++; if(verbose)printf("  SKIP %s (%s — not verifiable)\n",mem[i].name,codec_name(c));
            continue;
        } else {                                /* deflate / office whole-file: inflate + size check */
            size_t zoff=mem[i].payoff + (c==CODEC_OLE2?36:0);
            uint32_t zlen=mem[i].comp - (c==CODEC_OLE2?36:0);
            uint8_t *x=inflate_mem(d+zoff,zlen,mem[i].orig); good=(x!=NULL); free(x);
        }
        if(good){ ok++; if(verbose)printf("  OK: %s (%s)\n",mem[i].name,codec_name(c)); }
        else { bad++; printf("  FAILED: %s (%s)\n",mem[i].name,codec_name(c)); }
    }
    if (skip) printf("Tested %d member(s): %d OK, %d failed, %d skipped (proprietary).\n",ok+bad+skip,ok,bad,skip);
    else      printf("Tested %d member(s): %d OK, %d failed.\n",ok+bad,ok,bad);
    free(d); return bad?1:0;
}

static void banner(void){
    printf(
"\n"
"  ____    _    _____   ____  _____\n"
" / ___|  / \\  |_   _| |  _ \\| ____|   CAT RE v" VERSION "\n"
"| |     / _ \\   | |   | |_) |  _|     Choshuku / CAT (.qcf) archiver\n"
"| |___ / ___ \\  | |   |  _ <| |___    free reverse-engineered build (C)\n"
" \\____/_/   \\_\\ |_|   |_| \\_\\_____|   DEFLATE / JPEG2000 / Office, no DLLs\n\n");
}
static void usage(void){
    banner();
    printf(
"usage: catre <command> [options]\n\n"
"Commands:\n"
"  compress, c   FILE|DIR... -o OUT.qcf [-q 0-100] [-v]   Create an archive\n"
"  add,      a   ARCHIVE.qcf FILE|DIR... [-q 0-100] [-v]  Add files to an archive\n"
"  delete,   d   ARCHIVE.qcf MEMBER... [-v]               Remove members\n"
"  extract,  x   ARCHIVE.qcf [-o DIR] [-m NAME...] [-v]   Extract files\n"
"  list,     l   ARCHIVE.qcf [-v]                         List contents\n"
"  info,     i   ARCHIVE.qcf                              Header & codec details\n"
"  test,     t   ARCHIVE.qcf [-v]                         Verify integrity\n\n"
"Extract options:\n"
"  -m NAME...   extract only these members (naming a folder takes its contents)\n\n"
"Compress / add options:\n"
"  -q 0-100     image quality: target PSNR for JPEG2000 (q0~26.7dB .. q100~32.2dB,\n"
"               calibrated to the original engine). Lossy; default 100.\n"
"  --store      no special codecs — store every file with lossless DEFLATE\n"
"               (use for PNG/diagrams you need bit-exact).\n"
"  --no-image   don't JPEG2000-encode images (still uses Office/DEFLATE)\n\n"
"  -p/--progress, --no-progress   force/disable the progress bar\n"
"  -V, --version    print version\n\n"
"Images that would grow under JPEG2000 fall back to DEFLATE automatically.\n");
}

int main(int argc, char **argv){
    g_progress = isatty(STDERR_FILENO);   /* auto-off when piped; --no-progress to force */
    if (argc<2){ usage(); return 2; }
    const char *c=argv[1];
    if (!strcmp(c,"-V")||!strcmp(c,"--version")){ printf("CAT RE v" VERSION "\n"); return 0; }
    if (!strcmp(c,"-h")||!strcmp(c,"--help")){ usage(); return 0; }
    if (!strcmp(c,"compress")||!strcmp(c,"c")) return cmd_compress(argc-2,argv+2);
    if (!strcmp(c,"add")     ||!strcmp(c,"a")) return cmd_add(argc-2,argv+2);
    if (!strcmp(c,"delete")  ||!strcmp(c,"d")) return cmd_delete(argc-2,argv+2);
    if (!strcmp(c,"extract") ||!strcmp(c,"x")) return cmd_extract(argc-2,argv+2);
    if (!strcmp(c,"list")    ||!strcmp(c,"l")) return cmd_list(argc-2,argv+2);
    if (!strcmp(c,"info")    ||!strcmp(c,"i")) return cmd_info(argc-2,argv+2);
    if (!strcmp(c,"test")    ||!strcmp(c,"t")) return cmd_test(argc-2,argv+2);
    fprintf(stderr,"catre: unknown command '%s'\n",c); usage(); return 2;
}

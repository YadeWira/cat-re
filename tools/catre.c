/* CAT RE v1.0 — native C archiver for Choshuku/CAT `.qcf` (QCM) files.
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

#define VERSION "1.0"
#define MAGIC_QCM 0x014D4351u
#define MAGIC_QCF 0x01464351u
#define CODEC_DEFLATE 0
#define MAXMEM 4096

/* ---------- image codec (catre_img.c, OpenJPEG) ---------- */
int catre_is_image(const char *name);
uint8_t *catre_encode_image(const uint8_t *data, size_t len, int quality, uint32_t *out_len);
int catre_decode_image(const uint8_t *payload, uint32_t len, const char *out_path);
#define CODEC_IMAGE 1
#define CODEC_OLE2  2
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
} Member;

static uint8_t *read_file(const char *path, size_t *len){
    FILE *f=fopen(path,"rb"); if(!f) return NULL;
    fseek(f,0,SEEK_END); long n=ftell(f); fseek(f,0,SEEK_SET);
    uint8_t *d=malloc(n>0?n:1);
    if(fread(d,1,n,f)!=(size_t)n){ fclose(f); free(d); return NULL; }
    fclose(f); *len=n; return d;
}

/* parse a QCM container; returns member count, fills mem[] (caps at MAXMEM) */
static int qcm_read(const uint8_t *d, size_t len, Member *mem, int maxm, uint32_t *cdir_out){
    if (len<0x24 || rd32(d)!=MAGIC_QCM) return -1;
    /* walk streams: stream1 @ +0x08 (no prefix); others have a 4-byte size prefix */
    static struct { uint32_t so, hdr, comp, payoff, codec; } st[MAXMEM]; int ns=0;
    size_t off=8; int first=1;
    while (off+0x1c<=len){
        size_t hdr = first?off:off+4;
        if (hdr+0x1c>len || rd32(d+hdr)!=MAGIC_QCF) break;
        uint32_t comp=rd32(d+hdr+0x08); uint8_t codec=d[hdr+0x18], ext=d[hdr+0x1b];
        size_t payoff=hdr+0x1c+ext;
        if (comp==0 && codec==0 && payoff+12<=len && !memcmp(d+payoff,"\x32\x01\x12\x00",4)){
            comp = 36 + rd32(d+payoff+8);   /* MSOC21 office: 36B header + zlib(whole OLE2) */
            codec = CODEC_OLE2;
        }
        if (ns<MAXMEM){ st[ns].so=hdr-4; st[ns].hdr=hdr; st[ns].comp=comp;
                        st[ns].payoff=payoff; st[ns].codec=codec; ns++; }
        off=payoff+comp; first=0;
    }
    size_t cdir=off; if(cdir_out)*cdir_out=cdir;
    /* directory: 9 zeros, dt(4), 4 zeros, [namelen=3][00 00]"TOP", then records */
    size_t p=cdir+9+4+4; uint8_t tl=d[p]; p+=3;
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
        if (type==0x02){ int found=0; for(int i=0;i<ns;i++) if(st[i].so==so){found=1;break;} if(!found) break; }
        rec[nr].off=roff; rec[nr].parent=parent; rec[nr].so=so; rec[nr].type=type;
        rec[nr].orig=orig; rec[nr].dt=dt;
        int cn=nl<255?nl:255; memcpy(rec[nr].name,d+p,cn); rec[nr].name[cn]=0;
        p+=nl; nr++;
    }
    /* resolve full paths (follow parent pointers up to TOP=cdir) and emit files */
    int m=0;
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
        int si=-1; for(int j=0;j<ns;j++) if(st[j].so==rec[i].so){si=j;break;}
        if (si<0) continue;
        strncpy(mem[m].name,path,sizeof mem[m].name-1);
        mem[m].orig=rec[i].orig; mem[m].comp=st[si].comp; mem[m].codec=st[si].codec;
        mem[m].dt=rec[i].dt; mem[m].payoff=st[si].payoff; mem[m].hdr=st[si].hdr; m++;
    }
    return m;
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

static void gather(const char *path, const char *base, InFile **list, int *n, int *cap){
    struct stat sb; if (stat(path,&sb)!=0) return;
    if (S_ISDIR(sb.st_mode)){
        DIR *dp=opendir(path); if(!dp) return; struct dirent *e;
        while ((e=readdir(dp))){
            if (!strcmp(e->d_name,".")||!strcmp(e->d_name,"..")) continue;
            char sub[2048]; snprintf(sub,sizeof sub,"%s/%s",path,e->d_name);
            gather(sub,base,list,n,cap);
        }
        closedir(dp);
    } else {
        size_t len; uint8_t *d=read_file(path,&len); if(!d) return;
        /* member name = path relative to base's parent */
        const char *rel=path; size_t bl=strlen(base);
        if (!strncmp(path,base,bl)) rel=path+bl+(path[bl]=='/'?1:0);
        if (*n>=*cap){ *cap=*cap*2+16; *list=realloc(*list,*cap*sizeof(InFile)); }
        (*list)[*n].name=strdup(rel); (*list)[*n].data=d; (*list)[*n].len=len; (*n)++;
    }
}

static int cmd_compress(int argc, char **argv){
    const char *out=NULL; int q=100, verbose=0;
    InFile *files=NULL; int nf=0, cap=0;
    for (int i=0;i<argc;i++){
        if (!strcmp(argv[i],"-o")||!strcmp(argv[i],"--output")) out=argv[++i];
        else if (!strcmp(argv[i],"-q")||!strcmp(argv[i],"--quality")) q=atoi(argv[++i]);
        else if (!strcmp(argv[i],"-v")||!strcmp(argv[i],"--verbose")) verbose=1;
        else if (!strcmp(argv[i],"--no-progress")) g_progress=0;
        else if (!strcmp(argv[i],"-p")||!strcmp(argv[i],"--progress")) g_progress=2;
        else if (!strcmp(argv[i],"--store")) { g_image=0; g_office=0; }
        else if (!strcmp(argv[i],"--no-image")) g_image=0;
        else {
            char base[2048]; strncpy(base,argv[i],sizeof base-1); base[sizeof base-1]=0;
            struct stat sb; char parent[2048];
            if (stat(argv[i],&sb)==0 && S_ISDIR(sb.st_mode)){
                strncpy(parent,argv[i],sizeof parent-1);
                char *s=strrchr(parent,'/'); if(s)*s=0; else strcpy(parent,".");
                gather(argv[i],parent,&files,&nf,&cap);
            } else {
                strcpy(parent,"."); /* file: relative name = basename */
                size_t len; uint8_t *d=read_file(argv[i],&len);
                if(d){ const char*bn=strrchr(argv[i],'/'); bn=bn?bn+1:argv[i];
                    if(nf>=cap){cap=cap*2+16;files=realloc(files,cap*sizeof(InFile));}
                    files[nf].name=strdup(bn); files[nf].data=d; files[nf].len=len; nf++; }
            }
        }
    }
    if (!out){ fprintf(stderr,"catre: -o/--output required\n"); return 2; }
    if (!nf){ fprintf(stderr,"catre: no input files\n"); return 2; }

    uint32_t dt=now_dos();
    Buf out_b={0}; bu32(&out_b,MAGIC_QCM); bu32(&out_b,0);   /* [+04] patched later */
    uint32_t *so=malloc(nf*sizeof(uint32_t)); size_t total_in=0;
    for (int i=0;i<nf;i++) total_in+=files[i].len;
    double t0=now_sec(); size_t done=0;
    for (int i=0;i<nf;i++){
        bar("Compressing", done, total_in, i, nf, files[i].name);
        uint8_t *payload=NULL; uint32_t psize=0, inner_csize=0, field04=0;
        int codec=CODEC_DEFLATE; uint8_t t4[4]={0x00,0x05,0x04,0x01};   /* deflate */
        size_t len=files[i].len; const uint8_t *fd=files[i].data;
        int isole2 = len>=8 && !memcmp(fd,"\xd0\xcf\x11\xe0\xa1\xb1\x1a\xe1",8);
        if (g_image && catre_is_image(files[i].name))
            payload = catre_encode_image(fd, len, q, &psize);  /* JPEG2000 */
        if (payload){ codec=CODEC_IMAGE; field04=(uint32_t)len; inner_csize=psize;
                      t4[0]=0x01;t4[1]=0x01;t4[2]=0x04;t4[3]=0x01; }
        else if (g_office && isole2){               /* MSOC21: 36-byte hdr + zlib(whole OLE2) */
            uLongf zc=compressBound(len); uint8_t *z=malloc(zc); compress2(z,&zc,fd,len,9);
            psize=36+(uint32_t)zc; payload=malloc(psize); uint8_t *p=payload;
            memcpy(p,"\x32\x01\x12\x00\x00\x00",6); p+=6; memcpy(p,"\x33\x02",2); p+=2;
            wr32(p,(uint32_t)zc); p+=4; memset(p,0,6); p+=6;
            memcpy(p,"\x04\x0a\x00\x05",4); p+=4; memcpy(p,MSOC_TAIL,14); p+=14;
            memcpy(p,z,zc); free(z);
            codec=CODEC_OLE2; field04=(uint32_t)len; inner_csize=0;  /* office: +08 stays 0 */
            t4[0]=0x00;t4[1]=0x00;t4[2]=0x02;t4[3]=0x01;
        } else {                                    /* deflate (default / fallback) */
            uLongf cb=compressBound(len); payload=malloc(cb);
            compress2(payload,&cb,fd,len,9); psize=(uint32_t)cb; inner_csize=psize;
        }
        const char *bn=strrchr(files[i].name,'/');
        uint8_t ext=(uint8_t)(bn ? bn[1] : files[i].name[0]);   /* 1st char of basename */
        Buf inner={0};
        bu32(&inner,MAGIC_QCF); bu32(&inner,field04); bu32(&inner,inner_csize); bu32(&inner,0);
        bu32(&inner,0x0011001E);
        uint8_t tail[8]={0x01,0x00,0x04,0x00, t4[0],t4[1],t4[2],t4[3]};
        bput(&inner,tail,8);
        bu8(&inner,ext); bput(&inner,payload,psize);
        if (i==0){ so[0]=(uint32_t)out_b.n-4; bput(&out_b,inner.p,inner.n);
                   wr32(out_b.p+4,(uint32_t)out_b.n-4); }
        else { so[i]=(uint32_t)out_b.n; bu32(&out_b,4+(uint32_t)inner.n); bput(&out_b,inner.p,inner.n); }
        free(inner.p); free(payload);
        done+=files[i].len;
        if (verbose){ char a[16],b[16]; double r=files[i].len?100.0*psize/files[i].len:0;
            bar_clear();
            printf("  + %-32s %9s -> %9s (%.0f%%) %s\n", files[i].name,
                   human(files[i].len,a), human((double)psize,b), r,
                   codec==CODEC_IMAGE?"[jp2]":codec==CODEC_OLE2?"[office]":"[deflate]"); }
    }
    bar("Compressing", total_in, total_in, nf, nf, "done");
    uint32_t cdir=(uint32_t)out_b.n;
    for (int z=0;z<9;z++) bu8(&out_b,0);
    bu32(&out_b,dt); bu32(&out_b,0);
    uint8_t toph[3]={3,0,0}; bput(&out_b,toph,3); bput(&out_b,"TOP",3);

    /* native folder records: type=0x00 folder entries + parent pointers.
       Collect unique directory prefixes, emit them shallow-first (so a parent
       folder's record precedes its children), then emit file records. */
    static char dirs[MAXMEM][1024]; uint32_t diroff[MAXMEM]; int ndir=0;
    for (int i=0;i<nf;i++){
        char *nm=files[i].name;
        for (char *s=nm; *s; s++) if (*s=='/'){
            int L=s-nm; char pre[1024]; if(L>=1024)L=1023; memcpy(pre,nm,L); pre[L]=0;
            int found=0; for(int j=0;j<ndir;j++) if(!strcmp(dirs[j],pre)){found=1;break;}
            if(!found && ndir<MAXMEM){ strcpy(dirs[ndir++],pre); }
        }
    }
    for(int i=0;i<ndir;i++) for(int j=i+1;j<ndir;j++){           /* sort by depth asc */
        int di=0,dj=0; for(char*s=dirs[i];*s;s++)di+=(*s=='/'); for(char*s=dirs[j];*s;s++)dj+=(*s=='/');
        if(dj<di){ char t[1024]; strcpy(t,dirs[i]);strcpy(dirs[i],dirs[j]);strcpy(dirs[j],t); }
    }
    for(int i=0;i<ndir;i++){                                     /* folder records */
        uint32_t myoff=(uint32_t)out_b.n;
        char *ls=strrchr(dirs[i],'/'); uint32_t par=cdir; const char *bn=dirs[i];
        if(ls){ int L=ls-dirs[i]; char pp[1024]; memcpy(pp,dirs[i],L); pp[L]=0; bn=ls+1;
            for(int j=0;j<ndir;j++) if(!strcmp(dirs[j],pp)){par=diroff[j];break;} }
        bu32(&out_b,par); bu32(&out_b,0); bu8(&out_b,0); bu32(&out_b,dt);   /* type=0 folder */
        bu32(&out_b,0); size_t nl=strlen(bn); bu8(&out_b,(uint8_t)nl); bu8(&out_b,0); bu8(&out_b,0);
        bput(&out_b,bn,nl);
        diroff[i]=myoff;
    }
    for (int i=0;i<nf;i++){                                      /* file records */
        char *nm=files[i].name; char *ls=strrchr(nm,'/'); uint32_t par=cdir; const char *bn=nm;
        if(ls){ int L=ls-nm; char pp[1024]; memcpy(pp,nm,L); pp[L]=0; bn=ls+1;
            for(int j=0;j<ndir;j++) if(!strcmp(dirs[j],pp)){par=diroff[j];break;} }
        bu32(&out_b,par); bu32(&out_b,so[i]); bu8(&out_b,2); bu32(&out_b,dt);
        bu32(&out_b,(uint32_t)files[i].len);
        size_t nl=strlen(bn); bu8(&out_b,(uint8_t)nl); bu8(&out_b,0); bu8(&out_b,0);
        bput(&out_b,bn,nl);
    }
    FILE *f=fopen(out,"wb"); if(!f){ bar_clear(); perror("open out"); return 1; }
    fwrite(out_b.p,1,out_b.n,f); fclose(f);
    bar_clear();
    double dt_s=now_sec()-t0, ratio=total_in?100.0*out_b.n/total_in:0;
    char a[16],b[16],sp[16];
    printf("Created %s: %d file(s), %s -> %s (%.1f%%), %.2fs, %s/s\n",
           out, nf, human((double)total_in,a), human((double)out_b.n,b), ratio, dt_s,
           human(dt_s>0?total_in/dt_s:total_in, sp));
    return 0;
}

/* ---------- mkdir -p for a file path ---------- */
static void mkdirs(const char *path){
    char tmp[2048]; strncpy(tmp,path,sizeof tmp-1); tmp[sizeof tmp-1]=0;
    for (char *s=tmp+1; *s; s++) if (*s=='/'){ *s=0; mkdir(tmp,0755); *s='/'; }
}

static int cmd_extract(int argc, char **argv){
    const char *arc=NULL,*out="."; int verbose=0;
    for (int i=0;i<argc;i++){
        if (!strcmp(argv[i],"-o")||!strcmp(argv[i],"--output")) out=argv[++i];
        else if (!strcmp(argv[i],"-v")||!strcmp(argv[i],"--verbose")) verbose=1;
        else if (!strcmp(argv[i],"--no-progress")) g_progress=0;
        else if (!strcmp(argv[i],"-p")||!strcmp(argv[i],"--progress")) g_progress=2;
        else arc=argv[i];
    }
    if(!arc){ fprintf(stderr,"catre: archive required\n"); return 2; }
    size_t len; uint8_t *d=read_file(arc,&len); if(!d){ perror(arc); return 1; }
    static Member mem[MAXMEM]; int n=qcm_read(d,len,mem,MAXMEM,NULL);
    if (n<0){ fprintf(stderr,"catre: not a valid .qcf\n"); return 1; }
    mkdir(out,0755);
    size_t total_out=0; for(int i=0;i<n;i++) total_out+=mem[i].codec?mem[i].comp:mem[i].orig;
    double t0=now_sec(); size_t prog=0;
    int done=0;
    for (int i=0;i<n;i++){
        bar("Extracting", prog, total_out, i, n, mem[i].name);
        char target[2300];
        if (mem[i].codec==CODEC_IMAGE){          /* JPEG2000 -> decode to PNG */
            /* replace the member's extension with .png (don't append: file.png -> file.png, not file.png.png) */
            char stem[2048]; snprintf(stem,sizeof stem,"%s",mem[i].name);
            char *dot=strrchr(stem,'.'), *slash=strrchr(stem,'/');
            if (dot && (!slash || dot>slash)) *dot=0;     /* strip ext only if after the last '/' */
            snprintf(target,sizeof target,"%s/%s.png",out,stem);
            mkdirs(target);
            if (catre_decode_image(d+mem[i].payoff, mem[i].comp, target)){ done++; prog+=mem[i].comp;
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
    printf("Extracted %d file(s) to %s/ (%s, %.2fs, %s/s)\n", done, out,
           human((double)prog,a), dt_s, human(dt_s>0?prog/dt_s:prog, sp));
    free(d); return 0;
}

static const char *codec_name(uint32_t c){ return c==0?"deflate":c==1?"image-jp2":c==2?"office":"other"; }

static int cmd_list(int argc, char **argv){
    const char *arc=NULL; int verbose=0;
    for (int i=0;i<argc;i++){ if(!strcmp(argv[i],"-v")||!strcmp(argv[i],"--verbose"))verbose=1; else arc=argv[i]; }
    if(!arc){ fprintf(stderr,"catre: archive required\n"); return 2; }
    size_t len; uint8_t *d=read_file(arc,&len); if(!d){ perror(arc); return 1; }
    static Member mem[MAXMEM]; int n=qcm_read(d,len,mem,MAXMEM,NULL);
    if(n<0){ fprintf(stderr,"catre: not a valid .qcf\n"); return 1; }
    printf("Archive: %s  (%d file(s))\n",arc,n);
    if (verbose) printf("%11s %11s %7s  %-9s %-19s name\n","size","packed","ratio","codec","modified");
    for (int i=0;i<n;i++){
        if (verbose){ char dts[32]; dos_str(mem[i].dt,dts);
            double r=mem[i].orig?100.0*mem[i].comp/mem[i].orig:0;
            printf("%11u %11u %6.1f%%  %-9s %-19s %s\n",mem[i].orig,mem[i].comp,r,codec_name(mem[i].codec),dts,mem[i].name);
        } else printf("  %s\n",mem[i].name);
    }
    free(d); return 0;
}

static int cmd_info(int argc, char **argv){
    const char *arc=argc?argv[0]:NULL; if(!arc){ fprintf(stderr,"catre: archive required\n"); return 2; }
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
    int ok=0,bad=0;
    for(int i=0;i<n;i++){
        int good=1;
        if (mem[i].codec==CODEC_DEFLATE){ uint8_t *x=inflate_mem(d+mem[i].payoff,mem[i].comp,mem[i].orig);
            good=(x!=NULL); free(x); }
        if(good){ ok++; if(verbose)printf("  OK: %s\n",mem[i].name); }
        else { bad++; printf("  FAILED: %s\n",mem[i].name); }
    }
    printf("Tested %d member(s): %d OK, %d failed.\n",ok+bad,ok,bad);
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
"  extract,  x   ARCHIVE.qcf [-o DIR] [-v]                Extract files\n"
"  list,     l   ARCHIVE.qcf [-v]                         List contents\n"
"  info,     i   ARCHIVE.qcf                              Header & codec details\n"
"  test,     t   ARCHIVE.qcf [-v]                         Verify integrity\n\n"
"  -p/--progress, --no-progress   force/disable the progress bar\n"
"  -V, --version    print version\n");
}

int main(int argc, char **argv){
    g_progress = isatty(STDERR_FILENO);   /* auto-off when piped; --no-progress to force */
    if (argc<2){ usage(); return 2; }
    const char *c=argv[1];
    if (!strcmp(c,"-V")||!strcmp(c,"--version")){ printf("CAT RE v" VERSION "\n"); return 0; }
    if (!strcmp(c,"-h")||!strcmp(c,"--help")){ usage(); return 0; }
    if (!strcmp(c,"compress")||!strcmp(c,"c")) return cmd_compress(argc-2,argv+2);
    if (!strcmp(c,"extract") ||!strcmp(c,"x")) return cmd_extract(argc-2,argv+2);
    if (!strcmp(c,"list")    ||!strcmp(c,"l")) return cmd_list(argc-2,argv+2);
    if (!strcmp(c,"info")    ||!strcmp(c,"i")) return cmd_info(argc-2,argv+2);
    if (!strcmp(c,"test")    ||!strcmp(c,"t")) return cmd_test(argc-2,argv+2);
    fprintf(stderr,"catre: unknown command '%s'\n",c); usage(); return 2;
}

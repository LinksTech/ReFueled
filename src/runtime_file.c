/* Guest file system: PS4 mount points mapped onto host directories.
 *   /app0, /hostapp  -> game package root (read-only by convention)
 *   /temp0, /download0, /data, and mounts added by SaveData -> user directory
 * Guest descriptors are small integers in our own table; stdio 0-2 pass through.
 * Paths containing ".." components are rejected rather than normalized. */
#define _GNU_SOURCE
#include "runtime.h"
#include "platform/pf_fs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/stat.h>
#define ERR(n) ((int32_t)(UINT32_C(0x80020000)|(n)))
#define MAX_FILES 1024
#define MAX_MOUNTS 16

typedef struct { int64_t sec, nsec; } GuestTimespec;
typedef struct {
    uint32_t dev, ino;
    uint16_t mode, nlink;
    uint32_t uid, gid, rdev;
    GuestTimespec atime, mtime, ctime;
    int64_t size, blocks;
    uint32_t blksize, flags, gen;
    int32_t lspare;
    GuestTimespec birthtime;
} GuestStat;
_Static_assert(sizeof(GuestStat)==120,"FreeBSD stat layout");

typedef struct { char *names; size_t count, *offsets; unsigned char *types; } Listing;
/* commit: a save file opened for writing is written to a temporary copy (temp), which replaces
 * the file (commit) in one rename when closed: a crash or a kill mid-save leaves the old file. */
typedef struct { int used, host, dirty, save_logged; Listing *dir; size_t position; char path[512]; char *commit, *temp; } File;
typedef struct { char guest[64]; char host[512]; } Mount;
static File files[MAX_FILES];
static Mount mounts[MAX_MOUNTS];
static size_t mount_count, opens, reads, writes, missing;
static uint64_t bytes_read;
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;

static int save_path(const char *p) { return p && !strncmp(p,"/savedata",9); }
static int temp_name(const char *name) {
    size_t n=strlen(name);
    return n>6 && !strcmp(name+n-6,".bbtmp");
}
/* refueled: true for a directory entry that is a directory. Where readdir reports the type (Linux)
 * that decides, exactly as before; only DT_UNKNOWN (Windows: no d_type) asks pf_fs_stat. */
static int entry_is_dir(const struct dirent *e,const char *path) {
    unsigned char type=PF_DIRENT_TYPE(e);
    if (type!=DT_UNKNOWN) return type==DT_DIR;
    PfStat st;
    return !pf_fs_stat(path,&st) && S_ISDIR(st.mode);
}
/* Copies left by a crash (none of them open): removed when their directory is mounted. */
static void remove_stale_temps(const char *dir,int depth) {
    DIR *d=opendir(dir);
    if (!d) return;
    for (struct dirent *e; (e=readdir(d));) {
        if (e->d_name[0]=='.') continue;
        char path[1024];
        if ((size_t)snprintf(path,sizeof(path),"%s/%s",dir,e->d_name)>=sizeof(path)) continue;
        if (temp_name(e->d_name)) {
            int open_now=0;
            for (int i=3;i<MAX_FILES;++i) if (files[i].used && files[i].temp && !strcmp(files[i].temp,path)) open_now=1;
            if (!open_now && !unlink(path)) printf("Runtime: removed %s (a save interrupted earlier; the file it would have replaced is intact)\n",path);
        } else if (depth<4 && entry_is_dir(e,path)) remove_stale_temps(path,depth+1);
    }
    closedir(d);
}

int runtime_file_mount(const char *guest,const char *host) {
    pthread_mutex_lock(&lock);
    for (size_t i=0;i<mount_count;++i) if (!strcmp(mounts[i].guest,guest)) {
        snprintf(mounts[i].host,sizeof(mounts[i].host),"%s",host);
        pthread_mutex_unlock(&lock); return 0;
    }
    if (mount_count==MAX_MOUNTS || strlen(guest)>=64 || strlen(host)>=512) { pthread_mutex_unlock(&lock); return -1; }
    snprintf(mounts[mount_count].guest,64,"%s",guest);
    snprintf(mounts[mount_count].host,512,"%s",host);
    ++mount_count;
    if (save_path(guest)) remove_stale_temps(host,0);
    pthread_mutex_unlock(&lock);
    return 0;
}
void runtime_file_unmount(const char *guest) {
    pthread_mutex_lock(&lock);
    for (size_t i=0;i<mount_count;++i) if (!strcmp(mounts[i].guest,guest)) {
        mounts[i]=mounts[--mount_count]; break;
    }
    pthread_mutex_unlock(&lock);
}
static void free_listing(Listing *l) { if (l) { free(l->names); free(l->offsets); free(l->types); free(l); } }
static char user_root[512]="user";
const char *runtime_file_user_dir(void) { return user_root; }

/* refueled: virtual mod overlay (docs/refueled/MODLOADER.md section 3). BB_MOD_MANIFEST names a
 * manifest written by scripts/mods.py --manifest: "# refueled mod manifest 1", then one line per
 * file "<path relative to /app0>\t<host path>". /app0 and /hostapp paths found in the index open
 * the mod file; everything else falls through to the game folder; directory listings are the union.
 * Not set (Linux, which keeps the link tree): overlay_count stays 0 and nothing below acts. */
typedef struct { char *guest, *host; } OverlayFile;
static OverlayFile *overlay;
static size_t overlay_count;
static char app0_host[512];
static int overlay_compare(const void *a,const void *b) {
    return strcmp(((const OverlayFile *)a)->guest,((const OverlayFile *)b)->guest);
}
static const OverlayFile *overlay_find(const char *rel) {
    const OverlayFile key={(char *)rel,NULL};
    return overlay_count ? bsearch(&key,overlay,overlay_count,sizeof(key),overlay_compare) : NULL;
}
static void overlay_fail(const char *file,size_t line,const char *why) {
    if (line) fprintf(stderr,"Runtime: mod manifest %s, line %zu: %s\n",file,line,why);
    else fprintf(stderr,"Runtime: mod manifest %s: %s\n",file,why);
    exit(1);
}
static void overlay_oom(void) { fputs("Out of memory reading the mod manifest\n",stderr); exit(1); }
static void overlay_load(const char *file) {
    for (size_t i=0;i<overlay_count;++i) { free(overlay[i].guest); free(overlay[i].host); }
    free(overlay); overlay=NULL; overlay_count=0;
    if (!file || !*file) return;
    FILE *f=fopen(file,"rb");
    if (!f) { fprintf(stderr,"Runtime: cannot open mod manifest %s (%s)\n",file,strerror(errno)); exit(1); }
    size_t cap=65536,length=0;
    char *data=malloc(cap);
    if (!data) overlay_oom();
    for (;;) {
        size_t n=fread(data+length,1,cap-length-1,f);
        length+=n;
        if (n==0) break;
        if (length+1>=cap) { cap*=2; data=realloc(data,cap); if (!data) overlay_oom(); }
    }
    int read_error=ferror(f);
    fclose(f);
    if (read_error) overlay_fail(file,0,"read error");
    data[length]=0;
    size_t line_number=0,capacity=0;
    for (char *line=data;*line;) {
        char *end=strchr(line,'\n'), *next;
        if (end) { *end=0; next=end+1; } else next=line+strlen(line);
        size_t n=strlen(line);
        if (n && line[n-1]=='\r') line[--n]=0;
        ++line_number;
        if (line_number==1) {
            if (strcmp(line,"# refueled mod manifest 1")) overlay_fail(file,1,"first line must be \"# refueled mod manifest 1\"");
        } else if (n && line[0]!='#') {
            char *tab=strchr(line,'\t');
            if (!tab) overlay_fail(file,line_number,"no tab between guest path and host path");
            *tab=0;
            const char *guest=line, *host=tab+1;
            if (!*host || strchr(host,'\t')) overlay_fail(file,line_number,"empty host path or extra tab");
            if (!*guest || guest[0]=='/') overlay_fail(file,line_number,"guest path empty or absolute");
            for (const char *p=guest;;) {
                const char *slash=strchr(p,'/');
                size_t len=slash ? (size_t)(slash-p) : strlen(p);
                if (!len || (len==1 && p[0]=='.') || (len==2 && p[0]=='.' && p[1]=='.')) overlay_fail(file,line_number,"guest path has an empty, . or .. component");
                if (!slash) break;
                p=slash+1;
            }
            if (overlay_count==capacity) {
                capacity=capacity ? capacity*2 : 256;
                overlay=realloc(overlay,capacity*sizeof(*overlay));
                if (!overlay) overlay_oom();
            }
            overlay[overlay_count].guest=strdup(guest);
            overlay[overlay_count].host=strdup(host);
            if (!overlay[overlay_count].guest || !overlay[overlay_count].host) overlay_oom();
            ++overlay_count;
        }
        line=next;
    }
    free(data);
    if (!line_number) overlay_fail(file,0,"empty file");
    qsort(overlay,overlay_count,sizeof(*overlay),overlay_compare);
    for (size_t i=0;i<overlay_count;++i) {
        if (i && !strcmp(overlay[i].guest,overlay[i-1].guest)) overlay_fail(file,0,"the same guest path twice");
        /* a file may not also be a directory of another file */
        for (const char *slash=strchr(overlay[i].guest,'/');slash;slash=strchr(slash+1,'/')) {
            char prefix[1024]; size_t len=(size_t)(slash-overlay[i].guest);
            if (len>=sizeof(prefix)) continue;
            memcpy(prefix,overlay[i].guest,len); prefix[len]=0;
            if (overlay_find(prefix)) overlay_fail(file,0,"a path is both a file and a directory");
        }
    }
    printf("Runtime: mod manifest %s: %zu files\n",file,overlay_count);
}
/* The path below /app0 or /hostapp, normalized ('/' separated, no empty or . components), as the
 * manifest spells it; 0 when the guest path is somewhere else. ".." was rejected by translate. */
static int overlay_key(const char *guest,char *rel,size_t size) {
    char buffer[1024];
    if (!guest || !*guest) return 0;
    if (guest[0]!='/') snprintf(buffer,sizeof(buffer),"/app0/%s",guest);
    else snprintf(buffer,sizeof(buffer),"%s",guest);
    const char *p;
    if (!strncmp(buffer,"/app0",5) && (!buffer[5] || buffer[5]=='/')) p=buffer+5;
    else if (!strncmp(buffer,"/hostapp",8) && (!buffer[8] || buffer[8]=='/')) p=buffer+8;
    else return 0;
    size_t n=0;
    while (*p) {
        while (*p=='/') ++p;
        if (!*p) break;
        const char *end=strchr(p,'/');
        size_t len=end ? (size_t)(end-p) : strlen(p);
        if (len==1 && p[0]=='.') { p+=len; continue; }
        if (n+len+2>size) return 0;
        if (n) rel[n++]='/';
        memcpy(rel+n,p,len); n+=len; p+=len;
    }
    rel[n]=0;
    return 1;
}
/* A directory some mod file lies below. */
static int overlay_is_dir(const char *rel) {
    size_t len=strlen(rel);
    for (size_t i=0;i<overlay_count;++i)
        if (!strncmp(overlay[i].guest,rel,len) && (!len || overlay[i].guest[len]=='/')) return 1;
    return 0;
}
/* A directory that exists only in the overlay has no host directory: the game folder stands in
 * for the host side (stat, open), the listing comes from the overlay alone. */
static int overlay_standin(const char *guest,char *path,size_t size) {
    char rel[1024];
    if (!overlay_count || !overlay_key(guest,rel,sizeof(rel)) || !overlay_is_dir(rel)) return 0;
    snprintf(path,size,"%s",app0_host);
    return 1;
}
/* The listing of the game folder (or NULL) united with the overlay's entries of `guest`'s directory;
 * a name in both is listed once with the overlay's type. Takes over `game`. */
static Listing *overlay_merge_dir(Listing *game,const char *guest) {
    char rel[1024];
    if (!overlay_count || !overlay_key(guest,rel,sizeof(rel))) return game;
    size_t len=strlen(rel);
    const char **names=malloc((overlay_count+1)*sizeof(*names));
    size_t *lengths=malloc((overlay_count+1)*sizeof(*lengths));
    unsigned char *types=malloc(overlay_count+1);
    if (!names || !lengths || !types) { fputs("Out of memory listing directory\n",stderr); exit(1); }
    size_t m=0;
    for (size_t i=0;i<overlay_count;++i) {
        const char *g=overlay[i].guest, *rest=g;
        if (len) { if (strncmp(g,rel,len) || g[len]!='/') continue; rest=g+len+1; }
        const char *slash=strchr(rest,'/');
        size_t n=slash ? (size_t)(slash-rest) : strlen(rest);
        if (m && lengths[m-1]==n && !memcmp(names[m-1],rest,n)) continue; /* siblings are contiguous */
        names[m]=rest; lengths[m]=n; types[m]=slash ? 4 : 8; ++m;
    }
    size_t game_count=game ? game->count : 0, game_bytes=0, extra=0, extra_bytes=0;
    if (game_count) { const char *last=game->names+game->offsets[game_count-1]; game_bytes=game->offsets[game_count-1]+strlen(last)+1; }
    unsigned char *is_new=calloc(m+1,1);
    if (!is_new) { fputs("Out of memory listing directory\n",stderr); exit(1); }
    for (size_t j=0;j<m;++j) {
        int found=0;
        for (size_t k=0;k<game_count && !found;++k) {
            const char *name=game->names+game->offsets[k];
            if (!strncmp(name,names[j],lengths[j]) && !name[lengths[j]]) { game->types[k]=types[j]; found=1; }
        }
        if (!found) { is_new[j]=1; ++extra; extra_bytes+=lengths[j]+1; }
    }
    Listing *l=calloc(1,sizeof(*l));
    if (l) {
        l->count=game_count+extra;
        l->names=malloc(game_bytes+extra_bytes+1);
        l->offsets=malloc((l->count+1)*sizeof(size_t));
        l->types=malloc(l->count+1);
    }
    if (!l || !l->names || !l->offsets || !l->types) { fputs("Out of memory listing directory\n",stderr); exit(1); }
    if (game_count) {
        memcpy(l->names,game->names,game_bytes);
        memcpy(l->offsets,game->offsets,game_count*sizeof(size_t));
        memcpy(l->types,game->types,game_count);
    }
    size_t at=game_count, bytes=game_bytes;
    for (size_t j=0;j<m;++j) if (is_new[j]) {
        memcpy(l->names+bytes,names[j],lengths[j]); l->names[bytes+lengths[j]]=0;
        l->offsets[at]=bytes; l->types[at]=types[j];
        bytes+=lengths[j]+1; ++at;
    }
    free_listing(game); free(names); free(lengths); free(types); free(is_new);
    return l;
}

void runtime_file_configure(const char *app0,const char *user) {
    char path[600];
    snprintf(user_root,sizeof(user_root),"%s",user);
    snprintf(app0_host,sizeof(app0_host),"%s",app0);
    overlay_load(getenv("BB_MOD_MANIFEST")); /* refueled: unset or empty = no overlay (the Linux case) */
    runtime_file_mount("/app0",app0);
    runtime_file_mount("/hostapp",app0);
    const char *writable[]={"temp0","download0","data"};
    mkdir(user,0755);
    for (int i=0;i<3;++i) {
        snprintf(path,sizeof(path),"%s/%s",user,writable[i]);
        mkdir(path,0755);
        char guest[32]; snprintf(guest,sizeof(guest),"/%s",writable[i]);
        runtime_file_mount(guest,path);
    }
}
/* refueled: ".." is also a component border next to a backslash where the host takes it as a
 * separator (Windows). Elsewhere a backslash is an ordinary name character: unchanged. */
#ifdef _WIN32
#define PATH_SEP(c) ((c)=='/' || (c)=='\\')
#else
#define PATH_SEP(c) ((c)=='/')
#endif
/* Resolve a guest path to a host path; returns 0 or a host errno. */
static int translate(const char *guest,char *out,size_t size) {
    if (!guest || !*guest) return ENOENT;
    char buffer[1024];
    if (guest[0]!='/') snprintf(buffer,sizeof(buffer),"/app0/%s",guest);
    else snprintf(buffer,sizeof(buffer),"%s",guest);
    for (const char *p=buffer;(p=strstr(p,".."));p+=2)
        if ((p==buffer || PATH_SEP(p[-1])) && (p[2]==0 || PATH_SEP(p[2]))) return EACCES;
    int result=0, overlaid=0;
    if (overlay_count) { /* refueled: a mod file found in the overlay index replaces the game folder's */
        char rel[1024];
        const OverlayFile *hit=overlay_key(buffer,rel,sizeof(rel)) ? overlay_find(rel) : NULL;
        if (hit) { overlaid=1; if ((size_t)snprintf(out,size,"%s",hit->host)>=size) result=ENAMETOOLONG; }
    }
    if (!overlaid) {
        pthread_mutex_lock(&lock);
        size_t best=0; const Mount *m=NULL;
        for (size_t i=0;i<mount_count;++i) {
            size_t n=strlen(mounts[i].guest);
            if (!strncmp(buffer,mounts[i].guest,n) && (buffer[n]=='/' || !buffer[n]) && n>best) { best=n; m=&mounts[i]; }
        }
        if (!m) result=ENOENT;
        else if ((size_t)snprintf(out,size,"%s%s",m->host,buffer+best)>=size) result=ENAMETOOLONG;
        pthread_mutex_unlock(&lock);
    }
    if (result==ENOENT) fprintf(stderr,"Runtime: no mount for guest path %s\n",guest);
    /* BB_FILE_TRACE=1: every guest path the game opens, stats or checks (mods, missing files). */
    static int trace=-1;
    if (trace<0) { const char *t=getenv("BB_FILE_TRACE"); trace=t && t[0]=='1'; }
    if (trace) printf("File trace: %s -> %s\n",guest,result ? "(no mount)" : out);
    return result;
}
/* The PS4's file system ignores case, and the game asks for lower-case names
 * (adhoc/font/dbgfont14h.ccm): files from mods or dumps made elsewhere keep their own case.
 * After a miss, the host path is matched component by component ignoring case; true when it was
 * corrected (same length: only case differs). Only on misses: no cost for found files. */
static int fix_case(char *path) {
    if (path[0]!='/') return 0;
    char out[1024]="", trial[1024], part[256];
    const char *p=path;
    while (*p) {
        while (*p=='/') ++p;
        if (!*p) break;
        const char *end=strchr(p,'/');
        size_t len=end ? (size_t)(end-p) : strlen(p);
        if (len>=sizeof(part)) return 0;
        memcpy(part,p,len); part[len]=0;
        if ((size_t)snprintf(trial,sizeof(trial),"%s/%s",out,part)>=sizeof(trial)) return 0;
        if (access(trial,F_OK)) {
            DIR *d=opendir(out[0] ? out : "/");
            if (!d) return 0;
            int found=0;
            for (struct dirent *e;(e=readdir(d));)
                if (!strcasecmp(e->d_name,part)) { snprintf(trial,sizeof(trial),"%s/%s",out,e->d_name); found=1; break; }
            closedir(d);
            if (!found) return 0;
        }
        memcpy(out,trial,strlen(trial)+1);
        p+=len;
    }
    if (!strcmp(out,path) || strlen(out)!=strlen(path)) return 0;
    memcpy(path,out,strlen(out)+1);
    return 1;
}
static int host_flags(int flags) {
    int r;
    switch (flags&3) { case 0: r=O_RDONLY; break; case 1: r=O_WRONLY; break; default: r=O_RDWR; }
    if (flags&0x4) r|=O_NONBLOCK;
    if (flags&0x8) r|=O_APPEND;
#ifdef O_SYNC /* refueled: MinGW-w64 has no O_SYNC */
    if (flags&0x80) r|=O_SYNC;
#endif
    if (flags&0x200) r|=O_CREAT;
    if (flags&0x400) r|=O_TRUNC;
    if (flags&0x800) r|=O_EXCL;
    if (flags&0x20000) r|=O_DIRECTORY;
    return r|O_CLOEXEC;
}
static Listing *list_directory(const char *path) {
    DIR *d=opendir(path);
    if (!d) return NULL;
    Listing *l=calloc(1,sizeof(*l));
    size_t capacity=0,bytes=0,cap_names=0;
    struct dirent *e;
    while (l && (e=readdir(d))) {
        if (temp_name(e->d_name)) continue;
        size_t n=strlen(e->d_name)+1;
        if (l->count==capacity) {
            capacity=capacity ? capacity*2 : 64;
            l->offsets=realloc(l->offsets,capacity*sizeof(size_t));
            l->types=realloc(l->types,capacity);
        }
        if (bytes+n>cap_names) { cap_names=(bytes+n)*2; l->names=realloc(l->names,cap_names); }
        if (!l->offsets || !l->types || !l->names) { fputs("Out of memory listing directory\n",stderr); exit(1); }
        memcpy(l->names+bytes,e->d_name,n);
        l->offsets[l->count]=bytes;
        unsigned char type=PF_DIRENT_TYPE(e);
        if (type==DT_LNK || type==DT_UNKNOWN) {
            /* refueled: pf_fs_stat of "<path>/<name>" (follows links) replaces fstatat(dirfd,name,0) */
            char full[1400]; PfStat entry;
            if ((size_t)snprintf(full,sizeof(full),"%s/%s",path,e->d_name)<sizeof(full) && !pf_fs_stat(full,&entry))
                type=S_ISDIR(entry.mode) ? DT_DIR : S_ISREG(entry.mode) ? DT_REG : type;
        }
        l->types[l->count]=type==DT_DIR ? 4 : type==DT_REG ? 8 : type==DT_LNK ? 10 : 0;
        ++l->count; bytes+=n;
    }
    closedir(d);
    return l;
}
static void convert_stat(const PfStat *s,GuestStat *g) {
    memset(g,0,sizeof(*g));
    g->dev=(uint32_t)s->dev; g->ino=(uint32_t)s->ino;
    g->mode=(uint16_t)s->mode; g->nlink=(uint16_t)s->nlink;
    g->size=(int64_t)s->size; g->blocks=(int64_t)s->blocks; g->blksize=s->blksize;
    g->atime=(GuestTimespec){s->atime_sec,s->atime_nsec};
    g->mtime=(GuestTimespec){s->mtime_sec,s->mtime_nsec};
    g->ctime=(GuestTimespec){s->ctime_sec,s->ctime_nsec};
    g->birthtime=g->ctime;
}
static File *get(int fd) {
    if (fd<3 || fd>=MAX_FILES || !files[fd].used) return NULL;
    return &files[fd];
}
/* BB_AUDIO_TRACE=1: sound file opens and failed reads (missing game sounds). */
static int audio_trace(void) { static int v=-1; if (v<0) { const char *e=getenv("BB_AUDIO_TRACE"); v=e && e[0]=='1'; } return v; }
/* BB_SAVE_TRACE=1: every operation on save files (/savedataN). */
static int save_trace(void) { static int v=-1; if (v<0) { const char *e=getenv("BB_SAVE_TRACE"); v=e && e[0]=='1'; } return v; }
/* Game mounts (including linked mod overlays) are read-only. Saves use other mounts. */
static int game_path(const char *p) {
    if (!p || !*p) return 0;
    if (*p!='/') return 1;
    return (!strncmp(p,"/app0",5) && (!p[5] || p[5]=='/')) ||
           (!strncmp(p,"/hostapp",8) && (!p[8] || p[8]=='/'));
}
/* Saves (/savedataN): see File. The copy starts as the old file unless the open truncates. */
static unsigned temp_serial;
static int copy_contents(int from,int to) {
    char buffer[65536];
    for (;;) {
        ssize_t n=read(from,buffer,sizeof(buffer));
        if (n<0 && errno==EINTR) continue;
        if (n<=0) return n<0 ? -1 : 0;
        for (ssize_t done=0; done<n;) {
            ssize_t w=write(to,buffer+done,(size_t)(n-done));
            if (w<0 && errno==EINTR) continue;
            if (w<0) return -1;
            done+=w;
        }
    }
}
/* A save file open for writing is its copy to the operations by path too (the game stats the
 * file it is writing, which may not exist yet: it renames the old one to its backup first). */
static void current_copy(char *path,size_t size,int write) {
    pthread_mutex_lock(&lock);
    for (int i=3;i<MAX_FILES;++i)
        if (files[i].used && files[i].commit && !strcmp(files[i].commit,path)) { snprintf(path,size,"%s",files[i].temp); files[i].dirty|=write; break; }
    pthread_mutex_unlock(&lock);
}
/* Returns the descriptor of the copy, or -(errno). */
static int open_for_commit(const char *path,const char *source,int flags,int mode,char **commit,char **temp) {
    int hf=host_flags(flags);
    PfStat s;
    int exists=!pf_fs_stat(source,&s);
    if (exists && !S_ISREG(s.mode)) { int h=open(path,hf,mode ? mode : 0644); return h<0 ? -errno : h; }
    if (exists && (hf&O_CREAT) && (hf&O_EXCL)) return -EEXIST;
    if (!exists && !(hf&O_CREAT)) return -ENOENT;
    size_t n=strlen(path)+32;
    char *t=malloc(n), *c=strdup(path);
    if (!t || !c) { free(t); free(c); return -ENOMEM; }
    snprintf(t,n,"%s.%u.bbtmp",path,__atomic_add_fetch(&temp_serial,1,__ATOMIC_RELAXED));
    int out=open(t,O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC,exists ? (int)(s.mode&07777) : mode ? mode : 0644);
    int e=out<0 ? errno : 0;
    if (!e && exists && !(hf&O_TRUNC)) {
        int in=open(source,O_RDONLY|O_CLOEXEC);
        if (in<0 || copy_contents(in,out)) e=errno ? errno : EIO;
        if (in>=0) close(in);
    }
    if (out>=0) close(out);
    int host=e ? -1 : open(t,hf&~(O_CREAT|O_EXCL|O_TRUNC));
    if (host<0) {
        if (!e) e=errno;
        unlink(t); free(t); free(c);
        return -e;
    }
    *commit=c; *temp=t;
    return host;
}
static void sync_directory(const char *file) {
    char dir[1024];
    snprintf(dir,sizeof(dir),"%s",file);
    char *slash=strrchr(dir,'/');
    if (!slash) return;
    *slash=0;
    int d=open(dir,O_RDONLY|O_DIRECTORY|O_CLOEXEC);
    if (d>=0) { fsync(d); close(d); }
}
static int commit_file(const File *f) {
    if (!f->dirty) { close(f->host); unlink(f->temp); return 0; } /* opened for writing, not written */
    int e=fsync(f->host) ? errno : 0;
    close(f->host);
    if (!e && rename(f->temp,f->commit)) e=errno;
    if (e) {
        fprintf(stderr,"Runtime: save file %s not replaced (%s); the old one is kept\n",f->path,strerror(e));
        unlink(f->temp);
        return -e;
    }
    sync_directory(f->commit);
    return 0;
}
/* All operations return >=0 or -(host errno); wrappers adapt the convention. */
static int64_t do_open(const char *guest,int flags,int mode) {
    if (game_path(guest) && (flags & (3|0x8|0x200|0x400|0x800))) return -EROFS;
    char path[1024];
    int e=translate(guest,path,sizeof(path));
    if (e) return -e;
    char *commit=NULL, *temp=NULL, current[1024];
    int standin=0; /* refueled: a directory only the mod overlay has */
    snprintf(current,sizeof(current),"%s",path);
    if (save_path(guest)) current_copy(current,sizeof(current),0);
    int host;
    if (save_path(guest) && (flags&3)) {
        host=open_for_commit(path,current,flags,mode,&commit,&temp);
        if (host<0) { errno=-host; host=-1; }
    } else {
        host=open(current,host_flags(flags),mode ? mode : 0644);
        if (host<0 && errno==ENOENT && !(flags&0x200) && !save_path(guest) && fix_case(current)) {
            host=open(current,host_flags(flags),mode ? mode : 0644);
            snprintf(path,sizeof(path),"%s",current); // the directory listing below uses it
        }
        if (host<0 && errno==ENOENT && overlay_standin(guest,current,sizeof(current))) {
            host=open(current,O_RDONLY|O_DIRECTORY|O_CLOEXEC);
            standin=host>=0;
        }
    }
    if (save_trace() && save_path(guest))
        printf("Save trace: open(%s, flags 0x%x) -> host %d%s%s\n",guest,flags,host,temp ? ", copy " : "",temp ? temp : "");
    if (host<0) {
        e=errno;
        if (e==ENOENT) { ++missing; printf("Runtime: open(%s) -> not found\n",guest); }
        return -e;
    }
    PfStat s={0}; /* refueled: zeroed (was uninitialised when fstat failed; only the audio trace reads it) */
    Listing *dir=NULL;
    if (!pf_fs_fstat(host,&s) && S_ISDIR(s.mode)) {
        dir=standin ? NULL : list_directory(path);
        if (overlay_count) dir=overlay_merge_dir(dir,guest);
    }
    pthread_mutex_lock(&lock);
    int fd=-1;
    for (int i=3;i<MAX_FILES;++i) if (!files[i].used) { fd=i; break; }
    if (fd<0) {
        pthread_mutex_unlock(&lock); close(host); free_listing(dir);
        if (temp) unlink(temp);
        free(commit); free(temp); return -EMFILE;
    }
    files[fd]=(File){.used=1,.host=host,.dir=dir,.commit=commit,.temp=temp};
    snprintf(files[fd].path,sizeof(files[fd].path),"%s",guest);
    ++opens;
    pthread_mutex_unlock(&lock);
    if (audio_trace() && strstr(guest,"sound/")) printf("Audio trace: open(%s) -> fd %d, %lld bytes\n",guest,fd,(long long)s.size);
    const char *mod_trace=getenv("BB_MOD_TRACE"), *mod_root=getenv("BB_MODS_DIR");
    if (mod_trace && mod_trace[0]=='1' && mod_root) {
        char actual[PATH_MAX],root[PATH_MAX];
        static unsigned traced;
        if (realpath(path,actual) && realpath(mod_root,root)) {
            size_t n=strlen(root);
            if (!strncmp(actual,root,n) && actual[n]=='/' &&
                __atomic_fetch_add(&traced,1,__ATOMIC_RELAXED)<32)
                printf("Mods: open %s -> %s\n",guest,actual);
        }
    }
    if (save_trace() && save_path(guest)) printf("Save trace: open(%s) -> fd %d\n",guest,fd);
    return fd;
}
static int64_t do_close(int fd) {
    if (fd>=0 && fd<3) return 0;
    pthread_mutex_lock(&lock);
    File *f=get(fd);
    if (!f) { pthread_mutex_unlock(&lock); return -EBADF; }
    File closed=*f;
    *f=(File){0};
    pthread_mutex_unlock(&lock);
    free_listing(closed.dir);
    int result=0;
    if (closed.temp) result=commit_file(&closed);
    else close(closed.host);
    if (save_trace() && save_path(closed.path))
        printf("Save trace: close(fd %d, %s)%s -> %d\n",fd,closed.path,closed.temp ? closed.dirty ? " commit" : " unwritten" : "",result);
    free(closed.commit); free(closed.temp);
    return result;
}
static int host_fd(int fd) {
    if (fd>=0 && fd<3) return fd;
    File *f=get(fd);
    return f ? f->host : -1;
}
static int host_fd_written(int fd) {
    if (fd>=0 && fd<3) return fd;
    File *f=get(fd);
    if (!f) return -1;
    if (!f->dirty && save_trace() && save_path(f->path)) printf("Save trace: first write to fd %d (%s)\n",fd,f->path);
    f->dirty=1;
    return f->host;
}
/* The GPU side is told of the write first (runtime_memory_note_write: its tracking unprotects the
 * range). Pages protected again meanwhile would make the kernel's copy fail with EFAULT instead
 * of faulting to our handler: a user-mode write to each page first goes through the handler. */
static void touch_for_write(void *buffer,uint64_t size) {
    if (!size) return;
    uintptr_t p=(uintptr_t)buffer & ~(uintptr_t)4095, end=(uintptr_t)buffer+size;
    for (; p<end; p+=4096) {
        volatile unsigned char *b=(volatile unsigned char *)(p<(uintptr_t)buffer ? (uintptr_t)buffer : p);
        *b=*b;
    }
}
/* A page protected again between the touch and the kernel's copy (the GPU side re-arms its write
 * traps): EFAULT with nothing read. Touched again, the read is tried again a few times. */
static ssize_t read_retrying(int h,void *buffer,uint64_t size,int64_t offset,int positional) {
    ssize_t n=-1;
    for (int attempt=0; attempt<8; ++attempt) {
        n=positional ? pread(h,buffer,size,offset) : read(h,buffer,size);
        if (n>=0 || errno!=EFAULT) break;
        touch_for_write(buffer,size);
    }
    return n;
}
static int64_t do_read(int fd,void *buffer,uint64_t size) {
    int h=host_fd(fd);
    if (h<0) return -EBADF;
    runtime_memory_note_write((uintptr_t)buffer,size);
    touch_for_write(buffer,size);
    ssize_t n=read_retrying(h,buffer,size,0,0);
    if (n<0) { if (audio_trace()) printf("Audio trace: read(fd %d, %llu) failed, errno %d\n",fd,(unsigned long long)size,errno); return -errno; }
    if (n>0) runtime_memory_note_write((uintptr_t)buffer,(uint64_t)n); /* and once the data is there */
    __atomic_add_fetch(&reads,1,__ATOMIC_RELAXED); __atomic_add_fetch(&bytes_read,(uint64_t)n,__ATOMIC_RELAXED);
    return n;
}
static int64_t do_pread(int fd,void *buffer,uint64_t size,int64_t offset) {
    int h=host_fd(fd);
    if (h<0) return -EBADF;
    runtime_memory_note_write((uintptr_t)buffer,size);
    touch_for_write(buffer,size);
    ssize_t n=read_retrying(h,buffer,size,offset,1);
    if (n<0) { if (audio_trace()) printf("Audio trace: pread(fd %d, %llu @%lld) failed, errno %d\n",fd,(unsigned long long)size,(long long)offset,errno); return -errno; }
    if (n>0) runtime_memory_note_write((uintptr_t)buffer,(uint64_t)n); /* and once the data is there */
    __atomic_add_fetch(&reads,1,__ATOMIC_RELAXED); __atomic_add_fetch(&bytes_read,(uint64_t)n,__ATOMIC_RELAXED);
    return n;
}
/* Pages the GPU side keeps without access (data newer in VRAM, runtime_memory_trap reasons from
 * 16 up) would make the kernel's copy fail with EFAULT: a user-mode read of each page first goes
 * through the handler, which copies the data back. */
static void touch_for_read(const void *buffer,uint64_t size) {
    if (!size) return;
    uintptr_t p=(uintptr_t)buffer & ~(uintptr_t)4095, end=(uintptr_t)buffer+size;
    for (; p<end; p+=4096)
        (void)*(volatile const unsigned char *)(p<(uintptr_t)buffer ? (uintptr_t)buffer : p);
}
static int64_t do_write(int fd,const void *buffer,uint64_t size) {
    /* SacredFire: the first write into each opened save file is logged (plan milestone d). */
    { File *sf=get(fd); if (sf && !sf->save_logged && !strncmp(sf->path,"/savedata",9)) { sf->save_logged=1; printf("Runtime: save write: %s (%llu bytes)\n",sf->path,(unsigned long long)size); } }
    int h=host_fd_written(fd);
    if (h<0) return -EBADF;
    touch_for_read(buffer,size);
    ssize_t n=write(h,buffer,size);
    if (n<0) return -errno;
    __atomic_add_fetch(&writes,1,__ATOMIC_RELAXED);
    return n;
}
static int64_t do_pwrite(int fd,const void *buffer,uint64_t size,int64_t offset) {
    int h=host_fd_written(fd);
    if (h<0) return -EBADF;
    touch_for_read(buffer,size);
    ssize_t n=pwrite(h,buffer,size,offset);
    return n<0 ? -errno : n;
}
static int64_t do_lseek(int fd,int64_t offset,int whence) {
    if (save_trace()) { File *t=get(fd); if (t && save_path(t->path)) printf("Save trace: lseek(fd %d, %lld, %d)\n",fd,(long long)offset,whence); }
    File *f=get(fd);
    if (!f) return -EBADF;
    if (whence<0 || whence>2) return -EINVAL;
    if (f->dir) {
        /* Directory offsets are entry indices for getdirentries. */
        if (whence==0 && offset>=0) { f->position=(size_t)offset; return offset; }
        return -EINVAL;
    }
    off_t r=lseek(f->host,offset,whence);
    return r<0 ? -errno : r;
}
static int64_t do_fstat(int fd,GuestStat *out) {
    if (save_trace()) { File *t=get(fd); if (t && save_path(t->path)) printf("Save trace: fstat(fd %d)\n",fd); }
    int h=host_fd(fd);
    if (h<0) return -EBADF;
    if (!out) return -EFAULT;
    PfStat s;
    int r=pf_fs_fstat(h,&s);
    if (r) return r;
    convert_stat(&s,out); return 0;
}
static int64_t do_stat(const char *guest,GuestStat *out) {
    char path[1024]; PfStat s;
    int e=translate(guest,path,sizeof(path));
    if (e) return -e;
    if (save_path(guest)) current_copy(path,sizeof(path),0);
    if (!out) return -EFAULT;
    int r=pf_fs_stat(path,&s); /* 0 or PF_E* (PF_ENOENT == -ENOENT) */
    if (r==-ENOENT && overlay_standin(guest,path,sizeof(path))) r=pf_fs_stat(path,&s);
    if (r==-ENOENT && !save_path(guest) && fix_case(path)) r=pf_fs_stat(path,&s);
    if (save_trace() && save_path(guest)) printf("Save trace: stat(%s) -> %d, %lld bytes\n",guest,r,r ? -1LL : (long long)s.size);
    if (r) return r;
    convert_stat(&s,out); return 0;
}
static int64_t do_getdents(int fd,char *buffer,uint64_t size,int64_t *basep) {
    pthread_mutex_lock(&lock);
    File *f=get(fd);
    int64_t result=0;
    if (!f) result=-EBADF;
    else if (!f->dir) result=-EINVAL;
    else if (!buffer) result=-EFAULT;
    else if (size<512) result=-EINVAL;
    else {
        if (basep) *basep=(int64_t)f->position;
        uint64_t written=0;
        while (f->position<f->dir->count) {
            const char *name=f->dir->names+f->dir->offsets[f->position];
            size_t n=strlen(name); if (n>255) n=255;
            uint16_t reclen=(uint16_t)((8+n+1+7)&~(size_t)7);
            if (written+reclen>size) break;
            char *p=buffer+written;
            memset(p,0,reclen);
            uint32_t ino=(uint32_t)f->position+1;
            memcpy(p,&ino,4); memcpy(p+4,&reclen,2);
            p[6]=(char)f->dir->types[f->position]; p[7]=(char)n;
            memcpy(p+8,name,n);
            written+=reclen; ++f->position;
        }
        result=(int64_t)written;
    }
    pthread_mutex_unlock(&lock);
    return result;
}
static int64_t path_op(const char *guest,int op,int mode) {
    if (game_path(guest)) return -EROFS;
    char path[1024];
    int e=translate(guest,path,sizeof(path));
    if (e) return -e;
    int r= op==0 ? mkdir(path,mode ? mode : 0755) : op==1 ? rmdir(path) : unlink(path);
    r=r ? -errno : 0;
    if (save_trace() && save_path(guest)) printf("Save trace: %s(%s) -> %d\n",op==0 ? "mkdir" : op==1 ? "rmdir" : "unlink",guest,r);
    return r;
}
static int64_t do_rename(const char *from,const char *to) {
    if (game_path(from) || game_path(to)) return -EROFS;
    char a[1024],b[1024];
    int e=translate(from,a,sizeof(a));
    if (!e) e=translate(to,b,sizeof(b));
    if (e) return -e;
    int r=rename(a,b) ? -errno : 0;
    if (save_trace() && (save_path(from) || save_path(to))) printf("Save trace: rename(%s, %s) -> %d\n",from,to,r);
    return r;
}
static int64_t do_ftruncate(int fd,int64_t length) {
    int h=host_fd_written(fd);
    if (h<0) return -EBADF;
    return ftruncate(h,length) ? -errno : 0;
}
static int64_t do_truncate(const char *guest,int64_t length) {
    if (game_path(guest)) return -EROFS;
    char path[1024];
    int e=translate(guest,path,sizeof(path));
    if (e) return -e;
    if (save_path(guest)) current_copy(path,sizeof(path),1);
    if (save_trace() && save_path(guest)) printf("Save trace: truncate(%s, %lld)\n",guest,(long long)length);
    return truncate(path,length) ? -errno : 0;
}
static int64_t do_fsync(int fd) { int h=host_fd(fd); if (h<0) return -EBADF; return fsync(h) ? -errno : 0; }
static int64_t do_access(const char *guest,int mode) {
    char path[1024];
    int e=translate(guest,path,sizeof(path));
    if (e) return -e;
    if (save_path(guest)) current_copy(path,sizeof(path),0);
    int r=access(path,mode&7) ? -errno : 0;
    if (r==-ENOENT && overlay_standin(guest,path,sizeof(path))) r=access(path,mode&7) ? -errno : 0;
    if (r==-ENOENT && !save_path(guest) && fix_case(path)) r=access(path,mode&7) ? -errno : 0;
    return r;
}

/* Convention adapters: sceKernel* -> Orbis error codes, POSIX -> -1 + errno. */
static int64_t sce(int64_t r) { return r<0 ? ERR(runtime_guest_errno((int)-r)) : r; }
static int64_t posix(int64_t r) { if (r<0) { *runtime_errno()=runtime_guest_errno((int)-r); return -1; } return r; }
#define PAIR(name,params,args) \
    static ABI int64_t sce_##name params { return sce(do_##name args); } \
    static ABI int64_t posix_##name params { return posix(do_##name args); }
PAIR(open,(const char *p,int f,int m),(p,f,m))
PAIR(close,(int fd),(fd))
PAIR(read,(int fd,void *b,uint64_t n),(fd,b,n))
PAIR(pread,(int fd,void *b,uint64_t n,int64_t o),(fd,b,n,o))
PAIR(write,(int fd,const void *b,uint64_t n),(fd,b,n))
PAIR(pwrite,(int fd,const void *b,uint64_t n,int64_t o),(fd,b,n,o))
PAIR(lseek,(int fd,int64_t o,int w),(fd,o,w))
PAIR(fstat,(int fd,GuestStat *s),(fd,s))
PAIR(stat,(const char *p,GuestStat *s),(p,s))
PAIR(getdents,(int fd,char *b,uint64_t n,int64_t *base),(fd,b,n,base))
PAIR(rename,(const char *a,const char *b),(a,b))
PAIR(ftruncate,(int fd,int64_t l),(fd,l))
PAIR(truncate,(const char *p,int64_t l),(p,l))
PAIR(fsync,(int fd),(fd))
static ABI int64_t posix_access(const char *p,int m) { return posix(do_access(p,m)); }
static ABI int64_t sce_mkdir(const char *p,int m) { return sce(path_op(p,0,m)); }
static ABI int64_t posix_mkdir(const char *p,int m) { return posix(path_op(p,0,m)); }
static ABI int64_t sce_rmdir(const char *p) { return sce(path_op(p,1,0)); }
static ABI int64_t posix_rmdir(const char *p) { return posix(path_op(p,1,0)); }
static ABI int64_t sce_unlink(const char *p) { return sce(path_op(p,2,0)); }
static ABI int64_t posix_unlink(const char *p) { return posix(path_op(p,2,0)); }
/* SacredFire: sceKernelGetdents is getdirentries without the base pointer. */
static ABI int64_t sce_getdents_nobase(int fd,char *b,uint64_t n) { int64_t base=0; return sce(do_getdents(fd,b,n,&base)); }
static ABI int32_t sce_check_reachability(const char *p) {
    GuestStat s; return (int32_t)sce(do_stat(p,&s));
}

static const RuntimeExport exports[]={
    {"sceKernelOpen",sce_open}, {"open",posix_open}, {"_open",posix_open},
    {"sceKernelClose",sce_close}, {"close",posix_close}, {"_close",posix_close},
    {"sceKernelRead",sce_read}, {"read",posix_read}, {"_read",posix_read},
    {"sceKernelPread",sce_pread}, {"pread",posix_pread},
    {"sceKernelWrite",sce_write}, {"write",posix_write}, {"_write",posix_write},
    {"sceKernelPwrite",sce_pwrite}, {"pwrite",posix_pwrite},
    {"sceKernelLseek",sce_lseek}, {"lseek",posix_lseek},
    {"sceKernelFstat",sce_fstat}, {"fstat",posix_fstat},
    {"sceKernelStat",sce_stat}, {"stat",posix_stat},
    {"sceKernelGetdirentries",sce_getdents}, {"getdirentries",posix_getdents},
    {"sceKernelGetdents",sce_getdents_nobase},
    {"sceKernelRename",sce_rename}, {"rename",posix_rename},
    {"sceKernelFtruncate",sce_ftruncate}, {"ftruncate",posix_ftruncate},
    {"sceKernelTruncate",sce_truncate}, {"truncate",posix_truncate},
    {"sceKernelFsync",sce_fsync}, {"fsync",posix_fsync},
    {"access",posix_access}, {"sceKernelCheckReachability",sce_check_reachability},
    {"sceKernelMkdir",sce_mkdir}, {"mkdir",posix_mkdir},
    {"sceKernelRmdir",sce_rmdir}, {"rmdir",posix_rmdir},
    {"sceKernelUnlink",sce_unlink}, {"unlink",posix_unlink},
};
uintptr_t runtime_file_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }
/* Host-side helpers for other modules (e.g. SaveData, Fios). */
int64_t runtime_file_open(const char *p,int f,int m) { return do_open(p,f,m); }
int64_t runtime_file_close(int fd) { return do_close(fd); }
int64_t runtime_file_read(int fd,void *b,uint64_t n) { return do_read(fd,b,n); }
int64_t runtime_file_pread(int fd,void *b,uint64_t n,int64_t o) { return do_pread(fd,b,n,o); }
int64_t runtime_file_write(int fd,const void *b,uint64_t n) { return do_write(fd,b,n); }
int64_t runtime_file_lseek(int fd,int64_t o,int w) { return do_lseek(fd,o,w); }
int64_t runtime_file_stat(const char *p,void *s) { return do_stat(p,s); }
int64_t runtime_file_fstat(int fd,void *s) { return do_fstat(fd,s); }
int64_t runtime_file_getdents(int fd,char *b,uint64_t n,int64_t *base) { return do_getdents(fd,b,n,base); }
int runtime_file_translate(const char *guest,char *out,size_t size) { return translate(guest,out,size); }
void runtime_file_report(void) {
    printf("Runtime: files opened=%zu, reads=%zu (%llu bytes), writes=%zu, not found=%zu\n",
           opens,reads,(unsigned long long)bytes_read,writes,missing);
}

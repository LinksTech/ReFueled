/* Exercise actual guest file operations on a linked overlay and writable saves. */
#include "../src/runtime_file.c"
#include <assert.h>
#ifdef _WIN32
#include <windows.h>
#endif
static int32_t guest_errno;
int32_t *runtime_errno(void) { return &guest_errno; }
int32_t runtime_guest_errno(int e) { return e; }
/* Reads into guest memory tell the GPU side (runtime_memory.c); nothing to tell here. */
void runtime_memory_note_write(uintptr_t address, uint64_t size) { (void)address; (void)size; }
uintptr_t runtime_lookup(const RuntimeExport *table,size_t n,const char *name) {
    for (size_t i=0;i<n;++i) if (!strcmp(table[i].name,name)) return (uintptr_t)table[i].function;
    return 0;
}
/* refueled: virtual mod overlay (BB_MOD_MANIFEST); synthetic game and mod folders, no game data. */
static void put_file(const char *path,const char *text) {
    FILE *f=fopen(path,"wb"); assert(f); assert(fputs(text,f)>=0); assert(!fclose(f));
}
static void make_dir(const char *path) { assert(!mkdir(path,0755)); }
/* Names of a guest directory as "name:type" strings, in listing order. */
static int list_names(const char *guest,char names[][64],int max) {
    int dir=(int)do_open(guest,0x20000,0); assert(dir>=3);
    char entries[4096]; int64_t count=do_getdents(dir,entries,sizeof(entries),NULL); assert(count>=0);
    int n=0;
    for (int64_t p=0;p<count;) {
        uint16_t length; memcpy(&length,entries+p+4,2); assert(length);
        if (!strcmp(entries+p+8,".") || !strcmp(entries+p+8,"..")) { p+=length; continue; } /* readdir lists them */
        assert(n<max);
        snprintf(names[n++],64,"%s:%d",entries+p+8,entries[p+6]);
        p+=length;
    }
    assert(!do_close(dir));
    return n;
}
static int has_name(char names[][64],int n,const char *want) {
    int hits=0;
    for (int i=0;i<n;++i) if (!strcmp(names[i],want)) ++hits;
    return hits;
}
static void read_text(const char *guest,const char *expect) {
    int fd=(int)do_open(guest,0,0); assert(fd>=3);
    char content[64]={0}; assert(do_read(fd,content,sizeof(content)-1)==(int64_t)strlen(expect));
    assert(!strcmp(content,expect)); assert(!do_close(fd));
}
static void overlay_tests(const char *root,const char *user) {
    char p[1024],game[512],mod[512],manifest[512],env[600];
    snprintf(game,sizeof(game),"%s/ov-game",root);
    snprintf(mod,sizeof(mod),"%s/ov-mod",root);
    snprintf(manifest,sizeof(manifest),"%s/ov.manifest",root);
    make_dir(game); make_dir(mod);
    const char *game_dirs[]={"dvdroot_ps4","dvdroot_ps4/chr","dvdroot_ps4/msg"};
    for (int i=0;i<3;++i) { snprintf(p,sizeof(p),"%s/%s",game,game_dirs[i]); make_dir(p); }
    const char *game_files[][2]={{"dvdroot_ps4/chr/a.dcx","game-a"},{"dvdroot_ps4/chr/b.dcx","game-b"},{"dvdroot_ps4/msg/x.txt","game-x"}};
    for (int i=0;i<3;++i) { snprintf(p,sizeof(p),"%s/%s",game,game_files[i][0]); put_file(p,game_files[i][1]); }
    const char *mod_dirs[]={"chr","newdir","newdir/sub"};
    for (int i=0;i<3;++i) { snprintf(p,sizeof(p),"%s/%s",mod,mod_dirs[i]); make_dir(p); }
    const char *mod_files[][3]={{"chr/a.dcx","mod-a","dvdroot_ps4/chr/a.dcx"},{"chr/new.dcx","mod-new","dvdroot_ps4/chr/new.dcx"},
                                {"newdir/sub/f.bin","mod-f","dvdroot_ps4/newdir/sub/f.bin"}};
    FILE *m=fopen(manifest,"wb"); assert(m);
    fputs("# refueled mod manifest 1\n# a comment\n",m);
    for (int i=2;i>=0;--i) { /* unsorted on purpose */
        snprintf(p,sizeof(p),"%s/%s",mod,mod_files[i][0]); put_file(p,mod_files[i][1]);
        fprintf(m,"%s\t%s\n",mod_files[i][2],p);
    }
    assert(!fclose(m));

    /* without BB_MOD_MANIFEST: exactly the old behaviour */
    putenv("BB_MOD_MANIFEST=");
    runtime_file_configure(game,user);
    assert(overlay_count==0);
    read_text("/app0/dvdroot_ps4/chr/a.dcx","game-a");
    GuestStat st; assert(do_open("/app0/dvdroot_ps4/chr/new.dcx",0,0)==-ENOENT);
    assert(do_stat("/app0/dvdroot_ps4/newdir",&st)==-ENOENT);
    char names[16][64];
    int n=list_names("/app0/dvdroot_ps4/chr",names,16); assert(n==2 && has_name(names,n,"a.dcx:8")==1);

    /* with the manifest */
    snprintf(env,sizeof(env),"BB_MOD_MANIFEST=%s",manifest);
    static char env_static[600]; snprintf(env_static,sizeof(env_static),"%s",env);
    putenv(env_static);
    runtime_file_configure(game,user);
    assert(overlay_count==3);
    read_text("/app0/dvdroot_ps4/chr/a.dcx","mod-a");          /* replaced */
    read_text("/hostapp/dvdroot_ps4/chr/a.dcx","mod-a");
    read_text("dvdroot_ps4/chr/a.dcx","mod-a");
    read_text("/app0//dvdroot_ps4/./chr/a.dcx","mod-a");        /* normalized */
    read_text("/app0/dvdroot_ps4/chr/b.dcx","game-b");          /* untouched */
    read_text("/app0/dvdroot_ps4/chr/new.dcx","mod-new");       /* added */
    read_text("/app0/dvdroot_ps4/newdir/sub/f.bin","mod-f");    /* added in an overlay-only directory */
    assert(!do_stat("/app0/dvdroot_ps4/chr/a.dcx",&st) && st.size==5);
    assert(!do_stat("/app0/dvdroot_ps4/newdir",&st) && S_ISDIR(st.mode));
    assert(!do_stat("/app0/dvdroot_ps4/newdir/sub/f.bin",&st) && st.size==5);
    assert(do_stat("/app0/dvdroot_ps4/nothing",&st)==-ENOENT);
    assert(!do_access("/app0/dvdroot_ps4/newdir",0) && !do_access("/app0/dvdroot_ps4/chr/new.dcx",0));
    n=list_names("/app0/dvdroot_ps4/chr",names,16);            /* union, no duplicates */
    assert(n==3 && has_name(names,n,"a.dcx:8")==1 && has_name(names,n,"b.dcx:8")==1 && has_name(names,n,"new.dcx:8")==1);
    n=list_names("/app0/dvdroot_ps4",names,16);
    assert(n==3 && has_name(names,n,"chr:4")==1 && has_name(names,n,"msg:4")==1 && has_name(names,n,"newdir:4")==1);
    n=list_names("/app0/dvdroot_ps4/newdir",names,16); assert(n==1 && has_name(names,n,"sub:4")==1);
    n=list_names("/app0/dvdroot_ps4/newdir/sub",names,16); assert(n==1 && has_name(names,n,"f.bin:8")==1);
    n=list_names("/app0/dvdroot_ps4/msg",names,16); assert(n==1 && has_name(names,n,"x.txt:8")==1);
    /* read-only: the same error as /app0 gives without an overlay */
    const char *hit[]={"/app0/dvdroot_ps4/chr/a.dcx","/app0/dvdroot_ps4/chr/new.dcx","/app0/dvdroot_ps4/newdir/sub/f.bin"};
    for (int i=0;i<3;++i) {
        assert(do_open(hit[i],2,0)==-EROFS && do_open(hit[i],0x400,0)==-EROFS && do_open(hit[i],0x200,0)==-EROFS);
        assert(do_truncate(hit[i],0)==-EROFS && path_op(hit[i],2,0)==-EROFS);
        assert(do_rename(hit[i],"/data/moved")==-EROFS && do_rename("/data/x",hit[i])==-EROFS);
    }
    assert(path_op("/app0/dvdroot_ps4/newdir",0,0)==-EROFS && path_op("/app0/dvdroot_ps4/newdir",1,0)==-EROFS);
    /* the mod file itself stayed as it was */
    snprintf(p,sizeof(p),"%s/chr/a.dcx",mod); read_text("/app0/dvdroot_ps4/chr/a.dcx","mod-a");
    /* ".." is refused, with '/' everywhere and with '\' where the host treats it as a separator */
    char host[1024];
    assert(translate("/app0/dvdroot_ps4/../x",host,sizeof(host))==EACCES);
    assert(translate("../x",host,sizeof(host))==EACCES);
    assert(do_open("/app0/dvdroot_ps4/chr/../../x",0,0)==-EACCES);
#ifdef _WIN32
    assert(translate("/app0/dvdroot_ps4/..\\x",host,sizeof(host))==EACCES);
    assert(translate("/app0/dvdroot_ps4/chr\\..\\..\\x",host,sizeof(host))==EACCES);
    assert(translate("..\\x",host,sizeof(host))==EACCES);
    assert(translate("/app0/dvdroot_ps4/chr/a..b",host,sizeof(host))==0); /* not a component */
#endif
    /* a mount other than /app0 is untouched by the overlay */
    assert(translate("/data/anything",host,sizeof(host))==0 && strstr(host,"data"));

    putenv("BB_MOD_MANIFEST=");
    runtime_file_configure(game,user);
    assert(overlay_count==0);
    read_text("/app0/dvdroot_ps4/chr/a.dcx","game-a");
    assert(!unlink(manifest));
    snprintf(p,sizeof(p),"%s",mod); assert(!pf_fs_remove_tree(p));
    snprintf(p,sizeof(p),"%s",game); assert(!pf_fs_remove_tree(p));
}
int main(void) {
#ifdef _WIN32
    /* refueled: the temp root comes from TMP (never a fixed /tmp); forward slashes for mkdtemp. */
    char root[512];
    {   const char *tmp=getenv("TMP"); if (!tmp || !*tmp) tmp=getenv("TEMP"); assert(tmp && *tmp);
        snprintf(root,sizeof(root),"%s/bbport-mod-files-XXXXXX",tmp);
        for (char *c=root;*c;++c) if (*c=='\\') *c='/'; }
#else
    char root[]="/tmp/bbport-mod-files-XXXXXX";
#endif
    assert(mkdtemp(root));
    putenv("BB_MOD_MANIFEST="); /* unset: the old behaviour for the checks below */
    char game[512],user[512],source[512],link[512];
    snprintf(game,sizeof(game),"%s/game",root);
    snprintf(user,sizeof(user),"%s/user",root);
    snprintf(source,sizeof(source),"%s/mod.dcx",root);
    snprintf(link,sizeof(link),"%s/game/asset.dcx",root);
    assert(!mkdir(game,0755));
    FILE *f=fopen(source,"w"); assert(f); assert(fputs("modded",f)>=0); assert(!fclose(f));
#ifdef _WIN32
    /* refueled: symlink creation needs privileges on Windows (MODLOADER.md section 2). A hard link
     * stands in, so the read-only overlay checks still run; symlink traversal is not covered. */
    puts("skipped on Windows: symlink traversal (hard link used instead)");
    assert(CreateHardLinkA(link,source,NULL));
#else
    assert(!symlink(source,link));
#endif
    runtime_file_configure(game,user);
    for (int i=0;i<3;++i) {
        const char *path=i==0 ? "/app0/asset.dcx" : i==1 ? "/hostapp/asset.dcx" : "asset.dcx";
        int fd=(int)do_open(path,0,0); assert(fd>=3);
        char content[8]={0}; assert(do_read(fd,content,6)==6 && !strcmp(content,"modded"));
        GuestStat info; assert(!do_stat(path,&info) && info.size==6);
        assert(!do_close(fd));
        assert(do_open(path,2,0)==-EROFS);
        assert(do_open(path,0x400,0)==-EROFS);
        assert(do_truncate(path,0)==-EROFS);
        assert(path_op(path,2,0)==-EROFS);
        assert(do_rename(path,"/data/moved")==-EROFS);
    }
    int dir=(int)do_open("/app0",0x20000,0); assert(dir>=3);
    char entries[1024]; int64_t count=do_getdents(dir,entries,sizeof(entries),NULL); assert(count>0);
    int found=0;
    for (int64_t p=0;p<count;) {
        uint16_t length; memcpy(&length,entries+p+4,2);
        assert(length);
        if (!strcmp(entries+p+8,"asset.dcx")) { assert(entries[p+6]==8); found=1; }
        p+=length;
    }
    assert(found && !do_close(dir));
    int save=(int)do_open("/data/test-save",0x202,0644); assert(save>=3);
    assert(do_write(save,"save",4)==4 && !do_close(save));
    assert(!path_op("/data/test-save",2,0));
    assert(!unlink(link) && !unlink(source) && !rmdir(game));
    overlay_tests(root,user);
    const char *dirs[]={"temp0","download0","data"};
    for (int i=0;i<3;++i) { char p[1024]; snprintf(p,sizeof(p),"%s/%s",user,dirs[i]); assert(!rmdir(p)); }
    assert(!rmdir(user) && !rmdir(root));
    puts("Guest mod files: reads, stat, merged listing, readonly assets and writable saves, virtual overlay PASS");
}

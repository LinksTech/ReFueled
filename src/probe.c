/* Minimal x86-64 native loader experiment. Not a PS4 emulator or game port. */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <cpuid.h>
#include "runtime.h"
#include "gpu/bbgpu.h"
#if !defined(__x86_64__) || !defined(__GNUC__)
#error This prototype requires x86-64 GCC or Clang (including MinGW).
#endif
#include <unistd.h>
/* refueled: the host facilities the loader needs (memory, faults, watchdog, threads, symbols, restart)
 * come from the platform layer, src/platform/pf_*.h (docs/refueled/ARCHITECTURE.md). */
#include "platform/platform.h"

typedef struct { uint64_t address, size, flags; } Segment;
typedef struct { uint64_t target, kind, value, addend; } Reloc;
static char (*names)[128];
static uint64_t import_count;
static unsigned char *image;
static size_t page_size;
typedef struct { uint64_t base, size, init, tls_address, tls_memsz, tls_filesz, tls_module; } LinkedModule;
static LinkedModule modules[16];
static uint64_t module_count;
static int entered_game;
static int gpu_enabled;
int vulkan_smoke(void);

static void fail(const char *message) { fprintf(stderr, "ERROR: %s\n", message); exit(1); }

/* The game's code was compiled for the PS4's CPU (AMD Jaguar) and runs as it is: AVX, BMI1
 * (andn/bextr/blsr/tzcnt), MOVBE, LZCNT and POPCNT, thousands of each in eboot.bin. A CPU without
 * them stops at the first one with SIGILL (exit code 132, issue #26), and lzcnt/tzcnt even run as
 * bsr/bsf there, with other results. Said before the game starts; BB_SKIP_CPU_CHECK=1 skips it. */
static void check_cpu(void) {
    const char *skip = getenv("BB_SKIP_CPU_CHECK");
    if (skip && !strcmp(skip, "1")) return;
    unsigned a, b, c, d, leaf1_c = 0, leaf7_b = 0, ext1_c = 0;
    if (__get_cpuid(1, &a, &b, &c, &d)) leaf1_c = c;
    if (__get_cpuid_count(7, 0, &a, &b, &c, &d)) leaf7_b = b;
    if (__get_cpuid(0x80000001u, &a, &b, &c, &d)) ext1_c = c;
    int avx = (leaf1_c & bit_AVX) && (leaf1_c & bit_OSXSAVE);
    if (avx) {
        unsigned lo, hi;
        __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
        avx = (lo & 6) == 6; /* the OS saves SSE and AVX state */
    }
    const struct { int present; const char *name; } features[] = {
        {avx, "AVX"}, {(leaf7_b & bit_BMI) != 0, "BMI1"}, {(leaf1_c & bit_MOVBE) != 0, "MOVBE"},
        {(ext1_c & bit_LZCNT) != 0, "LZCNT"}, {(leaf1_c & bit_POPCNT) != 0, "POPCNT"},
        {(leaf1_c & bit_SSE4_2) != 0, "SSE4.2"},
    };
    char missing[64] = "";
    for (unsigned i = 0; i < sizeof(features) / sizeof(features[0]); ++i) {
        if (features[i].present) continue;
        if (missing[0]) strcat(missing, ", ");
        strcat(missing, features[i].name);
    }
    if (!missing[0]) return;
    fprintf(stderr,
            "ERROR: this CPU lacks %s. The game's code was compiled for the PS4's CPU and uses "
            "these instructions directly: it needs an Intel Haswell (4th generation Core, 2013) or "
            "newer, or an AMD Ryzen. BB_SKIP_CPU_CHECK=1 starts anyway.\n", missing);
    exit(1);
}
static uint64_t read64(FILE *f) {
    unsigned char b[8];
    if (fread(b, 1, 8, f) != 8) fail("truncated boot file");
    uint64_t n = 0;
    for (int i = 7; i >= 0; --i) n = (n << 8) | b[i];
    return n;
}
static size_t round_page(size_t size) { return (size + page_size - 1) & ~(page_size - 1); }
static void *allocate(size_t size) {
    void *low=runtime_low_map(size,PF_PROT_READ|PF_PROT_WRITE);
    if (low) return low;
    void *p = pf_vm_alloc(size, PF_PROT_READ | PF_PROT_WRITE);
    if (!p) fail("mmap failed");
    return p;
}
static void protect(void *p, size_t size, unsigned flags) {
    int mode = ((flags & 4) ? PF_PROT_READ : 0) | ((flags & 2) ? PF_PROT_WRITE : 0) | ((flags & 1) ? PF_PROT_EXEC : 0);
    if (pf_vm_protect(p, size, mode)) fail("mprotect failed");
}
static ABI __attribute__((noreturn)) void unresolved(uint32_t id, uintptr_t argument) {
    if (id >= import_count) fail("bad import trap index");
    printf("STOP: first unsupported PS4 import: %s (index %u)\n", names[id], id);
    printf("API: %s\n", runtime_import_name(names[id]));
    printf("Caller return offset: 0x%" PRIxPTR "; first argument: 0x%" PRIxPTR "\n",
           (uintptr_t)__builtin_return_address(0) - (uintptr_t)image, argument);
    uintptr_t caller=(uintptr_t)__builtin_return_address(0)-(uintptr_t)image;
    for (uint64_t m=0;m<module_count;++m)
        if (caller>=modules[m].base && caller-modules[m].base<modules[m].size)
            printf("Caller in linked module %" PRIu64 " (%s): +0x%" PRIxPTR "\n",m,m==0 ? "libc.prx" : "system module",caller-modules[m].base);
    runtime_report();
    puts(entered_game ? "Original guest entry instructions executed; game initialization is incomplete." :
                        "Native libc initialization is incomplete; game entry has not run.");
    fflush(NULL);
    _exit(20); /* no destructors: GPU, audio and guest threads are still running */
}
static ABI void guest_exit(void) { puts("Runtime: process finalizer callback reached"); }
/* Frames of the host call chain before the faulting code: pf_backtrace, fault, the dispatcher and the
 * signal trampoline (Linux) / exception dispatch (Windows). The numbering below keeps the old one
 * (#2 = the first frame after the trampoline, as with backtrace() called in the signal handler). */
#define FAULT_FRAMES_SKIPPED 4
/* Access, bus and illegal-instruction faults, in the order the old signal handler checked them:
 * GPU page tracking, recovery points of speculative reads, then the fatal report. */
static PfFaultResult fault(const PfFault *info, PfContext *context, void *user) {
    (void)user;
    const int sig = info->host_code;
    /* GPU page tracking (write-protected guest pages) is resolved first. */
    if (gpu_enabled && info->kind == PF_FAULT_ACCESS && bbgpu_handle_fault(context, (void *)info->address)) return PF_FAULT_HANDLED;
    /* A speculative guest memory read (runtime_memory.c) failed: resume its recovery point. */
    if ((info->kind == PF_FAULT_ACCESS || info->kind == PF_FAULT_BUS) && runtime_fault_recover) {
        pf_sigjmp_buf *recover = runtime_fault_recover;
        runtime_fault_recover = NULL;
        return pf_fault_resume_at(context, *recover, 1);
    }
    /* The process is terminating: symbol lookups and snprintf are acceptable here. */
    uintptr_t rip = (uintptr_t)pf_context_get(context, PF_REG_RIP);
    char line[512];
    PfSymbol where;
    if (rip - (uintptr_t)image < 0x10000000)
        snprintf(line, sizeof(line), "Guest fault (signal %d) at guest offset 0x%" PRIx64 ", address %p\n",
                 sig, (uint64_t)(rip - (uintptr_t)image), (void *)info->address);
    else if (pf_symbolize(rip, &where) == 0)
        snprintf(line, sizeof(line), "Host fault (signal %d) in %s+0x%" PRIx64 " (%s), address %p\n", sig, where.module,
                 (uint64_t)(rip - where.module_base), where.symbol[0] ? where.symbol : "?", (void *)info->address);
    else
        snprintf(line, sizeof(line), "Fault (signal %d) at RIP %p, address %p\n", sig, (void *)rip, (void *)info->address);
    pf_write_stderr_str(line);
    if (gpu_enabled) bbgpu_dump_guest_writes(context);
    /* Outside the image and any module (generated code, a freed mapping): the mapping of RIP
     * (Linux: its /proc/self/maps line), and the thread. */
    if (rip - (uintptr_t)image >= 0x10000000 && pf_symbolize(rip, &where) != 0) {
        char thread[32] = "?";
        pf_thread_get_name(thread, sizeof(thread));
        snprintf(line, sizeof(line), "  thread %s; mapping of RIP: ", thread);
        pf_write_stderr_str(line);
        if (pf_describe_mapping(rip, line, sizeof(line) - 1) == 0) {
            pf_write_stderr_str(line);
            pf_write_stderr_str("\n");
        } else {
            pf_write_stderr_str("none\n");
        }
    }
    /* Host call chain (frames with unwind info; guest frames end it). */
    void *frames[32 + FAULT_FRAMES_SKIPPED - 2];
    int depth = pf_backtrace(frames, (int)(sizeof(frames) / sizeof(*frames)));
    for (int i = FAULT_FRAMES_SKIPPED; i < depth; ++i) {
        const int number = i - FAULT_FRAMES_SKIPPED + 2;
        if (pf_symbolize((uintptr_t)frames[i], &where) == 0)
            snprintf(line, sizeof(line), "  #%d %s+0x%" PRIx64 " (%s)\n", number, where.module,
                     (uint64_t)((uintptr_t)frames[i] - where.module_base), where.symbol[0] ? where.symbol : "?");
        else
            snprintf(line, sizeof(line), "  #%d %p\n", number, frames[i]);
        pf_write_stderr_str(line);
    }
    /* Linux: 128 + signal number, as before; Windows: the exception code. */
    pf_exit_now(sig > 0 && sig < 64 ? 128 + sig : sig);
}
/* Watchdog: dump RIP and the rbp frame chain of every thread (guest offsets
 * when inside the image). Reads use pf_read_memory_safe so bad frames cannot fault. */
static uintptr_t exe_base;
static void write_hex(char *out, uint64_t v) {
    const char digits[] = "0123456789abcdef";
    for (int i = 0; i < 16; ++i) out[i] = digits[(v >> (60 - i * 4)) & 15];
}
static void dump_frames(const PfContext *context, uint64_t tid) {
    char line[] = "  tid=0000000000000000 rip=0000000000000000 image-relative=0000000000000000 host-relative=0000000000000000\n";
    uintptr_t rip=(uintptr_t)pf_context_get(context,PF_REG_RIP), rbp=(uintptr_t)pf_context_get(context,PF_REG_RBP);
    /* First argument register: the lock address when a thread waits on a futex. */
    char arg[]="  tid=0000000000000000 rdi=0000000000000000\n";
    write_hex(arg+6,tid); write_hex(arg+27,pf_context_get(context,PF_REG_RDI));
    pf_write_stderr(arg,sizeof(arg)-1);
    for (int depth=0; depth<24; ++depth) {
        write_hex(line+6,tid); write_hex(line+27,rip); write_hex(line+59,rip-(uintptr_t)image); write_hex(line+90,rip-exe_base);
        pf_write_stderr(line,sizeof(line)-1);
        uintptr_t frame[2];
        if (!rbp || pf_read_memory_safe(frame,rbp,sizeof(frame))!=(int64_t)sizeof(frame)) break;
        if (frame[0]<=rbp) break;
        rbp=frame[0]; rip=frame[1];
    }
}
/* The thread dump (pf_dump_signal_install): Linux the SIGUSR2 handler on that thread, as before;
 * Windows: the watchdog thread with the suspended thread's registers. */
static void thread_dump(const PfContext *context, uint64_t tid, void *user) { (void)user; dump_frames(context, tid); }
static void watchdog(PfContext *context, void *user) {
    (void)user;
    const char head[]="STOP: watchdog timeout; thread stacks:\n";
    pf_write_stderr(head,sizeof(head)-1);
    /* Linux: the context of the thread the alarm interrupted; Windows: none (timer thread). */
    if (context) dump_frames(context,pf_thread_id());
    pf_dump_other_threads(); /* Linux: SIGUSR2 to every other thread, 20 ms apart, then 100 ms */
    pf_exit_now(128 + 14); /* 128 + SIGALRM, the exit code of the Linux watchdog */
}
/* param.sfo lookup: string or integer value of key, 0 when absent. */
static int sfo_value(const char *path, const char *key, char *text, size_t text_size, uint32_t *number) {
    FILE *f=fopen(path,"rb");
    if (!f) return 0;
    unsigned char data[65536];
    size_t n=fread(data,1,sizeof(data),f); fclose(f);
    if (n<20 || memcmp(data,"\0PSF",4)) return 0;
    uint32_t keys, values, count;
    memcpy(&keys,data+8,4); memcpy(&values,data+12,4); memcpy(&count,data+16,4);
    for (uint32_t i=0;i<count && 20+i*16+16<=n;++i) {
        const unsigned char *e=data+20+i*16;
        uint16_t key_offset, format; uint32_t length, offset;
        memcpy(&key_offset,e,2); memcpy(&format,e+2,2); memcpy(&length,e+4,4); memcpy(&offset,e+12,4);
        if (keys+key_offset>=n || values+offset+length>n || strcmp((const char *)data+keys+key_offset,key)) continue;
        if (format==0x0404 && number && length>=4) { memcpy(number,data+values+offset,4); return 1; }
        if (text && text_size) {
            size_t copy=length<text_size-1 ? length : text_size-1;
            memcpy(text,data+values+offset,copy); text[copy]=0;
        }
        return 1;
    }
    return 0;
}
static int mapped(Segment *segments, uint64_t count, uint64_t address, uint64_t bytes) {
    for (uint64_t i = 0; i < count; ++i)
        if (address >= segments[i].address && bytes <= segments[i].size &&
            address - segments[i].address <= segments[i].size - bytes) return 1;
    return 0;
}
/* BBPATCH2 (patches.py): the patches' image base, then byte writes at image offsets, applied
 * after relocation. A write may replace a whole base-relative pointer slot (60/90 FPS++ swap
 * function pointers): the patch holds the address at the patches' base, rebased here. */
static void apply_patches(const char *path, Segment *segments, uint64_t ns, const Reloc *relocs, uint64_t nr) {
    FILE *f=fopen(path,"rb");
    char magic[8];
    if (!f || fread(magic,1,8,f)!=8 || memcmp(magic,"BBPATCH2",8)) fail("invalid patch file");
    uint64_t base=read64(f), count=read64(f), bytes=0, rebased=0;
    unsigned char data[4096];
    for (uint64_t i=0;i<count;++i) {
        uint64_t offset=read64(f), length=read64(f);
        if (!length || length>sizeof(data) || !mapped(segments,ns,offset,length) || fread(data,1,length,f)!=length)
            fail("bad patch entry");
        uint64_t slots[sizeof(data)/8]; size_t nslots=0;
        for (uint64_t r=0;r<nr;++r) {
            if (!(relocs[r].target<offset+length && offset<relocs[r].target+8)) continue;
            uint64_t target=relocs[r].target, value;
            if (relocs[r].kind || target<offset || target+8>offset+length) fail("patch overlaps a relocation");
            memcpy(&value,data+(target-offset),8);
            if (value<base || !mapped(segments,ns,value-base,1)) fail("patch writes a pointer outside the image");
            slots[nslots++]=target;
        }
        memcpy(image+offset,data,length);
        for (size_t s=0;s<nslots;++s) {
            uint64_t value;
            memcpy(&value,image+slots[s],8);
            value=(uint64_t)(uintptr_t)image+(value-base);
            memcpy(image+slots[s],&value,8);
        }
        rebased+=nslots;
        bytes+=length;
    }
    if (fgetc(f)!=EOF) fail("trailing data in patch file");
    fclose(f);
    printf("Patches: %" PRIu64 " writes, %" PRIu64 " bytes applied, %" PRIu64 " pointers rebased\n",count,bytes,rebased);
}
/* Restarts the game through run.sh (the settings menu: a new render resolution is a patch
 * applied at start). Descriptors are closed first so the old GPU device and its memory are
 * released before the new process opens its own. */
volatile int runtime_restarting;
void runtime_restart(void) {
    fflush(NULL);
    puts("Runtime: restarting through run.sh");
    /* The GPU threads still run until exec: their Vulkan calls fail once the device fd is
     * closed below, and an assertion there must not end the process (exit 23) before exec. */
    runtime_restarting = 1;
    __sync_synchronize();
    pf_restart_through_launcher(); /* Linux: close_range(3, ~0) + execlp("bash", "run.sh") */
    perror("runtime_restart: exec");
    _exit(1);
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    /* refueled: process set-up (Linux: the mallopt calls that keep guest-visible heap objects in
     * the brk heap below 1 TiB; Windows: binary mode, fault dispatcher, TLS slot), then the guest
     * address layout and the low heap, before anything maps guest memory: the image below is the
     * first low allocation and lands at 0x800000000. */
    if (pf_init()) fail("platform initialization failed");
    if (runtime_memory_init_layout()) fail("cannot reserve the guest address layout");
    if (argc == 2 && !strcmp(argv[1], "--vulkan-only")) return vulkan_smoke();
    check_cpu();
    int cpu_only = 0, strict_imports = 0, list_imports = 0;
    unsigned timeout_seconds = 10;
    const char *content_profile=NULL, *app0=NULL, *user_dir=NULL, *patch_file=NULL, *import_names=NULL;
    for (int i = 2; i < argc; ++i) {
        if (!strcmp(argv[i], "--cpu-only")) cpu_only = 1;
        else if (!strcmp(argv[i], "--strict-imports")) strict_imports = 1;
        /* SacredFire: --list-imports resolves every import, prints the outcome and exits without running. */
        else if (!strcmp(argv[i], "--list-imports")) { list_imports = 1; cpu_only = 1; }
        else if (!strcmp(argv[i], "--import-names") && i+1<argc) import_names=argv[++i];
        else if (!strcmp(argv[i], "--content-profile") && i+1<argc) content_profile=argv[++i];
        else if (!strcmp(argv[i], "--app0") && i+1<argc) app0=argv[++i];
        else if (!strcmp(argv[i], "--user") && i+1<argc) user_dir=argv[++i];
        else if (!strcmp(argv[i], "--patches") && i+1<argc) patch_file=argv[++i];
        else if (!strcmp(argv[i], "--timeout") && i+1<argc) timeout_seconds=(unsigned)strtoul(argv[++i],NULL,10);
        else { fprintf(stderr, "Unknown option: %s\n", argv[i]); return 1; }
    }
    if (argc < 2) {
        fprintf(stderr, "Usage: %s boot.bin [--cpu-only] [--strict-imports] [--list-imports] [--import-names file] [--content-profile file] [--app0 dir] [--user dir] [--patches file] [--timeout seconds] | --vulkan-only\n", argv[0]);
        return 1;
    }
    if (import_names) {
        int loaded = runtime_load_import_names(import_names);
        if (loaded < 0) fail("cannot read the import names file");
        printf("Import names: %d symbols from %s\n", loaded, import_names);
    }
    if (content_profile) {
        FILE *profile=fopen(content_profile,"rb");
        unsigned char data[28];
        if (!profile) fail("cannot open content profile");
        if (fread(data,1,sizeof(data),profile)!=sizeof(data) || fgetc(profile)!=EOF || memcmp(data,"BBCONT01",8)) fail("invalid content profile");
        fclose(profile);
        uint32_t values[5];
        for (unsigned i=0;i<5;++i) {
            unsigned char *p=data+8+i*4;
            values[i]=(uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;
        }
        runtime_content_configure(values);
    }
    if (app0) {
        runtime_file_configure(app0, user_dir ? user_dir : "user");
        char sfo[4096], id[16]="";
        snprintf(sfo,sizeof(sfo),"%s/sce_sys/param.sfo",app0);
        if (sfo_value(sfo,"INSTALL_DIR_SAVEDATA",id,sizeof(id),NULL) || sfo_value(sfo,"TITLE_ID",id,sizeof(id),NULL))
            runtime_savedata_configure(id);
    }
    bbgpu_register_kernel();
    page_size = (size_t)pf_page_size();
    /* First fault handler (D8): GPU tracking, recovery points, fatal report; the GPU library's
     * diagnostic hooks register theirs in bbgpu_init. Linux: sigaction SA_SIGINFO for SIGSEGV,
     * SIGILL and SIGBUS as before. */
    if (pf_fault_add_handler((1u << PF_FAULT_ACCESS) | (1u << PF_FAULT_BUS) | (1u << PF_FAULT_ILLEGAL), fault, NULL))
        fail("cannot install the fault handler");
    exe_base = pf_executable_base();
    /* Thread dump on request (Linux: SIGUSR2, also `kill -USR2` for profiling), then the watchdog. */
    if (pf_dump_signal_install(thread_dump, NULL)) fail("cannot install the thread dump");
    if (pf_watchdog_start(timeout_seconds, watchdog, NULL)) fail("cannot start the watchdog"); /* 0 disables it */
    FILE *f = fopen(argv[1], "rb");
    if (!f) fail("cannot open boot file; run prepare.py first");
    char magic[8];
    if (fread(magic, 1, 8, f) != 8 || (memcmp(magic, "BBPROBE1", 8) && memcmp(magic, "BBPROBE2", 8) && memcmp(magic,"BBPROBE3",8) && memcmp(magic,"BBPROBE4",8) && memcmp(magic,"BBPROBE5",8))) fail("bad boot file signature");
    uint64_t size = read64(f), entry = read64(f), ns = read64(f), nr = read64(f);
    import_count = read64(f);
    uint64_t capabilities = memcmp(magic, "BBPROBE1", 8) ? read64(f) : 0;
    if (capabilities & ~UINT64_C(1)) fail("unknown runtime capabilities");
    runtime_start(strict_imports ? 0 : capabilities);
    if (!size || size > 512*1024*1024 || entry >= size || !ns || ns > 64 || nr > 1000000 || import_count > 100000)
        fail("boot file limits exceeded");
    int multi=!memcmp(magic,"BBPROBE5",8);
    int linked=!memcmp(magic,"BBPROBE3",8) || !memcmp(magic,"BBPROBE4",8) || multi;
    int native_libc=linked && !strict_imports && (capabilities&1);
    uint64_t main_tls[4]={0};
    uint64_t nb=0,procparam=0;
    uint64_t *bindings=calloc(import_count ? import_count : 1,sizeof(*bindings));
    uint64_t *binding_kinds=calloc(import_count ? import_count : 1,sizeof(*binding_kinds));
    if (!bindings || !binding_kinds) fail("allocation failed");
    if (multi) {
        /* BBPROBE5: procparam, eboot TLS, module table, bindings (link_modules.py). */
        procparam=read64(f);
        for (int i=0;i<4;++i) main_tls[i]=read64(f);
        module_count=read64(f);
        if (!module_count || module_count>sizeof(modules)/sizeof(*modules)) fail("invalid module count");
        for (uint64_t m=0;m<module_count;++m) {
            LinkedModule *x=&modules[m];
            x->base=read64(f); x->size=read64(f); x->init=read64(f); x->tls_address=read64(f);
            x->tls_memsz=read64(f); x->tls_filesz=read64(f); x->tls_module=read64(f);
        }
        nb=read64(f);
    } else if (linked) {
        LinkedModule *x=&modules[0];
        module_count=1;
        x->base=read64(f); x->size=read64(f); x->init=read64(f);
        x->tls_address=read64(f); x->tls_memsz=read64(f); x->tls_filesz=read64(f); x->tls_module=2; nb=read64(f);
        procparam=read64(f);
        if (!memcmp(magic,"BBPROBE4",8)) for (int i=0;i<4;++i) main_tls[i]=read64(f);
    }
    if (linked) {
        if (main_tls[1]>main_tls[2] || main_tls[2]>1024*1024 || main_tls[0]>size ||
            main_tls[1]>size-main_tls[0] || (main_tls[3] & (main_tls[3]-1)) || main_tls[3]>4096)
            fail("invalid eboot TLS metadata");
        for (uint64_t m=0;m<module_count;++m) {
            LinkedModule *x=&modules[m];
            if (x->base>=size || !x->size || x->size>size-x->base || x->init<x->base || x->init-x->base>=x->size ||
                (x->tls_module && (x->tls_address>=size || x->tls_memsz>size-x->tls_address || x->tls_memsz>1024*1024 ||
                                   x->tls_filesz>x->tls_memsz || x->tls_module<2 || x->tls_module>7)))
                fail("invalid linked module metadata");
        }
        if (nb>import_count) fail("invalid binding count");
        for (uint64_t i=0;i<nb;++i) {
            uint64_t index=read64(f),address=read64(f),kind=read64(f);
            int inside=0;
            for (uint64_t m=0;m<module_count;++m)
                if (address>=modules[m].base && address-modules[m].base<modules[m].size) inside=1;
            if (index>=import_count || !inside || (kind!=1 && kind!=2) || bindings[index]) fail("invalid native binding");
            bindings[index]=address; binding_kinds[index]=kind;
        }
    }
    Segment *segments = calloc(ns, sizeof(*segments));
    Reloc *relocs = calloc(nr ? nr : 1, sizeof(*relocs));
    names = calloc(import_count ? import_count : 1, sizeof(*names));
    if (!segments || !relocs || !names) fail("allocation failed");
    for (uint64_t i = 0; i < ns; ++i) {
        segments[i].address = read64(f);
        segments[i].size = read64(f);
        segments[i].flags = read64(f);
        if (segments[i].address > size || segments[i].size > size - segments[i].address ||
            segments[i].address % page_size || segments[i].flags > 7) fail("bad segment");
    }
    if (fread(names, 128, import_count, f) != import_count) fail("truncated import names");
    for (uint64_t i = 0; i < import_count; ++i)
        if (!memchr(names[i], 0, 128)) fail("unterminated import name");
    for (uint64_t i = 0; i < nr; ++i) {
        relocs[i].target = read64(f);
        relocs[i].kind = read64(f);
        relocs[i].value = read64(f);
        relocs[i].addend = read64(f);
        if (!mapped(segments, ns, relocs[i].target, 8) || relocs[i].kind > 2 ||
            (relocs[i].kind && relocs[i].value >= import_count) ||
            (relocs[i].kind != 2 && relocs[i].addend) || relocs[i].addend >= page_size) fail("bad relocation");
    }
    for (uint64_t m=0;m<module_count;++m)
        if (!mapped(segments,ns,modules[m].init,1) || (modules[m].tls_module && !mapped(segments,ns,modules[m].tls_address,modules[m].tls_memsz)))
            fail("unmapped module metadata");
    if (module_count && !mapped(segments,ns,procparam,64)) fail("unmapped procparam");
    for (uint64_t i=0;i<import_count;++i) {
        if (!bindings[i]) continue;
        if (!mapped(segments,ns,bindings[i],1)) fail("unmapped native export");
        if (binding_kinds[i]==1) {
            int executable=0;
            for (uint64_t s=0;s<ns;++s)
                if ((segments[s].flags&1) && bindings[i]>=segments[s].address &&
                    bindings[i]-segments[s].address<segments[s].size) executable=1;
            if (!executable) fail("native function is not executable");
        }
    }
    image = allocate(round_page(size));
    if (fread(image, 1, size, f) != size || fgetc(f) != EOF) fail("incorrect memory image size");
    fclose(f);
    if (!cpu_only) {
        char title[128]="PS4 game", serial[16]="UNKNOWN", sfo[4096];
        uint32_t attributes=0;
        snprintf(sfo,sizeof(sfo),"%s/sce_sys/param.sfo",app0 ? app0 : ".");
        sfo_value(sfo,"TITLE",title,sizeof(title),NULL);
        sfo_value(sfo,"TITLE_ID",serial,sizeof(serial),NULL);
        sfo_value(sfo,"ATTRIBUTE",NULL,0,&attributes);
        uint64_t sdk=0;
        if (procparam) memcpy(&sdk,image+procparam+16,8); /* procparam: size, magic, count, sdk_version */
        char icon[4200];
        snprintf(icon,sizeof(icon),"%s/sce_sys/icon0.png",app0);
        BbGpuConfig gpu={title,serial,user_dir ? user_dir : "user",(uint32_t)sdk,attributes,1920,1080,icon};
        gpu_enabled=1; /* page tracking starts while the rasterizer registers guest memory */
        if (bbgpu_init(&gpu)) fail("GPU initialization failed");
        printf("GPU: window and Vulkan presenter ready; SDK 0x%08x, %u HLE symbols\n",(unsigned)sdk,bbgpu_symbol_count());
    }
    unsigned char *traps = allocate(round_page((import_count + 1) * 32));
    unsigned char *data_traps = allocate((import_count + 1) * page_size);
    protect(data_traps, (import_count + 1) * page_size, 0);
    for (uint64_t i = 0; i < import_count; ++i) {
        unsigned char *t = traps + i * 32;
        /* SysV: mov edi, index; movabs rax, handler; jmp rax. No fake return values. */
        uint32_t index = (uint32_t)i;
        uintptr_t handler = (uintptr_t)unresolved;
        t[0] = 0x48; t[1] = 0x89; t[2] = 0xfe; /* mov rsi,rdi: preserve arg0 */
        t[3] = 0xbf; memcpy(t + 4, &index, 4);
        t[8] = 0x48; t[9] = 0xb8; memcpy(t + 10, &handler, 8);
        t[18] = 0xff; t[19] = 0xe0;
    }
    /* SacredFire (--list-imports): how each import was bound: 1 runtime, 2 native module, 3 trap. */
    unsigned char *outcome = calloc(import_count ? import_count : 1, 1);
    if (!outcome) fail("allocation failed");
    for (uint64_t i = 0; i < nr; ++i) {
        uintptr_t value = relocs[i].kind == 2 ? (uintptr_t)(data_traps + page_size * relocs[i].value + relocs[i].addend)
                        : relocs[i].kind == 1 ? (uintptr_t)(traps + 32 * relocs[i].value)
                        : (uintptr_t)image + relocs[i].value;
        if (relocs[i].kind) {
            uintptr_t resolved = runtime_resolve(names[relocs[i].value], relocs[i].kind == 2);
            if (resolved) { value = resolved + relocs[i].addend; outcome[relocs[i].value] = 1; }
            else if (native_libc && bindings[relocs[i].value]) {
                uint64_t address=bindings[relocs[i].value];
                if (binding_kinds[relocs[i].value]!=relocs[i].kind || !mapped(segments,ns,address,relocs[i].addend+1)) fail("native export kind/range mismatch");
                value=(uintptr_t)image+address+relocs[i].addend;
                outcome[relocs[i].value] = 2;
            } else outcome[relocs[i].value] = 3;
        }
        memcpy(image + relocs[i].target, &value, 8);
    }
    if (list_imports) {
        static const char *words[] = {"unreferenced", "runtime", "native", "unresolved"};
        unsigned counts[4] = {0};
        for (uint64_t i = 0; i < import_count; ++i) {
            const char *symbol = runtime_symbol(names[i]);
            printf("%-12s %-40s %s\n", words[outcome[i]], names[i], symbol ? symbol : "?");
            ++counts[outcome[i]];
        }
        printf("Imports: %u runtime, %u native, %u unresolved, %u unreferenced\n", counts[1], counts[2], counts[3], counts[0]);
        return 0;
    }
    if (patch_file) apply_patches(patch_file, segments, ns, relocs, nr);
    if (!cpu_only) bbgpu_patch_image(image, size);
    /* refueled: the boot file's GS-form thread-pointer loads (link_*.py) for this platform
     * (Linux: counted only; Windows: rewritten to the TLS slot, pf_tls.h). */
    {
        PfCodeRange code[64];
        size_t code_count = 0;
        for (uint64_t i = 0; i < ns; ++i)
            if (segments[i].flags & 1) code[code_count++] = (PfCodeRange){image + segments[i].address, segments[i].size};
        if (pf_tls_patch_image(code, code_count) < 0) fail("cannot adapt the guest thread-pointer loads");
    }
    protect(traps, round_page((import_count + 1) * 32), 5);
    protect(image, round_page(size), 0);
    int executable_entry = 0;
    for (uint64_t i = 0; i < ns; ++i) {
        protect(image + segments[i].address, round_page(segments[i].size), (unsigned)segments[i].flags);
        if ((segments[i].flags & 1) && entry >= segments[i].address && entry - segments[i].address < segments[i].size)
            executable_entry = 1;
    }
    if (!executable_entry) fail("entry is not executable");
    printf("Mapped %" PRIu64 " bytes, %" PRIu64 " segments; applied %" PRIu64 " relocations\n", size, ns, nr);
    if (native_libc) {
        for (uint64_t m=0;m<module_count;++m) {
            int init_executable=0;
            for (uint64_t i=0;i<ns;++i)
                if ((segments[i].flags&1) && modules[m].init>=segments[i].address && modules[m].init-segments[i].address<segments[i].size) init_executable=1;
            if (!init_executable) fail("module init is not executable");
            if (modules[m].tls_module)
                runtime_set_module_tls(modules[m].tls_module,image+modules[m].tls_address,modules[m].tls_filesz,modules[m].tls_memsz);
        }
        runtime_set_main_tls(image+main_tls[0],main_tls[1],main_tls[2],main_tls[3]);
        runtime_thread_attach_main();
        runtime_set_procparam(image+procparam);
        /* Dependencies start in link order (libc first), as the PS4 dynamic linker does. */
        for (uint64_t m=0;m<module_count;++m) {
            printf("Starting linked module %" PRIu64 " at image offset 0x%" PRIx64 "; native bindings=%" PRIu64 "\n",m,modules[m].init,nb);
            typedef int (ABI *ModuleInit)(uint64_t,void *,void *);
            int result=((ModuleInit)(image+modules[m].init))(0,NULL,NULL);
            printf("Module %" PRIu64 " initializer returned %d\n",m,result);
            if (result) fail("module initializer failed");
        }
    }
    printf("Entering original x86-64 code at guest offset 0x%" PRIx64 "\n", entry);
    entered_game=1;
    struct { uint64_t argc; const char *argv[2]; } params = {1, {"/app0/eboot.bin", NULL}};
    /* The guest main thread runs on a stack below 1 TiB like PS4 stacks. */
    enum { MAIN_STACK=8*1024*1024 };
    unsigned char *stack=runtime_low_map(MAIN_STACK,PF_PROT_READ|PF_PROT_WRITE);
    if (!stack) fail("cannot allocate guest main stack");
    pf_call_guest_on_stack(image+entry,(uint64_t)(uintptr_t)&params,(uint64_t)(uintptr_t)guest_exit,stack,stack+MAIN_STACK-64);
    fail("entry unexpectedly returned");
}

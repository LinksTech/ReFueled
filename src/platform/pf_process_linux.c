/* SPDX-License-Identifier: GPL-2.0-or-later */
/* refueled: pf_process on Linux. Moved code: dladdr/backtrace/maps as in src/probe.c (fault
 * report, runtime_restart, main), statm and DRM fdinfo as in gpu/shadps4/core/libraries/videoout/
 * driver.cpp (PrintMemory). */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE /* before the first system header, as in the runtime sources */
#endif
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <execinfo.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>
#include "pf_process.h"

uint32_t pf_process_id(void) { return (uint32_t)getpid(); }

/* probe.c takes dladdr(main). Any function of the executable gives the same dli_fbase, and main
 * belongs to the program, not to this layer, so a function of this file is used (this file is
 * linked into the executable). */
uintptr_t pf_executable_base(void) {
    Dl_info info;
    if (dladdr((void *)pf_process_id, &info)) return (uintptr_t)info.dli_fbase;
    return 0;
}

static void pf_copy_text(char *destination, size_t capacity, const char *source) {
    size_t length = source ? strlen(source) : 0;
    if (length >= capacity) length = capacity - 1;
    if (length) memcpy(destination, source, length);
    destination[length] = '\0';
}

int pf_symbolize(uintptr_t address, PfSymbol *out) {
    Dl_info info;
    if (!out) return PF_EINVAL;
    out->module[0] = '\0';
    out->module_base = 0;
    out->symbol[0] = '\0';
    out->symbol_address = 0;
    if (!dladdr((void *)address, &info) || !info.dli_fname) return PF_ENOENT;
    pf_copy_text(out->module, sizeof out->module, info.dli_fname);
    out->module_base = (uintptr_t)info.dli_fbase;
    pf_copy_text(out->symbol, sizeof out->symbol, info.dli_sname);
    out->symbol_address = (uintptr_t)info.dli_saddr;
    return 0;
}

/* frames[0] is the code in pf_backtrace itself (as glibc backtrace), the caller is frames[1]. */
__attribute__((noinline)) int pf_backtrace(void **frames, int capacity) {
    int count = backtrace(frames, capacity);
    __asm__ volatile("" ::: "memory"); /* no tail call: keep this frame */
    return count;
}

/* Reads /proc/self/maps with open/read and a stack buffer only (async-signal-safe), finds the
 * line containing `address` and copies it without the newline. */
int pf_describe_mapping(uintptr_t address, char *line, size_t size) {
    char chunk[1024];
    char current[256];
    size_t length = 0;
    int overlong = 0;
    int found = 0;
    int fd;
    if (!line || size == 0) return PF_EINVAL;
    line[0] = '\0';
    do {
        fd = open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
    } while (fd < 0 && errno == EINTR);
    if (fd < 0) return PF_ENOENT;
    for (;;) {
        ssize_t got = read(fd, chunk, sizeof chunk);
        if (got < 0 && errno == EINTR) continue;
        size_t end = got > 0 ? (size_t)got : 0;
        for (size_t i = 0; i <= end && !found; i++) {
            int eof = (i == end);
            if (!eof && chunk[i] != '\n') {
                if (length < sizeof current - 1) current[length++] = chunk[i];
                else overlong = 1;
                continue;
            }
            if (eof && got > 0) break; /* chunk consumed, read more */
            if (length == 0 && !overlong) continue;
            current[length] = '\0';
            {
                uintptr_t from = 0, to = 0;
                size_t p = 0;
                int digit;
                while ((digit = (current[p] >= '0' && current[p] <= '9') ? current[p] - '0'
                              : (current[p] >= 'a' && current[p] <= 'f') ? current[p] - 'a' + 10 : -1) >= 0) {
                    from = (from << 4) | (uintptr_t)digit;
                    p++;
                }
                if (p > 0 && current[p] == '-') {
                    p++;
                    while ((digit = (current[p] >= '0' && current[p] <= '9') ? current[p] - '0'
                                  : (current[p] >= 'a' && current[p] <= 'f') ? current[p] - 'a' + 10 : -1) >= 0) {
                        to = (to << 4) | (uintptr_t)digit;
                        p++;
                    }
                    if (address >= from && address < to) {
                        size_t n = length < size - 1 ? length : size - 1;
                        memcpy(line, current, n);
                        line[n] = '\0';
                        found = 1;
                    }
                }
            }
            length = 0;
            overlong = 0;
        }
        if (found || got <= 0) break;
    }
    close(fd);
    return found ? 0 : PF_ENOENT;
}

/* /proc/self/statm: size and resident in pages. */
int pf_process_memory(uint64_t *resident_bytes, uint64_t *virtual_bytes) {
    char text[128];
    int fd = open("/proc/self/statm", O_RDONLY | O_CLOEXEC);
    ssize_t got;
    unsigned long long values[2] = {0, 0};
    int index = 0, have_digit = 0;
    if (fd < 0) return PF_ENOENT;
    got = read(fd, text, sizeof text - 1);
    close(fd);
    if (got <= 0) return PF_EIO;
    for (ssize_t i = 0; i < got && index < 2; i++) {
        if (text[i] >= '0' && text[i] <= '9') {
            values[index] = values[index] * 10 + (unsigned)(text[i] - '0');
            have_digit = 1;
        } else if (have_digit) {
            index++;
            have_digit = 0;
        }
    }
    if (index < 2) return PF_EIO;
    {
        uint64_t page = (uint64_t)sysconf(_SC_PAGESIZE);
        if (virtual_bytes) *virtual_bytes = values[0] * page;
        if (resident_bytes) *resident_bytes = values[1] * page;
    }
    return 0;
}

/* The same evaluation as driver.cpp PrintMemory: every descriptor with a "drm-driver:" line,
 * each "drm-client-id" counted once, summing "drm-memory-vram" and "drm-memory-gtt" (KiB). */
int pf_gpu_memory_in_use(uint64_t *bytes) {
    unsigned long long clients[64];
    size_t client_count = 0;
    unsigned long long vram_kib = 0, gtt_kib = 0;
    DIR *dir;
    if (!bytes) return PF_EINVAL;
    *bytes = 0;
    dir = opendir("/proc/self/fdinfo");
    if (!dir) return PF_ENOENT;
    for (const struct dirent *entry; (entry = readdir(dir)) != NULL;) {
        char path[300];
        char line[160];
        FILE *file;
        int drm = 0;
        unsigned long long client = 0, vram = 0, gtt = 0;
        snprintf(path, sizeof path, "/proc/self/fdinfo/%s", entry->d_name);
        file = fopen(path, "r");
        if (!file) continue;
        while (fgets(line, sizeof line, file)) {
            drm |= strncmp(line, "drm-driver:", 11) == 0;
            sscanf(line, "drm-client-id: %llu", &client);
            sscanf(line, "drm-memory-vram: %llu", &vram);
            sscanf(line, "drm-memory-gtt: %llu", &gtt);
        }
        fclose(file);
        if (drm) {
            int known = 0;
            for (size_t i = 0; i < client_count; i++) known |= clients[i] == client;
            if (!known) {
                if (client_count < sizeof clients / sizeof clients[0]) clients[client_count++] = client;
                vram_kib += vram;
                gtt_kib += gtt;
            }
        }
    }
    closedir(dir);
    *bytes = (uint64_t)(vram_kib + gtt_kib) * 1024u;
    return 0;
}

/* As runtime_restart in probe.c: close every descriptor above 2, exec "bash run.sh". */
int pf_restart_through_launcher(void) {
    syscall(SYS_close_range, 3u, ~0u, 0u);
    execlp("bash", "bash", "run.sh", (char *)NULL);
    return PF_EIO;
}

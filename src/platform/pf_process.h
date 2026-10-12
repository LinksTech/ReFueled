/* SPDX-License-Identifier: GPL-2.0-or-later */
/* refueled: process information, symbolisation, restart (replaces /proc, dladdr, backtrace,
 * execlp). */
#ifndef REFUELED_PF_PROCESS_H
#define REFUELED_PF_PROCESS_H
#include "pf_base.h"

PF_EXTERN_C_BEGIN

uint32_t pf_process_id(void);

/* Load address of the main executable (dladdr(main) / GetModuleHandle(NULL)). */
uintptr_t pf_executable_base(void);

typedef struct {
    char module[260];          /* path or file name of the containing module, "" if none */
    uintptr_t module_base;
    char symbol[128];          /* nearest exported symbol, "" if unknown */
    uintptr_t symbol_address;
} PfSymbol;

/* Describes the module/symbol containing `address` (dladdr / GetModuleHandleExW(FROM_ADDRESS) +
 * optional DbgHelp). Returns 0 when the address is inside a module, PF_ENOENT otherwise.
 * Not async-signal-safe on either platform; fatal-fault reports may still call it. */
int pf_symbolize(uintptr_t address, PfSymbol *out);

/* Return addresses of the calling thread's host frames (backtrace /
 * RtlCaptureStackBackTrace). Returns the count. */
int pf_backtrace(void **frames, int capacity);

/* One line describing the mapping that contains `address` ("start-end perms offset name", as a
 * /proc/self/maps line / from VirtualQuery). Returns 0 or PF_ENOENT. Async-signal-safe. */
int pf_describe_mapping(uintptr_t address, char *line, size_t size);

/* Resident and virtual size of the process in bytes (/proc/self/statm /
 * GetProcessMemoryInfo). */
int pf_process_memory(uint64_t *resident_bytes, uint64_t *virtual_bytes);

/* GPU memory the process holds, in bytes, where the platform reports it (Linux: DRM fdinfo of
 * the process's GPU file descriptors; Windows: PF_ENOSYS until implemented). */
int pf_gpu_memory_in_use(uint64_t *bytes);

/* Ends this process and starts the game again through the launch script: closes every
 * descriptor above 2 so the old GPU device is released, then execs "bash run.sh" (Linux,
 * unchanged); Windows: starts the launch command line as a new process without inherited
 * handles and exits. Returns only on failure (PF_E*). */
int pf_restart_through_launcher(void);

PF_EXTERN_C_END
#endif

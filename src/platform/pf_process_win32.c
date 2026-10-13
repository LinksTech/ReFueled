/* SPDX-License-Identifier: GPL-2.0-or-later */
/* refueled: pf_process on Windows. No DbgHelp yet: symbol names stay empty. */
#include <string.h>
#include <wchar.h>
#include <windows.h>
#include <psapi.h>
#include "pf_process.h"

#ifdef _MSC_VER
#define PF_NOINLINE __declspec(noinline)
#else
#define PF_NOINLINE __attribute__((noinline))
#endif

uint32_t pf_process_id(void) { return (uint32_t)GetCurrentProcessId(); }

uintptr_t pf_executable_base(void) { return (uintptr_t)GetModuleHandleW(NULL); }

int pf_symbolize(uintptr_t address, PfSymbol *out) {
    HMODULE module = NULL;
    DWORD length;
    if (!out) return PF_EINVAL;
    out->module[0] = '\0';
    out->module_base = 0;
    out->symbol[0] = '\0';
    out->symbol_address = 0;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCWSTR)address, &module) ||
        !module)
        return PF_ENOENT;
    length = GetModuleFileNameA(module, out->module, (DWORD)sizeof out->module);
    if (length == 0 || length >= sizeof out->module) out->module[length == 0 ? 0 : sizeof out->module - 1] = '\0';
    out->module_base = (uintptr_t)module;
    return 0;
}

static volatile int pf_backtrace_last;

/* frames[0] is pf_backtrace itself (return address of RtlCaptureStackBackTrace), the caller is
 * frames[1]: the same as glibc backtrace on Linux. */
PF_NOINLINE int pf_backtrace(void **frames, int capacity) {
    int count;
    if (!frames || capacity <= 0) return 0;
    count = (int)RtlCaptureStackBackTrace(0, (DWORD)capacity, frames, NULL);
    pf_backtrace_last = count; /* work after the call: no tail call, the frame stays */
    return count;
}

static size_t pf_hex(char *out, uint64_t value, int digits) {
    static const char table[] = "0123456789abcdef";
    for (int i = 0; i < digits; i++) out[i] = table[(value >> (4 * (digits - 1 - i))) & 0xf];
    return (size_t)digits;
}

/* "start-end perms offset name" like a /proc/self/maps line, built by hand (no stdio): the
 * region VirtualQuery reports, perms from Protect, 's' for MEM_MAPPED else 'p'. Free and
 * reserved regions: PF_ENOENT. */
int pf_describe_mapping(uintptr_t address, char *line, size_t size) {
    MEMORY_BASIC_INFORMATION info;
    size_t n = 0;
    char text[400];
    DWORD protect;
    if (!line || size == 0) return PF_EINVAL;
    line[0] = '\0';
    if (VirtualQuery((LPCVOID)address, &info, sizeof info) == 0) return PF_ENOENT;
    if (info.State != MEM_COMMIT) return PF_ENOENT;
    protect = info.Protect & 0xff;
    n += pf_hex(text + n, (uint64_t)(uintptr_t)info.BaseAddress, 12);
    text[n++] = '-';
    n += pf_hex(text + n, (uint64_t)((uintptr_t)info.BaseAddress + info.RegionSize), 12);
    text[n++] = ' ';
    text[n++] = (protect != PAGE_NOACCESS && protect != 0) ? 'r' : '-';
    text[n++] = (protect == PAGE_READWRITE || protect == PAGE_WRITECOPY || protect == PAGE_EXECUTE_READWRITE ||
                 protect == PAGE_EXECUTE_WRITECOPY)
                    ? 'w'
                    : '-';
    text[n++] = (protect == PAGE_EXECUTE || protect == PAGE_EXECUTE_READ || protect == PAGE_EXECUTE_READWRITE ||
                 protect == PAGE_EXECUTE_WRITECOPY)
                    ? 'x'
                    : '-';
    text[n++] = info.Type == MEM_MAPPED ? 's' : 'p';
    text[n++] = ' ';
    n += pf_hex(text + n, 0, 8);
    if (info.Type == MEM_IMAGE || info.Type == MEM_MAPPED) {
        char name[200];
        DWORD got = K32GetMappedFileNameA(GetCurrentProcess(), (LPVOID)address, name, (DWORD)sizeof name);
        if (got > 0 && got < sizeof name) {
            text[n++] = ' ';
            memcpy(text + n, name, got);
            n += got;
        }
    }
    if (n > size - 1) n = size - 1;
    memcpy(line, text, n);
    line[n] = '\0';
    return 0;
}

/* resident = working set; virtual = PrivateUsage (committed private memory), the closest
 * counterpart of the Linux statm size for what the process holds. */
int pf_process_memory(uint64_t *resident_bytes, uint64_t *virtual_bytes) {
    PROCESS_MEMORY_COUNTERS_EX counters;
    memset(&counters, 0, sizeof counters);
    counters.cb = sizeof counters;
    if (!K32GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&counters, sizeof counters))
        return PF_EIO;
    if (resident_bytes) *resident_bytes = (uint64_t)counters.WorkingSetSize;
    if (virtual_bytes) *virtual_bytes = (uint64_t)counters.PrivateUsage;
    return 0;
}

/* Later: VK_EXT_memory_budget. */
int pf_gpu_memory_in_use(uint64_t *bytes) {
    (void)bytes;
    return pf_not_implemented(__func__);
}

/* Starts the own command line as a new process without inherited handles and ends this one. */
int pf_restart_through_launcher(void) {
    STARTUPINFOW startup;
    PROCESS_INFORMATION process;
    const wchar_t *original = GetCommandLineW();
    size_t count = wcslen(original) + 1;
    wchar_t *command = (wchar_t *)HeapAlloc(GetProcessHeap(), 0, count * sizeof(wchar_t));
    if (!command) return PF_EIO;
    memcpy(command, original, count * sizeof(wchar_t)); /* CreateProcessW may modify the buffer */
    memset(&startup, 0, sizeof startup);
    startup.cb = sizeof startup;
    memset(&process, 0, sizeof process);
    if (!CreateProcessW(NULL, command, NULL, NULL, FALSE, 0, NULL, NULL, &startup, &process)) {
        HeapFree(GetProcessHeap(), 0, command);
        return PF_EIO;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    ExitProcess(0);
}

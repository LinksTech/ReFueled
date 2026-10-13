/* SPDX-License-Identifier: GPL-2.0-or-later */
/* refueled: host threads beyond the POSIX subset on Windows (pf_thread.h).
 *
 * Guest stacks: the thread starts on a small winpthreads stack and switches to the given one with
 * the TEB stack bounds moved (pf_stack_win32.S, D7). Sampling another thread suspends it from the
 * calling thread (SuspendThread + GetThreadContext); the callback therefore runs on the caller,
 * not on the target as on Linux, and must not take locks the target may hold. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include "pf_thread.h"
#include "pf_win32_internal.h"

void *pf_win32_run_on_stack(void *(*function)(void *), void *argument, void *stack_low, void *stack_top);

/* ---- Stacks ------------------------------------------------------------------------------ */

typedef struct {
    void *(*start)(void *);
    void *argument;
    unsigned char *stack;
    size_t size;
} Launch;

static void *launch(void *parameter) {
    Launch launch_copy = *(Launch *)parameter;
    free(parameter);
    return pf_win32_run_on_stack(launch_copy.start, launch_copy.argument, launch_copy.stack,
                                 launch_copy.stack + launch_copy.size);
}

int pf_thread_create_on_stack(pthread_t *out, void *(*start)(void *), void *argument, void *stack, size_t size) {
    pthread_attr_t attributes;
    pthread_t created;
    int result = pthread_attr_init(&attributes);
    if (result) return result;
    if (!stack) {
        if (size) pthread_attr_setstacksize(&attributes, size);
        result = pthread_create(out ? out : &created, &attributes, start, argument);
    } else {
        Launch *parameter = malloc(sizeof *parameter);
        if (!parameter) {
            pthread_attr_destroy(&attributes);
            return ENOMEM;
        }
        *parameter = (Launch){start, argument, stack, size};
        pthread_attr_setstacksize(&attributes, 256 * 1024); /* only the switch runs on it */
        result = pthread_create(out ? out : &created, &attributes, launch, parameter);
        if (result) free(parameter);
    }
    pthread_attr_destroy(&attributes);
    return result;
}

/* pf_call_guest_on_stack: pf_stack_win32.S */

int pf_thread_get_stack(void **base, size_t *size) {
    ULONG_PTR low = 0, high = 0;
    GetCurrentThreadStackLimits(&low, &high); /* DeallocationStack .. StackBase, also when moved */
    if (base) *base = (void *)low;
    if (size) *size = (size_t)(high - low);
    return 0;
}

/* ---- Identity ---------------------------------------------------------------------------- */

uint64_t pf_thread_id(void) { return GetCurrentThreadId(); }

/* Windows 10 1607+; looked up at run time like the other newer kernel32 functions. */
typedef HRESULT(WINAPI *SetThreadDescriptionFn)(HANDLE, PCWSTR);
typedef HRESULT(WINAPI *GetThreadDescriptionFn)(HANDLE, PWSTR *);
static SetThreadDescriptionFn set_description;
static GetThreadDescriptionFn get_description;
static INIT_ONCE description_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK load_description(PINIT_ONCE once, PVOID parameter, PVOID *context) {
    (void)once;
    (void)parameter;
    (void)context;
    HMODULE kernel = GetModuleHandleW(L"kernelbase.dll");
    if (kernel) {
        set_description = (SetThreadDescriptionFn)(void (*)(void))GetProcAddress(kernel, "SetThreadDescription");
        get_description = (GetThreadDescriptionFn)(void (*)(void))GetProcAddress(kernel, "GetThreadDescription");
    }
    return TRUE;
}

int pf_thread_set_name(const char *name) {
    InitOnceExecuteOnce(&description_once, load_description, NULL, NULL);
    if (!name) return PF_EINVAL;
    if (!set_description) return PF_ENOSYS;
    wchar_t wide[64];
    if (!MultiByteToWideChar(CP_UTF8, 0, name, -1, wide, 64)) wide[63] = 0;
    return SUCCEEDED(set_description(GetCurrentThread(), wide)) ? 0 : PF_EIO;
}

static int name_of(HANDLE thread, char *buffer, size_t size) {
    InitOnceExecuteOnce(&description_once, load_description, NULL, NULL);
    if (!buffer || !size) return PF_EINVAL;
    buffer[0] = 0;
    if (!get_description) return PF_ENOSYS;
    PWSTR wide = NULL;
    if (FAILED(get_description(thread, &wide)) || !wide) return PF_EIO;
    int n = WideCharToMultiByte(CP_UTF8, 0, wide, -1, buffer, (int)(size > 0x7fffffff ? 0x7fffffff : size), NULL, NULL);
    if (n == 0) buffer[size - 1] = 0;
    LocalFree(wide);
    return 0;
}

int pf_thread_get_name(char *buffer, size_t size) { return name_of(GetCurrentThread(), buffer, size); }

/* ---- Scheduling -------------------------------------------------------------------------- */

int pf_thread_set_idle_priority(void) {
    return SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_IDLE) ? 0 : PF_EPERM;
}

int pf_cpu_count_allowed(void) {
    DWORD_PTR process = 0, system = 0;
    if (!GetProcessAffinityMask(GetCurrentProcess(), &process, &system) || !process) return 1;
    int count = 0;
    for (; process; process &= process - 1) ++count;
    return count;
}

int pf_thread_cpu_time_ns(uint64_t *nanoseconds) {
    FILETIME created, exited, kernel, user;
    if (!nanoseconds) return PF_EINVAL;
    if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user)) return PF_EIO;
    const uint64_t k = ((uint64_t)kernel.dwHighDateTime << 32) | kernel.dwLowDateTime;
    const uint64_t u = ((uint64_t)user.dwHighDateTime << 32) | user.dwLowDateTime;
    *nanoseconds = (k + u) * 100;
    return 0;
}

/* ---- Other threads ----------------------------------------------------------------------- */

int pf_threads_list(void (*visit)(uint64_t tid, const char *name, void *user), void *user) {
    if (!visit) return PF_EINVAL;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return PF_EIO;
    THREADENTRY32 entry;
    entry.dwSize = sizeof entry;
    const DWORD self = GetCurrentProcessId();
    int count = 0;
    for (BOOL more = Thread32First(snapshot, &entry); more; more = Thread32Next(snapshot, &entry)) {
        if (entry.th32OwnerProcessID != self) continue;
        char name[64] = "";
        HANDLE thread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ThreadID);
        if (thread) {
            name_of(thread, name, sizeof name);
            CloseHandle(thread);
        }
        visit(entry.th32ThreadID, name, user);
        ++count;
    }
    CloseHandle(snapshot);
    return count;
}

int pf_thread_sample(uint64_t tid, void (*sample)(const PfContext *context, void *user), void *user) {
    if (!sample) return PF_EINVAL;
    if (tid == GetCurrentThreadId()) return PF_EINVAL; /* a thread cannot suspend itself here */
    HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, (DWORD)tid);
    if (!thread) return GetLastError() == ERROR_INVALID_PARAMETER ? PF_ENOENT : PF_EACCES;
    int result = 0;
    if (SuspendThread(thread) == (DWORD)-1) {
        result = PF_EIO;
    } else {
        CONTEXT native; /* GetThreadContext also waits until the suspension took effect */
        memset(&native, 0, sizeof native);
        native.ContextFlags = CONTEXT_FULL;
        if (GetThreadContext(thread, &native)) {
            PfContext context = {&native, PF_ACCESS_UNKNOWN};
            sample(&context, user);
        } else {
            result = PF_EIO;
        }
        ResumeThread(thread);
    }
    CloseHandle(thread);
    return result;
}

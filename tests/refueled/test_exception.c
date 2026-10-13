/* SPDX-License-Identifier: GPL-2.0-or-later */
/* refueled: unit test for pf_exception.h (faults, resume, breakpoints, single step, handler order,
 * unhandled faults, watchdog) and pf_context_get/set. Cases run through pf_* only; triggers and
 * helpers that have no portable form sit in the #ifdef blocks (Linux now; the Windows variants
 * are added with K7). */
#include "platform/pf_exception.h"
#include "platform/pf_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "platform/pf_win32_internal.h"
#else
#include <sys/mman.h>
#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>
#include "platform/pf_linux_internal.h"
#endif

static int g_checks, g_failed;
#define CHECK(cond) do { g_checks++; if (!(cond)) { g_failed++; \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

/* ---- platform helpers -------------------------------------------------------------------- */

#ifndef _WIN32
#define PAGE 4096
static void *page_new(int writable) {
    void *p = mmap(NULL, PAGE, writable ? PROT_READ | PROT_WRITE : PROT_READ,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? NULL : p;
}
static void page_unprotect(void *p) { mprotect(p, PAGE, PROT_READ | PROT_WRITE); }
static void *page_unmapped(void) {
    void *p = page_new(1);
    if (p) munmap(p, PAGE);
    return p;
}
/* int3 with the address after it stored in *after (the convention: RIP is behind the int3). */
static void trigger_breakpoint(volatile uint64_t *after) {
    __asm__ volatile("lea 1f(%%rip), %%rax\n\t"
                     "mov %%rax, %0\n\t"
                     "int3\n"
                     "1:"
                     : "=m"(*after) : : "rax", "memory");
}
static void trigger_single_step(void) {
    __asm__ volatile("pushfq\n\t"
                     "orq $0x100, (%%rsp)\n\t"
                     "popfq\n\t"
                     "nop\n\t"
                     "nop\n\t"
                     : : : "memory", "cc");
}
#define TF_BIT 0x100u
#define HOST_ACCESS_CODE SIGSEGV
static void page_free(void *p) { munmap(p, PAGE); }
#else
#define PAGE 4096
#define TF_BIT 0x100u
#define HOST_ACCESS_CODE ((int)EXCEPTION_ACCESS_VIOLATION)
static void *page_new(int writable) {
    return VirtualAlloc(NULL, PAGE, MEM_RESERVE | MEM_COMMIT, writable ? PAGE_READWRITE : PAGE_READONLY);
}
static void page_unprotect(void *p) {
    DWORD old;
    VirtualProtect(p, PAGE, PAGE_READWRITE, &old);
}
static void page_free(void *p) { VirtualFree(p, 0, MEM_RELEASE); }
static void *page_unmapped(void) {
    void *p = page_new(1);
    if (p) page_free(p);
    return p;
}
/* Generated code (works with every compiler): int3; ret, and pushfq; or qword [rsp], 0x100;
 * popfq; nop; nop; ret. */
static unsigned char *code_page;
static void code_init(void) {
    static const unsigned char code[] = {0xcc, 0xc3, 0x90, 0x90,
                                         0x9c, 0x48, 0x81, 0x0c, 0x24, 0x00, 0x01, 0x00, 0x00, 0x9d, 0x90, 0x90, 0xc3};
    code_page = VirtualAlloc(NULL, PAGE, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    if (code_page) memcpy(code_page, code, sizeof code);
}
static void trigger_breakpoint(volatile uint64_t *after) {
    *after = (uint64_t)(uintptr_t)(code_page + 1);
    ((void (*)(void))(void *)code_page)();
}
static void trigger_single_step(void) { ((void (*)(void))(void *)(code_page + 4))(); }
#endif

/* ---- handlers ---------------------------------------------------------------------------- */

enum { MODE_NONE, MODE_UNPROTECT, MODE_RESUME, MODE_ORDER = 5 };
static volatile int g_mode;
static void *volatile g_page;
static pf_sigjmp_buf *volatile g_env;
static volatile int g_order[4], g_order_count;
static volatile uintptr_t g_fault_address;
static volatile int g_host_code, g_breakpoints, g_steps, g_bad_rip;
static volatile uint64_t g_after_int3;

static PfFaultResult first_handler(const PfFault *fault, PfContext *context, void *user) {
    (void)user;
    if (g_mode == MODE_ORDER) {
        g_order[g_order_count++] = 1;
        return PF_FAULT_CONTINUE_SEARCH;
    }
    if (g_mode == MODE_RESUME) {
        g_fault_address = fault->address;
        return pf_fault_resume_at(context, *g_env, 1);
    }
    return PF_FAULT_CONTINUE_SEARCH;
}
static PfFaultResult second_handler(const PfFault *fault, PfContext *context, void *user) {
    (void)context;
    (void)user;
    if (g_mode == MODE_ORDER) g_order[g_order_count++] = 2;
    if (g_mode == MODE_UNPROTECT || g_mode == MODE_ORDER) {
        g_fault_address = fault->address;
        g_host_code = fault->host_code;
        page_unprotect(g_page);
        return PF_FAULT_HANDLED;
    }
    return PF_FAULT_CONTINUE_SEARCH;
}
static PfFaultResult trap_handler(const PfFault *fault, PfContext *context, void *user) {
    (void)user;
    if (fault->kind == PF_FAULT_BREAKPOINT) {
        g_breakpoints++;
        if (pf_context_get(context, PF_REG_RIP) != g_after_int3) g_bad_rip = 1;
        return PF_FAULT_HANDLED;
    }
    g_steps++;
    pf_context_set(context, PF_REG_EFLAGS, pf_context_get(context, PF_REG_EFLAGS) & ~(uint64_t)TF_BIT);
    return PF_FAULT_HANDLED;
}

/* ---- tests ------------------------------------------------------------------------------- */

static void test_registration(void) {
    CHECK(pf_fault_add_handler(0, first_handler, NULL) == PF_EINVAL);
    CHECK(pf_fault_add_handler(1u << PF_FAULT_ACCESS, NULL, NULL) == PF_EINVAL);
    CHECK(pf_fault_add_handler(0x80000000u, first_handler, NULL) == PF_EINVAL);
    CHECK(pf_fault_add_handler((1u << PF_FAULT_ACCESS) | (1u << PF_FAULT_BUS), first_handler, NULL) == 0);
    CHECK(pf_fault_add_handler((1u << PF_FAULT_ACCESS) | (1u << PF_FAULT_BUS), second_handler, NULL) == 0);
    CHECK(pf_fault_add_handler((1u << PF_FAULT_BREAKPOINT) | (1u << PF_FAULT_SINGLE_STEP), trap_handler, NULL) == 0);
}

#ifndef _WIN32
static void test_context(void) {
    ucontext_t uc;
    memset(&uc, 0, sizeof uc);
    PfContext context = {&uc};
    CHECK(pf_context_native(&context) == &uc);
    for (int r = 0; r < PF_REG_COUNT; r++) pf_context_set(&context, (PfReg)r, 0x1000u + (uint64_t)r);
    int ok = 1;
    for (int r = 0; r < PF_REG_COUNT; r++) if (pf_context_get(&context, (PfReg)r) != 0x1000u + (uint64_t)r) ok = 0;
    CHECK(ok);
    CHECK(uc.uc_mcontext.gregs[REG_RIP] == 0x1000 + PF_REG_RIP);
    CHECK(uc.uc_mcontext.gregs[REG_EFL] == 0x1000 + PF_REG_EFLAGS);
    CHECK(uc.uc_mcontext.gregs[REG_R15] == 0x1000 + PF_REG_R15);
}
#else
static void test_context(void) {
    CONTEXT native;
    memset(&native, 0, sizeof native);
    PfContext context = {&native, PF_ACCESS_UNKNOWN};
    CHECK(pf_context_native(&context) == &native);
    for (int r = 0; r < PF_REG_COUNT; r++) pf_context_set(&context, (PfReg)r, 0x1000u + (uint64_t)r);
    int ok = 1;
    for (int r = 0; r < PF_REG_COUNT; r++) if (pf_context_get(&context, (PfReg)r) != 0x1000u + (uint64_t)r) ok = 0;
    CHECK(ok);
    CHECK(native.Rip == 0x1000 + PF_REG_RIP && native.EFlags == 0x1000 + PF_REG_EFLAGS && native.R15 == 0x1000 + PF_REG_R15);
}
#endif

static void test_write_fault(void) {
    volatile int *p = page_new(0);
    CHECK(p != NULL);
    g_page = (void *)p;
    g_mode = MODE_UNPROTECT;
    *p = 7; /* faults, the handler lifts the protection, the write is repeated */
    CHECK(*p == 7);
    CHECK(g_fault_address == (uintptr_t)p);
    CHECK(g_host_code == HOST_ACCESS_CODE);
    g_mode = MODE_NONE;
    page_free((void *)p);
}

static void test_resume(void) {
    static pf_sigjmp_buf env;
    volatile int jumped = 0;
    volatile int *bad = page_unmapped();
    for (int round = 0; round < 2; round++) { /* the second round shows SIGSEGV is unblocked again */
        if (pf_sigsetjmp(env) == 0) {
            g_env = &env;
            g_mode = MODE_RESUME;
            (void)*bad;
            g_mode = MODE_NONE;
        } else {
            g_mode = MODE_NONE;
            jumped++;
        }
    }
    CHECK(jumped == 2);
    CHECK(g_fault_address == (uintptr_t)bad);
}

static void test_breakpoint(void) {
    g_breakpoints = 0;
    g_bad_rip = 0;
    trigger_breakpoint(&g_after_int3);
    CHECK(g_breakpoints == 1);
    CHECK(g_bad_rip == 0);
    CHECK(g_after_int3 != 0);
}

static void test_single_step(void) {
    g_steps = 0;
    trigger_single_step();
    CHECK(g_steps == 1);
}

static void test_order(void) {
    volatile int *p = page_new(0);
    g_page = (void *)p;
    g_order_count = 0;
    g_mode = MODE_ORDER;
    *p = 1;
    g_mode = MODE_NONE;
    CHECK(g_order_count == 2 && g_order[0] == 1 && g_order[1] == 2);
    CHECK(*p == 1);
    page_free((void *)p);
}

#ifndef _WIN32
static void watchdog_exit(PfContext *context, void *user) {
    (void)context;
    pf_exit_now((int)(intptr_t)user);
}

static int wait_child(pid_t pid, int *status) { return waitpid(pid, status, 0) == pid; }

static void test_unhandled(void) {
    fflush(NULL);
    pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        g_mode = MODE_NONE;
        *(volatile int *)page_unmapped() = 1; /* nobody handles it: default action */
        _exit(0);
    }
    int status = 0;
    CHECK(wait_child(pid, &status));
    CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV);
}

static void test_watchdog(void) {
    fflush(NULL);
    pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        pf_watchdog_start(1, watchdog_exit, (void *)(intptr_t)42);
        for (;;) pause();
    }
    int status = 0;
    CHECK(wait_child(pid, &status));
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 42);

    pid = fork(); /* 0 switches it off again */
    CHECK(pid >= 0);
    if (pid == 0) {
        pf_watchdog_start(1, watchdog_exit, (void *)(intptr_t)42);
        pf_watchdog_start(0, NULL, NULL);
        sleep(2);
        _exit(7);
    }
    CHECK(wait_child(pid, &status));
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 7);
}
#else
/* Children are this program started again with a role argument. */
static DWORD run_child(const char *role) {
    char path[MAX_PATH], command[MAX_PATH + 64];
    GetModuleFileNameA(NULL, path, sizeof path);
    snprintf(command, sizeof command, "\"%s\" %s", path, role);
    STARTUPINFOA startup;
    PROCESS_INFORMATION process;
    memset(&startup, 0, sizeof startup);
    startup.cb = sizeof startup;
    if (!CreateProcessA(NULL, command, NULL, NULL, FALSE, 0, NULL, NULL, &startup, &process)) return 0xffffffffu;
    WaitForSingleObject(process.hProcess, 20000);
    DWORD code = 0xfffffffeu;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return code;
}

static void watchdog_exit(PfContext *context, void *user) {
    if (context != NULL) pf_exit_now(99); /* Windows passes no context */
    pf_exit_now((int)(intptr_t)user);
}

static int child(const char *role) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX); /* no crash dialog */
    if (!strcmp(role, "--unhandled")) {
        pf_fault_add_handler(1u << PF_FAULT_ACCESS, first_handler, NULL); /* declines (MODE_NONE) */
        *(volatile int *)page_unmapped() = 1;
        return 0;
    }
    if (!strcmp(role, "--watchdog")) {
        pf_watchdog_start(1, watchdog_exit, (void *)(intptr_t)42);
        Sleep(10000);
        return 5;
    }
    if (!strcmp(role, "--watchdog-off")) {
        pf_watchdog_start(1, watchdog_exit, (void *)(intptr_t)42);
        pf_watchdog_start(0, NULL, NULL);
        Sleep(2000);
        return 7;
    }
    return 3;
}

static void test_unhandled(void) { CHECK(run_child("--unhandled") == (DWORD)EXCEPTION_ACCESS_VIOLATION); }

static void test_watchdog(void) {
    CHECK(run_child("--watchdog") == 42);
    CHECK(run_child("--watchdog-off") == 7);
}

/* A fault inside a view that pf_memory_win32.c is remapping right now is repeated; outside, not. */
static void test_transient(void) { CHECK(pf_win32_memory_transient_fault(0x10, PF_ACCESS_READ) == 0); }
#endif

int main(int argc, char **argv) {
#ifdef _WIN32
    if (argc == 2) return child(argv[1]);
    code_init();
    test_transient();
#else
    (void)argc;
    (void)argv;
#endif
    test_registration();
    test_context();
    test_write_fault();
#if !defined(_MSC_VER)
    test_resume(); /* needs pf_setjmp_impl (Windows: pf_jmp_win32.S, K6) */
#else
    (void)test_resume;
#endif
    test_breakpoint();
    test_single_step();
    test_order();
    test_unhandled();
    test_watchdog();
    if (g_failed) { printf("test_exception: %d of %d checks FAILED\n", g_failed, g_checks); return 1; }
    printf("test_exception: %d checks passed\n", g_checks);
    return 0;
}

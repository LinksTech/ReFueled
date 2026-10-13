/* SPDX-License-Identifier: GPL-2.0-or-later */
/* refueled: calls every Windows stub once and checks the PF_ENOSYS / NULL / 0 result.
 * The once-per-function stderr notice is checked by eye (each name appears once in the output
 * although pf_not_implemented is called twice for the probe name). pf_longjmp_impl is not called
 * (abort) and neither is pf_exit_now (ends the process). */
#include <stdio.h>
#include <string.h>
#include "platform/platform.h"

static int checks;
static int failures;

#define CHECK(cond) \
    do { \
        checks++; \
        if (!(cond)) { \
            failures++; \
            fprintf(stderr, "test_stubs: FAILED line %d: %s\n", __LINE__, #cond); \
        } \
    } while (0)

int main(void) {
    CHECK(strcmp(pf_platform_name(), "win32") == 0);

    CHECK(pf_not_implemented("test_stubs_probe") == PF_ENOSYS);
    CHECK(pf_not_implemented("test_stubs_probe") == PF_ENOSYS); /* second call: no second notice */

    /* pf_tls: implemented (K4), tested in test_tls.c. */

    CHECK(pf_context_get(NULL, PF_REG_RAX) == 0);
    pf_context_set(NULL, PF_REG_RAX, 0);
    CHECK(pf_context_native(NULL) == NULL);
    CHECK(pf_fault_add_handler(0, NULL, NULL) == PF_ENOSYS);
    CHECK(pf_fault_resume_at(NULL, NULL, 1) == PF_FAULT_CONTINUE_SEARCH);
    CHECK(pf_watchdog_start(0, NULL, NULL) == PF_ENOSYS);

    CHECK(pf_thread_create_on_stack(NULL, NULL, NULL, NULL, 0) == PF_ENOSYS);
    CHECK(pf_call_guest_on_stack(NULL, 0, 0, NULL, NULL) == 0);
    CHECK(pf_thread_get_stack(NULL, NULL) == PF_ENOSYS);
    CHECK(pf_thread_id() == 0);
    CHECK(pf_thread_set_name(NULL) == PF_ENOSYS);
    CHECK(pf_thread_get_name(NULL, 0) == PF_ENOSYS);
    CHECK(pf_thread_set_idle_priority() == PF_ENOSYS);
    CHECK(pf_cpu_count_allowed() == PF_ENOSYS);
    CHECK(pf_thread_cpu_time_ns(NULL) == PF_ENOSYS);
    CHECK(pf_threads_list(NULL, NULL) == PF_ENOSYS);
    CHECK(pf_thread_sample(0, NULL, NULL) == PF_ENOSYS);

    CHECK(pf_fs_remove_tree(NULL) == PF_ENOSYS);
    CHECK(pf_fs_rename_replace(NULL, NULL) == PF_ENOSYS);

    CHECK(pf_process_id() == 0);
    CHECK(pf_executable_base() == 0);
    CHECK(pf_symbolize(0, NULL) == PF_ENOSYS);
    CHECK(pf_backtrace(NULL, 0) == PF_ENOSYS);
    CHECK(pf_describe_mapping(0, NULL, 0) == PF_ENOSYS);
    CHECK(pf_process_memory(NULL, NULL) == PF_ENOSYS);
    CHECK(pf_gpu_memory_in_use(NULL) == PF_ENOSYS);
    CHECK(pf_restart_through_launcher() == PF_ENOSYS);

    {
        pf_jmp_buf env;
        CHECK(pf_setjmp_impl(env) == 0);
    }

    CHECK(pf_init() == 0); /* pf_tls_init is real since K4 */

    pf_write_stderr_str("test_stubs: pf_write_stderr_str ok\n");
    if (failures) {
        fprintf(stderr, "test_stubs: %d of %d checks FAILED\n", failures, checks);
        return 1;
    }
    printf("test_stubs: %d checks passed\n", checks);
    return 0;
}

/* SPDX-License-Identifier: GPL-2.0-or-later */
/* refueled: the Windows functions that are still stubs return PF_ENOSYS / NULL and say so once;
 * pf_not_implemented reports a name only once (checked by eye: the probe name appears once in the
 * output although it is reported twice). */
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

    /* Implemented since F2: pf_tls (K4), pf_exception (K7), pf_thread + pf_jmp (K6), pf_fs (R2),
     * pf_process (R3), pf_memory (K1), pf_low_* (LH); each has its own test. Not called here:
     * several have side effects (pf_restart_through_launcher starts a new process). What is
     * still a stub on Windows: */
    CHECK(pf_gpu_memory_in_use(NULL) == PF_ENOSYS);
    CHECK(pf_handle_map_fixed(PF_MEM_HANDLE_NONE, 0, 0, 0, 0) == PF_ENOSYS);
    CHECK(pf_handle_map(PF_MEM_HANDLE_NONE, 0, 0, 0) == NULL);

    CHECK(pf_init() == 0); /* pf_tls_init is real since K4 */

    pf_write_stderr_str("test_stubs: pf_write_stderr_str ok\n");
    if (failures) {
        fprintf(stderr, "test_stubs: %d of %d checks FAILED\n", failures, checks);
        return 1;
    }
    printf("test_stubs: %d checks passed\n", checks);
    return 0;
}

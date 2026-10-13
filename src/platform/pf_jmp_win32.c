/* SPDX-License-Identifier: GPL-2.0-or-later */
/* refueled: provisional stubs. K6 replaces this file with pf_jmp_win32.S (the register-only
 * setjmp/longjmp pair). */
#include <stdlib.h>
#include "pf_jmp.h"

int pf_setjmp_impl(pf_jmp_buf env) {
    (void)env;
    pf_not_implemented(__func__);
    return 0;
}

void pf_longjmp_impl(pf_jmp_buf env, int value) {
    (void)env;
    (void)value;
    pf_not_implemented(__func__);
    abort();
}

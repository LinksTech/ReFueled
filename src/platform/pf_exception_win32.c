/* SPDX-License-Identifier: GPL-2.0-or-later */
/* refueled: stubs, K7 replaces them. */
#include <stdlib.h>
#include "pf_exception.h"

uint64_t pf_context_get(const PfContext *context, PfReg reg) {
    (void)context;
    (void)reg;
    pf_not_implemented(__func__);
    return 0;
}

void pf_context_set(PfContext *context, PfReg reg, uint64_t value) {
    (void)context;
    (void)reg;
    (void)value;
    pf_not_implemented(__func__);
}

void *pf_context_native(PfContext *context) {
    (void)context;
    pf_not_implemented(__func__);
    return NULL;
}

int pf_fault_add_handler(unsigned kinds, PfFaultHandler handler, void *user) {
    (void)kinds;
    (void)handler;
    (void)user;
    return pf_not_implemented(__func__);
}

PfFaultResult pf_fault_resume_at(PfContext *context, pf_sigjmp_buf env, int value) {
    (void)context;
    (void)env;
    (void)value;
    pf_not_implemented(__func__);
    return PF_FAULT_CONTINUE_SEARCH;
}

/* Noreturn, so it cannot be a pure stub: it logs the notice and then really ends the process,
 * which is all the function means. K7 replaces it. */
void pf_exit_now(int status) {
    pf_not_implemented(__func__);
    _Exit(status);
}

int pf_watchdog_start(unsigned seconds, void (*on_timeout)(PfContext *context, void *user), void *user) {
    (void)seconds;
    (void)on_timeout;
    (void)user;
    return pf_not_implemented(__func__);
}

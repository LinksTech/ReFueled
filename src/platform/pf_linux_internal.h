/* SPDX-License-Identifier: GPL-2.0-or-later */
/* refueled: declarations shared between the Linux files of the platform layer only
 * (src/platform/pf_*_linux.c). Not part of the pf_* interface; code outside src/platform never
 * includes this header. Needs _GNU_SOURCE (ucontext_t, REG_*). */
#ifndef REFUELED_PF_LINUX_INTERNAL_H
#define REFUELED_PF_LINUX_INTERNAL_H
#include <ucontext.h>
#include "pf_exception.h"

/* A PfContext on Linux: the ucontext_t a signal handler receives (pf_exception_linux.c builds one
 * on the handler's stack for the fault handlers; pf_thread_linux.c for pf_thread_sample). */
struct PfContext {
    ucontext_t *native;
};

#endif

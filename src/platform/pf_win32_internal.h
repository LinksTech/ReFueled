/* SPDX-License-Identifier: GPL-2.0-or-later */
/* refueled: declarations shared between the Windows files of the platform layer only
 * (src/platform/pf_*_win32.c). Not part of the pf_* interface; code outside src/platform never
 * includes this header. */
#ifndef REFUELED_PF_WIN32_INTERNAL_H
#define REFUELED_PF_WIN32_INTERNAL_H
#include "pf_base.h"

PF_EXTERN_C_BEGIN

/* pf_memory_win32.c, for the fault dispatcher (pf_exception_win32.c, K7), which asks it first for
 * every access violation. Splitting a mapped view (a guest mapping replaced or unmapped in part)
 * takes the whole view down for a moment and maps the remaining pieces again, where Linux's
 * MAP_FIXED is atomic; another thread touching those pieces in that window faults although the
 * memory is there. Returns 1 when `address` lies in a view being remapped right now (after waiting
 * for the remap to finish) or in the last remapped range and is now accessible for `access`
 * (a PfAccess value): the dispatcher then resumes the thread unchanged so it repeats the access.
 * Returns 0 otherwise. Lock-free and async-safe. */
int pf_win32_memory_transient_fault(uintptr_t address, int access);

PF_EXTERN_C_END
#endif

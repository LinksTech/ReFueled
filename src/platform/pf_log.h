/* SPDX-License-Identifier: GPL-2.0-or-later */
/* refueled: output that must work inside fault handlers. Ordinary logging stays printf/fprintf. */
#ifndef REFUELED_PF_LOG_H
#define REFUELED_PF_LOG_H
#include "pf_base.h"

PF_EXTERN_C_BEGIN

/* Writes `size` bytes to standard error without buffering or locks (write(2, ...) /
 * WriteFile(GetStdHandle(STD_ERROR_HANDLE))). Async-signal-safe. */
void pf_write_stderr(const char *data, size_t size);

/* pf_write_stderr of a NUL-terminated string. */
void pf_write_stderr_str(const char *text);

PF_EXTERN_C_END
#endif

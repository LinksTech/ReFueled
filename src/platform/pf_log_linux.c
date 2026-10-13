/* SPDX-License-Identifier: GPL-2.0-or-later */
/* refueled: Linux pf_log: unbuffered, lock-free output to standard error. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE /* before the first system header, as in the runtime sources */
#endif
#include "pf_log.h"
#include <errno.h>
#include <string.h>
#include <unistd.h>

void pf_write_stderr(const char *data, size_t size) {
    while (size > 0) {
        ssize_t n = write(2, data, size);
        if (n < 0) {
            if (errno == EINTR) continue;
            return;
        }
        data += n;
        size -= (size_t)n;
    }
}

void pf_write_stderr_str(const char *text) {
    if (text) pf_write_stderr(text, strlen(text));
}

/* SPDX-License-Identifier: GPL-2.0-or-later */
/* refueled: Windows process initialisation and platform name. */
#include <fcntl.h>
#include <stdlib.h>
#include "pf_base.h"
#include "pf_tls.h"

int pf_init(void) {
    _fmode = _O_BINARY;
    /* refueled: K7 adds the fault dispatcher and timeBeginPeriod(1). */
    return pf_tls_init();
}

const char *pf_platform_name(void) { return "win32"; }

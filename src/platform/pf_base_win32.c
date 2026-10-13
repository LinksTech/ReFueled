/* SPDX-License-Identifier: GPL-2.0-or-later */
/* refueled: Windows process initialisation and platform name. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <fcntl.h>
#include <stdlib.h>
#include "pf_base.h"
#include "pf_tls.h"
#include "pf_win32_internal.h"
#include "pf_posix_gap_win32.h"

/* No modal CRT or system dialogs (failed assert, abort, crash box, "no disk"): they block
 * unattended runs and tests until someone clicks. The messages go to stderr instead. A constructor,
 * so it also covers tests that never call pf_init; build-windows.sh links this object into every
 * program with -Wl,-u,pf_win32_quiet_crt. */
__attribute__((constructor)) void pf_win32_quiet_crt(void) {
    _set_error_mode(_OUT_TO_STDERR);
    _set_abort_behavior(0, _CALL_REPORTFAULT);
    SetErrorMode(SetErrorMode(0) | SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
}

int pf_init(void) {
    _fmode = _O_BINARY;
    pf_gap_init_crt(); /* refueled F6: invalid CRT arguments (close(-1), ...) return EBADF instead of ending the process */
    int result = pf_win32_exception_init();
    if (!result) result = pf_tls_init();
    /* 1 ms timer resolution for sleeps and waits (winmm, loaded here so no import library is needed). */
    HMODULE winmm = LoadLibraryW(L"winmm.dll");
    if (winmm) {
        typedef UINT(WINAPI * TimeBeginPeriod)(UINT);
        TimeBeginPeriod begin = (TimeBeginPeriod)(void (*)(void))GetProcAddress(winmm, "timeBeginPeriod");
        if (begin) begin(1);
    }
    return result;
}

const char *pf_platform_name(void) { return "win32"; }

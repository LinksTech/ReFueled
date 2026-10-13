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

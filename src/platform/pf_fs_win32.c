/* SPDX-License-Identifier: GPL-2.0-or-later */
/* refueled: stubs, R2 replaces them. */
#include "pf_fs.h"

int pf_fs_remove_tree(const char *path) {
    (void)path;
    return pf_not_implemented(__func__);
}

int pf_fs_rename_replace(const char *from, const char *to) {
    (void)from;
    (void)to;
    return pf_not_implemented(__func__);
}

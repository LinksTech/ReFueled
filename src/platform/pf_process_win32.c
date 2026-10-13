/* SPDX-License-Identifier: GPL-2.0-or-later */
/* refueled: stubs, R3 replaces them. */
#include "pf_process.h"

uint32_t pf_process_id(void) {
    pf_not_implemented(__func__);
    return 0;
}

uintptr_t pf_executable_base(void) {
    pf_not_implemented(__func__);
    return 0;
}

int pf_symbolize(uintptr_t address, PfSymbol *out) {
    (void)address;
    (void)out;
    return pf_not_implemented(__func__);
}

int pf_backtrace(void **frames, int capacity) {
    (void)frames;
    (void)capacity;
    return pf_not_implemented(__func__);
}

int pf_describe_mapping(uintptr_t address, char *line, size_t size) {
    (void)address;
    (void)line;
    (void)size;
    return pf_not_implemented(__func__);
}

int pf_process_memory(uint64_t *resident_bytes, uint64_t *virtual_bytes) {
    (void)resident_bytes;
    (void)virtual_bytes;
    return pf_not_implemented(__func__);
}

int pf_gpu_memory_in_use(uint64_t *bytes) {
    (void)bytes;
    return pf_not_implemented(__func__);
}

int pf_restart_through_launcher(void) { return pf_not_implemented(__func__); }

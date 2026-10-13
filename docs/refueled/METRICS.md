# ReFueled – Metrik

Werkzeug: `docs/refueled/tools/linux_hits.py` (aus `sf_inventory.py` der Bestandsaufnahme
abgeleitet). Aufruf:

```
python docs/refueled/tools/linux_hits.py --ref C:/nitro-ps4-ref/shadPS4-b4e7ae7 [--list] [--json out.json]
```

**Kennzahl (Ziel 0):** Codezeilen mit Linux-only-Host-APIs außerhalb von `src/platform/`.
Gezählt wird in `src/`, `gpu/shim/`, `gpu/bbgpu.h`, `gpu/tools/` und in `gpu/shadps4/` **nur in
Zeilen, die sich vom Upstream-Stand b4e7ae7 unterscheiden** (bbport-Änderungen und von bbport
neu angelegte Dateien). Kommentare und Strings werden vor der Suche ausgeblendet. Nur die
Kategorie `procfs` sucht in Strings.

Nur zur Information: dieselbe Zählung **inklusive der POSIX-Teilmenge** (pthread, dirent,
clock_gettime/nanosleep). Diese Zeilen bleiben bei Toolchain A im gemeinsamen Code und sind
kein Ziel.

Kategorien Linux-only: mmap (inkl. mprotect/munmap/PROT_/MAP_), memfd, fallocate, mremap,
arch_prctl, signals, ucontext, userfaultfd, futex, syscall, thread-id (gettid, tgkill,
sched_*affinity, SCHED_IDLE, setpriority, prctl), pthread-np, getdents, ftw, dl, misc-linux
(process_vm_readv, close_range, execlp, backtrace, mallopt), dma-buf, procfs.

## Verlauf

| Datum | Stand (Commit) | Kennzahl Linux-only | davon eigener Code | davon bbport-Zeilen in gpu/shadps4 | inkl. POSIX (Info) | Plattform-#if außerhalb src/platform | Bemerkung |
|---|---|---|---|---|---|---|---|
| 2026-10-12 | f1f7935 (Baseline) | **335** | 283 | 52 | 696 | 23 | Ausgangswert vor jeder Änderung |
| 2026-10-13 | 1fa93bc (Welle 1) | **335** | 283 | 52 | 696 | 23 | Plattformschicht beider Plattformen fertig (K1, K2a, K4, K5a, K8, LH, R2/R3-Plattformteil); Aufrufer noch nicht umgestellt (braucht den Linux-Gesamtbuild in Nix) |
| 2026-10-13 | Welle A (ohne CLANG64) | **27** | 13 | 14 | 358 | – | Aufrufer umgestellt (K2, K3, K5, R1, R2b, R4, R5a, S1, S2, S3, V1, V2, V3). Rest gewollt: userfaultfd (D13, unter `__linux__`), dma-buf-Vulkan-Enums (D12, übersetzen unter Windows, laufen nie), fdinfo-Statistik unter `__linux__`, ftruncate/condattr_setclock über die Gap-Schicht (F6), malloc.h-Fehlalarm |
| 2026-10-13 | 13f1187 IF-10 (Rückweg D20) | **27** | 13 | 14 | 358 | – | unverändert: der Rückweg liegt ganz in src/platform (pf_exception_linux.c, pf_thread_linux.c) |
| 2026-10-13 | 3150109 IF-11 | **27** | 13 | 14 | 358 | – | unverändert (nur Konstanten in pf_memory.h) |
| 2026-10-13 | 170eddc Welle B (erster CLANG64-Build, exe gelinkt) | **27** | 13 | 14 | 358 | – | unverändert; Fixes nur Windows-Zweige, Gap, Build-Flags |

## Baseline nach Kategorie (eigener Code / bbport-Zeilen)

| Kategorie | eigener Code | bbport-Zeilen |
|---|---|---|
| mmap | 70 | 0 |
| memfd | 4 | 0 |
| fallocate | 4 | 0 |
| arch_prctl | 2 | 0 |
| signals | 35 | 4 |
| ucontext | 83 | 6 |
| userfaultfd | 0 | 7 |
| syscall | 12 | 0 |
| thread-id | 11 | 5 |
| pthread-np | 11 | 1 |
| getdents | 1 | 0 |
| ftw | 4 | 0 |
| dl | 15 | 11 |
| misc-linux | 19 | 9 |
| dma-buf | 8 | 5 |
| procfs | 4 | 4 |
| *pthread (POSIX)* | *303* | *4* |
| *posix-dir* | *20* | *4* |
| *posix-time* | *29* | *1* |

Nebenwerte (nicht in der Kennzahl): `tests/` + `tools/` 94 Zeilen (44 Linux-only, 50 POSIX).
Die Tests bekommen eigene Pakete (T1, F6).

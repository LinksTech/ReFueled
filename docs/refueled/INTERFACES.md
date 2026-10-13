# ReFueled – Schnittstellen (Vertrag)

Stand 2026-10-13, Version **IF-6** (eingefroren seit IF-2, Paket F1). Die Header unter `src/platform/` sind die
verbindliche Fassung. Dieses Dokument erklärt sie, ordnet jede Funktion den heutigen
Linux-Fundstellen zu und protokolliert Änderungen (§6). **Ändern darf sie nur Opus.**

## 1. Überblick

| Header | Bereich | Linux-Implementierung kommt aus | Windows-Kern |
|---|---|---|---|
| `pf_base.h` | Fehlercodes, `pf_init`, Stub-Helfer | probe.c:337–338 (mallopt) | Binärmodus, VEH, TLS-Slot |
| `pf_memory.h` | Layout, anonyme/feste Mappings, Backing-Objekt, Gastbereiche, Low-Heap, sicheres Lesen | runtime_memory.c, probe.c:88–113 | Platzhalter, Section, MapViewOfFile3 |
| `pf_tls.h` | Gast-Thread-Pointer | runtime_thread.c:60–62 | TlsAlloc-Slot + Patch |
| `pf_jmp.h` | Sprünge über Gastframes | runtime_thread.c:224/285, runtime.h:10 | eigenes Asm-setjmp |
| `pf_exception.h` | Fault-Dispatcher, Register-Kontext, Rücksprung, Watchdog | probe.c:141–201/392–401, shim SIGTRAP | VEH, CONTEXT |
| `pf_thread.h` | Gast-Stacks, Namen, IDs, Priorität, Sampling, Thread-Liste | runtime_thread.c:216–250, probe.c:133–137/203–247, runtime_kernel.c:451–570, bbport_threads.h | Stackwechsel mit TEB-Grenzen, Suspend/GetThreadContext |
| `pf_fs.h` | Baum löschen, atomar ersetzen | runtime_savedata.c:17/116/226–227 | FindFirstFile-Rekursion, MoveFileExW |
| `pf_process.h` | PID, Exe-Basis, Symbole, Backtrace, Mapping-Zeile, Speicher, GPU-Speicher, Neustart | probe.c:154–201/316–330, shim/vendored dladdr/backtrace, driver.cpp /proc | GetModuleHandleEx, RtlCaptureStackBackTrace, VirtualQuery, CreateProcess |
| `pf_log.h` | signal-sichere Ausgabe | `write(2, …)` in probe.c | WriteFile(STD_ERROR) |

## 2. Erlaubte POSIX-Teilmenge im gemeinsamen Code

Diese Aufrufe bleiben unverändert. Auf Windows liefern sie MinGW-w64 (UCRT) und winpthreads,
Lücken schließt `pf_posix_gap_win32.h`. **Alles andere POSIX-/Linux-Spezifische läuft über `pf_*`.**
F6 prüft jeden Punkt mit einem Unittest auf Windows. Punkte mit (P) sind bis dahin offen.

- **pthread:** `pthread_create/join/detach/self/equal/exit/once`,
  `pthread_attr_init/destroy/setstacksize/setdetachstate/setschedparam` (Wirkung auf Windows ggf.
  keine), `pthread_mutex_*` inkl. `trylock`, `timedlock` (P), `pthread_mutexattr_init/settype/destroy`,
  `pthread_cond_*` inkl. `timedwait`, `pthread_condattr_init/destroy`,
  `pthread_condattr_setclock(CLOCK_MONOTONIC)` (P), `pthread_rwlock_*` inkl. `timedrdlock/timedwrlock` (P),
  `pthread_key_create/delete`, `pthread_get/setspecific`, `PTHREAD_*_INITIALIZER`.
  **Nicht erlaubt:** `pthread_*_np`, `pthread_attr_setstack`, `pthread_sigmask`, `pthread_getcpuclockid`
  → `pf_thread.h`.
- **Zeit:** `clock_gettime(CLOCK_MONOTONIC | CLOCK_REALTIME)`, `nanosleep`, `usleep`, `sched_yield`, `time`.
- **Verzeichnisse:** `opendir/readdir/closedir`, `struct dirent::d_name`. `d_type` (P): Gibt es das bei
  MinGW nicht, liefert die Gap-Schicht einen Ersatz über `stat`.
- **Dateien:** `open` (Binärmodus erzwingt die Gap-Schicht), `read/write/close/lseek/fstat/stat/access/unlink/rmdir`,
  `ftruncate`, stdio. Gap: `pread/pwrite`, `fsync`, `realpath`, `mkdir(path, mode)`, `rename` (ersetzt
  ein bestehendes Ziel wie POSIX).
- **Typen:** `ssize_t`, `off_t` (64 Bit erzwingen: `_FILE_OFFSET_BITS=64` auf Windows), `pid_t`.
- **Sonstiges:** `getpid`, `strcasecmp/strncasecmp/strnlen/strdup`, `aligned_alloc` **nicht** (fehlt in UCRT)
  → `pf_low_aligned_alloc` oder `posix_memalign` aus der Gap-Schicht.

## 3. Funktionen je Header

### 3.1 `pf_base.h`
| Funktion | Ersetzt | Windows |
|---|---|---|
| `pf_init` | probe.c:337–338 `mallopt` | `_fmode = _O_BINARY`, `pf_tls_init`, VEH registrieren, `timeBeginPeriod(1)` |
| `pf_platform_name` | – | `"win32"` |
| `pf_not_implemented` | – | einmalige Meldung je Funktion, gibt `PF_ENOSYS` zurück |

### 3.2 `pf_memory.h`
| Funktion | Ersetzt (Linux-Fundstelle) | Windows-Mechanismus |
|---|---|---|
| `pf_page_size` | probe.c:391 `sysconf` | `GetSystemInfo().dwPageSize` |
| `pf_vm_reserve_layout` | – (neu, Linux no-op) | `VirtualAlloc2(MEM_RESERVE\|MEM_RESERVE_PLACEHOLDER)` |
| `pf_vm_alloc_fixed` | runtime_memory.c:679 (`MAP_FIXED_NOREPLACE`), bbport_gnm_hooks.cpp:58 | Platzhalter splitten + `VirtualAlloc2(MEM_REPLACE_PLACEHOLDER)`; außerhalb des Layouts `VirtualAlloc(addr)` |
| `pf_vm_alloc` | probe.c:97 | `VirtualAlloc(NULL, MEM_RESERVE\|MEM_COMMIT)` |
| `pf_vm_alloc_lazy` | runtime_memory.c:771 (`MAP_NORESERVE`) | `VirtualAlloc(MEM_RESERVE)` + Commit beim ersten Fault oder sofort `MEM_COMMIT` (Bitmap 32 MiB) |
| `pf_vm_free` | `munmap` dieser Bereiche | `VirtualFree` bzw. Rückgabe an Platzhalter |
| `pf_vm_protect` | probe.c:112, runtime_memory.c:550/940, shim mprotect (clobber, free_check, guest_hooks, gnm_hooks) | `VirtualProtect` (+ `FlushInstructionCache` bei EXEC) |
| `pf_backing_create` | runtime_memory.c:136–140 (memfd_create, ftruncate, Sicht) | `CreateFileMapping2(PAGE_EXECUTE_READWRITE, SEC_COMMIT)` + `MapViewOfFile3` (Sicht) |
| `pf_backing_view` | `backing_base` | Sicht-Adresse |
| `pf_backing_map_fixed` | runtime_memory.c:181/190/206 | Region-Tabelle, Platzhalter split, `MapViewOfFile3(MEM_REPLACE_PLACEHOLDER)` |
| `pf_backing_zero` | runtime_memory.c:223/231/237/275 (`fallocate PUNCH_HOLE`) | `memset` über die Sicht |
| `pf_handle_map_fixed` | runtime_memory.c:106/171/189/205 (dma-buf) | `PF_ENOSYS` (D12) |
| `pf_handle_map` | bbport_guest_memory.cpp:190/191/203 | `PF_ENOSYS` / NULL |
| `pf_handle_close` | `close(fd)` der Chunks | no-op |
| `pf_guest_reserve` | runtime_memory.c:355 | Sicht/Region lösen, Platzhalter behalten |
| `pf_guest_unmap` | runtime_memory.c:443 | `UnmapViewOfFile2(MEM_PRESERVE_PLACEHOLDER)` / `VirtualFree(MEM_RELEASE\|MEM_PRESERVE_PLACEHOLDER)`, `MEM_COALESCE_PLACEHOLDERS` |
| `pf_low_heap_init` / `pf_low_calloc` / `pf_low_aligned_alloc` / `pf_low_free` | runtime_thread.c:66/81/132 (calloc/aligned_alloc), Audit F9 | einfacher Allokator auf einem low-Teilbereich, mit Mutex |
| `pf_read_memory_safe` | probe.c:223, runtime_kernel.c:520, page_manager.cpp:113/123, liverpool.cpp:1054/2343/2396 (`process_vm_readv`) | `ReadProcessMemory(GetCurrentProcess())` |

Vorgaben für die Windows-Implementierung (K1): Region-Tabelle mit Split/Coalesce wie shadPS4
`AddressSpace::Impl` (Herkunft vermerken). Thread-sicher. `pf_backing_map_fixed` muss Teile einer
bestehenden Sicht ersetzen können (Aliasing, Teil-Unmap).

### 3.3 `pf_tls.h`
| Funktion | Ersetzt | Windows |
|---|---|---|
| `pf_tls_init` | – | `TlsAlloc` (< 64, sonst `PF_ENOSPC`), aus `pf_init` |
| `pf_tls_set_guest_base` | runtime_thread.c:61 `arch_prctl(ARCH_SET_GS)` | `TlsSetValue(slot, tcb)` |
| `pf_tls_get_guest_base` | – | `TlsGetValue(slot)` |
| `pf_tls_patch_image` | – (Linux: zählt nur) | `65 48 8b 04 25 00000000` → `65 48 8b 04 25 <0x1480+8*slot>` |

### 3.4 `pf_jmp.h`
Makros `pf_setjmp/pf_longjmp` (Thread-Ende, war `setjmp/longjmp`) und `pf_sigsetjmp/pf_siglongjmp`
(Rücksprungpunkte, war `sigsetjmp(b,0)/siglongjmp`). Typen `pf_jmp_buf`, `pf_sigjmp_buf`.
Windows: `pf_jmp_win32.S` sichert rbx, rbp, rdi, rsi, rsp, r12–r15, rip, mxcsr, x87-CW, xmm6–xmm15.
`runtime_fault_recover` wird `__thread pf_sigjmp_buf *` (runtime.h, runtime_memory.c:708; Nutzer:
vk_pipeline_cache.cpp:1086, vk_rasterizer.cpp:1198).

### 3.5 `pf_exception.h`
| Funktion | Ersetzt | Windows |
|---|---|---|
| `pf_context_get/set/native` | `ucontext_t`/`REG_*`-Zugriffe (83 Zeilen in probe.c, runtime_kernel.c, shim, page_manager) | `CONTEXT` |
| `pf_fault_add_handler` | `sigaction` SIGSEGV/SIGBUS/SIGILL (probe.c:392–394), SIGTRAP (bbport_guest_hooks.cpp:179, bbport_free_check.cpp:838) | ein VEH für alle Arten |
| `pf_fault_resume_at` | probe.c:145–153 (`pthread_sigmask` + `siglongjmp`) | CONTEXT aus `pf_sigjmp_buf` laden, `EXCEPTION_CONTINUE_EXECUTION` |
| `pf_exit_now` | `_exit` in Handlern | `TerminateProcess(GetCurrentProcess())` |
| `pf_watchdog_start` | probe.c:399–401 (`alarm` + SIGALRM) | Timer-Thread |

Reihenfolge der Handler (D8), registriert durch: (1) `bbgpu_handle_fault`, eingetragen von probe.c
vor `bbgpu_init`, (2) Rücksprung `runtime_fault_recover` (probe.c), (3) Diagnose-Hooks (shim,
nur wenn aktiviert), (4) fataler Bericht (probe.c, zuletzt). Auf Linux bleiben die
sigaction-Flags je Signal wie bisher (SIGTRAP mit `SA_NODEFER`). Der Dispatcher ruft bei
"nicht behandelt" die vorherige Aktion bzw. die Standardaktion auf, wie heute die Kette in den Shims.

### 3.6 `pf_thread.h`
| Funktion | Ersetzt | Windows |
|---|---|---|
| `pf_thread_create_on_stack` | runtime_thread.c:239–250 (`pthread_attr_setstack` + `pthread_create`) | winpthreads-Thread + `pf_stack_win32.S` Stackwechsel, TEB-Grenzen |
| `pf_call_guest_on_stack` | probe.c:133–137/605–607 (`enter_on_stack`) | Asm-Aufruf SysV, TEB-Grenzen |
| `pf_thread_get_stack` | runtime_kernel.c:451 (`pthread_getattr_np`) | `GetCurrentThreadStackLimits` |
| `pf_thread_id` | `gettid`/`SYS_gettid` (9 Stellen) | `GetCurrentThreadId` |
| `pf_thread_set_name` / `get_name` | `pthread_setname_np`/`getname_np` (8 Stellen) | `SetThreadDescription`/`GetThreadDescription` |
| `pf_thread_set_idle_priority` | bbport_threads.h:28–30 | `SetThreadPriority(THREAD_PRIORITY_IDLE)` |
| `pf_cpu_count_allowed` | bbport_threads.h:18–20 | `GetProcessAffinityMask` |
| `pf_thread_cpu_time_ns` | liverpool.cpp:123 | `GetThreadTimes` |
| `pf_threads_list` | probe.c:234–244, runtime_kernel.c:540–545, vk_scheduler.cpp:41 (`/proc/self/task`) | `CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD)` |
| `pf_thread_sample` | probe.c:228/242 (SIGUSR2), runtime_kernel.c:505–570 (SIGPROF) | `SuspendThread` + `GetThreadContext` + `ResumeThread` |

### 3.7 `pf_fs.h`, `pf_process.h`, `pf_log.h`
| Funktion | Ersetzt | Windows |
|---|---|---|
| `pf_fs_remove_tree` | runtime_savedata.c:116/226/227 (`nftw`) | rekursiv `FindFirstFileW`/`DeleteFileW`/`RemoveDirectoryW` |
| `pf_fs_rename_replace` | `rename` wo ein Ziel ersetzt wird (F6/R2 klärt die Stellen) | `MoveFileExW(REPLACE_EXISTING\|WRITE_THROUGH)` |
| `pf_process_id` | `getpid` in Diagnose | `GetCurrentProcessId` |
| `pf_executable_base` | probe.c:395–396 (`dladdr(main)`) | `GetModuleHandleW(NULL)` |
| `pf_symbolize` | `dladdr` (probe.c, runtime_kernel.c:583, page_manager.cpp:83, vk_rasterizer.cpp:1333, vk_scheduler.cpp:301/336) | `GetModuleHandleExW(FROM_ADDRESS)` + Modulname; Symbol optional (DbgHelp) |
| `pf_backtrace` | probe.c:191, vk_scheduler.cpp:66 | `RtlCaptureStackBackTrace` |
| `pf_describe_mapping` | probe.c:176–187 (`/proc/self/maps`) | `VirtualQuery` + `GetMappedFileNameW` |
| `pf_process_memory` | driver.cpp:335 (`/proc/self/statm`) | `GetProcessMemoryInfo` |
| `pf_gpu_memory_in_use` | driver.cpp:307–332 (`/proc/self/fdinfo`) | `PF_ENOSYS` (später `VK_EXT_memory_budget`) |
| `pf_restart_through_launcher` | probe.c:324–328 (`close_range` + `execlp`) | `CreateProcessW` ohne Handle-Vererbung + `ExitProcess` |
| `pf_write_stderr(_str)` | `write(2, …)` in Handlern | `WriteFile(GetStdHandle(STD_ERROR_HANDLE))` |

## 4. Geänderte interne Schnittstellen (Runtime ↔ GPU-Bibliothek)

| Schnittstelle | Heute | Neu | Paket |
|---|---|---|---|
| `bbgpu_handle_fault` (gpu/bbgpu.h:24) | `int (void *ucontext, void *address)` | `int (PfContext *context, void *address)` | S2 (Shim), K3 (Aufrufer) |
| `bbgpu_dump_guest_writes` | `void (void *ucontext)` | `void (PfContext *context)` | S2, K3 |
| `BbClobber::OnFault`, `BbFreeCheck::OnTrapFault` | `void *ucontext` | `PfContext *` | S1, S2 |
| `runtime_fault_recover` (runtime.h:10) | `__thread sigjmp_buf *` | `__thread pf_sigjmp_buf *` | K2 (Definition), K3 (Deklaration), V2 (Nutzer) |
| `runtime_low_map(size, prot)` (runtime.h:97) | `prot` = PROT_* | `prot` = PF_PROT_* | K2, K3, K5 |
| `runtime_memory_set_guest_chunk_allocator` | `int (*)(phys, size)` → fd | Rückgabe `PfMemHandle` (Linux weiterhin das fd) | K2, V3 |
| Linux-Header in `runtime.h` | `<setjmp.h>` für sigjmp_buf | `"platform/pf_jmp.h"` | K3 |

`runtime.h` und `gpu/bbgpu.h` gehören Opus. Pakete melden ihren Bedarf, Opus führt ihn aus.

## 5. Regeln für Stubs und Implementierungen

- Eine Windows-Stub-Funktion: `return pf_not_implemented(__func__);` bzw. `NULL`/`0` mit demselben
  Aufruf davor. Kein stilles Erfolg-Melden.
- Linux-Implementierungen sind **verschobener** Code: gleiche Systemaufrufe, gleiche Flags, gleiche
  Reihenfolge, gleiche Meldungstexte. Der Review vergleicht Zeile für Zeile mit dem alten Ort.
- Jede Implementierung bringt Unittests in `tests/refueled/test_<bereich>.c` mit. Sie laufen auf
  beiden Plattformen (Linux in WSL, Windows im MSYS2-Build).

## 6. Änderungsprotokoll

| Version | Datum | Änderung | Grund |
|---|---|---|---|
| IF-1 | 2026-10-12 | Erstfassung: pf_base, pf_memory, pf_tls, pf_jmp, pf_exception, pf_thread, pf_fs, pf_process, pf_log | Phase 1 |
| IF-2 | 2026-10-13 | Eingefroren nach Review (F1), inhaltlich gleich IF-1. Prüfung: jeder Header zweimal eingebunden, `-std=c11 -Wall -Wextra -Werror -pedantic` und `-std=c++23 -Wall -Wextra -Werror`, Linux- und `_WIN32`-Zweig, gcc 13.3 (WSL) grün. Wiederholung mit clang (CLANG64) in F3. Der Low-Heap-Allokator ist ab jetzt ein eigenes Paket (LH), die Funktionen in `pf_memory.h` bleiben gleich. | Freigabe Phase 2 |
| IF-3 | 2026-10-13 | `pf_fs_remove_tree`: bricht beim ersten Fehler ab (Kommentar sagte "walk continues"). | Linux-Verhalten von runtime_savedata.c (Callback gibt remove() zurück) muss gleich bleiben; im R2-Review gefunden |
| IF-4 | 2026-10-13 | Attribut-Makros `PF_NORETURN`, `PF_RETURNS_TWICE`, `PF_ALIGNED16` in pf_base.h statt `__attribute__` in pf_jmp.h/pf_exception.h; für gcc/clang textgleich expandiert | Win32-Dateien (K7) mit MSVC als echtem Zwischenlauf prüfbar, solange CLANG64 fehlt |
| IF-5 | 2026-10-13 | §4 umgesetzt: `runtime.h` bindet pf_jmp.h/pf_memory.h ein, `runtime_fault_recover` ist `pf_sigjmp_buf *` und nicht mehr Linux-only, `runtime_restart` ebenso; `runtime_low_map` nimmt PF_PROT_*; `runtime_memory_set_guest_chunk_allocator` mit Rückgabe `PfMemHandle` jetzt in runtime.h deklariert; `bbgpu.h`: `bbgpu_handle_fault(PfContext *, void *)`, `bbgpu_dump_guest_writes(PfContext *)` (PfContext vorwärts deklariert). Linux: gleiche Typen/Werte (sigjmp_buf, PROT_*-Werte 1/2/4, fd als intptr_t), Aufrufer passen K2/K3/K5/S2/V2/V3 an | Aufrufer-Umstellungen ohne Linux-Build (Planänderung 2026-10-13) |
| IF-6 | 2026-10-13 | `runtime_memory_init_layout()` (runtime.h): Layout-Reservierung + Low-Heap (oberste 512 MiB des low-Bereichs, [0xF_E000_0000, 0x10_0000_0000)); Implementierung K2, Aufruf in main direkt nach pf_init (K3) | Startreihenfolge pf_init → Layout → Image bei 0x800000000 (U1 §4) an einer Stelle |

# U2 – Signale und Threads: Wofür, und die Windows-Entsprechung

Stand 2026-10-12, Basis f1f7935. Quellen: `src/probe.c`, `src/runtime_kernel.c`,
`src/runtime_thread.c`, `gpu/shim/*`, `gpu/shadps4/video_core/*` (bbport-Zeilen), Referenz
shadPS4 b4e7ae7 (`core/signals.cpp`, `core/tls.cpp`, `core/cpu_patches.cpp`, `core/thread.cpp`).

## 1. Signale

| Signal | Wo | Wofür | Im Normalbetrieb aktiv? | Windows-Entsprechung |
|---|---|---|---|---|
| SIGSEGV, SIGBUS | `probe.c:141–201, 392–394` | (1) GPU-Seitenverfolgung: schreibgeschützte Gastseiten → `bbgpu_handle_fault` → `Core::Signals` → `page_manager` hebt den Schutz auf und merkt die Seite; (2) Rücksprung spekulativer Lesezugriffe (`runtime_fault_recover`, `siglongjmp`; Draw-Vorbereitung in `vk_pipeline_cache.cpp:1086`, `vk_rasterizer.cpp:1198`); (3) sonst fataler Bericht (dladdr, `/proc/self/maps`, backtrace) und `_exit(128+sig)` | **ja, ständig** (1) und gelegentlich (2) | Erster **Vectored Exception Handler** für `EXCEPTION_ACCESS_VIOLATION` / `EXCEPTION_IN_PAGE_ERROR`. Zugriffsart aus `ExceptionInformation[0]` (0 lesen, 1 schreiben, 8 ausführen), Adresse aus `[1]`. Rücksprung: Register aus eigenem `pf_sigjmp_buf` in den CONTEXT laden und `EXCEPTION_CONTINUE_EXECUTION` (kein `longjmp` aus dem VEH). |
| SIGILL | `probe.c:394` | fataler Bericht (z. B. CPU ohne AVX/BMI1 trotz Prüfung) | nein | VEH `EXCEPTION_ILLEGAL_INSTRUCTION`, `EXCEPTION_PRIV_INSTRUCTION` |
| SIGUSR2 + `tgkill` | `probe.c:228, 242, 397–398` | Watchdog-Thread-Dump: An jeden Thread aus `/proc/self/task` (gelesen per `getdents64`) geht SIGUSR2, sein Handler schreibt RIP und die rbp-Kette (gelesen per `process_vm_readv`) | nur bei Watchdog-Timeout | Vom Watchdog-Thread aus: Threads per Toolhelp32 aufzählen, je Thread `SuspendThread` + `GetThreadContext`, Stack per `ReadProcessMemory` lesen, `ResumeThread` (`pf_thread_sample`) |
| SIGALRM | `probe.c:229–247, 399–401` | Watchdog: `alarm(timeout)`, danach Dump und `_exit(142)`. probe.c-Standard 10 s; run.sh/Launcher übergeben `--timeout 0` = aus | nur Tests/Diagnose | Timer-Thread oder Threadpool-Timer (`pf_watchdog_start`) |
| SIGPROF + `tgkill` | `runtime_kernel.c:505–570` | Sampling-Profiler `BB_SAMPLE_THREAD=<name>` (mit Frame-Stats): Thread über `/proc/self/task/*/comm` finden, periodisch SIGPROF senden, Handler liest RIP und Stack | nein (Opt-in-Diagnose) | `pf_thread_sample` (Suspend/GetThreadContext) aus dem Sampler-Thread |
| SIGTRAP (int3) | `gpu/shim/bbport_guest_hooks.cpp:68–179` | int3-Hooks im Spielcode: GPU-Allokator meldet neue Bereiche, Ressourcen-memcpy meldet Schreibzugriffe. Der Handler emuliert die ersetzte Instruktion über ucontext (RIP, RSP, RAX…) | **nur PC-Modell** (`BB_GUEST_IN_PLACE=1`, Standard aus, nur AMD) | VEH `EXCEPTION_BREAKPOINT`. Achtung: Windows meldet RIP **auf** dem int3, Linux **dahinter**. `pf_exception` vereinheitlicht auf die Linux-Konvention. |
| SIGTRAP (int3 + Trap-Flag) | `gpu/shim/bbport_free_check.cpp:756–840` | Diagnose für Issue #2 (`BB_LABEL_TRAP`): Haltepunkte in free/GC des Spiels, Einzelschritt über EFLAGS.TF | nein (Diagnose) | VEH `EXCEPTION_BREAKPOINT` + `EXCEPTION_SINGLE_STEP`, TF über `CONTEXT.EFlags` |

**Thread-Interna ohne Signal**, aber Linux-only:

| Mechanismus | Wo | Wofür | Windows |
|---|---|---|---|
| `arch_prctl(ARCH_SET_GS)` | `runtime_thread.c:61` | GS-Basis = TCB des Gast-Threads; die Gast-Loads `gs:[0]` lesen den Selbstzeiger | GS ist der TEB und nicht verschiebbar → TLS-Slot < 64 und Umschreiben der Loads auf `gs:[0x1480+8*slot]` (TEB.TlsSlots), wie shadPS4 `cpu_patches.cpp:106–116`. Die 9-Byte-Instruktion bleibt gleich lang (nur die Verschiebung ändert sich). |
| `pthread_attr_setstack` | `runtime_thread.c:245` | Gast-Stacks unter 1 TiB (der Gast packt Stack-Adressen) | winpthreads unterstützt keine fremden Stacks → Thread startet auf Host-Stack, wechselt per Assembler auf den Gast-Stack und setzt TEB StackBase/StackLimit/DeallocationStack (wie Fibers). Sonst brechen `__chkstk` und die Ausnahmebehandlung. |
| `enter_on_stack` (asm) | `probe.c:133–137, 605–607` | Gast-Hauptthread auf 8-MiB-Stack unter 1 TiB | `pf_call_guest_on_stack`, dazu TEB-Grenzen umsetzen. Die alte Asm-Routine nimmt SysV-Argumente (rdi, rsi, rdx, rcx), ein Windows-Aufrufer übergibt aber rcx, rdx, r8, r9 → muss `sysv_abi` oder neu sein. |
| `setjmp/longjmp` über Gastframes | `runtime_thread.c:224, 285` | `scePthreadExit` springt aus dem Gastcode zurück in `host_start` | MinGW-`longjmp` **entrollt per SEH** → Absturz bei Gastframes ohne Unwind-Info. Eigenes registerbasiertes `pf_setjmp/pf_longjmp` (Asm). |
| `gettid` / `SYS_gettid` | 9 Stellen (runtime_kernel, runtime_thread, shim) | Thread-IDs in Logs, `sceKernelGetThreadId` | `GetCurrentThreadId` |
| `pthread_get/setname_np` | probe, runtime_thread, shim | Namen für Logs/Dumps | `SetThreadDescription` / `GetThreadDescription` (winpthreads hat `pthread_setname_np`, die Länge ist zu prüfen) |
| `sched_getaffinity`, `sched_setscheduler(SCHED_IDLE)`, `setpriority` | `gpu/shim/bbport_threads.h:18–30` | Zahl der Worker-Threads, Leerlauf-Priorität für Hintergrund-Threads | `GetProcessAffinityMask`, `SetThreadPriority(THREAD_PRIORITY_IDLE)` |
| `pthread_getattr_np` | `runtime_kernel.c:451` | Stackgrenzen für den Wait-Site-Scan | `GetCurrentThreadStackLimits` |
| `pthread_getcpuclockid` | `liverpool.cpp:123` (bbport) | CPU-Zeit des GPU-Threads | `GetThreadTimes` |
| `close_range` + `execlp("bash","run.sh")` | `probe.c:316–330` | Neustart nach Auflösungswechsel im Menü | `CreateProcess` (ohne vererbte Handles) + `ExitProcess` |

## 2. Wichtigstes Risiko: Die Red Zone des Gastcodes

Gastcode ist für die SysV-ABI kompiliert und darf 128 Byte unter RSP ("Red Zone") ohne
Stack-Anpassung nutzen. Linux legt Signal-Frames **unterhalb** der Red Zone ab. Windows kennt
keine Red Zone: Bei einer Ausnahme legt der Kernel den Ausnahme-Frame (CONTEXT,
EXCEPTION_RECORD) **direkt unter RSP** ab und überschreibt dabei die Red Zone des unterbrochenen
Gastcodes.

Relevanz: Die GPU-Seitenverfolgung erzeugt **im Normalbetrieb** laufend Schreib-Faults im
Gastcode. Trifft so ein Fault eine Blattfunktion, die gerade Werte in der Red Zone hält, sind diese
nach der Rückkehr kaputt.

Referenz: shadPS4 hat dafür `WindowsGuestRedZoneProtection` (statisches Umschreiben der
Red-Zone-Instruktionen, `cpu_patches.cpp:925ff`), **standardmäßig aus**. Die meisten Spiele laufen
offenbar ohne. Plan: zunächst ohne. Ein Paket (RZ) bleibt vorbereitet, Auslöser sind unerklärliche
Gast-Abstürze oder Datenfehler nach Faults. Diagnose: Fault-Zähler je Gast-RIP und Prüfung, ob die
Funktion RSP-relativ mit negativem Offset zugreift.

## 3. Sampling statt Signal: Unterschiede

- **Linux:** Der Handler läuft *auf* dem Ziel-Thread (Signal). **Windows:** Die Callback läuft auf
  dem *aufrufenden* Thread, während das Ziel angehalten ist. Für die Callback gilt deshalb auf
  beiden Plattformen: nicht allokieren, keine Locks nehmen (der angehaltene Thread kann sie
  halten), Fremdspeicher nur über `pf_read_memory_safe` lesen.
- `SuspendThread` auf einen Thread, der gerade selbst im VEH steckt, ist erlaubt. Der Dump zeigt
  dann den Handler-Kontext.

## 4. Folgen für den Plan

- **Kritischer Pfad:** Fault-Dispatcher (VEH) mit Kontext-Abstraktion und Rücksprung (für die
  GPU-Seitenverfolgung ab M2 zwingend), Gast-TLS (Slot + Patch), Threads auf Gast-Stacks mit
  TEB-Grenzen, `pf_setjmp/pf_longjmp`.
- **Nicht kritisch (Diagnose/Opt-in):** Watchdog, Sampler, Thread-Dump, int3-Hooks (PC-Modell),
  Free-Check/Label-Trap. Diese Pakete laufen parallel. Windows darf dort vorerst
  `PF_ENOSYS` liefern, ohne M1–M4 zu gefährden (Ausnahme: Watchdog-Dump hilft beim Debuggen von
  M2, Priorität mittel).

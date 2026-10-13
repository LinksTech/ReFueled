# Paket-Log K5a (Sonnet): pf_thread + pf_tls Linux

Verhaltensabweichung Linux: CPU-Zahl-Rückfall sysconf statt hardware_concurrency; unlesbares comm in der Thread-Liste als "?" (D21 Nr. 8); SIGRTMIN+2-Abtastung durch IF-10 entfernt

## Dateien
`src/platform/pf_thread_linux.c`, `src/platform/pf_tls_linux.c`, `tests/refueled/test_thread.c`
(Linux-Teil; Windows-Teil schreibt K6 in dieselbe Datei, Plattformhilfen in `#ifdef _WIN32`).
Die Aufrufer (runtime_thread.c, runtime_kernel.c, probe.c, Shims) sind nicht umgestellt.

## Alt <-> neu
| Funktion | Alter Ort | Anmerkung |
|---|---|---|
| pf_tls_set_guest_base | runtime_thread.c:61 `syscall(SYS_arch_prctl, ARCH_SET_GS, base)` | gleiche Syscall; Fehler als `-errno` zurueck, `perror`+`exit(21)` bleibt beim Aufrufer |
| pf_tls_get_guest_base | NEU (vorher nicht vorhanden) | `ARCH_GET_GS` |
| pf_tls_init | neu | 0 |
| pf_tls_patch_image | neu (Linux: nur zaehlen) | zaehlt `65 48 8b 04 25 00 00 00 00`, aendert nichts |
| pf_thread_create_on_stack | runtime_thread.c:239-250 | attr_init, setstack (stack != NULL) bzw. setstacksize (stack NULL, size != 0), pthread_create, attr_destroy; Rueckgabe = pthread-Fehlernummer. `pthread_detach` (Z. 251) und STOP-Meldung bleiben beim Aufrufer |
| pf_call_guest_on_stack | probe.c:133-137 `enter_on_stack` | gleiche Instruktionen (push rbp / rbp=rsp / rcx&-16 -> rsp / rdi->rax, rsi->rdi, rdx->rsi / call rax / zurueck); `stack_low` auf Linux ungenutzt; Asm-Symbol `pf_call_on_stack_asm` (hidden) |
| pf_thread_get_stack | runtime_kernel.c:451 | pthread_getattr_np + pthread_attr_getstack |
| pf_thread_id | `syscall(SYS_gettid)` | gleich |
| pf_thread_set_name | runtime_thread.c:213-219 set_host_name | 15 Bytes, gleiche Kuerzung |
| pf_thread_get_name | pthread_getname_np (runtime_kernel.c:471 u.a.) | ueber 16-Byte-Puffer, dann auf `size-1` gekuerzt |
| pf_thread_set_idle_priority | gpu/shim/bbport_threads.h:26-31 | `sched_setscheduler(0, SCHED_IDLE)`, Rueckfall `setpriority(PRIO_PROCESS, gettid, 19)`; beide fehlgeschlagen -> PF_EPERM (vorher stumm ignoriert) |
| pf_cpu_count_allowed | bbport_threads.h:15-22 | sched_getaffinity + CPU_COUNT, Rueckfall `sysconf(_SC_NPROCESSORS_ONLN)` (vorher `hardware_concurrency`), mind. 1 |
| pf_thread_cpu_time_ns | liverpool.cpp:123 | pthread_getcpuclockid(pthread_self) + clock_gettime |
| pf_threads_list | probe.c:234-244, runtime_kernel.c:540-545, vk_scheduler.cpp:41 | getdents64 auf /proc/self/task (wie probe.c, ohne stdio), comm je Task per open/read, Zeilenende abgeschnitten, nicht lesbar -> "?" |
| pf_thread_sample | probe.c:228/242 (SIGUSR2), runtime_kernel.c:555 (SIGPROF) | siehe unten |

## pf_thread_sample: Signal und Ablauf
- Signal: `SIGRTMIN+2` per `tgkill(getpid(), tid, ...)`, Handler `SA_SIGINFO|SA_RESTART`.
- Keine Kollision: SIGUSR2 (probe.c thread_dump) und SIGPROF (runtime_kernel.c sample_handler)
  behalten ihre Handler, bis K3/K5 sie auf pf_thread_sample umstellen; im Code von src/, gpu/shim
  und gpu/shadps4 kommt kein Echtzeit-Signal vor (grep SIGRTMIN/SIGUSR1/SIGUSR2/SIGPROF/SIGALRM);
  glibc/NPTL nutzt SIGCANCEL/SIGSETXID unterhalb von SIGRTMIN, `SIGRTMIN` ist die Funktion, die
  das schon beruecksichtigt. Echtzeit-Signale werden eingereiht, nicht zusammengelegt.
- Ablauf: Mutex um den einen Auftrags-Slot; Handler (installiert beim ersten Aufruf) baut
  `PfContext {native = ucontext}` auf dem Zielthread, ruft `sample(context, user)`, dann `sem_post`.
  Aufrufer `sem_timedwait` 2 s. Wer den Auftrag bearbeitet, entscheidet ein atomares `claimed`:
  bei Zeitueberschreitung gewinnt der Aufrufer (PF_ETIMEDOUT), ein spaeter Handler kehrt sofort
  zurueck ohne `sample`; laeuft `sample` schon, wartet der Aufrufer ohne Grenze darauf.
- Restrisiko: Trifft ein verspaetetes Signal eines abgelaufenen Auftrags erst beim naechsten
  Auftrag ein, wuerde es dessen Sample auf dem alten Zielthread ausfuehren (falscher Thread).
  Nur nach Timeout (Ziel blockiert Signale), fuer Diagnose hingenommen.
- tid nicht vorhanden -> PF_ENOENT; Selbst-Sampling funktioniert (Signal wird synchron zugestellt).

## Tests (WSL gcc 13, `-std=c11 -D_GNU_SOURCE -g -Wall -Wextra -Werror -pthread -Isrc -Isrc/platform`, plus pf_thread_linux.c, pf_tls_linux.c, pf_common.c und Wegwerf-Helfer unter C:\CTR Reload\tmp\refueled\K5a\helper.c)
```
== -O2                                                     test_thread: 41 checks passed  exit=0
== -O0                                                     test_thread: 41 checks passed  exit=0
== -O1 -fsanitize=undefined -fno-sanitize-recover=undefined test_thread: 41 checks passed  exit=0
-O2, 10 Wiederholungen: 10x "test_thread: 41 checks passed"
```
- ASan (`-O1 -fsanitize=address`): 3 von 41 Pruefungen rot, alle drei "lokale Variable liegt im
  Thread-Stack" (Zeilen mit `frame.local_inside` und `&local` in main). Ursache: ASan legt
  Locals auf einen "fake stack" ausserhalb des echten Stacks. Kein Fehler im Code; ASan fuer
  dieses Paket nicht verlangt. Sonst keine ASan-Meldung (kein Konflikt mit arch_prctl/eigenem Stack).
- Abgedeckt: Thread auf 1-MiB-mmap-Stack mit 100-KiB-Rahmen (Summe 332 erwartet), Stack NULL +
  size, call_guest_on_stack (Rueckgabe 7009, rsp im Stack), get_stack (Haupt- und Fremdthread),
  Namen (Kuerzung auf 15), pf_thread_id je Thread verschieden, threads_list findet den Thread samt
  Namen, TLS-Roundtrip in 3 Threads, cpu_count, cpu_time steigt, Sampling eines drehenden Threads
  (RIP != 0, RSP im Stack des Ziels, Callback lief auf dem Zielthread, zweimal hintereinander,
  unbekannte tid -> PF_ENOENT), patch_image zaehlt 2 Folgen und aendert nichts, idle priority.

## Offen
- Nicht getestet: PF_ETIMEDOUT-Pfad (Ziel blockiert das Signal).
- pf_thread_set_idle_priority im Test zuletzt (senkt den Hauptthread).
- Windows-Teil von test_thread.c: K6.

## Nachtrag 2026-10-13 (IF-10)
Der SIGRTMIN+2-Mechanismus von `pf_thread_sample` ist entfernt. Linux nutzt wieder SIGPROF ohne Warten (alter Sampler), der Thread-Dump wieder SIGUSR2 (`pf_dump_signal_install` / `pf_dump_other_threads`). Siehe log/IF-10.md.

## Prüfung Opus 2026-10-13 (Pflichtfeld)
Feld bei der einmaligen Durchsicht aller Logs gesetzt und gegen die alt↔neu-Tabelle geprüft.

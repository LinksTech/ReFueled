# B-RT: clang-Atomics, Mutex-Wand, test_lowheap unter Windows

Verhaltensabweichung Linux: keine (siehe Tabelle: gleiche __atomic-Builtins mit gleicher Speicherordnung, gleiche
Funktionen; in runtime_mutex.c entfaellt nur der `_WIN32`-Zweig, der unter Linux nie uebersetzt wurde)

Ausgefuehrt von Sonnet, zu pruefen von Opus. Nicht committet.

## alt <-> neu

| alt (Datei:Zeile, Code) | neu (Datei:Zeile, Code) | Linux-Gleichheit |
|---|---|---|
| runtime_kernel.c:315 `static _Atomic unsigned module_info_misses;` | `static unsigned module_info_misses; /* accessed only through __atomic builtins */` | Alle Zugriffe (Z. 318, 323) sind `__atomic_fetch_add(&x,1,__ATOMIC_RELAXED)`; gcc erzeugt fuer diese Builtins auf `unsigned` und `_Atomic unsigned` denselben Code (lock xadd), Speicherordnung unveraendert relaxed. Statisch null-initialisiert wie vorher. |
| runtime_kernel.c:335 `static _Atomic unsigned segment_info_calls;` | wie oben | einziger Zugriff Z. 338, `__atomic_fetch_add(...,__ATOMIC_RELAXED)` |
| runtime_thread.c:331 `static _Atomic unsigned cancels;` | wie oben | einziger Zugriff Z. 334, relaxed fetch_add |
| runtime_json2.c:18 `static _Atomic unsigned notified;` | wie oben | einziger Zugriff Z. 21, relaxed fetch_add |
| runtime_mutex.c:7 `#ifndef _WIN32` | entfernt | Linux-Zweig war unter Linux aktiv und ist jetzt fuer alle Plattformen gueltig; Text unveraendert |
| runtime_mutex.c:260-282 `#else` ... Windows-Stubs (posix_cond_*, runtime_mutex_resolve, runtime_mutex_report "Windows mutex backend not implemented") `#endif` | entfernt | Der Block gehoerte zum `_WIN32`-Zweig und war unter Linux nie Teil der Uebersetzung; der Linux-Zweig (der dort bereits dieselben posix_cond_*/resolve/report-Funktionen vor dem `#else` besitzt) bleibt unveraendert. Die geloeschten Zeilen sind exakt Z. 260-282 plus Z. 7. |
| tests/refueled/test_lowheap.c:9 `#include <sys/mman.h>` | nur `#ifndef _WIN32` | Testcode; Linux-Pfad make_area/free_area (mmap) und test_linux_wrappers unveraendert, nur eingeklammert |

Warum nicht stdatomic: `atomic_fetch_add_explicit` waere ebenfalls moeglich, aber die Entfernung von `_Atomic` aendert
an den Zugriffsstellen nichts (keine Zeile ausser den 4 Deklarationen) und es bleibt bei den im Projekt sonst genutzten
`__atomic`-Builtins. Die uebrigen `_Atomic uint64_t` in runtime_kernel.c (Z. 110, 296, 433, 500-518) wurden von clang
nicht beanstandet und bleiben unveraendert.

test_lowheap.c unter Windows: `pf_vm_reserve_layout(0x800000000, 0x800000000)` einmal, dann erster freier 1-GiB-Slot per
`pf_vm_alloc_fixed`; Freigabe `pf_vm_free`. `test_linux_wrappers` (pf_low_*) nur `#ifndef _WIN32`.

## Fehlerzahl (Zaehl-Build, clang 23)

| Gruppe | vorher | nachher |
|---|---|---|
| R1 (runtime_kernel/thread/json2, __atomic) | 5 Stellen | 0 |
| K5 (runtime_mutex.c, Wand) | Wand/Typfehler (Auftrag), Einheit scheiterte | 0, uebersetzt ohne Fehler |
| LH (test_lowheap.c) | 14 | 0 |
| R5 (runtime.c rand_s, nicht meine Datei) | 1 | 1 |
| F6 (test_posix_subset.c, gmtime_r/localtime_r) | 6 | 6 |
| R4 (runtime_pad.c strcasestr) | 1 | 1 |
| K6 (test_thread, Laufzeit: 5 von 45 Checks, Z. 274-283 TLS-Gastbasis) | 0 Compile / Lauf rot | gleich |

Gesamt nachher: 8 Fehler in 4 Gruppen (Auftrag nennt nur vorher fuer meine Gruppen; Gesamtstand davor 22+ siehe summary.md
des Laufs vor den Aenderungen).

## Testergebnis (wörtlich)

`bash build-windows.sh --keep-going --no-gpu --test`:
```
PASS test_exception
PASS test_fs
PASS test_lowheap
PASS test_memory
FAIL test_posix_subset
PASS test_process
PASS test_stubs
PASS test_sysv_abi
FAIL test_thread
PASS test_time
PASS test_tls
tests: 11 run, 2 failed
8 errors in 4 groups
```
`test_lowheap: 124 checks passed`. runtime_kernel.c, runtime_thread.c, runtime_json2.c, runtime_mutex.c kompilieren
ohne Fehler. Gap-Schicht (pf_posix_gap_win32.h) deckt pthread_condattr_setclock und pthread_mutex_timedlock ab; es fehlte nichts.
Gelinkt wurde runtime_mutex.c nicht (Zaehl-Build kompiliert nur Objekte).

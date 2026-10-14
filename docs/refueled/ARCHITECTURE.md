# ReFueled – Architektur des Windows-Ports

Stand 2026-10-13 (Phase 2 freigegeben, Freigaben in §4 D1, D10, D17; Planänderung D19: kein WSL/Linux lokal; D20 aufgehoben: Linux-Thread-Dump wieder per SIGUSR2; D21: bekannte Linux-Abweichungen). Basis: Branch `refueled/windows` ab f1f7935
(= Tag v0.2.1 + 7 Zeilen STATUS.md). Lizenz GPL-2.0-or-later wie SacredFire.

Zugehörig: [U1-SPEICHER.md](U1-SPEICHER.md), [U2-SIGNALE-THREADS.md](U2-SIGNALE-THREADS.md),
[INTERFACES.md](INTERFACES.md), [TASKS.md](TASKS.md), [METRICS.md](METRICS.md),
[TOOLCHAIN.md](TOOLCHAIN.md).

## 1. Ziel und Randbedingungen

- SacredFire läuft nativ unter Windows x64: Gastcode nativ, C-Runtime für die PS4-Bibliotheken,
  GPU-Bibliothek (eingebettete shadPS4-Kopie) auf Vulkan.
- **Der Linux-Build bleibt verhaltensgleich.** Linux-Code wird nur verschoben bzw. hinter
  Funktionen gelegt, nicht geändert.
- **Mergebar mit Upstream:** möglichst wenige geänderte Zeilen in gemeinsam genutztem Code, keine
  `#ifdef`-Wüste, Änderungen in `gpu/shadps4` minimal und mit `refueled:` markiert.
- Keine Spieldateien in Repo, Logs oder Commits.

## 2. Schichten

```
 Gastcode (eboot.bin, libc.prx, Fios2, NpToolkit2)        ── SysV-ABI, ruft Importe direkt
   │  Importe (NID → Host-Funktion, ABI = sysv_abi)
 ┌─┴──────────────────────────────────────────────────────────────────────────────┐
 │ src/  Runtime (probe.c Loader, runtime_*.c)          gpu/  GPU-Bibliothek        │
 │       gemeinsamer Code: C11 + POSIX-Teilmenge             shim/ + shadps4/ (C++23) │
 └─┬──────────────────────────────────────────────────────────┬─────────────────────┘
   │ pf_*  (src/platform/pf_*.h)                               │ pf_* (extern "C")
 ┌─┴──────────────────────────────┐  ┌─────────────────────────┴──────────────────┐
 │ pf_*_linux.c (verschobener Code)│  │ pf_*_win32.c (+ pf_posix_gap_win32.h)      │
 └─┬──────────────────────────────┘  └─┬──────────────────────────────────────────┘
   glibc, Linux-Syscalls                 MinGW-w64 UCRT, winpthreads, Win32/NT-API
```

Regeln:
1. Gemeinsamer Code (`src/` außerhalb `src/platform/`, `gpu/shim`, bbport-Zeilen in
   `gpu/shadps4`) nutzt nur C/C++-Standard, die **POSIX-Teilmenge** (INTERFACES.md §2) und `pf_*`.
2. Linux-only- oder Windows-only-APIs gibt es nur in `src/platform/pf_*_<plattform>.c`.
3. Die Plattformwahl passiert **im Build** (welche `pf_*_*.c` kompiliert werden), nicht per
   `#ifdef` im gemeinsamen Code. Ausnahme: `pf_jmp.h` (Typdefinition, muss ein Header sein).
4. Schnittstellen ändert nur Opus. Jede Änderung wird in INTERFACES.md §5 protokolliert.

## 3. Ordner und Dateien

```
src/platform/
  platform.h              Sammel-Header
  pf_base.h  pf_memory.h  pf_tls.h  pf_jmp.h  pf_exception.h  pf_thread.h  pf_fs.h  pf_process.h  pf_log.h
  pf_common.c             plattformneutral (pf_not_implemented)
  pf_<bereich>_linux.c    Linux-Implementierung = verschobener Bestandscode
  pf_<bereich>_win32.c    Windows-Implementierung (am Anfang Stubs → PF_ENOSYS)
  pf_jmp_win32.S          registerbasiertes setjmp/longjmp (Windows)
  pf_stack_win32.S        Stackwechsel mit TEB-Grenzen, Gast-Aufruf (Windows)
  pf_posix_gap_win32.h/.c POSIX-Lücken der MinGW-Umgebung (nur Windows, per -include)
  WIN32_SOURCES.txt       welche gemeinsamen Quellen der Windows-Build schon übersetzt
build-windows.sh          Windows-Build (MSYS2 CLANG64-Bash), analog build.sh
tests/refueled/           Plattform-Unittests (laufen auf beiden Plattformen)
docs/refueled/            Planung, Berichte, Audits, Metrik
docs/refueled/tools/      linux_hits.py (Metrik), linux-platform-tests.sh (CI)
.github/workflows/        linux-build.yml (Linux-Regression in CI, D19)
```

## 4. Entscheidungen

### D1 Toolchain: MSYS2 CLANG64 (Option A), Begründung mit Zahlen

**Freigegeben 2026-10-13 (Projektleitung).** Abbruchkriterium für die shadPS4-Kopie: Kompiliert G2/G3
nach zwei Fix-Runden unter MinGW nicht **oder** stürzt die GPU-Bibliothek beim ersten
Vulkan-Init ab, wechselt **nur die GPU-Bibliothek** auf clang-cl (Rückfallposition unten).
Opus entscheidet und meldet die Begründung an die Projektleitung.

Basis: Metrik `docs/refueled/tools/linux_hits.py` (siehe METRICS.md), Umfang eigener Code +
bbport-Zeilen in `gpu/shadps4`.

| Kriterium | A: MSYS2 CLANG64 (clang + lld + libc++, UCRT, winpthreads) | B: clang-cl, MSVC-ABI |
|---|---|---|
| Stellen im gemeinsamen Code, die angefasst werden müssen | **335** Linux-only-Zeilen. Die 361 POSIX-Zeilen (pthread 307, dirent 24, clock/nanosleep 30) bleiben, weil winpthreads/MinGW sie liefern. Lücken (mkdir mit 2 Argumenten 5×, pread/pwrite 3×, fsync 4×, realpath 2×, rename 5×) schließt ein Force-Include-Header **ohne** Aufrufänderung. | **696** + POSIX-Typen und -Header (ssize_t/off_t/pid_t 37×, unistd/strings/sys-Header 13×, strcasecmp & Co. 7×) ≈ **750**, außer man baut eine eigene pthread/dirent/unistd-Schicht. Das wäre ein Nachbau von winpthreads. |
| Vulkan / SDL3 / glslang / übrige GPU-Abhängigkeiten | alles als MSYS2-Pakete per pacman. `build.sh` nutzt schon pkg-config + CMake, das gleiche Schema geht in MSYS2 | Vulkan SDK vorhanden (MSVC-Libs); SDL3 als offizielles VC-Paket; fmt, boost, robin-map, xxhash, zydis, ffmpeg, miniz, magic_enum, VMA über vcpkg (nicht installiert); eigener Build-Weg ohne pkg-config |
| Risiko für die eingebettete shadPS4-Kopie | mittel: upstream ohne CI, aber von der Community gepflegt; MinGW-Fixes #4365 und #4402 (Mai 2026) sind in b4e7ae7 enthalten. Windows-Zweige im vendored Code: 58 Zeilen in 17 Dateien, davon 9 `_MSC_VER` | gering: upstream baut Windows mit clang-cl in CI |
| Debugbarkeit | gut: `-gcodeview` + lld `--pdb` → WinDbg/Visual Studio; dazu gdb/lldb aus MSYS2 | sehr gut: PDB nativ, Visual Studio, WinDbg |
| Linux-Build | unberührt; gleiche Compiler-Familie, gleiche Flags-Syntax | unberührt; aber zweite Flag-Welt (cl-Syntax) |
| Mergebarkeit mit Upstream | **hoch**: POSIX-Aufrufe bleiben unverändert | geringer: ≈ 360 zusätzliche Aufrufstellen ändern sich (oder Nachbau-Schicht) |

**Empfehlung A.** Sie halbiert die zu ändernden Stellen (335 statt ≈ 750), lässt die
Thread-/Sync-Aufrufe der Runtime unverändert (gut für Upstream-Merges) und nutzt das bestehende
pkg-config/CMake-Schema. Das Restrisiko (MinGW-Fixes in `gpu/shadps4`) ist begrenzt und landet in
eigenen Paketen (G2/G3) mit `refueled:`-Markern. **Rückfallposition:** Wird die
shadPS4-Kopie unter MinGW zu teuer, kann *nur die GPU-Bibliothek* auf clang-cl wechseln. Das
setzt die C-Schnittstelle `bbgpu.h` voraus (gilt schon: `extern "C"`, keine C++-Typen über die
Grenze), dann als DLL mit expliziten Exporten.

### D2 Plattformschicht als C-API `pf_*` mit Dateipaaren je Plattform
Begründung: Die Runtime ist C11, die Shims sind C++ – eine C-Schnittstelle bedient beide. Je
Bereich ein Header und je Plattform eine Datei. Der Linux-Teil ist der bisherige Code, 1:1
verschoben (überprüfbar per Diff), damit bleibt das Linux-Verhalten gleich.

### D3 POSIX-Teilmenge bleibt im gemeinsamen Code
pthread (ohne `_np` und ohne `attr_setstack`), dirent, `clock_gettime`/`nanosleep`/`usleep`, stdio,
`open/read/write/close/lseek/fstat/stat/unlink/rmdir/access`. Auf Windows liefern das MinGW-w64
und winpthreads. Abweichungen schließt `pf_posix_gap_win32.h` (nur Windows, per `-include`):
Binärmodus, `pread/pwrite`, `fsync`, `realpath`, `mkdir(path, mode)`, `rename` über bestehende
Ziele. Die genaue Liste und ihre Prüfung macht Paket F6 (Unittest
`tests/refueled/test_posix_subset.c`).

### D4 Gast-ABI: `ABI` bleibt `__attribute__((sysv_abi))`, keine Thunks
Clang unterstützt das Attribut auf Windows-Zielen. Die Runtime hat keine variadischen
ABI-Funktionen, und alle Funktionszeiger-Typen auf Gastcode sind schon mit `ABI` markiert
(probe.c:590/600, runtime_kernel.c:246/292, runtime_thread.c:26, runtime.h:15). Paket F7 prüft
das per Test: Argumente in Registern und auf dem Stack, Float/Double in XMM, Struct-Rückgabe
(sret), Rückrufe Host→Gast, Stack-Ausrichtung, xmm6–15 (unter Win64 callee-saved, unter SysV
nicht), Varargs in den PS4_SYSV_ABI-Funktionen der GPU-Bibliothek. Dazu ein Audit aller
Host→Gast-Aufrufe ohne `ABI` (unter Linux unsichtbar, unter Windows falsch).

### D5 Gleiches Adresslayout auf beiden Plattformen
low `[0x8_0000_0000, 0x10_0000_0000)`, user `[0x10_0000_0000, 0xfc_0000_0000)`.
Windows reserviert beide beim Start als Platzhalter (`pf_vm_reserve_layout`). Erste
low-Anforderung = Image = 0x800000000 (U1 §1). Konstanten bleiben in `runtime_memory.c`.

### D6 Gast-TLS: Linux GS-Basis (unverändert); Windows TEB-TlsSlots + Patch beim Laden
Das Boot-File bleibt plattformgleich (die Skripte erzeugen weiter die GS-Form). Windows schreibt
nach dem Relozieren die Verschiebung der 9-Byte-Loads auf `0x1480 + 8*slot` um
(`pf_tls_patch_image`). Gleiche Länge, kein Trampolin.

### D7 Gast-Stacks auf Windows: Stackwechsel mit TEB-Grenzen
Host-Thread startet normal (winpthreads) und wechselt in Assembler auf den Gast-Stack im
low-Bereich. Für die Dauer setzt er `NT_TIB.StackBase/StackLimit` und `TEB.DeallocationStack`
(wie es `SwitchToFiber` tut). So akzeptieren `__chkstk` und die Ausnahmebehandlung den Stack.
Kein `NtCreateThreadEx`.

### D8 Ein Fault-Dispatcher je Prozess, Handler-Kette in fester Reihenfolge
Linux: sigaction wie heute (gleiche Flags je Signal). Windows: erster VEH. Reihenfolge wie in
`probe.c:fault`: GPU-Seitenverfolgung → Rücksprung spekulativer Lesezugriffe → Diagnose-Hooks
→ fataler Bericht. Breakpoint-RIP wird auf die Linux-Konvention (hinter int3) vereinheitlicht.

### D9 Sprünge über Gastframes nur registerbasiert
`pf_setjmp/pf_longjmp` und `pf_sigsetjmp/pf_siglongjmp`. Linux: glibc wie bisher. Windows:
eigene Asm-Routinen. Das MinGW-`longjmp` entrollt per SEH und scheitert an Gastframes ohne
Unwind-Info.

### D10 Gast-sichtbare Host-Objekte unter 1 TiB
Linux: unverändert (brk-Heap + mallopt, `-no-pie`). Windows: Low-Heap im low-Bereich
(`pf_low_*`) und `-Wl,--disable-high-entropy-va`. Audit F9 findet alle betroffenen Allokationen.
**Freigabe 2026-10-13:** Der Low-Heap ist ein eigenes Paket (LH), K6 hängt davon ab. Der
Allokator selbst ist plattformneutral (`src/platform/pf_lowheap.c`, arbeitet auf einem
übergebenen Adressbereich) und wird so auch auf Linux getestet; Windows legt den Bereich mit
`pf_vm_alloc_fixed` im low-Bereich an. Linux leitet `pf_low_*` weiter an calloc/aligned_alloc/free.

### D11 GPU-Bibliothek auf Windows statisch gelinkt
Unter Linux löst `libbbgpu.so` Runtime-Symbole aus der Exe auf (`--allow-shlib-undefined`,
`-rdynamic`). Das geht mit einer Windows-DLL nicht ohne Import-Lib der Exe. Darum wird sie
unter Windows eine statische Bibliothek in `sacredfire.exe`. Linux bleibt bei der `.so`. Der
CMake-Zweig ist an `WIN32` gebunden (F5).

### D12 PC-Speichermodell (dma-buf, `BB_GUEST_IN_PLACE`) auf Windows aus
Es ist ohnehin Standard-aus und nur für AMD. Windows meldet "nicht unterstützt", es gilt der
vorhandene Rückfallweg. Eine spätere Umsetzung über `VK_EXT_external_memory_host` ist möglich.

### D13 userfaultfd bleibt Linux-only (Opt-in)
Windows nutzt den Seitenschutz-Weg (VirtualProtect über `pf_vm_protect`).

### D14 Änderungen in `gpu/shadps4` minimal, markiert mit `refueled:`
Möglichst über Helfer im Shim (`gpu/shim`), nicht im vendored Code. Jede Zeile bekommt einen
`refueled:`-Kommentar. Vom eingebetteten Upstream-Code übernommene Windows-Teile werden mit
Herkunft gekennzeichnet (shadPS4-Commit).

### D15 Linux-Regression in WSL2 – **ersetzt durch D19 (2026-10-13)**
Ubuntu 24.04 ist vorhanden, aber ohne cmake, pkg-config, Vulkan-Header und SDL3. Vorschlag:
Nix (single-user) in WSL. `build.sh` springt dann selbst in `nix-shell shell.nix`, das ist die
Upstream-Umgebung. Alternative: apt-Pakete + SDL3 aus Quellen.
**Freigegeben 2026-10-13: Nix aus `shell.nix`.** Installation macht die Projektleitung (TOOLCHAIN.md §3).

### D19 Kein Linux auf dem Entwicklungsrechner; Linux-Prüfung nur in CI (Planänderung 2026-10-13)
Auf diesem Rechner wird kein Linux benutzt (keine WSL-Distribution, kein Nix, keine VM). Einzige
Build-Umgebung ist Windows mit MSYS2 CLANG64, dazu MSVC aus den VS Build Tools für
Zwischenläufe reiner Win32-Dateien. Ziel ist ausschließlich der native Windows-Port (.exe).
Linux-Code bleibt erhalten (`pf_*_linux.c`, `build.sh`), wird lokal aber nicht gebaut oder
getestet. Die Regel "Linux-Verhalten nicht ändern, nur verschieben/abstrahieren" gilt weiter und
wird so abgesichert: (a) jedes Paket liefert eine alt↔neu-Tabelle für jede Linux-wirksame Stelle,
Opus prüft die Gleichheit beim Merge und vermerkt das im Paket-Log; (b)
`.github/workflows/linux-build.yml` (Ubuntu-Runner, `build.sh --test`, Python-Tests,
Plattformtests) ist vorbereitet und wird aktiv, sobald das GitHub-Repo existiert; (c)
Linux-Prüfpakete haben in TASKS.md den Status `deferred-ci` und blockieren nichts.

### D20 Linux-Thread-Dump per Abtastung statt SIGUSR2 – **aufgehoben 2026-10-13 (IF-10)**
**War:** Seit K3 (1675a75) installierte der Loader keinen SIGUSR2-Handler mehr; der Watchdog las
jeden Thread über `pf_thread_sample` (SIGRTMIN+2, synchron) aus. Folge unter Linux: `kill -USR2`
beendete den Prozess, statt einen Dump zu drucken ("Profiling ohne perf" entfiel). Zusätzlich hatte
R1 (5f68dbc) den Opt-in-Sampler `BB_SAMPLE_THREAD` von SIGPROF ohne Warten auf denselben
synchronen SIGRTMIN+2-Weg gelegt (im Paket-Log R1 begründet, aber nicht als Entscheidung geführt).
**Jetzt (Freigabe Projektleitung 2026-10-13, Rückweg):** Linux wieder wie Upstream.
- `pf_dump_signal_install(dump, user)` (pf_exception.h): Linux installiert den alten
  SIGUSR2-Handler (SA_SIGINFO|SA_RESTART, leere Maske) in `main` vor dem Watchdog wie vorher
  (alt probe.c:397–398); der Dump läuft auf dem empfangenden Thread mit `gettid()`. Windows:
  speichert nur den Callback.
- `pf_dump_other_threads()` (pf_exception.h): Linux die alte Watchdog-Schleife wörtlich
  (`/proc/self/task` per getdents64, `tgkill(SIGUSR2)`, 20 ms je Thread, am Ende 100 ms; alt
  probe.c:233–246); Windows: jeder Thread über `pf_thread_sample`.
- `pf_thread_sample` unter Linux: SIGPROF, Handler SA_SIGINFO|SA_RESTART, ohne Warten (alt
  runtime_kernel.c:511–574). Unter Windows bleibt es synchron.
- **SIGRTMIN+2 entfällt vollständig**: kein Code nutzt es mehr (Prüfung `grep SIGRTMIN` über src/,
  gpu/shim, tests: 0 Treffer).
Einzige verbleibende Unterschiede, beide ohne Wirkung nach außen: Der SIGPROF-Handler wird beim
ersten Abtastaufruf installiert statt in `sample_start` (vorher wird kein SIGPROF gesendet), und
der Dump liest Gastframes mit `pf_read_memory_safe` (unter Linux dasselbe `process_vm_readv`).
Windows ist unverändert (Watchdog-Thread, SuspendThread). Belege: docs/refueled/log/IF-10.md.

### D21 Bekannte Linux-Abweichungen (Durchsicht aller Paket-Logs 2026-10-13)
Ergebnis der einmaligen Durchsicht aller 35 Paket-Logs (Regel Projektleitung 2026-10-13). Keine weitere
Abweichung wirkt im Normalbetrieb; D20 ist aufgehoben. Punkte 3–10 von der Projektleitung abgenommen
(2026-10-13). Punkte 1 und 2 waren nur unter Windows nötig (unter Linux liegen C-Heap, `-no-pie`-Exe
und die Thread-TLS auf den Low-Stacks ohnehin unter 1 TiB, D10) und sind mit IF-11 auf Windows
beschränkt; Linux entspricht dort wieder dem Stand vor dem Port.

**Belegung des runtime_low_map-Bereichs (Prüfung 2026-10-13, aus Code und Doku):**
- Bereich [0x8_0000_0000, 0x10_0000_0000) = 32 GiB. Windows: davon 512 MiB Low-Heap (1,6 %),
  31,5 GiB für `runtime_low_map`; Linux: 32 GiB (IF-11).
- Nutzer (`runtime_low_map`, nie freigegeben, `low_next` wächst nur, je Abbildung 16 KiB Lücke):
  Image (probe.c `allocate`, Textsegment 0x50d96dc ≈ 81 MiB laut runtime_kernel.c, Grenze 512 MiB
  in probe.c), Import-Trampoline `(n+1)·32` B und Daten-Fallen `(n+1)·Seite` (730 Host-Importe laut
  docs/sacredfire/01-imports.md → einige MiB), Hauptstack 8 MiB, je Gast-Thread Stack
  (`attr.stack`, Standard 1 MiB, mindestens 64 KiB) + 256 KiB Rand + 16 KiB Lücke ≈ 1,27 MiB.
- Eine gemessene Spitzenbelegung des Spiels ist in Code und Doku nicht belegt (keine
  Thread-Zahl, kein "Mapped … bytes" für CTR). Abschätzung: Image ≤ 0,2 GiB + Fallen/Stack
  ≈ 0,03 GiB + Threads. Die 2×-Grenze (15,75 GiB auf Windows) wird erst bei rund 12 000
  Gast-Thread-Erzeugungen mit Standardstack über eine Sitzung erreicht, weil Stacks nie
  wiederverwendet werden (auch unter Linux, Verhalten vor dem Port). Messpunkt ab M1/M2:
  "Mapped … bytes" und "Runtime: guest threads created=…" im Log; liegt die Hochrechnung dann
  über 15,75 GiB, wird das sofort gemeldet.

| Nr. | Paket | Abweichung unter Linux | Wann sichtbar | Entscheidung |
|---|---|---|---|---|
| 1 | K2 | ~~Bereich für `runtime_low_map` endet 512 MiB früher~~ | – | **aufgehoben (IF-11)**: `PF_LOW_HEAP_SIZE` ist unter Linux 0, `LOW_MAP_END` = `USER_MIN` wie vorher; nur Windows reserviert 512 MiB |
| 2 | K5 | ~~Gast-errno-Zelle im TCB-Block~~ | – | **aufgehoben (IF-11)**: `PF_GUEST_ERRNO_IN_TCB` ist unter Linux 0, wieder `_Thread_local int32_t guest_errno` und TCB-Block ohne +16 Byte; nur Windows nutzt die Zelle im TCB-Block |
| 3 | R1b | `sysctl kern.arandom` füllt den Puffer vollständig (vorher ein einzelnes `getrandom`, kurze Lesung als Erfolg möglich); Fehler als EIO statt errno | nur bei durch Signal unterbrochenem `getrandom` > 256 Byte bzw. Fehler | beibehalten (Fehlerbehebung) |
| 4 | R1b | `localtime_r`-Fehlerpfad (ERR 22) entfällt; `getrusage`-Fehler liefert EIO statt uninitialisierter Werte | nur bei Fehlschlag dieser Aufrufe (praktisch nie) | beibehalten |
| 5 | R2c | `remove_stale_temps` steigt auch bei `d_type == DT_UNKNOWN` per stat in Unterordner ab (vorher nicht) | nur auf Dateisystemen ohne `d_type` | beibehalten (verhält sich wie upstream auf üblichen Dateisystemen) |
| 6 | K2a, S2 | `MAP_FIXED_NOREPLACE` auf Kerneln < 4.17: eine an anderer Adresse gelandete Abbildung wird freigegeben und als EEXIST gemeldet (vorher still angenommen) | nur Kernel < 4.17 | beibehalten (Fehlerbehebung) |
| 7 | K2 | `pool()` versucht nach einem fehlgeschlagenen Anlegen erneut (vorher "ok" mit NULL); `map_phys`-Meldung zeigt bei seltenen errno "Input/output error" | nur Fehlerpfade | beibehalten |
| 8 | K5a, S2 | CPU-Zahl-Rückfall `sysconf(_SC_NPROCESSORS_ONLN)` statt `hardware_concurrency()`; Thread-Liste meldet unlesbares `comm` als "?" statt den Eintrag zu überspringen | nur wenn `sched_getaffinity` bzw. das Lesen von `comm` scheitert | beibehalten |
| 9 | V1, V2, R1, S2 | Diagnosetexte: Symbol-/Modulnamen auf 127/259 Zeichen gekürzt; `vk_scheduler`-Backtrace selbst formatiert statt `backtrace_symbols_fd` (gleiches Muster, evtl. andere Symbolnamen); Wait-Trace bei `dli_fname == NULL` mit `%p` | nur in Diagnoseausgaben (BB_*-Schalter, Abstürze) | beibehalten |
| 10 | T2b, T2c | `mods.py`, `patches.py`, `gen_import_names.py` lesen/schreiben UTF-8 unabhängig von der Locale | nur mit Nicht-UTF-8-Locale (vorher Fehler bzw. andere Kodierung) | beibehalten |

Ohne beobachtbaren Unterschied (daher "keine"): Reihenfolge der zwei SIGTRAP-Handler (S1/K8, Fälle
disjunkt), `pause()` → Schlafschleife (S2), calloc statt malloc (R5a), rekursiver Mutex per
`pthread_once` (R4), `lstat` vor `nftw` (R2b), Installationszeitpunkt des SIGPROF-Handlers (IF-10).

### D16 Issue #2 (überschriebenes Dword) nicht jagen
`BB_ASYNC_PIPELINES` bleibt aus. Tritt `SacredFire issue #2:` im Windows-Log auf, wird es mit
Logauszug in `docs/refueled/ISSUE2-WINDOWS.md` notiert, ohne Port-Fehlersuche.

### D17 Red Zone: zunächst ohne Schutz, Paket RZ in Reserve
Siehe U2 §2. Auslöser für RZ: Gast-Abstürze oder Datenfehler, die mit Faults korrelieren.
**Freigabe 2026-10-13:** RZ ist eingeplant (nicht kritischer Pfad), fertig spätestens vor M3,
als Schalter `BB_REDZONE=0/1` (Umgebungsvariable, Standard bis zur Messung: 0). **Ab M2 wird
jeder unerklärliche Absturz zuerst mit RZ an/aus A/B getestet**, bevor andere Fehlersuche beginnt.

### D18 Sprache
Dokumente unter `docs/refueled/` deutsch, Code und Kommentare englisch (wie der übrige Code).

## 5. Arbeitsweise (für alle Agenten verbindlich)

- Jedes Paket hat eigenen Worktree `C:\ReFueled\wt\<ID>` auf eigenem lokalen Branch
  `refueled/pkg/<ID>` ab dem aktuellen `refueled/windows`.
- Ein Paket ändert **nur** seine besessenen Dateien (TASKS.md). Braucht es eine andere Datei
  oder eine Schnittstellenänderung, gibt es das Paket mit Begründung an Opus zurück.
- Sonnet committet nicht. Opus prüft den Diff, committet im Paket-Worktree und merged nach
  `refueled/windows`. Commit-Identität nur über Umgebungsvariablen:
  `GIT_AUTHOR_NAME=ReFueled GIT_AUTHOR_EMAIL=refueled@localhost GIT_COMMITTER_NAME=ReFueled GIT_COMMITTER_EMAIL=refueled@localhost`.
  Keine `git config`, kein Push, keine Remotes.
- Jedes Paket liefert kompilierenden Code, mindestens einen Test und einen Eintrag in
  `docs/refueled/log/<ID>.md` (was gemacht, was offen, Testbefehle mit Ergebnis).
- Jedes Paket-Log hat das Pflichtfeld `Verhaltensabweichung Linux: keine / <Beschreibung>`
  (AGENT-REGELN.md). Opus prüft es beim Merge ausdrücklich gegen die alt↔neu-Tabelle. Jeder Eintrag
  ungleich "keine" ist eine Entscheidung von Opus (beibehalten mit Begründung in D21 oder
  zurückbauen) und wird der Projektleitung im nächsten Bericht gemeldet, nie nur im Log. Beibehaltene
  Abweichungen stehen zusätzlich im Kopf von `.github/workflows/linux-build.yml`.
- Nach jedem Merge: `linux_hits.py` ausführen, Wert in METRICS.md eintragen.
- Temporäre Dateien nur unter `C:\ReFueled\tmp\` bzw. im Paket-Worktree unter `out/`.
- Fehler nicht wegimprovisieren: Ein festhängendes Paket meldet die Fehlermeldung wörtlich und
  geht an Opus zurück.

## 6. Risiken

| # | Risiko | Wirkung | Gegenmaßnahme |
|---|---|---|---|
| R1 | Red Zone wird von Windows-Ausnahmen überschrieben | sporadische Gastfehler ab M2 | D17, Paket RZ; Fault-Statistik je Gast-RIP |
| R2 | shadPS4-Kopie unter MinGW macht viele Fixes nötig | Verzug im GPU-Strang | G2/G3 früh starten; Rückfall: GPU-Lib mit clang-cl (D1) |
| R3 | Gast-sichtbare Host-Zeiger über 1 TiB | Abstürze bei Thread-Erzeugung | D10, Audit F9, Assertion im Low-Heap |
| R4 | winpthreads-Lücken (condattr_setclock MONOTONIC, timedlock, rwlock-Timeouts) | falsche Wartezeiten, Hänger | F6-Unittests vor M2; Gap-Implementierung |
| R5 | VirtualProtect-Kosten bei der GPU-Seitenverfolgung | FPS-Einbruch | ab M3 messen; Bündelung von Schutzwechseln |
| R6 | Commit-Charge der Section (6–17 GiB) | Start schlägt fehl bei kleiner Auslagerungsdatei | Fehlermeldung mit Hinweis; Option SEC_RESERVE |
| R7 | Vulkan-Treiberunterschiede (NVIDIA RTX 5080 statt RADV) | Grafikfehler M3+ | kein Port-Fehler; als Befund dokumentieren, eigene Pakete |
| R8 | lange Pfade (LongPathsEnabled = 0) | Build-/Dateifehler | kurze Pfade (C:\nitro-ps4-*), core.longpaths; Freischaltung optional |
| R9 | Opus als serieller Engpass | Leerlauf | Linux-Seite der Bereiche an Sonnet mit genauer Vorgabe; Opus macht nur die Windows-Kernteile |

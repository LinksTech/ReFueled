# ReFueled – Architektur des Windows-Ports

Stand 2026-10-13 (Phase 2 freigegeben, Freigaben in §4 D1, D10, D15, D17 eingetragen). Basis: Branch `refueled/windows` ab f1f7935
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
docs/refueled/tools/      linux_hits.py (Metrik), linux-regress.sh (WSL)
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

### D15 Linux-Regression in WSL2
Ubuntu 24.04 ist vorhanden, aber ohne cmake, pkg-config, Vulkan-Header und SDL3. Vorschlag:
Nix (single-user) in WSL. `build.sh` springt dann selbst in `nix-shell shell.nix`, das ist die
Upstream-Umgebung. Alternative: apt-Pakete + SDL3 aus Quellen.
**Freigegeben 2026-10-13: Nix aus `shell.nix`.** Installation macht die Projektleitung (TOOLCHAIN.md §3).

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

- Jedes Paket hat eigenen Worktree `C:\nitro-ps4-wt\<ID>` auf eigenem lokalen Branch
  `refueled/pkg/<ID>` ab dem aktuellen `refueled/windows`.
- Ein Paket ändert **nur** seine besessenen Dateien (TASKS.md). Braucht es eine andere Datei
  oder eine Schnittstellenänderung, gibt es das Paket mit Begründung an Opus zurück.
- Sonnet committet nicht. Opus prüft den Diff, committet im Paket-Worktree und merged nach
  `refueled/windows`. Commit-Identität nur über Umgebungsvariablen:
  `GIT_AUTHOR_NAME=ReFueled GIT_AUTHOR_EMAIL=refueled@localhost GIT_COMMITTER_NAME=ReFueled GIT_COMMITTER_EMAIL=refueled@localhost`.
  Keine `git config`, kein Push, keine Remotes.
- Jedes Paket liefert kompilierenden Code, mindestens einen Test und einen Eintrag in
  `docs/refueled/log/<ID>.md` (was gemacht, was offen, Testbefehle mit Ergebnis).
- Nach jedem Merge: `linux_hits.py` ausführen, Wert in METRICS.md eintragen.
- Temporäre Dateien nur unter `C:\CTR Reload\tmp\` bzw. im Paket-Worktree unter `out/`.
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

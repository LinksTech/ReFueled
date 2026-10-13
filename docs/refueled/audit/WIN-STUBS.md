# Audit WIN-STUBS: Windows-Zweige, die etwas Wirksames weglassen

Stand: Worktree AUD (refueled/windows, nach Entfernen der Wände in runtime_eventflag/sema/rwlock/
services/time/mutex). Nur lesen, nichts gebaut, nichts ausgeführt. Besitzer laut
`docs/refueled/tools/owners.txt` (erste passende Zeile; "-" = kein Eintrag).

Durchsucht: `src/**` (inkl. `src/platform/pf_*_win32.c`, `pf_common.c`), `gpu/shim/**`, `gpu/bbgpu.h`,
die `bbport`/`refueled`-Zeilen in `gpu/shadps4/**`, `gpu/CMakeLists.txt`, `run-windows.sh`,
`build-windows.sh`, `scripts/*.py`, `tools/gen_import_names.py`.

Methode: (a) alle `#if/#ifdef/#ifndef/#elif` mit `_WIN32`, `__linux__`, `__unix__`, `__APPLE__`, `_MSC_VER`
in `src/` und `gpu/shim/` gelistet, in `gpu/shadps4` alle `__linux__`/`__unix__`-Zweige; (b) `grep` nach
ENOSYS / not implemented / unsupported / Stub / TODO; (c) Abgleich aller in `pf_*.h` deklarierten
Funktionen gegen die Definitionen in `pf_*_win32.c`, `pf_common.c`, `pf_lowheap*.c`, `*.S`; (d) Lesen
aller `*_resolve`-Funktionen und ihrer Bedingungen; (e) Vergleich der `BB_*`-Variablen von run.sh und
run-windows.sh.

## Ergebnis in einem Satz

**Keine weitere Wand vom Typ "Windows-Zweig liefert resolve 0" gefunden.** In keiner `src/runtime*.c` gibt
es einen Plattform-`#if`, der Importe betrifft; kein Fund blockiert M2. Es bleiben bewusste Lücken
(D12 dma-buf, D13 userfaultfd), Diagnose-Lücken und ein still verworfenes `O_SYNC`.

## 1. Fundtabelle

Schwere: M1/M2/M3/M4 = blockiert diesen Meilenstein; D = nur Diagnose/Statistik; B = bewusst
(Entscheidung in ARCHITECTURE.md), wirkungslos, aber gewollt.

| Datei:Zeile | Art | Wirkung unter Windows | Schwere | Vorschlag | Besitzer |
|---|---|---|---|---|---|
| src/runtime_file.c:177-179 | `#ifdef O_SYNC` (MinGW kennt es nicht) | Gast-Flag 0x80 (SCE O_FSYNC) wird beim Öffnen still verworfen: kein Write-Through. `fsync` (Zeile 307, 574) und Savedata (runtime_savedata.c:95) flushen weiterhin | M4 (Speichern; nur bei Absturz/Stromverlust sichtbar) | In pf_posix_gap_win32.h `O_SYNC` als privates Bit führen und in `pf_gap_open`/`pf_gap_write` mit `_commit` oder `FILE_FLAG_WRITE_THROUGH` bedienen; mindestens Logzeile beim Verwerfen | R2c (Datei), F6 (Gap) |
| src/platform/pf_memory_win32.c:620-636 (`pf_handle_map_fixed`, `pf_handle_map`) | `pf_not_implemented` (PF_ENOSYS, einmal geloggt) | Gerätespeicher-Chunks (dma-buf) gibt es nicht. Aufrufer: runtime_memory.c:113,177,195,211 und gpu/shim/bbport_guest_memory.cpp:214-231. Unerreichbar, weil `chunk_alloc` (runtime_memory.c:85) unter Windows nie gesetzt wird (bbport_guest_memory.cpp:274-275 liegt hinter `PlatformSupported()==false`, Zeilen 127-141, 263-266). Direct Memory bleibt im Backing-Objekt | B (D12), wirkungslos | Keiner. Im Log prüfen, dass "pf_handle_map_fixed not implemented" nie erscheint | K1 |
| gpu/shim/bbport_guest_memory.cpp:127-141 (`PlatformSupported`) | `#ifdef _WIN32` liefert false | Schaltet PC-Speichermodell, `BB_GUEST_GPU_MEMORY`, `BB_GUEST_IN_PLACE`, `BB_PC_MODEL_ANY_GPU` ab; geprüft in buffer.cpp:115, buffer_cache.cpp:483 (`PcModelGpu`), Install (Zeile 263). GPU nutzt Kopien im VRAM (Verhalten "0.3"): langsamer, kein Fehler | B (D12) | Keiner; ab M3 Leistung messen | V3 |
| gpu/shadps4/video_core/renderer_vulkan/vk_instance.cpp:377 | Erweiterungen `VK_KHR_external_memory_fd`/`dma_buf` | Fehlen unter Windows, `guest_memory_export` bleibt false, Rest ungestört | B | Keiner | V3 |
| gpu/shadps4/video_core/buffer_cache/buffer.cpp:148 (Konstruktor mit `eDmaBufEXT`) | unbedingt, kein Plattformzweig | Nur im Pfad Guest-in-place erreichbar (dort aus). Würde unter Windows `createBuffer` mit unbekanntem Handle-Typ rufen, falls je erreicht | D (aktuell tot; Falle, falls D12 angefasst wird) | `PlatformSupported()` als Assert in den Konstruktor | V3 |
| gpu/shadps4/video_core/page_manager.cpp:37, 460-605, 637-650 | `#ifdef __linux__` (UffdImpl) | userfaultfd fehlt; immer `SignalImpl` (pf_fault_add_handler). `BB_UFFD=1` unter Windows ohne Wirkung und ohne Meldung | B (D13) | Optional eine Logzeile bei `BB_UFFD=1` | V1 |
| run-windows.sh:206 | `export BB_GUEST_IN_PLACE=0` (hart) | `BB_GUEST_IN_PLACE`/`BB_PC_MODEL` aus der Umgebung werden überschrieben. `BB_AS_0_3` wird gar nicht gelesen: run.sh:116-117 setzt dabei auch `BB_HOST_COPY_WAITS=all` und `BB_PRODUCER_CHECK=1`, die nicht dma-buf-spezifisch sind | B; `BB_AS_0_3=1` bleibt stumm ohne Wirkung | `BB_AS_0_3` auswerten (die zwei Variablen setzen) oder Hinweis ausgeben | - (nächstliegend F3b) |
| run-windows.sh:207 | `export BB_UFFD=0` | Gleich dem Linux-Standard (run.sh:168 `${BB_UFFD:-0}`); überschreibt nur einen Nutzerwert | B | Keiner | - |
| run-windows.sh:104-112 | Mods: Abbruch bei `BB_MODS_*`, Hinweis bei Ordner `mods` | Mods nicht nutzbar; `scripts/mods.py` läuft nicht (nutzt `symlink_to`: Zeilen 113, 141, 157; unter Windows Rechte nötig) | B (bis nach M4) | Spätere Variante mit Kopie/Hardlink statt Symlink | - |
| run-windows.sh:115-117 | `--software` ignoriert (Lavapipe) | Kein Software-Rendering, nur Meldung | D | Keiner | - |
| run.sh-Variablen, die run-windows.sh weglässt: `BB_AUTO_NOHIZ`, `RADV_DEBUG`, `__GL_SHADER_DISK_CACHE*`, `MESA_SHADER_CACHE_MAX_SIZE`, `VK_DRIVER_FILES`, `VK_LOADER_LAYERS_DISABLE`, `DISABLE_MANGOHUD`, `BB_IN_NIX_SHELL` | Linux-Treiber/Mesa/nix | Unter Windows ohne Wirkung (RADV/Mesa/MangoHud gibt es nicht). Der Pipeline-Cache des Spiels liegt in `user/cache` | D | Keiner | - |
| run-windows.sh:185 → tools/gen_import_names.py:20-34 | Netzabruf von `aerolib.inl` | Ohne Netz und ohne `--aerolib`-Kopie startet die Runtime ohne Symbolnamen: Importliste und "STOP: first unsupported PS4 import" zeigen nur NIDs. Die Auflösung läuft über NIDs und bleibt richtig. (AGENT-REGELN verbietet Downloads: M1-Läufe brauchen eine lokale Kopie `out/aerolib.inl`) | D (M1-Auswertung mühsam, aber richtig) | Lokale Kopie vorlegen bzw. `--aerolib` in run-windows.sh durchreichen | - |
| src/platform/pf_process_win32.c:112-116 (`pf_gpu_memory_in_use`) | `pf_not_implemented` | Statistikzeile "Memory: VRAM ..." zeigt VRAM 0 (driver.cpp:339-343). Einziger Aufrufer; keine Spielwirkung | D | `VK_EXT_memory_budget` (Kommentar "Later") | R3 |
| gpu/shadps4/core/libraries/videoout/driver.cpp:10-16, 312-341, 421-426, 435-441 | `#ifdef __linux__` | Statistik: `/proc/self/fdinfo` (VRAM/GTT), `getrusage` (Seitenfehler), Thread-CPU-Uhr des GPU-Threads. Unter Windows 0 (z. B. "Stall: GPU thread on CPU 0.0 ms") | D | Optional; liverpool.cpp:126 erklärt, warum `pf_thread_cpu_time_ns` (nur eigener Thread) nicht reicht | V2 |
| gpu/shadps4/video_core/amdgpu/liverpool.cpp:7-9, 126-131, 1817-1826 | `#ifdef __linux__` (getrusage, `pthread_getcpuclockid`) | `BbStats::gpu_user_us/gpu_sys_us/gpu_invol_switches/gpu_vol_switches/gpu_minor_faults/gpu_thread_clock` bleiben 0 bzw. -1 | D | wie oben | V2 |
| gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp:4273-4277, vk_rasterizer.h:259-261 | `#ifdef __linux__` um `Rasterizer::GetGpuCommandProcessorThreadId` | Wrapper fehlt unter Windows. Der einzige Aufruf (vk_rasterizer.cpp:752) geht direkt über `liverpool->`; `gpu_tid` wird unter Windows gesetzt (liverpool.cpp:135). Kein Fehler | D | Keiner | V2 |
| src/platform/pf_process_win32.c:20-35 (`pf_symbolize`) | Teilimplementierung | Liefert nur Modulname und -basis, nie ein Symbol (`symbol[0]==0`): Spuren in page_manager.cpp:83-90, bbport_wait_trace.cpp:40-60 und im Fehlerbericht zeigen `Modul+Offset` statt Funktionsnamen. Keine Spielwirkung | D (hilft bei M2-Debug) | Optional DbgHelp `SymFromAddr`; sonst Offsets extern auflösen (`-gcodeview` liegt vor) | R3 |
| src/platform/pf_exception_win32.c:234-254 (`pf_dump_signal_install`, `pf_dump_other_threads`) | Teilimplementierung, kein Signal | Dump läuft nur über den Watchdog (probe.c:361); kein `kill -USR2`-Dump auf Anforderung (probe.c:514). Pro Thread wird gesampelt (`pf_thread_sample`) | D | Keiner | K7 |
| src/platform/pf_thread_win32.c:94, 104 | `PF_ENOSYS` ohne `SetThreadDescription` (vor Win10 1607) | Thread-Namen leer; Aufrufer ignorieren das Ergebnis (runtime_thread.c:230), `pf_thread_get_name` liefert "" (Wait-Trace, Clobber). Win11: API vorhanden | D | Keiner | K6 |
| src/platform/pf_memory_win32.c:418, 569 | `PF_ENOSYS`, wenn `api_ready()` falsch (`VirtualAlloc2`/`MapViewOfFile3` nicht ladbar) | `pf_vm_reserve_layout`/`pf_backing_create` schlagen fehl, harter Start-Abbruch. Ab Win10 1803 nicht erreichbar | M1 (nur falls die API fehlt; hier Win11: nein) | Meldung mit Namen der fehlenden API und Windows-Version, falls noch nicht vorhanden | K1 |
| gpu/shadps4/video_core/buffer_cache/region_manager.h:15-31 | `#ifdef __unix__` → `AdaptiveMutex`, sonst `SpinLock` (Upstream) | Unter Windows ein Spinlock statt adaptivem Mutex in der Seitenverwaltung: bei Konkurrenz CPU-Last statt Schlafen | M3 (nur Leistung; R5 messen) | Bei FPS-Einbruch eine SRWLOCK-Variante | G3a |
| gpu/shadps4/video_core/renderer_vulkan/vk_instance.cpp:704-716 | `#if _WIN64 / #elif __linux__` | Upstream-Zweig, Windows nutzt die QPC-Zeitdomäne; gewollt, wirkt auf keine bbport-Funktion | B (nur notiert) | Keiner | V3 |
| gpu/shadps4/common/path_util.cpp:94-109; gpu/shim/bbgpu.cpp:269-275 | `#ifdef`-Kette; `_putenv_s` statt `setenv` | Benutzerordner: bbgpu setzt `BB_GPU_USER_DIR` (gleiche Semantik wie `setenv(...,0)`), run-windows.sh:230 liefert den Wert; ohne Wert Upstream-Fallback `%APPDATA%\shadPS4` (Pfadregel!) | B | Keiner, solange `BB_GPU_USER_DIR` immer gesetzt ist | G2a / G3c |

Gleichwertige Zweige (geprüft, keine Lücke): src/runtime.c:74-84 (`rand_s` statt `/dev/urandom`, R5);
gpu/shim/bbport_clobber.cpp:11-13, 206-232, 249-254; bbport_overlay.cpp:24-45; bbport_wait_trace.cpp:48-58;
window.cpp:6-9, 87-92 (G3c); gpu/CMakeLists.txt:64-70, 160-180, 197-230 (X11-Stand-in, TLS-Modell,
`ntapi.cpp`; Quelllisten gleich, F5); scripts/*.py und tools/*.py (keine `os.name`/`fcntl`/`/dev`/`/proc`-Treffer,
Ausnahme mods.py oben).

## 2. Plattformschicht: Funktionen mit PF_ENOSYS unter Windows

Alle in `pf_*.h` deklarierten Funktionen sind in den Windows-Quellen definiert (die drei Treffer
`pf_setjmp`/`pf_longjmp`/`pf_siglongjmp` sind Makros/Assembler in pf_jmp_win32.S, kein Fehlen).
`pf_not_implemented` wird unter Windows an genau drei Stellen aufgerufen:

| Funktion | Datei:Zeile | Aufrufer | Wirkung auf das Spiel |
|---|---|---|---|
| `pf_handle_map_fixed` | pf_memory_win32.c:620-628 | runtime_memory.c:113,177,195,211 | keine, nie erreicht (`has_handle()` verlangt `>=0`, es gibt nie ein Handle) |
| `pf_handle_map` | pf_memory_win32.c:630-637 | gpu/shim/bbport_guest_memory.cpp:214-227 | keine, nie erreicht (hinter `PlatformSupported()`) |
| `pf_gpu_memory_in_use` | pf_process_win32.c:113-116 | videoout/driver.cpp:341 | nur Statistik (VRAM 0) |

Dazu die bedingten `PF_ENOSYS` (`api_ready()`, `SetThreadDescription`) aus Abschnitt 1 und `pf_handle_close`
(leer, korrekt).

## 3. `*_resolve`-Funktionen

Maßstab: Plattform-`#if` in der Datei und Inhalt der Funktion. Alle 17 Teilfunktionen werden von
`runtime_resolve` (runtime.c:235-267) unbedingt aufgerufen; alle Quellen stehen in `WIN32_SOURCES.txt`.

| Funktion | Datei:Zeile | Plattform-`#if` in der Datei | Windows = Linux? |
|---|---|---|---|
| `runtime_resolve` | runtime.c:203 | 1 (Canary, gleichwertig) | ja |
| `runtime_mutex_resolve` | runtime_mutex.c:222 | 0 | ja |
| `runtime_thread_resolve` | runtime_thread.c:362 | 0 | ja |
| `runtime_content_resolve` | runtime_content.c:89 | 0 | ja |
| `runtime_time_resolve` | runtime_time.c:21 | 0 | ja (nur `gettimeofday`, wie Linux) |
| `runtime_sema_resolve` | runtime_sema.c:165 | 0 | ja |
| `runtime_eventflag_resolve` | runtime_eventflag.c:179 | 0 | ja |
| `runtime_json2_resolve` | runtime_json2.c:52 | 0 | ja |
| `runtime_rwlock_resolve` | runtime_rwlock.c:145 | 0 | ja |
| `runtime_memory_resolve` | runtime_memory.c:667 | 0 | ja |
| `runtime_kernel_resolve` | runtime_kernel.c:425 | 0 | ja |
| `runtime_services_resolve` | runtime_services.c:642 | 0 | ja |
| `runtime_ajm_resolve` | runtime_ajm.c:506 | nur `_GNU_SOURCE` | ja |
| `runtime_audio_resolve` | runtime_audio.c:270 | nur `_GNU_SOURCE` | ja (SDL3) |
| `runtime_pad_resolve` | runtime_pad.c:821 | nur `_GNU_SOURCE` | ja (SDL3) |
| `runtime_rtc_resolve` | runtime_rtc.c:150 | 0 | ja |
| `runtime_savedata_resolve` | runtime_savedata.c:325 | 0 | ja |
| `runtime_file_resolve` | runtime_file.c:639 | 1 (`O_SYNC`) | ja, bis auf O_SYNC |

Kein Treffer für `#if 0`, `HAVE_*` in `src/`; `__has_include` nur in gpu/shim/bbport_gnm_hooks.cpp:36
(generierte Tabelle, plattformneutral).

## 4. Nur unter Linux initialisiert, unter Windows gebraucht

In `src/` und `gpu/shim/` gibt es keinen `__linux__`-Zweig. In `gpu/shadps4` sind alle `__linux__`-Zweige
Statistik (driver.cpp, liverpool.cpp), der Uffd-Pfad (page_manager.cpp) oder der unbenutzte Wrapper
(vk_rasterizer). Das Prozess-Setup, das probe.c:447 als "Linux: mallopt" beschreibt, hat in `pf_init`
(pf_base_win32.c:22) sein Gegenstück: Ausnahme-Dispatcher, 1-ms-Timer; Low-Heap und TLS-Slot kommen aus
pf_lowheap_win32.c und pf_tls_win32.c. **Kein Fund.**

## 5. Schalter in run-windows.sh

| Variable | Zeile | Wirkung unter Windows |
|---|---|---|
| `BB_GUEST_IN_PLACE=0` | 206 | hart gesetzt, Nutzerwert ignoriert (B) |
| `BB_UFFD=0` | 207 | hart gesetzt; gleich dem Linux-Standard |
| `BB_PREUPLOAD`, `BB_COPY_GPU_BUFFERS`, `BB_GPU_WRITE_TWINS(_MAX)`, `BB_VBLANK_HZ`, `BB_NEO` | 194-212 | wirken (plattformneutral gelesen: buffer_cache.cpp:360, emulator_settings.h:56) |
| `BB_CRASH_DETAIL` | 240 | wirkt (probe.c:456) |
| `BB_RESTART_COMMAND` | 243 | wirkt (pf_process_win32.c:123-125), nur Windows |
| `BB_AS_0_3`, `BB_PC_MODEL`, `BB_PC_MODEL_ANY_GPU`, `BB_GUEST_GPU_MEMORY` | - | ohne Wirkung (D12); `BB_AS_0_3` lässt zusätzlich `BB_HOST_COPY_WAITS`/`BB_PRODUCER_CHECK` ungesetzt |
| `BB_MODS_*` | 104-112 | Abbruch, wenn gesetzt |
| `--software` | 115 | ignoriert |
| `TMP`/`TEMP` | 152-153 | auf `out/win/tmp` umgelegt |

## 6. Zusammenfassung (Anzahl je Schwere, 23 Tabellenzeilen aus Abschnitt 1)

| Schwere | Anzahl | Fundstellen |
|---|---|---|
| blockiert M1 | 1 (nur bedingt) | pf_memory_win32.c:418, 569 |
| blockiert M2 | 0 | - |
| blockiert M3 | 1 (Leistung) | region_manager.h:15-31 |
| blockiert M4 | 1 | runtime_file.c:177 |
| nur Diagnose | 11 | buffer.cpp:148 (Falle), --software, run.sh-Variablen, gen_import_names, pf_gpu_memory_in_use, driver.cpp, liverpool.cpp, vk_rasterizer, pf_symbolize, pf_dump_*, SetThreadDescription |
| bewusst / wirkungslos | 9 | pf_handle_*, PlatformSupported, VK-Erweiterung, page_manager (Uffd), BB_GUEST_IN_PLACE, BB_UFFD, Mods, vk_instance:704, path_util/bbgpu |

Kritische Funde (M1/M2): keine Wand. Einziger M1-Bezug ist `pf_memory_win32.c:418` und `:569` (nur bei
fehlender Win32-API). Für die nächste Runde wichtig: `runtime_file.c:177` (O_SYNC) vor M4,
`run-windows.sh:206` (`BB_AS_0_3` stumm), und für M1-Läufe eine lokale `aerolib.inl` (Download verboten).

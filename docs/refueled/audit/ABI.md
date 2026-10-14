# Audit F7: SysV-ABI an der Grenze Host/Gast

Stand: Worktree F7 (Basis 2c313ef), nur lesen. Geprüft: `src/*.c|h`, `gpu/bbgpu.h`, `gpu/shim/**`,
`gpu/shadps4/core/libraries/**` (die bbport-geänderten Teile von shadps4). Gründe: Unter Linux ist
`sysv_abi` die Standard-ABI, ein fehlendes Attribut fällt dort nie auf. Unter Windows (Win64:
rcx/rdx/r8/r9, Shadow Space, xmm6–15 callee-saved) ist jede solche Stelle ein stiller Fehler.

Methode: (1) Handlesen aller Funktionszeiger-Typen und aller Aufrufe über Zeiger; (2) Skripte
(`out/scan.py`, `out/scan2.py`, nicht eingecheckt) lesen alle Import-Tabellen (`{"Name",funktion}`,
`return (uintptr_t)funktion`, `LIB_FUNCTION(...)`) und prüfen, ob die Definition `ABI` bzw.
`PS4_SYSV_ABI` trägt (Heuristik: auf der Definitionszeile oder der Zeile davor); (3) Gegenprobe mit
gcc `-mabi=ms` (Win64 als Standard unter Linux): Das Attribut wirkt auch an
`using X = R PS4_SYSV_ABI (*)(...)` (Argumente landen in rdi/rsi/edx), und `va_list` ist dort
`char*` (Win64) statt Struct-Array (SysV).

`PS4_SYSV_ABI` (`gpu/shadps4/common/types.h:28`) ist unbedingt `__attribute__((sysv_abi))`, kein
`_WIN32`-Zweig. Gleichwertig zu `ABI` in `src/runtime.h:14`.

## Tabelle

Art: 1 = Host→Gast-Aufruf, 2 = Host-Funktion, die der Gast aufruft (Import, Stub-Ziel, Slot-Patch),
3 = setjmp/longjmp über Gastframes.

| # | Datei:Zeile | Art | Code (kurz) | ABI vorhanden | Risiko unter Win64 | Vorschlag | Paket |
|---|---|---|---|---|---|---|---|
| 1 | src/probe.c:590–591 | 1 | `typedef int (ABI *ModuleInit)(...)`, Modul-Init von libc.prx | ja | keins | – | – |
| 2 | src/probe.c:600–601 | 1 | `typedef void (ABI *Entry)(void*, void (ABI*)(void))`, Eintritt in `main` (Windows-Zweig), 2. Argument `guest_exit` (probe.c:139, ABI) | ja | Aufruf läuft auf dem Host-Stack statt Gast-Stack (D7 ist Aufgabe von K-Paketen); ABI selbst korrekt | Mit `pf_call_guest_on_stack` ersetzen (D7) | K-Strang |
| 3 | src/probe.c:132–137, 607 | 1 | Asm `enter_on_stack`, nimmt SysV-Argumente (rdi, rsi, rdx, rcx), `call *%rax` ins Gastimage; Prototyp ohne `ABI` | nein (Asm, nur `#ifndef _WIN32`) | Auf Windows nicht übersetzt (richtig). Wer es dort wiederverwendet, übergibt rcx/rdx/r8/r9 → falsch | Windows-Ersatz muss `ABI`-Signatur haben oder selbst Asm sein (steht in U2 §Zeile `enter_on_stack`) | K-Strang |
| 4 | src/runtime_thread.c:26, 224 | 1 | `typedef void *(ABI *GuestEntry)(void*)`; `t->entry(t->argument)` in `host_start` | ja | keins; Zugleich Art 3 (#32) | – | – |
| 5 | src/runtime_kernel.c:231, 237, 244 | 1 | `void (ABI *routine)(void)`, `routine()` in `thread_once` | ja | keins | – | – |
| 6 | src/runtime_kernel.c:246, 281 | 1 | `typedef void (ABI *KeyDestructor)(void*)`, `keys[i].destructor(value)` | ja | keins | – | – |
| 7 | src/runtime_kernel.c:292, 306 | 1 | `typedef void (ABI *ThreadHook)(void)`, `thread_dtors()` | ja | keins | – | – |
| 8 | src/runtime_kernel.c:294, 300–301 | 1 | `thread_atexit_count`, `thread_atexit_report`: werden gespeichert, im Baum nie aufgerufen | ja (Typ) | keins; sobald jemand aufruft, ist die Typisierung richtig | – | – |
| 9 | src/runtime.c:13, 123–131, 143–144 | 1 | `GuestCallback` (runtime.h:15) und `void (ABI *with_arg)(void*)`; `runtime_finalize` ruft beide | ja | keins | – | – |
| 10 | src/runtime.c:48–52 | 1 | `guest_set_heap_api` speichert die Funktionszeiger-Tabelle des Gasts (`void **`), `runtime_application_heap_api()` liefert sie; im Baum kein Aufruf | n/a (`void**`) | Wer die Einträge je aufruft, muss sie auf `ABI`-Typen casten | Hinweis im Header ergänzen, falls Aufrufer entsteht | – |
| 11 | src/runtime_kernel.c:181–185 | 1 | `guest_signal` merkt Gast-Signalhandler, "host signals are not forwarded" | n/a (nie aufgerufen) | keins | – | – |
| 12 | gpu/shadps4/core/libraries/avplayer/avplayer_impl.cpp:14, 21, 28, 35 | 1 | `allocate/deallocate/…_texture(ptr, …)` des Spiels (Typen avplayer.h:147–150) | ja (typedef mit `PS4_SYSV_ABI`) | keins | – | – |
| 13 | …/avplayer_impl.cpp:44, 53, 62, 71 | 1 | `open/close/read_offset/size` des Spiels (avplayer.h:160–163) | ja | keins | – | – |
| 14 | …/avplayer_file_streamer.cpp:33, 37, 60 | 1 | dieselben Dateizeiger, aus FFmpeg-Callback (`ReadFile`) | ja | keins (FFmpeg-Callback selbst ist Host→Host, normale ABI) | – | – |
| 15 | …/avplayer_state.cpp:95, 435 | 1 | `event_callback(ptr, event_id, …)` (avplayer.h:185) | ja | keins | – | – |
| 16 | …/avplayer.h:291; avplayer.cpp:200 | 1 | `AvPlayerLogCallback(void*, const char*, va_list)`, nie aufgerufen (`sceAvPlayerSetLogCallback` ist Stub) | ja | **`va_list`**: unter Win64 `char*`, der Gast übergibt einen Zeiger auf ein SysV-`va_list`-Struct. Sobald das Log je implementiert wird, falsch | Typ `__builtin_sysv_va_list` verwenden; `vsnprintf` darauf nicht aufrufen (MinGW-vsnprintf erwartet Win64-va_list), Argumente von Hand ausfalten | G-Strang (nur bei Implementierung) |
| 17 | gpu/shim/bbport_heap_sites.cpp:29–34 (Typen), 160, 170, 173, 180, 197, 204 (Aufrufe) | 1 | `using MallocFn = void* (*)(std::size_t)` usw.; `orig_malloc(size)` … zeigen auf libc.prx-Funktionen des Spiels (Slots, :247–252) | **nein** (6 Typen, 6 Aufrufe) | Aufruf mit rcx/rdx statt rdi/rsi: malloc/free im Spiel stürzen ab oder liefern Müll | Die sechs `using` mit `PS4_SYSV_ABI (*)` schreiben | Opus (gpu/shim) |
| 18 | gpu/shim/bbport_gnm_hooks.cpp:212 (Stub-Aufruf :329) | 2 | `extern "C" void BbGnmEnter(uint32_t index, u64 cb, u64* slot)`, aufgerufen von erzeugtem Maschinencode: `mov edi,index; mov rsi,[rsp+0xb0]; lea rdx,[rsp+0xb8]; call rax` | **nein** | Win64 liest rcx/rdx/r8 statt edi/rsi/rdx → `index`, `cb`, `slot` falsch → Absturz beim ersten Gnm-Aufruf; außerdem sichert der Stub xmm0–7 aber nicht xmm6–15 (nur nötig, wenn der Host sie als callee-saved erwartet; mit `sysv_abi` nicht nötig) | `PS4_SYSV_ABI` an die Definition | Opus (gpu/shim) |
| 19 | gpu/shim/bbport_gnm_hooks.cpp:229 (Stub-Aufruf :307) | 2 | `extern "C" u64 BbGnmLeave()`, aus dem Exit-Thunk (WriteExitThunk :300–313) per `call rax`, Rückgabe in rax | **nein** | Rückgabe rax stimmt zufällig, aber Stack-Anforderung: Win64 erwartet 32 Byte Shadow Space; der Thunk legt 0x28 an (ok), trotzdem sollte die Konvention explizit sein | `PS4_SYSV_ABI` | Opus (gpu/shim) |
| 20 | gpu/shim/bbport_heap_sites.cpp:159 | 2 | `void* WrapMalloc(size_t)`, als Slot-Wert in die libc des Gasts geschrieben (:253) | **nein** | Gast ruft mit rdi → Host liest rcx | `PS4_SYSV_ABI` | Opus (gpu/shim) |
| 21 | …/bbport_heap_sites.cpp:166 | 2 | `void WrapFree(void*)` (Slot :254) | **nein** | wie #20 | wie #20 | Opus |
| 22 | …/bbport_heap_sites.cpp:172 | 2 | `WrapCalloc` (Slot :255) | **nein** | wie #20 | wie #20 | Opus |
| 23 | …/bbport_heap_sites.cpp:179 | 2 | `WrapRealloc` (Slot :256) | **nein** | wie #20 | wie #20 | Opus |
| 24 | …/bbport_heap_sites.cpp:196 | 2 | `WrapMemalign` (Slot :257) | **nein** | wie #20 | wie #20 | Opus |
| 25 | …/bbport_heap_sites.cpp:203 | 2 | `WrapPosixMemalign` (Slot :259) | **nein** | wie #20 | wie #20 | Opus |
| 26 | gpu/shadps4/core/libraries/avplayer/avplayer.cpp:179 | 2 | `s32 PS4_SYSV_ABI sceAvPlayerPrintf(const char*, ...)` variadisch; Registrierung ist auskommentiert (:271) | ja | Definition ohne `va_start` übersetzt. Wird sie je mit `va_start` genutzt, lehnt clang das in `sysv_abi`-Funktionen mit Win64-`va_list` ab bzw. liest falsch | Beim Aktivieren `__builtin_sysv_va_start` / `__builtin_sysv_va_list` (clang) nutzen | G-Strang (nur bei Aktivierung) |
| 27 | …/avplayer.cpp:248, 280 | 2 | `sceAvPlayerVprintf(const char*, va_list)`, registriert | ja | Parameter `va_list` ist unter Win64 `char*`; die Funktion ist ein Stub und berührt es nicht, also derzeit harmlos | `__builtin_sysv_va_list` als Typ, falls sie je ausgewertet wird | G-Strang |
| 28 | src/runtime*.c NID-Tabellen | 2 | `RuntimeExport exports[]` (11 Dateien) und `runtime_*_resolve`-Ketten: 378 verschiedene Funktionen, dazu 28 aus Makro `PAIR` (runtime_file.c:576–578) und 24 aus Makro `DIALOG` (runtime_services.c:214–219) | ja, alle | keins; keine variadische Runtime-Funktion (`grep '\.\.\.)'` leer, `va_*` kommt nicht vor) | – | – |
| 29 | gpu/shadps4/core/libraries/{avplayer,gnmdriver,kernel/equeue,videoout} | 2 | 328 `LIB_FUNCTION`-Registrierungen (`reinterpret_cast<u64>(fn)`), Skript-Heuristik | ja, alle (0 ohne) | keins gefunden; Struct-Rückgaben/-Argumente in den registrierten Funktionen nicht einzeln geprüft (`sysv_abi` regelt sie) | – | – |
| 30 | src/probe.c:116 und 536–540 | 2 | `unresolved` (ABI) wird von erzeugten Trap-Stubs per `mov rsi,rdi; mov edi,index; movabs rax,handler; jmp rax` angesprungen | ja | keins | – | – |
| 31 | src/runtime_json2.c:26–28 | 2 | C++-vtable mit Host-Funktionen (`base_dtor`, `pure_virtual`, `notify_error`), vom Gast per Zeiger aufgerufen | ja | keins | – | – |
| 32 | src/runtime_thread.c:13, 43, 224, 285 | 3 | `setjmp(t->exit_jump)` in `host_start`, `longjmp` aus `thread_exit` (Gast ruft `scePthreadExit`), springt über Gastframes ohne Unwind-Info | n/a | MinGW-`longjmp` entrollt per SEH → Absturz. Genau der D9-Fall | `pf_setjmp/pf_longjmp` (src/platform/pf_jmp.h) einsetzen; `jmp_buf` → `pf_jmp_buf`; `thread_exit` ist `noreturn` und ABI, bleibt | K-Strang |
| 33 | src/probe.c:152 → gpu/shadps4/video_core/renderer_vulkan/vk_pipeline_cache.cpp:1087, vk_rasterizer.cpp:1199 | 3 | `siglongjmp(*recover,1)` im Fault-Handler zurück in `sigsetjmp(recover,0)` (Draw-Vorbereitung liest spekulativ Gastspeicher); Zeiger `runtime_fault_recover` (runtime.h:9–11, bbport_toggles.h:15) | n/a | Zwischen setjmp und Sprung liegen nur Host-Frames (kein Gastframe). Unter Windows geht der Sprung nicht aus dem VEH (U2): Kontext-Rücksprung nötig | `pf_sigsetjmp` + `pf_exception_resume` (src/platform/pf_exception.h); die zwei GPU-Stellen und `runtime_fault_recover` umstellen | K-Strang / gpu-Port |
| 34 | src/import_names.inc:71, 79 | 3 | NID-Namen `setjmp`, `longjmp` der PS4-libc | n/a | Nur Namen; libc.prx läuft nativ als Gastcode, kein Host-Code dazu (`runtime_lookup` liefert für libc 0, runtime.c:325) | – | – |
| 35 | gpu/shadps4/video_core/renderer_vulkan/vk_presenter.cpp:28 | 3 | `#include <csetjmp>`, keine Verwendung | n/a | keins | Include bei Gelegenheit entfernen | – |

## Zusammenfassung

| Art | Stellen (Zeilen der Tabelle) | davon ohne ABI |
|---|---|---|
| 1 Host→Gast | 17 (13 aktive Aufrufstellen, 4 gespeichert/nie aufgerufen oder `void**`) | 2 (#17 bbport_heap_sites, 6 Typen/6 Aufrufe; #3 Asm nur Linux) |
| 2 Importe/Stub-Ziele | Gruppen #18–#31: 378 + 52 Runtime-Funktionen, 328 GPU-Registrierungen, 8 Einzelfunktionen aus bbport | 8 (BbGnmEnter, BbGnmLeave, WrapMalloc/Free/Calloc/Realloc/Memalign/PosixMemalign); 0 in den NID-Tabellen |
| 2 variadisch/va_list | 3 (#16, #26, #27) | 0 ohne ABI, aber `va_list`-Typ unter Win64 falsch, sobald genutzt |
| 3 setjmp/longjmp | 4 Zeilen (#32–#35), 2 echte Sprünge | – (D9: #32 über Gastframes, #33 nur Hostframes) |

Alle 758 Einträge der Import-Tabellen (Runtime und GPU) tragen `ABI`/`PS4_SYSV_ABI`. Die Lücken sitzen
ausschließlich in zwei bbport-Dateien (`bbport_gnm_hooks.cpp`, `bbport_heap_sites.cpp`) und an den
vom Gast aufgerufenen, erzeugten Maschinencode-Stubs. Beide Dateien nutzen außerdem Linux-APIs
(`ucontext.h`, `REG_RIP`, feste Gast-Adressen) und gehören vermutlich ohnehin nicht zur ersten
Windows-Stufe; das Attribut sollte trotzdem sofort nachgetragen werden, weil es unter Linux nichts
ändert (Gate: Rennen byte-gleich).

## Die fünf wichtigsten Befunde

1. `BbGnmEnter`/`BbGnmLeave` (gnm_hooks.cpp:212, 229) haben kein `ABI`, werden aber aus erzeugtem
   Gastcode mit SysV-Registern aufgerufen. Unter Win64 sind die Argumente falsch.
2. `bbport_heap_sites.cpp`: sechs `Wrap*`-Funktionen werden als Slots in die Gast-libc geschrieben
   und sechs `orig_*`-Zeiger rufen die echten Gast-Funktionen auf, alle ohne ABI (12 Stellen).
3. `va_list` unter Win64: `AvPlayerLogCallback` (avplayer.h:291) und `sceAvPlayerVprintf`
   (avplayer.cpp:248) nehmen den Win64-Typ `char*`, der Gast schickt ein SysV-Struct-Array. Derzeit
   Stubs, also harmlos. Ein `va_start` in einer `sysv_abi`-Funktion auf Windows braucht
   `__builtin_sysv_va_start` und `__builtin_sysv_va_list` (und `__builtin_ms_va_list` für Host-Code,
   der Win64-`vsnprintf` ruft).
4. `thread_exit` springt per `longjmp` über Gastframes (runtime_thread.c:285): D9-Pflichtfall.
   Der Fault-Rücksprung (probe.c:152) quert dagegen nur Hostframes.
5. Das Attribut an Funktionszeiger-Typen per `using X = R PS4_SYSV_ABI (*)(...)` wirkt (Gegenprobe
   mit `gcc -mabi=ms`). Alle 758 Tabelleneinträge sind sauber; das Risiko liegt in den wenigen
   nicht-tabellengebundenen Stellen (Befund 1 und 2).

## Offene Fragen

- Gehören `bbport_gnm_hooks.cpp` und `bbport_heap_sites.cpp` zur ersten Windows-Stufe (sie sind
  stark an die Linux-Gastadressen und `ucontext_t` gebunden)? Wenn nein, genügt vorerst das
  Attribut plus `#ifndef _WIN32`.
- Die Skript-Heuristik (Definition mit `ABI` auf derselben oder der vorherigen Zeile) ist kein
  Compilerbeweis: gcc/clang warnen nicht, wenn eine Funktion ohne Attribut per Cast in eine Tabelle
  wandert. Ein Windows-Test, der jede Tabellenfunktion mit bekannten Argumenten ruft, läge
  außerhalb von F7.
- `runtime_application_heap_api()` (runtime.c:52) liefert eine Gast-Zeigertabelle; falls später ein
  Aufrufer entsteht, braucht er `ABI`-Typen. Aktuell kein Aufrufer.
- Nicht geprüft: Struct-Rückgabe/-Übergabe in den 328 GPU-Funktionen im Einzelnen und
  `gpu/third_party`. `sysv_abi` regelt das durch den Compiler; der Test `test_sysv_abi` belegt
  die Mechanik (sret, rax:rdx, xmm0:xmm1, Struct auf dem Stack).

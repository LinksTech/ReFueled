# Paket DBG: M2-Debug-Werkzeug (Branch refueled/pkg/DBG)

Ausgeführt von Sonnet, zu prüfen von Opus. Nicht committet. Ungebaut bis CLANG64 (`C:\msys64\clang64\bin\clang.exe`
existiert nicht; das Verzeichnis ist leer).

Geänderte Dateien: `src/probe.c` (nur Einfügungen, 0 gelöschte Zeilen), `run-windows.sh` (1 Zeile + 1 Kommentarzeile),
`docs/refueled/DEBUG-M2.md` (neu), `docs/refueled/log/DBG.md` (neu).

Verhaltensabweichung Linux: keine

## alt ↔ neu (probe.c)

Der Diff gegen HEAD enthält ausschließlich hinzugefügte Zeilen; kein bestehender Ausdruck wurde verändert.

| alt (Datei:Zeile, Code) | neu (Datei:Zeile, Code) | Linux-Gleichheit |
|---|---|---|
| fault(): nach dem Bericht direkt `void *frames[...]` (probe.c ~Z.150) | probe.c:152 `if (crash_detail) crash_detail_report(...)` vor dem Backtrace | `crash_detail` ist 0, solange `BB_CRASH_DETAIL` nicht exakt "1" ist; dann nur ein Vergleich eines statischen int, keine Ausgabe. Unter Linux setzt run.sh den Schalter nicht. |
| (neu) probe.c:111-112 | `static int crash_detail;` + Vorwärtsdeklaration von `crash_detail_report` | reine Deklaration |
| (neu) probe.c:~168-246 | `describe_guest_value`, `crash_detail_report` | nur aufgerufen bei gesetztem Schalter; nur pf_*-Aufrufe, snprintf, pf_write_stderr_str; keine Allokation |
| (neu) probe.c:~253-325 | `trace_import`, `thunk_template` (288 Byte), `THUNK_*`, `trace_wants` | nur erreichbar über Thunks, die nur bei gesetztem `BB_TRACE_IMPORTS` gebaut werden |
| main(): `check_cpu();` dann `int cpu_only...` | probe.c:453-459 neuer Block: getenv BB_CRASH_DETAIL / BB_TRACE_IMPORTS / BB_TRACE_IMPORTS_LIMIT in statische Variablen | getenv ohne Nebenwirkung; ohne Schalter bleiben `crash_detail=0`, `trace_filter=NULL`, `trace_limit=8` ungenutzt |
| Bindungsschleife: `outcome = calloc(...)` | danach probe.c:648-655: `thunks`/`thunk_built`/`trace_counts` nur `if (trace_filter && !list_imports)`, sonst `thunks=NULL` | ohne Schalter keine zusätzliche Allokation, kein zusätzliches `allocate()` (Reihenfolge und Adressen der übrigen Low-Heap-Allokationen unverändert) |
| `memcpy(image + relocs[i].target, &value, 8);` | davor probe.c:670-682 `if (thunks && kind==1 && trace_wants(...)) value = thunk` | bei `thunks==NULL` ist die Bedingung falsch, `value` unverändert: byte-gleiche Werte im image |
| `protect(traps, ..., 5);` | danach probe.c:708 `if (thunks) protect(thunks, ..., 5);` | bei `thunks==NULL` nichts |

Anmerkung zu fault(): Die Pflichtausgaben (Guest/Host fault, Mapping, Backtrace, Exitcode) sind unverändert.

## Prüfungen

- Syntaxcheck probe.c: nicht möglich, ungebaut bis CLANG64 (kein clang/gcc auf dem Rechner). Statisch von Hand geprüft:
  Typen/Signaturen gegen pf_exception.h, pf_thread.h, pf_process.h, pf_memory.h, pf_log.h; `access` als Variablenname
  vermieden (unistd.h), Formate mit PRIx64/PRIu64/%zu, kein unbenutztes Symbol.
- Thunk-Bytes: `C:\CTR Reload\tmp\refueled\DBG\gen_thunk.py` (Generator mit eigener Kodiertabelle, erzeugt das Array in
  probe.c) und `check_thunk.py` (liest das Array aus probe.c, disassembliert mit Capstone 5.0.7, vergleicht die
  47 Befehle mit der erwarteten Folge). Ergebnis: ALL OK. Geprüft: Länge 288 = deklariert, Slot 320 (Vielfaches von 16),
  INDEX/TRACE/TARGET-Offsets 128/144/280 stehen hinter `mov edi`/`movabs rax`/`jmp [rip+0]`, lückenlose Dekodierung,
  rsp beim `call rax` 16-Byte-ausgerichtet (Eintritt rsp%16==8), Rücksprungadresse bei rbp+0x88, Sicherungsblock 0xd0
  endet genau hinter xmm7, rsp nach der Wiederherstellung = rsp beim Eintritt, letzte drei Befehle vor dem `jmp` ändern
  nur rsp/rbp.
- Nicht getestet: Ausführung (kein Build, kein Spiel). Erster Lauf: `BB_TRACE_IMPORTS=sceKernelUsleep` und ein
  absichtlicher Fault.

## Offene Punkte / Schnittstellen

- Keine Headeränderung nötig. Keine pf_*-Funktion fehlt.
- `trace_import` bekommt als dritten Parameter den Zeiger auf den Sicherungsblock (erlaubt laut Auftrag).
- Thunk sichert keine ymm-Oberhälften; für Imports mit 256-Bit-Argumenten ungeeignet (unwahrscheinlich).
- Die Zeile in run-windows.sh steht hinter dem Dry-Run-Ausstieg, damit `BB_DRY_RUN=1` unverändert bleibt.
- Die Thunk-Prüfskripte liegen unter `C:\CTR Reload\tmp\refueled\DBG\`, nicht im Repo; `probe.c` verweist im Kommentar darauf.

## Review Opus
Diff Zeile für Zeile geprüft: ohne Schalter keine neue Ausgabe, keine Allokation, keine geänderten
Bindungswerte (Linux unverändert). Thunk: Ausrichtung (rsp ≡ 0 mod 16 beim call), Rücksprung-
adresse rbp+0x88, Red-Zone-Sprung, jmp statt call geprüft. Ergänzt: Thunk nur für Relokationen mit
Addend 0 (ein Thunk je Import wird von allen Relokationen geteilt). Ergänzt: `#define _GNU_SOURCE`
in probe.c geschützt (K3-Fehler, unter CLANG64 mit `-D_GNU_SOURCE` sonst `-Werror`).
Syntaxprüfung unter CLANG64 (clang 23.1.3, Flags von build-windows.sh, `-fsyntax-only`): fehlerfrei.

# B-GAP – Gap-Schicht: strcasestr, gmtime_r/localtime_r, rand_s (Welle B, F6/R4/R5-Teil)

Verhaltensabweichung Linux: keine
(Die Gap-Schicht gilt nur unter `_WIN32` per Force-Include von `build-windows.sh`; unter Linux wird sie nie gesehen. Keine Linux-wirksame Stelle geändert, daher keine Gleichheitstabelle nötig.)

Geänderte Dateien: `src/platform/pf_posix_gap_win32.h`, `src/platform/pf_posix_gap_win32.c`, `tests/refueled/test_posix_subset.c`.

## Neue Gap-Funktionen

| Funktion | Zweck | Zuordnung C | Zuordnung C++ |
|---|---|---|---|
| `pf_gap_strcasestr` | ASCII-Groß/Klein wie glibc, leere Nadel trifft am Anfang, Bytes >= 0x80 exakt | `#define strcasestr` (nur wenn nicht schon Makro) | `static inline strcasestr` (aus mit `PF_GAP_NO_CXX_FORWARDERS`) |
| `pf_gap_gmtime_r` | `_gmtime64_s` mit POSIX-Argumentreihenfolge, `struct tm*` oder NULL | `#define gmtime_r` nur wenn `_POSIX_THREAD_SAFE_FUNCTIONS` nach den System-Headern nicht gesetzt ist | keine (MinGW deklariert es in C++ ohne strikten Modus; imgui definiert ein eigenes static) |
| `pf_gap_localtime_r` | wie oben mit `_localtime64_s` | wie oben | keine |
| `_CRT_RAND_S` | `rand_s` deklariert (src/runtime.c:76 unverändert) | `#define _CRT_RAND_S` am Anfang des Force-Includes | gleich |

## Fehlerzahl Zähl-Build (`--keep-going --no-gpu --test`)

| Gruppe | vorher | nachher |
|---|---|---|
| F6 (test_posix_subset: gmtime_r/localtime_r) | 6 | 0 |
| R4 (runtime_pad.c strcasestr) | 1 | 0 |
| R5 rand_s (runtime.c:76) | 1 | 0 (runtime.c baut) |
| gesamt | 42 | 34 |

Verbleibend (andere Agenten/Opus): K5 1, R1 3, R5 29 (runtime_json2 atomics, runtime_mutex GuestCond), LH 1 (sys/mman.h), K6 test_thread (Einheit fehlgeschlagen, 0 Fehlerzeilen).

## test_posix_subset

Beim ersten Lauf nach dem Übersetzen: `FAIL tests/refueled/test_posix_subset.c:264: lseek(fd, 0, SEEK_CUR) == 0x80000001LL`.
Ursache: Testfehler, nicht Gap-Fehler. Die Zeile davor hatte `lseek(fd, 0, SEEK_END)` ausgeführt, der Zeiger stand also bei `big`; pread/pwrite haben ihn korrekt unverändert gelassen. Test angepasst: vor dem pwrite wird mit `lseek(fd, 0x80000001LL, SEEK_SET)` auf die erwartete Position gesetzt (eine Zeile hinzugefügt).

Ergebnis wörtlich: `test_posix_subset: 243 checks passed` (Build: `PASS test_posix_subset`; Gesamt `tests: 11 run, 2 failed` = test_lowheap, test_thread).
C++-Gegenprobe: `clang++ -std=c++17 -include pf_posix_gap_win32.h` mit `strcasestr` übersetzt (Smoke, Datei unter tmp).

# T1c: tests/test_file_mods.c unter Windows

Geändert: `tests/test_file_mods.c` (nur `#ifdef _WIN32`-Zweige), dieses Log.

Verhaltensabweichung Linux: keine (Test-Datei; der Nicht-Windows-Pfad ist textgleich, keine src/-Änderung)

## Befund
Einziger Compilerfehler: `symlink` undeklariert (Z. 23). Die Runtime (`src/runtime_file.c`) unterscheidet
Symlinks nicht von Dateien (kein lstat/readlink); die Fälle prüfen nur schreibgeschützte Game-Mounts (EROFS),
Lesen, stat, zusammengeführte Auflistung und schreibbare Saves.

## alt <-> neu
| alt (Datei:Zeile, Code) | neu | Linux-Gleichheit |
|---|---|---|
| test_file_mods.c:14 `char root[]="/tmp/bbport-mod-files-XXXXXX";` | `#ifdef _WIN32` Root aus `TMP`/`TEMP`, sonst unverändert `#else` dieselbe Zeile | Linux-Zweig textgleich |
| test_file_mods.c:23 `assert(!symlink(source,link));` | Windows: `CreateHardLinkA(link,source,NULL)` + Ausgabe "skipped on Windows: ..."; `#else` dieselbe Zeile | Linux-Zweig textgleich |
| (neu) `#include <windows.h>` | nur unter `_WIN32` | nicht übersetzt unter Linux |

## Übersprungen
Symlink-Traversierung (Symlinks brauchen unter Windows Rechte, MODLOADER.md §2). Ersatz: Hardlink auf dieselbe
Datei, damit alle übrigen Fälle (Lesen, stat, EROFS für open/truncate/path_op/rename, Verzeichnis-getdents,
Save-Schreiben) laufen.

## Ausgabe
```
PASS test_file_mods
skipped on Windows: symlink traversal (hard link used instead)
Guest mod files: reads, stat, merged listing, readonly assets and writable saves PASS
```
Temp-Ordner nach dem Lauf leer. Keine Runtime-Fehler unter Windows gefunden (Verzeichnis-open, getdents, `..`
wurden in diesem Test nicht verletzt).

## Hinweise außerhalb des Pakets
- `build-windows.sh:258` (und :292 analog): `test_log=/dev/stderr` schlägt in dieser MSYS2-Umgebung ohne
  `--keep-going` fehl ("/dev/stderr: Kein Verzeichnis"), alle 11 `tests/refueled`-Tests meldeten FAIL; mit
  `--keep-going` PASS. Außerdem lief der Befehl nur, wenn MSYSTEM=CLANG64 vor `bash -lc` im Aufrufer gesetzt war.

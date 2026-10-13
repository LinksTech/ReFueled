# WS1 – O_SYNC unter Windows nachbilden

Verhaltensabweichung Linux: keine

(Alle Änderungen stehen in `pf_posix_gap_win32.h/.c`, das nur unter `_WIN32` wirkt und sonst leer ist;
`src/runtime_file.c` und alle Linux-Dateien sind unberührt. Der Test `test_posix_subset.c` läuft
plattformneutral, die neuen Fälle prüfen unter Linux nur write/pwrite/pread mit O_SYNC, die
`pf_gap_fd_is_sync`-Prüfungen stehen unter `#ifdef _WIN32`. Linux wurde nicht gebaut, Regel 2026-10-13.)

## Änderungen

| Datei | Änderung |
|---|---|
| `src/platform/pf_posix_gap_win32.h` | `PF_GAP_O_SYNC 0x02000000` (privates Bit; kein MinGW-`_O_*`-Wert >= 0x01000000), `#ifndef O_SYNC #define O_SYNC PF_GAP_O_SYNC`; Deklarationen `pf_gap_close`, `pf_gap_fd_is_sync`; C-Makro `close(fd)` -> `pf_gap_close(fd)` |
| `src/platform/pf_posix_gap_win32.c` | Bitfeld `g_sync_bits[8192/32]` mit `InterlockedOr/And`; `pf_gap_open` entfernt das Bit vor `_open` und setzt/löscht die Markierung des neuen Deskriptors (auch O_DIRECTORY-Pfad löscht); `pf_gap_write`/`pf_gap_pwrite` rufen nach Erfolg (Ergebnis > 0) bei Markierung `_commit(fd)`; `pf_gap_close` löscht die Markierung, dann `_close`; `#undef close` |
| `tests/refueled/test_posix_subset.c` | O_SYNC-Fälle: open O_SYNC\|O_CREAT\|O_WRONLY, write, pwrite, Inhalt per zweitem Deskriptor gelesen, Markierung nach close weg, Wiederverwendung ohne O_SYNC nicht markiert |

## Entscheidungen

- Deskriptor-Tabelle: Die UCRT-Low-Level-Tabelle fasst höchstens 8192 Deskriptoren; das Bitfeld deckt alle
  ab, größere Nummern werden nie markiert (kommen nicht vor). Kein Lock nötig.
- Fehler von `_commit`: write/pwrite liefern -1 mit `errno = EIO`. Begründung: POSIX O_SYNC verlangt,
  dass write erst nach dem Durchschreiben zurückkehrt; ein Fehler beim Durchschreiben ist ein write-Fehler
  (Linux meldet EIO). Die Daten sind dann bereits im Cache geschrieben (wie unter Linux möglich); der
  Aufrufer behandelt -1 als fehlgeschlagenes Schreiben (runtime_file.c bricht ab/meldet Fehler).
  Bei einem mehrteiligen write (> 1 GiB) wird einmal am Ende committet.
- close-Makro: Reihenfolge Markierung löschen, dann `_close`, damit eine nach `_close` sofort
  wiederverwendete Nummer nicht versehentlich entmarkiert wird. `pf_gap_open` setzt/löscht die Markierung
  zusätzlich bei jedem open (Schutz gegen Deskriptoren, die über `_close`/`fclose` geschlossen wurden).
- Prüfung grep: In `src/**` ruft nur `runtime_file.c` `close()` auf (Dateideskriptoren); keine
  Struktur-Member `.close(`/`->close(`, kein dup/dup2, keine close()-Nutzung auf Sockets/Pipes.
  dup/dup2 werden nicht genutzt; ein dupliziertes Handle würde die Markierung nicht erben (dokumentiert).
  C++ bekommt weiterhin keine Makros.
- Direkt über `_close` geschlossene Deskriptoren (z. B. in `pf_gap_truncate`) sind nie markiert.

## Testergebnis (wörtlich)

`bash build-windows.sh --no-gpu --test --skip-gpu` (CLANG64, GPU-Bibliothek aus out/win/gpu kopiert), Exit 0:

```
test_posix_subset: 254 checks passed
PASS test_posix_subset
Guest mod files: reads, stat, merged listing, readonly assets and writable saves PASS
PASS test_file_mods
tests (build.sh set): 6 run, 0 failed
```
Alle Plattformtests (test_exception, test_fs, test_lowheap, test_memory, test_process, test_stubs,
test_sysv_abi, test_thread, test_time, test_tls u. a.) bestanden. Volllog: `C:\CTR Reload\tmp\refueled\WS1\build.log`.

## Offene Punkte

- Linux nicht gebaut (Regel). Kein Test, dass `_commit`-Fehler -> EIO tatsächlich auftritt (schwer zu erzwingen).

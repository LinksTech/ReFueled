# Paket R2b – Aufrufer in runtime_savedata.c / runtime_file.c

Stand 2026-10-13, ausgeführt von Sonnet, Prüfung und Commit durch Opus. Ungebaut bis CLANG64.

## Geändert (nur src/runtime_savedata.c)
- `#include <ftw.h>` entfernt, `#include "platform/pf_fs.h"` ergänzt (wie `platform/pf_jmp.h` in runtime.h).
- `remove_entry` entfernt. 2 Aufrufe umgestellt. `runtime_file.c`: keine Änderung nötig.

## Alt <-> neu (Linux-Gleichheit)
| alt (Datei:Zeile, Code) | neu (Datei:Zeile, Code) | Linux-Gleichheit |
|---|---|---|
| runtime_savedata.c:116-118 `static int remove_entry(...) { return remove(path); }` | entfernt | Gleicher Callback steht in pf_fs_linux.c:27-32 (`remove(path)`, zusätzlich nur errno merken; Rückgabe unverändert). |
| runtime_savedata.c:226 `nftw(host,remove_entry,16,FTW_DEPTH\|FTW_PHYS);` | :223 `pf_fs_remove_tree(host);` | pf_fs_linux.c:42 `nftw(path,remove_entry,16,FTW_DEPTH\|FTW_PHYS)`: gleiche Funktion, Tiefe, Flags, `remove()`; Abbruch beim ersten Fehler wie vorher. Unterschied nur: vorgeschaltetes `lstat` (pf_fs_linux.c:39), fehlender Pfad -> 0 ohne nftw-Aufruf. Hier belanglos: nftw liefert für fehlenden Pfad nur -1 ohne Seiteneffekt, und `host` wurde in Z. 222 per `stat` schon als vorhanden bestätigt; `meta` fehlend ergibt dasselbe Ergebnis (nichts gelöscht). Ein Symlink als Wurzel: lstat ok, nftw mit FTW_PHYS entfernt den Link selbst, wie vorher. |
| runtime_savedata.c:227 `nftw(meta,...)` | :224 `pf_fs_remove_tree(meta);` | wie oben. Rückgabewert wird weiter ignoriert (Kommentar ergänzt). Reihenfolge host, dann meta unverändert; das erste ignorierte Ergebnis stoppt das zweite nicht. |
| `#include <ftw.h>` (Z. 17) | entfernt | nur noch in pf_fs_linux.c:9 benötigt. |

Hinweis Dokumentation: pf_fs.h und pf_fs_linux.c sagen "Abbruch beim ersten Fehler" (stimmt: Callback gibt remove()-Fehler zurück); R2.md schreibt unter Windows/Linux "Durchlauf geht weiter" – für Linux falsch, nur Hinweis.

## rename-Stellen (nicht umgestellt)
| Stelle | Ziel muss ersetzt werden? | Zuständig |
|---|---|---|
| runtime_savedata.c:98 `write_atomic`: `rename(temp,path)` | ja, `path` (param.bin, Memory-Datei...) existiert meist schon; Atomarität ist der Zweck (bbport 0.5 #64) | F6 Gap-Schicht (rename ersetzt unter Windows) |
| runtime_file.c:298 `commit_file`: `rename(f->temp,f->commit)` | ja, `commit` ist die bestehende Speicherdatei (Z. 249 "renames the old one to its backup first": das Spiel schreibt sie neu) | F6 |
| runtime_file.c:544 `do_rename` (Gast-`rename`) | POSIX/FreeBSD: ja, ein bestehendes Ziel (Datei) wird ersetzt; Gast-Backup-Muster (alt -> .bak, neu -> alt) verlangt das | F6 |
Windows-Zusatz für F6: offenes Ziel in einer anderen Handle-Instanz (Spiel hat Datei noch offen) -> MoveFileExW scheitert mit SHARING_VIOLATION; commit_file schließt `f->host` vor dem rename (Z. 297), passt.

## struct stat: Vorlage für Opus (PfStat), nichts umgebaut
Gast-Struct: `GuestStat` (120 Byte, Z. 25-35). Alle Leser:
- `convert_stat(const struct stat*,GuestStat*)` Z. 207-216, aufgerufen aus `do_fstat` (Z. 485) und `do_stat` (Z. 497). Zuordnung:
  - `st_dev` -> `dev` (uint32 Cast); `st_ino` -> `ino` (uint32 Cast); `st_mode` -> `mode` (uint16 Cast); `st_nlink` -> `nlink` (uint16 Cast);
  - `st_size` -> `size` (int64); `st_blocks` -> `blocks` (int64); `st_blksize` -> `blksize` (uint32);
  - `st_atim/st_mtim/st_ctim` (`tv_sec`,`tv_nsec`) -> `atime/mtime/ctime`; `birthtime=ctime`.
  - uid, gid, rdev, flags, gen, lspare bleiben 0 (memset). PfStat aus LLP64 Nr. 21 (dev, ino, mode, nlink, size, atime_ns/mtime_ns/ctime_ns) deckt das ab bis auf `blocks` und `blksize`: entweder PfStat um diese ergänzen oder in convert_stat berechnen (`(size+511)/512`, 4096). `*_ns` -> sec/nsec per `/1000000000` und `%1000000000` (Vorzeichen für Zeiten vor 1970 beachten).
- weitere `struct stat`-Nutzer in runtime_file.c (nur diese Felder):
  - Z. 197-199 `fstatat(dirfd(d),name,&entry,0)` in `list_directory`: nur `S_ISDIR/S_ISREG(st_mode)` (Eintragstyp bei DT_LNK/DT_UNKNOWN). Braucht `pf_is_dir`/`PfStat` auf Pfad (`dir/name`) und `dirfd` entfällt.
  - Z. 259-268 `open_for_commit`: `stat(source)`; `S_ISREG(st_mode)`; `st_mode&07777` als Rechte der Kopie (Windows: 0644 fest).
  - Z. 334-336 `do_open`: `fstat(host)` + `S_ISDIR(st_mode)`; Z. 349 `(long long)s.st_size` nur Trace (Hinweis: `s` bleibt unbelegt, wenn fstat scheitert, Linux-Verhalten, nicht ändern).
  - Z. 488-495 `do_stat`: `stat(path)` -> `convert_stat`; Z. 495 `(long long)s.st_size` Trace.
- runtime_savedata.c: Z. 112-113 `stat(path,&st)`: nur `st_mtime` -> `Param.mtime` (int64, `time_t` 64 Bit unter UCRT, LLP64 Nr. 29); Z. 133 und 224 `stat(host)`: nur Existenz und `S_ISDIR` (Z. 133).

## read/write/pread/pwrite mit großen Größen (LLP64 Nr. 24/25), nichts geändert
- Z. 410 `pread(h,buffer,size,offset)` / `read(h,buffer,size)` (`read_retrying`, size uint64, offset int64, Ergebnis `ssize_t`).
- Z. 453 `write(h,buffer,size)` (size uint64); Z. 462 `pwrite(h,buffer,size,offset)`.
- Z. 237/241 `read/write` mit 64-KiB-Puffer (Nr. 23, ok).
- Z. 475 `lseek(f->host,offset,whence)`, Ergebnis `off_t` (Nr. 25).
- Z. 551/560 `ftruncate/truncate` (Nr. 26); savedata Z. 286-287 `fseek/ftell/ftruncate`, Z. 307 `fseek(f,offset,SEEK_SET)` (Nr. 27/28).
- Alle `%lld`/`(long long)`, `%llu`/`(unsigned long long)`: bereits portabel. `%lx`/`(long)`-Casts auf 64-Bit-Werte: keine gefunden. Linux-Ausgabe unverändert (nichts geändert).

## Weitere Linux-/Gap-Abhängigkeiten (melden, nichts geändert)
Kein `getdents`/`getdirentries`-Syscall: `do_getdents` baut FreeBSD-Einträge aus der eigenen Liste (`list_directory`), kein Host-Aufruf. Offen für F6/Opus:
- `d_type`/`DT_*`/`DT_UNKNOWN` bei `readdir` (runtime_file.c:65, 195-201): fehlt unter MinGW teils, Gap-Schicht oder Fallback über `PfStat`.
- `fstatat`, `dirfd` (Z. 198).
- `O_DIRECTORY`, `O_CLOEXEC` (Z. 173-174, 268, 271, 291): MinGW kennt `O_DIRECTORY` nicht; `O_CLOEXEC` ~ `O_NOINHERIT`; `sync_directory` (Z. 288-293) Windows: leer.
- `realpath` (Z. 354), `strcasecmp` (`<strings.h>` ok in MinGW), zweiargumentiges `mkdir` (runtime_file.c:99, 102, 533; savedata make_dirs), `fsync` (Z. 296, 562; savedata Z. 96), `fileno`, `ftruncate`, `truncate` (Z. 560), `pread/pwrite`, `unlink`, `rmdir`.
- `__atomic_add_fetch` (Z. 266, Clang ok).
- WICHTIG: Beide Dateien sind komplett in `#ifndef _WIN32 ... #else (Stub) #endif` gefasst (runtime_file.c:12/643, runtime_savedata.c:14/331). Die Guards bleiben vorerst; Entfernen ist Opus' Entscheidung nach F6/PfStat.

## linux_hits.py --list (Treffer dieser Dateien, primary)
runtime_file.c: 32 Treffer, davon `posix-dir` (dirent.h, opendir/readdir/closedir an Z. 13, 55-67, 149-154, 178-204), `pthread` (Z. 17, 46, 71-125, 251-254, 337-371, 500-525), `memfd` (Z. 551 `ftruncate`, Fehlklassifizierung durch Muster, gehört zu LLP64 Nr. 26).
runtime_savedata.c: 26 Treffer: `posix-dir` (Z. 16, 249-260), `pthread` (Z. 13, 64, 134-311), `memfd` (Z. 289 `ftruncate`).
Verschwunden gegenüber vorher: der `ftw`/`nftw`-Treffer in runtime_savedata.c. Verbleibende Kategorien sind POSIX-Teilmenge (dirent, pthread via winpthreads) oder Gap (ftruncate); keine neue Linux-only-API.

## Prüfungen
Kein Build (kein CLANG64). Gelesen: pf_fs.h, pf_fs_linux.c, R2.md, LLP64.md. `git diff` geprüft; keine Reste von `nftw`, `FTW_`, `remove_entry`, `ftw.h` in beiden Dateien (grep).

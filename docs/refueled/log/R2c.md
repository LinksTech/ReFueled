# Paket R2c – runtime_file.c und runtime_savedata.c ohne Windows-Stub-Wände

Verhaltensabweichung Linux: remove_stale_temps steigt bei DT_UNKNOWN per stat in Unterordner ab (D21 Nr. 5); stat-Fehlercodes seit 1b6cb2b wieder -errno

Stand 2026-10-13, ausgeführt von Sonnet, Prüfung und Commit durch Opus. Ungebaut bis CLANG64 (nur gelesen und gegen die Header geprüft).
Geändert: `src/runtime_file.c`, `src/runtime_savedata.c`, dieses Log. "neu"-Zeilen = Stand im Worktree.

## Wichtigste Abweichung (Opus entscheiden)
`pf_fs_linux.c:62` (`to_pf_stat`) bildet jeden stat-Fehler außer ENOENT/EACCES auf PF_EIO ab. Der Altcode lieferte `-errno`.
Linux-Unterschied nur im Fehlerfall von `do_stat`/`do_fstat`: ENOTDIR (Pfad läuft durch eine Datei), ELOOP, ENAMETOOLONG
(Komponente >255), EOVERFLOW erreichen den Gast jetzt als EIO (über `runtime_guest_errno`). Erfolgspfad sowie ENOENT/EACCES/EIO
sind identisch. Die ENOENT-Wiederholung nach `fix_case` (`r==-ENOENT`) bleibt wirksam, da PF_ENOENT == -2 == -ENOENT. Für `do_fstat`
ist EBADF ausgeschlossen (`host_fd` hat den Deskriptor geprüft). Wenn exakt gleiches Verhalten verlangt ist: pf_fs_linux.c müsste
ENOTDIR/ELOOP/ENAMETOOLONG durchreichen (PF_E*-Enum in pf_base.h erweitern) – Header gehören Opus, nicht angefasst.

## Alt <-> neu
| alt (Datei:Zeile, Code) | neu (Datei:Zeile, Code) | Linux-Gleichheit |
|---|---|---|
| runtime_file.c:12 `#ifndef _WIN32` | entfernt | nur Präprozessor; unter Linux war der Zweig aktiv. |
| runtime_file.c:643-647 `#else` + Stubs `runtime_file_resolve/report/configure` + `#endif` | entfernt | Linux-Zweig hat eigene Definitionen (:101, :639, :651); Stubs waren nur Windows. |
| (neu) | runtime_file.c:8 `#include "platform/pf_fs.h"` | nur Deklarationen. |
| :65 `e->d_type==DT_DIR` (remove_stale_temps) | :73 `entry_is_dir(e,path)`; Helfer :55-60: `type=PF_DIRENT_TYPE(e); if (type!=DT_UNKNOWN) return type==DT_DIR; …pf_fs_stat(path)&&S_ISDIR(st.mode)` | Linux: PF_DIRENT_TYPE = `e->d_type` (pf_fs.h), bei bekanntem Typ gleiche Entscheidung. Nur bei DT_UNKNOWN (Linux: Dateisysteme ohne d_type; ext4/btrfs/tmpfs/xfs mit ftype liefern ihn) wird per stat entschieden statt nicht zu rekursieren – dort kein Unterschied auf den üblichen Dateisystemen. DT_LNK wird wie vorher nicht verfolgt. |
| :168 `if (flags&0x80) r|=O_SYNC;` (host_flags) | :177-179 dieselbe Zeile in `#ifdef O_SYNC … #endif` | Linux definiert O_SYNC: Zeile bleibt wirksam, gleiche Flags. MinGW-w64 kennt O_SYNC nicht (sonst Compilefehler); unter Windows entfällt das Flag. |
| :195 `unsigned char type=e->d_type;` (list_directory) | :205 `PF_DIRENT_TYPE(e)` | = `e->d_type` unter Linux. |
| :197-199 `struct stat entry; if (!fstatat(dirfd(d),e->d_name,&entry,0)) type=S_ISDIR(entry.st_mode)…S_ISREG` | :207-212 `char full[1400]; PfStat entry; if (snprintf("%s/%s",path,name)<sizeof(full) && !pf_fs_stat(full,&entry)) type=S_ISDIR(entry.mode)…S_ISREG(entry.mode)` | fstatat(…,0) folgt Links, stat(path) folgt Links (pf_fs_linux.c:75): gleicher Inode, gleicher Typ. `path` ist der `opendir(path)`-Pfad, also dasselbe Verzeichnis wie `dirfd(d)`. Unterschiede: Race, wenn das Verzeichnis zwischen opendir und stat umbenannt wird (irrelevant); Pfadlänge: `path` ≤1023 + Name ≤255 < 1400, Rückfall wird nie übersprungen. Fehler: `type` bleibt wie vorher unverändert. Modusbits = POSIX (pf_fs_linux.c:65). |
| :207-216 `convert_stat(const struct stat*…)` | :218-227 `convert_stat(const PfStat*…)`: `(uint32_t)dev`, `(uint32_t)ino`, `(uint16_t)mode`, `(uint16_t)nlink`, `(int64_t)size`, `(int64_t)blocks`, `blksize`, `{atime_sec,atime_nsec}` usw. | pf_fs_linux.c:63-68 kopiert jedes Feld 1:1 in 64-Bit-Felder; die Kürzungen auf uint32/uint16 erfolgen wie vorher in convert_stat. size/blocks uint64→int64: gleicher Wert (nie ≥2^63). Zeiten unverändert. birthtime=ctime, memset unverändert. |
| :259-261 `struct stat s; int exists=!stat(source,&s); … S_ISREG(s.st_mode)` (open_for_commit) | :270-272 `PfStat s; int exists=!pf_fs_stat(source,&s); … S_ISREG(s.mode)` | `exists` hängt nur von Erfolg/Misserfolg ab (alle Fehler = "nicht vorhanden", wie vorher). Gleiche Modusbits. |
| :268 `(int)(s.st_mode&07777)` | :279 `(int)(s.mode&07777)` | gleiche Bits. |
| :334-336 `struct stat s; … !fstat(host,&s) && S_ISDIR(s.st_mode)` (do_open) | :344-347 `PfStat s={0}; … !pf_fs_fstat(host,&s) && S_ISDIR(s.mode)` | pf_fs_fstat = `fstat(fd)` (pf_fs_linux.c:81). Einzige Änderung: `s` nullinitialisiert (vorher bei fstat-Fehler uninitialisiert; nur der Audio-Trace liest es). |
| :349 `(long long)s.st_size` (Audio-Trace) | `(long long)s.size` (Zeile direkt unter dem do_open-Registrieren, ~:360) | gleicher Wert. |
| :483-485 `struct stat s; if (fstat(h,&s)) return -errno; convert_stat(&s,out);` (do_fstat) | :494-497 `PfStat s; int r=pf_fs_fstat(h,&s); if (r) return r; convert_stat(&s,out);` | Erfolg identisch. Fehler `-errno` → PF_E* (siehe Abschnitt Abweichung; EBADF unmöglich). |
| :488 `char path[1024]; struct stat s;` (do_stat) | :499 `… PfStat s;` | – |
| :493 `int r=stat(path,&s) ? -errno : 0;` | :505 `int r=pf_fs_stat(path,&s);` | pf_fs_linux.c:75 `stat(path)`; Erfolg identisch; ENOENT/EACCES/EIO gleiche Zahl (-2/-13/-5), andere Fehler → -5 (siehe oben). |
| :494 `… r=stat(path,&s) ? -errno : 0;` nach fix_case | :506 `… r=pf_fs_stat(path,&s);` | Bedingung gleich (PF_ENOENT == -ENOENT). |
| :495 `… r ? -1LL : (long long)s.st_size` (Save-Trace) | :507 `… s.size` | gleicher Wert. |
| runtime_savedata.c:14 `#ifndef _WIN32` | entfernt | nur Präprozessor. |
| runtime_savedata.c:328-332 `#else` + Stubs `runtime_savedata_configure/resolve/report` + `#endif` | entfernt | Linux-Zweig hat eigene Definitionen (:71, :325, :326). |
| runtime_savedata.c:130-131 `struct stat st; int exists=!stat(host,&st) && S_ISDIR(st.st_mode);` (mount) | :129-130 `PfStat st; int exists=!pf_fs_stat(host,&st) && S_ISDIR(st.mode);` | nur Erfolg/Misserfolg und Typbits werden ausgewertet, kein Fehlercode: identisch. |

POSIX unverändert (MinGW hat `stat`): runtime_savedata.c:111-112 `read_param` (`stat(path,&st)`, nur `st_mtime` → Param.mtime), :220-221 `save_delete` (`stat(host,&st)` nur Existenz). `st_mtime` ist unter MinGW-UCRT 64 Bit (Gap-Header erzwingt es).
Nicht umgestellt (Gap-Schicht deckt ab): open/read/write/pread/pwrite/lseek/ftruncate/truncate/fsync/rename/mkdir/rmdir/unlink/access/realpath/opendir/readdir.

## Punkt 3: Linux-only-Reste
`linux_hits.py --ref C:/nitro-ps4-ref/shadPS4-b4e7ae7 --list` nach der Änderung:
- runtime_file.c: posix-dir :13 (`#include <dirent.h>`), :55 (entry_is_dir), :63/:65/:75 (remove_stale_temps), :157/:160/:162 (fix_case), :188/:192/:193/:215 (list_directory); pthread :17, :46, :79-97, :123/:132, :262/:265, :348-359, :377-382, :512/:537; memfd (Fehlklassifikation) :563 `ftruncate(h,length)`.
- runtime_savedata.c: pthread :13, :63, :133-210, :281-310; posix-dir :15, :248/:249/:259 (save_search); memfd (Fehlklassifikation) :288 `ftruncate(fileno(f),…)`.
Alles POSIX-Teilmenge bzw. winpthreads/Gap; nichts Echtes fehlt. Windows-Auffälligkeiten (nur gemeldet):
- `__atomic_*` (runtime_file.c:278, 368, 435, 446, 466): Clang/CLANG64 ok, nicht MSVC.
- `sync_directory` (:296-303): `open(dir,O_RDONLY|O_DIRECTORY|O_CLOEXEC)` + `fsync(d)`: Gap-open öffnet das Verzeichnis, Gap-fsync ist dort No-op; `strrchr(dir,'/')` passt zu `C:/…`.
- `do_open` auf ein Verzeichnis: Das Gastflag O_DIRECTORY (0x20000) wird nur dann weitergereicht, wenn der Gast es setzt. Ohne das ruft Gap-open `_open` auf ein Verzeichnis, das unter Windows mit EACCES scheitert. Prüfen, ob SacredFire Verzeichnisse immer mit O_DIRECTORY öffnet; sonst Gap-open: Verzeichnis-Fallback (F6/Opus).
- `fix_case` (:145): `if (path[0]!='/') return 0` – Windows-Hostpfade `C:/…` beginnen mit Laufwerksbuchstabe: dort No-op. Unkritisch (NTFS case-insensitiv).
- `translate` (:108-134) prüft ".."-Komponenten nur mit '/' als Trenner; die Windows-CRT akzeptiert zusätzlich '\\'. Ein Gastpfad wie `/app0/..\x` würde die Prüfung passieren und könnte unter Windows aus dem Mount ausbrechen. Sicherheitsfrage für Opus (nicht geändert).
- `realpath` (:365, nur BB_MOD_TRACE): Vergleich `actual[n]=='/'` schlägt bei '\\'-Rückgabe fehl; nur Trace, keine Funktionsstörung.
- `S_ISDIR/S_ISREG` auf `PfStat.mode`: Gap-Header definiert beide, falls fehlend.

## Punkt 4: Pfadtrenner
Alle Hostpfade werden mit '/' zusammengesetzt (`snprintf "%s/%s"`: remove_stale_temps :69, fix_case :150, list_directory :209, `runtime_file_configure` `user/…`, translate `"%s%s"` mit dem '/'-Gastrest; savedata root/mount/meta `"%s/%s"`, make_dirs zerlegt an '/'). Die Windows-CRT (und CreateFileW über die Gap-Schicht) akzeptiert '/', also funktionsfähig. `make_dirs` ruft bei `C:/…` einmal `mkdir("C:")` (scheitert harmlos, Ergebnis ignoriert). Nichts geändert.

## Zähler
Stat-Stellen auf PfStat: 9 (list_directory-Fallback, convert_stat, open_for_commit, do_open, do_fstat, do_stat ×2, savedata mount; dazu 3 Trace-Größenzugriffe). d_type-Stellen: 3 (remove_stale_temps, list_directory ×2 Zeilen). Wände entfernt: 2. Zusätzlich `#ifdef O_SYNC`: 1.

## Review Opus
Linux-Gleichheit geprüft: convert_stat-Zuweisungen gleich, fstatat(dirfd,name,0) ≡ stat(path/name) (beide folgen Links), d_type über PF_DIRENT_TYPE; O_SYNC nur wo definiert. Fehlercodes: pf_fs_stat gibt unter Linux seit diesem Merge -errno unverändert zurück (to_pf_stat angepasst), damit gleich. Offen (Folgepaket B): ..-Prüfung in translate kennt nur "/" (Windows akzeptiert auch "\"); Verzeichnis-open ohne O_DIRECTORY unter Windows.

## Prüfung Opus 2026-10-13 (Pflichtfeld)
Feld bei der einmaligen Durchsicht aller Logs gesetzt und gegen die alt↔neu-Tabelle geprüft.

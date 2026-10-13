# ReFueled – Audit F10: LLP64 und Typgrößen

Stand 2026-10-13, Worktree `C:\nitro-ps4-wt\F10` (Branch `refueled/pkg/F10`). Reiner Lese-Audit, es wurde kein Code geändert.
Zugehörig: [../ARCHITECTURE.md](../ARCHITECTURE.md), [../INTERFACES.md](../INTERFACES.md) §2, [../TASKS.md](../TASKS.md).

## 1. Ausgangslage

Linux x86-64 ist LP64, Windows x64 (clang, `x86_64-w64-windows-gnu`, UCRT) ist LLP64:

| Typ | Linux (LP64) | Windows (LLP64) |
|---|---|---|
| `long`, `unsigned long` | 64 Bit | **32 Bit** |
| `wchar_t` | 32 Bit | **16 Bit** |
| `time_t` | 64 Bit | 64 Bit (UCRT, solange `_USE_32BIT_TIME_T` nicht gesetzt ist) |
| `struct timespec` | `{time_t; long}` = 16 Byte, `tv_nsec` 64 Bit | `{time_t; long}` = 16 Byte (Padding), **`tv_nsec` nur 32 Bit** |
| `off_t` | 64 Bit | 32 Bit, mit `_FILE_OFFSET_BITS=64` 64 Bit (F3 setzt das) |
| `ssize_t`, `size_t`, `intptr_t` | 64 Bit | 64 Bit |
| `clock_t` | 64 Bit | 32 Bit |
| `struct stat` | `st_atim/st_mtim/st_ctim`, `st_blocks`, `st_blksize`, 64-Bit-`st_ino` | **kein** `st_atim`, kein `st_blocks`/`st_blksize`, `st_ino` 16 Bit, `st_nlink` 16 Bit, `st_size` je nach Variante 32 oder 64 Bit |
| `struct tm` | hat `tm_gmtoff` | **hat kein `tm_gmtoff`** |
| `read/write/_read/_write` | `size_t`-Länge, `ssize_t`-Rückgabe | `unsigned int`-Länge, `int`-Rückgabe |

Der Gast (PS4, FreeBSD, LP64) erwartet in seinen Strukturen 64-Bit-`long`, 64-Bit-`tv_nsec` usw.
Die Angaben zur Windows-Spalte stammen aus dem Wissen über UCRT/MinGW-w64; belegt wird sie erst durch den Unittest aus Abschnitt 7 Nr. 12 (siehe offene Frage 1).

## 2. Methode und Suchmuster

Geprüft wurden `src/*.c`, `src/*.h` (7454 Zeilen), `gpu/shim/**`, `gpu/bbgpu.h` und die gegenüber
`C:\nitro-ps4-ref\shadPS4-b4e7ae7\src` geänderten oder neuen Dateien unter `gpu/shadps4` (118 Dateien; von diesen nur die
**hinzugefügten Zeilen** je `diff -U0`, ohne Zeilenende-Unterschiede, bei neuen Dateien die ganze Datei).
Der unveränderte Upstream-Code (G2/G3) wurde nur stichprobenartig auf Gast-ABI-Typen geprüft (Abschnitt 6).

Suchmuster (Python-Skripte unter `C:\CTR Reload\tmp\refueled\F10\`: `scan.py`, `added.py`, `diff.sh`; nicht im Repo):

| Kürzel | Muster |
|---|---|
| long | `\b(unsigned\s+)?long\b` (ohne `long long`) |
| typen | `off_t off64_t time_t ssize_t clock_t wchar_t suseconds_t useconds_t pid_t pthread_t intptr_t ptrdiff_t` |
| strukturen | `struct timespec/timeval/stat/dirent/iovec/tm`, `ucontext_t`, `greg_t`, `REG_*`, `fstat stat readdir nanosleep clock_gettime gettimeofday localtime pread pwrite lseek ftruncate mmap fallocate`, `st_*`, `tv_sec tv_nsec tv_usec tm_gmtoff` |
| suffix | Literale `\d+[uU]?[lL]` (ohne `ll`), `0x..UL` |
| format | `%[flags][breite]l[diuxXo]`, `%z`, `%j`, `%t`, sowie `%ll` gegen die Argumenttypen |
| cast | `(long)`, `(unsigned long)`, `reinterpret_cast<long>`, `(int)(uintptr_t)`, `(uint32_t)(uintptr_t)` |
| builtin/lib | `__builtin_{clz,ctz,popcount,ffs}l`, `strtol/strtoul/atol/labs/lround/lrint`, `sscanf` |
| ABI | alle Zeilen mit `ABI`/`PS4_SYSV_ABI` auf Typen `long time_t off_t ssize_t wchar_t clock_t bool float double short signed unsigned` |
| Gaststrukturen | alle `typedef struct`, `_Static_assert`, Bitfelder (`: N;`), `packed`, `aligned` in `src/` |

Ergebnis in Zahlen (vor dem Aussortieren): 175 Treffer `long` (überwiegend harmlose `unsigned long long`-Casts in
`%llu/%llx`-Formaten), 60 Treffer Typen, 68 Treffer Format, 20 Treffer Cast, 0 Treffer Literal-Suffix `UL` in `src/` und `gpu/shim`.
`(unsigned long long)` kommt in 135 Zeilen vor und ist **richtig** (auf beiden Plattformen 64 Bit, passend zu `%llu/%llx/%#llx`).
Der Code ist in den Gast-ABI-Teilen bereits sauber: alle Gaststrukturen in `src/` verwenden `<stdint.h>`-Typen, alle
`ABI`-Funktionen verwenden `intN_t/uintN_t/int/uint32_t/size_t/uintptr_t/void*`. Befunde liegen überwiegend an der
Grenze Host-API zu Gast (b/c) sowie in Diagnose-Ausgaben (d/e).

Schweregrad in der Spalte "Problem": **[hoch]** Übersetzungsfehler oder falsches Verhalten im Spielbetrieb, **[mittel]**
Datenverlust in Diagnose/Logs oder Grenzfälle, **[niedrig]** korrekt, aber fragil oder Linux-only, **[ok]** dokumentierte Prüfstelle ohne Handlungsbedarf.

## 3. Befunde

Kategorien: a Gaststruktur, b ABI-Signatur bzw. API-Parameter, c Kopie/Cast Host-Gast, d Formatstring, e Literal-Suffix/Zeiger-Cast.
Pakete wie in TASKS.md (K2 `runtime_memory.c`, K3 `probe.c`, K5 `runtime_thread.c`, R1 `runtime_kernel.c`, R2 Dateien/Spielstände, R4 Audio/Pad/AJM, R5 übrige Runtime, S1/S2/S3 Shim, V1/V2/V3 vendored Diagnose, G3 vendored sonst).

| # | Datei:Zeile | Code (kurz) | Kat. | Problem unter LLP64 | Fix-Vorschlag | Paket |
|---|---|---|---|---|---|---|
| 1 | src/probe.c:160-161 | `"…offset 0x%lx…", (unsigned long)(rip - (uintptr_t)image)` | d,e | [mittel] `unsigned long` hat 32 Bit: Offset/Adresse wird abgeschnitten (Gast-Offset < 0x10000000 passt, Host-Adressen nicht). `-Wformat` schweigt, weil Cast und Format zusammenpassen. | `"0x%" PRIx64`, Argument `(uint64_t)(rip - (uintptr_t)image)` (`<inttypes.h>`, wie `runtime_memory.c:954`). Die Funktion wandert nach `pf_exception_*`/`pf_process_*` (K3/R3), dort gleich so schreiben. | K3 |
| 2 | src/probe.c:163-164 | `"%s+0x%lx", … (unsigned long)(rip - (uintptr_t)where.dli_fbase)` | d,e | [mittel] wie 1, hier mit Host-Modulbasis, Abschneiden realistisch. | wie 1. | K3 |
| 3 | src/probe.c:179-180 | `unsigned long from, to; sscanf(line, "%lx-%lx", &from, &to)` | d | [niedrig] Liest `/proc/self/maps` (nur Linux). Unter LP64 richtig; bliebe der Code im gemeinsamen Teil, wäre er unter Windows falsch. | Nach `pf_describe_mapping` (`pf_process_linux.c`) verschieben; dort `uint64_t` und `SCNx64`. | K3 (R3) |
| 4 | src/probe.c:194-195 | `"  #%d %s+0x%lx (%s)\n", … (unsigned long)((uintptr_t)frames[i] - (uintptr_t)where.dli_fbase)` | d,e | [mittel] wie 1 (Backtrace). | wie 1. Mit `pf_backtrace`/`pf_symbolize` ohnehin ersetzt. | K3 |
| 5 | src/probe.c:236,239 | `long n; … syscall(SYS_getdents64,…); for (long at=0; at<n;)` | c | [niedrig] `syscall()` liefert `long`; nur Linux (Watchdog, `/proc/self/task`). | Mit `pf_threads_list` (pf_thread.h) ersetzt; in `pf_thread_linux.c` bleibt `long` (LP64) zulässig. | K3 |
| 6 | src/probe.c:241 | `pid_t tid=(pid_t)strtol(d->name,NULL,10)` | e | [niedrig] `strtol` liefert 32 Bit; TIDs passen. Nur Linux. | Mit Nr. 5 erledigt (`pf_threads_list` liefert `uint64_t tid`). | K3 |
| 7 | src/runtime_thread.c:61 | `syscall(SYS_arch_prctl, ARCH_SET_GS, (unsigned long)base)` | e | [niedrig] Zeiger nach `unsigned long` verliert unter LLP64 die oberen 32 Bit (der Gast-TCB liegt über 4 GiB). Nur Linux. | Wandert nach `pf_tls_linux.c`; dort ist der Cast unter LP64 richtig, die Schnittstelle nimmt `void*`/`uintptr_t`. | K5 |
| 8 | src/runtime_thread.c:255 | `printf("…stack=%llu…", (unsigned long long)stack)` | d | [ok] richtig. | – | K5 |
| 9 | src/runtime_kernel.c:117 | `struct timespec t={.tv_sec=(time_t)(ns/1000000000),.tv_nsec=(long)(ns%1000000000)}` | c,e | [niedrig] `tv_nsec` (32 Bit) fasst Werte < 1e9, `time_t` 64 Bit: korrekt. `nanosleep` kommt aus winpthreads. | Gemeinsamer Helfer `static inline void pf_ns_to_timespec(uint64_t ns, struct timespec *ts)` (ohne `(long)`-Cast); ersetzt auch Nr. 33, 36, 37, 38, 39. | R1 |
| 10 | src/runtime_kernel.c:66-70, 82 | `ts->sec=t.tv_sec; ts->nsec=t.tv_nsec;` (Host nach `GuestTimespec{int64,int64}`) | a,c | [ok] feldweise Kopie in feste 64-Bit-Felder, richtig (kein `memcpy`); `tv_nsec` wird vorzeichenrichtig erweitert. | Beibehalten. `_Static_assert(sizeof(GuestTimespec)==16)` ergänzen (fehlt). | R1 |
| 11 | src/runtime_kernel.c:87-91, 102, 123-124 | `(uint64_t)(t.tv_sec-process_start.tv_sec)*1000000+…(t.tv_nsec-…)/1000` | c | [ok] die `tv_sec`-Differenz ist 64 Bit, die `tv_nsec`-Differenz liegt in ±1e9 und passt in 32 Bit; Vorzeichenerweiterung vor `(uint64_t)` wie unter Linux. | Beibehalten. | R1 |
| 12 | src/runtime_kernel.c:142-143, 350-351, 359-360; src/runtime_rtc.c:71-74; src/runtime_services.c:98 | `localtime_r(&t,&lt); … lt.tm_gmtoff` | c | [hoch] **Übersetzungsfehler**: UCRT-`struct tm` hat kein `tm_gmtoff` (unter Linux `long`, 64 Bit); `localtime_r` ist nur mit MinGW-Zusatz da. | Neue Schnittstelle (Opus): `int64_t pf_utc_offset_seconds(int64_t utc_seconds, int *is_dst)` in `pf_process.h`. Linux: `localtime_r` + `tm_gmtoff`; Windows: `_localtime64_s` + `_get_timezone`/`_get_dstbias` oder `GetTimeZoneInformation`. Argumente und Rückgabe `int64_t`. | R1, R5 (Schnittstelle: Opus) |
| 13 | src/runtime_kernel.c:201-208 | `getrusage(…); out->utime=(GuestTimeval){r.ru_utime.tv_sec, r.ru_utime.tv_usec}` | a,c | [mittel] `getrusage`/`struct rusage` fehlt unter Windows. Die feldweise Kopie in `GuestTimeval{int64,int64}` ist richtig; `GuestRusage` (`rest[14]`) ist fest 144 Byte. | Windows: `GetProcessTimes`/`GetThreadTimes` (FILETIME nach µs, `uint64_t`) hinter `pf_thread_cpu_time_ns` bzw. einer neuen `pf_process_cpu_time_us`. `_Static_assert(sizeof(GuestRusage)==144)` ergänzen. | R1 |
| 14 | src/runtime_kernel.c:377 | `ssize_t n=getrandom((char*)buffer+done,size-done,0)` | b,c | [mittel] `getrandom` gibt es nicht; der Windows-Ersatz `BCryptGenRandom` nimmt eine **32-Bit-Länge (`ULONG`)**. Bei `size` über 4 GiB falsch abgeschnitten (praktisch nie, aber Typfalle). | `pf_random(void*, uint64_t)` in `pf_base.h`, Schleife in Blöcken ≤ 1 GiB. | R1 |
| 15 | src/runtime_kernel.c:491-492 | `fprintf(dump,"%s %d %s %#lx %#lx %#lx …", …(unsigned long)w->site, …site2, …site3)` | d,e | [mittel] `site*` sind `uint64_t`; `unsigned long` schneidet unter Windows ab. Die Datei `BB_WAIT_LOG` wird von Werkzeugen gelesen, falsche Werte ohne Warnung. | `"%#" PRIx64` mit `uint64_t`-Argumenten (ohne Cast). | R1 |
| 16 | src/runtime_kernel.c:499-500 | `printf("… +%#lx<+%#lx<+%#lx …", (unsigned long)…site, …)` | d,e | [mittel] wie 15. | wie 15. | R1 |
| 17 | src/runtime_kernel.c:584-585 | `printf("GOT %s -> %#lx %s:%s+%#lx\n", got,(unsigned long)target,…,(unsigned long)(target-…))` | d,e | [mittel] `target` ist ein Hostzeiger, Windows schneidet auf 32 Bit ab. | `PRIx64`/`uint64_t`, oder `%p` für `target`. | R1 |
| 18 | src/runtime_kernel.c:597, 603-605 | `printf("%s +%#lx %.1f%%",…,(unsigned long)(rows[i].rip-text_lo)…)`, `%#lx`, `(unsigned long)rows[i].caller` | d,e | [mittel] wie 17 (Sampler-Bericht, Host-RIP). | wie 15. | R1 |
| 19 | src/runtime_kernel.c:521-522 | `const ssize_t got=process_vm_readv(…); for (ssize_t i=0;i<got/8;++i)` | c | [niedrig] `ssize_t` ist auch unter Windows 64 Bit; die Funktion ist Linux-only (Signalhandler, `ucontext`). | Wird durch `pf_read_memory_safe` (liefert `int64_t`) ersetzt. | R1 |
| 20 | src/runtime_kernel.c:53-60 | `host_clock(): CLOCK_REALTIME_COARSE, CLOCK_THREAD_CPUTIME_ID, CLOCK_PROCESS_CPUTIME_ID` | b | [mittel] Kein Größenproblem, aber `clockid_t`-Konstanten: die drei Uhren fehlen in winpthreads/MinGW. | `host_clock` auf `pf_clock_ns(PfClock)` (mono, real, thread-CPU, process-CPU) umstellen, Ergebnis `uint64_t` Nanosekunden. Mit `pf_thread_cpu_time_ns` abstimmen (Opus). | R1 |
| 21 | src/runtime_file.c:207-214 (`convert_stat`) und 197-199 (`fstatat`, `dirfd`) | `g->dev=(uint32_t)s->st_dev; g->ino=(uint32_t)s->st_ino; g->blocks=s->st_blocks; g->atime=(GuestTimespec){s->st_atim.tv_sec, s->st_atim.tv_nsec}` | a,c | [hoch] **Übersetzungsfehler/Falschwerte**: UCRT-`struct stat` hat kein `st_atim/st_mtim/st_ctim`, kein `st_blocks`, kein `st_blksize`; `st_ino` ist 16 Bit, `st_nlink` 16 Bit, `st_dev` 32 Bit; `st_size` ist je nach Variante (`_stat32`/`_stat64i32`/`_stat64`) 32 oder 64 Bit; `fstatat`/`dirfd` fehlen. | Neue Schnittstelle (Opus) in `pf_fs.h`: `typedef struct { uint64_t dev, ino; uint32_t mode, nlink; int64_t size; int64_t atime_ns, mtime_ns, ctime_ns; } PfStat; int pf_stat(const char*, PfStat*); int pf_fstat(int, PfStat*); int pf_is_dir(const char*)`. Windows: `GetFileInformationByHandle` (`nFileIndex` 64 Bit, FILETIME nach ns), Blöcke = `(size+511)/512`, `blksize` 4096. `convert_stat` nimmt `PfStat`. | R2 |
| 22 | src/runtime_file.c:259-268, 334-336, 483-484, 488-495 | `struct stat s; stat(source,&s); S_ISREG(s.st_mode); s.st_mode&07777; (long long)s.st_size` | c | [hoch] gleiche Wurzel wie 21: `stat`-Variante/`st_size`-Breite, `S_ISDIR/S_ISREG` nur mit Gap-Schicht, Berechtigungsbits (`07777`) gibt es unter Windows nicht. | Über `PfStat`/`pf_stat` (Nr. 21); `mode`-Bits für Windows fest `0644/0755` (offene Frage 3). | R2 |
| 23 | src/runtime_file.c:237-241 | `ssize_t n=read(from,buffer,sizeof(buffer)); … write(to,buffer+done,(size_t)(n-done))` | b | [ok] 64-KiB-Puffer, passt in `unsigned int`. | Beibehalten. | R2 |
| 24 | src/runtime_file.c:407-414, 421, 430-433, 453, 462 | `read_retrying(int h,void *buffer,uint64_t size,int64_t offset,…)`: `pread(h,buffer,size,offset)`, `read(h,buffer,size)`, `write(h,buffer,size)`, `pwrite(h,buffer,size,offset)` | b,c | [mittel] `size` ist `uint64_t`. UCRT-`_read/_write` nehmen `unsigned int` und liefern `int`: ab 4 GiB stilles Abschneiden, ab 2 GiB negative Rückgabe wie ein Fehler. Gap-`pread/pwrite` (F6) müssen dasselbe beachten; der Offset (`int64_t`) darf nicht über ein 32-Bit-`off_t`/`long` laufen (`OVERLAPPED.Offset/OffsetHigh`). | In `pf_posix_gap_win32.h` `read/write/pread/pwrite` als Schleife in Blöcken ≤ 1 GiB (`0x40000000`) mit `ssize_t`-Summe. Unittest in F6: Blocklogik mit Größe 0x1'0000'0001 als Funktion prüfen (keine Datei nötig). | R2 (Gap: F6) |
| 25 | src/runtime_file.c:475-476 | `off_t r=lseek(f->host,offset,whence); return r<0 ? -errno : r;` | b,c | [mittel] Hängt an `_FILE_OFFSET_BITS=64`. Fehlt das Makro vor dem ersten Include, sind `off_t`/`lseek` 32 Bit, Offsets über 2 GiB werden zu `long`. | `_Static_assert(sizeof(off_t)==8,…)` in `pf_posix_gap_win32.h` (wird per `-include` vor allem geladen); Unittest `lseek` über 2 GiB. Aufruf auf `(int64_t)lseek(…)` umstellen. | R2 (Gap: F6) |
| 26 | src/runtime_file.c:551, 560 | `ftruncate(h,length)`, `truncate(path,length)` (`int64_t length`) | b | [mittel] `truncate` fehlt unter MinGW; `ftruncate` nimmt `off_t` (mit `_FILE_OFFSET_BITS=64` → `ftruncate64`, sonst `long`). | Gap: `truncate` als `open`+`ftruncate`; `_Static_assert` wie Nr. 25. | R2 (Gap: F6) |
| 27 | src/runtime_savedata.c:291-292 | `fseek(f,0,SEEK_END); if ((uint64_t)ftell(f)<size && ftruncate(fileno(f),(off_t)size))` | b,c | [niedrig] `fseek/ftell` arbeiten mit `long` (32 Bit): unter 2 GiB richtig; `memory.dat` kann laut PS4-API bis 1 GiB groß sein, bleibt also knapp darunter. Fragil. | `fseeko/ftello` (mit `_FILE_OFFSET_BITS=64` auf MinGW `fseeko64`) oder `_fseeki64/_ftelli64` über die Gap-Schicht. | R2 |
| 28 | src/runtime_savedata.c:310 | `fseek(f,offset,SEEK_SET)` mit `int64_t offset` | b,c | [niedrig] `int64_t` nach `long`: bei Offset über 2 GiB abgeschnitten (durch die `memory_size`-Prüfung in Zeile 308 ausgeschlossen, solange die Größe unter 2 GiB bleibt). | wie Nr. 27 (`fseeko`). | R2 |
| 29 | src/runtime_savedata.c:113 | `if (!stat(path,&st)) p->mtime=st.st_mtime;` (`Param.mtime` ist `int64_t`, Zeile 43) | a,c | [ok] `time_t` ist unter UCRT 64 Bit; `Param` ist als `OrbisSaveDataParam` fest (1328 Byte, `_Static_assert`). Vorausgesetzt `_USE_32BIT_TIME_T` fehlt. | `_Static_assert(sizeof(time_t)==8,…)` in der Gap-Schicht. | R2 |
| 30 | src/runtime_memory.c:137, 181, 189-190, 205-206, 223, 231, 237, 275 | `ftruncate(pool_fd,(off_t)…)`, `mmap(…,pool_fd,(off_t)phys)`, `fallocate(…,(off_t)at,(off_t)n)` | b,c | [niedrig] Linux-Code (memfd), unter LP64 richtig. Windows hat weder `mmap` noch `fallocate`. | K2 verschiebt das nach `pf_memory_linux.c`; die `pf_*`-Signaturen haben schon `uint64_t offset` (pf_memory.h:73-91). Nichts davon im gemeinsamen Code lassen. | K2 |
| 31 | src/runtime_memory.c:726 (`runtime_memory_set_guest_chunk_allocator(int (*alloc)(uint64_t phys, uint64_t size))`), :90 (`Region.fd`, `int fd`), `chunk_fds[]` | Dateideskriptor als `int` | b | [hoch] Der Rückgabewert des Allokators ist ein Linux-`fd` (`int`). Ein Windows-`HANDLE` ist ein Zeiger (64 Bit) und würde als `int` abgeschnitten. `PfMemHandle` ist schon `intptr_t` (pf_memory.h:83), der Rückruf und `Region.fd` noch nicht. | Rückruf: `PfMemHandle (*alloc)(uint64_t phys, uint64_t size)`; `Region.fd`/`chunk_fds` auf `PfMemHandle`; `PF_MEM_HANDLE_NONE` statt `-1`. Gegenstück: Nr. 45. | K2 (+V3) |
| 32 | src/runtime_pad.c:107-110, 604-613 | `struct stat st; stat(path,&st); st.st_mtim.tv_sec==config_mtime.tv_sec && st.st_mtim.tv_nsec==…; config_mtime=st.st_mtim;` | c | [hoch] **Übersetzungsfehler** (`st_mtim` fehlt); außerdem `tv_nsec` 32 Bit gegen 64-Bit-Quellen. | `PfStat.mtime_ns` (Nr. 21) oder `pf_mtime_ns(const char*, int64_t*)`; `config_mtime` als `int64_t`. | R4 |
| 33 | src/runtime_mutex.c:116-119 | `clock_gettime(CLOCK_REALTIME,end); … end->tv_sec += (time_t)(…); end->tv_nsec=(long)(ns%1000000000);` | c,e | [niedrig] korrekt (siehe Nr. 9). | Helfer aus Nr. 9. | R5 |
| 34 | src/runtime_mutex.c:204 | `struct timespec end={.tv_sec=(time_t)abs->sec,.tv_nsec=(long)abs->nsec}` (`abs` ist `GuestTimespec*`, Zeile 201) | a,c | [niedrig] feldweise Wandlung vom Gasttyp (64/64) zum Hosttyp (64/32). `abs->nsec` ist in Zeile 203 auf `[0, 1e9)` geprüft, `tv_nsec` verliert nichts. | Beibehalten; Cast durch Helfer ersetzbar. `GuestTimespec` ist **viermal** definiert (Nr. 40). | R5 |
| 35 | src/runtime_rwlock.c:99 | `struct timespec deadline={.tv_sec=(time_t)time->seconds,.tv_nsec=(long)time->nanoseconds}` | a,c | [niedrig] wie 34; der Gasttyp heißt hier `GuestTime` (Zeile 23), `nanoseconds` ist in Zeile 97 geprüft. | wie 34. | R5 |
| 36 | src/runtime_sema.c:81 | `struct timespec end={.tv_sec=(time_t)(deadline/1000000000),.tv_nsec=(long)(deadline%1000000000)}` | c,e | [niedrig] korrekt. Eine absolute `CLOCK_MONOTONIC`-Deadline mit `pthread_cond_timedwait` ist die (P)-Zeile "condattr_setclock" (Risiko R4), kein Typproblem. | Helfer aus Nr. 9. | R5 |
| 37 | src/runtime_eventflag.c:136-138 | `start_ns=(uint64_t)deadline.tv_sec*1000000000+…; deadline.tv_sec=(time_t)(total/1e9); deadline.tv_nsec=(long)(total%1e9)` | c,e | [niedrig] korrekt (siehe Nr. 36). | Helfer aus Nr. 9. | R5 |
| 38 | src/runtime_audio.c:55-56 | `struct timespec t={(time_t)(deadline/1000000000u),(long)(deadline%1000000000u)}; clock_nanosleep(CLOCK_MONOTONIC,TIMER_ABSTIME,&t,NULL)` | c,e | [mittel] Typen korrekt; `clock_nanosleep` und `TIMER_ABSTIME` gibt es in MinGW/winpthreads nicht. | `pf_sleep_until_ns(uint64_t)` (Windows: Waitable Timer, Linux: bisheriger Aufruf). | R4 |
| 39 | src/runtime_services.c:161 | `struct timespec t={timeout/1000000,(timeout%1000000)*1000}; nanosleep(&t,NULL)` | c | [niedrig] Positionsinitialisierung von `{time_t,long}` aus 64-Bit-Ausdrücken: Verengung auf 32 Bit bei `tv_nsec`, Wert < 1e9. | Helfer aus Nr. 9. | R5 |
| 40 | src/runtime_file.c:24, src/runtime_kernel.c:29-30, 139, src/runtime_mutex.c:201, src/runtime_rwlock.c:23, src/runtime_time.c:9-10 | `GuestTimespec` (3x), `GuestTime` (1x), `GuestTimeval` (2x), `GuestTimezone` (2x) | a | [niedrig] Alle richtig (`int64_t`/`int32_t`), aber mehrfach definiert und ohne `_Static_assert`. Eine spätere Änderung an einer Kopie bliebe unbemerkt. | Eine Datei `src/guest_types.h` (Opus entscheidet): `GuestTimespec`, `GuestTimeval`, `GuestTimezone`, `GuestStat` je mit `_Static_assert(sizeof/offsetof)`; alle Module nutzen sie. | R1, R2, R5 |
| 41 | gpu/shim/bbport_guest_hooks.cpp:157; gpu/shim/bbport_gnm_hooks.cpp:48, 532 | `const long page_size = sysconf(_SC_PAGESIZE);` bzw. `std::size_t(sysconf(_SC_PAGESIZE))` | e | [niedrig] `long` reicht für 4096; `sysconf` fehlt unter Windows. | `const uint64_t page_size = pf_page_size();`. | S1 (guest_hooks), S2 (gnm_hooks) |
| 42 | gpu/shim/bbport_free_check.cpp:273-275, 680-681, 690-693, 806-807; bbport_guest_hooks.cpp:152-153 | `iovec local{…}, remote{…}; process_vm_readv(getpid(),&local,1,&remote,1,0) == ssize_t(PageSize)` | c | [mittel] Linux-only; `iovec`-Längen sind `size_t`, Vergleich mit `ssize_t(…)`. Windows `ReadProcessMemory` liefert `SIZE_T` über einen Zeiger. | `pf_read_memory_safe(dst, src, size) == int64_t(size)` (pf_memory.h:121, schon `int64_t`). | S1 |
| 43 | gpu/shim/bbport_clobber.cpp:109; bbport_free_check.cpp:116, 684; bbport_write_log.cpp:60; bbport_threads.h:30 | `static_cast<std::uint32_t>(gettid())`, `u32(syscall(SYS_gettid))`, `setpriority(PRIO_PROCESS, static_cast<id_t>(gettid()), 19)` | c | [niedrig] `pid_t`/`long`-Rückgabe von `syscall`; TIDs passen in 32 Bit, Windows-Thread-IDs (`DWORD`) auch. | `u32(pf_thread_id())` (`uint64_t`, pf_thread.h:39); `setpriority` über eine pf-Funktion für Priorität. | S1, S2 |
| 44 | gpu/shim/bbport_wait_trace.cpp:49-54 | `snprintf(text,…,"%s+%#lx", info.dli_sname, (unsigned long)(static_cast<char*>(address) - static_cast<char*>(info.dli_saddr)))` | d,e | [mittel] wie Nr. 15 (Zeigerdifferenz auf 32 Bit abgeschnitten). | `"%s+%#" PRIx64`, `uint64_t(…)`; Symbolauflösung über `pf_symbolize`. | S2 |
| 45 | gpu/shim/bbport_guest_memory.cpp:190-191, 203 (Rückgabe `int fd`: Zeilen 98-121) | `mmap(nullptr, Size, …, MAP_SHARED, fd, 0)`; `mmap(…, 4096, …, fd, Offset)` | b,c | [hoch] `off_t Offset` bei `mmap`, und der Chunk-Allokator gibt `int` zurück (Gegenstück zu Nr. 31). Windows: kein dma-buf (D12). | `pf_handle_map(handle, uint64_t offset, …)` (pf_memory.h:91); Allokator gibt `PfMemHandle` (`intptr_t`), `-1` wird `PF_MEM_HANDLE_NONE`. Unter Windows "nicht unterstützt". | V3 |
| 46 | gpu/shadps4/video_core/amdgpu/liverpool.cpp:1052-1054, 2342-2343, 2395-2396; video_core/page_manager.cpp:112-124 | `iovec local{…}, remote{…}; process_vm_readv(…) == ssize_t(sizeof(u32))`, `const ssize_t got=…; for (ssize_t i=0;i<got/8;++i)` | c | [mittel] wie Nr. 42. | `pf_read_memory_safe`. | V2 (liverpool), V1 (page_manager) |
| 47 | gpu/shadps4/video_core/page_manager.cpp:104-106, 112, 122, 299 | `static_cast<const ucontext_t*>(context)->uc_mcontext.gregs; u64(g[REG_RIP]); reinterpret_cast<void*>(g[REG_RBP])` | c,e | [niedrig] `greg_t` ist unter Linux `long long`; Linux-only, gehört zu `PfContext`. | `PfContext`-Felder `uint64_t` (pf_exception.h) benutzen. | V1 |
| 48 | gpu/shadps4/video_core/amdgpu/liverpool.cpp:123-128, 1810-1816; gpu/shadps4/core/libraries/videoout/driver.cpp:414-416, 427-429 | `pthread_getcpuclockid(…, &clock)`; `BbStats::gpu_thread_clock` als `int`; `clock_gettime(static_cast<clockid_t>(clock), &ts); u64(ts.tv_sec)*1000000000ull+u64(ts.tv_nsec)`; `getrusage(RUSAGE_THREAD,&usage); u64(usage.ru_utime.tv_sec)*1000000 + usage.ru_utime.tv_usec; …ru_nivcsw; …ru_minflt` | c | [mittel] Linux-API. Die Wandlungen sind korrekt (zuerst auf `u64`). Die `rusage`-Zähler (`ru_nivcsw`, `ru_minflt`) sind `long`; Windows kennt sie nicht. | `pf_thread_cpu_time_ns(uint64_t*)` (existiert) ersetzt `getcpuclockid`+`clock_gettime`; Kontextwechsel-/Fault-Zähler hinter `pf_process_*` ("nicht verfügbar" = 0). | V2 |
| 49 | gpu/shadps4/video_core/renderer_vulkan/vk_scheduler.cpp:302-305, 337-340 | `printf("… %llu x %s+0x%lx\n", …, static_cast<unsigned long>(reinterpret_cast<uintptr_t>(top[i].second) - reinterpret_cast<uintptr_t>(info.dli_fbase)))` | d,e | [mittel] wie Nr. 15; dazu `dladdr` (nur Linux). | `PRIx64`/`uint64_t` und `pf_symbolize`. | V2 |
| 50 | gpu/shadps4/video_core/renderer_vulkan/vk_instance.cpp:777 | `VkDeviceSize(env ? std::max(1ul, std::strtoul(env, nullptr, 10)) : 64ul) << 20` | e | [niedrig] `1ul`/`64ul`/`strtoul` sind 32 Bit; das Ergebnis wird erst danach auf 64 Bit erweitert. Werte bis 4095 (MiB) richtig, darüber stilles Abschneiden. Übersetzt auf beiden Plattformen. | `std::max<u64>(1, std::strtoull(env, nullptr, 10))` und `64ull`. | V3 |
| 51 | gpu/shadps4/video_core/renderer_vulkan/scene_resolution.h:17-18 | `std::max(1l, std::lround(output.width / scale / 2)) * 2` | e | [niedrig] `1l`/`lround` sind `long` (32 Bit); Bildgrößen liegen unter 2^31, korrekt. Beide Argumente haben denselben Typ, `std::max` übersetzt auf beiden. | `std::max<int64_t>(1, std::llround(…))`. | G3 |
| 52 | gpu/shim/core/emulator_settings.h:18-20, 38, 46, 53 | `static long Number(const char*, long)`, `std::strtol`, `const long hz`, `std::max(0L, Number("BB_FPS_LIMIT", 0))` | b,e | [niedrig] `long` 32 Bit; die Werte (Auflösung, Hz, GPU-Id, FPS) sind klein. Übersetzt auf beiden. Die Datei gehört keinem Paket. | `static int64_t Number(const char*, int64_t)` mit `std::strtoll`; `std::max<int64_t>(0, …)`. | ohne Paket (Opus) |
| 53 | gpu/shim/window.cpp:30 | `const long number = std::strtol(wanted, &end, 10);` (`BB_DISPLAY`) | e | [ok] kleiner Wert. (Nebenbei: `strcasestr`, Zeile 38, fehlt unter MinGW, kein LLP64-Thema.) | optional `strtoll`. | S3 |
| 54 | gpu/shim/bbport_free_check.cpp:971, 1009, 1056 u. a.; bbport_clobber.cpp:140-171; bbport_gnm_hooks.cpp:237-420; bbport_heap_sites.cpp:237-351; bbport_write_log.cpp:95-177; bbport_timeline.h:69-73 | `std::fprintf(…, "%#llx", (unsigned long long)x)` (135 Zeilen) | d | [ok] richtig auf beiden Plattformen. | Beibehalten; Muster für Regel 5 (Abschnitt 7). | S1, S2 |

## 4. Geprüft, ohne Befund

Ausdrücklich kontrolliert und in Ordnung (damit sie nicht erneut gesucht werden):

| Stelle | Prüfung |
|---|---|
| `src/runtime_file.c:25-35` `GuestStat` | nur `uint32_t/uint16_t/int64_t/GuestTimespec{int64,int64}`, `_Static_assert(sizeof==120)`. |
| `src/runtime_file.c:499-523` `do_getdents` | FreeBSD-`dirent` von Hand gebaut (`memcpy` in `uint32_t ino`, `uint16_t reclen`, `p[6]`, `p[7]`), kein Hosttyp beteiligt. `d_type`/`DT_*` bei `readdir` (Zeilen 65, 195, 201) braucht die Gap-Schicht (INTERFACES §2, (P)). |
| `src/runtime_kernel.c:139, 178, 200` `GuestTimezone`, `GuestSigset`, `GuestRusage` | feste Typen (`int32_t`, `uint32_t[4]`, `int64_t[14]`). |
| `src/runtime_time.c:9-18` `GuestTimeval`, `guest_gettimeofday` | feldweise Zuweisung `value->seconds=t.tv_sec` (`time_t` nach `int64_t`), `microseconds=t.tv_usec` (`long` nach `int64_t`). |
| `src/runtime_thread.c:27-45` `ThreadAttr`, `GuestThread` | Hostobjekte; der Gast sieht nur den Zeiger. Enthalten `jmp_buf` (Host, pf_jmp) und `pthread_t` (winpthreads: `uintptr_t`). |
| `src/runtime_thread.c:332-345` `GuestSchedParam`, `posix()` | `int32_t`. |
| `src/runtime_pad.c:42-67` `PadData`, `PadTouch`, `ControllerInfo` | feste Typen und `float`, `_Static_assert` auf Größe/Offset. |
| `src/runtime_ajm.c:47-56`, `runtime_audio.c:45-46`, `runtime_memory.c:47-55, 611`, `runtime_rtc.c:21-22`, `runtime_savedata.c:37-62`, `runtime_json2.c:32`, `runtime_services.c:228, 389, 404` | feste Typen, Größen per `_Static_assert` gesichert (soweit vorhanden). |
| ABI-Signaturen in `src/` | Zeilen mit `ABI` und Typen `long/time_t/off_t/ssize_t/wchar_t/clock_t/bool/float/double/short`: **0 Treffer** (nur `unsigned char *` für Bytepuffer). Rückgabetypen: `int32_t/int64_t/uint64_t/uint32_t/void*/uintptr_t/size_t`. |
| `gpu/bbgpu.h` | `uint32_t/int32_t/uint64_t/unsigned/int`, Zeiger. Kein Befund. |
| `gpu/shim/bbgpu.cpp:217-228` `PS4_SYSV_ABI` | `u64/u32/s32/s32*`. |
| Bitfelder in Gaststrukturen | keine (nur ein Kommentar `ident:6 payload:20` in `runtime_ajm.c:47`). Damit entfällt die Frage MS- gegen Itanium-Bitfeldlayout. |
| `__builtin_*l`, `sizeof(long)`, `va_arg` | keine Treffer in `src/`, `gpu/shim`. |
| Literal-Suffix `UL`/`L` in `src/`, `gpu/shim` | keine; durchgängig `ull`/`ULL`/`UINT64_C`. |
| `strtoul`/`strtol` in `src/probe.c:241, 355` | kleine Werte (TID, Sekunden). |
| `u32(std::strtoul)` in `liverpool.cpp:413-415`, `buffer_cache.cpp:1362`, `vk_pipeline_cache.cpp:562`, `vk_rasterizer.cpp:695` | Ergebnis ohnehin `u32`. |
| Zeitdifferenzen `tv_sec`/`tv_nsec` (Nr. 11) | rechnen im 64-Bit-Bereich. |
| `%zu`-, `%lld`-, `%llu`-Formate in geänderten vendored Zeilen (z. B. `vk_frame_capture.cpp:109,125`, `vk_scheduler.cpp:144`, `buffer_cache.cpp:767,1861`) | auf UCRT/clang-mingw richtig; die Typprüfung übernimmt `-Wformat`. |
| `pf_memory.h`, `pf_thread.h`, `pf_process.h` (Schnittstellen IF-2) | Adressen `uintptr_t`, Größen/Offsets `uint64_t`, `pf_read_memory_safe` liefert `int64_t`, `PfMemHandle` ist `intptr_t`, `pf_thread_id` liefert `uint64_t`. Typen stimmen mit den Regeln aus Abschnitt 7 überein. |

## 5. Zusammenfassung

### 5.1 Je Kategorie

Zählung über die Tabellenzeilen 1-54 in Abschnitt 3 (Mehrfachnennung möglich). "Ohne [ok]" lässt die sieben Zeilen
8, 10, 11, 23, 29, 53, 54 weg.

| Kat. | Bedeutung | Zeilen gesamt | ohne [ok]-Zeilen |
|---|---|---|---|
| a | Teil einer Gaststruktur | 7 | 5 |
| b | `ABI`-Signatur bzw. API-Parameter | 12 | 11 |
| c | Kopie/Cast Host-Gast | 30 | 27 |
| d | Formatstring `%l*` | 12 | 10 |
| e | Literal-Suffix / Zeiger-Cast | 22 | 21 |

Nach Schwere (54 Zeilen): **[hoch] 6** (Nr. 12, 21, 22, 31, 32, 45; drei Ursachen: `tm_gmtoff`, `struct stat`, `int` als Handle),
**[mittel] 19**, **[niedrig] 22**, **[ok] 7**.
Alle [hoch]-Befunde sind Übersetzungsfehler oder Typverluste an der Grenze zu Windows-APIs. Ein Fehler in einer
Gaststruktur selbst wurde nicht gefunden.

### 5.2 Je Paket

| Paket | Nr. | Zeilen | Schwerpunkt |
|---|---|---|---|
| K2 | 30, 31 | 2 | `off_t` nur in Linux-Code; `int fd` nach `PfMemHandle` |
| K3 | 1-6 | 6 | `%lx` + `(unsigned long)` in Fehlerberichten, `/proc/maps`-Parser, `syscall`-`long` |
| K5 | 7, 8 | 2 | `(unsigned long)base` bei `arch_prctl` (zieht nach `pf_tls_linux.c`) |
| R1 | 9-20 | 12 | `tm_gmtoff`, `getrusage`, `getrandom`, `%lx`-Abschneiden (15-18), Uhren-Konstanten |
| R2 | 21-29 | 9 | `struct stat`, `read/write` mit `unsigned int`, `lseek/ftruncate/fseek` |
| R4 | 32, 38 | 2 | `st_mtim`, `clock_nanosleep` |
| R5 | 33-37, 39 (+12) | 6 (+1) | `struct timespec`-Initialisierung (korrekt, Helfer empfohlen), `tm_gmtoff` |
| R1/R2/R5 | 40 | 1 | `Guest*`-Typen vereinheitlichen |
| S1 | 41, 42, 43, 54 | 4 | `sysconf`, `process_vm_readv`, `gettid` |
| S2 | 41, 43, 44, 54 | 4 | `sysconf`, `gettid`, `%lx` |
| S3 | 53 | 1 | nur Prüfstelle |
| V1 | 46, 47 | 2 | `process_vm_readv`, `ucontext_t` |
| V2 | 46, 48, 49 | 3 | `rusage`, `clock_gettime`, `%lx` |
| V3 | 45, 50 | 2 | `mmap(…, off_t)`/`int fd`, `strtoul` |
| G3 | 51 | 1 | `lround`/`1l` |
| ohne Paket | 52 | 1 | `gpu/shim/core/emulator_settings.h` |

### 5.3 Die fünf wichtigsten Befunde

1. **Nr. 21, 22, 32:** `struct stat` ist nicht portierbar (`st_atim/st_mtim`, `st_blocks`, `st_blksize` fehlen, `st_ino` 16 Bit, `st_size`-Breite hängt an der Variante). Lösung ist eine eigene `PfStat` in `pf_fs.h`, die es heute noch nicht gibt.
2. **Nr. 12:** `tm_gmtoff` fehlt in `struct tm` (fünf Stellen in R1/R5), `localtime_r` ist nicht zugesichert. Neue Funktion `pf_utc_offset_seconds` nötig.
3. **Nr. 31, 45:** Der Chunk-Allokator-Rückruf und `Region.fd` sind `int`; ein Windows-`HANDLE` ist 64 Bit. Auf `PfMemHandle` (`intptr_t`) umstellen.
4. **Nr. 1, 2, 4, 15-18, 44, 49:** `%lx`/`%#lx` mit `(unsigned long)`-Cast auf 64-Bit-Werte (Host-Adressen, Site-Nummern) schneidet unter Windows still ab; `-Wformat` meldet es nicht, weil Cast und Format zusammenpassen. 9 Tabellenzeilen, Lösung `PRIx64`.
5. **Nr. 24, 25:** `read/write/pread/pwrite` mit `uint64_t size` laufen unter UCRT über `unsigned int`/`int`; `lseek/ftruncate` hängen am Makro `_FILE_OFFSET_BITS=64`. Die Gap-Schicht muss in 1-GiB-Blöcken arbeiten und `sizeof(off_t)==8` per `_Static_assert` absichern.

## 6. Hinweise zu nicht geänderten vendored Dateien (G2/G3, nicht Teil dieses Audits)

Stichprobe im unveränderten Upstream-Code unter `gpu/shadps4`:

- `core/libraries/kernel/time.h:43, 80, 83` hat `time_t` in `PS4_SYSV_ABI`-Funktionen (`sceKernelConvertLocaltimeToUtc(time_t, int64_t, time_t*, …)`). Unter UCRT ist `time_t` 64 Bit, also gleich dem Gast. Gilt nur mit `sizeof(time_t)==8` (kein `_USE_32BIT_TIME_T`): `_Static_assert` in der Gap-Schicht.
- Die Gast-Zeitstrukturen im Upstream (`OrbisKernelTimeval`, `OrbisKernelTimespec`, `time.h:20-30`) wurden nur auf ihr Vorhandensein geprüft, nicht auf ihre Feldtypen.
- `common/aes.h` (Zeilen 365-950) benutzt `unsigned long` für Größen, rein hostintern, nicht gastsichtbar; `common/memory_patcher.cpp:92` und `renderdoc.cpp:47` haben `wchar_t` (hostintern, Windows-Zweig). Kein Befund für den Gast.
- Der upstream-eigene Windows-Zweig (`ntapi`, `signal_context`, `address_space`) ist laut D1 für MinGW bereits geflickt und wurde hier nicht geprüft.

## 7. Zur Übernahme in die Gap-Schicht und in die Konventionen (Vorschlag für INTERFACES.md)

Allgemeine Regeln, die Opus aufnehmen könnte:

1. **Gaststrukturen nur mit `<stdint.h>`-Typen.** Kein `long`, `unsigned long`, `time_t`, `off_t`, `wchar_t`, `bool`, keine `enum`-Felder, keine Bitfelder, keine Hoststrukturen (`struct timespec/timeval/stat/dirent/tm/rusage`). `size_t`/Zeiger nur, wenn der Gast ebenfalls 64-Bit-Zeiger meint.
2. **Jede Gaststruktur hat `_Static_assert(sizeof(T)==N)` und, wo Felder nicht trivial folgen, `offsetof`-Asserts.** Gast-Zeit- und Stat-Typen stehen genau einmal in `src/guest_types.h` (Nr. 40).
3. **Host-Gast-Kopien nur feldweise**, mit expliziter Weitung (`(int64_t)`/`(uint64_t)`), nie `memcpy` zwischen Host- und Gasttyp. Umgekehrt (Gast nach Host) mit Bereichsprüfung vor der Verengung (`nsec` in `[0, 1e9)`, wie schon in `runtime_mutex.c:203`, `runtime_rwlock.c:97`).
4. **`ABI`-Funktionen: Parameter und Rückgabe nur `intN_t/uintN_t`, `int`, `unsigned`, `size_t`, `uintptr_t`, Zeiger auf Gasttypen.** Ein `int`-Parameter, den der Gast in einem 64-Bit-Register übergibt, liest die unteren 32 Bit (SysV); das ist auf beiden Plattformen gleich und erlaubt. `long` ist verboten (der Host läse unter LLP64 dieselben 32 Bit, der Gast meinte aber 64).
5. **Formatstrings:** 64-Bit-Werte immer `"%" PRIu64`/`PRIx64`/`PRId64` (`<inttypes.h>`) oder `%llu/%llx` mit `(unsigned long long)`-Cast. **Nie** `%lu/%lx/%ld` und **nie** `(unsigned long)` als Cast für Adressen oder Offsets. Zeiger mit `%p`, `size_t` mit `%zu`.
6. **Keine Literal-Suffixe `UL`/`L`.** Konstanten über 32 Bit: `UINT64_C(…)` oder `ULL`. Umgebungsvariablen mit `strtoull/strtoll` und Zielwerten `uint64_t/int64_t`.
7. **Zeiger und Ganzzahl nur über `uintptr_t`/`intptr_t`** (nie `long`, nie `int`). Handles, die zwischen Schichten wandern, sind `PfMemHandle` (`intptr_t`), nicht `int`.
8. **Zeit:** Absolute und relative Zeiten im gemeinsamen Code als `uint64_t` Nanosekunden. Die Umwandlung in `struct timespec` geschieht an einer Stelle (`pf_ns_to_timespec`, Nr. 9), nicht mit `(time_t)`/`(long)`-Casts verteilt.
9. **Dateien:** Offsets und Größen sind `int64_t`/`uint64_t`; `off_t` nur über die POSIX-Teilmenge und nur nach `_Static_assert(sizeof(off_t)==8)` in der Gap-Schicht. `fseek/ftell` (`long`) durch `fseeko/ftello` ersetzen. `struct stat` nur in `pf_fs_*` (Vorschlag `PfStat`).
10. **Windows-APIs mit 32-Bit-Längen** (`ReadFile/WriteFile` `DWORD`, `BCryptGenRandom` `ULONG`, `_read/_write` `unsigned int`, `WideCharToMultiByte` `int`): jede Gap- oder `pf_*`-Implementierung schleift in Blöcken ≤ 1 GiB.
11. **Gap-Header `pf_posix_gap_win32.h` zusätzlich:** `_Static_assert(sizeof(off_t)==8)`, `_Static_assert(sizeof(time_t)==8)`, `_Static_assert(sizeof(void*)==8)`; Fehler, falls `_USE_32BIT_TIME_T` definiert ist. Compilerflag `-Wshorten-64-to-32` (clang) für `src/` im Windows-Build: es zeigt Verengungen wie Nr. 24, 27, 28 und die meisten `long`-Zuweisungen sofort.
12. **Unittest `tests/refueled/test_llp64.c`** (beide Plattformen, F6-nah): `sizeof` aller `Guest*`-Typen, `offsetof` der Zeitfelder, `sizeof(off_t/time_t/size_t/uintptr_t)`, `sizeof(((struct stat*)0)->st_size)`, eine Rundreise `PfStat` nach `GuestStat` mit Werten über 2^32 in Größe und Zeitstempel.

Neue Schnittstellen, die aus den Funden folgen (alle Entscheidung Opus, ARCHITECTURE.md §2 Regel 4):

| Vorschlag | Aus Nr. | Header |
|---|---|---|
| `PfStat` + `pf_stat/pf_fstat/pf_is_dir/pf_mtime_ns` | 21, 22, 32 | `pf_fs.h` |
| `int64_t pf_utc_offset_seconds(int64_t utc, int *is_dst)` | 12 | `pf_process.h` oder `pf_base.h` |
| `pf_ns_to_timespec(uint64_t, struct timespec*)` (inline) | 9, 33-39 | `pf_base.h` |
| `pf_sleep_until_ns(uint64_t)` | 38 | `pf_thread.h` |
| `pf_clock_ns(PfClock)` (mono, real, thread-CPU, process-CPU) | 20, 48 | `pf_thread.h` oder `pf_base.h` |
| `pf_random(void*, uint64_t)` | 14 | `pf_base.h` |
| `src/guest_types.h` | 40 | neu |
| `PfMemHandle` auch im Allokator-Rückruf | 31, 45 | `runtime.h` / `bbport_guest_memory.*` |

## 8. Offene Fragen an Opus

1. `struct stat` unter MinGW mit `_FILE_OFFSET_BITS=64`: wird `stat` zu `_stat64` (64-Bit-`st_size`, 64-Bit-Zeiten) oder `_stat64i32`? Das war ohne Windows-Build nicht zu belegen (die MinGW-Header liegen nicht im Repo). Der Unittest aus Abschnitt 7 Nr. 12 klärt es. Nr. 21/22 bleiben davon unabhängig [hoch], und `PfStat` ist in beiden Fällen die Lösung.
2. `tm_gmtoff` lässt sich in der Gap-Schicht nicht nachrüsten (es ist ein Strukturfeld), darum der Vorschlag einer `pf_*`-Funktion (Nr. 12). Einverstanden?
3. `GuestStat`-Felder ohne Windows-Entsprechung (`uid/gid/rdev/flags/gen`, Rechtebits `07777`): Werte festlegen (Vorschlag 0, Rechte `0644` bzw. `0755`)? Das beeinflusst, ob das Spiel Dateien als schreibbar erkennt.
4. Gehört `gpu/shim/core/emulator_settings.h` (Nr. 52) einem Paket? In TASKS.md steht sie nirgends.
5. `gpu/shadps4`: 118 Dateien weichen von b4e7ae7 ab (viele Neuanlagen, darunter FSR/Upscaler). Gescannt wurden nur die hinzugefügten Zeilen. Reicht das, oder sollen G2/G3 den vollen Upstream-Bestand von `core/libraries/**` (der einzige gastsichtbare Teil) auf `long` in Gast-ABI durchsehen?
6. `-Wshorten-64-to-32` für `src/` einschalten (Regel 11)? Es erzeugt auch unter Linux Warnungen und wäre ein Eingriff in `build.sh` (F4). Alternative: nur im Windows-Build (F3).
7. winpthreads: `pthread_cond_timedwait` mit absoluter Zeit auf `CLOCK_MONOTONIC` (`runtime_sema.c:73`, `runtime_eventflag.c:39`) ist kein Typproblem, aber die `timespec`-Rechnung (Nr. 36, 37) setzt voraus, dass `pthread_condattr_setclock` dort wirkt (Risiko R4 in ARCHITECTURE.md §6). Bleibt bei F6.

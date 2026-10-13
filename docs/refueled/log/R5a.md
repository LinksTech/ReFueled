# R5a – übrige Runtime-Dateien (Low-Heap, LLP64, Linux-only-Reste)

Ausgeführt von Sonnet, zu prüfen von Opus. Ungebaut bis CLANG64. Nicht committet.
Geändert: `src/runtime.c`, `src/runtime_mutex.c`, `src/runtime_rwlock.c`, dieses Log.
`src/runtime.h` bindet `platform/pf_memory.h` schon ein (pf_low_* dort deklariert), keine neuen Includes nötig.

## 1. Low-Heap-A-Stellen (13 Zeilen, 5 Allokationen, 8 free)

pf_lowheap_linux.c: `pf_low_calloc` = `calloc(count,size)`, `pf_low_free` = `free(p)`.
malloc -> `pf_low_calloc(1,n)` ist unter Linux "nicht genullt" -> "genullt". Unbedenklich, weil jede
Stelle den Speicher danach vollständig initialisiert bzw. er sofort durch pthread_*_init
überschrieben wird (Details unten). Keine Rückfrage nötig.

| alt (Datei:Zeile, Code) | neu | Linux-Gleichheit |
|---|---|---|
| runtime_mutex.c:30 `malloc(sizeof(*attr))` (GuestAttr = `{int type}`) | `pf_low_calloc(1, sizeof(*attr))` | pf_lowheap_linux.c: calloc; Zeile 32 setzt `type` ohnehin, einziges Feld, Nullung ohne Wirkung; gleiche Fehlerpfade (NULL -> ENOMEM) |
| runtime_mutex.c:45 `free(*attr)` | `pf_low_free(*attr)` | = `free` |
| runtime_mutex.c:52 `malloc(sizeof(*mutex))` (GuestMutex = pthread_mutex_t) | `pf_low_calloc(1, sizeof(*mutex))` | calloc; `pthread_mutex_init` (Z. 59) überschreibt das ganze Objekt, vorherige Nullung ohne Wirkung (glibc-Init setzt alle Felder; Nullbytes entsprechen sogar PTHREAD_MUTEX_INITIALIZER) |
| runtime_mutex.c:56 `free(mutex)` | `pf_low_free(mutex)` | = `free`, Fehlerpfad von `pthread_mutexattr_init` |
| runtime_mutex.c:61 `free(mutex)` | `pf_low_free(mutex)` | = `free`, Fehlerpfad von `pthread_mutex_init` |
| runtime_mutex.c:112 `free(*mutex)` (mutex_destroy) | `pf_low_free(*mutex)` | = `free`; Zeiger stammt aus Z. 52 |
| runtime_mutex.c:139 `malloc(sizeof(*c))` (GuestCond) | `pf_low_calloc(1, sizeof(*c))` | calloc; `pthread_cond_init` (Z. 141) initialisiert alles |
| runtime_mutex.c:142 `free(c)` | `pf_low_free(c)` | = `free` |
| runtime_mutex.c:162 `free(*cond)` (cond_destroy) | `pf_low_free(*cond)` | = `free`; Zeiger stammt aus Z. 139 |
| runtime_rwlock.c:48 `calloc(1,sizeof(*r))` | `pf_low_calloc(1,sizeof(*r))` | = calloc, identisch |
| runtime_rwlock.c:51 `free(r)` | `pf_low_free(r)` | = `free` (Fehlerpfad pthread_rwlock_init) |
| runtime_rwlock.c:142 `free(r)` (rw_destroy) | `pf_low_free(r)` | = `free`; Zeiger stammt aus Z. 48 |
| runtime.c:64 `calloc(1,tls_modules[..].memsz)` | `pf_low_calloc(1,tls_modules[..].memsz)` | = calloc, identisch; Block wird nie freigegeben (kein free vorhanden, Audit A9) |

Alle Freigaben der umgestellten Objekte per grep geprüft: es gibt keine weitere `free`-Stelle für
GuestAttr/GuestMutex/GuestCond/Rwlock/TLS-Block in diesen Dateien.
Bewusst NICHT umgestellt (Audit C, host-intern): runtime_rwlock.c:67/87 (`Holder`, calloc/free bleiben
zusammen), runtime.c:115/289/295 (realloc/malloc Exit-Handler, Import-Namen), runtime_sema.c:47/50/98/154,
runtime_eventflag.c:71/92 (Handle ist ID), vulkan_smoke.c:15/18/21/26 (Host-Diagnose).

B-Zeilen (Statics, nur beschrieben, unverändert; Laufzeitprüfung `< 1 TiB` ist Sache von Opus):
- runtime.c:22 `stack_canary`, :207-211 `need_libc_internal`, `progname`: static, Adresse via `runtime_resolve(kind==2)` im GOT.
- runtime_services.c:116/119 `static int32_t net_errno`: `sceNetErrnoLoc` gibt `&net_errno` zurück.
- runtime_json2.c:23-28 `memallocator_vtable[]`: `&vtable[2]` wird ins Gastobjekt geschrieben.

## 2. LLP64 (alle Befunde in diesen Dateien)

Keine Quelländerung nötig; Begründung je Stelle:
| Stelle | Befund (LLP64.md) | Entscheidung |
|---|---|---|
| runtime_mutex.c:119 `end->tv_nsec=(long)(ns%1000000000)` (Nr. 33) | korrekt, Wert < 1e9 | unverändert (Helfer `pf_ns_to_timespec` wäre Schnittstelle, gehört Opus/R1) |
| runtime_mutex.c:204 `.tv_sec=(time_t)abs->sec,.tv_nsec=(long)abs->nsec` (Nr. 34) | korrekt, nsec in Z. 203 auf [0,1e9) geprüft | unverändert |
| runtime_rwlock.c:99 (Nr. 35) | wie 34, Prüfung Z. 97 | unverändert |
| runtime_sema.c:81 (Nr. 36) | korrekt | unverändert |
| runtime_eventflag.c:138 (Nr. 37) | korrekt | unverändert |
| runtime_services.c:161 `struct timespec t={timeout/1000000,(timeout%1000000)*1000}` (Nr. 39) | Verengung nur bei tv_nsec, Wert < 1e9 | unverändert; nach Einführung des Helfers `pf_ns_to_timespec` (Opus) umstellen |
| `%llx`/`%llu` mit `(unsigned long long)` in runtime.c:36/41, runtime_json2.c:21/40 | richtig auf beiden Plattformen (Audit "ok") | unverändert |
| `%zu` mit size_t (runtime.c:88-103, mutex/rwlock/sema/eventflag Statistik) | richtig | unverändert |
| `%lx`, `(unsigned long)`-Zeigercasts, `long`-ABI-Parameter | in diesen Dateien keine Treffer (grep) | – |
| `GuestTimespec` (runtime_mutex.c:201), `GuestTime` (runtime_rwlock.c:23), `GuestTimeval/GuestTimezone` (runtime_time.c:9-10) mehrfach definiert (Nr. 40) | Typen richtig, Zusammenführung in `guest_types.h` ist Opus-Entscheidung | unverändert |

Linux-Ausgabe unverändert, da keine Format- oder Typänderung erfolgt.

## 3. Schnittstellenbedarf

- `pf_utc_offset_seconds(int64_t utc_seconds, int *is_dst)` (pf_process.h, LLP64 Nr. 12) für
  `tm_gmtoff` an:
  - src/runtime_rtc.c:74 `return (int64_t)local.tm_gmtoff*1000000;` (in der Funktion davor `localtime_r`, Z. ~71)
  - src/runtime_services.c:98 `localtime_r(&now,&t); *value=(int32_t)(t.tm_gmtoff/60);`
  Nicht gelöst (Auftrag).
- `pf_ns_to_timespec(uint64_t ns, struct timespec*)` (Nr. 9), optional; Stellen: runtime_mutex.c:119, runtime_sema.c:81, runtime_eventflag.c:138, runtime_services.c:161.
- Absolute `CLOCK_MONOTONIC`-Deadlines mit `pthread_condattr_setclock` + `pthread_cond_timedwait` (runtime_sema.c:73, runtime_eventflag.c:39) hängen an winpthreads (Risiko R4), keine pf-Funktion vorhanden.

## 4. linux_hits.py

`python docs/refueled/tools/linux_hits.py --ref C:/nitro-ps4-ref/shadPS4-b4e7ae7 --list`, Treffer in R5a-Dateien
(alle `primary`; Kategorien nur `pthread`, `pthread-np`, `posix-time`, die unter MSYS2/winpthreads vorhanden sind):

| Datei | Kategorie | Zeilen |
|---|---|---|
| runtime.c | pthread | 10, 110, 112, 116, 120, 137, 140, 142 |
| runtime_mutex.c | pthread, posix-time | pthread: 8, 12, 54-60, 64, 71, 78, 84-111, 128, 134, 141-198; posix-time: 116 (`clock_gettime(CLOCK_REALTIME)`) |
| runtime_rwlock.c | pthread | 9, 13, 18, 24, 50-161 (pthread_rwlock_*, pthread_equal/self) |
| runtime_sema.c | pthread, pthread-np, posix-time | pthread: 9, 16, 27, 49-179; pthread-np: 73 (`pthread_condattr_setclock`); posix-time: 40, 73 |
| runtime_eventflag.c | pthread, pthread-np, posix-time | pthread: 12, 34-171; pthread-np: 39; posix-time: 39, 135, 153 |
| runtime_services.c | pthread, posix-time | pthread: 14, 39-85; posix-time: 161 (`nanosleep`) |
| runtime_rtc.c | posix-time | 66 (`clock_gettime(CLOCK_REALTIME)`) |
| runtime_time.c, runtime_content.c, runtime_json2.c, vulkan_smoke.c | – | keine Treffer |

Keine echten Linux-only-Aufrufe (epoll, mmap, /proc, signalfd, ...) in diesen Dateien. Für die
Treffer gibt es keine passende pf-Funktion (pf_thread/pf_process bieten kein Mutex/Cond/Rwlock/Clock-API);
sie bleiben bis zur Entscheidung über winpthreads (R4) unverändert. Nicht umgestellt, gemeldet.
Zusätzlicher Treffer-Kandidat, nicht von linux_hits gemeldet: `runtime.c:9 #include <sched.h>` (winpthreads liefert sched.h).

## 5. Prüfungen
- `git diff`: 3 Quelldateien, je Zeile 1:1 Austausch (13 Zeilen), keine Folgeänderungen.
- Nicht gebaut/kein Test (ungebaut bis CLANG64).

## 6. Nachtrag: tm_gmtoff -> pf_utc_offset_seconds (pf_time.h, pf_time_linux.c:15-21)
Linux-Implementierung: `localtime_r(&(time_t)utc_seconds)`, Rückgabe `(int64_t)tm_gmtoff`, also derselbe Aufruf wie vorher.
| alt | neu | Linux-Gleichheit |
|---|---|---|
| runtime_rtc.c:71-74 `time_t seconds=(time_t)((int64_t)(utc-EPOCH)/1000000); struct tm local; localtime_r(&seconds,&local); return (int64_t)local.tm_gmtoff*1000000;` | `const int64_t seconds=(int64_t)(utc-EPOCH)/1000000; return pf_utc_offset_seconds(seconds,NULL)*1000000;` | gleiche Sekundenzahl; pf_time_linux.c castet sie wieder auf time_t und ruft localtime_r; *1000000 wie zuvor |
| runtime_services.c:98 `time_t now=time(NULL); struct tm t; localtime_r(&now,&t); *value=(int32_t)(t.tm_gmtoff/60);` | `*value=(int32_t)(pf_utc_offset_seconds((int64_t)time(NULL),NULL)/60);` | gleiche Zeit an localtime_r, /60 und int32-Cast wie zuvor |
Beide Dateien binden `platform/pf_time.h` ein. linux_hits: runtime_rtc.c verliert keinen Treffer (Zeile 66 clock_gettime bleibt), die localtime_r-Stellen sind weg. Ungebaut.

## Review Opus
Linux-Gleichheit geprüft: calloc/free = pf_low_*, malloc→pf_low_calloc nur bei sofort vollständig initialisierten Objekten, tm_gmtoff über pf_utc_offset_seconds mit derselben Eingabezeit.

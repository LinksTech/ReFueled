# Paket LH – Low-Heap unter 1 TiB

Verhaltensabweichung Linux: keine (pf_low_* = calloc/aligned_alloc/free)

Stand 2026-10-13, ausgeführt von Sonnet, Prüfung und Commit durch Opus.

## Dateien (alle neu)
- `src/platform/pf_lowheap.h` / `pf_lowheap.c` – plattformneutraler Allokator auf festem Bereich
- `src/platform/pf_lowheap_linux.c` – `pf_low_*` = calloc / aligned_alloc / free, `pf_low_heap_init` = 0
- `src/platform/pf_lowheap_win32.c` – statischer `PfLowHeap`, Bereich per `pf_vm_alloc_fixed`
- `tests/refueled/test_lowheap.c`

## Verfahren
Blöcke mit 16-Byte-Kopf (Größe | belegt-Bit, Größe des Vorgängers). `free` verschmilzt beide
Nachbarn in O(1). Freie Blöcke in einer doppelt verketteten LIFO-Liste (Links im Nutzbereich,
Mindestblock 32 Byte), first fit. Ausrichtung >16: das Vorderstück wird als freier Block
abgespalten, wenn es mindestens ein Mindestblock ist, sonst rückt der Nutzzeiger zur nächsten
passenden Adresse. Rest hinten wird abgespalten, wenn mindestens ein Mindestblock bleibt. Kein
Wachstum, ein Mutex.

## Entscheidungen
- **Mutex ohne pthread.h im Header.** MSVC hat kein pthread.h; `PfLowHeap` enthält darum ein
  `_Alignas(16) unsigned char lock[64]`, `pf_lowheap.c` prüft per `_Static_assert`
  `sizeof(pthread_mutex_t) <= 64` und initialisiert den Mutex in `pf_lowheap_setup` (ein
  Null-Objekt ist bei winpthreads kein gültiger Mutex). Ein nicht eingerichteter Heap liefert NULL.
- **1-TiB-Prüfung** im Setup auf den Zahlen (base >= 2^40 oder size > 2^40-base), ohne Zugriff.
  Zeiger der Rückgaben liegen dadurch im Bereich; `pf_lowheap_contains` prüft den Nutzbereich.
- **Ausrichtung** nicht Potenz von 2, 0 oder >65536 gibt NULL; 1..15 wird auf 16 angehoben.
  `pf_low_calloc` richtet auf 16 aus, Größe 0 liefert einen gültigen Mindestblock.
- **Fehlerfälle von free** (fremder Zeiger, Doppel-free) brechen nach einer `pf_write_stderr`-Meldung ab.
- **Testbereich** liegt per mmap(MAP_FIXED_NOREPLACE) bei 0x3_0000_0000 (unter ASan: unter 2 GiB,
  weil dort das Schatten-Gap liegt), da aligned_alloc für 16 MiB oberhalb von 1 TiB landet und
  das Setup korrekt ablehnen würde.

## Prüfungen (WSL gcc 13, `-std=c11 -D_GNU_SOURCE -O2 -g -Wall -Wextra -Werror -pthread -Isrc`)
- mit `-fsanitize=address,undefined`: `test_lowheap: 128 checks passed`, Exit 0
- ohne Sanitizer: `test_lowheap: 128 checks passed`, Exit 0
- `pf_lowheap_win32.c`: `gcc -D_WIN32 -fsyntax-only -Werror` grün; MSVC
  `cl /c /std:c11 /W4 /WX /Isrc` grün
- `pf_write_stderr` kam im Test aus einer Wegwerf-Datei unter `C:\CTR Reload\tmp\refueled\LH\`
  (pf_log_linux.c gehört K8). Der Test linkt also erst mit K8 allein.

## Offen
- Windows-Lauf des Tests in CLANG64 (F3). Dafür braucht der Test auf Windows eine andere
  `make_area` (mmap fehlt; `pf_vm_alloc_fixed` bei 0x8_0000_0000 oder VirtualAlloc), und
  `test_linux_wrappers` entfällt dort (win32-Fassung braucht `pf_low_heap_init` mit echtem Bereich).
- `pf_lowheap.c` selbst wurde mit MSVC nicht übersetzt (pthread.h fehlt dort); gedacht für clang + winpthreads.
- K1 muss `pf_vm_alloc_fixed` / `pf_vm_free` liefern; `pf_low_heap_init` gibt `PF_EBUSY` beim zweiten Aufruf.

## Prüfung Opus 2026-10-13 (Pflichtfeld)
Feld bei der einmaligen Durchsicht aller Logs gesetzt und gegen die alt↔neu-Tabelle geprüft.

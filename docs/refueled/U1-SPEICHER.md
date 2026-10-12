# U1 – Speicher: Wie das Image bei 0x800000000 landet, und was Windows dafür braucht

Stand 2026-10-12, Basis `refueled/windows` = f1f7935 (Code identisch mit Tag v0.2.1).
Quellen: `src/probe.c`, `src/runtime_memory.c`, `src/runtime_thread.c`, `scripts/link_libc.py`,
`scripts/link_modules.py`, `run.sh`, Referenz shadPS4 b4e7ae7 `src/core/address_space.cpp`.

## 1. Antwort: Warum das Image bei 0x800000000 liegt

Das Image wird **nicht** mit fester Adresse geladen, sondern landet dort, weil es die **erste
Anforderung an den "low"-Bereich** ist:

1. `probe.c:allocate()` (Z. 88–101) fragt zuerst `runtime_low_map(size, RW)`. Erst wenn das NULL
   liefert, kommt der Rückfall `mmap(NULL, …)` (Z. 97), also eine beliebige Adresse.
2. `runtime_low_map` (`runtime_memory.c:672–685`) verteilt den Bereich
   `[LOW_MIN = 0x8_0000_0000, USER_MIN = 0x10_0000_0000)` von unten nach oben: `low_next` beginnt
   bei `LOW_MIN`. Jede Anforderung wird mit `mmap(low_next, …, MAP_FIXED_NOREPLACE)` versucht.
   Danach rückt `low_next` um `size + 16 KiB` weiter (die Lücke fängt Überläufe ab). Ist die Stelle
   belegt, wird die nächste versucht.
3. Vor dem Image (`probe.c:501`) ruft niemand `runtime_low_map` auf. `runtime_start`,
   `bbgpu_register_kernel` und das Lesen des Boot-Headers nutzen den normalen Heap. Darum bekommt
   das Image genau `0x800000000`. Danach folgen im selben Bereich die Import-Trampoline
   (`probe.c:520–521`), der Stack des Gast-Hauptthreads (8 MiB, `probe.c:605`) und die Stacks
   aller Gast-Threads (`runtime_thread.c:244`).
4. Das Image ist relozierbar. `scripts/link_*.py` linken mit Basis 0, `probe.c:536–551` wendet die
   Relokationen auf die tatsächliche Adresse an, und Patches werden umgerechnet (`probe.c:303`).
   Funktional ginge also jede Adresse unter 1 TiB.
5. **Aber:** Diagnose- und Hook-Code rechnet fest mit 0x800000000:
   `gpu/shim/bbport_guest_hooks.cpp:24` (`ImageBase`, int3-Hooks im Spielcode),
   `bbport_free_check.cpp:651`, `runtime_kernel.c:447/515/581/595` (Text-Bereich
   `0x800000000 + 0x50d96dc`). Landet das Image woanders, laufen diese Teile ins Leere.
   Für den Windows-Port heißt das: **0x800000000 ist Pflicht** (Meilenstein M1 prüft es).

Warum überhaupt unter 1 TiB? PS4-Code packt Zeiger in 40-Bit-Felder, und GPU-Deskriptoren
kodieren 40-Bit-Adressen (`runtime_memory.c:4–5`, `:670–671`). Aus demselben Grund sorgt
`probe.c:337–338` mit `mallopt(M_ARENA_MAX,1)` und `M_MMAP_THRESHOLD=32 MiB` zusammen mit `-no-pie`
(`build.sh:83`) dafür, dass Host-Heap-Objekte, die der Gast sieht, im brk-Heap unter 1 TiB bleiben:
Thread-Handles (`runtime_thread.c:81 calloc`), TCB/TLS-Blöcke (`:66 aligned_alloc`) und
Attribut-Objekte.

## 2. Benötigte feste Bereiche und Größen

| Bereich | Adresse / Größe | Herkunft | Inhalt |
|---|---|---|---|
| low | `0x8_0000_0000` – `0x10_0000_0000` (32 GiB) | `LOW_MIN`, `USER_MIN` | Image (≤ 512 MiB, `probe.c:412`; Text ≈ 81 MiB), Trampoline, Gast-Stacks (Haupt-Thread 8 MiB, sonst `attr.stack + STACK_MARGIN`) |
| user | `0x10_0000_0000` – `0xfc_0000_0000` (≈ 944 GiB) | `USER_MIN`, `USER_MAX` | alle Gast-Mappings: Direct, Flexible, Reservierungen (first fit ab Hint, `find_free`) |
| Backing-Objekt | `POOL_SIZE + FLEX_SPAN` | memfd `bb-guest-memory` | Direct Memory 5056 MiB (PS4 Pro / `BB_NEO=1`: 5568 MiB, `BB_DMEM_MB` bis 16384 MiB) + 1 GiB Flexible-Spanne (Budget 448 MiB) → **6,0 bis 17 GiB** |
| Backing-Sicht | beliebig, gleiche Größe | `pool()` Z. 138 | Host-Schreibzugriffe an Gast- und GPU-Schutz vorbei (`runtime_memory_write_backing`) |
| Write-Watch-Bitmap | 32 MiB, lazy | Z. 771 (`MAP_NORESERVE`) | nur `BB_GUEST_IN_PLACE` |
| Seitengröße | Gast 16 KiB (`PAGE`), Host 4 KiB | | Gast-Mappings sind 16-KiB-ausgerichtet |

Weitere Mechanismen in `runtime_memory.c`:
- **Aliasing:** Dieselbe physische Spanne kann mehrfach gemappt sein, `MAP_SHARED|MAP_FIXED`
  aus dem memfd (`map_phys`, Z. 179–217).
- **Ersetzen:** `place()` mappt mit `MAP_FIXED` über Vorhandenes (Z. 353–355).
  Reservierungen: `PROT_NONE|MAP_NORESERVE|MAP_FIXED`.
- **Freigeben:** `munmap` (Z. 443). Zurücksetzen auf null per `fallocate(PUNCH_HOLE)`
  (Z. 223, 231, 237, 275).
- **Schutz:** `mprotect` für Gast-Mprotect (Z. 550) und für die GPU-Seitenverfolgung
  (Z. 940, sehr häufig aufgerufen).
- **dma-buf-Chunks** (Z. 77–114, 152–177): nur mit `BB_GUEST_GPU_MEMORY` / `BB_GUEST_IN_PLACE=1`.
  Das ist das "PC-Modell", standardmäßig **aus** (`run.sh:119`) und nur auf AMD.

## 3. Windows-Abbildung

| Linux heute | Windows | Bemerkung |
|---|---|---|
| Bereiche low/user frei halten (implizit: niemand mappt dort) | **beim Start** `VirtualAlloc2(MEM_RESERVE \| MEM_RESERVE_PLACEHOLDER)` über low und user (`pf_vm_reserve_layout`) | Windows legt Heaps und DLLs sonst an zufällige Adressen; ohne Reservierung kann 0x800000000 belegt sein. shadPS4 macht dasselbe (`address_space.cpp:147–190`). |
| `mmap(addr, MAP_FIXED_NOREPLACE)` | Platzhalter an `addr` abspalten (`VirtualFree(MEM_RELEASE \| MEM_PRESERVE_PLACEHOLDER)`), dann `VirtualAlloc2(MEM_RESERVE \| MEM_COMMIT \| MEM_REPLACE_PLACEHOLDER)` | `pf_vm_alloc_fixed` |
| memfd + ftruncate | `CreateFileMapping2(INVALID_HANDLE_VALUE, …, PAGE_EXECUTE_READWRITE, SEC_COMMIT)` (Pagefile-Section) | Commit-Charge 6–17 GiB, kein physischer Speicher. Bei 31 GB RAM unkritisch. Alternative `SEC_RESERVE` + Commit beim Mappen, falls die Commit-Grenze stört. |
| `mmap(MAP_SHARED\|MAP_FIXED, memfd, off)` | Platzhalter passend splitten, `MapViewOfFile3(section, …, addr, off, size, MEM_REPLACE_PLACEHOLDER, prot)` | Mit Platzhaltern geht 4-KiB-Granularität für Adresse und Offset. shadPS4 mappt so mit 16-KiB-PS4-Seiten (`MapRegion`, `SplitRegion`). |
| Ersetzen eines bestehenden Mappings (MAP_FIXED) | erst `UnmapViewOfFile2(MEM_PRESERVE_PLACEHOLDER)` bzw. `VirtualFree(MEM_RELEASE\|MEM_PRESERVE_PLACEHOLDER)`, Platzhalter neu schneiden, dann mappen | Die Windows-Implementierung braucht eine eigene Region-Tabelle (wie shadPS4). |
| `munmap` | Sicht lösen und Platzhalter zurückgeben, Nachbarn mit `MEM_COALESCE_PLACEHOLDERS` vereinen | |
| `mprotect` | `VirtualProtect` (4 KiB, auch auf Teilen einer Sicht) | `PROT_WRITE` ohne READ gibt es nicht → READWRITE. EXEC+WRITE → `PAGE_EXECUTE_READWRITE`; dafür muss die Section mit `PAGE_EXECUTE_READWRITE` angelegt sein. |
| `fallocate(PUNCH_HOLE)` | über die Backing-Sicht nullen (`memset`) | Gibt keinen Speicher zurück. `DiscardVirtualMemory` liefert keinen garantierten Nullinhalt. Kosten messen (direct_release großer Bereiche). |
| `MAP_NORESERVE` (Bitmap) | `VirtualAlloc(MEM_RESERVE)` + Commit bei Zugriff (Fault-Handler) oder `MEM_COMMIT` ohne Berührung | 32 MiB, nur PC-Modell |
| dma-buf-Chunks | **nicht** im ersten Schritt: PC-Modell auf Windows aus; der bestehende Rückfallweg (memfd) bleibt | Später möglich über `VK_EXT_external_memory_host` |
| brk-Heap < 1 TiB für Gast-sichtbare Host-Objekte | **eigener Low-Heap** im low-Bereich (`pf_low_*`), plus Linker-Option gegen High-Entropy-ASLR | Windows-Heaps liegen typischerweise bei 0x1xx_xxxx_xxxx bis 0x2xx_xxxx_xxxx, also **über** 1 TiB. Ohne Low-Heap sind Thread-Handles und TCBs für den Gast unbrauchbar. |
| `-no-pie`, Exe unter 1 TiB | Exe-Basis 0x140000000 (Standard), `--disable-high-entropy-va` | Host-Funktionsadressen, die als Importziele in den Gast gehen, liegen dann niedrig. Nötig ist das nicht, auf Linux liegt libbbgpu.so bei 0x7f… |

## 4. Folgen für den Plan

- **Kritischer Pfad:** `pf_memory_win32.c` (Platzhalter-Layout, Section, Sichten mit
  Split/Coalesce, Schutz, Low-Heap) ist das erste Opus-Paket nach den Schnittstellen.
- Die **Reihenfolge beim Start** muss garantieren: `pf_init` → Layout reservieren → erstes
  `runtime_low_map` = Image. Das prüft ein Unittest (Adresse == 0x800000000) und Meilenstein M1.
- **Low-Heap-Audit:** Alle Host-Allokationen, deren Adresse der Gast sieht, müssen auf `pf_low_*`
  umgestellt werden. Bekannt: `runtime_thread.c:66, 81, 132`. Ein eigenes Prüfpaket sucht den Rest.
- `bbport_guest_hooks.cpp` und `bbport_free_check.cpp` (feste ImageBase) gehören nur zu
  PC-Modell und Diagnose und stehen damit nicht auf dem kritischen Pfad.

## 5. Offene Punkte, die erst mit Spiel messbar sind

- Tatsächliche Image-Größe (eboot + libc.prx + Fios2 + NpToolkit2), ergibt sich aus `prepare.py`/`link_modules.py`.
- Wie oft die GPU-Seitenverfolgung Schutzwechsel macht (Kosten von VirtualProtect gegenüber mprotect).
- Commit-Charge bei `BB_DMEM_MB` für hohe Auflösungen (+4 GiB).

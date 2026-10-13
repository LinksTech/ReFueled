# Paket K2a – pf_memory Linux

Stand 2026-10-13, ausgeführt von Sonnet, Prüfung und Commit durch Opus.
Nicht Teil: Umstellung von runtime_memory.c / probe.c / Shims (Nix-Gesamtbuild), test_memory.c
und linux-platform-tests.sh (Opus).

## Datei
`src/platform/pf_memory_linux.c` (neu). `pf_low_*` bleibt in pf_lowheap_linux.c.

## Gegenüberstellung alt <-> neu
| neu | alt |
|---|---|
| `pf_page_size` = `sysconf(_SC_PAGESIZE)` | probe.c `page_size = sysconf(_SC_PAGESIZE)` |
| `pf_vm_reserve_layout` = 0 | nichts (Linux hat keine Platzhalter) |
| `pf_vm_alloc_fixed`: `mmap(addr,size,prot,MAP_PRIVATE\|MAP_ANONYMOUS\|MAP_FIXED_NOREPLACE,-1,0)` | runtime_memory.c `runtime_low_map` (~Z. 676). Neu: EEXIST -> PF_EEXIST, andere Adresse als gewünscht (alter Kernel ohne NOREPLACE) -> `munmap` + PF_EEXIST (der alte Code prüft das nicht); Ausrichtung/Größe nicht 4 KiB -> PF_EINVAL |
| `pf_vm_alloc`: `mmap(NULL,…,MAP_PRIVATE\|MAP_ANONYMOUS)` | probe.c `allocate` Z. 97 |
| `pf_vm_alloc_lazy`: `…\|MAP_NORESERVE` (RW) | runtime_memory.c `runtime_memory_set_cpu_write_hook` Z. 771 |
| `pf_vm_free` = `munmap` | – |
| `pf_vm_protect` = `mprotect`, PF_PROT 1:1, WRITE schließt READ ein; kein Cache-Flush (x86) | probe.c `protect`, runtime_memory.c `host_prot` |
| `pf_backing_create`: `memfd_create("bb-guest-memory", MFD_CLOEXEC)`, `ftruncate`, `mmap(NULL,size,RW,MAP_SHARED\|MAP_NORESERVE,fd,0)`; zweiter Aufruf -> PF_EBUSY | runtime_memory.c `pool` Z. 136–140 |
| `pf_backing_map_fixed`: `mmap(addr,size,prot,MAP_SHARED\|MAP_FIXED,fd,offset)` | `map_phys` Z. 181 (und die Teilzuordnungen darin) |
| `pf_backing_zero`: `fallocate(PUNCH_HOLE\|KEEP_SIZE)` | `zero_phys` Z. 275 |
| `pf_handle_map_fixed`: `mmap(addr,…,MAP_SHARED\|MAP_FIXED,fd,off)` | `add_region` Z. 106, `ensure_chunks` Z. 171 |
| `pf_handle_map`: `mmap(NULL,…,MAP_SHARED,fd,off)` | bbport_guest_memory.cpp Z. 190/191/203 |
| `pf_handle_close` = `close` | `close(fd)` in add_region / ensure_chunks |
| `pf_guest_reserve`: `mmap(addr,size,PROT_NONE,MAP_PRIVATE\|MAP_ANONYMOUS\|MAP_NORESERVE\|MAP_FIXED,-1,0)` | `place` Z. 355 |
| `pf_guest_unmap` = `munmap` | – |
| `pf_read_memory_safe`: `process_vm_readv(getpid(),…)`; Rückgabe = Bytes; EFAULT ohne Kopie -> 0 | page_manager.cpp Z. 113/123, liverpool.cpp |

Fehlerabbildung errno -> PF_E*: EEXIST, ENOMEM, EINVAL/EBADF, EACCES, EPERM, ENOSPC, EAGAIN,
ENOSYS, sonst PF_EIO.

## Prüfung (WSL gcc 13, Flags wie linux-platform-tests.sh; Test mit `/mnt/c/CTR Reload/tmp/refueled/K2a/` als cwd)
Der Test definiert `_GNU_SOURCE` selbst (Zeile 7); mit `-D_GNU_SOURCE` auf der Kommandozeile
bricht `-Werror` ab ("redefined"). Ich habe darum test_memory.c ohne `-D_GNU_SOURCE`
übersetzt und die Plattformquellen mit. Das gilt auch für das Opus-Skript, sobald test_memory
aus der SKIP-Liste kommt.

Ergebnis -O2, -O0 und UBSan (-O1): jeweils
```
test_memory: FAILED line 220: pf_vm_alloc_fixed(USER_MIN - PAGE, 2 * PAGE, PF_PROT_READ) == PF_EINVAL
test_memory: 1 of 83 checks failed
```
83 statt 85 Prüfungen: die zwei Windows-Prüfungen (`#ifdef _WIN32` in test_layout_and_low) entfallen.

### Plattformunterschied (Zeile 222)
Der Test erwartet PF_EINVAL für einen festen Bereich, der die Grenze low/user überschreitet
(`USER_MIN - PAGE`, `2 * PAGE`). Unter Windows scheitert das an zwei verschiedenen Platzhaltern.
Unter Linux ist dort nichts gemappt, `MAP_FIXED_NOREPLACE` gelingt, und die Vorgabe lässt
`pf_vm_reserve_layout` nichts merken. Das ist kein Fehler der Linux-Fassung, sondern eine
Testannahme, die auf Linux nur mit zusätzlichem Zustand gilt. Optionen für Opus: (a) Zeile 222
und die Layout-Vorgabe als Windows-only hinter `#ifdef _WIN32`, (b) Linux merkt sich die
reservierten Bereiche (kleines statisches Feld in `pf_vm_reserve_layout`) und lehnt
`pf_vm_alloc_fixed` über eine Bereichsgrenze mit PF_EINVAL ab (ca. 15 Zeilen, ändert aber die
Aussage "Linux: nichts zu tun" im Header). Ich habe nichts davon umgesetzt.

### ASan
Mit `-fsanitize=address` bricht der Test bei `memset((void *)image, ...)` (Z. 274 Aufruf-Kette:
test_layout_and_low) ab: "Address 0x000800000000 is located in the shadow gap area". ASan reserviert
die Adressen 0x7fff8000..0x10007fff7fff als Schatten bzw. Lücke; 0x8_0000_0000 liegt darin. Die
Linux-Fassung selbst hat den mmap gar nicht abgelehnt, ASan fängt das memset des Tests ab (auch bei
nicht instrumentiertem Test, weil memset abgefangen wird). Deshalb ist ASan mit festen
Adressen ab 0x8_0000_0000 nicht lauffähig; als Ersatz lief UBSan (siehe oben, gleiches Ergebnis).

## Nachtrag: korrigierter Test
test_memory.c wurde mit `git checkout refueled/windows -- tests/refueled/test_memory.c` auf den
Stand ohne doppeltes `_GNU_SOURCE` gebracht (die Datei ist dadurch im Index, Opus-Datei, nicht
von mir geändert). Mit den Skript-Flags (`-D_GNU_SOURCE`, direkt in einem Aufruf) wiederholt:
-O2, -O0 und UBSan: jeweils `test_memory: FAILED line 222: pf_vm_alloc_fixed(USER_MIN - PAGE, 2 * PAGE,
PF_PROT_READ) == PF_EINVAL` / `test_memory: 1 of 83 checks failed`; ASan: wie oben, Abbruch in der
Schattenlücke bei 0x8_0000_0000. Der einzige Fehlschlag ist die Zeile 222 (Plattformunterschied).

# Debug-Werkzeug für M2 (Paket DBG)

Zwei Schalter in `src/probe.c`, beide ohne Wirkung, solange sie nicht gesetzt sind (Linux unverändert).
`run-windows.sh` setzt `BB_CRASH_DETAIL=1` standardmäßig (`BB_CRASH_DETAIL=0` schaltet ab); `run.sh` nicht.

## Schalter

| Schalter | Wirkung |
|---|---|
| `BB_CRASH_DETAIL=1` | Bei einem schweren Fault (nach dem normalen Bericht, vor dem Host-Backtrace): Fault-Art/Zugriff/Adresse, Thread-Name und -ID, alle Register, RIP als `image+0x..` bzw. Hostmodul+Offset bzw. Mapping, 64 Qwords ab RSP, rbp-Kette (bis 24 Rahmen). |
| `BB_TRACE_IMPORTS=<Teilstring>` oder `=1` | Jeder Aufruf der passenden Funktions-Imports (Name enthält den Teilstring; `1` = alle) meldet Thread, Importname, Aufrufer (`image+0x..`) und rdi/rsi/rdx, dann läuft der Aufruf unverändert weiter. |
| `BB_TRACE_IMPORTS_LIMIT=<n>` | Höchstzahl Meldungen je Import (Standard 8). Danach eine Schlusszeile "limit ... reached". |

Hinweis Trace: Datenimporte (kind 2) werden nicht verfolgt. Der Thunk sichert rdi, rsi, rdx, rcx, r8, r9, rax,
r10, r11 und xmm0-7 und springt zum Ziel (kein `call`); ymm-Oberhälften und xmm8-15 sind nicht gesichert (wie
bei jedem SysV-Aufruf ohnehin nicht erhalten). Ein Thunk braucht etwa 350 Byte Gaststack.

## Beispielausgabe (erfundene Werte)

```
Guest fault (signal -1073741819) at guest offset 0x1a2b3c, address 0x0000000000000010
---- crash detail (BB_CRASH_DETAIL) ----
fault: kind=access access=read address=0x0000000000000010 thread=GameMain tid=4321
rax   =0000000000000000  rbx   =0000000800123450  rcx   =0000000000000001
...
rip   =0000000801a2b3c0  eflags=0000000000010246
rip at image+0x1a2b3c
stack from rsp=0x000000010f7ff9a0 (64 of 64 qwords readable):
  [rsp+0x000] 0000000800412340 image+0x412340
  [rsp+0x008] 000000010f7ffa10
rbp chain:
  #0 rip=0000000801a2b3c0 image+0x1a2b3c
  #1 rip=0000000800412340 image+0x412340 mod0+0x2340
---- end of crash detail ----
TRACE_IMPORT tid=4321 sceKernelUsleep #1 caller=image+0x412340 rdi=0x2710 rsi=0x0 rdx=0x0
```
(`modN+0x..` erscheint, wenn der Wert im N-ten gelinkten Modul liegt; Modul 0 ist libc.prx.)

## Von `image+0x...` zur Gast-Funktion

1. Offset notieren (z. B. `0x1a2b3c`). Das Image ist `out/boot-linked.bin`-Speicherabbild: Offset = Adresse im Image.
2. Im Gast-Disassembly nachsehen (Image-Offset = virtuelle Adresse minus Image-Basis der Linkausgabe aus
   `scripts/link_*.py`): die Funktion ist der nächste Funktionsanfang unterhalb des Offsets, der Aufrufer
   steht in der rbp-Kette (`#1`, `#2`, ...).
3. Mit `BB_TRACE_IMPORTS=<Name>` die zuletzt aufgerufenen Imports vor dem Absturz einengen.

## Kombination mit Red Zone (ARCHITECTURE.md D17)

Jeden unerklärlichen Absturz ab M2 zuerst als A/B starten: `BB_REDZONE=0` und `BB_REDZONE=1`, jeweils mit
`BB_CRASH_DETAIL=1`. Ändert sich Fault-RIP, Register oder Stack zwischen A und B (z. B. ein Rücksprungwert im
Stack, der kein `image+0x..` mehr ist), liegt es an der Red Zone. Bleibt alles gleich, ist es ein anderer Fehler.

## Hänger statt Absturz

- Linux: `kill -USR2 <pid>` druckt den Stack jedes Threads (rip image-relativ, rbp-Kette); `BB_SAMPLE_THREAD=<Name>`
  (mit Frame-Statistik) sampelt, wo ein Thread läuft.
- Windows: der Watchdog (`--timeout`, `BB_TIMEOUT`) schreibt beim Ablauf die Stacks aller Threads
  ("STOP: watchdog timeout; thread stacks:").

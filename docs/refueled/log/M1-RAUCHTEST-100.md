# M1 – 1.00-Rauchtest (2026-10-14)

**Kein M1-Nachweis für 1.21.** Spielordner hat weiterhin nur 1.00 (`game_check.py`: missing_update,
APP_VER 01.00). Lauf nach Vorgabe Projektleitung als Rauchtest: `BB_SKIP_GAME_CHECK=1`, alle Patches aus
(leeres Patch-Verzeichnis, "Patches: 0 writes"), sacredfire.exe von a35f959 (CLANG64), `run-windows.sh`,
Daten/Ausgaben unter `C:\CTR Reload\tmp\refueled\M1R` (danach gelöscht). Ausgeführt und ausgewertet von Opus.
Nicht über M1 hinaus debuggt.

## A: `--list-imports`
- Link: native libc + libSceFios2 + libSceNpToolkit2, 286 native Bindungen, 727 Importe an die Runtime,
  fs->gs gepatcht = 3748 (wie TLS-Scan 1.00).
- `Imports: 448 runtime, 275 native, 290 unresolved, 0 unreferenced`, Exit 0.
- Linux-Referenz für 1.00 gibt es nicht (SacredFire unterstützt nur 1.21); Vergleich steht aus, bis 1.21 da ist.

## B: Start mit `BB_TIMEOUT=20`
- `GPU: window and Vulkan presenter ready; SDK 0x06008001, 327 HLE symbols` (RTX 5080, Vulkan 1.4.351)
- `Mapped 45551968 bytes, 12 segments; applied 459723 relocations`
- Spielcode läuft an: 97 Gast-Threads erzeugt (96 laut Zähler), Direktspeicher 3 Maps (4,9 GB live),
  42 Dateien geöffnet, 887 Lesezugriffe (65,6 MB), 3 Audio-Ports, 148 Audiopuffer ausgegeben.
- **GPU-Submits erreicht**: Submissions 1–18 dekodiert, EOP/EOS-Fences geschrieben (Zeilen "Guest write check").
- Ende: `STOP: first unsupported PS4 import: -quPa4SEJUw#libkernel (index 921)` = `scePthreadAttrGetstack`,
  Aufrufer in gelinktem Modul 2 (libSceNpToolkit2.prx, 1.00-Fassung). Sauberer Stopp, kein Fault, Exit 20.
- Basisadresse 0x800000000 steht nicht in der Ausgabe (probe.c druckt sie nicht); belegt nur über den
  K1-Test (erste low-Allokation = 0x800000000). Für den 1.21-Lauf eine Adresszeile ergänzen (Opus, probe.c).

## Bewertung
- M1-Kriterien auf 1.00: Image geladen und gelinkt, Importe aufgelöst bzw. klassifiziert: ja (Rauchtest).
- M2-Kriterium (erster GPU-Submit) wurde auf 1.00 nebenbei erreicht – nicht offiziell, da falsche Version und ohne Patches.
- Der Stopp an `scePthreadAttrGetstack` kommt aus der 1.00-Fassung von libSceNpToolkit2; mit 1.21 neu prüfen.

Verhaltensabweichung Linux: keine (nur Lauf, kein Code geändert)

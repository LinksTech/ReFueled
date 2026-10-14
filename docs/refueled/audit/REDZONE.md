# Vorstudie RZ: Red-Zone-Schutz des Gastcodes unter Windows

Stand 2026-10-13, Worktree `C:\nitro-ps4-wt\RZ` (Branch `refueled/pkg/RZ` ab 07e3d58). Nur Doku, kein Code.
Bezug: ARCHITECTURE.md D17 (Freigabe 2026-10-13: Schalter `BB_REDZONE=0/1`, fertig vor M3, ab M2 wird jeder
unerklärliche Absturz zuerst mit RZ an/aus getestet), U2-SIGNALE-THREADS.md §2, TASKS.md Zeile RZ.

**Quellen und Kennzeichnung.** Referenzcode: `C:\nitro-ps4-ref\shadPS4` (Stand `0ae50a4b`, 2026-10-12, im Folgenden
"main") und `C:\nitro-ps4-ref\shadPS4-b4e7ae7` (2026-09-24, "alt"). Windows-Verhalten ist mit Microsoft-Doku
belegt (Name und Aussage in §5, abgerufen am 2026-10-13) oder als **[Vermutung]** bzw. **[Eigenwissen]** gekennzeichnet.
Versuche auf Windows gab es nicht.

## 1. Was shadPS4 tut

### 1.1 Herkunft, Stand
- Eingeführt als "Add experimental Windows static red-zone protection (#4802)", Commit `dff1aad0`, 2026-08-06
  (`git log -S` auf `cpu_patches.h` in "main").
- Beide Referenzen enthalten es. **alt** (b4e7ae7): Funktion `PatchRedZoneMemoryInstructions` mit eigenem
  Rumpf, `cpu_patches.cpp:1597–2120`, `#ifdef _WIN32`-Zweig; außerhalb ein leerer Stub (`:2121`). Dazu
  `WindowsGuestRedZoneProtection` (`:922–944`), Aufräum-Test `tests/test_windows_guest_red_zone_protection_settings.cpp`.
  **main**: derselbe Algorithmus, umgebaut zu `PatchSegmentStatically` (`cpu_patches.cpp:1804–2342`), das
  dieselbe Durchlaufstruktur auch für die statischen SSE4a-CPU-Patches (`PatchCpuInstructionsStatically`) auf allen
  Systemen nutzt; `PatchRedZoneMemoryInstructions` (`:2398`) und `PatchCpuInstructionsStatically` (`:2403`) sind dünne Aufrufer.
  main hat zusätzlich Windows-Exception-Behandlung für `EXCEPTION_PRIV_INSTRUCTION` (`signals.cpp:67–73`).

### 1.2 Schalter und Modus
- Einstellung `redzone_patches` (Standard **false**, `emulator_settings.h:201`; Menüpunkt in `imgui/big_picture`).
  `emulator.cpp:551–554`: bei `IsRedZonePatchingEnabled()` wird `WindowsGuestRedZoneProtection::SetActiveMode(StaticPatching)`
  gesetzt (`cpu_patches.h:11–28`, `cpu_patches.cpp:1125–1147`). Kein Laufzeitumschalten pro Modul.
- Nur Windows nutzt den Modus. Auf Linux/macOS gibt es keine Red-Zone-Gefahr (Linux legt Signalframes
  unterhalb der Red Zone an [Eigenwissen, x86-64-ABI und Kernel-Signalaufbau]); dort läuft `PatchSegmentStatically` ohne Red Zone nur für CPU-Patches.

### 1.3 Wann und womit
1. **Beim Laden jedes Moduls**, nachdem die Segmente kopiert und die Spiel-Speicherpatches (`MemoryPatcher::OnGameLoaded`)
   angewandt sind, "damit der dekodierte Code der ausgeführte ist" (`module.cpp:296–312`, Kommentar vor `if (use_static_patching)`).
   Aufruf je ausführbarem Segment: `PatchRedZoneMemoryInstructions(segment_addr, segment_size, function_starts)` (`module.cpp:303–309`).
2. **Funktionsgrenzen** kommen aus der Tabelle der `.eh_frame_hdr` (PT_GNU_EH_FRAME): `Dwarf::DecodeEHHdr` und
   `Dwarf::DecodeEHHdrTable` (`module.cpp:~255–270`, `core/loader/dwarf.cpp`). Gibt es keine Tabelle, wird nichts gepatcht, Warnung
   "could not find function boundaries" (`module.cpp:310–316`).
3. **Zydis** zerlegt jede Funktion (`DecodeFunction`, `cpu_patches.cpp:1530–1607`): rekursiver Abstieg über Verzweigungen,
   Sprungtabellen werden begrenzt aufgelöst (`ResolveBoundedJumpTable`, `:1340`, aufgerufen `:1586`); unaufgelöste indirekte
   Sprünge setzen `has_indirect_branch`.
4. **Erkennung der Red Zone** (`DecodeCodeInstruction`, `:1214–1322`): Operand `[rsp + disp]` mit negativer Verschiebung
   (`:1262–1296`) markiert die Funktion als `uses_red_zone`; je Befehl werden 128 Bit (`GuestRedZoneSize = 128`, `:1151`) für
   gelesene (`red_zone_use`) und geschriebene (`red_zone_def`) Bytes geführt. `lea` mit negativem RSP-Offset und Änderungen von RSP
   (`add/sub rsp, imm`, `push/pop`, `lea rsp,[rsp+d]`) werden modelliert (`stack_pointer_delta`, `:1253–1292`), unmodellierbare Fälle
   erzwingen konservative Verfolgung (alles lebendig).
5. **Lebendigkeitsanalyse** (`AnalyzeRedZoneLiveness`, `:1623–1740`): rückwärts über den Funktionsgraphen, `TranslateRedZoneMask`
   (`:1608`) verschiebt die Maske bei RSP-Änderung. Ergebnis je Befehl: `red_zone_live`, die Red-Zone-Bytes, die danach noch gelesen
   werden. Nur Befehle mit **lebender** Red Zone werden gepatcht.
6. **Welche Befehle** (`:1953–1989`): in Funktionen mit Red-Zone-Nutzung jeder Befehl, der Speicher liest oder schreibt, **nicht**
   RSP-relativ ist (`accesses_memory`, `:1246–1248`: nicht LEA, nicht NOP, ohne RSP als Basis/Index), und bei dem `red_zone_live.any()`.
   Ausnahmen, die gezählt, aber nicht geschützt werden: RSP-abhängige Zugriffe (`stack_dependent`), Aufrufe und Kontrollfluss-Enden
   (`control_flow`: die Rückkehradresse liegt ohnehin unter RSP). Indirekte `call [mem]` bekommen eine eigene Sequenz
   (`GenerateProtectedIndirectCall`, `:1730–1779`: Zeiger über R11 laden, RSP absenken, Aufruf).
7. **Umschreiben mit Trampolin** (`emit_span`, `:1921–1963`; `EncodeRelocatedInstruction`, `:1689`): der Befehl wird in einen Trampolin-Bereich
   kopiert, umrahmt von
   ```
   lea rsp, [rsp - 128]
   <Befehl, mit Zydis-Encoder an die neue Adresse umkodiert (RIP-relativ bleibt gültig)>
   lea rsp, [rsp + 128]      ; entfällt, wenn der Befehl RSP ersetzt
   jmp <Fortsetzung>
   ```
   `lea` ändert keine Flags. Ein Fault im Befehl schreibt den Windows-Ausnahmeframe damit **unter** die Red Zone.
8. **Einbau am Fundort**: Die Stelle bekommt `jmp rel32` (5 Byte, `NearJumpSize`, `:728`) auf das Trampolin. Ist der Befehl kürzer
   als 5 Byte, werden die **Nachbarbefehle** mit verschoben (`collect_forward_span`/`collect_backward_span`, `:1992–2030`), solange
   dazwischen kein Sprungziel liegt (`branch_targets`) und kein Kontrollfluss-Ende; bei Funktionen mit unaufgelöstem indirektem Sprung
   nur der Fall, dass der Befehl selbst lang genug ist. Reicht das nicht: 2-Byte-`jmp short` auf einen "Relais-Slot" (ein NOP-gefüllter
   Rest eines anderen gepatchten Bereichs, der selbst ein `jmp rel32` trägt), notfalls über eine Kette (`:2055–2335`). Was sich nicht
   umsetzen lässt, wird gezählt (`unrelocatable_memory_instruction_count`) und bleibt ungeschützt, mit Warnung (`module.cpp:~340`).
9. **Trampolin-/Patch-Speicher**: je Modul ein fester 8-MiB-Bereich **direkt hinter dem Modul** (`TrampolineSize = 8_MB`,
   `module.cpp:110`, abgebildet `:126–139`, registriert mit `RegisterPatchModule`, `cpu_patches.cpp:2469`), damit `jmp rel32` in beide
   Richtungen reicht. Erzeugt mit Xbyak (`PatchModule::trampoline_gen`/`patch_gen`, `:758–785`). Ist der Bereich voll, wird weiter
   nicht gepatcht (`trampoline_exhausted`, `:778`, `HandleTrampolineError` `:787`).
10. **Zur Laufzeit**: Windows' `SignalHandler` (`signals.cpp:29–150`) meldet unbehandelte Zugriffsfehler/Illegal-Instruction-Ausnahmen im
    Red-Zone-Modus nur für die dort behandelten Arten (`static_protection_exception`, `:54–143`). Die Fault-Kette (`Signals`) bleibt
    sonst unverändert.

### 1.4 Kosten, Grenzen (aus dem Code, nicht gemessen)
- Einmalig beim Laden: Zydis-Zerlegung **aller** ausführbaren Funktionen des Moduls plus Liveness. Für 81 MiB Text (U1) vermutlich
  Sekunden [Vermutung, nicht gemessen]; der Code selbst erwähnt keine Zahlen.
- Zur Laufzeit je geschütztem Befehl: zwei `jmp` und zwei `lea`, I-Cache-Verteilung über den Trampolinbereich. Befehle in heißen
  Schleifen mit Red-Zone-Nutzung wären betroffen. Nicht gemessen.
- Nicht geschützt: Funktionen mit unaufgelöstem indirektem Sprung, wenn der Befehl kürzer als 5 Byte ist; RSP-relative Zugriffe;
  Code, der erst zur Laufzeit entsteht (JIT, selbstmodifizierend); nicht in der `eh_frame`-Tabelle stehende Funktionen (z. B. handgeschriebene Asm).
- shadPS4 liefert es als "experimentell", **Standard aus**; Quelle der Behauptung "Mehrheit der Spiele läuft ohne": nur
  U2 §2 (dort ohne Beleg). Der Code enthält keinen Hinweis darauf.

## 2. Was davon für SacredFire nötig ist

### 2.1 Wann Red Zone relevant wird
- Die Gefahr ist real, sobald Gastcode **mitten in einer Funktion mit lebender Red Zone** einen Ausnahme-Frame bekommt
  (MS: "All memory beyond the current address of RSP is considered volatile: The OS, or a debugger, may overwrite this memory ...
  or an interrupt handler", x64 stack usage, §5). Ausnahmequellen im Gastcode: (a) Schreibfaults der GPU-Seitenverfolgung
  (`page_manager.cpp` `SignalImpl::Protect` setzt Seitenschutz, `GuestFaultSignalHandler`, `:603–630`), (b) Lese-Faults bei
  Readbacks (`page_manager.cpp:461–536`), (c) Absichtliche Faults: `SIGTRAP`/`int3`-Hooks (`bbport_guest_hooks.cpp:163`,
  `bbport_free_check.cpp`), (d) `EXCEPTION_ILLEGAL_INSTRUCTION`, wenn der Gast SSE4a-Befehle auf einer CPU ohne SSE4a ausführt (shadPS4 fängt das ab, unser Loader hat dafür keine Entsprechung, siehe offene Frage 3).
- Auf Linux tritt es nicht auf [Eigenwissen] (Kernel überspringt 128 Byte). Darum ist das Problem bisher unbeobachtet.
- **Wie häufig** Faults im Normalbetrieb sind, ist unbekannt. Es gibt zwei Modi (`runtime_memory.c:760–802`, `page_manager.cpp:613–630`):
  mit Seitenschutz (jeder erste Schreibzugriff auf beobachtete Seite = Fault) oder die Write-Watch-Bitmap (`VideoCore::WriteTracking()` aus,
  Standard `BB_GUEST_IN_PLACE`-Pfad), bei der Gast-Stores **keine** Faults erzeugen und stattdessen die Hooks (`LoaderCopy`,
  `bbport_guest_hooks.cpp:49–66`; libc-Importe `runtime_memory_note_cpu_write`) die GPU-Seite benachrichtigen. Unter Windows ist
  `BB_GUEST_IN_PLACE` (dma-buf) aus (D12), also gilt dort der Schutzweg mit Faults. **Messung vor Bauen** (§4, RZ1).

### 2.2 Wo RZ in unserem Loader einhaken würde
Ablauf heute (`src/probe.c`): Image per `allocate` (`:501`), Relokation und Trampolin-Auflösung (`:520–552`), `apply_patches` (`:563`,
BBPATCH2: Byte-Schreibzugriffe), `bbgpu_patch_image` (`:564`), danach `protect(...)` der Segmente (`:565–573`). Dazu kommt (K3, geplant)
`pf_tls_patch_image(ranges, n)` "nach Relokation und vor Ausführbarmachung" (`pf_tls.h:32–38`). **RZ gehört direkt dahinter**:
1. nach `apply_patches` und `bbgpu_patch_image` (RZ muss den endgültigen Code sehen, wie `module.cpp:296` bei shadPS4),
2. nach `pf_tls_patch_image` (die `gs:[…]`-Ladebefehle behalten dabei ihre Länge, aber ihre Bytes ändern sich),
3. **vor** `protect(image …, exec)`. Danach schreibt RZ nur noch im noch schreibbaren Image und in der Trampolinregion, die anschließend
   `PF_PROT_READ|EXEC` bekommt.

Konflikte und Anforderungen:
- **Laufzeit-Hooks erwarten Originalbytes.** `bbport_guest_hooks.cpp:57–58` prüft/patcht feste Offsets (Aufrufbefehle `e8 …` bei `0x216d5ea`,
  `0x2171fea`, int3 bei `0x26aa255` usw.), `bbport_gnm_hooks.cpp` verschiebt Funktionsanfänge in Stubs. RZ darf diese Stellen nicht umbauen
  (Ausschlussliste aus den Hook-Tabellen oder RZ **nach** deren Installation, die aber erst zur Laufzeit geschieht). Klärung: Opus (offene Frage 2).
- **Trampolinspeicher** muss mit `jmp rel32` erreichbar sein, also ±2 GiB vom Image, im Low-Bereich (<1 TiB, F9). Einfachste Lösung wie bei shadPS4:
  Image-Allokation um einen Trampolinbereich verlängern (`allocate(round_page(size) + RZ_AREA)`), die Pagemap steht danach auf RX. Größe: aus Zählung
  (RZ1) bestimmen; shadPS4 nimmt pauschal 8 MiB je Modul. Der Bereich muss zum `image`-Block gehören, damit `bbgpu_patch_image` und Image-Prüfungen
  (`mapped()`) ihn nicht sehen.
- **Funktionsgrenzen** hat der Loader nicht: Das Boot-File enthält weder `.eh_frame_hdr` noch eine Funktionsliste (`scripts/prepare.py`,
  `link_modules.py`: kein `eh_frame`; `tools/re/pm4_scan.py:32–48` liest sie nur offline aus `eboot.elf`). RZ braucht also einen neuen
  Boot-File-/Sidecar-Teil "Funktionsanfänge" je Modul (eboot, libc.prx, Fios2 ...), erzeugt in `prepare.py`/`link_modules.py` aus
  `PT_GNU_EH_FRAME` (Python-Stdlib genügt, Format wie in `pm4_scan.py`). Format-Entscheidung = Opus. Ob alle gelinkten PRX eine
  `eh_frame_hdr` haben, ist ungeprüft [Vermutung: ja, Sony-PRX sind mit Unwind-Info gebaut].
- **Ein Sidecar statt Boot-File-Änderung** hält "Boot-File ist auf jeder Plattform gleich" (`pf_tls.h:5–10`) ein: Linux ignoriert es.

### 2.3 Abhängigkeiten, Sprache, Ort
- **Zydis**: für die GPU-Bibliothek ohnehin Pflicht (`gpu/CMakeLists.txt:54` `find_package(Zydis REQUIRED)`, genutzt in `gpu/shadps4/common/decoder.cpp`;
  MSYS2 `mingw-w64-clang-x86_64-zydis` 0.8.4/4.1.1 laut TOOLCHAIN.md Tabelle). Der **Zydis-Encoder** (`ZydisEncoderDecodedInstructionToEncoderRequest`,
  `ZydisEncoderEncodeInstructionAbsolute`) wird von shadPS4 für das Umkodieren genutzt (`cpu_patches.cpp:~1695–1779`); er ist in Zydis 4.x enthalten
  (Eigenwissen, gegen die installierte Version beim Bau prüfen).
- **Xbyak**: kommt über `tools/deps.sh` (v7.30, Header-only) in `out/deps/prefix`, nicht in MSYS2 (TOOLCHAIN.md). Für RZ nicht nötig: gebraucht werden nur
  `jmp rel32`, `lea rsp,[rsp±128]`, `jmp`. Das lässt sich mit festen Bytes (`48 8d 64 24 80`, `48 8d a4 24 80 00 00 00`, `e9 rel32`) erzeugen.
  Empfehlung: **ohne Xbyak**, das hält `src/` frei von einer zusätzlichen Header-Abhängigkeit.
- **Sprache**: Zydis ist eine C-Bibliothek mit C-API; eine C-Datei in `src/platform/` (`pf_redzone.c`) ist möglich. Die shadPS4-Vorlage ist C++ (std::map/bitset); die Portierung nach
  C++17 ohne Exceptions/RTTI in `src/platform/pf_redzone.cpp` mit `extern "C"`-Schnittstelle (`pf_redzone_patch_image(...)`) ist die geringste Abweichung vom Vorbild. Das Projekt hat in `src/`
  sonst nur C; `gpu/` ist C++23. Vorschlag: **eine C++-Datei in `src/platform/`**, gebaut mit dem Windows-Teil (F5/CMake), plus ein C-Header (`pf_redzone.h`) wie `pf_tls.h`. Entscheidung Opus.
- Der Rewriter selbst ist **plattformneutral** (er verwendet nur Bytes und Adressen). Er kann auf Linux (WSL, Zydis aus `libzydis-dev`) getestet werden; nur das Einhängen und der Fault-Test brauchen Windows.

### 2.4 Zu klären, ob RZ überhaupt gebraucht wird (Kontext SacredFire)
- Gastcode ist hier Crash Team Racing Nitro-Fueled (eboot + libc.prx + Fios2 + NpToolkit2). Ob dessen Compiler Red-Zone-Nutzung erzeugt (Clang für PS4: ja, x86-64-SysV-ABI, Blattfunktionen)
  zeigt eine Zählung: Anzahl Funktionen mit `[rsp-N]`-Operand (§4 RZ1).
- Zweite Frage: Treffen die Faults diese Funktionen? Die bbport-Hooks fangen schon heute die großen GPU-Schreibwege ab (`LoaderCopy`, libc-Importe `memcpy/memset/memmove`, Dateilesen);
  das senkt die Zahl der Faults [Vermutung], erklärt aber nicht, wo sie verbleiben. Darum Fault-Statistik je Gast-RIP (Risiko R1 in ARCHITECTURE.md verlangt sie schon).

## 3. Alternativen

### 3.1 Ausnahme auf anderem Stack (wie `sigaltstack`)
- `SetThreadStackGuarantee` reserviert Platz **auf dem eigenen Stack** des Threads für die Behandlung von Stack-Overflow-Ausnahmen; es gibt keinen alternativen Ausnahmestack
  (MS-Doku "SetThreadStackGuarantee": "Sets the minimum size of the stack associated with the calling thread or fiber that will be available during any stack overflow exceptions"). **Nicht geeignet.**
- Vektorisierte/strukturierte Ausnahmebehandlung läuft auf dem Stack des fehlerhaften Threads; der Kernel legt `CONTEXT` und `EXCEPTION_RECORD` dort an, bevor
  `KiUserExceptionDispatcher` läuft [Eigenwissen; shadPS4 und U2 §2 gehen davon aus]. Ein Handler kann den Ort des Frames nicht mehr ändern, er läuft schon danach. Eine Windows-API für einen alternativen Ausnahmestack ist mir **nicht bekannt**
  [Eigenwissen, nicht belegt]. Ein Einhängen von `ntdll!KiUserExceptionDispatcher` hilft nicht, weil der Kernel den Frame vorher schreibt [Vermutung].
- Eine Ausnahme-lose Variante wäre, Gast-RSP dauerhaft 128 Byte tiefer zu führen. Das bedeutet jeden RSP-Bezug im Gastcode umzuschreiben und ist teurer als RZ.

### 3.2 Faults im Normalbetrieb vermeiden
- **`GetWriteWatch`/`MEM_WRITE_WATCH`**: Der Aufruf gilt nur für Speicher, der mit `VirtualAlloc` und `MEM_WRITE_WATCH` angelegt wurde (MS-Doku "GetWriteWatch": "This address must be in a memory region
  that is allocated by the VirtualAlloc function using MEM_WRITE_WATCH"; Remarks: "you can specify MEM_WRITE_WATCH ... causes the system to keep track of the pages that are written to in the committed memory region").
  Die Gast-Views sind aber `MapViewOfFile3`-Views einer Section (`pf_memory.h`, D5; Aliasing mehrerer Views auf dieselbe Backing-Section). Für Section-Views ist **kein** Write-Watch-Flag dokumentiert
  (`MapViewOfFile3` kennt es meines Wissens nicht [Eigenwissen, in der Doku nicht gegengeprüft]). Folge: GetWriteWatch ist für die aliasierenden Gast-Mappings **nicht anwendbar**; denkbar nur für
  reine VirtualAlloc-Bereiche ohne Aliasing, was das Speichermodell (D5) ändern würde. Außerdem liefert GetWriteWatch Auskunft *nach* dem Schreiben (kein Hook davor), taugt also nur für Frame-Granularität, nicht für sofortige Benachrichtigung der GPU-Seite.
  **Fazit: nicht ohne Modelländerung.** Ein Windows-Versuch mit `VirtualAlloc2`+Placeholders und `MEM_WRITE_WATCH` wäre ein eigener Machbarkeitstest [Vermutung: nicht kombinierbar mit `MEM_REPLACE_PLACEHOLDER`/Section].
- **Write-Watch-Bitmap ohne Seitenschutz** (existiert schon: `runtime_memory.c:760–802`, `page_manager.cpp:613–630`): Gast-Stores werden **nicht** erkannt, nur Hook-Schreibwege. Für das Spiel gedacht mit `BB_GUEST_IN_PLACE` (GPU liest den Gastspeicher direkt). Auf Windows nicht ohne D12-Änderung.
- **Gröbere Verfolgung** (mehr Seiten je Schutzwechsel, Bündeln, Fault-freier Pfad bei bekannten großen Kopien): senkt die Faultzahl, beseitigt nicht die Gefahr.
- **Gezielter Schutz nach Messung** (Hybrid): Fault-Statistik je Gast-RIP ergibt eine kurze Liste der tatsächlich fehlerauslösenden Befehle; RZ patcht dann nur diese (Profil `BB_REDZONE=census` schreibt die Liste, ein späterer Lauf liest sie). Kein Zydis-Liveness nötig, wenn die Liste klein ist: pro Stelle `lea/Befehl/lea` von Hand verifiziert.
  Nachteil: unvollständig bei Pfaden, die der Messlauf nicht trifft.

### 3.3 Messkriterien
- **Fault-Zahl** im Normalbetrieb (pro Frame, pro Gast-RIP), getrennt nach Schreib-/Lesefaults. Entscheidet über den Bedarf.
- **Anzahl Funktionen mit Red-Zone-Nutzung**, Anteil mit lebender Red Zone bei einem schreibenden/lesenden Befehl, Anzahl umzuschreibender Befehle, davon kurze (<5 Byte), unrelozierbare (RZ-Statistik von shadPS4 als Vorlage: `RedZonePatchResult`, `cpu_patches.h:34–64`).
- **Trampolinbedarf** in KiB, Ladezeit der Analyse (ms), Laufzeit-Delta der Bildzeit (A/B `BB_REDZONE=0/1`, gleiche Szene, p50/p95), Kennzahl "Frametime-Anstieg < 1 %" als Annahme-Schwelle [Vorschlag].
- **Korrektheit**: Absturz-/Datenfehlerrate A/B über denselben Pfad (ab M2 laut D17).

## 4. Paketvorschlag RZ

Abhängigkeiten laut TASKS.md: K3 (Loader), K7 (Fault-Kette). Opus-Anteil nach TASKS: "RZ (mit)".

| Teil | Inhalt | Umfang | Wer | Abnahme |
|---|---|---|---|---|
| RZ1 Zählung (census) | Offline und im Loader: Funktionsliste lesen, Zydis-Durchlauf **ohne Umschreiben**, Statistik (Funktionen, Red-Zone-Funktionen, lebende Befehle, kurze Stellen, indirekte Sprünge); `BB_REDZONE=census`. Plus Fault-Zähler je Gast-RIP im Fault-Pfad (K7) | M | Opus (Entwurf), Sonnet (Statistikausgabe, Skript) | Ausgabe der Zahlen aus §3.3 für eboot + libc.prx; entscheidet über RZ3 |
| RZ2 Funktionsliste | `prepare.py`/`link_modules.py`: `PT_GNU_EH_FRAME` lesen, Sidecar `redzone.bin` (Modul-ID, sortierte Startadressen als Image-Offsets); Parser-Test (Python) mit synthetischem ELF | S | Sonnet (nach Formatvorgabe von Opus) | Python-Unittest grün, Zahl der Funktionen stimmt mit `pm4_scan.py` überein |
| RZ3 Rewriter | `src/platform/pf_redzone.cpp/.h`: Dekodieren, Funktionsgraph, Sprungtabellen, Liveness, Relokation/Trampoline, kurze Stellen (Nachbarn, Relais), Statistik. Plattformneutral, ohne Xbyak (feste Bytes + Zydis-Encoder) | L | Opus | Linux-Unittests (RZ4); Zähler = Ergebnis von RZ1 |
| RZ4 Unittests Rewriter (Linux, WSL) | synthetische Funktionen als Bytes (Blattfunktion mit `[rsp-8]..[rsp-128]`, Sprungtabelle, RIP-relativer Zugriff, `gs:`-Zugriff, kurze 2–4-Byte-Befehle, `rep movs`, `lock cmpxchg`): Original und gepatchte Variante laufen auf Linux in RWX-Speicher und liefern gleiche Ergebnisse; Trampolin-Byte-Prüfung | M | Sonnet nach Testliste von Opus | gleiche Ergebnisse, kein Befehl ausserhalb des Plans geändert |
| RZ5 Loader-Einbau | `probe.c`: hinter `bbgpu_patch_image`/`pf_tls_patch_image`, Trampolinbereich an das Image, Ausschlussliste Hook-Stellen, `BB_REDZONE=0/1`, Statistikzeile, Rückfall bei voller Trampolinregion | M | Opus | Linux: `--list-imports`/Boot unverändert bei `BB_REDZONE=0`; Windows: Start + Statistik |
| RZ6 Windows-Beweistest | `tests/refueled/test_redzone_win.c` + Asm-Teil (siehe unten) | M | Sonnet (Asm nach Vorgabe), Opus (Prüfung) | beide Testfälle wie unten |
| RZ7 A/B-Protokoll | Messskript und Tabelle (Faults, Frametime, Trampolinbedarf), Auswertung | S | Sonnet (Lauf), Opus (Bewertung) | Zahlen für M3-Entscheidung |

Reihenfolge: RZ1 + RZ2 zuerst (klein, klären den Bedarf); erst bei Bedarf RZ3–RZ5. Fällt die Zählung eindeutig aus (keine lebenden Red-Zone-Zugriffe an Fault-Orten), bleibt RZ bei Messkriterium und Test und wird nicht eingebaut (D17 bleibt "Reserve").

### Beweis ohne Spiel, dass die Red Zone geschützt ist
Testprogramm unter Windows (MinGW/clang, `tests/refueled/test_redzone_win.c` mit Assembler-Funktionen):
1. **Gefahr-Beweis (Mutant)**: Asm-Funktion `rz_probe(volatile uint64_t *ro_page)`: schreibt 16 Sentinel-Werte nach `[rsp-8] … [rsp-128]`, speichert dann nach `*ro_page` (Seite mit `PAGE_READONLY`; ein VEH setzt sie auf `PAGE_READWRITE` und liefert `EXCEPTION_CONTINUE_EXECUTION`), liest die 16 Werte zurück und gibt die Zahl der Abweichungen zurück. **Erwartet** (laut MS-Doku über den Zustand von Speicher unter RSP): > 0 ohne Schutz [Vermutung für die konkrete Windows-Version]. Ist es 0, kennzeichnet der Test "Gefahr nicht reproduzierbar" statt FAIL und der Befund geht an Opus.
2. **Schutz-Beweis**: dieselbe Funktion nach dem Rewriter (RZ3 auf den Bytes von `rz_probe` angewandt, Trampolin in RWX-Speicher nah daran): Abweichungen == 0. Zusätzlich ein Fall je Befehlsart (store, load, `lock xadd`, `movups`, `rep stosb`, RIP-relativ, `fs/gs`-Präfix), jeweils mit Fault auf dem Zielbefehl.
3. **Mehrfach/Thread**: gleicher Test in 4 Threads mit unterschiedlichen Stacks (auch Stack-Grenzfall: Funktion nahe der Stack-Guard-Seite) und 10 000 Wiederholungen, damit Zeitfenster-Effekte auffallen.
4. **Kontrolle der Analyse**: Statistik des Rewriters für die Testfunktionen (erwartete Zahlen); ein Befehl ohne lebende Red Zone darf **nicht** umgeschrieben werden (Test auf unveränderte Bytes).
Der Rewriter-Teil (RZ4) läuft zusätzlich auf Linux, nur der VEH-/Fault-Teil (RZ6) braucht Windows.

## 5. Belege (Microsoft) und Eigenwissen

- Microsoft Learn, "x64 stack usage" (cpp/build/stack-usage): "All memory beyond the current address of RSP is considered volatile: The OS, or a debugger, may overwrite this memory during a user debug session, or an interrupt handler. Thus, RSP must always be set before attempting to read or write values to a stack frame." Die Seite beschreibt keine Red Zone (Windows-ABI hat keine); Leaf-Funktionen dürfen RSP nicht ändern und nichts darunter ablegen.
- Microsoft Learn, "x64 ABI conventions" (cpp/build/x64-software-conventions): bestätigt Windows-x64-ABI (Register, Alignment, Stack/Prolog), keine Red Zone; Verweis auf "x64 exception handling".
- Microsoft Learn, "GetWriteWatch function" (memoryapi): nur für mit `VirtualAlloc`/`MEM_WRITE_WATCH` angelegte Regionen (Zitate §3.2).
- Microsoft Learn, "SetThreadStackGuarantee function" (processthreadsapi): Platz für Stack-Overflow-Behandlung auf dem Thread-Stack, kein zweiter Stack (Zitat §3.1).
- **Nicht belegt (Eigenwissen/Vermutung):** dass der Kernel den Ausnahme-Frame unmittelbar unter RSP anlegt (bei shadPS4 und U2 §2 so beschrieben, Windows-interne Details nicht aus Doku); dass es keinen alternativen Ausnahmestack gibt; dass `MapViewOfFile3` kein Write-Watch kennt; Linux-Signalframe-Verhalten (Red Zone wird übersprungen); Laufzeitkosten und Analysezeit.

## 6. Offene Fragen an Opus
1. **Bedarf zuerst messen?** Vorschlag: RZ1+RZ2 vor allem anderen, RZ3–RZ5 nur bei Befund. Passt das zu "fertig vor M3"? Ohne Spieldaten ist RZ1 nur bis zu den Boot-File-Daten ausführbar; die Fault-Statistik braucht Windows plus Spiel (ab M2).
2. **Hook-Konflikt**: Sollen die Stellen der bbport-Hooks (`bbport_guest_hooks.cpp` Offsets `0x216d5ea`, `0x2171fea`, `0x26aa255`, `0x11e6629…`, `0xbb5fb0`; Gnm-Census-Stubs) per Ausschlussliste vom Umschreiben ausgenommen werden, oder soll RZ erst nach deren Installation (zur Laufzeit, Image dann RX) laufen?
3. **SSE4a**: shadPS4 verwendet dieselbe Maschinerie für CPU-Patches (EXTRQ/INSERTQ/MOVNTSS auf CPUs ohne SSE4a, `cpu_patches.cpp:538–542`). Der Zielrechner (NVIDIA RTX 5080, vermutlich Intel/AMD Zen mit SSE4a ja/nein?) und die Frage, ob SacredFire/CTR SSE4a überhaupt benutzt, ist offen; Linux-Stand hat dafür nichts. Eigenes Paket, nicht RZ.
4. **Sprache/Ort**: C++ in `src/platform/pf_redzone.cpp` (Vorschlag) oder C?
5. **Sidecar-Format** für Funktionsanfänge (je Modul, Image-Offsets): neues Boot-File-Magic `BBPROBE6` oder Datei neben dem Boot-File? Beachten: Boot-File bleibt plattformgleich.
6. **Trampolinbereich**: am Image anhängen (Vorschlag) oder eigener `runtime_low_map`-Block innerhalb ±2 GiB (Reihenfolge der Low-Vergaben ist fest, F9/U1)?
7. **Reichweite des Schutzes**: Reicht "nur Befehle mit lebender Red Zone" (Vorlage) oder wählen wir die einfachere, größere Variante "jeder Speicherbefehl in Red-Zone-Funktionen" (weniger Analysecode, mehr Trampoline)?

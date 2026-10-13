# ReFueled – Mod-Loader: Architekturentwurf (Stand 2026-10-13, Opus)

**Status: Entwurf, kein Code.** Keine Schnittstelle hieraus ist eingefroren. Umsetzung frühestens
nach M4; vorher nur Entscheidungen, die die laufenden Pakete nicht stören. Betrifft beide
Plattformen gleich (Linux und Windows), Linux-Verhalten ohne Mods bleibt unverändert.

## 1. Ziel und Abgrenzung

Ein Mod ist ein Ordner mit einer Beschreibung (`mod.json`) und bis zu drei Teilen:

| Teil | Was | Risiko |
|---|---|---|
| **Assets** | Ersatz- und Zusatzdateien für `/app0` (Texturen, Modelle, Strecken, Sounds, Texte) | gering, nur Lesen |
| **Patches** | Code-/Datenänderungen am Image: Byte-Patches und Funktions-Hooks auf Gast-Code | mittel, versionsabhängig |
| **Plugin** | native Bibliothek (Windows DLL, Linux .so) gegen eine versionierte C-API | hoch, beliebiger Code |

Nicht Ziel: Ersatz von `eboot.bin`, `sce_module`, `sce_sys`; Online-Funktionen; Änderungen an
den Spieldateien auf der Platte (Spielordner bleibt nur lesbar, wie heute).

## 2. Bestand (bbport/SacredFire) und was davon bleibt

- **Dateimods** (`scripts/mods.py`): bauen vor dem Start ein Link-Baum `out/mod-game-*` aus Spiel
  und Mods. Unter Windows nicht tragfähig (Symlinks brauchen Rechte, T2a Befund 8–10; Löschrisiko
  T2a Befund 1). **Ersetzt** durch ein Overlay im Runtime-Dateisystem (§3), das ohne Links auskommt
  und auf beiden Plattformen gleich arbeitet. `mods.py` bleibt für Linux als Rückfall, bis das
  Overlay gleichwertig ist (Linux-Verhalten unverändert, D19/D21-Regel).
- **XML-Patches** (`scripts/patches.py`, `apply_patches` in probe.c, shadPS4/GoldHEN-Format):
  bleiben unverändert das Format für Byte-Patches; Mods liefern sie in `patches/*.xml` mit.
- **Trace** (`BB_MOD_TRACE`): wird zur Overlay-Diagnose (welcher Mod liefert welche Datei).

## 3. Asset-Overlay (Runtime, kein Link-Baum)

- Ort: `src/runtime_file.c`, an der Stelle, an der ein Gastpfad unter `/app0` auf einen Hostpfad
  übersetzt wird (`translate`). Neu: zuerst ein **Overlay-Index**, dann der Spielordner.
- Index: beim Start einmal aufgebaut, aus allen aktiven Mods in Ladereihenfolge, Schlüssel =
  normalisierter Gastpfad (Kleinbuchstaben, `/`, ohne `..`; die heutige `..`-Prüfung wird dabei
  auch für `\` dicht gemacht, offener Punkt aus R2c). Wert = Hostpfad der Mod-Datei. Letzter Mod
  gewinnt; Konflikte werden einmal geloggt ("Mod B ersetzt Datei X von Mod A").
- Verzeichnislisten (`list_directory`, `getdents` des Gasts): Vereinigung aus Spielordner und
  Overlay, Overlay-Einträge überdecken gleichnamige.
- Schreiben: `/app0` bleibt nur lesbar; Spielstände unverändert im Nutzerordner.
- Plattform: nur `pf_fs`-Funktionen und der gemeinsame Pfadcode; keine Links, kein Kopieren,
  deshalb auch unter Windows ohne Sonderrechte.
- Archive: zunächst nur ganze Dateien. Ersetzen einzelner Einträge in Spielarchiven ist ein
  späterer Schritt (Format-Kenntnis nötig, eigenes Paket).

## 4. Code-Hooks

Drei Stufen, aufsteigend nach Macht und Risiko:

1. **Byte-Patches**: wie heute (XML, `apply_patches`), angewendet nach den Relokationen und vor dem
   Schutzwechsel. Überlappung zweier Mods ist ein Ladefehler mit klarer Meldung, nicht "der
   letzte gewinnt".
2. **Import-Hooks**: jeder Import (NID/Name) kann vor dem Binden auf einen Thunk umgelenkt werden,
   der einen Vor-/Nach-Rückruf ruft und dann das echte Ziel. Gleicher Mechanismus wie der
   Import-Trace aus dem Paket DBG (Thunk sichert Argumentregister, ruft `sysv_abi`-Rückruf, springt
   weiter); für Mods kommt ein Nach-Rückruf mit Rückgabewert dazu. Versionsunabhängig, weil
   NID-basiert. Gut für: Dateizugriffe, Eingabe, Audio, Zeit, Zufall.
3. **Funktions-Hooks auf Gast-Code** (Detours): ein Ziel im Image (Offset oder Signatur) wird
   durch einen 14-Byte-Sprung auf einen Trampolin-Block ersetzt; der Block enthält die
   verschobenen Originalbefehle (mit Zydis dekodiert und RIP-relativ korrigiert) und den
   Rücksprung, damit der Mod das Original rufen kann. Trampoline liegen in `runtime_low_map`
   (unter 1 TiB, Gast erreicht sie). Ziele werden bevorzugt über **Signaturen** (Bytes + Maske)
   gefunden, mit optionalem Offset-Tabellen-Rückfall je `APP_VER`; ein Hook, dessen Signatur nicht
   genau einmal passt, wird nicht gesetzt und gemeldet.
   Grenzen: keine Hooks in Funktionen kürzer als 14 Byte ohne freien Platz; kein Hook auf Code,
   den der TLS-Patcher oder `bbgpu_patch_image` schon geändert hat (Konfliktprüfung gegen deren
   Listen).

Reihenfolge beim Start: Relokationen → eingebaute Patches (`patches/` des Ports) → GPU-Patches →
TLS-Patch → **Mod-Byte-Patches** → **Mod-Detours** → Schutzwechsel → Import-Hooks greifen beim
Binden (läuft vorher). Ein Konflikt mit eingebauten Patches ist ein Ladefehler.

## 5. Lade-API für Plugins

- Einstieg: `int refueled_mod_init(const RfModApi *api, RfModContext *mod)` in der Plugin-Bibliothek,
  Rückgabe 0 = geladen. Laden über eine neue Plattformfunktion (`pf_library_open/symbol/close`,
  Linux `dlopen`, Windows `LoadLibraryW`); Plugins sind Opt-in je Mod und standardmäßig aus.
- `RfModApi`: Struktur mit `size` und `version` vorn (vorwärtskompatibel: neue Felder nur hinten,
  Plugins prüfen `size`), Funktionen:
  - Protokoll: `log(mod, level, fmt, ...)`.
  - Gastspeicher: `guest_read/guest_write` (über `pf_read_memory_safe`, Schutz beachtet),
    `image_base`, `image_size`, `app_version`.
  - Hooks: `hook_import(name, pre, post, user)`, `hook_function(signature|offset, handler, &original)`,
    `patch_bytes(offset, bytes, size)` – nur in der Ladephase erlaubt, danach Fehler.
  - Assets: `add_file(guest_path, host_path)`, `add_directory(...)` – speist den Overlay-Index.
  - Ereignisse: `on_frame(callback)` (aus dem Present-Pfad der GPU-Bibliothek, ohne Blockieren),
    `on_exit(callback)`.
  - Einstellungen: `config_get(key)` aus `mod.json`/Nutzerprofil.
- Aufrufkonvention der Rückrufe in Gast-Kontexten (Hooks): `sysv_abi`, wie alle Host-Funktionen,
  die Gast-Code direkt erreicht (D4). Rückrufe ohne Gast-Kontext (Frame, Exit): normale
  Plattformkonvention.
- Threads: Hook-Rückrufe laufen auf dem Gast-Thread, der den Hook trifft. Regeln wie für den
  Runtime-Code: nicht lange blockieren, keine Gast-Locks halten.

## 6. Mod-Format und Verwaltung

```text
mods/<Mod-ID>/
  mod.json        {"id","name","version","game":"CUSA13795","app_ver":["01.21"],
                   "requires":[...], "conflicts":[...], "load_after":[...],
                   "assets":"assets", "patches":"patches", "plugin":{"windows":"x.dll","linux":"x.so"}}
  assets/app0/... Dateien relativ zu /app0
  patches/*.xml   Byte-Patches im shadPS4-Format
```

- Reihenfolge: aus `mods.json` des Nutzerprofils (wie heute: `order`, `disabled`), geprüft gegen
  `requires`/`load_after`/`conflicts`; Zyklus oder Konflikt = Mod deaktiviert, Meldung.
- `app_ver` muss passen, sonst nur Assets laden, Patches/Plugin nicht (mit Meldung).
- Ein Bericht beim Start: aktive Mods, ersetzte Dateien je Mod, gesetzte Hooks/Patches, Fehler.
- Spielstände: Mods, die Spielstände verändern, kennzeichnen das in `mod.json`; der Loader legt
  vor dem ersten Start mit so einem Mod eine Sicherung des Spielstandordners an.

## 7. Sicherheit und Stabilität

- Plugins sind nativer Code ohne Sandbox: nur aus vom Nutzer gewählten Ordnern, Opt-in je Mod,
  deutlicher Hinweis im Launcher. Keine automatischen Downloads.
- Asset-Pfade: nur unterhalb des Mod-Ordners (keine Links, keine `..`), Größenprüfung gegen
  offensichtliche Fehler.
- Absturzdiagnose: der Crash-Detail-Bericht (Paket DBG) nennt aktive Mods und, wenn RIP in einem
  Trampolin oder Plugin liegt, den verantwortlichen Mod.
- Rückweg: `BB_MODS_ENABLED=0` schaltet alles ab; ein abgestürzter Start mit Mods schlägt beim
  nächsten Start "ohne Mods starten" vor.

## 8. Offene Fragen (Entscheidung Projektleitung)

1. Plugins überhaupt (Stufe 3 + native API) oder zunächst nur Assets + Byte-Patches + Import-Hooks?
2. Archiv-Einträge ersetzen (Format der CTR-Archive) – wie wichtig für die ersten Mods?
3. Soll der Launcher Mods verwalten (wie bbport), oder reicht anfangs `mods.json` + Startbericht?
4. Linux: Overlay auch dort als Standard (dann ist `mods.py` überflüssig) – das wäre eine
   Linux-Verhaltensänderung und braucht eine Entscheidung nach der D21-Regel.

## 9. Mögliche Pakete (nach M4)

| ID | Inhalt | hängt ab von |
|---|---|---|
| ML1 | Overlay-Index in runtime_file.c + Verzeichnis-Vereinigung + `..`-Prüfung – **vorgezogen als Paket MO (TASKS.md), nach M1 und vor M4**, weil der Link-Baum unter Windows ohne Admin/Entwicklermodus nicht geht | M1 |
| ML2 | `mod.json`-Lesen, Reihenfolge, Startbericht | ML1 |
| ML3 | Import-Hooks (Thunks aus DBG verallgemeinert) | DBG, ML2 |
| ML4 | Detours mit Zydis, Signatursuche, Konfliktprüfung | ML2 |
| ML5 | `pf_library_*` + `RfModApi` v1 + Beispiel-Plugin | ML3, ML4 |
| ML6 | Launcher-Seite | ML2 |

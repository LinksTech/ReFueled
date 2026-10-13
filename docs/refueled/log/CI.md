# Paket-Log CI - Linux-Pruefung in GitHub Actions (vorbereitet, nicht ausgefuehrt)

Verhaltensabweichung Linux: keine (nur Workflow und Testskript-Pfade)

Stand 2026-10-13, ausgefuehrt von Sonnet, Pruefung und Commit durch Opus.

## Dateien
- `.github/workflows/linux-build.yml` (neu)
- `docs/refueled/tools/linux-platform-tests.sh` (umgebaut auf Repo-relative Pfade)
- `docs/refueled/tools/linux-regress.sh` (nur Kopfzeilen: als veraltet markiert)

## Workflow
- Trigger: `workflow_dispatch`, `push` und `pull_request` auf `refueled/windows`. Job auf `ubuntu-24.04`.
- Actions: `actions/checkout@v4` (`submodules: recursive`, wie tests.yml), `cachix/install-nix-action@v31`,
  `actions/cache@v4`. tests.yml benutzt kein Nix, daher keine Nix-Version zum Uebernehmen.
- Nix-Pin: `shell.nix` hat keinen eigenen Pin (`import <nixpkgs>`), deshalb `nix_path: nixpkgs=channel:nixos-unstable`
  (im Workflow kommentiert; Risiko: unstable bewegt sich).
- Schritte: (a) `bash build.sh --test` (geht selbst in nix-shell), (b) Python-Unittests in nix-shell,
  (c) `linux-platform-tests.sh` in nix-shell, (d) `linux_hits.py` nur Info (`continue-on-error`): shallow Fetch von
  shadPS4 bei `b4e7ae7` fuer `--ref`; scheitert der Fetch (GitHub erlaubt Fetch per Kurz-Hash evtl. nicht, ggf.
  vollen Hash eintragen), laeuft das Skript ohne `--ref` (gpu/shadps4 wird dann uebersprungen, steht im Bericht).
- Cache `out/deps` (optional): `build.sh` legt dort per `tools/deps.sh` das Prefix (`out/deps/prefix`) ab, Schluessel aus
  shell.nix, build.sh und gpu/**; bei Stale-Problemen entfernen.
- Keine Secrets.

## linux-platform-tests.sh
Aufruf aus der Repo-Wurzel, optional `[REPO_DIR]`; Ausgabe nach `out/platform-tests/`. Test-Auswahl, Flags,
Quellen und Exit-Code unveraendert (nur `cd` und `out` geaendert). Keine geaenderte Linux-wirksame Quelltext-Stelle.

## Pruefungen
- YAML mit Windows-Python/PyYAML geladen: Syntax ok, Trigger und Job-Schluessel wie erwartet.
- `bash -n` fuer beide Skripte: ok.
- Workflow und Skripte nicht ausgefuehrt (kein push, kein act).

## Prüfung Opus 2026-10-13 (Pflichtfeld)
Feld bei der einmaligen Durchsicht aller Logs gesetzt und gegen die alt↔neu-Tabelle geprüft.

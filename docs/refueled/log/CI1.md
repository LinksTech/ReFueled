# CI1 – erster Linux-Lauf in GitHub Actions (2026-10-14)

Erster Lauf von `.github/workflows/linux-build.yml` (Run 38618651874): `out/sacredfire` baut unter Linux (Nix).
Fehler nur beim Linken von `out/pad-test`: `undefined reference to pf_fs_stamp` (runtime_pad.c:111/614). Ursache:
`tests/test_pad.c` und `tests/test_file_mods.c` binden `src/runtime_pad.c` bzw. `src/runtime_file.c` direkt ein; diese
rufen seit R4/R2c `pf_fs_*` auf, die Testzeilen in build.sh linkten die Plattformschicht aber nicht mit
(Windows-Zweig nimmt `libpf.a`). Fix (Opus): `src/platform/pf_fs_linux.c` in beiden Testzeilen von build.sh.

| alt (build.sh) | neu | Linux-Gleichheit |
|---|---|---|
| :90 `tests/test_pad.c "${libraries[@]}"` | `tests/test_pad.c src/platform/pf_fs_linux.c "${libraries[@]}"` | nur Testprogramm, Spiel-Exe unverändert |
| :94 `tests/test_file_mods.c -o …` | `tests/test_file_mods.c src/platform/pf_fs_linux.c -o …` | nur Testprogramm |

Verhaltensabweichung Linux: keine (nur Test-Link-Zeilen; `out/sacredfire` und run.sh unverändert)

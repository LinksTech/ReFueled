# PUB2 – Namensentfernung in der Commit-Historie, publish_check prüft Autorennamen (2026-10-14)

Ausgeführt und geprüft von Opus, Auftrag der Projektleitung.

- Der einzige vorher schon öffentliche Commit (GitHub-Weboberfläche, „Create Redme.md“) trug einen
  persönlichen Namen als Autor. Er ist einmalig mit der Platzhalter-Identität `ReFueled <refueled@localhost>`
  neu geschrieben (gleicher Baum, gleiche Zeitstempel, gleiche Nachricht, ohne GitHub-Signatur); die
  13 späteren Commits sind unverändert umgehängt. Danach ein einmaliger force push (freigegeben von der
  Projektleitung). Kein Dokument und keine Commit-Nachricht verwies auf die geänderten Hashes.
- Lücke in publish_check.py: Bei Commits wurden nur die E-Mail-Adressen von Autor/Committer geprüft,
  nicht die Namen. Jetzt laufen auch Autor- und Committer-Name gegen die privaten Begriffe
  (`.publish-private.txt`, Treffer = Push blockiert). Gegenprobe: der alte Commit liefert 3 Treffer
  `private`, die neue Historie ist sauber.

Verhaltensabweichung Linux: keine (nur Prüfwerkzeug, kein Spiel- oder Build-Code)

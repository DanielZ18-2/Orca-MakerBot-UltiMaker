#!/bin/bash
set -e
if pgrep -f "orca-slicer" >/dev/null; then
  echo "FEHLER: Orca laeuft noch - schliessen, dann erneut bauen."; exit 1
fi
cd ~/Orca_Dev
cmake --build build --config Release --target OrcaSlicer
echo "OK: gebaut ($(date +%H:%M:%S)). package/bin ist Symlink - nichts zu kopieren."

# ---------------------------------------------------------------------------
# Profile an BEIDE Stellen spiegeln.
#
# 1) ~/.config/OrcaSlicer/system - was der laufende Orca liest.
#    Ohne das sieht Orca die Repo-Aenderungen NIE (Stand vor diesem
#    Zusatz: Profile vom 21.07.).
#
# 2) build/package/resources/profiles - die im Paket mitgelieferten Profile.
#    ACHTUNG: package/resources ist KEIN Symlink, nur package/bin ist einer.
#    Der Build-Target OrcaSlicer kopiert resources nicht mit; das passiert nur
#    im Install-/Package-Schritt, den wir hier nicht fahren. Ohne diesen Block
#    bleibt der Ordner auf dem Stand des letzten vollen Builds stehen
#    (gefunden 2026-08-16: 29. Juni) und ist damit die Quelle, aus der Orca
#    beim naechsten Versions-Bump den Systemordner ueberschreibt.
#
# Kein Symlink fuer package/resources: dort liegen erzeugte Dateien, die es im
# Repo nicht gibt - z.B. i18n/*/OrcaSlicer.mo (.mo ist gitignoriert).
# ---------------------------------------------------------------------------
SYS=~/.config/OrcaSlicer/system
PKG=~/Orca_Dev/build/package/resources/profiles
SRC=~/Orca_Dev/resources/profiles

for V in MakerBot UltiMaker; do
  rm -rf "$SYS/$V"
  cp -r "$SRC/$V" "$SYS/"
  cp "$SRC/$V.json" "$SYS/"

  rm -rf "$PKG/$V"
  cp -r "$SRC/$V" "$PKG/"
  cp "$SRC/$V.json" "$PKG/"
done
echo "OK: Profile gespiegelt nach $SYS"
echo "OK: Profile gespiegelt nach $PKG"

# Gegenprobe: beide Kopien muessen dieselbe Vendor-Version tragen wie das Repo.
for V in MakerBot UltiMaker; do
  A=$(python3 -c "import json;print(json.load(open('$SRC/$V.json'))['version'])")
  B=$(python3 -c "import json;print(json.load(open('$SYS/$V.json'))['version'])")
  C=$(python3 -c "import json;print(json.load(open('$PKG/$V.json'))['version'])")
  if [ "$A" = "$B" ] && [ "$A" = "$C" ]; then
    echo "OK: $V $A (Repo = System = Paket)"
  else
    echo "WARNUNG: $V Versionen weichen ab - Repo $A, System $B, Paket $C"
  fi
done

# ---------------------------------------------------------------------------
# Uebersetzungen an BEIDE Stellen bringen - dieselbe Falle wie oben.
#
# Gefunden 2026-09-22: build/package/resources/i18n stand auf dem 29. Juni,
# genau wie die Profile es vor dem Block darueber taten. 5504 Eintraege im
# Paket gegen 5607 in der Quelle - 103 Texte, die die gebaute Anwendung nicht
# kennt, darunter der ganze Birdwing-Kopplungsdialog.
#
# Der Bau erzeugt die .mo NICHT; das passiert nur im Install-/Package-Schritt,
# den wir hier nicht fahren. msgfmt uebersetzt
# localization/i18n/<L>/OrcaSlicer_<L>.po nach resources/i18n/<L>/OrcaSlicer.mo.
# Die .mo sind gitignoriert - dieser Lauf ist die einzige Stelle, die sie
# aktuell haelt. Wer einen _L()-Text aendert und das hier auslaesst, aendert
# die msgid und verliert die Uebersetzung in jeder Sprache, ohne Warnung.
# ---------------------------------------------------------------------------
PO=~/Orca_Dev/localization/i18n
MO=~/Orca_Dev/resources/i18n
PKGI=~/Orca_Dev/build/package/resources/i18n

if ! command -v msgfmt >/dev/null; then
  echo "WARNUNG: msgfmt fehlt (sudo apt-get install -y gettext) - Kataloge bleiben alt."
else
  N=0
  for D in "$PO"/*/; do
    L=$(basename "$D")
    [ -f "$D/OrcaSlicer_$L.po" ] || continue
    mkdir -p "$MO/$L" "$PKGI/$L"
    msgfmt -o "$MO/$L/OrcaSlicer.mo" "$D/OrcaSlicer_$L.po"
    cp "$MO/$L/OrcaSlicer.mo" "$PKGI/$L/OrcaSlicer.mo"
    N=$((N+1))
  done
  echo "OK: $N Kataloge uebersetzt und nach $PKGI gespiegelt"

  # Gegenprobe: Quelle und Paket muessen gleich viele Eintraege tragen.
  Z='import gettext,sys;print(len(gettext.GNUTranslations(open(sys.argv[1],"rb"))._catalog))'
  A=$(python3 -c "$Z" "$MO/de/OrcaSlicer.mo")
  B=$(python3 -c "$Z" "$PKGI/de/OrcaSlicer.mo")
  if [ "$A" = "$B" ]; then
    echo "OK: Katalog de $A Eintraege (Quelle = Paket)"
  else
    echo "WARNUNG: Katalog de weicht ab - Quelle $A, Paket $B"
  fi
fi

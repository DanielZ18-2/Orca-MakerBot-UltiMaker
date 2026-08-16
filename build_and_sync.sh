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

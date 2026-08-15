#!/bin/bash
set -e
if pgrep -f "orca-slicer" >/dev/null; then
  echo "FEHLER: Orca laeuft noch - schliessen, dann erneut bauen."; exit 1
fi
cd ~/Orca_Dev
cmake --build build --config Release --target OrcaSlicer
echo "OK: gebaut ($(date +%H:%M:%S)). package/bin ist Symlink - nichts zu kopieren."

# Profile in Orcas Systemordner spiegeln - ohne das sieht Orca die
# Repo-Aenderungen NIE (Stand vor diesem Zusatz: Profile vom 21.07.).
SYS=~/.config/OrcaSlicer/system
for V in MakerBot UltiMaker; do
  rm -rf "$SYS/$V"
  cp -r ~/Orca_Dev/resources/profiles/$V "$SYS/"
  cp ~/Orca_Dev/resources/profiles/$V.json "$SYS/"
done
echo "OK: Profile gespiegelt nach $SYS"

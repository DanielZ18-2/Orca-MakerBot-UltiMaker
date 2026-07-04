#!/bin/bash
set -e
if pgrep -f "orca-slicer" >/dev/null; then
  echo "FEHLER: Orca laeuft noch - schliessen, dann erneut bauen."; exit 1
fi
cd ~/Orca_Dev
cmake --build build --config Release --target OrcaSlicer
echo "OK: gebaut ($(date +%H:%M:%S)). package/bin ist Symlink - nichts zu kopieren."

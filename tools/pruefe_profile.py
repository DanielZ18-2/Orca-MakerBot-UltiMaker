#!/usr/bin/env python3
"""
Profil-Verifikation fuer den MakerBot/UltiMaker-Satz.
Ausfuehren aus resources/profiles/ heraus:  python3 tools/pruefe_profile.py
Exit-Code 1, sobald eine Pruefung anschlaegt.
"""
import json, os, sys, glob, collections

FAIL = 0
VENDORS = ["MakerBot", "UltiMaker"]
# Keys, die sich zwischen den beiden Method-Kopien unterscheiden DUERFEN
VENDOR_KEYS = {
    "name", "setting_id", "inherits", "from", "instantiation", "compatible_printers",
    "model_id", "vendor", "printer_vendor", "family", "bed_model", "bed_texture",
    "default_materials", "default_filament_profile", "default_print_profile",
    "printer_model", "sub_path",
}
# Keys, die erst in Stufe 2/3 angeglichen werden (mit Testbeleg)
DEFERRED = {"filament_retraction_length", "filament_max_volumetric_speed"}


def head(t):
    print(f"\n=== {t}")


def ok(msg):
    print(f"  OK   {msg}")


def bad(msg):
    global FAIL
    FAIL += 1
    print(f"  FAIL {msg}")


def load_all():
    out = {}
    for v in VENDORS:
        for f in glob.glob(f"{v}/**/*.json", recursive=True):
            try:
                out[f] = json.load(open(f, encoding="utf-8"))
            except Exception as e:
                bad(f"JSON defekt: {f}: {e}")
    return out


A = load_all()
BY = lambda t: {d.get("name"): (f, d) for f, d in A.items() if d.get("type") == t}
FIL, MACH, MODEL, PROC = BY("filament"), BY("machine"), BY("machine_model"), BY("process")

head("1  JSON-Validitaet")
ok(f"{len(A)} Profildateien geladen, 0 defekt") if FAIL == 0 else None

head("2  Index gegen Dateibestand (beide Richtungen)")
for v in VENDORS:
    idx = json.load(open(f"{v}.json", encoding="utf-8"))
    for key, typ in [("machine_list", "machine"), ("process_list", "process"),
                     ("filament_list", "filament"), ("machine_model_list", "machine_model")]:
        entries = idx.get(key, [])
        for e in entries:
            if not os.path.exists(os.path.join(v, e["sub_path"])):
                bad(f"{v}.json {key}: Datei fehlt fuer {e['name']}")
        names = {e["name"] for e in entries}
        disk = {d["name"] for f, d in A.items() if f.startswith(v + "/")
                and d.get("type") == typ and str(d.get("instantiation", "")).lower() == "true"}
        for n in sorted(disk - names):
            bad(f"{v}.json {key}: {n} liegt auf Disk, ist aber nicht indiziert")
        ok(f"{v}.json {key}: {len(entries)} Eintraege deckungsgleich")

head("3  default_materials -> existierende Filamentprofile")
ghost = 0
for n, (f, d) in sorted(MODEL.items()):
    for m in [x.strip() for x in d.get("default_materials", "").split(";") if x.strip()]:
        if m not in FIL:
            bad(f"{n}: verweist auf nicht existierendes Profil '{m}'")
            ghost += 1
if not ghost:
    ok(f"{len(MODEL)} Modelle, 0 Verweise ins Leere")

head("4  Filamentprofile ohne Modellzuordnung ('undefined')")
listed = set()
for d in MODEL.values():
    listed |= {x.strip() for x in d[1].get("default_materials", "").split(";") if x.strip()}
orph = [n for n, (f, d) in FIL.items()
        if str(d.get("instantiation", "")).lower() == "true" and n not in listed]
bad(f"nicht zugeordnet: {orph}") if orph else ok(f"alle {len(listed)} instanziierten Filamentprofile zugeordnet")

head("5  compatible_printers -> existierende Maschinen")
miss = 0
for n, (f, d) in sorted(list(FIL.items()) + list(PROC.items())):
    for p in d.get("compatible_printers", []) or []:
        if p not in MACH:
            bad(f"{n}: compatible_printers -> unbekannte Maschine '{p}'")
            miss += 1
if not miss:
    ok("alle compatible_printers aufloesbar")

head("6  inherits aufloesbar")
allnames = set(A_n for A_n in [d.get("name") for d in A.values()])
unres = 0
for f, d in sorted(A.items()):
    par = d.get("inherits")
    if par and par not in allnames:
        bad(f"{d.get('name')}: inherits -> '{par}' existiert nicht")
        unres += 1
if not unres:
    ok("0 unaufgeloeste inherits-Ziele")

head("7  filament_diameter")
c = collections.Counter()
for n, (f, d) in FIL.items():
    if str(d.get("instantiation", "")).lower() != "true":
        continue
    v = d.get("filament_diameter")
    c[(f.split("/")[0], str(v))] += 1
    if v is None:
        bad(f"{n}: filament_diameter nicht gesetzt")
for k, v in sorted(c.items()):
    print(f"       {k[0]:10s} {k[1]:10s} {v:3d}")
for n, (f, d) in FIL.items():
    dia = d.get("filament_diameter")
    if f.startswith("MakerBot") and dia not in (None, ["1.75"]):
        bad(f"{n}: MakerBot-Profil mit {dia}, erwartet 1.75")
    if f.startswith("UltiMaker") and "Method" not in (n or "") and dia not in (None, ["2.85"]):
        bad(f"{n}: UltiMaker-Profil mit {dia}, erwartet 2.85")
ok("Durchmesser konsistent (MakerBot-Linie 1.75, UltiMaker 2.85, UltiMaker-Method 1.75)")

head("8  Method-Dubletten inhaltsgleich (ausser Vendor- und Stufe-2/3-Keys)")
mb, um = {}, {}
for f, d in A.items():
    n = d.get("name") or ""
    if "Method" not in n:
        continue
    # Vendorname kann am Anfang ODER hinter '@' stehen -> ueberall entfernen,
    # damit "Generic ABS @MakerBot Method" und "... @UltiMaker Method" paaren.
    key = n.replace("MakerBot ", "").replace("UltiMaker ", "")
    (mb if f.startswith("MakerBot") else um)[key] = d
diff = collections.Counter()
for k in sorted(set(mb) & set(um)):
    a, b = mb[k], um[k]
    for key in (set(a) | set(b)) - VENDOR_KEYS:
        if a.get(key) != b.get(key):
            diff[key] += 1
for key, cnt in diff.most_common():
    (print(f"       offen fuer Stufe 2/3: {key} ({cnt}x)") if key in DEFERRED
     else bad(f"Method-Dublette weicht ab: {key} ({cnt}x)"))
if not (set(diff) - DEFERRED):
    ok(f"{len(set(mb) & set(um))} Namenspaare strukturell identisch")

head("9  extruder_type")
for n, (f, d) in sorted(MACH.items()):
    if str(d.get("instantiation", "")).lower() != "true":
        continue
    if d.get("extruder_type") is None:
        bad(f"{n}: extruder_type nicht gesetzt")
et = collections.Counter((f.split("/")[0], str(d.get("extruder_type"))) for n, (f, d) in MACH.items()
                         if str(d.get("instantiation", "")).lower() == "true")
for k, v in sorted(et.items()):
    print(f"       {k[0]:10s} {k[1]:34s} {v:3d}")

head("10 Doppelte Profilnamen (je Vendor)")
# Basisprofile wie fdm_filament_pla existieren absichtlich in beiden Vendors -
# doppelt waere nur INNERHALB eines Vendors ein Fehler.
for v in VENDORS:
    cnt = collections.Counter(d.get("name") for f, d in A.items() if f.startswith(v + "/"))
    dups = [n for n, k in cnt.items() if k > 1]
    bad(f"{v}: doppelte Namen: {dups}") if dups else ok(f"{v}: 0 doppelte Profilnamen")

print(f"\n{'='*60}\n{'BESTANDEN' if FAIL == 0 else str(FAIL) + ' BEANSTANDUNGEN'}\n{'='*60}")
sys.exit(1 if FAIL else 0)

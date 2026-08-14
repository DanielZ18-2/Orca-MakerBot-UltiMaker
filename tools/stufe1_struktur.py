#!/usr/bin/env python3
"""
Stufe 1 - rein strukturelle Korrekturen an den MakerBot/UltiMaker-Profilen.

Kein Eingriff in Geschwindigkeiten, Retraction oder Volumenstrom bestehender
Profile. Ausfuehren aus resources/profiles/ heraus:

    python3 tools/stufe1_struktur.py --apply
    python3 tools/stufe1_struktur.py            # nur Bericht, keine Aenderung

Belege je Massnahme stehen als Kommentar am jeweiligen Block.
"""
import json, os, sys, glob, copy, collections

APPLY = "--apply" in sys.argv
CH = collections.Counter()
LOG = []


def rd(p):
    with open(p, encoding="utf-8") as f:
        return json.load(f)


def wr(p, d):
    if not APPLY:
        return
    if os.path.dirname(p):
        os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, "w", encoding="utf-8") as f:
        json.dump(d, f, indent=4, ensure_ascii=False)
        f.write("\n")


def note(tag, msg):
    CH[tag] += 1
    LOG.append(f"  [{tag}] {msg}")


def scan(vendor, typ):
    out = {}
    for f in glob.glob(f"{vendor}/**/*.json", recursive=True):
        d = rd(f)
        if d.get("type") == typ:
            out[d.get("name")] = (f, d)
    return out


# ---------------------------------------------------------------- S1.1
# Sketch: filament_diameter 1.77 -> 1.75
# Beleg: Cura ultimaker_sketch.def.json / ultimaker_sketch_sprint.def.json
#        material_diameter = 1.75. Sketch wurde nie mit grue gesliced, der
#        1.77-Kalibrierwert der MakerBot-Slicer gilt dort nicht. Ausserdem
#        Projektentscheidung: der gesamte Satz rechnet mit 1,75-mm-Filament.
def s1_1():
    for f in sorted(glob.glob("MakerBot/filament/Sketch/*.json")):
        d = rd(f)
        if d.get("filament_diameter") == ["1.77"]:
            d["filament_diameter"] = ["1.75"]
            wr(f, d)
            note("S1.1", f"{d['name']}: 1.77 -> 1.75")


# ---------------------------------------------------------------- S1.2
# UltiMaker-Method-Filamente: filament_diameter fehlt -> 1.75
# Beleg: die inhaltsgleiche MakerBot-Kopie setzt 1.75; ohne Wert greift
#        Orcas Default 1.75 zwar zufaellig richtig, die Deklaration fehlt aber.
#        Cura ultimaker_method_base.def.json: material_diameter = 1.75.
def s1_2():
    mb = {n.replace("MakerBot ", "", 1): v for n, v in scan("MakerBot", "filament").items()
          if n and "Method" in n}
    for n, (f, d) in sorted(scan("UltiMaker", "filament").items()):
        if not n or "Method" not in n:
            continue
        key = n.replace("UltiMaker ", "", 1)
        if d.get("filament_diameter") is None and key in mb:
            d["filament_diameter"] = mb[key][1].get("filament_diameter", ["1.75"])
            wr(f, d)
            note("S1.2", f"{n}: filament_diameter ergaenzt {d['filament_diameter']}")


# ---------------------------------------------------------------- S1.3
# Method-Dubletten strukturell angleichen (MakerBot-Kopie ist fuehrend).
# Nur Keys ohne Einfluss auf Fluss/Geschwindigkeit. Verhaltens-Keys
# (filament_retraction_length, filament_max_volumetric_speed) bleiben hier
# bewusst unangetastet - die kommen in Stufe 2/3 mit Testbeleg.
SYNC_KEYS = ["default_nozzle_volume_type", "gpx_machine_type", "machine_start_gcode"]


def s1_3():
    mb = {}
    for n, (f, d) in scan("MakerBot", "machine").items():
        if n and "Method" in n:
            mb[n.replace("MakerBot ", "", 1)] = d
    for n, (f, d) in sorted(scan("UltiMaker", "machine").items()):
        if not n or "Method" not in n:
            continue
        key = n.replace("UltiMaker ", "", 1)
        src = mb.get(key)
        if not src:
            continue
        changed = False
        for k in SYNC_KEYS:
            if k in src and d.get(k) != src[k]:
                d[k] = copy.deepcopy(src[k])
                changed = True
                note("S1.3", f"{n}: {k} an MakerBot-Kopie angeglichen")
        if changed:
            wr(f, d)


# ---------------------------------------------------------------- S1.4
# UltiMaker 2: Reste der Orca-Upstream-Definition bereinigen.
#  - printer_settings_id "Qidi" ist ein Fremd-Erbe der Upstream-Datei
#  - 0.4 nozzle fehlt extruder_type; die drei Schwestervarianten haben Bowden
#  - inherits fdm_machine_common -> fdm_ultimaker_common (einziger Effekt:
#    printer_variant wird gesetzt, sonst 0 Wertaenderungen - geprueft)
#  - model_id/family weichen als einzige aller 19 UltiMaker-Modelle vom
#    Schema ab ("UltiMaker-2"/"UltiMaker" statt "UltiMaker 2"/"Classic")
def s1_4():
    for n, (f, d) in sorted(scan("UltiMaker", "machine").items()):
        if not n or not n.startswith("UltiMaker 2 ") or "nozzle" not in n:
            continue
        ch = False
        if d.get("printer_settings_id") == "Qidi":
            d["printer_settings_id"] = ""
            ch = True
            note("S1.4", f"{n}: printer_settings_id 'Qidi' -> ''")
        if d.get("extruder_type") is None:
            d["extruder_type"] = ["Bowden"]
            ch = True
            note("S1.4", f"{n}: extruder_type ergaenzt ['Bowden']")
        if d.get("inherits") == "fdm_machine_common":
            d["inherits"] = "fdm_ultimaker_common"
            ch = True
            note("S1.4", f"{n}: inherits -> fdm_ultimaker_common")
        if ch:
            wr(f, d)
    for n, (f, d) in scan("UltiMaker", "machine_model").items():
        if n != "UltiMaker 2":
            continue
        ch = False
        if d.get("model_id") != "UltiMaker 2":
            d["model_id"] = "UltiMaker 2"
            ch = True
            note("S1.4", "machine_model UltiMaker 2: model_id -> 'UltiMaker 2'")
        if d.get("family") != "Classic":
            d["family"] = "Classic"
            ch = True
            note("S1.4", "machine_model UltiMaker 2: family -> 'Classic'")
        if ch:
            wr(f, d)


# ---------------------------------------------------------------- S1.5
# UltiMaker 2: die 5 in default_materials gelisteten Filamentprofile fehlen
# komplett -> Verweise laufen ins Leere, das Modell hat keine Materialauswahl.
# Vorlage: die inhaltlich passenden @UltiMaker 2+ Profile (gleiche Bauart,
# Bowden, 2,85 mm), nur Name/setting_id/compatible_printers angepasst.
UM2_PRINTERS = [f"UltiMaker 2 {s} nozzle" for s in ("0.25", "0.4", "0.6", "0.8")]
UM2_MATS = ["Generic PLA", "Generic Silk PLA", "Generic PETG", "Generic ABS", "Generic ASA"]


def s1_5():
    fil = scan("UltiMaker", "filament")
    new = []
    for mat in UM2_MATS:
        src = fil.get(f"{mat} @UltiMaker 2+")
        if not src:
            note("WARN", f"Vorlage {mat} @UltiMaker 2+ nicht gefunden")
            continue
        name = f"{mat} @UltiMaker 2"
        if name in fil:
            continue
        d = copy.deepcopy(src[1])
        d["name"] = name
        d["setting_id"] = name
        d["compatible_printers"] = list(UM2_PRINTERS)
        sub = f"filament/Classic/{name}.json"
        wr(os.path.join("UltiMaker", sub), d)
        new.append({"name": name, "sub_path": sub})
        note("S1.5", f"neu angelegt: {name} (Vorlage: {mat} @UltiMaker 2+)")
    return new


# ---------------------------------------------------------------- S1.6
# Legacy: vier default_materials-Verweise auf nicht existierende PETG-Profile.
# Vorlage: Generic PETG @MakerBot Replicator 2 (240 C, Bett 70 C, 1,75 mm).
# filament_max_volumetric_speed nach dem im Satz bereits verwendeten
# Klassenverhaeltnis: Replicator-Klasse 5.9, Thing-O-Matic-Klasse 4.2
# (TOM/Replicator liegt bei PLA und ABS durchgaengig bei 0,72).
LEGACY_PETG = {
    "MakerBot Replicator Original": ("5.9", "Legacy"),
    "MakerBot Replicator Original Dual": ("5.9", "Legacy"),
    "MakerBot Thing-O-Matic ABP": ("4.2", "Legacy"),
    "MakerBot Thing-O-Matic Acrylic": ("4.2", "Legacy"),
}


def s1_6():
    fil = scan("MakerBot", "filament")
    mach = scan("MakerBot", "machine")
    src = fil.get("Generic PETG @MakerBot Replicator 2")
    if not src:
        note("WARN", "Vorlage Generic PETG @MakerBot Replicator 2 fehlt")
        return []
    new = []
    for model, (vol, folder) in LEGACY_PETG.items():
        name = f"Generic PETG @{model}"
        if name in fil:
            continue
        # Zuordnung ueber printer_model, nicht ueber Namenspraefix:
        # "MakerBot Replicator Original " matcht sonst auch "... Original Dual".
        printers = sorted(n for n, (_, md) in mach.items()
                          if md.get("printer_model") == model and "nozzle" in (n or ""))
        if not printers:
            note("WARN", f"keine Maschinen fuer {model}")
            continue
        d = copy.deepcopy(src[1])
        d["name"] = name
        d["setting_id"] = name
        d["compatible_printers"] = printers
        d["filament_max_volumetric_speed"] = [vol]
        sub = f"filament/{folder}/{name}.json"
        wr(os.path.join("MakerBot", sub), d)
        new.append({"name": name, "sub_path": sub})
        note("S1.6", f"neu angelegt: {name} ({len(printers)} Maschinen, vol={vol})")
    return new


# ---------------------------------------------------------------- S1.7
# Index-JSONs mitpflegen: neue Filamentprofile registrieren.
def s1_7(vendor, entries):
    if not entries:
        return
    p = f"{vendor}.json"
    idx = rd(p)
    have = {e["name"] for e in idx["filament_list"]}
    # Bewusst nur anhaengen, nicht sortieren: die vorhandene Liste folgt der
    # Erzeugungsreihenfolge, ein Umsortieren wuerde einen 350-Zeilen-Diff
    # ohne inhaltlichen Wert erzeugen.
    for e in entries:
        if e["name"] not in have:
            idx["filament_list"].append(e)
            note("S1.7", f"{p}: filament_list += {e['name']}")
    wr(p, idx)


def main():
    if not (os.path.isdir("MakerBot") and os.path.isdir("UltiMaker")):
        sys.exit("Bitte aus resources/profiles/ heraus starten.")
    s1_1(); s1_2(); s1_3(); s1_4()
    um_new = s1_5()
    mb_new = s1_6()
    s1_7("UltiMaker", um_new)
    s1_7("MakerBot", mb_new)
    print("\n".join(LOG))
    print("\n== Zusammenfassung ==")
    for k, v in sorted(CH.items()):
        print(f"  {k}: {v}")
    print(f"  GESAMT: {sum(CH.values())} Aenderungen" + ("" if APPLY else "  (DRY RUN - nichts geschrieben)"))


main()

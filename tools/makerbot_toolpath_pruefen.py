#!/usr/bin/env python3
"""
Prueft eine vom Fork erzeugte .makerbot-Datei auf Extrusionsfehler im Toolpath.

    python3 makerbot_toolpath_pruefen.py datei.makerbot

Meldet:
  - stationaere Extrusionen (Materialausstoss ohne Bewegung -> Blobs)
  - Retraction/Restart-Bilanz
  - effektive Bahnbreite je Feature-Typ
  - Abweichung zwischen Toolpath und meta.json
"""
import zipfile, json, math, sys, collections

if len(sys.argv) < 2:
    sys.exit(__doc__)

z = zipfile.ZipFile(sys.argv[1])
meta = json.loads(z.read("meta.json"))
tp = json.loads(z.read("print.jsontoolpath"))

mc = meta["machine_config"]["extruder_profiles"]
prof = next(iter(next(iter(mc.values()))["materials"].values()))
D = prof["feed_diameter"]
LH = meta["miracle_config"]["gaggles"]["default"]["layerHeight"]
AREA = math.pi * (D / 2) ** 2

print(f"Datei      : {sys.argv[1]}")
print(f"Bot/Extruder: {meta['bot_type']} / {meta['tool_type']}  Material {meta['material']}")
print(f"feed_diameter {D} mm, layerHeight {LH} mm\n")

pos = {"x": None, "y": None, "z": None}
st = collections.defaultdict(lambda: {"len": 0.0, "e": 0.0, "n": 0, "se": 0.0, "sn": 0})
for c in tp:
    cmd = c["command"]
    if cmd["function"] != "move":
        continue
    p, tag = cmd["parameters"], tuple(cmd.get("tags", [])) or ("<ohne>",)
    nx, ny, nz = p.get("x"), p.get("y"), p.get("z")
    dl = 0.0
    if None not in (pos["x"], pos["y"]) and None not in (nx, ny):
        dl = math.hypot(nx - pos["x"], ny - pos["y"])
    de = p.get("a", 0.0) if cmd["metadata"]["relative"]["a"] else 0.0
    s = st[tag]
    s["len"] += dl; s["e"] += de; s["n"] += 1
    if dl < 1e-9 and de > 1e-9:
        s["se"] += de; s["sn"] += 1
    if nx is not None: pos["x"] = nx
    if ny is not None: pos["y"] = ny
    if nz is not None: pos["z"] = nz

print(f"{'Tag':32s} {'n':>7s} {'Weg mm':>10s} {'E mm':>9s} {'Bahnbreite':>11s}")
blob_e = blob_n = 0.0, 0
blob_e, blob_n = 0.0, 0
tot = 0.0
for tag, s in sorted(st.items(), key=lambda kv: -kv[1]["n"]):
    tot += s["e"]
    if s["len"] > 1e-6:
        # Bahn mit runden Enden: A = w*h - h^2*(1-pi/4)
        a_cross = (s["e"] - s["se"]) / s["len"] * AREA
        w = (a_cross + LH * LH * (1 - math.pi / 4)) / LH
        wtxt = f"{w:11.3f}"
    else:
        wtxt = f"{'-':>11s}"
    print(f"{str(tag)[:32]:32s} {s['n']:7d} {s['len']:10.1f} {s['e']:9.2f} {wtxt}")
    if s["sn"]:
        print(f"{'':32s}   stationaer: {s['se']:.2f} mm in {s['sn']} Befehlen")
        blob_e += s["se"]; blob_n += s["sn"]

ret = sum(v["e"] for k, v in st.items() if "Retract" in k)
res = sum(v["e"] for k, v in st.items() if "Restart" in k)
print(f"\nRetraction gesamt : {ret:9.2f} mm")
print(f"Restart gesamt    : {res:9.2f} mm")
print(f"Netto-Bilanz      : {ret + res:9.2f} mm  (sollte nahe 0 sein)")
print(f"\nStationaere Extrusion (Blobs): {blob_e:.2f} mm in {blob_n} Befehlen")
if tot:
    print(f"  = {blob_e / tot * 100:.1f} % der Gesamtextrusion, {blob_e * AREA:.0f} mm3 Material")
    if blob_n:
        print(f"  = {blob_e / blob_n * AREA:.3f} mm3 pro Punkt "
              f"(Kugeldurchmesser {(6 * blob_e / blob_n * AREA / math.pi) ** (1/3):.2f} mm)")
print(f"\nToolpath gesamt   : {tot:9.2f} mm")
print(f"meta.json meldet  : {meta['extrusion_distance_mm']:9.2f} mm")
d = tot - meta["extrusion_distance_mm"]
print(f"Abweichung        : {d:9.2f} mm ({d / meta['extrusion_distance_mm'] * 100:+.1f} %)")

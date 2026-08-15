#!/usr/bin/env python3
"""
Regressionstest fuer gcode_to_birdwing_jsontoolpath (MakerBotToolpath.cpp).

Bildet die Klassifizierungslogik des Konverters in Python nach und vergleicht
den Stand VOR und NACH dem Fix an einem G-Code-Ausschnitt, wie Orca ihn fuer
gcfMakerBotBirdwing erzeugt (relatives E, Travels ohne E).

Erwartung nach dem Fix:
  - Travels tragen Tag "Travel Move" und a = 0
  - die De-Retraktion nach dem Travel traegt Tag "Restart"
  - Gesamtextrusion = Summe der E-Werte aus dem G-Code
"""
import math, sys

GCODE = """
;TYPE:Outer wall
;WIDTH:0.42
G1 X10 Y10 E0.29 F2400
G1 X20 Y10 E0.29
G1 E-0.5 F3000
G1 X60 Y50 F9000
G1 E0.6 F1800
G1 X70 Y50 E0.29 F2400
;TYPE:Sparse infill
;WIDTH:0.45
G1 X80 Y50 E0.31 F6600
G1 E-0.5 F3000
G1 X20 Y80 F9000
G1 E0.6 F1800
G1 X30 Y80 E0.29 F2400
""".strip().splitlines()

TAGMAP = {"Outer wall": "Outline", "Inner wall": "Inset",
          "Sparse infill": "Infill", "Internal solid infill": "Infill"}


def convert(gcode, fixed):
    x = y = 0.0
    tag_cur = "Outline"
    layer_w = 0.4
    retracted = False
    seen_e = False
    out = []
    for raw in gcode:
        line, _, comment = raw.partition(";")
        if comment.startswith("TYPE:"):
            tag_cur = TAGMAP.get(comment[5:].strip(), "Infill")
            continue
        if comment.startswith("WIDTH:"):
            layer_w = float(comment[6:])
            continue
        t = line.split()
        if not t or t[0] not in ("G0", "G1"):
            continue
        nx, ny, e = x, y, None
        for p in t[1:]:
            if p[0] == "X": nx = float(p[1:])
            elif p[0] == "Y": ny = float(p[1:])
            elif p[0] == "E": e = float(p[1:]); seen_e = True
        has_xy = nx != x or ny != y
        has_e = e is not None
        dist = math.hypot(nx - x, ny - y)

        if not has_xy and has_e and e < -1e-4:
            out.append(("Retract", 0.0, e)); retracted = True
        elif not has_xy and has_e and e > 1e-4 and retracted:
            out.append(("Restart", 0.0, e)); retracted = False
        elif has_e and e > 1e-6:
            tg = "Restart" if retracted else (
                "Trailing Extrusion Move" if dist < 0.5 else tag_cur)
            retracted = False
            out.append((tg, dist, e))
        elif not has_e and has_xy and (not seen_e if fixed else True):
            # Geometrie-Fallback (vor dem Fix: immer, nach dem Fix: nur ohne E)
            tg = "Restart" if retracted else tag_cur
            retracted = False
            a = dist * 0.2 * layer_w / (math.pi * 0.885 ** 2)
            out.append((tg, dist, a))
        elif not has_e and has_xy:
            out.append(("Travel Move", dist, 0.0))   # nur im Fix-Zweig erreichbar
        elif has_e and e > 1e-4:
            out.append(("Trailing Extrusion Move", dist, e))
        else:
            x, y = nx, ny
            continue
        # merken, ob der zugrunde liegende G-code ein E trug
        out[-1] = out[-1] + (has_e,)
        x, y = nx, ny
    return out


gcode_e = sum(float(p[1:]) for l in GCODE for p in l.split()
              if p.startswith("E") and l.split()[0] in ("G0", "G1"))

ok = True
for name, fixed in [("VORHER (Stand v2.4.2-makerbot-test1)", False), ("NACHHER (mit Fix)", True)]:
    r = convert(GCODE, fixed)
    # Travel = Bewegung, deren G-code KEIN E trug
    trav_e = sum(a for t, d, a, he in r if not he and d > 1e-9)
    print(f"\n=== {name}")
    for t, d, a, he in r:
        mark = "   <== Travel, extrudiert!" if (not he and a > 1e-9) else ""
        print(f"   {t:26s} Weg={d:6.2f}  a={a:+.4f}{mark}")
    print(f"   Summe a          = {sum(a for _,_,a,_ in r):+.4f} mm  (G-code: {gcode_e:+.4f})")
    print(f"   Extrusion auf Travels = {trav_e:.4f} mm")
    if fixed:
        tags = [t for t, _, _, _ in r]
        checks = [
            ("Travels als 'Travel Move'", tags.count("Travel Move") == 2),
            ("keine Extrusion auf Travels", abs(trav_e) < 1e-9),
            ("De-Retraktion als 'Restart'", tags.count("Restart") == 2),
            ("Bilanz = G-code", abs(sum(a for _, _, a, _ in r) - gcode_e) < 1e-9),
        ]
        print()
        for label, good in checks:
            print(f"   [{'OK ' if good else 'FEHLER'}] {label}")
            ok &= good

sys.exit(0 if ok else 1)

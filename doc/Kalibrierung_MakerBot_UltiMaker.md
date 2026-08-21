# Filamentkalibrierung auf MakerBot- und UltiMaker-Druckern

Stand 21.08.2026 · gilt für den Fork
`DanielZ18-2/Orca-MakerBot-UltiMaker`, Zweig
`feature/makerbot-ultimaker-native-support`

> **Für den Pull Request:** Dieses Dokument muss vor der Einreichung ins
> Englische übertragen werden. Upstream-Dokumentation unter `doc/` ist
> durchgehend englisch.

---

## 0. Zuerst das Unangenehme: drei Menüpunkte tun hier nichts

Orcas Kalibriermenü bietet Verfahren an, die auf **keiner** Maschine dieses
Forks wirken. Sie erzeugen einen Prüfkörper, der Nutzer misst ihn aus, trägt
einen Wert ein — und der Wert erreicht den Drucker nie.

| Verfahren | Verlässt sich auf | Warum es hier ausfällt |
|---|---|---|
| **Pressure Advance** (Line / Pattern / Tower) | `M900 K…` | Der MakerBot-Konverter kennt genau 13 Befehle (§ 2); `M900` gehört nicht dazu und wird still verworfen. GPX kennt 59 M-Codes, `M900` ist keiner davon. |
| **Input Shaping** (Frequenz, Dämpfung) | `M593` | Weder GPX noch der Werkzeugwegkonverter kennen ihn. Keine der erfassten Firmwares führt einen Eingangsformer. |
| **Cornering / Junction Deviation** | `M204`, `M205` | Beide werden verworfen. Auf Legacy zusätzlich belegt: `ACCELERATION_ACTIVE` steht im EEPROM, nicht in der Datei. |

**Es gibt einen Ersatz, aber nur für Legacy mit Sailfish:** Der Druckvorhalt
heißt dort `JKN_ADVANCE_K` und `JKN_ADVANCE_K2` und steht **im EEPROM des
Geräts**. Er wird mit ReplicatorG oder einem EEPROM-Editor gesetzt, nicht aus
dem Slicer. Ein Pressure-Advance-Prüfkörper aus Orca kann trotzdem sinnvoll
sein — man druckt ihn mehrfach und ändert zwischen den Drucken den
EEPROM-Wert von Hand.

Für Birdwing, Lava und Sketch gibt es keinen Ersatz. Die Firmware ist
geschlossen und führt keinen entsprechenden Parameter.

---

## 1. Voraussetzung: erst die Profile richtigstellen, dann kalibrieren

**Nicht vorher kalibrieren.** Vier Werte im Z18-Zweig weichen belegt von
MakerBots eigenem Profil ab, und zwei davon verschieben das Ergebnis der
Flusskalibrierung:

| Schlüssel | unser Wert | MakerBot | Herkunft |
|---|---|---|---|
| `layer_height` | 0,20 | **0,204** | `layerHeight` |
| `bottom_shell_layers` | 3 | **4** | `floorThickness` 0,804 ÷ 0,204 |
| `top_shell_layers` | 5 | **4** | `roofThickness` 0,804 ÷ 0,204 |
| `support_threshold_angle` | 30 | **68** | `supportAngle` |

Die Lagenhöhe geht direkt in den Volumenstrom je Millimeter ein. 0,20 gegen
0,204 sind **2 %** — dieselbe Größenordnung wie die Auflösung einer
Flusskalibrierung. Wer zuerst kalibriert und danach die Lagenhöhe korrigiert,
kalibriert ein zweites Mal.

---

## 2. Die vier Ausgabewege und was sie durchlassen

Ein Kalibrierverfahren funktioniert genau dann, wenn der Befehl, auf dem es
beruht, bis zum Drucker durchkommt. Das hängt nicht am Modell, sondern am
Ausgabeweg.

| Befehl | Legacy → `.x3g` | Birdwing / Lava → `.makerbot` | Sketch → `.makerbot` | UltiMaker → `.gcode` / `.ufp` |
|---|---|---|---|---|
| `M104` / `M109` Düsentemperatur | **ja** | **ja** → `set_toolhead_temperature` | durchgereicht | ja |
| `M106` / `M107` Bauteillüfter | **ja** | **ja** → `toggle_fan` / `fan_duty` | durchgereicht | ja |
| `M140` / `M190` Betttemperatur | **ja** | **nein** — nur einmalig aus `meta.json` | durchgereicht | ja |
| `M141` / `M191` Kammer | nein | **nein** — nur einmalig aus `meta.json` | durchgereicht | — |
| `M221` Flussfaktor | **ja** | **nein** | durchgereicht | ja |
| `M204` / `M205` Beschleunigung, Jerk | **nein** | **nein** | durchgereicht | ja |
| `M900` Pressure Advance | **nein** | **nein** | ungeprüft | ungeprüft |
| `M593` Input Shaping | **nein** | **nein** | ungeprüft | nein |
| `G2` / `G3` Kreisbögen | **nein** | **ja** | durchgereicht | ja |

**Belege.** Der Werkzeugwegkonverter wertet in `MakerBotToolpath.cpp` genau
diese Befehle aus: `G0 G1 G2 G3 G28 G90 G91 G92 M82 M83 M104 M106 M107 M109`.
Alles andere fällt aus der Auswertung, ohne Fehlermeldung. Bett- und
Kammertemperatur werden in `MakerBotExport.cpp` als **einzelner Wert** nach
`meta.json` und in den `gaggle`-Block geschrieben — ein Temperaturturm für das
Bett ist damit ausgeschlossen. Die GPX-Liste stammt aus
`GPX_BEFEHLSMATRIX_und_plan_legacy.md`. Für Sketch reicht
`MakerBotExport.cpp` den erzeugten G-Code unverändert als `print.gcode` durch;
was ankommt, entscheidet allein die Firmware des Geräts.

---

## 3. Welche Maschine gehört zu welcher Klasse

### MakerBot (19 Modelle)

| Klasse | Ausgabe | Modelle |
|---|---|---|
| **Legacy** | `.x3g` über GPX | Cupcake CNC · Thing-O-Matic ABP · Thing-O-Matic Acrylic · Replicator Original · Replicator Original Dual · Replicator 2 · Replicator 2X |
| **Birdwing** | `.makerbot` mit `print.jsontoolpath` | Replicator 5th Gen · Replicator Mini · Replicator Mini+ · Replicator+ · Replicator Z18 |
| **Lava** | `.makerbot` mit `print.jsontoolpath` | Method · Method X · Method X Carbon Fiber · Method XL |
| **Sketch** | `.makerbot` mit `print.gcode` | Sketch · Sketch Large · Sketch Sprint |

### UltiMaker (19 Modelle)

| Klasse | Ausgabe | Modelle |
|---|---|---|
| **Lava** | `.makerbot` | Method · Method X · Method X Carbon Fiber · Method XL |
| **S-Serie** | `.ufp` / `.gcode` | S3 · S5 · S7 · S8 |
| **Factor** | `.ufp` / `.gcode` | Factor 4 |
| **Classic** | `.gcode` | Original · Original+ · 2 · 2 Go · 2+ · 2+ Connect · 2 Extended · 2 Extended+ · 3 · 3 Extended |

Prüfen lässt sich die Zuordnung mit:

```bash
cd ~/Orca_Dev/resources/profiles && ls MakerBot/machine UltiMaker/machine
```

---

## 4. Was auf welcher Klasse kalibrierbar ist

| Kalibrierung | Legacy | Birdwing | Lava | Sketch | UltiMaker |
|---|---|---|---|---|---|
| Erste Lage / Z-Versatz | ja | ja | ja | ja | ja |
| **Düsentemperatur (Turm)** | **ja** | **ja** | **ja** | **ja** | **ja** |
| **Flussrate Durchgang 1 + 2** | **ja** | **ja** | **ja** | **ja** | **ja** |
| **Retraktion** | **ja** | **ja** | **ja** | **ja** | **ja** |
| **Max. Volumenstrom** | **ja** | **ja** | **ja** | **ja** | **ja** |
| Toleranz / Maßhaltigkeit | ja | ja | ja | ja | ja |
| Kühlung / Überhang | ja | ja | eingeschränkt¹ | ja | ja |
| Betttemperatur (Turm) | ja | **nein**² | **nein**² | ja | ja |
| Pressure Advance | **nein**³ | **nein** | **nein** | ungeprüft | ungeprüft |
| Input Shaping | **nein** | **nein** | **nein** | **nein** | **nein** |
| Cornering / Jerk | **nein** | **nein** | **nein** | durchgereicht | ja |

¹ Bei ABS und ASA führt MakerBot über die gesamte Method-Reihe `fanSpeed 0,0` —
der Bauteillüfter läuft nicht. Eine Kühlungskalibrierung ist dort
gegenstandslos.
² Betttemperatur steht einmalig in `meta.json`, sie lässt sich innerhalb eines
Drucks nicht ändern. Man kalibriert sie mit mehreren Drucken.
³ Ersatz über `JKN_ADVANCE_K` im EEPROM, siehe § 0.

**Der wichtigste Satz dieser Tabelle:** Die fünf fett gesetzten Verfahren
funktionieren auf **allen** Klassen. Sie sind rein slicerseitig — sie ändern
Geometrie, Flussfaktor oder Temperatur, und alle drei kommen überall durch.

---

## 5. Reihenfolge

Die Reihenfolge ist nicht beliebig. Jeder Schritt setzt voraus, dass die
darüberliegenden stehen.

| | Schritt | Warum an dieser Stelle |
|---|---|---|
| **1** | Erste Lage und Z-Versatz | Maschinensache, kein Filament. Eine schiefe erste Lage verfälscht jede folgende Messung |
| **2** | **Düsentemperatur** | bestimmt Viskosität. Fluss, Retraktion und Volumenstrom hängen alle davon ab |
| **3** | **Flussrate, Durchgang 1** | grobe Annäherung, ±5 % |
| **4** | **Flussrate, Durchgang 2** | Feinabgleich, ±1 % |
| **5** | **Retraktion** | wirkt erst sinnvoll, wenn Temperatur und Fluss stehen — beide verändern das Fadenziehen |
| **6** | **Maximaler Volumenstrom** | begrenzt die Geschwindigkeit. Erfordert die endgültige Temperatur |
| 7 | Kühlung und Überhang | materialabhängig, siehe § 7 |
| 8 | Toleranztest | Abschlusskontrolle, keine eigene Einstellung |

**Ein Schritt je Druck.** Wer Temperatur und Fluss zusammen ändert, weiß bei
einer Verbesserung nicht, welcher der beiden es war.

---

## 6. Die sechs Schritte im Einzelnen

### Schritt 1 — Erste Lage und Z-Versatz

Kein Orca-Verfahren, sondern Maschinenarbeit. Auf Birdwing und Lava über das
Gerätemenü, auf Legacy über den EEPROM-Wert oder die Z-Endschalterschraube.

**Ziel:** eine erste Lage, deren Bahnen sich berühren, ohne dass Material
seitlich herausquillt.

### Schritt 2 — Düsentemperatur

**Verfahren:** Kalibrierung → Temperaturturm.

Orca schreibt `M104` an den Lagenwechseln. Auf Birdwing und Lava wird daraus
`set_toolhead_temperature` im Werkzeugweg — belegt gemessen.

**Bereich:** PLA 190–230 · PETG 220–260 · ABS/ASA 230–270 · Nylon 240–280.
Schrittweite 5 °C.

**Bewertung:** niedrigste Temperatur, bei der die Schichten noch fest
verbunden sind und keine Fäden entstehen.

**Landet in:** `nozzle_temperature`, `nozzle_temperature_initial_layer`
(erste Lage üblicherweise 5 °C höher).

**Fallstrick Z18 und Method:** beheizter Bauraum. Der Turm misst dort die
Kombination aus Düse und Kammer. Die Kammertemperatur muss zwischen den
Versuchen gleich bleiben, sonst ist der Turm nicht vergleichbar.

### Schritt 3 und 4 — Flussrate

**Verfahren:** Kalibrierung → Flussrate → Durchgang 1, danach Durchgang 2.

Rein slicerseitig: Orca schneidet dieselbe Geometrie mehrfach mit
unterschiedlichem `filament_flow_ratio`. Es wird kein einziger Sonderbefehl
erzeugt — deshalb funktioniert das Verfahren auf jeder Klasse.

**Bewertung:** die Fläche, die am gleichmäßigsten aussieht, ohne Rillen
zwischen den Bahnen und ohne aufgeworfene Kanten.

**Landet in:** `filament_flow_ratio`.

**Fallstrick MakerBot:** MakerBot Print rechnet die Materialmenge mit einem
Filamentdurchmesser von **1,77 mm**, unsere Profile mit 1,75. Das sind 2,3 %
Querschnittsunterschied. Wer unseren Fluss gegen eine Angabe aus MakerBot
Print hält, muss diesen Faktor herausrechnen — der Unterschied ist kein
Fehler.

### Schritt 5 — Retraktion

**Verfahren:** Kalibrierung → Retraktionstest.

**Landet in:** In Orca gehört die Retraktion zum **Maschinenprofil**
(`retraction_length`, `retract_restart_extra`). Für eine
**filamentabhängige** Einstellung ist der Weg über das Filamentprofil zu
gehen:

```
filament_retraction_length
filament_retraction_speed
filament_retract_restart_extra
```

Sonst überschreibt der nächste Filamentwechsel die mühsam gefundenen Werte
nicht — er lässt sie einfach stehen, obwohl sie nicht mehr passen.

**Fallstrick Smart Extruder:** Birdwing und Lava führen im Extruder eine
eigene Steuerung. Sehr große Retraktionswerte werden dort nicht sauber
umgesetzt. Ausgangspunkt 0,5 bis 1,5 mm, nicht mehr.

### Schritt 6 — Maximaler Volumenstrom

**Verfahren:** Kalibrierung → Erweitert → Max. Volumenstrom.

**Landet in:** `filament_max_volumetric_speed`.

**Warum er wichtig ist:** Er ist die einzige Bremse, die auf allen Klassen
zuverlässig wirkt. Beschleunigung und Jerk werden verworfen (§ 2) — der
Volumenstrom nicht, weil Orca ihn beim Schneiden in die Vorschübe einrechnet.

**Fallstrick Legacy mit Werksfirmware:** Dort ist bereits die reine
Bahngeschwindigkeit auf ≤ 50 mm/s begrenzt, weil `ACCELERATION_ACTIVE` in
Firmware 5.2 nicht existiert. Der Volumenstrom wird in diesem Fall selten der
begrenzende Wert sein.

---

## 7. Kühlung und Überhang — nur wo es Sinn hat

MakerBots eigene Zuordnung, aus dem echten Z18-PLA-Profil:

| Rolle | Vorschub | Lüfter |
|---|---|---|
| `outlines` (Außenwand) | 40 | **0,95** |
| `insets` | 90 | 0,50 |
| `solid`, `sparse`, `roof`, `floor` | 110 | 0,50 |
| `bridges` | 40 | 0,50 |
| erste Lage | 30 | 1,00 |

In Orca bilden wir das mit `overhang_fan_threshold: 0%`,
`overhang_fan_speed: 95` und `fan_min_speed: 50` ab.

**Nicht kalibrieren bei ABS und ASA auf der Method-Reihe.** Dort führt
MakerBot durchgehend `fanSpeed 0,0` — der Bauteillüfter läuft nicht, und der
beheizte Bauraum erledigt die Aufgabe.

**Silk-Filamente sind ein eigener Fall.** Gemessen am selben Prüfkörper,
derselben Maschine und demselben Profil: mit Silk-PLA brechen Kanten an
Überhängen weg, mit normalem PLA nicht. Silk-Blends brauchen eigene
Filamentprofile mit niedrigerer Überhanggeschwindigkeit und mehr Kühlung —
das ist kein Fehler der Maschine und keiner des Profils.

---

## 8. Protokollvorlage

```
Filament ........................  Hersteller, Typ, Farbe, Charge
Drucker .........................  Modell, Klasse, Firmwareversion
Düse ............................  Durchmesser, Typ (Smart Extruder / 1XA / …)
Ausgangsprofil ..................  Name

1  Erste Lage        Z-Versatz ............  erledigt am ....
2  Düsentemperatur   Turm .... bis ....     gewählt ....  °C
3  Fluss Durchgang 1 gewählt ....          
4  Fluss Durchgang 2 gewählt ....           -> filament_flow_ratio ....
5  Retraktion        Länge .... mm  Tempo .... mm/s
6  Volumenstrom      .... mm³/s              -> filament_max_volumetric_speed
7  Kühlung           fan_min .... / overhang .... 
8  Toleranztest      Abweichung X .... Y .... Z ....
```

---

## 9. Prüfbefehle

```bash
# Welche Befehle wertet der Werkzeugwegkonverter aus?
grep -o 'cmd == "[A-Z0-9]*"' ~/Orca_Dev/src/libslic3r/Format/MakerBotToolpath.cpp | sort -u

# Wird Bett- oder Kammertemperatur je Lage geschrieben?
grep -n "platform_temperature\|chamber_temperature" ~/Orca_Dev/src/libslic3r/Format/MakerBotExport.cpp

# Welche Maschinenklassen liegen im Repo?
ls ~/Orca_Dev/resources/profiles/MakerBot/machine ~/Orca_Dev/resources/profiles/UltiMaker/machine

# Sailfish oder Werksfirmware? (Legacy, vor der Profilwahl)
#   eeprom_2x_lesen.gcode an den Drucker senden; Variante 0x80 = Sailfish
```

---

## 10. Was dieses Dokument bewusst nicht sagt

Die Angaben zu **Sketch** und zu den **UltiMaker-Modellen** in § 2 sind mit
„durchgereicht" beziehungsweise „ungeprüft" gekennzeichnet, weil für diese
Wege kein Konverter im Fork steht, der die Befehle filtert — es entscheidet
die Gerätefirmware, und die ist für diese Modelle noch nicht erfasst. Wer dort
Pressure Advance oder Cornering ausprobiert, sollte den erzeugten G-Code
vorher auf `M900` beziehungsweise `M204` durchsuchen und den Drucker
beobachten.

Für Legacy, Birdwing und Lava sind alle Angaben aus dem Quelltext des Forks
und aus der GPX-Befehlsmatrix belegt.

# Filament calibration on MakerBot and UltiMaker printers

For test build [`v2026.9.0-test2`](https://github.com/DanielZ18-2/Orca-MakerBot-UltiMaker/releases/tag/v2026.9.0-test2)
of this unofficial OrcaSlicer fork. Which printers belong to which class is listed in
[SUPPORTED_PRINTERS.md](SUPPORTED_PRINTERS.md).

A calibration only works if the command it relies on reaches the printer. On these
machines that depends on the **output path**, not on the model. This guide says, per
path, which of OrcaSlicer's calibrations have an effect, in which order to run them, and
where each result belongs.

---

## 1. First the bad news: some menu entries do nothing here

| Calibration | Relies on | What happens in this build |
|---|---|---|
| **Pressure advance** (line, pattern, tower) | `M900` | Only the UltiMaker **S8** and **Factor 4+** have an equivalent (`M214`), and they get it. The UltiMaker **Original / Original+** receive `M900`; whether their firmware acts on it depends on how it was built. On **every other** machine the setting is hidden in the filament tab and the calibration is greyed out, because the firmware has no such command. |
| **Input shaping** (frequency, damping) | `M593` | No effect on any MakerBot machine. Not verified on UltiMaker firmware; assume it has no effect. |
| **Cornering / junction deviation** | `M204`, `M205` | No effect on the legacy line, Birdwing and the Method series. The Method takes acceleration only from the file header, once per file, so it cannot be calibrated per layer. The S8 and Factor 4+ use true jerk (`M215`, in m/s³) instead, set in the process profile, which this test does not calibrate. On the Sketch series and the other UltiMaker machines the commands reach the firmware; how it responds has not been checked on hardware. |

**A substitute for pressure advance exists on the MakerBot legacy line with Sailfish
firmware only.** It is called `JKN_ADVANCE_K` / `JKN_ADVANCE_K2` and lives in the
printer's **EEPROM**. It is set with ReplicatorG or an EEPROM editor, not from the
slicer. You can still print OrcaSlicer's test object several times and change the
EEPROM value by hand between prints.

---

## 2. The output paths and what gets through

| Command | Legacy → `.x3g` | Birdwing → `.makerbot` | Method → `.makerbot` | Sketch → `.makerbot` | UltiMaker S / Factor / 2+ Connect / 3 → `.ufp` | UltiMaker Original → `.gcode` |
|---|---|---|---|---|---|---|
| `M104` / `M109` nozzle temperature | yes | yes | yes | passed through | yes | yes |
| `M106` / `M107` part cooling fan | yes (on/off only) | yes | yes | passed through | yes | yes |
| `M140` / `M190` bed temperature | yes | **once per file** | **once per file** | passed through | yes | yes |
| `M141` / `M191` chamber | no | **once per file**¹ | **once per file**¹ | — | — | — |
| `M221` flow factor | yes | no | no | passed through | yes | yes |
| `M204` / `M205` acceleration, jerk | no | no | acceleration **via the file header**, per feature | passed through | yes (`M215` jerk on S8 / Factor 4+) | yes |
| `M900` pressure advance | no | no | no | no | `M214` on S8 / Factor 4+ only | yes |
| `M593` input shaping | no | no | no | no | no | no |
| `G2` / `G3` arcs | no | **export refused**² | **export refused**² | passed through | yes | yes |

**How to read "once per file":** bed and chamber temperature are written into the
`.makerbot` header as a single value. A bed temperature tower is therefore impossible on
Birdwing and Method; calibrate bed temperature with several prints instead.

¹ Only if the filament profile switches chamber heating on.
² Keep arc fitting off, as the bundled MakerBot profiles do; the export stops with an
error rather than silently dropping the arcs.

**How to read "passed through":** for the Sketch series the generated G-code goes into
the `.makerbot` file unchanged apart from moving the origin to the bed centre, and the
printer's firmware alone decides what it does with it. This has not been checked on
hardware.

**Legacy fan:** Sailfish has no fan speed control. Any value above zero switches the fan
fully on.

> **UltiMaker 2, 2+, 2 Go, 2 Extended and 2 Extended+:** this build writes `.ufp` for them,
> which their firmware cannot read. Calibration on these five machines has to wait for a
> fix. The 2+ Connect and the 3 / 3 Extended are not affected.

---

## 3. What can be calibrated where

| Calibration | Legacy | Birdwing | Method | Sketch⁵ | UltiMaker⁶ |
|---|---|---|---|---|---|
| First layer / Z offset | yes | yes | yes | yes | yes |
| **Nozzle temperature (tower)** | **yes** | **yes** | **yes** | **yes** | **yes** |
| **Flow rate, pass 1 + 2** | **yes** | **yes** | **yes** | **yes** | **yes** |
| **Retraction** | **yes** | **yes** | **yes** | **yes** | **yes** |
| **Max volumetric speed** | **yes** | **yes** | **yes** | **yes** | **yes** |
| Tolerance / dimensional accuracy | yes | yes | yes | yes | yes |
| Cooling / overhang | yes¹ | yes | limited² | yes | yes |
| Bed temperature (tower) | yes | no³ | no³ | untested | yes |
| Pressure advance | no⁴ | no | no | no | S8, Factor 4+ (`M214`); Original / Original+ `M900`, untested |
| Input shaping | no | no | no | no | no |
| Cornering / jerk | no | no | no | untested | untested |

¹ On/off only, see above. Several legacy machines shipped without a part cooling fan.
² For ABS and ASA, MakerBot runs the part cooling fan at zero across the whole Method
series; the heated build chamber does the job. A cooling calibration is pointless there.
³ Written once per file, see section 2.
⁴ Substitute through `JKN_ADVANCE_K` in the EEPROM, see section 1.
⁵ The Sketch series has not been printed on with this build; "yes" means the commands
these calibrations need are written, not that the result was checked.
⁶ Except the UltiMaker 2, 2+, 2 Go, 2 Extended and 2 Extended+, see section 2.

**The most important point of this table:** the five calibrations in bold work on
**every** class. They only change geometry, the extrusion amounts computed while
slicing, or the nozzle temperature, and all of these reach every class.

---

## 4. Order

The order matters. Each step assumes the ones above it are done.

| | Step | Why here |
|---|---|---|
| **1** | First layer and Z offset | A machine matter, not a filament one. A bad first layer distorts every later measurement. |
| **2** | **Nozzle temperature** | Sets the viscosity. Flow, retraction and volumetric speed all depend on it. |
| **3** | **Flow rate, pass 1** | Coarse adjustment. |
| **4** | **Flow rate, pass 2** | Fine adjustment. |
| **5** | **Retraction** | Only meaningful once temperature and flow are set; both change stringing. |
| **6** | **Max volumetric speed** | Limits speed. Needs the final temperature. |
| 7 | Cooling and overhangs | Material-dependent, see section 6. |
| 8 | Tolerance test | Final check, no setting of its own. |

**One change per print.** If you change temperature and flow together and the print gets
better, you do not know which of the two it was.

---

## 5. The steps in detail

### Step 1 — First layer and Z offset

Not an OrcaSlicer calibration but machine work: through the printer menu on Birdwing and
Method, through the EEPROM value or the Z endstop screw on the legacy line, through the
printer's own levelling procedure on UltiMaker machines.

**Goal:** a first layer whose lines touch without material squeezing out at the sides.

### Step 2 — Nozzle temperature

**Menu:** Calibration → Temperature tower.

OrcaSlicer writes the temperature change at each band. On Birdwing and Method it becomes
a temperature command in the toolpath; this was measured in sliced files.

**Starting ranges:** PLA 190–230 °C · PETG 220–260 °C · ABS/ASA 230–270 °C ·
Nylon 240–280 °C, in steps of 5 °C.

**Judge:** the lowest temperature at which layers still bond firmly and no strings form.

**Result goes into:** the filament profile, `nozzle_temperature` and
`nozzle_temperature_initial_layer` (the first layer usually 5 °C higher).

**Watch out on the Z18 and the Method series:** the build chamber is heated. The tower
measures nozzle and chamber together, so keep the chamber temperature the same between
attempts or the towers are not comparable.

### Steps 3 and 4 — Flow rate

**Menu:** Calibration → Flow rate → Pass 1, then Pass 2.

This is purely slicer-side: OrcaSlicer slices the same geometry several times with a
different `filament_flow_ratio`. No special command is written, which is why it works on
every class.

**Judge:** the patch that looks most even, without gaps between lines and without raised
edges.

**Result goes into:** `filament_flow_ratio` in the filament profile.

**UltiMaker 2, 3 and S series run 2.85 mm filament.** OrcaSlicer's own library filaments
are all 1.75 mm and remain selectable; on a 2.85 mm machine they extrude about 2.65 times
the intended volume. Pick one of this fork's 2.85 mm filament profiles before you start.

**Comparing with MakerBot Print:** MakerBot Print calculates material with a filament
diameter of 1.77 mm, this fork's profiles with 1.75 mm, about 2.3 % in cross-section (this
does not apply to the Sketch series, which MakerBot Print slices differently). MakerBot
also slices its standard Z18 layer at 0.204 mm where this fork uses 0.20 mm. Both
differences are expected; take them out before comparing flow values.

### Step 5 — Retraction

**Menu:** Calibration → Retraction test.

**Result goes into:** retraction is part of the **printer profile** in OrcaSlicer. For a
value that belongs to a filament, use the overrides in the **filament profile**:

```
filament_retraction_length
filament_retraction_speed
filament_retract_restart_extra
```

Otherwise the next filament change keeps the values you found for the previous material.

**Method series:** its filament profiles already carry a retraction length per material,
following Cura. A result entered only in the printer profile is overridden by it; enter
it in the filament profile.

**Smart Extruders (Birdwing, Method):** they have their own control, and very long
retractions are not carried out cleanly. Start small and increase in small steps.

### Step 6 — Max volumetric speed

**Menu:** Calibration → More → Max volumetric speed.

**Result goes into:** `filament_max_volumetric_speed`.

**Why it matters here:** it is the one speed limit that works on every class, because
OrcaSlicer builds it into the feed rates while slicing. Acceleration and jerk are dropped
on the legacy and Birdwing paths and reach the Method only as fixed per-file values
(section 2); the volumetric limit is not affected.

**Legacy with stock firmware:** here the plain print speed is already limited, because
stock firmware has no acceleration control. The volumetric speed will rarely be the value
that limits.

---

## 6. Cooling and overhangs — only where it makes sense

MakerBot's own Z18 PLA profile runs the outer wall with much more fan than the rest of
the part and the first layer at full fan. For PLA and Silk PLA on the Replicator Z18 this
fork maps that with a lower minimum fan speed and a higher overhang fan speed that engages
on every overhang. On the legacy line the fan is off for ABS.

**Do not calibrate cooling for ABS and ASA on the Method series.** MakerBot runs the
part cooling fan at zero there; the heated chamber does the job.

**Silk filaments are a case of their own.** On the same test object, machine and profile,
silk PLA lost edges on overhangs where standard PLA did not. Silk blends need their own
filament profiles with slower overhangs and more cooling. That is neither a machine fault
nor a profile fault.

---

## 7. Log template

```
Filament ...............  maker, type, colour, batch
Printer ................  model, class, firmware version
Nozzle .................  diameter, type (Smart Extruder / 1XA / print core ...)
Starting profile .......  name

1  First layer        Z offset ..............  done on ....
2  Nozzle temperature tower .... to ....      chosen .... °C
3  Flow, pass 1       chosen ....
4  Flow, pass 2       chosen ....              -> filament_flow_ratio ....
5  Retraction         length .... mm  speed .... mm/s
6  Volumetric speed   .... mm³/s               -> filament_max_volumetric_speed
7  Cooling            fan min .... / overhang ....
8  Tolerance test     deviation X .... Y .... Z ....
```

Please attach this to an issue when you report calibrated values. Values measured on real
machines are what the bundled filament profiles are missing most.

---

## 8. What this guide does not claim

For the legacy line, Birdwing and the Method series, every statement about which commands
get through comes from the fork's source code and from GPX's command list.

For the **Sketch series** and the **UltiMaker** machines, the fork writes only the commands
the dialect knows (`M214` and `M215` on the S8 and Factor 4+, no `M900` on the other
UltiMaker machines except the Original / Original+, none on the Sketch). Whether the
firmware acts on them has not been checked on hardware for these models. Before relying
on pressure advance or cornering there, search the generated file for `M900`, `M214`,
`M204`, `M205` or `M215` and watch the printer.

Only the Replicator Z18 and the Replicator 2X have been printed on with this build.

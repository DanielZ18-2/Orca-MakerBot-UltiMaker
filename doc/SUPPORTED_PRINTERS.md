# Supported printers

Printers with machine, filament and process profiles in this fork, as of test build
[`v2026.9.0-test2`](https://github.com/DanielZ18-2/Orca-MakerBot-UltiMaker/releases/tag/v2026.9.0-test2).
**39 models: 19 MakerBot, 20 UltiMaker.**

**Printed on real hardware with this build: Replicator Z18 and Replicator 2X** (marked ✔).
Every other model is built from vendor firmware, Cura and MakerBot Print and has not been
confirmed on a real machine yet. If you own one, a test report is the most useful thing you
can send: open an issue with the printer, the profile, and the sliced file.

Build volume is the printable area in this fork (width × depth × height, mm). Nozzle sizes
are the variants that come with their own profiles.

## MakerBot — 19 models

### Legacy (Sailfish / stock firmware) — `.x3g`

| Model | Extruders | Build volume (mm) | Nozzles (mm) | |
|---|---|---|---|---|
| Cupcake CNC | 1 | 100 × 100 × 100 | 0.4, 0.6 |  |
| Thing-O-Matic ABP | 1 | 106 × 120 × 106 | 0.4, 0.6 |  |
| Thing-O-Matic Acrylic | 1 | 106 × 120 × 106 | 0.4, 0.6 |  |
| Replicator Original | 1 | 225 × 145 × 150 | 0.2, 0.4, 0.6, 0.8, 1.0 |  |
| Replicator Original Dual | 2 | 225 × 145 × 150 | 0.2, 0.4, 0.6, 0.8, 1.0 |  |
| Replicator 2 | 1 | 285 × 153 × 155 | 0.2, 0.4, 0.6, 0.8, 1.0 |  |
| Replicator 2X | 2 | 246 × 152 × 155 | 0.2, 0.4, 0.6, 0.8, 1.0 | ✔ |

### Birdwing — `.makerbot`

| Model | Extruders | Build volume (mm) | Nozzles (mm) | |
|---|---|---|---|---|
| Replicator 5th Gen | 1 | 252 × 199 × 148 | 0.2, 0.4, 0.6, 0.8, 1.0 |  |
| Replicator Mini | 1 | 100 × 100 × 125 | 0.2, 0.4, 0.6, 0.8, 1.0 |  |
| Replicator Mini+ | 1 | 101 × 126 × 126 | 0.2, 0.4, 0.6, 0.8, 1.0 |  |
| Replicator+ | 1 | 300 × 200 × 165 | 0.2, 0.4, 0.6, 0.8, 1.0 |  |
| Replicator Z18 | 1 | 300 × 305 × 457 | 0.2, 0.4, 0.6, 0.8, 1.0 | ✔ |

### Method — `.makerbot`

| Model | Extruders | Build volume (mm) | Nozzles (mm) | |
|---|---|---|---|---|
| Method | 2 | 152 × 190 × 196 | 0.4 |  |
| Method X | 2 | 152 × 190 × 196 | 0.4 |  |
| Method X Carbon Fiber | 2 | 152 × 190 × 196 | 0.4 |  |
| Method XL | 2 | 305 × 305 × 320 | 0.4 |  |

### Sketch — `.makerbot`

| Model | Extruders | Build volume (mm) | Nozzles (mm) | |
|---|---|---|---|---|
| Sketch | 1 | 150 × 150 × 150 | 0.4 |  |
| Sketch Large | 1 | 220 × 200 × 250 | 0.4 |  |
| Sketch Sprint | 1 | 221.5 × 221.5 × 220.4 | 0.4 |  |

## UltiMaker — 20 models

### Classic

> **UltiMaker 2, 2+, 2 Go, 2 Extended and 2 Extended+: do not print with this build.**
> It writes a `.ufp` file for them, which their firmware cannot read. The 2+ Connect and
> the 3 / 3 Extended are not affected.

| Model | Extruders | Build volume (mm) | Nozzles (mm) |
|---|---|---|---|
| Original | 1 | 210 × 210 × 205 | 0.25, 0.4, 0.6, 0.8 |
| Original+ | 1 | 210 × 210 × 205 | 0.25, 0.4, 0.6, 0.8 |
| 2 | 1 | 223 × 223 × 205 | 0.25, 0.4, 0.6, 0.8 |
| 2 Go | 1 | 120 × 120 × 115 | 0.25, 0.4, 0.6, 0.8 |
| 2 Extended | 1 | 223 × 223 × 305 | 0.25, 0.4, 0.6, 0.8 |
| 2+ | 1 | 223 × 223 × 205 | 0.25, 0.4, 0.6, 0.8 |
| 2 Extended+ | 1 | 223 × 223 × 305 | 0.25, 0.4, 0.6, 0.8 |
| 2+ Connect | 1 | 223 × 220 × 205 | 0.25, 0.4, 0.6, 0.8 |
| 3 | 2 | 215 × 215 × 200 | 0.25, 0.4, 0.8 |
| 3 Extended | 2 | 215 × 215 × 300 | 0.25, 0.4, 0.8 |

### S series

| Model | Extruders | Build volume (mm) | Nozzles (mm) |
|---|---|---|---|
| S3 | 2 | 230 × 190 × 200 | 0.25, 0.4, 0.6, 0.8 |
| S5 | 2 | 330 × 240 × 300 | 0.25, 0.4, 0.6, 0.8 |
| S7 | 2 | 330 × 240 × 300 | 0.25, 0.4, 0.6, 0.8 |
| S8 | 2 | 330 × 240 × 300 | 0.25, 0.4, 0.6, 0.8 |

### Factor

| Model | Extruders | Build volume (mm) | Nozzles (mm) |
|---|---|---|---|
| Factor 4 | 2 | 330 × 240 × 300 | 0.25, 0.4, 0.6, 0.8 |
| Factor 4+ | 2 | 330 × 240 × 300 | 0.4, 0.6 |

### Method

| Model | Extruders | Build volume (mm) | Nozzles (mm) |
|---|---|---|---|
| Method | 2 | 152 × 190 × 196 | 0.4 |
| Method X | 2 | 152 × 190 × 196 | 0.4 |
| Method X Carbon Fiber | 2 | 152 × 190 × 196 | 0.4 |
| Method XL | 2 | 305 × 305 × 320 | 0.4 |

The Method series is listed under both vendors, as it is in the vendors' own software.
Both entries use the same Method firmware dialect and machine identifiers.

How to calibrate filaments on these printers, and which calibrations have an effect on
which of them: [Calibration_MakerBot_UltiMaker.md](Calibration_MakerBot_UltiMaker.md).

See the [release notes](https://github.com/DanielZ18-2/Orca-MakerBot-UltiMaker/releases/tag/v2026.9.0-test2)
for what changed since test 1 and the known limitations.

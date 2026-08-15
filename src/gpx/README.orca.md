# GPX, embedded in Orca

Upstream: <https://github.com/markwal/GPX>, release **2.6.8**.

GPX converts RepRap/Marlin G-code into the `.x3g` (s3g) stream the MakerBot
legacy line speaks: Cupcake, Thing-O-Matic, Replicator 1 / 2 / 2X and the
clones GPX knows (ZYYX, FlashForge Creator Pro, Core-XY).

Orca previously shelled out to a `gpx` binary on `PATH`. That meant every user
had to install GPX separately, a failure surfaced only as an exit code, and on
Windows and macOS packages it did not work at all. This directory embeds GPX as
a static library instead.

## What is in here

| File | Origin |
|---|---|
| `gpx.c`, `gpx.h`, `vector.c`, `vector.h`, `winsio.h` | upstream `src/gpx/` |
| `shared/` | upstream `src/shared/` |
| `LICENSE` | upstream |
| `gpx_config.h` | **new**, replaces the autotools-generated `config.h` |
| `GPXConvert.hpp`, `GPXConvert.cpp` | **new**, the C++ facade Orca calls |
| `CMakeLists.txt` | **new** |

Not embedded:

* `gpx-main.c` — the CLI's `main()`, not needed by a library.
* `gpxresp.c`, `winsio.c` — serial/USB printing. Orca never drives an S3G
  serial port itself, so `SERIAL_SUPPORT` stays undefined and these do not
  compile without it.
* `shared/s3g*.c` — only used by the upstream `s3gdump` utility.
* `pymodule/`, `utils/`, `tests/`, autotools scaffolding.

## The only change made to upstream sources

Two `#include "config.h"` lines were redirected to `#include "gpx_config.h"`:

* `gpx.h:39`
* `shared/machine_config.c:34`

`config.h` is an autotools artefact and far too generic a name to put on an
include path inside Orca. `gpx_config.h` supplies the four macros the sources
actually reference (`PACKAGE_STRING`, `PACKAGE_VERSION`, `VERSION`,
`HAVE_NANOSLEEP`).

Apart from those two lines the vendored sources are byte-identical to upstream
2.6.8, which keeps a future version bump a straight file copy.

## Verification

The library path was checked against the upstream CLI built from the same
sources, `gpx -I -v -m r2x <in> <out>`, on the upstream regression fixtures
`src/gpx/tests/lint.gcode` and `issue13.gcode`. Output is **byte-identical**
in both cases when the build name matches. The build name is the only thing
that can differ, and it is only the string shown on the printer's LCD.

## G-code flavour

`gpx->flag.reprapFlavor` is left at its default of `1`. The CLI's `-g` switch,
which turns it off, is deliberately **not** reproduced — it would route `M106`
to the extruder heatsink fan instead of the part cooling fan, make `Tn`
non-sticky, and mis-read `M109`'s tool parameter. The reasoning is documented
at length in `src/libslic3r/Format/GPXExport.cpp`.

## Default `.ini` lookup

The CLI searches for `gpx.ini` / `~/.gpx.ini` unless given `-I`. The library
does **not**: a stray ini in a user's home directory must not silently change
what Orca sends to a printer. An explicit ini is still honoured via the
`ORCA_GPX_INI` environment variable.

## Licence

GPX is GPL-2.0-**or-later** (`gpx.c`, `vector.c`; see the file headers), and
the `shared/` helpers by Dan Newman are 3-clause BSD. "or later" makes the GPL
part usable under GPL-3.0, which is compatible with Orca's AGPL-3.0. `LICENSE`
in this directory is the upstream copy.

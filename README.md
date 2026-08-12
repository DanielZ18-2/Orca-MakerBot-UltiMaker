# Unofficial OrcaSlicer for MakerBot & UltiMaker

> ## ⚠️ Independent, unofficial fork — not affiliated with OrcaSlicer
> This is an **independent, community-made fork** of [OrcaSlicer](https://github.com/OrcaSlicer/OrcaSlicer). It is **not produced by, affiliated with, endorsed by, or supported by** the OrcaSlicer project, MakerBot, or UltiMaker, and it is **not** an official build of any of them. It is an **experimental build shared for community testing.** **Use at your own risk.**

## Why this fork exists

Stock OrcaSlicer cannot drive MakerBot and UltiMaker printers, because these machines need container/binary output formats and vendor-specific network protocols instead of plain G-code. This fork adds that natively — so you can slice, send, and monitor these printers from one modern slicer.

The goal is to contribute this work back to OrcaSlicer through pull requests once the community has tested it. **Until that happens, this fork is the only place this support lives, and it remains unofficial.**

## Install

Download the build for your operating system from the **[Releases](../../releases)** page:

- **Windows:** the `…_x64.exe` (or `…_arm64.exe`) installer, or the portable `.zip`
- **macOS:** the universal `.dmg`
- **Linux:** the `.AppImage` (`chmod +x` then run), or the Flatpak bundle

> **The binaries are not code-signed.** Windows SmartScreen and macOS Gatekeeper will warn you the first time you run it:
> - **Windows:** click *More info* → *Run anyway*.
> - **macOS:** right-click the app → *Open* → confirm.
>
> This is normal for an independent community build. If that is a concern for you, build from source instead.

## What this fork adds

- **Output formats:** `.makerbot` (Birdwing), `.x3g` (legacy / Sailfish), `.ufp` (UltiMaker)
- **Network printing + a Device tab:** live telemetry, webcam, print controls (pause / resume / cancel), live Z-offset adjustment, and a start-print flow with an on-camera build-plate confirmation
- **Vendor profiles** for the MakerBot & UltiMaker range (machine / process / filament)
- **Z-offset calibration** with printed value flags on the test model
- **User interface translated into 21 languages**

## Supported printers

- **MakerBot Birdwing:** Replicator Z18, Replicator+, Replicator Mini, Mini+, 5th Gen, Method / Lava
- **Legacy MakerBot / Sailfish** (via `.x3g`)
- **UltiMaker** (via `.ufp` / REST)

## Verification status — please read

Being honest about what is proven and what is not:

- **Hardware-verified:** Replicator **Z18** (Birdwing) and Replicator **2X** (legacy) — printed and confirmed on real machines.
- **Firmware-derived, NOT yet hardware-tested:** all other models. Their settings (build volume, park position, end-of-print behaviour) are read directly from the manufacturer firmware, but they still need real-world confirmation. **If you own one of these printers, your test reports are exactly what this fork needs.**

## Translations

The interface is translated into 21 languages. **German** is native quality and **English** is the source language. The other **19 languages are machine-assisted** and marked for community review — corrections via issue or pull request are very welcome. The ones that most need a native review are **Thai, Vietnamese, Lithuanian, and Hungarian**.

## Status & roadmap

This is a community-testing build. Once the printer range has been tested in the real world, the work will be offered to **OrcaSlicer upstream** as a series of small, reviewable pull requests. **Downloading and sending test feedback directly supports getting this merged upstream.**

## Safety

3D printers get hot and have moving parts, and this is experimental software talking to your printer over the network or USB:

- Keep the build plate completely clear before starting a print — a leftover object can damage the print head.
- Watch the first prints and don't leave prints unattended.
- A wrong **Z-offset can damage the build plate or nozzle** — read the on-screen warnings before adjusting.

## Reporting issues & contributing

Please open a GitHub issue with your **printer model**, **operating system**, and **what happened** (logs and screenshots help a lot). Especially valuable:

- Test reports for the non-Z18 printers listed above.
- Native-speaker corrections for the machine-assisted translations.

## License & credits

This is a **fork of OrcaSlicer** and inherits its license (**AGPL-3.0**). It gratefully builds on the work of the **OrcaSlicer** project and its upstream lineage (**Bambu Studio**, **PrusaSlicer**, **Slic3r**).

"OrcaSlicer", "MakerBot", "Replicator", "Method", "Sailfish", and "UltiMaker" are trademarks of their respective owners. This is an independent project; the names are used only to describe compatibility, and no association or endorsement is claimed or implied.

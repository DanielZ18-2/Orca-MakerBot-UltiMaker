# Inoffizieller OrcaSlicer für MakerBot & UltiMaker

> ## ⚠️ Unabhängiger, inoffizieller Fork — nicht mit OrcaSlicer verbunden
> Dies ist ein **unabhängiger, von der Community erstellter Fork** von [OrcaSlicer](https://github.com/OrcaSlicer/OrcaSlicer). Er wird **nicht** vom OrcaSlicer-Projekt, MakerBot oder UltiMaker erstellt, unterstützt, empfohlen oder betreut und ist **kein** offizielles Build dieser Anbieter. Es handelt sich um ein **experimentelles Build für Community-Tests.** **Nutzung auf eigene Gefahr.**

## Warum dieser Fork existiert

Der reguläre OrcaSlicer kann MakerBot- und UltiMaker-Drucker nicht ansteuern, weil diese Geräte Container-/Binärformate und herstellereigene Netzwerkprotokolle statt reinem G-Code benötigen. Dieser Fork ergänzt das nativ — so lassen sich diese Drucker aus einem modernen Slicer slicen, senden und überwachen.

Ziel ist, diese Arbeit über Pull Requests an OrcaSlicer zurückzugeben, sobald die Community sie getestet hat. **Bis dahin existiert diese Unterstützung nur in diesem Fork und bleibt inoffiziell.**

## Installation

Lade das Build für dein Betriebssystem von der **[Releases](../../releases)**-Seite:

- **Windows:** das `…_x64.exe`- (oder `…_arm64.exe`-)Installationsprogramm oder das portable `.zip`
- **macOS:** das universelle `.dmg`
- **Linux:** das `.AppImage` (`chmod +x`, dann ausführen) oder das Flatpak-Bundle

> **Die Binaries sind nicht signiert.** Windows SmartScreen und macOS Gatekeeper warnen beim ersten Start:
> - **Windows:** *Weitere Informationen* → *Trotzdem ausführen*.
> - **macOS:** Rechtsklick auf die App → *Öffnen* → bestätigen.
>
> Das ist bei einem unabhängigen Community-Build normal. Wenn dich das stört, baue stattdessen aus dem Quelltext.

## Was dieser Fork ergänzt

- **Ausgabeformate:** `.makerbot` (Birdwing), `.x3g` (Legacy / Sailfish), `.ufp` (UltiMaker)
- **Netzwerkdruck + ein Geräte-Tab:** Live-Telemetrie, Webcam, Drucksteuerung (Pause / Fortsetzen / Abbrechen), Live-Z-Offset-Anpassung und ein Druckstart mit Kamera-Bestätigung der freien Druckplatte
- **Vendor-Profile** für die MakerBot- & UltiMaker-Palette (Drucker / Prozess / Filament)
- **Z-Offset-Kalibrierung** mit aufgedruckten Wert-Fähnchen am Testmodell
- **Oberfläche in 21 Sprachen übersetzt**

## Unterstützte Drucker

- **MakerBot Birdwing:** Replicator Z18, Replicator+, Replicator Mini, Mini+, 5th Gen, Method / Lava
- **Legacy MakerBot / Sailfish** (über `.x3g`)
- **UltiMaker** (über `.ufp` / REST)

## Verifikationsstatus — bitte lesen

Ehrlich, was belegt ist und was nicht:

- **Am Gerät verifiziert:** Replicator **Z18** (Birdwing) und Replicator **2X** (Legacy) — auf echten Maschinen gedruckt und bestätigt.
- **Aus der Firmware abgeleitet, NOCH NICHT am Gerät getestet:** alle anderen Modelle. Ihre Einstellungen (Bauraum, Parkposition, Druckende-Verhalten) sind direkt aus der Hersteller-Firmware ausgelesen, brauchen aber noch reale Bestätigung. **Wenn du einen dieser Drucker besitzt, sind deine Testberichte genau das, was dieser Fork braucht.**

## Übersetzungen

Die Oberfläche ist in 21 Sprachen übersetzt. **Deutsch** ist muttersprachliche Qualität, **Englisch** ist die Quellsprache. Die übrigen **19 Sprachen sind maschinengestützt** und zur Community-Prüfung markiert — Korrekturen per Issue oder Pull Request sind sehr willkommen. Am dringendsten brauchen eine muttersprachliche Prüfung: **Thai, Vietnamesisch, Litauisch und Ungarisch**.

## Status & Fahrplan

Dies ist ein Build für Community-Tests. Sobald die Druckerpalette real getestet ist, wird die Arbeit OrcaSlicer als Serie kleiner, prüfbarer Pull Requests angeboten. **Herunterladen und Testrückmeldungen geben unterstützen direkt die Aufnahme in den offiziellen OrcaSlicer.**

## Sicherheit

3D-Drucker werden heiß und haben bewegliche Teile, und dies ist experimentelle Software, die über Netzwerk oder USB mit deinem Drucker spricht:

- Halte die Druckplatte vor dem Start vollständig frei — ein zurückgelassenes Objekt kann den Druckkopf beschädigen.
- Beobachte die ersten Drucke und lass Drucke nicht unbeaufsichtigt.
- Ein falscher **Z-Offset kann Druckplatte oder Düse beschädigen** — lies die Warnungen auf dem Bildschirm, bevor du ihn änderst.

## Fehler melden & mitwirken

Bitte öffne ein GitHub-Issue mit deinem **Druckermodell**, **Betriebssystem** und **was passiert ist** (Logs und Screenshots helfen sehr). Besonders wertvoll:

- Testberichte für die oben genannten Nicht-Z18-Drucker.
- Muttersprachliche Korrekturen für die maschinengestützten Übersetzungen.

## Lizenz & Danksagung

Dies ist ein **Fork von OrcaSlicer** und übernimmt dessen Lizenz (**AGPL-3.0**). Er baut dankbar auf der Arbeit des **OrcaSlicer**-Projekts und seiner Vorgänger (**Bambu Studio**, **PrusaSlicer**, **Slic3r**) auf.

„OrcaSlicer", „MakerBot", „Replicator", „Method", „Sailfish" und „UltiMaker" sind Marken ihrer jeweiligen Inhaber. Dies ist ein unabhängiges Projekt; die Namen werden nur zur Beschreibung der Kompatibilität verwendet, eine Verbindung oder Empfehlung wird weder behauptet noch impliziert.

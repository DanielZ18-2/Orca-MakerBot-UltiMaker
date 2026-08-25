# Test Report - Method X

I am pretty new to this machine - one was donated to our makerspace, and I am trying to make it work.

## Hardware Configuration

- Makerbot Method X
- Extruders: 1XA, 2XA
- Filament loaded: Makerbot ABS-R, Makerbot RapidRinse

![Initial Filament Configuration](filament.jpg)

## Software Configuration

- Windows OrcaSlicer portable release
  - default OrcaSlicer WAS installed on this machine as well...
- Add Printer -> Method X
- Add all Generic Filaments for that printer
 
### Slice 1 - defaults to ABS/ABS:

- Material not recognized. Seems to want BOTH configured filaments in extruder 1.

![Default Slice](initial_abs_slice.jpg)

### Slice 2 - Custom single-filament 

- Modified printer profile to have 1 extruder, and set to ABS. Material not compatible, as ABS-R is loaded.

![Single extruder ABS](custom_single_filament_1.jpg)

### Slice 3 - Custom single-filament ABS-R

- Created a custom filament profile with ABS-R as the type. Same result on the screen - incompatible material.
- It seems like there is a dangling '+' but I don't quite know what to expect from the print properties.

![Single extruder ABS-R](custom_single_filament_2.jpg)

### Slice 4 - Back to default printer profile, with custom filament profile:

- Again seems to want both materials on extruder 1.

![Custom filaments default printer](try_custom_absr_filaments.jpg)

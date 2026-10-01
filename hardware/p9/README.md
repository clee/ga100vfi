# P9 assembly reference

P9 uses the P8 PCB layout and silkscreen with a revised customer hand-fit
assembly plan. `jlc-bom.csv` lists factory-fitted parts, `jlc-cpl.csv` gives
placement coordinates, and `rotation-corrections.csv` records vendor-specific
orientation changes. D2/D3 use PMEG2010AEJ,115 in the assembly BOM rather
than the PMEG2010EA,115 shown in the board schematic.

Generate Gerber and stencil outputs from the KiCad board and compare them with
the selected BOM and CPL before ordering.

The customer supplies and hand-fits the STM32G431CBT6 MCU and nine hooks.
Confirm the board's bootloader and option configuration. Confirm D1's absolute rotation
with the assembler using `JLC_D1_ASSEMBLY_NOTE.txt` and
`D1-assembly-orientation.png`.

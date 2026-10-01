# PWMVID board sources

Open [`p8/design/pwmvid_concept.kicad_pro`](p8/design/pwmvid_concept.kicad_pro)
in KiCad 9 for the schematic and PCB. The design directory includes its custom
symbols and footprints. [`p8/docs/testpad-key.csv`](p8/docs/testpad-key.csv)
maps the exposed probe points to nets and board coordinates.

[`p9/`](p9/README.md) contains the customer hand-fit BOM, placement file,
and rotation corrections. P9 uses P8 copper and retains the P8 silkscreen.
The schematic specifies STM32G431CBT6 as U1 for the 128 KiB firmware. Check
the fitted part and boot configuration on an assembled board.

The [P8 expansion note](p8/P8_EXPANSION_AND_TESTPOINTS.md) describes J9, test
pads, and electrical limits. Firmware qualification steps are in the
[bench checklist](../firmware/docs/BENCH_ACCEPTANCE.md).

# P8 expansion and local probe access

P8 has an unpopulated 2×4, 2.54 mm expansion header and local bare probe
pads. The MCU and nine Keystone hooks are hand-fitted parts.

## J9 pin map

Viewed from the component side, the square pad is pin1. Odd pins form the inner column; even pins face the right board edge. From top to bottom:

| Inner pin | GPIO | MCU physical pin | Outer pin | Rail / return |
|---|---|---:|---|---|
| 1 | PB0 | 16 | 2 | USB VBUS |
| 3 | PA5 | 13 | 4 | 3.3 V |
| 5 | PB10 | 22 | 6 | 1.8 V |
| 7 | PB11 | 25 | 8 | GND |

These four GPIOs avoid USB, HSE, SWD/debug, BOOT0, capture, PWM and protection
assignments. Their reset state is analog mode with no pull, so firmware must
explicitly configure any extension function.

GPIO usage is restricted to powered-board 0–3.3 V signals. This is not a 5 V or isolated interface. The presence of a 1.8 V supply pin does not make these GPIOs compatible with a 1.8 V logic high; the conservative guaranteed high threshold is 0.7×VDD. Do not drive GPIOs or rails when the adapter is unpowered. Do not use the header as an external power input or connect it to another actively driven supply. There is no additional isolation, current limiter or short-circuit protection at J9.

The three rails are unswitched outputs from the existing shared supplies. USB VBUS is the connector's raw bus voltage, not a separately regulated 5 V output. A provisional **10 mA combined external allocation** includes all header rail loads and GPIO-sourced loads together, not 10 mA per pin or per rail. The existing approximately 70 mA board allocation includes typical increments and engineering reserves; it is not a universal measured maximum. Verify the complete device remains within its USB current allowance, including pre-enumeration, suspend and startup/inrush. Disable or remove extension loads as required for those states. Supply availability during disarm is not a permission signal, and no guaranteed current entitlement is implied. Include any attached extension and its capacitance in startup, USB-detach, brownout and fault/Hi-Z qualification; the board-only model does not establish shutdown timing with arbitrary attachments.

## Local probe pads

The added pads are 1.0 mm exposed copper with no paste, no purchased part and short local connections. Their reference/net/coordinate map is provided in docs/testpad-key.csv. They expose intermediate control, reset, comparator, watchdog and enable-chain nodes. Nearby GND pads provide short probe returns. USB data, HSE, timing RC and precision threshold networks do not receive additional pads; the existing fast PWM/GND pairs and REF/THR pads remain.

Use high-impedance, low-capacitance probes and short ground connections. Added probe loading can change edges and protection timing; validate the measurement setup, particularly at /OE. These points are for observation, not external control injection. A static color or probe label never substitutes for checking the actual net and state.

## Validation and use

Keep JP1 open during initial checks. Native routing, ERC/DRC, pin/net parity,
ground return and manufacturing-export checks passed. The design has 148
schematic records, 154 footprints, 1,575 track segments and 293 tented vias,
with no unrouted connections. Supplier process acceptance and staged electrical
qualification are still required. The approximately 71–113 mm J9 GPIO paths
are general-purpose debug access, without high-speed signal-integrity
qualification. PB2 remains unused and unconnected.

PA5 has selectable analog alternate functions, including COMP2_INM. Preserve the protection configuration: COMP2 negative input remains DAC3_CH2 (INMSEL=100), not PA5 (INMSEL=110). Keep PA5 DAC/OPAMP alternate functions disabled when using it as an extension GPIO. This is a firmware configuration requirement, not an automatic reset connection.

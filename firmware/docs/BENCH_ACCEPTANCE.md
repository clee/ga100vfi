# PWMVID P8 first-board bench acceptance

Prepared from the P8 schematic and test-pad key, not from measurements of every assembled board. Oscilloscope and multimeter are useful for the checks below. They do not replace a controlled 1.8-V signal source and a current-limited adjustable low-voltage source for the full active test.

**Keep the GPU entirely disconnected. Start with JP1 open.** Do not bridge, drive or override the protection test pads to force arming. Use the telemetry firmware first; any active fixture image is still a qualification candidate.

## Scope and power setup

- Verify your actual board revision and markings against this P8 map before connecting anything.
- Connect ordinary earth-referenced scope ground clips ONLY to board GND. All channels' grounds are commonly connected; never clip one to a signal, rail, comparator threshold or transistor terminal. A USB-connected Mac, ST-Link, supply and scope can create shared ground paths. If grounds cannot safely be common, use an appropriately rated differential/isolated measurement arrangement; do not defeat scope protective earth.
- Use high-impedance 10x probes, compensated first, with short ground springs for PWM/ARM observations. Avoid 50-ohm scope termination on the logic/sense nodes.
- Power down before attaching wiring, setting JP1, or moving a grabber near fine-pitch parts. Prove GND continuity with power off. Do not use continuity/resistance mode on a powered board.
- Use one intentional board power path, with current limiting where available. USB powers the board; do not simultaneously feed a probe's 3.3-V power output into the board. J6 pin 1 is the board's 3.3-V net and should be used as target voltage reference only if your exact ST-Link supports that. Confirm the probe's actual pinout, especially for clones.

## Verified probe map

All stated voltages are **nominal design expectations**, not measured readings or validated pass tolerances. Stop for unexplained deviation, heating, excessive current, or unstable power; do not infer a safe tolerance from this table.

| Probe point | Native net | Expected observation / use |
|---|---|---|
| TP9; TP10/11/12; TP34/35/36 | GND | Meter/scope reference |
| TP15 | USB_VBUS | Approximately 5 V from USB when powered |
| TP33 | USB_REG_IN | Regulator input after the fitted diode path; below VBUS by its actual forward drop |
| TP6 | 3V3 | Approximately 3.3 V |
| TP7 | 1V8_IO | Approximately 1.8 V from the local regulator |
| TP16 | REF1V25 | Approximately 1.25 V precision-reference output |
| TP14 | NRST | Low during reset; approximately 3.3 V when released |
| TP1 / J1.1 | GPU_PWM_1V8 | Controlled fixture PWM INPUT, 0 to 1.8 V, nominally 675 kHz |
| TP2 / J1.2 | VRM_PWM_1V8 | Output observation, after the fitted 100-ohm series resistor; do not externally drive while output enabled |
| TP3 | PWM_TIM1_3V3 | MCU PWM source, nominally 675 kHz and 0 to 3.3 V in PREARM/ARMED; inactive in PASSIVE |
| TP4 / J1.3 | NVVDD_SENSE | Controlled low-voltage fixture sense input; begin near 0.8 V, measured by meter, not connected to a GPU rail |
| TP13 | COMP2_OVP | MCU comparator fault output; low healthy, high on comparator overvoltage |
| TP17 | OVP_THRESHOLD | External comparator threshold; nominal calculation about 0.648 V (see below) |
| TP18 / TP19 | HOST_ARM_RAW / HOST_ARM | MCU arm request / gated latch-clock input; normally low, brief deliberate pulse only when arming |
| TP20 / TP21 | HOST_PERMIT_RAW / HOST_PERMIT | MCU permit / gated permit; low in PASSIVE, reset and fault |
| TP22 | CONFIG_VALID | Low with JP1 open; high when intentionally fitted on isolated fixture |
| TP23 | RESET_OK | Reset-health branch; high in stable released reset |
| TP24 | COMP_OK_FAST | Comparator-health branch; high healthy |
| TP25 | OVP_OK | External OVP health; high healthy, low external fault |
| TP26 / TP27 | RISE_RECENT / FALL_RECENT | Source edge-watchdog branches; only expected high with qualifying source edges |
| TP28 / TP29 | EDGE_OVP_OK / SYSTEM_OK | Combined health logic; diagnose rather than override |
| TP30 / TP31 | HEALTH_CLEAR_N / HARD_CLEAR_N | Active-low latch-clear conditions; JP1 open must prevent authorization |
| TP8 | RUN_PERMISSION | Actual latch Q; low until successful deliberate arm; MCU reads through 47 kohms |
| TP5 | FAULT_NORUN | Latch complementary output; opposite of RUN in valid powered operation |
| TP32 | SYNTH_OE_N | Driver active-low enable; high (nominally 1.8 V) disables the output, low enables it |

P8 SWD: J6.1=3V3, J6.2=GND, J6.3=SWDIO, J6.4=SWCLK. Separate reset header: J7.1=NRST, J7.2=GND. Check orientation physically; pin order is not a connector-view drawing.

The external threshold design has R16=8.45 kohms and R17=10 kohms in parallel with R18=100 kohms, fed by nominal 1.25 V: 1.25 × (10k || 100k)/(8.45k + (10k || 100k)) ≈ 0.648 V. With the nominal 2:1 rail-sense divider this corresponds to about 1.296 V. Comparator offset, reference/resistor tolerances, sense switching and dynamics are not included. The firmware's internal threshold is nominally 1.250 V referred to rail. Neither calculation establishes a 1.5-V peak limit.

## Stage 1: inspect and first flash

1. With no power/GPU connected, inspect assembly/orientation/solder bridges and check supply-to-ground for a persistent short. Charging capacitors can affect resistance readings; no universal resistance threshold is assumed.
2. Leave JP1 open. Apply the intended power and verify TP15, TP33, TP6, TP7, TP16 and TP14. Observe supply current and stop for heating or instability.
3. Connect the verified ST-Link wiring, read device identity and option bytes, then program and verify the telemetry image using the first-flash guide. Do not blindly write a full option-byte word, enable readout protection, or disable debug access. If parity-enabled startup is unsupported, record the readback and follow the explicit recovery instructions.
4. Confirm the host sees the expected USB device and serial port. Check TELEMETRY with the port opened; source acquisition begins when CDC/DTR is active. Save actual image hash, device ID/revision and option-byte readback.
5. In PASSIVE, confirm TP20/21 and TP18/19 low, TP8 low and TP32 high. Reset, close serial/DTR, unplug USB and reconnect. There must be no autonomous arm. Output voltage alone does not prove Hi-Z; use driver-enable observation and a controlled nonconflicting fixture load for later characterization.

## Stage 2: sense and acquisition, still JP1 open

1. A disconnected sense input is not a healthy real rail. Use a measured current-limited low-voltage source at TP4/J1.3 referenced to board GND. Begin at 0.8 V and keep routine sense checks within 0–1.2 V. Do not feed that source through the ST-Link or into a supply rail.
2. The three fitted sense dividers are nominally 2:1. At stable reset release a nominal 0.8-V sense input should produce approximately 0.4 V at the ADC/comparator branches; probe component pads only if you can do so without shorting adjacent pads. Compare TELEM rail_mV with the meter; document measured error before making a calibration profile.
3. Supply the GPU-input fixture signal at TP1/J1.1: 0–1.8 V, 675 kHz nominal, for example 64/240 duty (26.67%) to start inside the supported arm window. A generator configured for a 50-ohm load can produce twice its displayed voltage into a high-impedance input; verify actual amplitude and offset on the scope before connection. Never substitute 3.3-V or 5-V PWM without checking receiver ratings.
4. Verify GPU period/high capture against the scope, including missing input and duty changes. At 162 MHz, nominal period is 240 timer ticks. Normalized duty preview is reconstructed from /8 decimated independent observations; it is not same-cycle capture or a phase-locked forwarding path.
5. Deliberately enter PREARM with JP1 still open. Confirm TP3 waveform and reconstructed duty tracking, while TP8 remains low and TP32 high. Check source frequency and clean preload duty updates. PREARM is not permission to drive TP2.

## Stage 3: active isolated fixture

Proceed only after prior stages behave correctly. Power down to set JP1; leave GPU disconnected. Use the firmware/host's explicit isolated-fixture qualification and a profile derived from the fixture measurements, not the example template.

- Start with observed input CCR <=96; ARM rejects higher starting duty because the implemented low-phase strobe window cannot safely support it. Do not silently choose an alternate rail voltage to bypass this limitation.
- Trigger the scope on TP18 or TP19. Observe TP3, TP8 and TP32 over repeated deliberate arm attempts. Confirm one arm pulse, latch RUN assertion, correct output enable and TP2 output amplitude/timing. Compare phase transitions and look for runt pulses/overshoot; no timing claim is accepted from software tests alone.
- Once ARMED, exercise calibrated target changes in both directions. Observe TP2 response and feedback convergence; rapid repeated SET commands must not postpone a pending mismatch deadline indefinitely.
- Exercise STOP, DISARM/PASSIVE, lease expiration, host-process termination, DTR close, USB disconnect, input loss and sense faults. Observe actual TP8 and TP32 transitions plus output release. A cleared fault must not automatically rearm.
- Measure internal comparator trip/release by a controlled sense sweep with GPU absent. The internal trip will ordinarily occur before the external OVP trip; observe TP13 and TP25 independently rather than assuming both branches are proven by one RUN drop. Use a suitable voltage source and documented board limits; do not short TP13/TP25 or defeat interlocks.
- Verify watchdog behavior without debugger freeze hiding it. Test resets/power ordering with scope captures. Additional intentional oscillator-failure injection needs a suitable safe fixture; do not short crystal pins as a casual test.

## Record and release gate

Record firmware hash, board revision, probe/source settings, measured supply rails, ADC error, comparator thresholds, output timing, fault response captures and any discrepancy. Missing equipment or a failed check is an open acceptance item. An oscilloscope and multimeter alone cannot generate all required input/sense conditions.

This board is full-intercept, not a native bypass. Actual GPU/VRM fallback, calibration and rail overshoot require a separate physical qualification plan. Do not connect it to the GPU merely because this isolated test passes or a binary builds. A Hi-Z replacement driver is not proof that the VRM shuts down safely.

# P8 ROM USB-DFU electrical conflict

This review identifies an electrical conflict with application-to-ROM DFU on
the P8 board. USB updating uses the board-aware custom loader instead.

## Primary evidence

ST AN2606 Rev 70, section 52, Table 115 pp274–276 and Figure 68 p277:
https://www.st.com/resource/en/application_note/an2606-introduction-to-system-memory-boot-mode-on-stm32-mcus-stmicroelectronics.pdf

The G431 ROM supports USB DFU at PA11/PA12. System memory starts at 0x1FFF0000. The separate 0x1FFF6800 address is a securable-memory exit helper, not the ROM vector base.

The ROM also initializes SPI/USART/I2C. Figure 68 initializes SPI before checking USB selection. Its table notes SPI MISO becomes a 3.3-V driven output during initialization.

## Native P8 wiring

- /VRM_CAPTURE_3V3 has U1 pin14 (PA6) directly connected to U3 pin4.
- U3 is SN74AUP1T17DCKR, an always-driven receiver; pin1 is NC, not an enable.
- ROM uses PA6 as SPI1 MISO. Thus a low receiver output can oppose the ROM's high output. There is no series resistor between these outputs.
- /RUN_SENSE PB14 is also ROM SPI2 MISO, but has R49=47 kohms between MCU and RUN.
- PA2 becomes USART2 TX, conflicting with its application analog-sense role.

JP1 open and GPU unplugged are necessary precautions, but do not eliminate the internal PA6/U3 connection. A software reset or peripheral teardown before the ROM jump also does not stop the ROM from reconfiguring PA6.

## Recommendation

Do not enable an unrestricted ROM DFU command or describe it as board-safe based only on compilation. Resolve with a documented, reviewed electrical procedure that prevents conflicting output levels throughout entry, or implement a board-aware USB updater that preserves application pin constraints. An entry address correction or watchdog fix alone is insufficient.

A reset-based one-shot request is otherwise preferable to a direct live-application jump because the application's IWDG is already running. A no-init marker must have a complementary validation word, be consumed once, and be conditioned on the intended reset reason, outside normal startup clearing; ROM vector/MSP validation and interrupt reset-state handoff are separate requirements. No option-byte changes are authorized by this review.

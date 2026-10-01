# PWMVID STM32 firmware

The R3 application for STM32G431CBT6 observes GPU PWMVID and rail telemetry,
generates timed PWM sequences, and controls the output gate through PASSIVE,
PREARM, and ARMED states. The project also contains a custom USB bootloader,
Python host utilities, and native tests.

`platformio.ini` defines these build environments:

| Environment | Purpose |
| --- | --- |
| `telemetry_cb` | CB application with output permission disabled |
| `fixture_cb` | CB application with sequence output enabled |
| `sequencer_bench_cb` | Synthetic-input bench build for GPU-disconnected testing |

From this directory:

```sh
sh tests/run_tests.sh
sh bootloader/tests/run_all.sh
pio run -e telemetry_cb
pio run -e fixture_cb
```

The native tests use a C11 compiler and Python 3. PlatformIO downloads the
pinned STM32 packages for firmware builds. The USB VID/PID are demonstration
identifiers; select an assigned identity for a product.

Use the [bench acceptance checklist](docs/BENCH_ACCEPTANCE.md) to qualify
output and protection behavior on the intended board. The
[ROM DFU pin review](docs/ROM_DFU_PIN_CONFLICT.md) explains why this PCB uses a
custom bootloader.

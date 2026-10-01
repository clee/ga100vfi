# PWMVID and GPU0 tools

This repository contains a PWMVID control-board design, STM32 firmware and host
tools, GA100 GPU0 register capture and analysis code, and an experiment-loop
description. Together they support board bring-up, PWM waveform experiments,
and checking how GPU register state changes after an attempt.

| Path | What it contains |
| --- | --- |
| [`hardware/`](hardware/README.md) | P8 KiCad schematic and PCB, test-pad map, and P9 assembly references |
| [`firmware/`](firmware/README.md) | STM32G431 application, custom USB bootloader, host utilities, and tests |
| [`driver/pwmvidctl/`](driver/pwmvidctl/README.md) | Operator CLI, sequence planner, calibration profiles, and waveform examples |
| [`gpu0-registers/`](gpu0-registers/README.md) | GA100 register maps, bounded capture engine, and offline state analysis |
| [`glitch-loop/`](glitch-loop/README.md) | Experiment-loop pseudocode and historical commanded waveforms |

## Offline checks

```sh
(cd firmware && sh tests/run_tests.sh && sh bootloader/tests/run_all.sh)
(cd driver/pwmvidctl && python3 -m unittest discover -s tests -v)
(cd gpu0-registers && cargo test --workspace --locked)
```

Building the STM32 images requires PlatformIO; see the firmware README for
build environments. Hardware bring-up requires the measurements in
[`firmware/docs/BENCH_ACCEPTANCE.md`](firmware/docs/BENCH_ACCEPTANCE.md).

Original project material is licensed under [MIT](LICENSE). Third-party terms
and source notices are described in [LICENSING.md](LICENSING.md).

# Native USB transport tests

Run `tests/usb_mock/run.sh` from any directory. Requires Python 3 and a C11
compiler with AddressSanitizer/UBSan. `SANITIZE=0` runs without sanitizers;
`CC=clang` or another compiler can be selected.

`generate.py` compiles the queue/session section of the actual current
`src/usb_transport.c`, with hardware and stack API definitions supplied by
`mock_platform.h`. The MCU descriptors/PCD bridge are excluded at a checked
source boundary. The test does not substitute a second transport implementation.
All generated files are temporary and removed by the runner.

Covers CRLF and fragmented lines, all buffer limits, rejected text, loss clearing
queued/partial data, DTR recovery, simulated suspend/resume session gating,
TX busy/drop behavior and buffer ownership, counter changes, BASEPRI restoration,
and the static allocator. These are software-logic checks only: no actual USB
enumeration, host class driver, NVIC timing, peripheral registers or electrical
behavior are exercised. The real HAL callbacks are compiled in ARM builds, but
their hardware effects need board-level validation.

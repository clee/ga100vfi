#!/usr/bin/env python3
"""Build a native test translation unit from the current transport logic.

Only the MCU-specific descriptors and PCD bridge are excluded. No duplicate
queue/session implementation is tested. The generated file stays in a temporary
build directory, so normal PlatformIO builds do not see mock definitions.
"""
from pathlib import Path
import sys

root = Path(__file__).resolve().parents[2]
source = (root / "src/usb_transport.c").read_text()
marker = "/* Device and string descriptors."
if source.count(marker) != 1:
    raise SystemExit("Transport structure changed; review the mock extraction boundary")
source = source.split(marker)[0]
source = "\n".join(line for line in source.splitlines()
                   if not line.startswith("#include "))
Path(sys.argv[1]).write_text('#include "mock_platform.h"\n' + source +
                            '\n#include "test_usb.c"\n')

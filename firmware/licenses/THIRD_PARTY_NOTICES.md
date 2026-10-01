# Third-party notices

The startup and system source files originate in STMicroelectronics CMSIS device
templates distributed with pinned STM32CubeG4 1.6.3. Original copyright headers
are retained; modified files are marked. The official ST CMSIS device license
is included as ST-CMSIS-DEVICE-G4-LICENSE.md, sourced from
https://raw.githubusercontent.com/STMicroelectronics/cmsis-device-g4/master/LICENSE.md.

HAL, USB middleware, ARM toolchain and PlatformIO packages are fetched as pinned
build dependencies and are not bundled here. Their licenses remain applicable.
The host client depends on pyserial 3.5. Firmware builds use the pinned ST
HAL, CMSIS and USB middleware. ST USB middleware is licensed under SLA0044:
https://www.st.com/SLA0044. Original copyright notices remain in the sources.
This file does not grant a USB VID/PID assignment.

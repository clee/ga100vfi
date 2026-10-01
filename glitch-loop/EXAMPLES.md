# Historical commanded examples

These commanded waveforms come from earlier GPU specimens. The reported held
observations are specific to those specimens; the timing values are command
parameters rather than scope measurements.

| Example | Commanded pattern | Reported held observation |
| --- | --- | --- |
| A | 900 mV background; three 2 ms lows starting at 979,940, 983,940 and 987,940 µs | Readable PCI ID `20c2`, CRC55/idle, 17 selected published security aliases zero |
| B | 900 mV background; 960,000/60,000 µs pattern | Same selected phenotype |
| C | 900 mV background; 959,880/60,200 µs continuation pattern | Selective state; later independently reviewed write-route evidence |
| D | 800 mV background; three brief lows at 970,755, 972,005 and 975,005 µs; 50 µs CCR1 tail | Readable PCI ID `20c2`, CRC55/idle, 17 selected published security aliases zero |

The 17 aliases below are published option values whose mapped primary and
redundant bits are in physical rows 0 and 1. In the selected held phenotype,
each published alias was reported as `0x00000000`. Published alias values,
physical row words, and write-route eligibility are distinct observations.

| Published alias | Held value |
| --- | --- |
| `NV_FUSE_OPT_DEBUG_FEATURE_SW_OVERRIDE_ENABLE` | `0x00000000` |
| `NV_FUSE_OPT_DEBUG_SW_OVERRIDE_ENABLE` | `0x00000000` |
| `NV_FUSE_OPT_HDCP_RAM_ACCESS_DIS` | `0x00000000` |
| `NV_FUSE_OPT_IST_DEBUG_FA` | `0x00000000` |
| `NV_FUSE_OPT_PRIVKEY_ACC_DIS` | `0x00000000` |
| `NV_FUSE_OPT_PRIV_SEC_EN` | `0x00000000` |
| `NV_FUSE_OPT_SECURED_BURN_FEATURE_ENABLE` | `0x00000000` |
| `NV_FUSE_OPT_SECURE_FUSE_RESENSE_DISABLE` | `0x00000000` |
| `NV_FUSE_OPT_SECURE_GSP_DEBUG_DIS` | `0x00000000` |
| `NV_FUSE_OPT_SECURE_MINION_DEBUG_DIS` | `0x00000000` |
| `NV_FUSE_OPT_SECURE_NVDEC_DEBUG_DIS` | `0x00000000` |
| `NV_FUSE_OPT_SECURE_PMU_DEBUG_DIS` | `0x00000000` |
| `NV_FUSE_OPT_SECURE_PRI_SOURCE_ISOLATION_EN` | `0x00000000` |
| `NV_FUSE_OPT_SECURE_SECENGINE_DEBUG_DIS` | `0x00000000` |
| `NV_FUSE_OPT_SECURE_XUSB_DEBUG_DIS` | `0x00000000` |
| `NV_FUSE_OPT_SFK_ACC_DIS` | `0x00000000` |
| `NV_FUSE_OPT_XV2CLK_RESET_RETIME_DISABLE` | `0x00000000` |

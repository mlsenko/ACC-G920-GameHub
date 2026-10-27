# Tools

Small probes used while bringing G920 FFB up. Not required to run the app.

| File | Purpose |
| --- | --- |
| `g920_read.swift` | Dump HID values / GameController presence |
| `g920_ffb_test.swift` | Direct HID++ constant-force smoke test |
| `g920_ffb_sdl_test.c` | Load the shim and call SDL haptic APIs |
| `g920_ffb_timing.c` | Measure UpdateEffect latency |

These can move the wheel. Do not run them while ACC owns the device.

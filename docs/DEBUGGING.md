# Debugging Reference

[TESTING.md](../TESTING.md) is the step-by-step bring-up ladder — start there.
This document is the reference for *fixing* a specific failed rung: hardware
checks, log interpretation, and symptom → cause tables.

## Enable full logging

```yaml
logger:
  level: VERBOSE
  logs:
    proflame2: VERBOSE
    cc1101: VERBOSE
    remote_transmitter: VERBOSE
```

```bash
esphome logs proflame2_test.yaml
```

Key log lines and what they tell you:

| Log line | Meaning |
|---|---|
| `[cc1101] Chip ID: 0x0014` | SPI works, chip alive (`0x0004`/`0x0017` also fine) |
| `[cc1101] Failed to verify CC1101.` | Chip didn't answer over SPI; the component is marked failed |
| `[cc1101] Frequency: 314972900 Hz` | ~314.973 MHz programmed correctly |
| `[proflame2] Frame: id=... cmd1=... err1=...` | Exactly what rtl_433 should decode — compare field-for-field |
| `[proflame2] Sending burst: ... 958 bits, ... 400ms` | Burst handed to `remote_transmitter` |
| `[cc1101] Beginning TX sequence` (VERBOSE) | Radio switching to TX for the burst |
| `[cc1101] Beginning RX sequence` (VERBOSE) | Burst finished, radio back in RX, GDO0 released |
| `[cc1101] Failed to enter TX state!` | Radio didn't reach TX — SPI, supply or calibration problem |
| `[cc1101] PLL lock failed ...` | VCO didn't lock — check crystal, supply voltage |
| `[remote_transmitter] rmt_transmit failed` | RMT couldn't start — check the transmitter pin |

## Hardware checklist

- **Power**: CC1101 is 3.3 V only — 5 V kills it. Add a 100 nF decoupling cap
  near VCC if you see erratic behavior; long dupont wires on SPI are a common
  source of flakiness (keep under ~15 cm).
- **Wiring** (must match your YAML, not the diagram — check both):
  CLK→SCLK, MISO→SO(GDO1), MOSI→SI, CS, and GDO0 → the `remote_transmitter`
  pin (required: it carries the transmit data).
- **Antenna**: a quarter-wave wire for 315 MHz is **23.8 cm** (not the 17.3 cm
  used for 433 MHz). Solder it to the ANT pad; no antenna = millimeters of range
  and a possibly stressed PA.
- **Module band**: "433 MHz" CC1101 modules tune to 315 MHz fine (the chip
  covers 300–348 MHz), but their antenna matching network is optimized for 433 —
  expect reduced range. A 315 MHz module (e.g. E07-M1101S variant) is better.
- **Crystal**: ESPHome's `cc1101` component assumes a 26 MHz crystal. A 27 MHz module would
  transmit at ~327 MHz instead of 315 (see frequency sweep below to detect this).

## Symptom → cause

### `Part Number: 0xFF` (or all registers read 0xFF/0x00)

Shows up as `Failed to verify CC1101.` with the `cc1101` component marked
failed. SPI failure. In order of likelihood: CS pin doesn't match YAML; MISO
not connected; 5 V on VCC (chip dead); swapped MOSI/MISO.

### rtl_433 sees nothing when the ESP transmits

1. Confirm the ESP *thinks* it transmitted (`Sending burst` in logs, then
   `Beginning TX sequence` / `Beginning RX sequence` at VERBOSE). If
   `Failed to enter TX state!` or `PLL lock failed` appear, it's a radio/SPI
   problem, not RF.
2. Press "DEBUG: 500ms Carrier". No carrier on the SDR means GDO0 isn't wired
   to the `remote_transmitter` pin, or `modulation_type` isn't `ASK/OOK`.
3. Antenna connected?
4. **Frequency sweep**: run `rtl_433 -f 315M -A` and press transmit. Nothing?
   Try scanning wider — a 27 MHz-crystal module lands near 327 MHz:
   `rtl_433 -f 327M -A`. Or watch a waterfall (GQRX/SDR++/URH) from 310–330 MHz.

### rtl_433 sees pulses but won't decode (`-R 207` silent)

Compare `rtl_433 -A` pulse analysis of the ESP vs the remote (you saved the
remote's baseline per [CAPTURE.md](CAPTURE.md)):

- Pulse widths should be ~396/804/1212 µs. The ESP sends exact multiples of
  416.7 µs; rtl_433 reads OOK pulses slightly short, as it does for the remote.
- 35 sync pulses (1212 µs) per burst. Wrong count → word/packet structure.
- Try `rtl_433 -f 315M -R 207 -M bits` to see the raw decoded rows.

### rtl_433 decodes, but fields don't match the ESP's `Frame:` log

- Wrong `id` → `serial_number:` typo (remember rtl_433 JSON prints decimal).
- Wrong `err1`/`err2` → checksum constants; re-derive per
  [CAPTURE.md](CAPTURE.md) and confirm with
  `python3 tools/verify_protocol.py selftest` that the code itself is sound.
- Wrong `cmd1`/`cmd2` → entity state isn't what you think; check the ESP log.

### rtl_433 decodes perfectly, fireplace doesn't respond

The information is right; the RF link to the *fireplace's* receiver isn't.

1. **Frequency match**: compare the `freq` field of ESP captures vs remote
   captures **taken with the same dongle** — dongle ppm error cancels out.
   Adjust `cc1101: frequency:` in the YAML until they match (try `314.973MHz` →
   `315.000MHz` → `315.070MHz`).
2. **Power/distance**: start with the ESP a meter from the fireplace. The
   remote reaches ~10 m; a mismatched 433 MHz antenna may manage far less.
3. **Pairing**: the receiver only obeys serials it has learned. If the original
   remote still works, your cloned serial is paired; if you changed serials,
   re-pair (LEARN button on the IFC board).
4. **Watch for the echo**: the receiver echoes accepted commands back over RF.
   `python3 tools/pf2_monitor.py` flags it automatically as `ECHO (receiver
   ACK)`. Echo present = fireplace accepted the command (any remaining problem
   is fireplace-side, e.g. pilot mode, valve).

### Commands work but are occasionally missed

- RF is fire-and-forget with no retry above the 5-in-burst repetition; increase
  ESP proximity or antenna quality.
- Bit timing comes from the ESP32's RMT peripheral, so main-loop load or WiFi
  activity can't distort a burst once it has started.

## Capturing evidence for a bug report

For anything you can't resolve, capture:

```bash
tools/pf2_capture.sh bug-report-1                 # decodes + raw .cu8 samples + env info
esphome logs proflame2_test.yaml | tee esp.log    # VERBOSE component logs
```

plus your YAML (redact secrets) and `dump_config` output. The `.cu8` files can
be re-decoded offline (`rtl_433 -r file.cu8 ...`) — they make the failure
reproducible for anyone helping.

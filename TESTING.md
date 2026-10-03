# Bring-Up & Verification Guide

Work through the rungs in order — each one isolates a layer, so a failure tells
you exactly where the problem is and what evidence to capture before changing
anything.

The key insight: **rtl_433 with your RTL-SDR is a reference receiver.** If it
decodes our transmission *identically* to the real remote's, the fireplace
receives the same information from both. At that point only RF-level issues
(power, frequency offset, antenna) can remain.

Keep two terminals open throughout:

```bash
# Terminal 1: ESP logs
esphome logs proflame2_test.yaml

# Terminal 2: reference receiver (add -A for pulse analysis when needed)
rtl_433 -f 315M -R 207 -F json
# or, much friendlier (hex fields, checksum validation, echo detection):
python3 tools/pf2_monitor.py
```

## Rung 0 — Protocol math (no hardware)

```bash
python3 tools/verify_protocol.py selftest
```

All tests must pass. Run this after *any* change to the packet-building or
encoding code — it checks against real SDR captures of the paired remote and
the smartfire reference example.

## Rung 1 — SPI / chip sanity (ESP logs only)

The radio is configured by ESPHome's `cc1101` component. On boot it resets the
chip and reads its part number and version; its `dump_config` should show:

| Field | Expected | If wrong |
|---|---|---|
| `Chip ID` | `0x0014` (or `0x0004`, `0x0017`) | `Failed to verify CC1101.` and the component is marked failed → SPI wiring / CS pin / MISO |
| `Frequency` | `314972900 Hz` or close | `cc1101: frequency:` not set — the default is 433.92 MHz |
| `Modulation` | `ASK/OOK` | set `modulation_type: ASK/OOK` |
| `Output Power` | `10.0 dBm` | set `output_power: 10` |

`remote_transmitter` should also report its pin with no `Configuring RMT
driver failed` error.

## Rung 2 — Raw RF out ("DEBUG: 500ms Carrier" button)

Sends 500 ms of unmodulated carrier through the full GDO0 → CC1101 path. Watch
a waterfall (GQRX/SDR++/URH) at 315 MHz, or run `rtl_433 -f 315M -A`: you
should see one solid ~500 ms pulse per press.

- **Nothing received** → antenna, frequency, GDO0 not wired to the
  `remote_transmitter` pin, or the radio never entered TX (check the ESP log
  for `Failed to enter TX state!` / `PLL lock failed`).
- **Carrier present but never stops** → GDO0 stuck high; check that
  `remote_transmitter` and `cc1101: gdo0_pin` use the same GPIO.

## Rung 3 — Full protocol frame ("DEBUG: Replay Captured Frame" button)

This transmits the exact state captured from the real remote on 2025-12-29.
Terminal 2 (`-R 207`) must print:

```json
{"model": "Proflame2-Remote", "id": 11179010, "cmd1": 2, "cmd2": 38,
 "err1": 188, "err2": 70, ..., "mic": "CHECKSUM"}
```

(`11179010` = `0xAA9402`, `38` = `0x26`, `188` = `0xBC`, `70` = `0x46`.)

The ESP logs the same fields at TX time (`Frame: id=aa9402 cmd1=02 cmd2=26
err1=bc err2=46`) — the two must match field-for-field. For any other entity
state, predict the expected decode with:

```bash
python3 tools/verify_protocol.py frame --serial 0xAA9402 --constants F,E,E,2 \
    --power --flame 3
```

The ESP's `Sending burst:` log line should report `958 bits` and `400ms`.
Also compare pulse analysis (`-A`) against the real remote's signature:

| Metric | Real remote (dec 29 capture) |
|---|---|
| Burst length | ~395 ms, 325 pulses |
| Pulse widths | ~396 / ~804 / ~1212 µs |
| 1212 µs pulse count | 35 (7 word-syncs × 5 packets) |
| Inter-packet gaps | 4 gaps of ~5.2 ms (12 zero bits) |

## Rung 4 — The fireplace itself

Per the smartfire docs, **the receiver echoes accepted commands back over RF**.
Keep terminal 2 running: after our burst you should see a *second* decode of
the same frame — that echo is a definitive ACK from the fireplace, observable
even with the pilot off.

1. Confirm the original remote still works (baseline + keep it as backup).
2. Press "Replay Captured Frame" near the fireplace. Watch for the echo.
3. Then try real state changes (fan is the safest first test — audible, no gas).

## Failure → diagnosis matrix

| Symptom | Most likely layer | Evidence to capture |
|---|---|---|
| rtl_433 sees nothing from ESP | RF hardware / radio never TXes | ESP log at VERBOSE (`cc1101` `Failed to enter TX state!` / `PLL lock failed`, `remote_transmitter` errors); try Rung 2 |
| Pulses visible but garbage / wrong count | Bit timing or encoding | `rtl_433 -f 315M -A` output for ESP vs remote, side by side |
| Decodes but `mic` missing or parity fails | Word structure / parity | The raw rows: `rtl_433 -f 315M -R 207 -M bits` |
| Decodes but wrong id/cmd/err | State or constants mismatch | ESP `Frame:` log line vs rtl_433 JSON |
| rtl_433 decode is perfect, no fireplace echo | Frequency offset, TX power, or pairing | Try `cc1101: frequency: 315.000MHz` then `315.070MHz` (remote measured ~315.07 on our dongle); move ESP closer; re-pair (LEARN button) |
| Echo seen but no flame | Fireplace-side (valve/pilot mode) | Compare with what the real remote does for the same command |

When filing away a failing capture for later analysis, record raw samples too:

```bash
rtl_433 -f 315M -R 207 -S unknown   # writes .cu8 sample files of undecoded bursts
```

## Safety

This controls a gas appliance. Test with the fan first, keep the original
remote within reach, verify OFF works reliably before automating anything,
and consider a Home Assistant automation that forces power off after a
maximum runtime.

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
```

## Rung 0 — Protocol math (no hardware)

```bash
python3 tools/verify_protocol.py selftest
```

All tests must pass. Run this after *any* change to the packet-building or
encoding code — it checks against real SDR captures of the paired remote and
the smartfire reference example.

## Rung 1 — SPI / chip sanity (ESP logs only)

On boot, `dump_config` should show:

| Field | Expected | If wrong |
|---|---|---|
| CC1101 Part Number | `0x00` | `0xFF` or `0x00` for *everything* → SPI wiring/CS pin |
| CC1101 Version | `0x14` (or `0x04`, `0x17`) | `0x00`/`0xFF` → MISO not connected or wrong chip |
| FREQ | `0x0C1D46` (314.973 MHz) | config not applied — check reset sequence |
| MDMCFG2 | `0x30` | " |
| PKTCTRL0 | `0x00` | " |
| FREND0 | `0x11` | " |

The "DEBUG: Check CC1101 Config" button re-reads these at runtime and also
verifies MDMCFG4/3 = `0xF6`/`0x83` (2400 baud) and PA table = `00`/`C0`.

## Rung 2 — Raw RF out ("DEBUG: Minimal TX Test" button)

Sends a 23-byte `AA55...` pattern. In terminal 2, run `rtl_433 -f 315M -A`
instead: you should see an OOK pulse train of ~184 bits with uniform ~416 µs
timing each press.

- **Nothing received** → antenna, PA table, frequency, or the radio never
  entered TX (check ESP log for `MARCSTATE` warnings / `TX error`).
- **Received but timing ≠ ~416 µs** → data rate registers.

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
| rtl_433 sees nothing from ESP | RF hardware / radio never TXes | ESP log at VERBOSE (`MARCSTATE`, `TX error/timeout` lines); try Rung 2 |
| Pulses visible but garbage / wrong count | Data rate or encoding | `rtl_433 -f 315M -A` output for ESP vs remote, side by side |
| Decodes but `mic` missing or parity fails | Word structure / parity | The raw rows: `rtl_433 -f 315M -R 207 -M bits` |
| Decodes but wrong id/cmd/err | State or constants mismatch | ESP `Frame:` log line vs rtl_433 JSON |
| rtl_433 decode is perfect, no fireplace echo | Frequency offset, TX power, or pairing | Try `frequency: 315.000MHz` then `315.070MHz` (remote measured ~315.07 on our dongle); move ESP closer; re-pair (LEARN button) |
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

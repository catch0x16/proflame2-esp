# Capturing Your Remote with an RTL-SDR

To control your fireplace you must clone two things from your existing remote:
the **24-bit serial number** and the **four checksum constants** (C1/D1/C2/D2).
Both come from a few minutes of SDR capture. This guide walks through it.

## What you need

- Any RTL-SDR USB dongle (RTL2832U based, ~$30). A HackRF or similar also works.
- [rtl_433](https://github.com/merbanan/rtl_433) — includes a ProFlame 2
  decoder (`-R 207`, model `Proflame2-Remote`).
- Your working fireplace remote.

Install rtl_433 natively (`brew install rtl_433`, `apt install rtl-433`) or run
it in Docker:

```bash
# Find your dongle's bus/device first with `lsusb`
docker run --device /dev/bus/usb/001/003 hertzg/rtl_433 -f 315M -R 207 -F json
```

If you have multiple SDR dongles attached, pick one with `-d <index>`.

## Step 1 — Capture command decodes

The easiest way is the monitor tool, which wraps rtl_433 and does the hex
conversion, state decoding, checksum validation, and constant derivation for
you:

```bash
python3 tools/pf2_monitor.py            # Ctrl-C when done -> prints YAML constants
```

Press buttons on the remote; each frame prints as:

```
21:04:04  id=aa9402  cmd1=02 cmd2=26 err1=bc err2=46  checksum OK
          power=off flame=6 fan=2 light=0 thermo=ON aux=off front=off pilot=IPI
```

If you prefer raw rtl_433 (or are running it in Docker, piping to
`pf2_monitor.py --stdin` also works):

```bash
rtl_433 -f 315M -R 207 -F json
```

Each press prints a JSON line like:

```json
{"model": "Proflame2-Remote", "id": 11179010, "cmd1": 2, "cmd2": 38,
 "err1": 188, "err2": 70, "pilot": 0, "light": 0, "thermostat": 1,
 "power": 0, "front": 0, "fan": 2, "aux": 0, "flame": 6, "mic": "CHECKSUM"}
```

Notes:
- Values are **decimal** in JSON output (`11179010` = `0xAA9402`).
- `mic: CHECKSUM` here means the per-word parity validated, so the decode is
  trustworthy — rtl_433 does **not** validate `err1`/`err2` (it can't; the
  constants are device specific).
- Capture at least two presses with **different** commands (e.g. two flame
  levels) so you can cross-check the derived constants.

## Step 2 — Extract the serial number

Convert `id` to hex: `printf '0x%X\n' 11179010` → `0xAA9402`. That is your
`serial_number:`.

## Step 3 — Derive the checksum constants

If you used `pf2_monitor.py`, the session summary already printed the YAML
block **and** verified consistency across every captured frame — paste it and
skip ahead. Deriving manually from a raw rtl_433 JSON line instead (values in
hex or decimal):

```bash
python3 tools/verify_protocol.py derive --cmd1 0x02 --err1 0xBC --cmd2 0x26 --err2 0x46
```

It prints the exact lines for your YAML:

```yaml
  checksum_c1: 0xf
  checksum_d1: 0xe
  checksum_c2: 0xe
  checksum_d2: 0x2
```

Now repeat with your *second* capture (different command) — it must produce the
**same four constants**. If it doesn't, one of the captures was corrupted;
capture more frames.

## Step 4 — Record the remote's RF signature (baseline for later comparison)

While you have the remote out, save the evidence you'll want if the ESP32
transmission ever needs debugging. One command records everything into a
timestamped directory (decoded frames, raw samples, environment info):

```bash
tools/pf2_capture.sh remote-baseline
```

Or manually:

```bash
# Pulse-timing analysis - press a button, save the output
rtl_433 -f 315M -R 207 -A

# Raw sample files (.cu8) of every burst, replayable/inspectable later
rtl_433 -f 315M -R 207 -S all
```

From the pulse analysis, note for later:

| Metric | Typical value (our reference remote) |
|---|---|
| Total burst | ~395 ms, ~325 pulses |
| Pulse widths | ~396 µs (1 bit), ~804 µs (2 bits), ~1212 µs (3 bits, word sync) |
| 1212 µs pulse count | 35 per burst (7 words × 5 packet repeats) |
| Long gaps | 4 × ~5.2 ms (the 12-zero-bit packet separators) |
| `freq` field in JSON | Remote's carrier as measured **by your dongle** |

The `freq` field matters: cheap dongles have ±50–100 ppm error, so absolute
readings are unreliable — but *relative* comparison with the same dongle is
excellent. When you later capture the ESP32's transmission, tune the
`frequency:` option until the ESP's `freq` field matches the remote's. That
cancels the dongle error entirely.

## Step 5 — Configure and verify

Put the serial and constants into your YAML, flash, and follow
[TESTING.md](../TESTING.md) — the ESP32's transmissions should decode
identically to the remote's, and `tools/verify_protocol.py frame` predicts the
expected decode for any entity state:

```bash
python3 tools/verify_protocol.py frame --serial 0xAA9402 --constants F,E,E,2 --power --flame 3
```

## Alternative: pairing a fresh serial

If you can't capture the remote (lost/broken), you can generate a random 24-bit
serial and pair it: hold the LEARN/PROG button on the fireplace's IFC board
(3 beeps, amber LED), then send any command from the ESP32 (4 beeps = paired).
**Caveat:** the checksum constants for an arbitrary serial are unknown — the
relationship between serial and constants hasn't been reverse engineered. If
the receiver validates the error words (all evidence suggests it does), only a
cloned remote is guaranteed to work. If you try a fresh serial and pairing
succeeds, please report it in a GitHub issue — that would tell us the receiver
learns or ignores the constants.

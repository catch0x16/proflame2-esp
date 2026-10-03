# ProFlame 2 ESPHome Component for ESP32 + CC1101

Control your ProFlame 2 fireplace system using an ESP32 and CC1101 RF module through Home Assistant with native ESPHome integration.

## Features

- ✅ Full control of ProFlame 2 fireplace systems
- ✅ Native Home Assistant integration via ESPHome
- ✅ Control power, flame height (0-6), fan speed (0-6), light level (0-6)
- ✅ Switch between IPI/CPI pilot modes
- ✅ Auxiliary power control
- ✅ No cloud dependency - fully local control
- ✅ Web interface for standalone control
- ✅ MQTT support (via ESPHome)

## Documentation

| Doc | What's in it |
|---|---|
| [TESTING.md](TESTING.md) | Step-by-step bring-up ladder: verify each layer from protocol math to the fireplace's RF echo |
| [docs/CAPTURE.md](docs/CAPTURE.md) | Capturing your remote with an RTL-SDR: serial number, checksum constants, RF baseline |
| [docs/DEBUGGING.md](docs/DEBUGGING.md) | Symptom → cause reference: SPI, antenna, frequency, log interpretation, evidence to collect |
| [docs/PROTOCOL.md](docs/PROTOCOL.md) | Full ProFlame 2 RF protocol specification with capture-verified corrections |
| [tools/verify_protocol.py](tools/verify_protocol.py) | Protocol self-test, checksum-constant derivation, expected-frame prediction |
| [tools/pf2_monitor.py](tools/pf2_monitor.py) | Live rtl_433 dashboard: hex fields, decoded state, checksum validation, echo (ACK) detection |
| [tools/pf2_capture.sh](tools/pf2_capture.sh) | One-command evidence bundle: JSON log + raw .cu8 samples + environment info |

## Hardware Requirements

- **ESP32 Development Board** (ESP32-WROOM-32 recommended)
- **CC1101 RF Module** (315 MHz capable - ProFlame 2 transmits at ~315 MHz)
- **Jumper wires** for connections
- **3.3V power supply** (USB power from ESP32 is sufficient)

### Compatible CC1101 Modules
- E07-M1101D (recommended)
- Generic CC1101 modules from AliExpress/eBay
- The ProFlame 2 protocol uses **314.973 MHz**, so prefer a 315 MHz module.
  A "433 MHz" module will usually still work (the CC1101 chip tunes 300-348 MHz)
  but its antenna matching network is not optimal at 315 MHz - expect reduced range.

## Wiring Diagram

### ESP32 to CC1101 Connections

```
ESP32          CC1101
-----          ------
3.3V    <-->   VCC
GND     <-->   GND
GPIO5   <-->   CSN (Chip Select)
GPIO18  <-->   SCK (SPI Clock)
GPIO23  <-->   MOSI (SPI Data Out)
GPIO19  <-->   MISO (SPI Data In) [Required: chip ID / state reads]
GPIO4   <-->   GDO0 [Required: TX data, keyed by remote_transmitter]
              GDO2 [Not connected]
```

The CC1101 runs in asynchronous serial mode: ESPHome's `cc1101` component
configures and tunes it over SPI, and `remote_transmitter` keys the carrier
by driving GDO0 with the ProFlame 2 bit timings.

### Pinout Diagram

```
    ESP32 DEVKIT V1
    ________________
   |                |
   | EN         D23 |---- MOSI (CC1101)
   | VP         D22 |
   | VN         TX0 |
   | D34        RX0 |
   | D35        D21 |
   | D32        D19 |---- MISO (CC1101) 
   | D33        D18 |---- SCK (CC1101)
   | D25        D5  |---- CSN (CC1101)
   | D26        TX2 |
   | D27        RX2 |
   | D14        D4  |---- GDO0 (CC1101)
   | D12        D2  |
   | D13        D15 |
   | GND        GND |---- GND (CC1101)
   | VIN        3V3 |---- VCC (CC1101)
   |________________|
```

## Installation

### Method 1: External Component from GitHub (Recommended)

```yaml
external_components:
  - source: github://j2deen/proflame2-esp@main
    components: [proflame2]
```

### Method 2: Local Component

1. **Copy the `components/proflame2` directory** next to your ESPHome YAML:
   ```bash
   cp -r components ~/esphome/
   ```

2. **Reference it in your configuration**:
   ```yaml
   external_components:
     - source:
         type: local
         path: ./components
       components: [proflame2]
   ```

3. **Create your ESPHome configuration**:
   ```bash
   cp proflame2_fireplace.yaml ~/esphome/my_fireplace.yaml
   ```
   Then edit WiFi credentials, API key, pin assignments, your serial number,
   and your checksum constants (see below), and:
   ```bash
   esphome run my_fireplace.yaml
   ```

## Configuration

### Basic Configuration

The radio is three blocks: ESPHome's built-in `cc1101` (radio setup and
TX/RX switching), a `remote_transmitter` on GDO0 (bit timing), and
`proflame2` (the protocol). GDO0 is shared between `cc1101` and
`remote_transmitter`, which is why both pins say `allow_other_uses`.

```yaml
spi:
  clk_pin: GPIO18
  miso_pin: GPIO19
  mosi_pin: GPIO23

cc1101:
  id: cc1101_radio
  cs_pin: GPIO5              # CC1101 chip select
  gdo0_pin:                  # Lets cc1101 release GDO0 when it returns to RX
    number: GPIO4
    allow_other_uses: true
  frequency: 314.973MHz      # ProFlame 2 is ~315 MHz - not the 433.92MHz default
  modulation_type: ASK/OOK
  output_power: 10

remote_transmitter:
  id: rf_tx
  pin:
    number: GPIO4            # Same GPIO as gdo0_pin
    allow_other_uses: true
  carrier_duty_percent: 100%
  non_blocking: true
  on_transmit:
    then:
      - cc1101.begin_tx: cc1101_radio
  on_complete:
    then:
      - cc1101.begin_rx: cc1101_radio

proflame2:
  transmitter_id: rf_tx      # Optional if there is only one remote_transmitter
  serial_number: 0xAA9402    # Your remote's serial number (24 bits)
  # Device-specific error-detection constants - REQUIRED for the fireplace to
  # accept commands. See "Checksum Constants" below for how to derive yours.
  checksum_c1: 0xF
  checksum_d1: 0xE
  checksum_c2: 0xE
  checksum_d2: 0x2

  power:
    name: "Fireplace Power"

  thermostat:
    name: "Thermostat Mode"

  front:
    name: "Front Flame"

  flame:
    name: "Flame Height"

  fan:
    name: "Fan Speed"

  light:
    name: "Light Level"
```

### Checksum Constants (IMPORTANT)

Each ProFlame 2 packet ends with two error-detection words computed from the
command words using four 4-bit constants (C1, D1, C2, D2). **These constants
are unique per remote** - with the wrong ones the fireplace silently ignores
every command, even if the serial number is correct.

Derive them from a single rtl_433 capture of your remote (`rtl_433 -f 315M -R 207 -F json`),
which reports `cmd1`, `err1`, `cmd2`, `err2`. With `h`/`l` the high/low nibble
of a command byte and `X`/`Y` the high/low nibble of its error byte:

```
C = X ^ ((h << 1) & 0xF) ^ h ^ ((l << 1) & 0xF)
D = Y ^ h ^ l
```

Apply that to the `cmd1`/`err1` pair to get `checksum_c1`/`checksum_d1`, and to
the `cmd2`/`err2` pair to get `checksum_c2`/`checksum_d2`. Example: remote
`0xAA9402` sent `cmd1=0x02, err1=0xBC` -> C1 = 0xB ^ 0 ^ 0 ^ 0x4 = 0xF,
D1 = 0xC ^ 0 ^ 0x2 = 0xE. Verify against a second capture with a different
command if you can.

### Full Configuration Example

See `proflame2_fireplace.yaml` for a complete example with all options.

## Getting Your Serial Number and Checksum Constants

Clone them from your existing remote with an RTL-SDR — full walkthrough in
**[docs/CAPTURE.md](docs/CAPTURE.md)**. Short version:

```bash
rtl_433 -f 315M -R 207 -F json          # press remote buttons, note id/cmd/err fields
printf '0x%X\n' <id>                    # id in hex = serial_number
python3 tools/verify_protocol.py derive --cmd1 .. --err1 .. --cmd2 .. --err2 ..
```

Pairing a random/fresh serial instead of cloning is **not recommended**: the
checksum constants for an uncaptured serial are unknown (their relationship to
the serial hasn't been reverse engineered), so the receiver would likely reject
every command. See the note at the end of CAPTURE.md.

## Pairing with Fireplace

1. **Enter pairing mode on the fireplace receiver**:
   - Press and hold the LEARN/PROG button on the IFC board (inside fireplace)
   - You should hear 3 beeps
   - The amber LED will illuminate

2. **Send pairing signal from ESP32**:
   - Toggle the power switch in Home Assistant
   - The receiver should beep 4 times indicating successful pairing

3. **Test the connection**:
   - Try turning the fireplace on/off
   - Adjust flame height
   - Test fan and light controls

## Home Assistant Integration

Once configured and running, the fireplace will appear in Home Assistant with:

### Entities Created
- `switch.fireplace_power` - Main on/off control
- `switch.fireplace_pilot_mode` - IPI/CPI mode selection
- `switch.fireplace_aux_power` - Auxiliary outlet control
- `number.fireplace_flame_height` - Flame height (0-6)
- `number.fireplace_fan_speed` - Fan speed (0-6)
- `number.fireplace_light_level` - Light brightness (0-6)

### Example Automations

#### Turn on at sunset:
```yaml
automation:
  - alias: "Fireplace Sunset"
    trigger:
      - platform: sun
        event: sunset
    action:
      - service: switch.turn_on
        entity_id: switch.fireplace_power
      - service: number.set_value
        entity_id: number.fireplace_flame_height
        data:
          value: 3
```

#### Temperature-based control:
```yaml
automation:
  - alias: "Fireplace Temperature Control"
    trigger:
      - platform: numeric_state
        entity_id: sensor.living_room_temperature
        below: 18
    action:
      - service: switch.turn_on
        entity_id: switch.fireplace_power
```

## Testing & Verification

See **[TESTING.md](TESTING.md)** for the full bring-up ladder: protocol
self-test (`tools/verify_protocol.py selftest`), CC1101 sanity checks, raw RF
verification, decoding your own transmissions with rtl_433, and watching for
the fireplace's RF echo (its acknowledgment of an accepted command).

## Troubleshooting

Work through [TESTING.md](TESTING.md) first — it isolates the failing layer.
Then use [docs/DEBUGGING.md](docs/DEBUGGING.md) for the symptom → cause
reference and how to capture evidence (logs, pulse analysis, raw samples).
Quick hits:

### Fireplace doesn't respond
1. **Check wiring** - SPI connections, and GDO0 to the `remote_transmitter` pin
2. **Verify serial number** - Must match paired remote or be freshly paired
3. **Check logs** - `esphome logs my_fireplace.yaml`
4. **Verify checksum constants** - wrong C/D constants are the #1 cause of "no response" (see Checksum Constants)

### Intermittent control
1. **Antenna** - Ensure CC1101 antenna is connected and positioned well
2. **Distance** - Move ESP32 closer to fireplace
3. **Interference** - Check for other 315MHz devices (garage doors, tire sensors)

### Can't compile
1. **ESPHome version** - Needs ESPHome's built-in `cc1101` component (tested on 2026.9.1)
2. **Board selection** - Verify ESP32 board type matches your hardware
3. **Dependencies** - `spi:`, `cc1101:` and `remote_transmitter:` must all be in your YAML

## Safety Considerations

⚠️ **IMPORTANT SAFETY NOTES**:
- This project controls a gas appliance - incorrect use could be dangerous
- Always maintain the original remote as a backup
- Test thoroughly before relying on automation
- Install CO detectors near the fireplace
- Follow local codes and regulations
- Consider adding timeout automations to prevent extended operation

## Protocol Details

Full specification with packet diagrams: [docs/PROTOCOL.md](docs/PROTOCOL.md).
Summary:
- **Frequency**: 314.973 MHz per the FCC filing (captured remotes measure ~315.07 MHz;
  OOK receivers are wide enough that either works - set it with `cc1101: frequency:`)
- **Modulation**: OOK (On-Off Keying)
- **Baud Rate**: 2400 baud for the *Manchester-encoded* on-air bits (~416 us/bit)
- **Encoding**: Thomas Manchester variant (0->01, 1->10, sync->11)
- **Packet**: 7 words x 13 bits = 91 bits, encoded to 182 bits; each burst sends
  5 identical packets separated by 12 zero bits (~395 ms total)
- **Word format**: sync, start guard (1), 8 data bits, padding bit (1 in word 1
  only), parity over data+padding, end guard (1)
- **Checksum**: Nibble-based XOR with **device-specific** constants (see
  "Checksum Constants" above)

### v2.0 protocol/driver fixes (July 2026)

Version 2.0 fixes several bugs that each prevented the fireplace from accepting
any command, all verified against SDR captures of a real remote:

- Packet words were missing the start guard bit and misplaced the padding bit
  (the corrected builder reproduces the smartfire reference bitstring exactly)
- Checksum constants are now configurable per device (they were hardcoded to
  another remote's values)
- Manchester encoder overflowed its output buffer (stack corruption)
- CC1101 data rate was 1200 baud instead of 2400 (MDMCFG4 0xF5 -> 0xF6)
- SFTX/SFRX strobes were swapped, so the TX FIFO was never flushed
- The OOK PA table momentarily set the "0" symbol to full carrier power
- Rapid state changes were silently dropped by the rate limiter (now queued)
- Removed `esphome.h` include (breaks modern ESPHome external components)

## Advanced Features

### Custom Commands

You can send custom commands by calling the component methods directly:

```cpp
id(my_fireplace)->set_flame_level(4);
id(my_fireplace)->set_fan_level(2);
```

Each call transmits the full state. To change several settings without first
sending a frame with only the first change applied, set them all at once:

```cpp
id(my_fireplace)->set_state(esphome::proflame2::ProFlame2Command{
    .pilot_cpi = false, .light_level = 0, .thermostat = false, .power = true,
    .front_flame = false, .fan_level = 2, .aux_power = false, .flame_level = 4});
```

The last commanded state is saved to flash and restored (display only, never
transmitted) after a reboot. The ESP can't see changes made with the physical
remote, so after using it Home Assistant shows what the ESP last sent.

### Receive Mode (Future)

The `cc1101` component already returns the radio to RX after each burst, so a
`remote_receiver` (on GDO2, or sharing GDO0 via `allow_other_uses`) could
add receive capability to:
- Detect remote control usage
- Sync state with physical remote
- Monitor fireplace status

## Contributing

Contributions are welcome! Please submit pull requests for:
- Additional fireplace model support
- Receive mode implementation
- Climate component integration
- Improved error handling

## Credits

- Original protocol reverse engineering: [johnellinwood/smartfire](https://github.com/johnellinwood/smartfire)
- ProFlame 2 protocol documentation: FCC ID T99058402300
- ESPHome CC1101 examples: [LSatan/SmartRC-CC1101-Driver-Lib](https://github.com/LSatan/SmartRC-CC1101-Driver-Lib)

## License

This project is licensed under the MIT License - see LICENSE file for details.

## Disclaimer

This project is not affiliated with, endorsed by, or connected to SIT Group, ProFlame, or any fireplace manufacturers. Use at your own risk. The authors assume no responsibility for damages or injuries resulting from the use of this software.

## Support

For issues, questions, or contributions:
1. Check the [Troubleshooting](#troubleshooting) section
2. Review existing GitHub issues
3. Create a new issue with:
   - ESPHome version
   - ESP32 board type
   - CC1101 module type
   - Complete logs
   - Wiring photos if applicable

## Changelog

### v2.1.0
- Radio handled by ESPHome's built-in `cc1101` component; bursts sent through
  `remote_transmitter` on GDO0 (RMT timing) instead of a custom FIFO driver
- `proflame2` no longer takes `cs_pin`, `gdo0_pin` or `frequency` - configure
  those on `cc1101:` (see Basic Configuration)
- Builds on the ESP-IDF framework (the old driver crashed at boot on IDF by
  releasing the SPI bus before acquiring it)
- Every command is transmitted, even if the value didn't change, so "off"
  always works after the physical remote was used
- Last commanded settings persist across reboots and are shown in Home
  Assistant. They are **not** transmitted at boot, so a reboot never
  re-lights the fireplace. Entity `restore_mode` options are ignored.
- New `set_state()` sends a multi-field change as a single frame

### v1.0.0 (2024-12-10)
- Initial release
- Basic transmit functionality
- Home Assistant integration
- Support for all ProFlame 2 controls

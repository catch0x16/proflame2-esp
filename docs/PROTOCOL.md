# ProFlame 2 RF Protocol

The SIT Group's ProFlame 2 control system is used in many modern natural gas and
propane fireplaces (Jotul, Mendota, Regency, Empire, Lennox, and others). A
handheld remote transmits OOK-modulated commands at ~315 MHz to a receiver in
the fireplace; the receiver echoes accepted commands back over RF.

This document is the protocol specification used by this component. The
original reverse engineering is from
[johnellinwood/smartfire](https://github.com/johnellinwood/smartfire) (see also
[FCC ID T99058402300](https://fcc.report/FCC-ID/T99058402300)); the
"Project Notes" section at the end records corrections and device-specific
findings from our own SDR captures.

## Implementation

### Specification
There is no publically available specification for the Proflame 2 protocol. Its usage is described most thoroughly in [FCC ID T99058402300](https://fcc.report/FCC-ID/T99058402300)

### RF Modulation and Encoding Protocols
The Proflame 2 system uses a proprietary packet structure and encoding. Commands are sent to the receiver in a single
burst for each command. The fireplace responds to a successful command by echoing it back exactly.

#### Modulation
The command bursts are transmitted at 314,973 KHz using the On-Off Keying (OOK) variant of Amplitude Shift Keying (ASK)
which is a type of Amplitude Modulation (AM), and are sent at a transmission rate of 2400 baud. Inside the command burst
are the command packets. The same command packet is transmitted 5 times in each command burst, and the packet
repetitions are separated by 12 low amplitude bits (zeros).

#### Encoding
The command packets are encoded with a variant of Thomas Manchester encoding. In this variant, 0 is represented by 01,
a 1 by 10, zero padding (Z) by 00, and synchronization words (S) as 11. The encoded command packet is 182 bits, and the
decoded packet is 91 bits. If each data part of the packet were manchester encoded separately, and then separated with
the bit pattern 11, then the encoding of individual command packets could be considered standard Thomas Manchester
encoding.

#### Packet Structure
The decoded packet is made up of 7 words, each 13 bits. The first 3 words are a unique identifier for the transmitter,
possibly related to the serial number. The next two words are command words controlling the state of the fireplace.
The last 2 words are error detection words, possibly used for security. Each word starts with a synchronization symbol,
followed by a 1 as a guard bit, then 8 bits of data, a padding bit, a parity bit, and finally a 1 as an end guard bit.
The padding bit is 1 for the first word and 0 for all other words. The parity bit is calculated over the data bits and
the padding bit, and is 0 if there are an even number of ones and 1 if there are an odd number of ones.

Packet Words:
* Serial 1
* Serial 2
* Serial 3
* Command 1
* Command 2
* Error Detection 1
* Error Detection 2 

Word Parts:
* 1 synchronization Symbol with a value of 'S' decoded or '11' raw
* 1 start guard bit with a value of '1'
* 8 bits of data
* 1 padding bit
* 1 parity bit calculated over the data part
* 1 end guard bit with a value of '1'

#### Serial Number
The serial number will need to either be cloned from an existing remote, or randomly generated and paired directly with
the fireplace. One valid serial number is the the data + padding portions of the following three words: '0b001001011', 
'0b011110100', '0b000000100'.

#### Command Words
The data portion of the first command word is made up of 1 bit for the pilot light, 3 big-endian bits for the light 
level, 2 zeros, 1 bit for the thermostat setting and 1 bit for the unit's main power. The second command word is made 
up of 1 bit for the front flame / flame split, 3 big-endian bits for the fan blower level, 1 bit for the auxiliary power
outlet, and 3 big-endian bits for the main flame level. For each single bit variable in the command, a 0 represents off 
and a 1 represents on. In the case of the pilot, that means 1 is CPI and 0 is IPI. For the thermostat, that means 1 is 
either on or the smart thermostat. The 3 bit numbers are the level between 0 and 6, where 0 is off and 6 is high. 7 is 
not an allowed value.

#### Error Detection Words
The first error detection word is calculated from the first command word, and the second error detection word is 
calculated from the second command word. For the purposes of calculating the error detection words, both the command 
words and the error detection words can be viewed as two 4-bit nibbles. Each 4-bit nibble of an error detection word
is calculated using a function based on the two 4-bit nibbles of its corresponding command word.  

They are calculated as follows:

Let:
* A represent the high nibble of the command word
* B represent the low nibble of the command word
* C and D represent constants
* X represent the high nibble of the error detection word
* Z represent the low nibble of the error detection word

Then:
* X = ( C ^ ( x << 1 ) ^  x  ^ ( y << 1 ) ) & 0xF
* Y = ( D ^ x ^ y )

For the first error detection word, C=0b1101 and D=0. The second error detection word is calculated the same way, but
using the second command word as its input, and with values of C=0 and D=0b0111. The constants C and D are possibly
related to the serial number or could otherwise be unique for each device.

#### Packet Diagram
```
        Bit
        1   2   3   4   5   6   7   8   9  10  11  12  13
Word |----------------------------------------------------|
   1 | S | 1 |      Serial Number             | 1 |Par| 1 | Serial Word 1
     |----------------------------------------------------|
   2 | S | 1 |      Serial Number             | 0 |Par| 1 | Serial Word 2
     |----------------------------------------------------|
   3 | S | 1 |      Serial Number             | 0 |Par| 1 | Serial Word 3
     |----------------------------------------------------|
   4 | S | 1 |CPI|    Light   | 0 | 0 |Th.|Pwr| 0 |Par| 1 | Command Word 1
     |----------------------------------------------------|
   5 | S | 1 |Fnt|     Fan    |Aux|   Flame   | 0 |Par| 1 | Command Word 2
     |----------------------------------------------------|
   6 | S | 1 |      Ecc       |      ECC      | 0 |Par| 1 | Error Detection Word 1
     |----------------------------------------------------|
   7 | S | 1 |      Ecc       |      ECC      | 0 |Par| 1 | Error Detection Word 2
     |----------------------------------------------------|
```

#### Packet Examples
Here is an example of a received packet and what command it represents.

AM Demodulated Bit String:
* 1110010110010110011010011011100110101010011001011010111001010101010110010110101110011001100101011001101011101010100101011001010110111001010101011001010110101110011010011001101001101

Decoded Command Packet:
* S100100101101S101111010011S100000010011S101010001011S111100010001S100000100011S101101011011

Parsed Values:
* Serial 1:  S1 0010 0101 1 0 1
* Serial 2:  S1 0111 1010 0 1 1
* Serial 3:  S1 0000 0010 0 1 1
* Command 1: S1 0101 0001 0 1 1
* Command 2: S1 1110 0010 0 0 1
* Error 1:   S1 0000 0100 0 1 1
* Error 2:   S1 0110 1011 0 1 1

Command Values:
* Pilot: Off / IPI
* Light: 5
* Thermostat: On
* Power: Off
* Front: On
* Fan: 6
* Aux: Off
* Flame: 2

## Examples

### Command Examples
* fire.set(power=False) # Turn off
* fire.set(power=True, flame=1, light=1, fan=0, front=False) # My favorite low mode
* fire.set(power=True, flame=6, light=6, fan=6, front=True) # Highest setting

---

## Project Notes (corrections from our SDR captures, 2026-07 audit)

The description above is the smartfire reference doc. Findings from our own SDR
captures of the paired remote (serial `0xAA9402`, capture files in the development repo):

### Error-detection constants are device specific
The C/D constants in the error-detection formula differ per remote. For **our**
remote they are `C1=0xF, D1=0xE, C2=0xE, D2=0x2` — verified against all 15
distinct command frames in the captures. (The smartfire reference device uses
`C1=0xD, D1=0x0, C2=0x0, D2=0x7`.) They are configurable in the ESPHome component
(`checksum_c1/d1/c2/d2`).

To derive them for any remote from a single rtl_433 capture (fields `cmd1`,
`err1`, etc.), with `h`/`l` the high/low nibble of the command byte and `X`/`Y`
the high/low nibble of the error byte:

    C = X ^ ((h<<1) & 0xF) ^ h ^ ((l<<1) & 0xF)
    D = Y ^ h ^ l

Note: the shifted nibbles are truncated to 4 bits (`& 0xF`), not rotated —
confirmed by the reference example (`cmd2=0xE2` where the high nibble overflows).

### Command word 1 bit order
`Thermostat` is bit 1 and `Power` is bit 0, as in the packet diagram. The
"Parsed Values" of the packet example above have them swapped; rtl_433 and our
captures (`cmd1=0x02` decodes as thermostat=1, power=0) confirm the diagram.

### Timing / modulation (from rtl_433 pulse analysis, dec 29 capture)
- The 2400 baud rate applies to the **Manchester-encoded** on-air bits: encoded
  bit ≈ 416 µs (pulses of ~396/804/1212 µs = 1/2/3 encoded bits).
- Whole burst = 395.6 ms ≈ 5 × 182 bits + 4 × 12 zero-bit separators at 2400 baud.
- Separator gap measured ≈ 5.2 ms ≈ 12 zero bits. Sync "11" + guard "1" produces
  the distinctive 1.2 ms pulse (35 per burst = 7 words × 5 packets).
- rtl_433 measured the remote's carrier at ~315.06–315.07 MHz (FCC filing says
  314.973 MHz; OOK receivers are wide enough that either works).

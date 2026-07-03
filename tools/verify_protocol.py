#!/usr/bin/env python3
"""ProFlame 2 protocol reference implementation and self-test.

This mirrors the C++ in components/proflame2/ exactly. Use it to:

  selftest  - verify the protocol math against known-good SDR captures
              (run this after ANY change to the packet/encoding code)
  derive    - compute your remote's checksum constants from an rtl_433 capture
  frame     - print the expected frame (and rtl_433 fields) for a given state

Examples:
  ./verify_protocol.py selftest
  ./verify_protocol.py derive --cmd1 0x02 --err1 0xBC --cmd2 0x26 --err2 0x46
  ./verify_protocol.py frame --serial 0xAA9402 --constants F,E,E,2 \
      --thermostat --fan 2 --flame 6
"""
import argparse
import sys


def checksum(cmd, c, d):
    h, l = (cmd >> 4) & 0xF, cmd & 0xF
    x = (c ^ ((h << 1) & 0xF) ^ h ^ ((l << 1) & 0xF)) & 0xF
    y = (d ^ h ^ l) & 0xF
    return (x << 4) | y


def derive_constants(cmd, err):
    """Inverse of checksum(): recover (C, D) from one observed cmd/err pair."""
    h, l = (cmd >> 4) & 0xF, cmd & 0xF
    x, y = (err >> 4) & 0xF, err & 0xF
    c = (x ^ ((h << 1) & 0xF) ^ h ^ ((l << 1) & 0xF)) & 0xF
    d = (y ^ h ^ l) & 0xF
    return c, d


def parity(data, pad):
    return (bin(data).count("1") + pad) & 1


def build_cmd_bytes(power=False, pilot_cpi=False, thermostat=False, light=0,
                    front=False, fan=0, aux=False, flame=0):
    cmd1 = (0x80 if pilot_cpi else 0) | ((light & 7) << 4) | \
           (0x02 if thermostat else 0) | (0x01 if power else 0)
    cmd2 = (0x80 if front else 0) | ((fan & 7) << 4) | \
           (0x08 if aux else 0) | (flame & 7)
    return cmd1, cmd2


def build_words(serial, cmd1, cmd2, c1, d1, c2, d2):
    data = [(serial >> 16) & 0xFF, (serial >> 8) & 0xFF, serial & 0xFF,
            cmd1, cmd2, checksum(cmd1, c1, d1), checksum(cmd2, c2, d2)]
    words = []
    for i, b in enumerate(data):
        pad = 1 if i == 0 else 0
        words.append(0x1000 | 0x800 | (b << 3) | (pad << 2) | (parity(b, pad) << 1) | 1)
    return data, words


def encode(words):
    """Manchester variant: sync->11, 1->10, 0->01. 91 bits -> 182 bits."""
    out = []
    for w in words:
        for b in range(12, -1, -1):
            if b == 12:
                out.append("11")
            else:
                out.append("10" if (w >> b) & 1 else "01")
    return "".join(out)


def cmd_selftest(_args):
    failures = 0

    def check(name, got, want):
        nonlocal failures
        ok = got == want
        failures += 0 if ok else 1
        print(f"  [{'PASS' if ok else 'FAIL'}] {name}" + ("" if ok else f": got {got!r}, want {want!r}"))

    print("Checksum function vs smartfire reference device (C=D,0 / 0,7):")
    check("cmd1 0x51 -> err1", checksum(0x51, 0xD, 0x0), 0x04)
    check("cmd2 0xE2 -> err2", checksum(0xE2, 0x0, 0x7), 0x6B)

    print("Checksum vs ALL captured frames from remote 0xAA9402 (C1=F D1=E C2=E D2=2):")
    pairs1 = [(0x00, 0xFE), (0x01, 0xDF), (0x02, 0xBC), (0x03, 0x9D)]
    pairs2 = [(0x21, 0xA1), (0x22, 0xC2), (0x23, 0xE3), (0x26, 0x46), (0x32, 0xF3),
              (0x42, 0x64), (0x52, 0x55), (0x53, 0x74), (0x54, 0x93), (0x55, 0xB2), (0x56, 0xD1)]
    for c, e in pairs1:
        check(f"cmd1 {c:#04x} -> err1 {e:#04x}", checksum(c, 0xF, 0xE), e)
    for c, e in pairs2:
        check(f"cmd2 {c:#04x} -> err2 {e:#04x}", checksum(c, 0xE, 0x2), e)

    print("Constant derivation round-trip:")
    check("derive from cmd1=0x02/err1=0xBC", derive_constants(0x02, 0xBC), (0xF, 0xE))
    check("derive from cmd2=0x56/err2=0xD1", derive_constants(0x56, 0xD1), (0xE, 0x2))

    print("Full encoded bitstring vs smartfire reference example:")
    # Reference doc example: serial words 0x25/0x7A/0x02, cmd1=0x51, cmd2=0xE2.
    # The doc's bitstring is 181 bits - it is missing the final '0' of the last
    # guard bit's Manchester pair; we compare its full length.
    ref = ("111001011001011001101001101110011010101001100101101011100101010101011001"
           "011010111001100110010101100110101110101010010101100101011011100101010101"
           "1001010110101110011010011001101001101")
    _, words = build_words(0x257A02, 0x51, 0xE2, 0xD, 0x0, 0x0, 0x7)
    bits = encode(words)
    check("bit length", len(bits), 182)
    check("matches reference (first 181 bits)", bits[:181], ref)

    print("Command byte layout vs rtl_433 decode of captured frame:")
    cmd1, cmd2 = build_cmd_bytes(thermostat=True, fan=2, flame=6)
    check("cmd1 (thermostat=1, power=0)", cmd1, 0x02)
    check("cmd2 (fan=2, flame=6)", cmd2, 0x26)

    print(f"\n{'ALL TESTS PASSED' if failures == 0 else f'{failures} TEST(S) FAILED'}")
    return 1 if failures else 0


def cmd_derive(args):
    results = []
    if args.cmd1 is not None and args.err1 is not None:
        c, d = derive_constants(args.cmd1, args.err1)
        results.append(("checksum_c1", c)); results.append(("checksum_d1", d))
    if args.cmd2 is not None and args.err2 is not None:
        c, d = derive_constants(args.cmd2, args.err2)
        results.append(("checksum_c2", c)); results.append(("checksum_d2", d))
    if not results:
        print("Provide --cmd1/--err1 and/or --cmd2/--err2 from an rtl_433 capture", file=sys.stderr)
        return 2
    print("Add to your proflame2: config block:")
    for k, v in results:
        print(f"  {k}: {v:#x}")
    print("\nVerify against a second capture with a DIFFERENT command if possible.")
    return 0


def cmd_frame(args):
    c1, d1, c2, d2 = (int(x, 16) for x in args.constants.split(","))
    cmd1, cmd2 = build_cmd_bytes(power=args.power, pilot_cpi=args.cpi,
                                 thermostat=args.thermostat, light=args.light,
                                 front=args.front, fan=args.fan, aux=args.aux,
                                 flame=args.flame)
    data, words = build_words(args.serial, cmd1, cmd2, c1, d1, c2, d2)
    bits = encode(words)
    print(f"Expected rtl_433 decode:  id={args.serial:06x} cmd1={cmd1:02x} cmd2={cmd2:02x} "
          f"err1={data[5]:02x} err2={data[6]:02x}")
    print(f"ESP log line to expect:   Frame: id={args.serial:06x} cmd1={cmd1:02x} "
          f"cmd2={cmd2:02x} err1={data[5]:02x} err2={data[6]:02x}")
    print(f"Encoded packet ({len(bits)} bits):\n{bits}")
    return 0


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)
    sub.add_parser("selftest").set_defaults(func=cmd_selftest)

    d = sub.add_parser("derive")
    for a in ("--cmd1", "--err1", "--cmd2", "--err2"):
        d.add_argument(a, type=lambda x: int(x, 0))
    d.set_defaults(func=cmd_derive)

    f = sub.add_parser("frame")
    f.add_argument("--serial", type=lambda x: int(x, 0), required=True)
    f.add_argument("--constants", default="D,0,0,7", help="C1,D1,C2,D2 hex nibbles (default: smartfire ref)")
    f.add_argument("--power", action="store_true")
    f.add_argument("--cpi", action="store_true")
    f.add_argument("--thermostat", action="store_true")
    f.add_argument("--front", action="store_true")
    f.add_argument("--aux", action="store_true")
    f.add_argument("--light", type=int, default=0)
    f.add_argument("--fan", type=int, default=0)
    f.add_argument("--flame", type=int, default=0)
    f.set_defaults(func=cmd_frame)

    args = p.parse_args()
    sys.exit(args.func(args))


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Live ProFlame 2 frame monitor for rtl_433 (Linux/macOS, stdlib only).

Wraps `rtl_433 -f 315M -R 207 -F json` (or reads a saved log) and turns the
raw JSON into a readable bring-up dashboard:

  - hex id/cmd/err fields (rtl_433 prints decimal) + decoded switch/level state
  - checksum validation against your remote's C/D constants
    (auto-derived from the first frame if --constants is not given)
  - echo detection: an identical frame arriving within --echo-window seconds
    is flagged ECHO - that is the fireplace receiver acknowledging a command
  - Ctrl-C summary: frames seen, serials, derived constants as a ready-to-paste
    YAML block

Examples:
  ./pf2_monitor.py                          # spawn rtl_433 live
  ./pf2_monitor.py -- -d 1 -g 40            # extra args passed to rtl_433
  ./pf2_monitor.py --file frames.json       # replay a saved capture/log
  rtl_433 -f 315M -R 207 -F json | ./pf2_monitor.py --stdin
"""
import argparse
import json
import re
import signal
import subprocess
import sys
import time
from datetime import datetime

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from verify_protocol import checksum, derive_constants  # noqa: E402

GREEN, RED, YELLOW, CYAN, DIM, RESET = "\033[32m", "\033[31m", "\033[33m", "\033[36m", "\033[2m", "\033[0m"


def decode_state(cmd1, cmd2):
    return {
        "pilot": "CPI" if cmd1 & 0x80 else "IPI",
        "light": (cmd1 >> 4) & 7,
        "thermostat": bool(cmd1 & 0x02),
        "power": bool(cmd1 & 0x01),
        "front": bool(cmd2 & 0x80),
        "fan": (cmd2 >> 4) & 7,
        "aux": bool(cmd2 & 0x08),
        "flame": cmd2 & 7,
    }


def parse_time(rec):
    t = rec.get("time")
    if t:
        for fmt in ("%Y-%m-%d %H:%M:%S", "%Y-%m-%dT%H:%M:%S"):
            try:
                return datetime.strptime(t.split(".")[0], fmt).timestamp()
            except ValueError:
                pass
    return time.time()


class Monitor:
    def __init__(self, constants, echo_window, color):
        self.constants = constants  # (c1, d1, c2, d2) or None until derived
        self.constants_source = "--constants" if constants else None
        self.echo_window = echo_window
        self.color = color
        self.frames = 0
        self.chk_ok = 0
        self.chk_fail = 0
        self.echoes = 0
        self.serials = set()
        self.last = None  # (timestamp, id, cmd1, cmd2)
        self.derived_consistent = True

    def c(self, code, s):
        return f"{code}{s}{RESET}" if self.color else s

    KV_RE = re.compile(r"^(?:\[[^\]]*\]\s*)?([A-Za-z][A-Za-z0-9 ]*?)\s*:\s*(.+?)\s*$")
    KV_FIELDS = ("id", "cmd1", "cmd2", "err1", "err2")

    def __post_init_kv(self):
        if not hasattr(self, "_kv"):
            self._kv = {}

    def handle_line(self, line):
        line = line.rstrip()
        start = line.find("{")
        if start >= 0:
            # JSON format: numeric fields are DECIMAL
            try:
                rec = json.loads(line[start:])
            except json.JSONDecodeError:
                return
            if not all(k in rec for k in ("cmd1", "cmd2", "err1", "err2")):
                return  # not a decoded ProFlame2 frame (e.g. raw-bits rows)
            self.show(parse_time(rec), int(rec.get("id", 0)),
                      *(int(rec[k]) for k in ("cmd1", "cmd2", "err1", "err2")))
            return

        # kv format (rtl_433 -F kv / default console output): fields are HEX
        self.__post_init_kv()
        m = self.KV_RE.match(line)
        if not m:
            return
        key, val = m.group(1).strip().lower(), m.group(2)
        if key == "model":
            self._kv = {}  # new record starts
            if "proflame" not in val.lower():
                self._kv = None
            return
        if self._kv is None:
            return
        if key == "time":
            self._kv["time"] = val
        elif key in self.KV_FIELDS:
            try:
                self._kv[key] = int(val, 16)
            except ValueError:
                return
        if all(k in self._kv for k in ("cmd1", "cmd2", "err1", "err2")):
            rec = self._kv
            self._kv = {}
            self.show(parse_time(rec), rec.get("id", 0),
                      rec["cmd1"], rec["cmd2"], rec["err1"], rec["err2"])

    def show(self, ts, rid, cmd1, cmd2, err1, err2):
        self.frames += 1
        self.serials.add(rid)

        # Derive constants from the first frame if none were provided
        if self.constants is None:
            c1, d1 = derive_constants(cmd1, err1)
            c2, d2 = derive_constants(cmd2, err2)
            self.constants = (c1, d1, c2, d2)
            self.constants_source = f"auto-derived from first frame (id={rid:06x})"

        c1, d1, c2, d2 = self.constants
        ok1 = checksum(cmd1, c1, d1) == err1
        ok2 = checksum(cmd2, c2, d2) == err2
        if ok1 and ok2:
            self.chk_ok += 1
            chk = self.c(GREEN, "checksum OK")
        else:
            self.chk_fail += 1
            self.derived_consistent = False
            want1, want2 = checksum(cmd1, c1, d1), checksum(cmd2, c2, d2)
            chk = self.c(RED, f"CHECKSUM MISMATCH (expected err1={want1:02x} err2={want2:02x})")

        # Echo detection: identical frame shortly after the previous one
        echo = ""
        if self.last and self.last[1:] == (rid, cmd1, cmd2) and 0 < ts - self.last[0] <= self.echo_window:
            self.echoes += 1
            echo = "  " + self.c(CYAN, "ECHO (receiver ACK)")
        self.last = (ts, rid, cmd1, cmd2)

        st = decode_state(cmd1, cmd2)
        tstr = datetime.fromtimestamp(ts).strftime("%H:%M:%S")
        onoff = lambda b: "ON " if b else "off"
        print(f"{self.c(DIM, tstr)}  id={rid:06x}  cmd1={cmd1:02x} cmd2={cmd2:02x} "
              f"err1={err1:02x} err2={err2:02x}  {chk}{echo}")
        print(f"          power={onoff(st['power'])} flame={st['flame']} fan={st['fan']} "
              f"light={st['light']} thermo={onoff(st['thermostat'])} aux={onoff(st['aux'])} "
              f"front={onoff(st['front'])} pilot={st['pilot']}")
        sys.stdout.flush()

    def summary(self):
        print("\n----- session summary -----")
        print(f"frames: {self.frames}   checksum ok: {self.chk_ok}   "
              f"mismatch: {self.chk_fail}   echoes: {self.echoes}")
        if self.serials:
            print("serials seen: " + ", ".join(f"0x{s:06X}" for s in sorted(self.serials)))
        if self.constants:
            c1, d1, c2, d2 = self.constants
            print(f"constants ({self.constants_source}):")
            print(f"  checksum_c1: 0x{c1:X}\n  checksum_d1: 0x{d1:X}"
                  f"\n  checksum_c2: 0x{c2:X}\n  checksum_d2: 0x{d2:X}")
            if self.frames > 1:
                verdict = ("consistent across all frames - safe to use"
                           if self.derived_consistent else
                           "NOT consistent - corrupted capture or mixed devices; recapture")
                print(f"  # {verdict}")
        if self.echoes:
            print("echoes observed: the fireplace receiver is acknowledging commands.")


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--file", help="parse a saved rtl_433 JSON log / capture file instead of running live")
    p.add_argument("--stdin", action="store_true", help="read rtl_433 JSON output from stdin")
    p.add_argument("--constants", help="C1,D1,C2,D2 hex nibbles (e.g. F,E,E,2); default: auto-derive")
    p.add_argument("--freq", default="315M", help="rtl_433 tune frequency (default 315M)")
    p.add_argument("--echo-window", type=float, default=2.0, help="seconds within which a duplicate counts as echo")
    p.add_argument("--no-color", action="store_true")
    p.add_argument("rtl433_args", nargs="*", help="extra args after -- are passed to rtl_433")
    args = p.parse_args()

    constants = tuple(int(x, 16) for x in args.constants.split(",")) if args.constants else None
    if constants and len(constants) != 4:
        p.error("--constants needs exactly 4 comma-separated hex nibbles")

    mon = Monitor(constants, args.echo_window, color=not args.no_color and sys.stdout.isatty())

    try:
        if args.file:
            with open(args.file, errors="replace") as f:
                for line in f:
                    mon.handle_line(line)
        elif args.stdin:
            for line in sys.stdin:
                mon.handle_line(line)
        else:
            cmd = ["rtl_433", "-f", args.freq, "-R", "207", "-F", "json"] + args.rtl433_args
            print(f"running: {' '.join(cmd)}  (Ctrl-C to stop)", file=sys.stderr)
            try:
                proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, text=True)
            except FileNotFoundError:
                sys.exit("rtl_433 not found - install it, or pipe it in with --stdin "
                         "(e.g. from docker: docker run ... hertzg/rtl_433 ... | pf2_monitor.py --stdin)")
            signal.signal(signal.SIGINT, lambda *_: proc.terminate())
            for line in proc.stdout:
                mon.handle_line(line)
    except KeyboardInterrupt:
        pass
    finally:
        mon.summary()


if __name__ == "__main__":
    main()

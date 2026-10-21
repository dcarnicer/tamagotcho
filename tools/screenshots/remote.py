# /// script
# requires-python = ">=3.11"
# dependencies = ["pyserial>=3.5", "Pillow>=10.3"]
# ///
"""Drive a device running a -shots firmware over USB and read its screen.

The StickS3 (sticks3-shots) or the Cardputer ADV (cardputer-adv-shots).

    uv run tools/screenshots/remote.py [options] STEP...
    uv run tools/screenshots/remote.py [options] -f scenario.txt [STEP...]

Steps (a scenario file holds the same words, whitespace-separated, `#` comments,
quotes for text with spaces):

    tap, tap-side          tap the front / side button  (also a, b)
    hold, hold-side        hold it until the hold fires (also A, B)
    key:KEY                a key of a keyboard (the Cardputer ADV): up, down,
                           left, right, enter, esc, menu (Tab), del
    key-hold:ARROW         hold an arrow for a second (its auto-repeat)
    STEP:N                 a press N times, e.g. tap:3 or key:down:3 (or tap*3,
                           quoted in zsh)
    shot[:NAME]            save the screen as NAME.png (shot-001.png, ...); with
                           --scale N also NAME@Nx.png
    wait:SECONDS           pause, printing the device log
    until:[SECONDS:]TEXT   wait for a log line containing TEXT (60 s default)
    wake                   light a dim or dark panel without a press
    date                   test hook: the boot-time date question
    fail-pin               test hook: the next Wi-Fi join's pinned step fails
    fail-join              test hook: the next Wi-Fi join fails outright

The firmware side is the TGC_REMOTE block in firmware/src/main.cpp; see
tools/screenshots/README.md. Never flash that build as a release.
"""

from __future__ import annotations

import argparse
import glob
import os
import queue
import re
import shlex
import threading
import time
from dataclasses import dataclass, field
from pathlib import Path

import serial
from PIL import Image

# One byte a command (firmware/src/main.cpp, TGC_REMOTE).
PRESSES = {
    "tap": b"a", "a": b"a", "tap-front": b"a",
    "hold": b"A", "A": b"A", "hold-front": b"A",
    "tap-side": b"b", "b": b"b",
    "hold-side": b"B", "B": b"B",
}
# Keys: 'k' and a letter (a board with a keyboard); upper case holds an arrow.
KEYS = {"up": b"u", "down": b"d", "left": b"l", "right": b"r",
        "enter": b"e", "esc": b"x", "menu": b"m", "del": b"b"}
PRESSES.update({f"key:{name}": b"k" + c for name, c in KEYS.items()})
HELD_KEYS = {f"key-hold:{name}": b"k" + c.upper()
             for name, c in KEYS.items() if name in ("up", "down", "left", "right")}
PRESSES.update(HELD_KEYS)
HOOKS = {"wake": b"w", "date": b"d", "fail-pin": b"f", "fail-join": b"x"}

ROW_RE = re.compile(r"#r (\d+) ([0-9a-f]+)$")
SHOT_RE = re.compile(r"#shot (\d+) (\d+)(?: (\w+))?$")
# Above this, a press is preceded by a wake: a panel that went dark (2 min
# without input on USB) would otherwise swallow it.
AUTO_WAKE_S = 60.0


def rgb565_table() -> list[bytes]:
    """RGB565 value -> 3 bytes of RGB888 (bit replication)."""
    out = []
    for v in range(65536):
        r, g, b = (v >> 11) & 31, (v >> 5) & 63, v & 31
        out.append(bytes(((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2))))
    return out


_TABLE: list[bytes] | None = None


def decode_rows(width: int, height: int, rows: dict[int, str]) -> Image.Image:
    """Rows of big-endian RGB565 as hex (four digits a pixel) -> an RGB image."""
    global _TABLE
    if _TABLE is None:
        _TABLE = rgb565_table()
    t = _TABLE
    buf = bytearray()
    for y in range(height):
        raw = bytes.fromhex(rows[y])
        buf += b"".join(t[(raw[i] << 8) | raw[i + 1]] for i in range(0, 2 * width, 2))
    return Image.frombytes("RGB", (width, height), bytes(buf))


@dataclass
class Shot:
    width: int
    height: int
    state: str  # the backlight: on, dim or dark (the picture is then the last frame)
    rows: dict[int, str] = field(default_factory=dict)
    logs: int = 0  # log lines that came in the middle of the dump
    damaged: int = 0  # row lines that came in cut or with something written into them

    def complete(self) -> bool:
        n = 4 * self.width
        return all(len(self.rows.get(y, "")) == n for y in range(self.height))


class Device:
    """The serial link: a reader thread splits the stream into log lines and shots."""

    def __init__(self, port: str, log_file: Path | None, quiet: bool):
        # A device path, or any pyserial URL (rfc2217://host:port, loop:// ...).
        s = serial.serial_for_url(port, do_not_open=True)
        s.baudrate = 115200
        s.timeout = 0.1
        # The ESP32-S3's USB Serial/JTAG resets the chip while RTS is up and DTR
        # down. The port opens with both up and pyserial then sets DTR before
        # RTS, so asking for both down resets it (seen on the StickS3). DTR up
        # and RTS down never passes through that state.
        s.dtr = True
        s.rts = False
        s.open()
        self.ser = s
        self.quiet = quiet
        self.log = open(log_file, "a", encoding="utf-8") if log_file else None  # noqa: SIM115
        self.lines: queue.Queue[str] = queue.Queue()
        self.shots: queue.Queue[Shot] = queue.Queue()
        self.headers = 0  # "#shot" lines seen
        self.abandon = threading.Event()  # shot() gave up on the dump in progress
        self.stop = False
        self.thread = threading.Thread(target=self._read, daemon=True)
        self.thread.start()

    def close(self) -> None:
        self.stop = True
        self.thread.join(timeout=1)
        self.ser.close()
        if self.log:
            self.log.close()

    def note(self, text: str) -> None:
        """A line of our own, on stdout and in the log file."""
        print(f"> {text}", flush=True)
        if self.log:
            self.log.write(f"{time.strftime('%H:%M:%S')} > {text}\n")
            self.log.flush()

    def _emit(self, line: str) -> None:
        if not line.strip():
            return
        if not self.quiet:
            print(f"| {line}", flush=True)
        if self.log:
            self.log.write(f"{time.strftime('%H:%M:%S')} | {line}\n")
            self.log.flush()
        self.lines.put(line)

    def _read(self) -> None:
        pending = b""
        shot: Shot | None = None
        while not self.stop:
            try:
                data = self.ser.read(4096)
            except serial.SerialException as e:
                self._emit(f"(serial error: {e})")
                return
            if not data:
                continue
            pending += data
            *done, pending = pending.split(b"\n")
            for raw in done:
                if self.abandon.is_set():
                    self.abandon.clear()
                    shot = None
                line = raw.decode("utf-8", "replace").rstrip("\r")
                # A log fragment printed without a newline may sit in front of a
                # shot line; it is split off and logged.
                cut = max(line.find("#shot "), line.find("#r "), line.find("#end"))
                if cut > 0:
                    self._emit(line[:cut])
                    line = line[cut:]
                m = SHOT_RE.match(line)
                if m:
                    shot = Shot(int(m[1]), int(m[2]), m[3] or "on")
                    self.headers += 1
                    continue
                if line.startswith("#r ") or line == "#end":
                    # Without a shot: the rest of a dump given up on, or whose
                    # header was lost. Dropped either way.
                    if shot is None:
                        continue
                    if line == "#end":
                        self.shots.put(shot)
                        shot = None
                        continue
                    m = ROW_RE.match(line)
                    if m:
                        shot.rows[int(m[1])] = m[2]
                    else:
                        shot.damaged += 1
                    continue
                if shot is not None:
                    shot.logs += 1
                self._emit(line)

    def send(self, b: bytes) -> None:
        self.ser.write(b)
        self.ser.flush()

    def drain_shots(self) -> None:
        while not self.shots.empty():
            self.shots.get_nowait()

    def shot(self, tries: int = 3) -> tuple[Image.Image, Shot]:
        """Asks for the back buffer until a complete one arrives.

        The USB link can lose bytes now and then (the device's serial driver drops
        what it cannot send in time), so a dump with a row missing or cut, or with
        its end lost, is asked for again.
        """
        headers = self.headers
        for attempt in range(1, tries + 1):
            self.drain_shots()
            self.send(b"s")
            try:
                # The first ask may wait behind a press still running on the device.
                s = self.shots.get(timeout=10.0 if attempt == 1 else 5.0)
            except queue.Empty:
                self.abandon.set()
                self.note(f"shot {attempt}/{tries}: no complete dump, again")
                continue
            if s.complete():
                return decode_rows(s.width, s.height, s.rows), s
            bad = sum(1 for y in range(s.height) if len(s.rows.get(y, "")) != 4 * s.width)
            self.note(f"shot {attempt}/{tries}: {bad} of {s.height} rows missing or damaged, again")
        if self.headers == headers:
            raise SystemExit(
                "no screen dump from the device: is a -shots firmware on it "
                "(cd firmware && pio run -e sticks3-shots -t upload, or cardputer-adv-shots)?"
            )
        raise SystemExit(f"no complete screen dump in {tries} tries")

    def wait_for(self, text: str, timeout: float) -> bool:
        end = time.monotonic() + timeout
        while True:
            left = end - time.monotonic()
            if left <= 0:
                return False
            try:
                line = self.lines.get(timeout=min(left, 0.5))
            except queue.Empty:
                continue
            if text in line:
                return True

    def clear_lines(self) -> None:
        while not self.lines.empty():
            self.lines.get_nowait()


def find_port() -> str:
    env = os.environ.get("TGC_PORT")
    if env:
        return env
    ports = sorted(glob.glob("/dev/cu.usbmodem*") + glob.glob("/dev/ttyACM*"))
    if not ports:
        raise SystemExit("no USB serial port found; pass --port")
    return ports[0]


def parse_steps(words: list[str]) -> list[str]:
    steps = []
    for w in words:
        m = re.fullmatch(r"(.+)[:*](\d+)", w)
        if m and m[1] in PRESSES:
            steps += [m[1]] * int(m[2])
        else:
            steps.append(w)
    for s in steps:
        head = s.split(":", 1)[0]
        if s not in PRESSES and s not in HOOKS and head not in ("shot", "wait", "until"):
            raise SystemExit(f"unknown step: {s!r}")
    return steps


def run(dev: Device, steps: list[str], args: argparse.Namespace, last_input: float) -> None:
    """Runs the steps; `last_input`: time.monotonic() of our last press or wake."""
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    count = 0
    for step in steps:
        head, _, rest = step.partition(":")
        # A panel that went dark would swallow a press (and hide a hook's screen).
        idle = time.monotonic() - last_input > AUTO_WAKE_S
        if idle and not args.no_wake and step != "wake" and (step in PRESSES or step in HOOKS):
            dev.send(b"w")
            last_input = time.monotonic()
            time.sleep(0.3)
        if step in PRESSES:
            dev.note(step)
            dev.clear_lines()  # until: matches what follows this step
            dev.send(PRESSES[step])
            last_input = time.monotonic()
            # The firmware takes the next byte once this press is over; the gap
            # leaves time for what the press started (a redraw, a list scroll).
            hold = PRESSES[step] in (b"A", b"B") or step in HELD_KEYS
            time.sleep(args.gap + (args.hold if hold else 0.1))
        elif step in HOOKS:
            dev.note(step)
            dev.clear_lines()
            dev.send(HOOKS[step])
            if step == "wake":
                last_input = time.monotonic()
            time.sleep(args.gap)
        elif head == "shot":
            count += 1
            name = rest or f"shot-{count:03d}"
            img, info = dev.shot()
            path = out / f"{name}.png"
            img.save(path)
            msg = f"shot {path} ({img.width}x{img.height}"
            msg += ")" if info.state == "on" else f", panel {info.state})"
            if info.logs:
                msg += f" [{info.logs} log lines during the dump]"
            if args.scale > 1:
                big = img.resize((img.width * args.scale, img.height * args.scale), Image.NEAREST)
                bpath = out / f"{name}@{args.scale}x.png"
                big.save(bpath)
                msg += f", {bpath}"
            dev.note(msg)
        elif head == "wait":
            dev.note(step)
            time.sleep(float(rest))
        elif head == "until":
            secs, _, text = rest.partition(":")
            try:
                timeout = float(secs)
            except ValueError:
                timeout, text = 60.0, rest
            dev.note(f"until {text!r} (at most {timeout:g} s)")
            # The next until: goes on from the line after this one.
            if not dev.wait_for(text, timeout):
                raise SystemExit(f"timed out waiting for {text!r}")


def main() -> None:
    p = argparse.ArgumentParser(
        description=__doc__.split("\n\n")[0],
        epilog="Steps: tap tap-side hold hold-side key:KEY key-hold:ARROW STEP:N shot[:NAME] "
        "wait:S until:[S:]TEXT wake date fail-pin fail-join. See tools/screenshots/README.md.",
    )
    p.add_argument("steps", nargs="*", help="steps to run, after those of --file")
    p.add_argument("-f", "--file", action="append", default=[], help="scenario file(s)")
    p.add_argument("-p", "--port",
                   help="serial port (default: $TGC_PORT, else the first /dev/cu.usbmodem*)")
    p.add_argument("-o", "--out", default=str(Path(__file__).resolve().parent / "out"),
                   help="where shots go (default: tools/screenshots/out/, git-ignored)")
    p.add_argument("--scale", type=int, default=1,
                   help="also save NAME@Nx.png, N times larger, nearest neighbour")
    p.add_argument("--log", type=Path,
                   help="append the serial log (and the steps) to this file")
    p.add_argument("--gap", type=float, default=0.4, help="seconds after each press (default 0.4)")
    p.add_argument("--hold", type=float, default=1.0, help="extra seconds after a hold (default 1.0)")
    p.add_argument("--settle", type=float, default=0.5,
                   help="seconds of log to read before the first step")
    p.add_argument("--tail", type=float, default=0.5,
                   help="seconds of log to read after the last step")
    p.add_argument("-q", "--quiet", action="store_true",
                   help="do not echo the device log (still saved with --log)")
    p.add_argument("--no-wake", action="store_true",
                   help="never send a wake (a press on a dark panel then only wakes it, "
                   "as on the device)")
    args = p.parse_args()

    words: list[str] = []
    for f in args.file:
        words += shlex.split(Path(f).read_text(encoding="utf-8"), comments=True)
    words += args.steps
    steps = parse_steps(words)

    port = args.port or find_port()
    try:
        dev = Device(port, args.log, args.quiet)
    except serial.SerialException as e:
        raise SystemExit(f"cannot open {port}: {e}")
    try:
        time.sleep(args.settle)
        last_input = 0.0
        if not args.no_wake and any(s in PRESSES for s in steps):
            dev.send(b"w")  # a dark panel would swallow the first press
            last_input = time.monotonic()
            time.sleep(0.3)
        run(dev, steps, args, last_input)
        time.sleep(args.tail)
    except KeyboardInterrupt:
        pass
    finally:
        dev.close()


if __name__ == "__main__":
    main()

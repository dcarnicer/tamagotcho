# Serial remote and screenshots

`remote.py` drives a StickS3 or a Cardputer ADV over its USB cable: it presses
the buttons (and the Cardputer's keys), reads the screen back as a PNG and
prints the serial log. It is for checking UI flows and taking the README
screenshots without anyone at the buttons, by hand or by an agent.

It needs a firmware with the serial remote, the **`sticks3-shots`** or
**`cardputer-adv-shots`** env (`sticks3` / `cardputer-adv` plus
`-DTGC_REMOTE=1`). **Never flash it as a release or leave it on someone else's
device**: anything on the USB cable can press its buttons. The release
workflow builds the normal envs only, which carry none of this code.

```sh
cd firmware && pio run -e sticks3-shots -t upload   # the remote build (app only)
cd firmware && pio run -e sticks3 -t upload         # back to the normal build
cd firmware && pio run -e cardputer-adv-shots -t upload   # the Cardputer ADV's
```

## Usage

A PEP 723 script; `uv run` fetches pyserial and Pillow.

```sh
uv run tools/screenshots/remote.py shot:now                       # the screen as it is
uv run tools/screenshots/remote.py hold-side shot:settings tap-side
uv run tools/screenshots/remote.py --scale 3 -f tools/screenshots/scenarios/tour.txt
uv run tools/screenshots/remote.py wait:30                        # just print the log for 30 s
```

Shots go to `tools/screenshots/out/` (git-ignored) unless `-o DIR` says
otherwise. The port is `--port`, else `$TGC_PORT`, else the first
`/dev/cu.usbmodem*` (or `/dev/ttyACM*`).

| option          | what                                                            |
|-----------------|-----------------------------------------------------------------|
| `-f FILE`       | a scenario file (repeatable); steps on the command line run after |
| `-o DIR`        | where the PNGs go                                               |
| `--scale N`     | also save `NAME@Nx.png`, N times larger, nearest neighbour (3 for docs) |
| `--log FILE`    | append the serial log and the steps, with times, to FILE        |
| `-q`            | do not echo the device log (it still goes to `--log`)           |
| `--gap S`       | seconds after each press (0.4); `--hold S` extra after a hold (1.0) |
| `--no-wake`     | never send a wake by itself (see below)                         |

### Steps

| step                    | does                                                    |
|-------------------------|---------------------------------------------------------|
| `tap`, `tap-side`       | tap the front / side button (also `a`, `b`)              |
| `hold`, `hold-side`     | hold it until the hold fires (also `A`, `B`)             |
| `key:KEY`               | a keyboard key (Cardputer ADV): `up`, `down`, `left`, `right`, `enter`, `esc`, `menu` (Tab), `del` |
| `key-hold:ARROW`        | hold an arrow for a second: its auto-repeat (`key-hold:down`) |
| `tap:3`                 | a press three times (`key:down:3`, `tap*3` too, quoted in zsh) |
| `shot[:NAME]`           | save the screen as `NAME.png` (`shot-001.png`, ... without a name) |
| `wait:S`                | pause S seconds, printing the log                        |
| `until:[S:]TEXT`        | wait until a log line contains TEXT (at most S s, 60 by default), counting the lines since the last press or hook; quote TEXT with spaces |
| `wake`                  | light a dim or dark panel without a press                |
| `date`, `fail-pin`, `fail-join` | the test hooks below                             |

A scenario file holds the same words, whitespace-separated, with `#` comments
and shell-style quotes. Device lines print as `| ...`, the tool's own as
`> ...`; a shot line says when the panel was dim or dark and how many log
lines came in during the dump.

## Protocol

One byte a command over the USB serial port (115200, though the USB
Serial/JTAG ignores it), read by `loop()` (`TGC_REMOTE` block in
`firmware/src/main.cpp`):

| byte  | does                                                                 |
|-------|----------------------------------------------------------------------|
| `a`/`b` | tap the front / side button: down for 80 ms                        |
| `A`/`B` | hold it: down until the hold fires (the Hold time setting), then up |
| `k` + letter | a key, down for 80 ms: `u` `d` `l` `r` the arrows, `e` Enter, `x` Esc, `m` the menu key (Tab), `b` del; `U` `D` `L` `R` hold an arrow for 1 s |
| `s`   | dump the back buffer (below)                                         |
| `w`   | wake: counts as activity and lights a dim or dark panel, no press    |
| `d`   | test hook: the boot-time date question                               |
| `f`   | test hook: the next home Wi-Fi join's pinned step fails              |
| `x`   | test hook: the next home Wi-Fi join fails                            |

A press is a virtual button that goes through the same tap/hold detection as
the real ones: it counts as activity for the dim and sleep timers, a dim panel
brightens and the press acts, and on a dark panel (2 min idle on USB) it only
wakes it, as on the device. That is why the tool sends `w` before the first
press and before any press that follows a minute without input; `--no-wake`
turns that off. While a virtual button is down the firmware leaves the next
bytes waiting, so a burst runs in order, one press at a time, and a hold cannot
turn into a tap when a loop turn is slow.

A key goes through the same path as the keyboard's (`keyTurn()` in
`main.cpp`): the same action per screen, the same wake rules (a press on a dark
panel only wakes it), the same auto-repeat on the lists and the date stepper.
An unknown key letter is ignored with a `remote: no key` line. On the
Cardputer ADV `tap`/`hold` press its G0 button; `tap-side`/`hold-side` still
press a virtual side button, which that board does not have, so a keyboard
flow should use the keys.

The dump is what the panel shows, in the UI's orientation (135x240 portrait,
240x135 landscape whichever way it is turned; the Cardputer ADV is always
240x135):

```
#shot W H on|dim|dark
#r 0 <W x 4 hex digits: RGB565, big-endian, as stored>
...
#r H-1 ...
#end
```

With the panel dark it is the last frame drawn. Log lines from other tasks
(the Wi-Fi events) can land between rows; they are printed as usual. The USB
link also drops bytes now and then (seen once in ~60 shots, and one log line
once): a dump with a row missing or cut, or whose `#end` never comes, makes the
tool ask again, three times at most. For the same reason an `until:` on one
log line can, rarely, time out; a shot is the surer check.

The tool opens the port with DTR up and RTS down. The ESP32-S3's USB
Serial/JTAG resets the chip while RTS is up and DTR down: the port opens with
both up and pyserial sets DTR before RTS, so an open that asks for both down
resets the StickS3 every time (seen). A plain `serial.Serial(port)` (both up)
did not.

## Test hooks

Each one is a byte, compiled only into the `-shots` envs.

- **`date`** opens the boot-time date question exactly as after a power cut:
  with a home Wi-Fi and a time zone, "Getting the date via <ssid>..." first,
  then the stepper, with "No network: set it by hand" (or "No network time")
  when that failed; a press while it joins skips to the stepper. It writes
  nothing itself: a **side tap cancels** (back to the quiz, nothing saved), a
  hold confirms the date shown (saved, as at boot), and a network time that
  answers is applied as by every sync (a same-day one writes nothing). It is
  refused while the portal, Get decks or the network test has the radio.
- **`fail-pin`**: the next home Wi-Fi join (date question, Get decks, network
  test, portal) pins its join to `02:00:00:00:7E:57`, an access point that does
  not exist, so the pinned step fails and the "any access point" step runs after
  25 s (docs/decisions.md, 2026-10-19). Log: `[net] test: pinned to ...`, then
  `did not take us in 25 s, any access point now`, then `[net] connected to`.
- **`fail-join`**: the same for the whole join, which fails after 40 s
  (`[net] could not join "<ssid>" (reason 201)`); on the date question that is
  the "No network: set it by hand" stepper.

```sh
# The power-cut question when the network fails, then cancel it.
uv run tools/screenshots/remote.py fail-join date wait:3 shot:date-getting \
    until:70:"asking by hand" shot:date-no-network tap-side
# The any-access-point fallback (needs the home Wi-Fi in reach).
uv run tools/screenshots/remote.py fail-pin date until:40:"did not take us" until:30:"clock:"
```

## Keyboard flows (Cardputer ADV)

```sh
# The settings home by the menu key, two rows down, into the page, back twice.
uv run tools/screenshots/remote.py key:menu shot:home key:down:2 shot:home-2 \
    key:enter shot:page key:esc key:esc shot:quiz
# A list's focus up (it wraps to the last row), then a held arrow repeating.
uv run tools/screenshots/remote.py key:menu key:up shot:wrapped key-hold:down \
    shot:repeated key:esc
# The quiz's options without answering: down, up, left, right.
uv run tools/screenshots/remote.py key:down shot:q1 key:up key:left shot:q2 key:right
```

## On a device with real progress

The buttons do what they do: a **hold on the quiz answers** the card (graded and
saved), a tap on the done screen is "study more", a hold on a deck loads it, a
hold on a settings choice applies it, the Device page's **Sleep** row sleeps, a
hold on the date stepper sets the date. `key:enter` is the hold: it answers,
loads, applies and sets just the same. Opening and leaving screens, taps on the
quiz (moving the highlight) and cancelling are safe; `scenarios/tour.txt` sticks
to those. Take a full flash backup before flashing a device whose progress
matters.

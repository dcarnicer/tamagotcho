# Phase 2 – SRS

Spaced-repetition scheduling and persisted review state for the on-device quiz.
This document is the design; the implementation lives in `firmware/src/srs/`
(pure C++, no Arduino, unit-tested natively) plus a small LittleFS layer in
`firmware/src/platform/` and the wiring in `firmware/src/main.cpp`.

## 1. Goals and non-goals

**Goals**

- One memory state per *kanji*, so the scheduler can say what is due and how
  well a card is known. State is keyed by Unicode codepoint (see §2).
- Grade each answer from correctness and response time (`docs/decisions.md`):
  Again on a miss, Hard on a slow correct, Good on a fast correct.
- Survive power loss without losing more than a few answers, and without
  wearing the flash out.
- Work with **no RTC and no network** (§5).
- Keep it deterministic and host-testable: the same input sequence must
  produce the same schedule in a native test and on the device.

**Non-goals (phase 2)**

- No on-device FSRS **parameter optimisation** (no training data, no math
  library budget, no reason to write an optimiser). We hardcode defaults.
- No review-log export, no sync (phase 4), no Wi-Fi.
- No per-question-type state and no per-language state: one state per
  codepoint, the mode is chosen at ask time (§6).
- No stats screen. The cross-cutting "Statistics" item on the roadmap can read
  the same file later; phase 2 only keeps the counters it needs.
- No RTC and no required network: the day clock in §5 derives the date. A real
  wall-clock time (the phone's browser through the Wi-Fi portal, or NTP over the
  home Wi-Fi) is an optional correction on top of it (§5.1).

## 2. State model

**Key: the card's Unicode codepoint.** A `Card` is a kanji; the pack, the deck
name and the meaning language are properties of the *material*, not of the
learner. Knowing 日 does not reset when you switch from `n5-en.tgc` to
`n4-en.tgc` or to `n5-es.tgc`, so the state table is global and shared across
packs. Consequences, all intended:

- Switching decks keeps progress; cards you already know arrive as reviews,
  not as new.
- The deck menu can show `N due / M new` per pack by joining the pack's card
  table against this table.
- The file grows with the number of *distinct kanji seen*, not with the number
  of packs. All jouyou kanji is ~2 136; cap at 8192 records and ignore new
  cards past the cap (log it). At 16 B/record that is 128 KB worst case.

Caveat, accepted: `n5-en` and `n5-es` share state even though the glosses
differ. Reading the meaning "day" in English and "día" in Spanish are not the
same skill, but they track closely enough; if that turns out wrong the header
has a `namespace`-free layout that can be extended (key = codepoint + lang
byte) without a format break, because the record has reserved bytes.

### Per-card record (16 bytes, in memory and on disk)

| off | type | field          | notes                                                        |
|-----|------|----------------|--------------------------------------------------------------|
| 0   | u32  | codepoint      | identity, sorted ascending (binary search)                   |
| 4   | u16  | stability_cds  | stability S in **centidays** (1/100 day), 1 .. 65535         |
| 6   | u16  | due_day        | day index this card is next due (0 = new, never reviewed)    |
| 8   | u16  | last_day       | day index of the last review (0 = never)                     |
| 10  | u8   | difficulty10   | difficulty D × 10, 10 .. 100                                 |
| 11  | u8   | reps           | saturating review count (255 max)                            |
| 12  | u8   | lapses         | saturating Again count                                       |
| 13  | u8   | mode_seen      | bit0 meaning, bit1 reading, bit2 meaning→kanji               |
| 14  | u8   | last_grade     | 0 Again, 1 Hard, 2 Good, 0xFF never                          |
| 15  | u8   | reserved       | 0                                                            |

S in centidays: resolution 14.4 min, which is far below the day resolution of
intervals, and enough range for the 365-day interval cap (§3). A record with
`due_day == 0` is a new card.

In memory: `std::vector<Record>` kept sorted by codepoint, held in PSRAM on
device (`psramAllocator`-style) — ~40 KB for a 2 500-card collection. The file
is a straight dump of that vector, so load and save are both O(n) with no
`std::map` overhead and no heap churn.

## 3. Scheduling algorithm

### Choice: FSRS-5, fixed default weights, three grades

We implement the **FSRS-5 memory model** (stability S, difficulty D, power
forgetting curve) with the **published default weights**, and skip everything
that needs data we cannot produce on device:

- no optimiser, no per-user weights;
- no `Easy` grade (`docs/decisions.md` fixes Again/Hard/Good from an MC quiz);
- day granularity, integer intervals, one interval cap.

Justification, against the alternatives:

- **A faithful FSRS-6** (21 params, learnable decay, same-day model) is not
  worth it: the extra accuracy comes from per-user optimisation, which we
  cannot do, and from a review history we do not collect.
- **A simpler SM-2/Leitner scheduler** would save ~40 lines but is strictly a
  worse fit: it cannot express the difference between a lapse on a mature card
  and on a new one, has no forgetting curve, and would contradict the
  architecture doc (`SRS (FSRS)`). The FSRS core is only `expf`/`sqrtf` plus
  two `powf` calls per answer — microseconds on the S3.
- **FSRS-5 over FSRS-4.5** because its difficulty mean-reversion is more
  stable when only three grades are ever fed in, and its same-day formula gives
  a principled in-session relearn step rather than a hand-rolled one.

The honest caveat: our signal is *multiple choice*, so a "Good" is weaker
evidence than an Anki "Good". FSRS is fairly robust to that (it is a model of
S and D, not of the UI), but the default retention of 0.9 may be optimistic;
see §9.

### Grade mapping

| observed                              | grade    | G |
|---------------------------------------|----------|---|
| wrong option                          | Again    | 1 |
| correct, response time ≥ `slow_ms`    | Hard     | 2 |
| correct, response time < `slow_ms`    | Good     | 3 |

`slow_ms` default 3000, stored in the state header so it can be tuned (or set
to 0xFFFF to disable Hard) without a firmware rebuild. Response time is
question-drawn → the **press-down edge of the confirming hold**, minus a fixed
**400 ms per navigation tap** (`kNavAllowanceMs` in `main.cpp`). This excludes
the mandatory 600 ms hold and the mechanical cost of tapping through to the right
option, but keeps the thinking time before and between taps (the earlier rule
reset the clock on each tap, so a long think followed by quick taps graded Good).
The UI geometry still leaks into the number; see §9.

### Formulas (FSRS-5, `w[0..18]` hardcoded)

All in `float` (the S3 has a single-precision FPU; `double` is software and
slow). Grade `G` ∈ {1,2,3}; `t` = `last_day`..`today` elapsed days.

First review (`reps == 0`):

```
S = w[G-1]
D = clamp(w[4] - expf(w[5] * (G - 1)) + 1, 1, 10)
```

Subsequent review, `R = (1 + F * t / S) ^ (-0.5) = 1 / sqrtf(1 + F*t/S)`:

```
if t == 0:            # same day (in-session relearn)
    S' = S * expf(w[17] * (G - 3 + w[18]))
elif G == 1:          # lapse
    S' = w[11] * powf(D, -w[12]) * (powf(S + 1, w[13]) - 1) * expf((1 - R) * w[14])
    S' = min(S', S)                    # a miss never increases stability
else:                 # Hard / Good
    hard = (G == 2) ? w[15] : 1
    S' = S * (1 + expf(w[8]) * (11 - D) * powf(S, -w[9])
                 * (expf((1 - R) * w[10]) - 1) * hard)

deltaD = -w[6] * (G - 3)
D' = w[7] * D0_4 + (1 - w[7]) * clamp(D + deltaD * (10 - D) / 9, 1, 10)   # D0_4 = init D for Easy
S  = clamp(S', 0.01, 500)              # kSMax = 500 d, see below
D  = clamp(D', 1, 10)
```

`kSMax = 500` days is a deliberate shrink of FSRS's 36500: intervals are capped
at 365 days anyway, and 500 fits the u16 centiday field. The deviation is
invisible at our interval range.

**Interval.** With desired retention `r = 0.9`:

```
I = roundf( S / F * (powf(r, 1/DECAY) - 1) )
```

and because `F = 19/81` and `r^(-2) - 1 = 1/0.81 - 1 = 19/81`, the two factors
cancel exactly: **`I = roundf(S)`**. No `powf` needed for the interval, and
the "S is in days" reading stays true. Clamp `I` to `[1, 365]`, then apply
**fuzz**: for `I ≥ 2`, `I = max(1, roundf(I * u))` with `u` uniform in
[0.95, 1.05], drawn from the injected `XorShiftRng`. Fuzz matters because every
new card starts at the same `S = w[2]` and would otherwise come due in one
lump.

`F = 19/81 = 0.2345679`, `DECAY = -0.5`. Desired retention 0.9 (see §9 for
raising it).

### Hardcoded parameters

```
desired_retention  = 0.9
max_interval_days  = 365
S_MIN              = 0.01      S_MAX = 500
new_per_day        = 10        session_cap = 200
slow_ms            = 3000
w = [ 0.40255, 1.18385, 3.173, 15.69105, 7.1949, 0.5345, 1.4604, 0.0046,
      1.54575, 0.1192, 1.01925, 1.9395, 0.11, 0.29605, 2.2698, 0.2315,
      2.9898, 0.51655, 0.6621 ]        // FSRS-5 defaults (19 params)
```

`w[3]` (Easy initial stability) and `w[16]` (Easy bonus) are unused but kept
in the array so the parameter set stays recognisable and a future Easy/unlock
grade can be added without a format change.

Sanity values: first Good → `S = 3.17` d, due in ~3 d; first Again → `S = 0.40`
d, re-asked this session and due tomorrow; `D0(Good) = 5.28`.

## 4. Persistence

### File

- Path: **`/state/srs.bin`** (LittleFS). Directory created on first use.
- Temp file: `/state/srs.tmp`; previous good file kept as `/state/srs.bak`.

LittleFS is *not* preserved by `pio run -t uploadfs` (it rebuilds the whole
image from `data/`), unlike NVS. That is the price of a 40 KB blob: the NVS
partition is too small. Document it next to the existing "keep every pack in
`data/packs/`" advice: `uploadfs` also wipes review progress.

### Byte layout (version 1)

```
+------------------------------+ 0
| Header (48 bytes)            |
+------------------------------+ 48
| Records: count * 16 bytes    |  sorted by codepoint ascending
+------------------------------+
| CRC-32 (4 bytes, LE)         |  covers header + records
+------------------------------+
```

Header (little-endian):

| off | type   | field                 | notes                                        |
|-----|--------|-----------------------|----------------------------------------------|
| 0   | char[4]| magic                 | `"TNGS"`                                     |
| 4   | u16    | version               | 1                                            |
| 6   | u16    | header_size           | 48                                           |
| 8   | u16    | now_day               | derived day index (§5)                        |
| 10  | u16    | count                 | record count                                 |
| 12  | u32    | ms_carry              | ms accumulated toward the next day index      |
| 16  | u32    | answered_total        | lifetime answers (stats, and "session happened") |
| 20  | u16    | slow_ms               | Hard threshold; 0xFFFF disables Hard          |
| 22  | u16    | new_per_day           | daily new-card cap                            |
| 24  | u16    | new_introduced_day    | day the counter below belongs to              |
| 26  | u16    | new_introduced_count  | new cards introduced on that day              |
| 28  | u16    | max_interval_days     | 365                                           |
| 30  | u16    | flags                 | bit0 = clean shutdown (unused); bit1 = has studied (streak §4.1) |
| 32  | u16    | streak_current        | consecutive SRS days with an answer (§4.1)    |
| 34  | u16    | streak_best           | best day streak ever (§4.1)                   |
| 36  | u16    | last_study_day        | SRS day of the last answer; 0 = never         |
| 38  | u16    | epoch_anchor          | days since 1970-01-01 of SRS day 0; 0 = never synced (§4.1, §5.1) |
| 40  | u32    | review_ms             | lifetime time on questions, ms (saturates)    |
| 44  | u32    | (unused)              | 0                                             |

#### 4.1 Header extension in the reserved bytes (v1-compatible)

Bytes 32..47 were `reserved[4] = 0` in the first v1 writer and are not
validated, so the streak, time and wall-clock fields reuse them **without a
version bump**: `0` keeps meaning "absent", so a file written by the older
firmware loads with zeroed streak/time and no anchor, and an older reader
ignores the new fields while still verifying the CRC (which covers the whole
header, new bytes included). Bytes 44..47 are still unused (0).

- **Day streak.** A day streak over the SRS day index (§5), not wall time. In
  `grade(codepoint, grade, day, ...)`: when `day != last_study_day` (or nothing
  has ever been studied), `streak_current` becomes `streak_current + 1` if a
  study day already exists and `day == last_study_day + 1`, otherwise `1`; then
  `last_study_day = day` and `streak_best = max(streak_best, streak_current)`. A
  same-day answer changes nothing. Both counters saturate at `0xFFFF`.
  **First-vs-consecutive is decided by a persisted "has studied" flag** (header
  `flags` bit1), not by `last_study_day == 0`: day index `0` is a valid study
  day, so the first answer a fresh store sees — even on day `0` — starts the
  streak at `1`. `last_study_day == 0` alone would be ambiguous with "never". A
  file written before the flag existed recovers it from a non-zero
  `last_study_day` on load.
- **Review time.** `addReviewMs(ms)` adds to `review_ms` and saturates at
  `0xFFFFFFFF`. The app calls it once per answer with the same response time
  that graded it (question shown → the confirming press-down, minus the
  per-tap allowance), so it is a pure UI measure, not a clock.
- **Wall-clock anchor.** `epoch_anchor` (u16 at 38) is the calendar day, counted
  in days since 1970-01-01, that SRS day `0` corresponds to; the date shown for
  any SRS day `d` is `epoch_anchor + d`. It is written by the first real-time
  sync and moved only by a sync (§5.1). `0` means "never synced": the display
  then falls back to the fixed base of the first firmware (SRS day 0 =
  2026-10-17, epoch day 20743), exactly as before. Epoch day 0 (1970-01-01) can
  never be a real anchor, because syncs before 2026 are refused, so `0` is
  unambiguous; a u16 covers dates up to 2149 and syncs after 2099 are refused.

CRC-32 (poly `0xEDB88320`, init/final `0xFFFFFFFF`). On load: size ≥ 48, magic,
`version == 1`, `header_size == 48`, `count` consistent with file size, CRC
ok. Anything else → try `/state/srs.bak`; if that fails too, start from empty
state and log. An unknown *newer* version is backed up to `.bak` and not
overwritten until the user's first answer, so a downgrade cannot silently
destroy a newer file.

The scheduler knobs live in the header so a future settings screen (or a
`pio run` patch) can change them without touching the record format.

### Atomicity

Write path is **temp + rename**, the same strategy the design brief suggests:

1. Serialise to a buffer (header + records + CRC).
2. `LittleFS.open("/state/srs.tmp", "w")`, write, `flush()`, `close()`.
3. `LittleFS.rename("/state/srs.bin", "/state/srs.bak")` (ignore failure).
4. `LittleFS.rename("/state/srs.tmp", "/state/srs.bin")`.

LittleFS rename commits atomically within a directory entry, so a power loss
leaves either the old, the new, or (worst case, between steps 3 and 4) only the
`.bak` — all of which load. Two renames per flush, both cheap metadata ops.

### Flush policy

Full-file rewrite, batched:

- mark **dirty** on every answer;
- flush when **8 answers** have accumulated, **or** 120 s have passed since the
  last answer, **or** the pack changes, **or** the deck menu is entered, **or**
  battery drops below 10 %/charging starts (`board::isCharging` flipped),
  whichever comes first.

Cost: ~40 KB per flush for a full collection. A 100-answer session ends in
~12 flushes ≈ 0.5 MB written per session; at LittleFS wear-levelling a
multi-GB endurance budget that is thousands of sessions. Per-answer flushing
would be ~50× that and add a visible pause; losing ≤ 8 answers (one or two
questions) on a battery pull is the accepted trade-off. The `answered_total`
counter is only advanced in the file, so the loss is also visible.

## 5. Time handling without an RTC

There is no RTC chip and no network, so "what day is it" has to be derived. We
use a **persisted, day-granular clock**:

- `now_day` (u16) is a monotonic day index. The schedule only uses
  differences; due days live in this index space and it never goes back. The
  date shown to the user is `epoch_anchor + now_day` once a real time has been
  synced (§5.1), else the fixed base 2026-10-17 + `now_day` (§4.1).
- **Uptime rule** (`tick(now_ms)` from `loop()`): accumulate elapsed
  `millis()` into `ms_carry`; every 86 400 000 ms, `now_day += 1`, carry the
  remainder. `ms_carry` is persisted, so short sessions add up. After a sync,
  `ms_carry` is the time since the last 04:00 local, so this rollover falls at
  04:00 local (§5.1) instead of 24 h after an arbitrary origin.
- **Sleep rule** (`addElapsedMs(ms)` at boot): the app deep-sleeps when idle
  (docs/decisions.md, 2026-10-18). The ESP32's RTC timer keeps counting through
  deep sleep, so on a wake-up the app adds the slept time (RTC now minus the
  timestamp it left in RTC memory before sleeping) to the same carry. The RTC
  runs from the internal RC oscillator (a few % drift), which is irrelevant at
  day granularity.
- **Power-cut rule** (app, not the store): a full power-off clears the RTC timer
  and RTC memory, so the elapsed time is unknown. The app detects this boot
  (`RTC_NOINIT` magic) and **asks the user** on the date screen: it shows the
  last-used date (the persisted `now_day` at boot) and a proposed "today" that
  starts there; a tap adds a day, a side-button hold takes one back (never below
  the current day), a hold confirms and a side tap skips the question. The
  confirmed difference `n` goes through `advanceDaysManual(n)`, which is
  `advanceDays(n)` plus, for `n > 0`, `ms_carry = 0`: the time of day is unknown
  after a power cut, so the next rollover comes a full day after the answer.
  That is late rather than early on purpose: an early rollover would hand out a
  day that cannot be taken back (the index never rewinds), a late one is fixed
  by the next answer or sync. The same screen is in the settings for manual
  corrections. A real-time sync (§5.1) answers the question instead, so it is
  not asked when the time is already known and it is dismissed if a sync
  arrives while it is shown. With a home Wi-Fi and a UTC offset saved, the
  screen tries one itself: it asks NTP over the home Wi-Fi in the background
  while the stepper stays usable (§5.1).
  On the StickS3 a click of the power
  button used to be such a cut: the M5PM1's "reset" cycles the chip's power. The
  click now saves and deep-sleeps instead, and so does a critical battery
  (docs/decisions.md, 2026-10-18). What is left is the deliberate double-click
  power-off, a long press into download mode, and a cell left to go flat.
  If the device goes to sleep (idle timeout, power button, Sleep row) while the
  boot-time question is still open, a flag in RTC memory (`gDatePending`)
  makes the next wake ask it again instead of dropping it.
- `beginSession()` only resets the tick base and the new-card counter on a day
  change; it never advances the day by itself. (An earlier "boot rule" advanced
  it on every boot that followed a session with answers; it inflated the
  schedule with every restart and was removed.)

**Trade-off (explicit).** With deep sleep the day index follows wall time
except across real power-offs, where it is only as good as the user's answer or
the next sync. FSRS recomputes S from the *observed* elapsed time every review,
so an answer that is off by a day changes S by a factor, not by an order of
magnitude.

Testability: the pure module never calls `millis()` or the RTC; `beginSession()`,
`tick(now_ms)`, `addElapsedMs(ms)`, `advanceDays(n)`, `advanceDaysManual(n)`,
`syncWallClock(utc_ms, tz_offset_min)` and a test-only `setToday()` are all the
clock needs, so the native tests drive time explicitly.

### 5.1 Real time from the phone and from NTP

Opening the Wi-Fi portal page (`http://192.168.4.1`) makes the browser post its
clock: `POST /time` with `ms` = `Date.now()` (UTC milliseconds), `tz` = minutes
**east** of UTC (`-new Date().getTimezoneOffset()`, so +120 for CEST) and an
optional IANA `zone` name used only in the portal's message. The portal checks
the syntax and hands the two numbers to the app (`WifiPortal::Hooks::onTime`);
the app's `applyWallClock(utc_ms, tz_offset_min)` calls the pure
`SrsStore::syncWallClock()` and is the single entry point for both sources (NTP
below calls it too). The offset is stored in NVS (`tangocho`/`tz`, int16, minutes
east of UTC; absent until the first sync) so NTP, which only yields UTC, can
reuse it. It is not part of the SRS file.

**Plausibility.** A time before 2026-01-01T00:00Z or from 2100 on, or an offset
outside -12 h..+14 h, is a broken browser clock (or a forged request) and is
rejected; nothing changes.

**Study day.** The day rolls over at a fixed **local 04:00**, as in Anki, so a
late-night session still counts for the evening's day:

```
local     = utc_ms + tz_offset_min * 60 000
real_day  = floor((local - 4 h) / 86 400 000)      # days since 1970-01-01
into_day  = (local - 4 h) mod 86 400 000           # ms since the last 04:00
```

**Applying it** (`now_day` before the sync, `anchor` = `epoch_anchor`):

| state                                | result       | effect                                                   |
|--------------------------------------|--------------|----------------------------------------------------------|
| no anchor yet                        | `Anchored`   | `anchor = real_day - now_day`; the index is unchanged (the first sync defines the device's current day as today) |
| `real_day - anchor > now_day`        | `Advanced`   | `advanceDays(real_day - anchor - now_day)`               |
| `real_day - anchor == now_day`       | `Same`       | nothing but the carry                                    |
| `real_day - anchor < now_day`        | `Reanchored` | `anchor = real_day - now_day`; the index is **kept**     |

and in every accepted case `ms_carry = into_day`, so the uptime and sleep rules
roll the day over at the next 04:00 local.

- **Never rewind.** When the index is ahead of the real date (a manual answer
  that overshot, or the RC oscillator running fast through a long deep sleep),
  the *display* is re-based and the index stays: due days are index days, and
  rewinding would make already-reviewed cards look reviewed in the future.
  From then on index and real date advance together.
- **Counters.** An `Advanced` sync goes through `advanceDays`, so the new-card
  counter resets exactly as on any other day change. The streak compares the
  SRS days of answers, and a sync only moves the day the next answer is
  recorded on: an answer on the day after the last study day extends the
  streak, a later one restarts it at 1. `Same` and `Reanchored` leave both
  untouched.
- **Idempotent.** Posting the same instant twice gives the same state; a later
  instant on the same day only moves the carry. The page posts on every load.
- **Time zones and DST.** The offset is a snapshot. After a DST change (or
  travel) the rollover is off by the difference until the next phone sync (NTP
  reuses the stored offset); the day index itself is unaffected unless the gap
  crosses 04:00.
- **First sync after a power cut.** A never-synced store cannot measure a
  power-off gap: the first sync anchors the current index as today. This can
  only lose a gap once in a device's life.
- **Display.** The statistics screen, the date screen and `/api/stats` show
  `epoch_anchor + now_day`; `/api/stats` also says whether the date came from a
  real clock (`dateSynced`).

**NTP over the home Wi-Fi.** `HomeWifi::fetchUtcMs()` runs SNTP (pool.ntp.org,
time.google.com, time.cloudflare.com) for at most **3 s** once the home Wi-Fi
is connected. NTP yields UTC only, so the result goes through `applyWallClock()`
with the **stored** offset, and only when the `tz` key exists. Without it the
sync is skipped (and logged): guessing UTC could put the study day off by one,
and the offset is normally known, because the portal page that saves the home
Wi-Fi also posts the browser's clock. The radio is never on just for the clock;
NTP runs at two moments:

- **Get decks**, once connected and before the catalog is read. A failed NTP
  is logged and Get decks carries on. The screen stays as it is (a new day
  rebuilds the queue, and "Continue" then shows a fresh question).
- **The boot-time date question** (after a power cut, or a wake that re-asks
  it), once per boot. With a home Wi-Fi and the offset saved, the date screen
  joins in the background (15 s join timeout, CPU at 240 MHz only while the
  radio is on) and says "Syncing via <ssid>..."; the stepper works meanwhile.
  Connected -> NTP (the loop blocks for at most 3 s) -> radio off ->
  `applyWallClock()`, which answers the question and starts the quiz. A failed
  join or NTP turns the radio off and the status line says to set the date by
  hand. Confirming or skipping the question by hand stops the attempt, radio
  off. The join and its outcome count as activity for the idle timers, so the
  30 s dim and the 2 min sleep cannot cut it short; an idle timeout or an
  explicit sleep turns the radio off like any other.

## 6. Queue building

`Queue` (in `srs/`, takes a `PackReader&` and the store) produces a session
list. Rebuilt on boot and on every pack change.

1. **Classify** every card in the current pack by codepoint:
   `new` (`due_day == 0`), `due` (`due_day <= now_day`), or `future`.
2. **Reviews first**, ordered by `due_day` ascending, then stability ascending
   (most overdue / weakest first). Ties broken by codepoint for determinism.
3. **New cards**, ordered by the pack's `level` hint then codepoint — the
   builder's "learn earlier" order, which for the N5 deck means frequency-ish
   and for the Jōyō deck is school order (grade 1..6, then 8, each by
   frequency; `tangocho-pack build --order`, tools/pack-builder/README.md).
   Take at most `new_per_day - new_introduced_count` (and reset the counter when
   `new_introduced_day != now_day`), then append. Reviews are not capped in a
   day; the session as a whole has a soft `session_cap = 200`.
4. **Interleaving the three question types.** State is per card, so the mode is
   chosen when the item is served, not scheduled. For each item pick, among the
   modes this card can be asked (`buildQuestion`'s existing fallbacks), the one
   with the lowest `mode_seen` usage, breaking ties toward a mode different
   from the previous item's mode. This rotates Meaning / Reading /
   MeaningToKanji per card instead of globally (today's `gModeCycle`), and the
   bitmask is persisted so a card's modes even out across sessions. Reading
   questions keep the existing on/kun alternation.
5. **No repeats within a session**: a `seen` set of card indices; an item whose
   card is in `seen` is skipped. New cards are inserted into `seen` when
   introduced, so a card is not asked twice in one run.
6. **In-session relearn**: grade Again (or a lapse on a `t == 0` item) →
   re-insert the card `kRelearnGap = 5` items ahead of the cursor, max 3
   relearn pushes per card per session. At the end of the session its `due_day`
   is `now_day` if still `I < 1`, so it comes back tomorrow. This is the only
   path that can show a card twice.
7. **Empty queue**: "done" state showing the number due next and the interval
   of the next card (from `future`). A tap there is the "study more" override:
   each one raises today's new-card allowance by 10 and rebuilds the queue. It
   belongs to the day it was granted: once the day index moves on (uptime
   rollover, sleep time, a manual date or a real-time sync) it no longer
   applies, and loading another deck resets it.

Optionally mixing one new card every 3 reviews (instead of all reviews then all
new) is a one-constant change; phase 2 keeps the literal reviews-first order.

## 7. Integration in `main.cpp`

New globals: `tgc::SrsStore gSrs;`, `tgc::Queue gQueue;`, `uint32_t gShownAt;`.

| where | change |
|-------|--------|
| `setup()` | after `loadPack`: `gSrs.begin(&gSrsFile)` (creates `/state`, loads, `beginSession()`), then `gQueue.rebuild(gPack, gSrs, gRng)`; replace the "random card" `newQuestion()` with a queue pop. Add a boot line to the serial log with `now_day`, `count`, due/new counts. |
| `newQuestion()` → `nextQuestion()` | no longer picks `gRng.below(cardCount)`. Pops the next `QueueItem` (card index + mode), calls the existing `buildQuestion` with fallbacks, sets `gShownAt = millis()`, and logs `S/D/I` estimates. Returns false when the queue is exhausted → new "done" screen. |
| `tapAction()` | count the navigation tap (`gNavTaps`); the response time later subtracts a fixed allowance per tap. Unchanged otherwise. |
| `holdAction()` | compute `correct`, `rt_ms` and `grade` (`rt >= slow_ms` → Hard), call `gSrs.grade(card.codepoint, grade, now_day)`, `gQueue.answered(cardIndex, grade)`, mark dirty, then the existing reveal. QLOG grows one line: grade, `S -> S'`, `D`, interval. |
| `loop()` | add `gSrs.tick(millis())` and `gSrs.maybeFlush(millis(), dirtyCount)`. Keep the existing 10 ms delay. |
| `applyWallClock()` | real time from the portal's `POST /time` or from NTP over the home Wi-Fi (Get decks, the boot-time date question): `gSrs.syncWallClock()`, NVS `tz`, save, rebuild the queue when the day moved, and dismiss the boot-time date question (§5.1). |
| `selectPack()` / `loadPack()` | flush state before opening the new pack; after loading, `gQueue.rebuild(...)`. State is global, so progress carries across decks. |
| `openMenu()`/`drawMenu()` | optional: append `due/new` counts per pack (a `gSrs.countDue(gPack, now_day)` helper) and a small progress line in the quiz header. |
| `showError()` | a corrupt state file is *not* an error screen; log and start empty. |

`TGC_QUIZ_LOG` stays the debugging channel; phase 2 adds the grade/S/D/interval
line to it rather than introducing a second mechanism.

The quiz loop itself (question → reveal → next) is unchanged; scheduling only
changes *which* card the loop asks next and records the outcome.

## 8. Implementation task list

1. **`docs/srs.md`** (this file) and a dated entry in `docs/decisions.md`:
   FSRS-5 fixed weights, codepoint key, session-day clock, `/state/srs.bin`.
2. **`firmware/src/srs/Srs.h` / `Srs.cpp`** — pure math, no Arduino:
   `enum class Grade { Again, Hard, Good }`, `struct Memory { float s, d; }`,
   `Memory initMemory(Grade)`, `float retrievability(float s, float tDays)`,
   `Memory nextMemory(Memory, Grade, float elapsedDays)`,
   `int intervalDays(float s, Rng&)`, the weight table and clamps.
3. **`firmware/src/srs/SrsStore.h` / `SrsStore.cpp`** — records (sorted
   `std::vector`), get-or-create by codepoint, `grade(codepoint, Grade, day)`,
   the day clock (`beginSession`, `tick`, `today`), `serialize`/
   `deserialize` (CRC-32), `StateIo` interface (`read`/`writeAtomic`) and a
   `MemoryIo` test double. No `millis()`.
4. **`firmware/src/srs/Queue.h` / `Queue.cpp`** — classify, order, new cap,
   mode choice, `seen` set, relearn gap/cap; depends on `pack/` and `quiz/`.
5. **`firmware/src/platform/FsSrsFile.h`** — device-only `StateIo` over
   LittleFS: create `/state`, load with `.bak` fallback, temp+rename atomic
   write, unknown-version backup.
6. **`firmware/src/main.cpp`** — the wiring in §7.
7. **`firmware/platformio.ini`** — native env: add `+<srs/>` to
   `build_src_filter`; `test_filter = test_native, test_srs`.
8. **`firmware/test/test_srs/test_srs.cpp`** (new dir; keeps the SRS tests
   independent of `test_native/test_main.cpp`):
   - `init_memory`: S from `w[0..2]`, D clamp, D0(Good) ≈ 5.28.
   - `retrievability`: `R(0, S) == 1`, `R(S, S) ≈ 0.9`, monotone decreasing.
   - `interval_at_90`: `I == round(S)`, min 1, cap 365, fuzz within ±5 % and
     deterministic for a seed.
   - `success_growth`: Hard < Good, S increases, D moves the right way.
   - `lapse`: S decreases and never exceeds the old S; D increases; S ≥ S_MIN.
   - `same_day`: `t == 0` uses `w[17]/w[18]`; Again lowers S, Good raises it.
   - `reference_vectors`: a handful of `(S, D, t, grade) → (S', D')` from
     `fsrs-rs`/`ts-fsrs` (e.g. `next_difficulty(5.0)` = 8.34176237, 6.66599536,
     4.99022837 for Again/Hard/Good), compared with tolerance.
   - `serialize_roundtrip`: store → bytes → store equality; sorted order kept.
   - `serialize_rejects`: bad magic, version 2, bad size, bad CRC, truncated.
   - `codepoint_identity`: a card in a second pack picks up the same record.
   - `store_cap`: adding past 8192 records is refused.
   - `clock_boot_and_uptime`: boot after a used session advances a day; an
     unused boot does not; 24 h of ticks advances exactly one day.
   - `queue_due_before_new` and `queue_new_cap_resets_on_new_day`.
   - `queue_no_repeat_within_session`, `queue_relearn_requeues_and_caps`.
   - `queue_interleaves_modes`: no two consecutive items share a mode when a
     card offers alternatives, and all three modes appear.
   - `queue_cross_pack_state`: same kanji in a second pack arrives as a review.
9. **Docs touch-up** — `docs/architecture.md` already lists `srs/`; extend the
   `LittleFS` box in its diagram with `state/srs.bin`, and add the sparsity note
   to `firmware/README.md` ("`uploadfs` also clears review progress").
10. **Hardware pass** — verify flush-on-power-pull loses ≤ 8 answers, tune
    `slow_ms` with `TGC_QUIZ_LOG` output, and check the flash-write pauses are
    not visible.

## 9. Open questions / risks

- **Response time is mostly UI.** Options are navigated one tap at a time and
  confirming needs a 600 ms hold, so the measured time mixes recall speed with
  option position. Worst case the signal is noise and `slow_ms` should be
  disabled (0xFFFF) until it is calibrated on hardware. This is the main reason
  the timing knob is persisted rather than compiled in.
- **Multiple choice is a weak signal.** A correct guess looks like a Good.
  Mitigations, in order of likely value: raise desired retention above 0.9
  (shortens intervals; costs `powf` for the interval and loses the `I = S`
  nicety); weight lapses more; or require a second consecutive correct before
  a card leaves the "learning" set. Decide after hardware use.
- **The day index is not wall time** (§5) until a real time is synced (§5.1),
  and after a power cut it is only as good as the user's answer. The 365-day
  cap and FSRS's per-review recomputation bound the damage; opening the Wi-Fi
  portal from a phone now and then re-anchors it.
- **`uploadfs` clears `/state`.** Unavoidable in LittleFS; document it. A
  small NVS mirror is possible only for tiny collections.
- **Flash wear and write pauses.** ~40 KB per flush, every 8 answers. Fine on
  paper; measure the pause on the S3, especially with the sprite push.
- **LittleFS rename atomicity** under a hard power cut is assumed, not proven
  here; the `.bak` fallback is the safety net. A first hardware test should cut
  power mid-flush deliberately.
- **Default weights are not ours.** They are optimised on Anki free-recall
  data; a 3-grade MC app may want different `w[0..3]`/`w[8]`. Off-device
  optimisation from `TGC_QUIZ_LOG` dumps is a phase 3+ option; the format
  leaves room for an on-device weight override later.
- **`float` determinism** between host and ESP32 is close but not bit-exact
  (FPU contraction). Native tests compare with tolerance; keep the SRS math in
  one translation unit so the compiler options are the same.
- **State namespace across languages.** Shared per codepoint in v1; if en/es
  mixing proves confusing, add a 1-byte language namespace using the record's
  reserved byte (a v1-compatible extension because `0` can mean "all").
- **New-card `level` order** is a builder hint ("lower = learn earlier") and
  was not designed as a curriculum order; if it looks wrong on device, sort new
  cards by `freq` instead.

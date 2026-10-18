#!/usr/bin/env python3
"""Checks the portal page's time zone rules (a developer check, not part of
CI: it needs Node.js and a host C++ compiler).

1. Extracts `tzr()` (kTzRuleJs in src/platform/WifiPortal.cpp), the function
   the page uses to turn the browser's zone into a POSIX TZ rule, and runs it
   under Node.js for every zone Python's zoneinfo knows (process.env.TZ), as a
   browser on that day would.
2. Builds a small host program from src/srs/TimeZone.cpp and WallClock.cpp,
   the parser and offset code the device uses, and evaluates each rule.
3. Compares the rule's offsets with the browser's own (Node's
   getTimezoneOffset()) every 12 h and one second either side of every change,
   for the year the rule was derived in -- they must match exactly, that is the
   function's promise -- and the next three years (reported only: a zone whose
   rules change later is caught by the next page visit).
4. Reports where Node's tzdata and the system's (zoneinfo) disagree: those are
   tzdata releases, not firmware bugs.

Usage: python3 scripts/check_tz_rules.py [--now 2026-10-18] [--zones A,B,...]
Exit status 1 when a rule does not parse or is wrong in its own year.
"""
import argparse
import json
import os
import re
import subprocess
import sys
import tempfile
from datetime import datetime, timezone
from zoneinfo import ZoneInfo, available_timezones

FIRMWARE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

EVAL_CPP = r"""
#include <cstdio>
#include <cstring>
#include "srs/TimeZone.h"
// stdin: "R <rule>" selects a rule, then one UTC second per line.
// stdout: the offset in minutes, or "ERR" when the rule does not parse.
int main() {
    char line[256];
    tgc::TzRule r;
    bool ok = false;
    while (std::fgets(line, sizeof line, stdin)) {
        line[std::strcspn(line, "\n")] = 0;
        if (line[0] == 'R') {
            ok = tgc::parseTzRule(line + 2, &r);
            if (!ok) std::puts("ERR");
            continue;
        }
        long long t = 0;
        std::sscanf(line, "%lld", &t);
        if (ok) std::printf("%d\n", tgc::tzOffsetMinAt(r, t * 1000));
    }
}
"""

NODE_RUNNER = r"""
const fs = require('fs');
const job = JSON.parse(fs.readFileSync(process.argv[2], 'utf8'));
const out = {};
for (const z of Object.keys(job.instants)) {
  process.env.TZ = z;
  let r;
  try { r = tzr(job.now); } catch (e) { r = 'THROW ' + e; }
  out[z] = {rule: r, offsets: job.instants[z].map(t => -new Date(t * 1000).getTimezoneOffset())};
}
console.log(JSON.stringify(out));
"""


def extract_js():
    src = open(os.path.join(FIRMWARE, "src/platform/WifiPortal.cpp"), encoding="utf-8").read()
    m = re.search(r'const char kTzRuleJs\[\] = R"JS\((.*?)</script>\)JS";', src, re.S)
    if not m:
        sys.exit("kTzRuleJs not found in WifiPortal.cpp")
    return m.group(1)


def offset(z, t):
    return int(datetime.fromtimestamp(t, z).utcoffset().total_seconds() // 60)


def instants(z, y0, y1):
    """Every 12 h in [y0, y1) plus one second either side of each change."""
    t = int(datetime(y0, 1, 1, tzinfo=timezone.utc).timestamp())
    end = int(datetime(y1, 1, 1, tzinfo=timezone.utc).timestamp())
    out = []
    o = offset(z, t)
    while t < end:
        out.append(t)
        n = offset(z, t + 43200)
        if n != o:
            lo, hi = t, t + 43200
            while hi - lo > 1:
                m = (lo + hi) // 2
                if offset(z, m) == o:
                    lo = m
                else:
                    hi = m
            out += [lo, hi]
            o = n
        t += 43200
    return out


def first_utc(ts):
    return datetime.fromtimestamp(ts[0], timezone.utc).strftime("%Y-%m-%d %H:%M:%SZ")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--now", default=None, help="YYYY-MM-DD the page runs on (default: today)")
    ap.add_argument("--zones", default=None, help="comma-separated IANA names (default: all)")
    args = ap.parse_args()
    now = datetime.fromisoformat(args.now).replace(tzinfo=timezone.utc) if args.now \
        else datetime.now(timezone.utc)
    year = now.year
    zones = args.zones.split(",") if args.zones else sorted(
        z for z in available_timezones() if not z.startswith(("posix/", "right/")) and z != "Factory")
    plan = {name: instants(ZoneInfo(name), year, year + 4) for name in zones}

    with tempfile.TemporaryDirectory() as d:
        job = os.path.join(d, "job.json")
        json.dump({"now": int(now.timestamp() * 1000), "instants": plan}, open(job, "w"))
        runner = os.path.join(d, "run.js")
        open(runner, "w").write(extract_js() + NODE_RUNNER)
        node = json.loads(subprocess.run(["node", runner, job], capture_output=True, text=True,
                                         check=True).stdout)
        src = os.path.join(d, "eval.cpp")
        exe = os.path.join(d, "eval")
        open(src, "w").write(EVAL_CPP)
        subprocess.run(["c++", "-std=c++17", "-O2", "-I", os.path.join(FIRMWARE, "src"), src,
                        os.path.join(FIRMWARE, "src/srs/TimeZone.cpp"),
                        os.path.join(FIRMWARE, "src/srs/WallClock.cpp"), "-o", exe], check=True)
        lines = []
        for name in zones:
            lines.append("R " + node[name]["rule"])
            lines += [str(t) for t in plan[name]]
        out = iter(subprocess.run([exe], input="\n".join(lines) + "\n", capture_output=True,
                                  text=True, check=True).stdout.split())

    own_end = int(datetime(year + 1, 1, 1, tzinfo=timezone.utc).timestamp())
    wrong, later, data = [], [], []
    for name in zones:
        rule, browser = node[name]["rule"], node[name]["offsets"]
        ts = plan[name]
        first = next(out)
        if first == "ERR":
            wrong.append((name, rule, "does not parse"))
            continue
        got = [int(first)] + [int(next(out)) for _ in ts[1:]]
        own = [t for t, g, b in zip(ts, got, browser) if t < own_end and g != b]
        rest = [t for t, g, b in zip(ts, got, browser) if t >= own_end and g != b]
        tzdata = [t for t, b in zip(ts, browser) if b != offset(ZoneInfo(name), t)]
        if own:
            wrong.append((name, rule, "%d in %d, first %s" % (len(own), year, first_utc(own))))
        elif rest:
            later.append((name, rule, "%d after %d, first %s" % (len(rest), year, first_utc(rest))))
        if tzdata:
            data.append((name, rule, "%d, first %s" % (len(tzdata), first_utc(tzdata))))

    print("%d zones, rules derived on %s (Node tzdata %s)" % (
        len(zones), now.date(),
        subprocess.run(["node", "-p", "process.versions.tz"], capture_output=True,
                       text=True).stdout.strip()))
    sections = (("Rule differs from the browser in its own year (bug)", wrong),
                ("Rule differs from the browser in later years (rules change)", later),
                ("Browser (Node) and system tzdata differ (data releases)", data))
    for title, rows in sections:
        print("\n%s: %d" % (title, len(rows)))
        for name, rule, why in rows:
            print("  %-32s %-48s %s" % (name, rule, why))
    return 1 if wrong else 0


if __name__ == "__main__":
    sys.exit(main())

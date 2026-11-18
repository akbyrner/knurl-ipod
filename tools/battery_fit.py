#!/usr/bin/env python3
"""Fit the battery curve (device/power.c) to this iPod's cell from /.knurl/battery.log.

    tools/battery_fit.py <iPod>/.knurl/battery.log

The log (format in device/power.c) holds a sample a minute. The fit needs one complete
discharge: charge the iPod full, unplug it, use it normally until Knurl turns it off with
BATTERY LOW. Time on battery stands in for charge used (fine for steady use like playback);
time spent switched off is left out. Prints the 11-value table for power.c: mV at 0%..100%.
"""
import struct
import sys

START, SAMPLE, OFF = 0xB0, 0xB1, 0xB2
CHARGING, USB = 1, 2
OFF_BATTERY = 3


def records(path):
    data = open(path, "rb").read()
    for i in range(0, len(data) - 7, 8):
        tag, flags, mv, s = struct.unpack_from("<BBHI", data, i)
        if tag == 0:
            break
        yield tag, flags, mv, s


def discharges(recs):
    """Runs on battery: lists of (seconds on battery, mV), ended by the next charge, or by BATTERY LOW."""
    run, clock, last_s, ended = [], 0, None, None
    for tag, flags, mv, s in recs:
        if tag == START:
            last_s = None                       # a new boot: its clock starts again
            continue
        if tag == OFF:
            if run and flags == OFF_BATTERY:
                yield run, True
                run, clock = [], 0
            last_s = None
            continue
        if flags & (CHARGING | USB):            # on the charger: any run so far is over
            if run:
                yield run, False
            run, clock, last_s = [], 0, s
            continue
        if last_s is not None:
            clock += s - last_s
        last_s = s
        run.append((clock, mv))
    if run:
        yield run, False


def smooth(run, k=5):
    out = []
    for i in range(len(run)):
        window = sorted(mv for _, mv in run[max(0, i - k // 2): i + k // 2 + 1])
        out.append((run[i][0], window[len(window) // 2]))
    return out


def fit(run):
    run = smooth(run)
    total = run[-1][0]
    table = []
    for pct in range(0, 101, 10):
        t = total * (1 - pct / 100)
        j = min(range(len(run)), key=lambda i: abs(run[i][0] - t))
        table.append(run[j][1])
    for i in range(len(table) - 2, -1, -1):     # the table must rise with charge
        table[i] = min(table[i], table[i + 1] - 1)
    return total, table


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    runs = list(discharges(records(sys.argv[1])))
    if not runs:
        sys.exit("No time on battery in the log yet.")
    full = [r for r, complete in runs if complete and len(r) >= 30]
    if not full:
        longest = max(runs, key=lambda r: len(r[0]))[0]
        h = longest[-1][0] / 3600
        sys.exit(f"No complete discharge yet (longest run on battery: {h:.1f} h, "
                 f"{longest[0][1]} -> {longest[-1][1]} mV). Run it until Knurl turns off with BATTERY LOW.")
    total, table = fit(full[-1])
    print(f"Last full discharge: {total / 3600:.1f} h on battery, {full[-1][0][1]} -> {full[-1][-1][1]} mV.")
    print("device/power.c:")
    print("static const uint16_t curve[11] = { " + ", ".join(map(str, table)) + " };")


if __name__ == "__main__":
    main()

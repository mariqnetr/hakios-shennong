#!/usr/bin/env python3
"""gpu-report.py LOG [LOG2]: one run of the desktop's GPU in numbers, from the touch script's first step on.

    python3 -I tools/gpu-report.py build/hackios/logs/last.log
    python3 -I tools/gpu-report.py before.log after.log      (the two side by side)

Reads what the kernel and the Metal plug-in print: the plug-in's fps lines (GPU time a command buffer, frames late and
stalled while something moves), the kernel's DCVS line (time by MHz, temperatures), its counters line (sgpu_counters=1)
and the GMEM phases line (metal_gmem_phases=1). Lines a log repeats (the live log's copies) are counted once.
"""
import re
import sys
from collections import defaultdict


def lines_of(path):
    # The session's console as it came, up to the kernel's own log the loader appends after it ("== KERNEL LOG ==",
    # the same lines again with times): read once, in order, every line counted (a line may rightly come twice)
    text = open(path, errors="ignore").read()
    parts = text.split("== KERNEL LOG ==")
    text = max(parts, key=len)     # (the kernel's log is the whole; a short session console before it is a part of it)
    text = text.split("--- kernel message buffer ---")[0]   # (then the message buffer: its tail again, with times)
    started = "touch script: round" not in text
    for raw in text.splitlines():
        line = re.sub(r"^\[\s*[\d.]+\]: ", "", raw)
        if "touch script: round" in line:
            started = True
        yield started, line


COUNTERS = ["cp", "sp", "alu", "sp-stall-tp", "sp-noexec", "sp-stall-rb", "waves", "icl1-miss", "fs-full", "fs-half", "pixels",
            "eu-fs", "tp", "tp-l1-req", "tp-l1-miss", "rb", "rb-starve-sp", "rb-stall-ccu", "rb-stall-hlsq", "ccu", "ccu-blocks", "ccu-partial", "ccu-gmem-rd", "ccu-gmem-wr", "rb-fifo0-full", "uche-stall",
            "rd-ch0", "rd-ch1", "wr-ch0", "wr-ch1"]


def report(path, group=None):
    busy_ok = False
    r = defaultdict(float)
    mhz = defaultdict(float)
    counters = defaultdict(list)
    temps = {"gpu": [], "cpu": []}
    segments = defaultdict(lambda: [0, 0, 0])
    segment = "before the script"
    script = False
    ab = None
    fresh = 0       # (the first 5 s lines after a flip straddle it: left out)
    for started, line in lines_of(path):
        script = script or started
        m = re.search(r"A/B: (\S+ [AB]) \(", line)
        if m:
            ab = m.group(1)
            if re.match(r"\S+ A$", ab):
                ab = "A"
            elif ab.startswith("gmem B") or ab.startswith("order B"):
                ab = ab
            fresh = 2
            continue
        if group is not None and ab != group:
            continue
        if fresh and ("sgpu: dcvs:" in line or "sgpu: counters per busy cycle" in line):
            if "(more)" not in line:
                fresh -= 1
            continue
        m = re.search(r"touch script: round \d+: (.*)", line)
        if m:
            segment = re.sub(r"\d+$", "N", m.group(1))
            continue
        if not started and script:
            continue
        m = re.search(r"(\d+) command buffers\); GPU busy (\d+) ms", line)
        if m and started:
            r["cbs"] += int(m.group(1))
            r["gpu_ms"] += int(m.group(2))
        m = re.search(r"(\d+) on time.*?(\d+) a frame late.*?(\d+) stalls", line)
        if m and started:
            a, b, c = map(int, m.groups())
            r["on"] += a
            r["late"] += b
            r["stalls"] += c
            s = segments[segment]
            s[0] += a
            s[1] += b
            s[2] += c
        m = re.search(r"sgpu: dcvs: the last 5 s by MHz:(.*?); now", line)
        if m and started:
            for k, v in re.findall(r"(\d+):(\d+)%", m.group(1)):
                mhz[int(k)] += int(v)
            t = re.search(r"gpu ([\d.]+) C, cpu ([\d.]+) C", line)
            if t:
                temps["gpu"].append(float(t.group(1)))
                temps["cpu"].append(float(t.group(2)))
        m = re.search(r"sgpu: counters per busy cycle, the last 5 s: (?:busy (\d+) Mcycles;|\(more\))(.*)", line)
        if m and started:
            if m.group(1) is not None:
                busy_ok = int(m.group(1)) > 50
                if busy_ok:
                    counters["busy Mcycles/5s"].append(float(m.group(1)))
            if busy_ok:
                known = "|".join(re.escape(c) for c in COUNTERS)
                for name, val in re.findall(r" (" + known + r") (\d+(?:\.\d+)?)(?= |$)", m.group(2)):
                    counters[name].append(float(val))
                for name, val in re.findall(r" (" + known + r") (\d+) MB/s", m.group(2)):
                    counters[name + " MB/s"].append(float(val))
        m = re.search(r"GMEM phases of (\d+) passes.*?loads (\d+), draws (\d+), stores (\d+)", line)
        if m and started:
            r["ph_n"] += 1
            r["ph_load"] += int(m.group(2))
            r["ph_draw"] += int(m.group(3))
            r["ph_store"] += int(m.group(4))
    out = {}
    if r["cbs"]:
        out["GPU ms a command buffer"] = r["gpu_ms"] / r["cbs"]
    frames = r["on"] + r["late"] + r["stalls"]
    if frames:
        out["frames while moving"] = frames
        out["late % (one frame)"] = 100 * r["late"] / frames
        out["stalls per 1000 (>50 ms)"] = 1000 * r["stalls"] / frames
    total = sum(mhz.values())
    if total:
        out["GPU mean MHz"] = sum(k * v for k, v in mhz.items()) / total
        out["GPU at 903 MHz %"] = 100 * mhz.get(903, 0) / total
    for k in ("gpu", "cpu"):
        if temps[k]:
            out[f"{k} C mean / max"] = f"{sum(temps[k]) / len(temps[k]):.1f} / {max(temps[k]):.1f}"
    if r["ph_n"]:
        out["GMEM pass us: loads / draws / stores"] = f"{r['ph_load'] / r['ph_n']:.0f} / {r['ph_draw'] / r['ph_n']:.0f} / {r['ph_store'] / r['ph_n']:.0f}"
    for name, vals in counters.items():
        out["counter " + name] = sum(vals) / len(vals)
    for seg, (a, b, c) in sorted(segments.items()):
        n = a + b + c
        if n:
            out[f"stalls/1000 in '{seg}'"] = f"{1000 * c / n:.1f} ({n} frames, late {100 * b / n:.1f}%)"
    return out


def show(v):
    return f"{v:.2f}" if isinstance(v, float) else str(v)


def main():
    names, runs = [], []
    for p in sys.argv[1:]:
        text = open(p, errors="ignore").read()
        if "A/B: " in text:
            groups = []
            for g in re.findall(r"A/B: (\S+ [AB]) \(", text):
                g = "A" if re.match(r"\S+ A$", g) else g
                if g not in groups:
                    groups.append(g)
            for g in groups:
                names.append(f"{p[-10:]} {g}")
                runs.append(report(p, g))
        else:
            names.append(p)
            runs.append(report(p))
    keys = []
    for o in runs:
        for k in o:
            if k not in keys:
                keys.append(k)
    width = max(len(k) for k in keys) if keys else 10
    print(" " * width + "".join(f"  {p[-30:]:>30}" for p in names))
    for k in keys:
        print(f"{k:<{width}}" + "".join(f"  {show(o.get(k, '-')):>30}" for o in runs))


if __name__ == "__main__":
    main()

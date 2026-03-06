#!/usr/bin/env python3
import argparse
import json
import re
from pathlib import Path


LINE_RE = re.compile(
    r"\[writer\.flight\]\s+file=(?P<file>\S+)\s+"
    r"flush\.size=(?P<size>\d+)\s+"
    r"flush\.timeout=(?P<timeout>\d+)\s+"
    r"flush\.finalize=(?P<finalize>\d+)\s+"
    r"avg_raw_batch_kb=(?P<avgkb>\d+)\s+"
    r"batch_mode=(?P<mode>\S+)\s+"
    r"batch_kb=(?P<batchkb>\d+)\s+"
    r"ratio=(?P<ratio>[0-9.]+)\s+"
    r"first_flush_ms=(?P<ffms>-?[0-9.]+)"
)
PWRITE_RE = re.compile(
    r"\[writer\.pwrite\]\s+file=(?P<file>\S+)\s+"
    r"writes=(?P<writes>\d+)\s+"
    r"bytes=(?P<bytes>\d+)\s+"
    r"wait_calls=(?P<wait_calls>\d+)\s+"
    r"wait_us_total=(?P<wait_total>\d+)\s+"
    r"wait_us_avg=(?P<wait_avg>\d+)\s+"
    r"wait_us_max=(?P<wait_max>\d+)\s+"
    r"first_write_ms=(?P<first_ms>-?[0-9.]+)"
)

TIME_RE = re.compile(r"^real\s+([0-9.]+)$")


def parse_stderr(path: Path):
    rows = []
    pwrite_rows = []
    real_s = None
    for raw in path.read_text(errors="ignore").splitlines():
        m = LINE_RE.search(raw)
        if m:
            d = m.groupdict()
            rows.append(
                {
                    "file": d["file"],
                    "flush_size": int(d["size"]),
                    "flush_timeout": int(d["timeout"]),
                    "flush_finalize": int(d["finalize"]),
                    "avg_raw_batch_kb": int(d["avgkb"]),
                    "batch_mode": d["mode"],
                    "batch_kb": int(d["batchkb"]),
                    "ratio": float(d["ratio"]),
                    "first_flush_ms": float(d["ffms"]),
                }
            )
            continue
        m2 = PWRITE_RE.search(raw)
        if m2:
            d = m2.groupdict()
            pwrite_rows.append(
                {
                    "file": d["file"],
                    "writes": int(d["writes"]),
                    "bytes": int(d["bytes"]),
                    "wait_calls": int(d["wait_calls"]),
                    "wait_us_total": int(d["wait_total"]),
                    "wait_us_avg": int(d["wait_avg"]),
                    "wait_us_max": int(d["wait_max"]),
                    "first_write_ms": float(d["first_ms"]),
                }
            )
            continue
        tm = TIME_RE.search(raw.strip())
        if tm:
            real_s = float(tm.group(1))
    return rows, pwrite_rows, real_s


def parse_trace(path: Path):
    ev = json.loads(path.read_text())["traceEvents"]
    out = {}
    for e in ev:
        if e.get("ph") != "X":
            continue
        name = e.get("name", "")
        dur = int(e.get("dur", 0))
        if ".gap." in name:
            k = name.split(".gap.", 1)[1]
            out[k] = out.get(k, 0) + dur
        elif name.endswith(".busy"):
            out["busy"] = out.get("busy", 0) + dur
        elif name.endswith(".total"):
            out["total"] = out.get("total", 0) + dur
    return out


def main():
    ap = argparse.ArgumentParser(description="Analyze writer flight batch stats + trace gaps")
    ap.add_argument("--stderr", required=True, help="fastp stderr path")
    ap.add_argument("--trace", default="", help="trace json path")
    args = ap.parse_args()

    rows, pwrite_rows, real_s = parse_stderr(Path(args.stderr))
    if not rows:
        print("No [writer.flight] lines found.")
    else:
        print("Writer flight summary:")
        total_size = total_timeout = total_finalize = 0
        ff = []
        for r in rows:
            total_size += r["flush_size"]
            total_timeout += r["flush_timeout"]
            total_finalize += r["flush_finalize"]
            if r["first_flush_ms"] >= 0:
                ff.append(r["first_flush_ms"])
            print(
                f"  {r['file']}: size={r['flush_size']} timeout={r['flush_timeout']} finalize={r['flush_finalize']} "
                f"avg_batch={r['avg_raw_batch_kb']}KB mode={r['batch_mode']} batch_kb={r['batch_kb']} "
                f"ratio={r['ratio']:.3f} first_flush={r['first_flush_ms']:.2f}ms"
            )
        print(
            f"  total: size={total_size} timeout={total_timeout} finalize={total_finalize} "
            f"timeout_share={(100.0 * total_timeout / max(1, total_size + total_timeout + total_finalize)):.1f}%"
        )
        if real_s is not None and ff:
            ff_max = max(ff)
            print(f"  first_flush_max_share_of_runtime={100.0 * (ff_max / 1000.0) / real_s:.2f}% (real={real_s:.3f}s)")

    if pwrite_rows:
        print("Writer pwrite summary:")
        total_writes = total_wait_calls = total_wait_us = total_bytes = 0
        first_ms = []
        for r in pwrite_rows:
            total_writes += r["writes"]
            total_wait_calls += r["wait_calls"]
            total_wait_us += r["wait_us_total"]
            total_bytes += r["bytes"]
            if r["first_write_ms"] >= 0:
                first_ms.append(r["first_write_ms"])
            print(
                f"  {r['file']}: writes={r['writes']} bytes={r['bytes']} wait_calls={r['wait_calls']} "
                f"wait_total={r['wait_us_total']/1000.0:.2f}ms wait_avg={r['wait_us_avg']}us wait_max={r['wait_us_max']}us "
                f"first_write={r['first_write_ms']:.2f}ms"
            )
        ratio = 100.0 * total_wait_calls / max(1, total_writes)
        print(
            f"  total: writes={total_writes} bytes={total_bytes} wait_calls={total_wait_calls} "
            f"wait_call_ratio={ratio:.2f}% wait_total={total_wait_us/1000.0:.2f}ms"
        )
        if real_s is not None and first_ms:
            first_max = max(first_ms)
            print(f"  first_write_max_share_of_runtime={100.0 * (first_max / 1000.0) / real_s:.3f}% (real={real_s:.3f}s)")

    if args.trace:
        gap = parse_trace(Path(args.trace))
        total = max(1, gap.get("total", 0))
        print("Trace gap summary:")
        for k in ("wait_raw_chunk", "queue_full_wait", "backpressure_writer", "wait_input"):
            if k in gap:
                print(f"  {k}: {gap[k]/1000.0:.3f} ms ({100.0*gap[k]/total:.2f}% of total)")
        if "busy" in gap:
            print(f"  busy: {gap['busy']/1000.0:.3f} ms ({100.0*gap['busy']/total:.2f}% of total)")


if __name__ == "__main__":
    main()

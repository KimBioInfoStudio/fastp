#!/usr/bin/env python3
"""Quick BGZF benchmark for fastp auto BGZF thread routing.

Example:
  python3 scripts/bench_bgzf.py --fastp ./fastp --threads 4,8,12 --runs 3
"""

import argparse
import json
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path
from statistics import median


BENCH_DIR = Path("/tmp/fastp_bgzf_bench")
DATA_DIR = BENCH_DIR / "data"
OUT_DIR = BENCH_DIR / "out"
RESULT_JSON = BENCH_DIR / "results_bgzf.json"


def require_cmd(name: str) -> None:
    if shutil.which(name) is None:
        print(f"Missing required command: {name}", file=sys.stderr)
        sys.exit(1)


def run_cmd(cmd: list[str]) -> None:
    r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    if r.returncode != 0:
        print("Command failed:", " ".join(cmd), file=sys.stderr)
        if r.stderr:
            print(r.stderr.strip(), file=sys.stderr)
        sys.exit(1)


def ensure_bgzf_input(bgzf_path: Path, pairs: int, seed: int) -> None:
    if bgzf_path.exists():
        return

    DATA_DIR.mkdir(parents=True, exist_ok=True)
    fq_path = DATA_DIR / "bench_bgzf.fq"

    # Generate deterministic small FASTQ file.
    import random
    random.seed(seed)
    bases = "ACGT"
    q = "I" * 151
    with fq_path.open("w") as f:
        for i in range(pairs):
            seq = "".join(random.choice(bases) for _ in range(151))
            f.write(f"@bgzf_{i}\n{seq}\n+\n{q}\n")

    # bgzip produces BGZF-compliant .gz blocks.
    # bgzip -c output is in stdout; capture via Python pipe for portability.
    with bgzf_path.open("wb") as out:
        p = subprocess.Popen(
            ["bgzip", "-f", "-@", "4", "-c", str(fq_path)],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        data, err = p.communicate()
        if p.returncode != 0:
            print("bgzip failed", file=sys.stderr)
            if err:
                print(err.decode("utf-8", errors="ignore"), file=sys.stderr)
            sys.exit(1)
        out.write(data)


def run_once(fastp_bin: str, in_bgzf: Path, out_fq: Path, threads: int) -> float:
    cmd = [
        fastp_bin,
        "-i",
        str(in_bgzf),
        "-o",
        str(out_fq),
        "-w",
        str(threads),
        "-j",
        "/tmp/fastp_bgzf_bench/fastp.json",
        "-h",
        "/tmp/fastp_bgzf_bench/fastp.html",
    ]
    t0 = time.perf_counter()
    r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    dt = time.perf_counter() - t0
    if r.returncode != 0:
        print(r.stderr, file=sys.stderr)
        raise RuntimeError(f"fastp failed for threads={threads}")
    return dt


def main() -> None:
    parser = argparse.ArgumentParser(description="Quick BGZF benchmark for fastp")
    parser.add_argument("--fastp", default="./fastp", help="path to fastp binary")
    parser.add_argument("--threads", default="4,8,12", help="comma-separated thread list")
    parser.add_argument("--runs", type=int, default=3, help="runs per thread")
    parser.add_argument("--pairs", type=int, default=200000, help="reads to generate if input missing")
    parser.add_argument("--seed", type=int, default=20260305, help="random seed for data generation")
    parser.add_argument("--input-bgzf", default="", help="optional existing BGZF .fq.gz input")
    args = parser.parse_args()

    if not os.access(args.fastp, os.X_OK):
        print(f"fastp binary not executable: {args.fastp}", file=sys.stderr)
        sys.exit(1)
    require_cmd("bgzip")

    BENCH_DIR.mkdir(parents=True, exist_ok=True)
    OUT_DIR.mkdir(parents=True, exist_ok=True)

    if args.input_bgzf:
        in_bgzf = Path(args.input_bgzf)
    else:
        in_bgzf = DATA_DIR / "bench_bgzf.fq.gz"
        ensure_bgzf_input(in_bgzf, args.pairs, args.seed)

    thread_list = [int(x.strip()) for x in args.threads.split(",") if x.strip()]
    results: dict[str, dict[str, float | int | list[float]]] = {}

    print(f"Input BGZF: {in_bgzf}")
    print(f"Threads: {thread_list}, runs={args.runs}")

    for w in thread_list:
        samples = []
        for i in range(args.runs):
            out_fq = OUT_DIR / f"out.w{w}.run{i}.fq"
            if out_fq.exists():
                out_fq.unlink()
            dt = run_once(args.fastp, in_bgzf, out_fq, w)
            samples.append(dt)
        med = median(samples)
        results[str(w)] = {
            "threads": w,
            "median_sec": med,
            "samples_sec": samples,
        }
        print(f"w={w:>2} median={med:.4f}s samples={[round(x,4) for x in samples]}")

    best_w = min(thread_list, key=lambda x: results[str(x)]["median_sec"])
    print("\nAligned Summary")
    print("threads | median_sec")
    print("--------+----------")
    for w in thread_list:
        print(f"{w:>7} | {results[str(w)]['median_sec']:.4f}")
    print(f"\nBest: w={best_w}, {results[str(best_w)]['median_sec']:.4f}s")

    payload = {
        "fastp": args.fastp,
        "input_bgzf": str(in_bgzf),
        "runs": args.runs,
        "results": results,
        "best_threads": best_w,
    }
    RESULT_JSON.write_text(json.dumps(payload, indent=2))
    print(f"Saved: {RESULT_JSON}")


if __name__ == "__main__":
    main()

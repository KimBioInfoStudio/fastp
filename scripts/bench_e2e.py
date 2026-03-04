#!/usr/bin/env python3
"""End-to-end benchmark: optimized vs baseline fastp.

Usage: python3 scripts/bench_e2e.py [OPTIONS]

Options:
  --json PATH     Load results from JSON and print summary table (skip benchmark)
  --mode MODE     Benchmark mode(s), comma-separated (default: all)
                  PE: fq-fq, fq-gz, gz-fq, gz-gz
                  SE: se-fq-fq, se-fq-gz, se-gz-fq, se-gz-gz, stdin-stdout
                  Groups: all, all-pe, all-se
  --pairs N       Number of read pairs to generate (default: 10000000)
  --threads N     Worker threads (default: 4)
  --runs N        Repeat count, median reported (default: 3)
  --seed N        Random seed for data generation (default: 2026)
  --orig PATH     Path to baseline binary (default: none, opt-only mode)
  --opt PATH      Path to optimized binary (default: /tmp/fastp_opt)
"""

import argparse
import gzip
import hashlib
import json
import os
import platform
import random
import shutil
import subprocess
import sys
import time
from pathlib import Path
from statistics import median

# ── Paths ────────────────────────────────────────────────────────────────────

BENCH_DIR = Path("/tmp/fastp_benchmark")
DATA_DIR = BENCH_DIR / "data"
OUT_DIR = BENCH_DIR / "output"
RESULTS_JSON = BENCH_DIR / "results.json"

CPU_COUNT = os.cpu_count() or 4

PE_MODES = ["fq-fq", "fq-gz", "gz-fq", "gz-gz"]
SE_MODES = ["se-fq-fq", "se-fq-gz", "se-gz-fq", "se-gz-gz"]
ALL_MODES = PE_MODES + SE_MODES + ["stdin-stdout"]

MODE_ALIASES = {
    "all-pe": PE_MODES,
    "all-se": SE_MODES + ["stdin-stdout"],
}


def parse_mode(mode: str) -> tuple[str, str, str]:
    """Parse mode string → (type, in_fmt, out_fmt).

    Returns ("stdin", "", "") for stdin-stdout,
            ("se", "fq"|"gz", "fq"|"gz") for se-* modes,
            ("pe", "fq"|"gz", "fq"|"gz") for bare fq-fq etc.
    """
    if mode == "stdin-stdout":
        return ("stdin", "", "")
    if mode.startswith("se-"):
        parts = mode[3:].split("-")
        return ("se", parts[0], parts[1])
    parts = mode.split("-")
    return ("pe", parts[0], parts[1])


def auto_threads(mode: str) -> int:
    """Calculate worker threads, reserving reader/writer for each mode.

    PE:  2 readers (L+R) + 2 writers (L+R) = reserve 4
    SE:  1 reader + 1 writer               = reserve 2
    """
    mtype = parse_mode(mode)[0]
    reserved = 4 if mtype == "pe" else 2
    return max(1, CPU_COUNT - reserved)


# ── Helpers ──────────────────────────────────────────────────────────────────

def human_size(path: Path) -> str:
    sz = path.stat().st_size
    for unit in ("B", "KB", "MB", "GB"):
        if sz < 1024:
            return f"{sz:.1f} {unit}"
        sz /= 1024
    return f"{sz:.1f} TB"


def md5_file(path: Path) -> str:
    h = hashlib.md5()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def md5_gz_content(path: Path) -> str:
    """MD5 of decompressed content (compares FASTQ content, not gz bytes)."""
    h = hashlib.md5()
    with gzip.open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def fq_md5(path: Path) -> str:
    """MD5 of FASTQ content — decompress if .gz, otherwise hash directly."""
    if path.suffix == ".gz":
        return md5_gz_content(path)
    return md5_file(path)


def banner(text: str, char: str = "=", width: int = 44):
    print(char * width)
    print(f"  {text}")
    print(char * width)


def system_info() -> dict:
    """Collect hardware, OS, and current resource snapshot."""
    info: dict = {
        "os": f"{platform.system()} {platform.release()}",
        "arch": platform.machine(),
        "cpus": CPU_COUNT,
    }
    # CPU model + core topology (physical/logical + P/E)
    def _sysctl_int(key: str) -> int | None:
        r = subprocess.run(["sysctl", "-n", key], capture_output=True, text=True)
        return int(r.stdout.strip()) if r.returncode == 0 and r.stdout.strip().isdigit() else None

    def _count_cpulist(path: Path) -> int:
        n = 0
        for part in path.read_text().strip().split(","):
            if "-" in part:
                lo, hi = part.split("-", 1)
                n += int(hi) - int(lo) + 1
            elif part.strip():
                n += 1
        return n

    if sys.platform == "darwin":
        r = subprocess.run(["sysctl", "-n", "machdep.cpu.brand_string"],
                           capture_output=True, text=True)
        if r.returncode == 0:
            info["cpu"] = r.stdout.strip()
        # physical / logical totals
        phys = _sysctl_int("hw.physicalcpu")
        logi = _sysctl_int("hw.logicalcpu")
        if phys:
            info["physical_cores"] = phys
        if logi:
            info["logical_cores"] = logi
        # P/E core topology (Apple Silicon, Intel 12th+)
        nlevels = _sysctl_int("hw.nperflevels")
        if nlevels:
            cores = []
            for i in range(nlevels):
                p = _sysctl_int(f"hw.perflevel{i}.physicalcpu")
                l = _sysctl_int(f"hw.perflevel{i}.logicalcpu")
                if l:
                    label = "P" if i == 0 else "E" if i == 1 else f"L{i}"
                    entry: dict = {"type": label, "logical": l}
                    if p and p != l:
                        entry["physical"] = p
                    cores.append(entry)
            if cores:
                info["cores"] = cores
    else:
        # Linux: CPU model
        if os.path.exists("/proc/cpuinfo"):
            with open("/proc/cpuinfo") as f:
                for line in f:
                    if line.startswith("model name"):
                        info["cpu"] = line.split(":", 1)[1].strip()
                        break
        # Linux: physical / logical totals
        cpu_dir = Path("/sys/devices/system/cpu")
        online = cpu_dir / "online"
        if online.exists():
            info["logical_cores"] = _count_cpulist(online)
        # count unique physical core IDs
        phys_ids: set[str] = set()
        for topo in cpu_dir.glob("cpu[0-9]*/topology/core_id"):
            phys_ids.add(topo.read_text().strip())
        if phys_ids:
            info["physical_cores"] = len(phys_ids)
        # Linux: core topology
        # Method 1: Intel hybrid — /sys/devices/system/cpu/types/
        cpu_types_dir = cpu_dir / "types"
        if cpu_types_dir.is_dir():
            cores = []
            for d in sorted(cpu_types_dir.iterdir()):
                cpulist = d / "cpulist"
                if not cpulist.exists():
                    continue
                n = _count_cpulist(cpulist)
                name = d.name  # e.g. "intel_core_0", "intel_atom_0"
                if "core" in name:
                    label = "P"
                elif "atom" in name:
                    label = "E"
                else:
                    label = name
                cores.append({"type": label, "logical": n})
            if cores:
                info["cores"] = cores
        # Method 2: ARM/RISC-V — /sys/devices/system/cpu/cpu*/cpu_capacity
        elif (cpu_dir / "cpu0/cpu_capacity").exists():
            cap_counts: dict[int, int] = {}
            for cap_file in sorted(cpu_dir.glob("cpu[0-9]*/cpu_capacity")):
                cap = int(cap_file.read_text().strip())
                cap_counts[cap] = cap_counts.get(cap, 0) + 1
            if len(cap_counts) > 1:
                labels = ["P", "E"] + [f"L{i}" for i in range(2, 10)]
                cores = []
                for i, (cap, cnt) in enumerate(sorted(cap_counts.items(), reverse=True)):
                    cores.append({"type": labels[min(i, len(labels) - 1)], "logical": cnt})
                info["cores"] = cores
    # Total memory
    if sys.platform == "darwin":
        r = subprocess.run(["sysctl", "-n", "hw.memsize"],
                           capture_output=True, text=True)
        if r.returncode == 0:
            info["mem_total_gb"] = round(int(r.stdout.strip()) / (1 << 30), 1)
    elif os.path.exists("/proc/meminfo"):
        with open("/proc/meminfo") as f:
            for line in f:
                if line.startswith("MemTotal"):
                    info["mem_total_gb"] = round(int(line.split()[1]) / (1 << 20), 1)
                    break
    # Available memory (reclaimable without swapping)
    if sys.platform == "darwin":
        # vm_stat: avail ≈ free + inactive + purgeable
        r = subprocess.run(["vm_stat"], capture_output=True, text=True)
        if r.returncode == 0:
            page_size = os.sysconf("SC_PAGE_SIZE") if hasattr(os, "sysconf") else 16384
            pages: dict[str, int] = {}
            for line in r.stdout.splitlines():
                if "page size of" in line:
                    page_size = int(line.split()[-2])
                elif ":" in line:
                    key, val = line.split(":", 1)
                    val = val.strip().rstrip(".")
                    if val.isdigit():
                        pages[key.strip()] = int(val)
            avail = (pages.get("Pages free", 0)
                     + pages.get("Pages inactive", 0)
                     + pages.get("Pages purgeable", 0))
            info["mem_avail_gb"] = round(avail * page_size / (1 << 30), 1)
    elif os.path.exists("/proc/meminfo"):
        with open("/proc/meminfo") as f:
            for line in f:
                if line.startswith("MemAvailable"):
                    info["mem_avail_gb"] = round(int(line.split()[1]) / (1 << 20), 1)
                    break
    # Load average
    try:
        l1, l5, l15 = os.getloadavg()
        info["load_avg"] = [round(l1, 2), round(l5, 2), round(l15, 2)]
    except OSError:
        pass
    # Memory type + Disk info
    if sys.platform == "darwin":
        try:
            r = subprocess.run(
                ["system_profiler", "SPMemoryDataType", "SPNVMeDataType", "-json"],
                capture_output=True, text=True, timeout=10)
            if r.returncode == 0:
                sp = json.loads(r.stdout)
                # Memory: type, manufacturer
                mem = (sp.get("SPMemoryDataType") or [{}])[0]
                mem_type = mem.get("dimm_type", "")
                mem_mfr = mem.get("dimm_manufacturer", "")
                if mem_type:
                    info["mem_type"] = f"{mem_type}" + (f" ({mem_mfr})" if mem_mfr else "")
                # NVMe disk
                nvme = (sp.get("SPNVMeDataType") or [{}])[0]
                items = nvme.get("_items", [])
                if items:
                    disk = items[0]
                    info["disk"] = {
                        "model": disk.get("device_model", "?"),
                        "size": disk.get("size", "?"),
                        "protocol": "NVMe",
                        "interface": "Apple Fabric",
                    }
        except Exception:
            pass
    else:
        # Linux: memory type from DMI (best-effort, may need root)
        dmi_mem_type = Path("/sys/devices/virtual/dmi/id/memory_type")
        if dmi_mem_type.exists():
            info["mem_type"] = dmi_mem_type.read_text().strip()
        # Linux: disk info from sysfs (boot disk)
        root_dev = None
        try:
            r = subprocess.run(["findmnt", "-no", "SOURCE", "/"],
                               capture_output=True, text=True, timeout=5)
            if r.returncode == 0:
                # e.g. "/dev/nvme0n1p2" → "nvme0n1"
                src = r.stdout.strip().split("/")[-1]
                import re
                m = re.match(r"(sd[a-z]+|nvme\d+n\d+|vd[a-z]+)", src)
                if m:
                    root_dev = m.group(1)
        except Exception:
            pass
        if root_dev:
            blk = Path(f"/sys/block/{root_dev}")
            disk_info: dict = {}
            model_f = blk / "device/model"
            if model_f.exists():
                disk_info["model"] = model_f.read_text().strip()
            rot_f = blk / "queue/rotational"
            if rot_f.exists():
                disk_info["type"] = "HDD" if rot_f.read_text().strip() == "1" else "SSD"
            # NVMe link speed
            if root_dev.startswith("nvme"):
                nvme_name = root_dev.split("n")[0]  # nvme0
                nvme_sys = Path(f"/sys/class/nvme/{nvme_name}")
                transport_f = nvme_sys / "transport"
                if transport_f.exists():
                    disk_info["protocol"] = "NVMe"
                    disk_info["interface"] = transport_f.read_text().strip().upper()
                # PCI link speed
                addr_f = nvme_sys / "address"
                if addr_f.exists():
                    addr = addr_f.read_text().strip()
                    try:
                        r2 = subprocess.run(
                            ["lspci", "-vv", "-s", addr],
                            capture_output=True, text=True, timeout=5)
                        if r2.returncode == 0:
                            for line in r2.stdout.splitlines():
                                if "LnkSta:" in line and "Speed" in line:
                                    # e.g. "LnkSta: Speed 16GT/s, Width x4"
                                    parts = line.split(",")
                                    speed = ""
                                    width = ""
                                    for p in parts:
                                        p = p.strip()
                                        if "Speed" in p:
                                            speed = p.split("Speed")[-1].strip()
                                        if "Width" in p:
                                            width = p.strip()
                                    if speed:
                                        disk_info["link_speed"] = f"{speed} {width}".strip()
                                    break
                    except Exception:
                        pass
            elif root_dev.startswith("sd"):
                disk_info["protocol"] = "SATA/SAS"
            if disk_info:
                disk_info.setdefault("size", "?")
                size_f = blk / "size"
                if size_f.exists():
                    sectors = int(size_f.read_text().strip())
                    gb = sectors * 512 / (1 << 30)
                    disk_info["size"] = f"{gb:.0f} GB"
                info["disk"] = disk_info
    return info


# ── Data generation ──────────────────────────────────────────────────────────

def generate_data(r1_gz: Path, r2_gz: Path, num_pairs: int, seed: int = 2026):
    READ_LEN = 150
    BATCH = 100_000
    BASES = b"ACGT"
    QUAL_POOL = 512
    rng = random.Random(seed)

    pool = []
    for _ in range(QUAL_POOL):
        q = bytearray(READ_LEN)
        for j in range(READ_LEN):
            if j < 5 or j > READ_LEN - 10:
                q[j] = rng.randint(20, 35) + 33
            else:
                q[j] = rng.randint(30, 40) + 33
        pool.append(bytes(q))

    t0 = time.time()
    written = 0
    with gzip.open(r1_gz, "wb", compresslevel=1) as f1, \
         gzip.open(r2_gz, "wb", compresslevel=1) as f2:
        while written < num_pairs:
            n = min(BATCH, num_pairs - written)
            b1 = bytearray()
            b2 = bytearray()
            for i in range(n):
                rid = written + i + 1
                name = f"@SIM:BENCH:1:{1101 + rid // 10000000}:{rid % 50000}:{(rid * 7) % 50000}".encode()
                s1 = bytes(rng.choices(BASES, k=READ_LEN))
                s2 = bytes(rng.choices(BASES, k=READ_LEN))
                q1 = pool[rng.randint(0, QUAL_POOL - 1)]
                q2 = pool[rng.randint(0, QUAL_POOL - 1)]
                b1 += name + b" 1:N:0:ATCG\n" + s1 + b"\n+\n" + q1 + b"\n"
                b2 += name + b" 2:N:0:ATCG\n" + s2 + b"\n+\n" + q2 + b"\n"
            f1.write(bytes(b1))
            f2.write(bytes(b2))
            written += n
            if written % 1_000_000 == 0:
                e = time.time() - t0
                eta = (num_pairs - written) / (written / e)
                print(f"  {100 * written / num_pairs:5.1f}%  {written / 1e6:.0f}M pairs"
                      f"  {written / e / 1e6:.2f}M/s  ETA {eta:.0f}s", flush=True)

    print(f"  Generated in {time.time() - t0:.1f}s")


# ── Benchmark runner ─────────────────────────────────────────────────────────

def output_files(label: str, mode: str) -> list[Path]:
    """Return list of output file paths for a given label+mode."""
    mtype, _, out_fmt = parse_mode(mode)
    if mtype == "stdin":
        return []
    ext = "fq" if out_fmt == "fq" else "fq.gz"
    if mtype == "se":
        return [OUT_DIR / f"{label}_R1.{ext}"]
    return [OUT_DIR / f"{label}_R1.{ext}", OUT_DIR / f"{label}_R2.{ext}"]


def build_cmd(binary: str, mode: str, label: str, threads: int,
              r1_fq: Path, r2_fq: Path, r1_gz: Path, r2_gz: Path) -> list[str]:
    """Return argv list for the given mode."""
    inp = {"fq": r1_fq, "gz": r1_gz}
    inp2 = {"fq": r2_fq, "gz": r2_gz}
    out_ext = {"fq": "fq", "gz": "fq.gz"}

    mtype, in_fmt, out_fmt = parse_mode(mode)

    if mtype == "stdin":
        return [
            binary, "--stdin", "--stdout",
            "-j", "/dev/null", "-h", "/dev/null",
            "-w", str(threads),
        ]

    ext = out_ext[out_fmt]
    if mtype == "se":
        return [
            binary,
            "-i", str(inp[in_fmt]),
            "-o", str(OUT_DIR / f"{label}_R1.{ext}"),
            "-j", "/dev/null", "-h", "/dev/null",
            "-w", str(threads),
        ]
    # pe
    return [
        binary,
        "-i", str(inp[in_fmt]), "-I", str(inp2[in_fmt]),
        "-o", str(OUT_DIR / f"{label}_R1.{ext}"),
        "-O", str(OUT_DIR / f"{label}_R2.{ext}"),
        "-j", "/dev/null", "-h", "/dev/null",
        "-w", str(threads),
    ]


def run_once(cmd: list[str], mode: str, r1_fq: Path) -> tuple[float, int]:
    """Run a single benchmark iteration, return (wall-clock seconds, peak RSS in KB)."""
    stdin_f = None
    kwargs: dict = dict(stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    if mode == "stdin-stdout":
        stdin_f = open(r1_fq, "rb")
        kwargs["stdin"] = stdin_f

    t0 = time.perf_counter()
    proc = subprocess.Popen(cmd, **kwargs)
    _, status, rusage = os.wait4(proc.pid, 0)
    elapsed = time.perf_counter() - t0
    proc.returncode = os.waitstatus_to_exitcode(status)

    if stdin_f:
        stdin_f.close()
    if proc.returncode != 0:
        raise subprocess.CalledProcessError(proc.returncode, cmd)

    peak_kb = rusage.ru_maxrss
    if sys.platform == "darwin":
        peak_kb //= 1024  # macOS reports bytes
    return elapsed, peak_kb


def run_bench(label: str, binary: str, mode: str, runs: int, threads: int,
              r1_fq: Path, r2_fq: Path, r1_gz: Path, r2_gz: Path) -> tuple[float, int]:
    """Run benchmark N times, print per-run times, return (median time, max peak RSS KB)."""
    cmd = build_cmd(binary, mode, label, threads, r1_fq, r2_fq, r1_gz, r2_gz)
    print(f"[bench] {label} ({mode}, {runs} runs)")

    # warmup
    run_once(cmd, mode, r1_fq)

    times = []
    peak_rss = 0
    for i in range(1, runs + 1):
        # clean previous outputs
        for f in OUT_DIR.glob(f"{label}_*"):
            f.unlink()
        t, rss = run_once(cmd, mode, r1_fq)
        times.append(t)
        peak_rss = max(peak_rss, rss)
        print(f"  Run {i}: {t:.2f}s  peak={rss // 1024}MB")

    med = median(times)
    print(f"  >> Median: {med:.4f}s  peak={peak_rss // 1024}MB")
    print()
    return med, peak_rss


# ── Verification ─────────────────────────────────────────────────────────────

def verify_outputs(modes: list[str], has_baseline: bool = True) -> dict[str, dict]:
    """Verify output FASTQ content (decompress gz first). Returns per-mode verification dict."""
    if has_baseline:
        print("[verify] Computing FASTQ content md5 (decompressing gz if needed)...")
    else:
        print("[verify] Computing opt output md5 (no baseline to compare)...")
    verification: dict[str, dict] = {}

    for mode in modes:
        if mode == "stdin-stdout":
            verification[mode] = {"status": "skipped", "reason": "no output files"}
            print(f"  {mode}: skipped (no output files)")
            continue

        opt_files = output_files(f"opt_{mode}", mode)
        mode_result: dict = {"files": {}}

        if has_baseline:
            orig_files = output_files(f"orig_{mode}", mode)
            all_match = True
            for f_orig, f_opt in zip(orig_files, opt_files):
                name = f_orig.name.split("_", 2)[-1]
                h_orig = fq_md5(f_orig)
                h_opt = fq_md5(f_opt)
                match = h_orig == h_opt
                if not match:
                    all_match = False
                mode_result["files"][name] = {
                    "orig_md5": h_orig,
                    "opt_md5": h_opt,
                    "match": match,
                }
                status = "MATCH" if match else "DIFFER"
                print(f"  {mode} {name}: {status}  orig={h_orig[:12]}  opt={h_opt[:12]}")
            mode_result["status"] = "pass" if all_match else "fail"
        else:
            for f_opt in opt_files:
                name = f_opt.name.split("_", 2)[-1]
                h_opt = fq_md5(f_opt)
                mode_result["files"][name] = {"opt_md5": h_opt}
                print(f"  {mode} {name}: md5={h_opt[:12]}")
            mode_result["status"] = "ok"

        verification[mode] = mode_result

    # summary line
    n_pass = sum(1 for v in verification.values() if v.get("status") == "pass")
    n_check = sum(1 for v in verification.values() if v.get("status") in ("pass", "fail"))
    if n_check > 0:
        print(f"  Passed: {n_pass} / {n_check} modes")
    print()
    return verification


# ── Summary ──────────────────────────────────────────────────────────────────

def format_cores(si: dict) -> str:
    """Format core topology string from system info dict."""
    phys = si.get("physical_cores")
    logi = si.get("logical_cores", si.get("cpus", "?"))
    # per-type breakdown
    topo = ""
    if "cores" in si:
        parts = []
        for c in si["cores"]:
            n = c.get("logical", c.get("count", "?"))
            p = c.get("physical")
            if p and p != n:
                parts.append(f"{p}{c['type']}×2")  # HT: 8P×2 = 16 logical
            else:
                parts.append(f"{n}{c['type']}")
        topo = " + ".join(parts)
    if phys and phys != logi:
        # has hyperthreading
        if topo:
            return f"{phys}C/{logi}T ({topo})"
        return f"{phys}C/{logi}T"
    else:
        if topo:
            return f"{logi} ({topo})"
        return str(logi)


def get_version(binary: str) -> str:
    """Extract version string from fastp binary."""
    try:
        r = subprocess.run([binary, "--version"], capture_output=True, text=True, timeout=5)
        out = (r.stderr + r.stdout).strip()
        # e.g. "fastp 1.1.0 (3fc4ff9)" → "1.1.0 (3fc4ff9)"
        for line in out.splitlines():
            if "fastp" in line.lower():
                return line.strip().replace("fastp ", "")
        return out.splitlines()[0] if out else "?"
    except Exception:
        return "?"


def print_summary(report: dict):
    """Print flat markdown table from full report dict."""
    cfg = report["config"]
    results = report["modes"]
    num_pairs = cfg["pairs"]

    # header
    si = report.get("system", {})
    has_baseline = cfg.get("orig") is not None
    orig_ver = get_version(cfg["orig"]) if has_baseline else None
    opt_ver = get_version(cfg["opt"])
    cpus = cfg.get("cpus", cfg.get("threads", "?"))
    print()
    print(f"## fastp Benchmark")
    print()
    cores_str = format_cores(si)
    if "cpu" in si:
        print(f"- **CPU:** {si['cpu']}  **Cores:** {cores_str}")
    else:
        print(f"- **Cores:** {cores_str}")
    mem_parts = []
    if "mem_total_gb" in si:
        mem_parts.append(f"{si['mem_total_gb']}GB")
    if "mem_type" in si:
        mem_parts.append(si["mem_type"])
    if "mem_avail_gb" in si:
        mem_parts.append(f"{si['mem_avail_gb']}GB avail")
    if mem_parts:
        print(f"- **Mem:** {', '.join(mem_parts)}")
    if "load_avg" in si:
        l = si["load_avg"]
        print(f"- **Load:** {l[0]} (1m)  {l[1]} (5m)  {l[2]} (15m)")
    disk = si.get("disk", {})
    if disk:
        disk_parts = [disk.get("model", "?"), disk.get("size", "")]
        proto = disk.get("protocol", "")
        iface = disk.get("interface", "")
        link = disk.get("link_speed", "")
        if proto:
            conn = proto
            if iface and iface != proto:
                conn += f"/{iface}"
            if link:
                conn += f" {link}"
            disk_parts.append(conn)
        print(f"- **Disk:** {', '.join(p for p in disk_parts if p)}")
    if "os" in si:
        print(f"- **OS:** {si['os']} ({si.get('arch', '?')})")
    print(f"- **Pairs:** {num_pairs:,}  **Runs:** {cfg['runs']}  **Seed:** {cfg['seed']}")
    if has_baseline:
        print(f"- **Orig:** `{cfg['orig']}` ({orig_ver})")
    print(f"- **Opt:**  `{cfg['opt']}` ({opt_ver})")
    print()

    # build rows
    global_w = cfg.get("threads")  # compat: old JSON has global threads
    rows = []
    for mode, entry in results.items():
        w = entry.get("threads", global_w or auto_threads(mode))
        t_opt = entry["opt_median"]
        tp_opt = num_pairs / t_opt / 1e6
        mem_opt = entry.get("opt_peak_mb", 0)
        mtype = parse_mode(mode)[0]
        io_type = "PE" if mtype == "pe" else "SE"

        verify = entry.get("verify", {})
        v_status = verify.get("status", "n/a")
        v_tag = {"pass": "PASS", "fail": "FAIL", "ok": "OK"}.get(v_status, "n/a")

        if has_baseline:
            t_orig = entry["orig_median"]
            speedup = t_orig / t_opt
            tp_orig = num_pairs / t_orig / 1e6
            saved = t_orig - t_opt
            mem_orig = entry.get("orig_peak_mb", 0)
            mem_ratio = f"{mem_opt / mem_orig:.2f}x" if mem_orig else "-"
            rows.append((mode, io_type, w, t_orig, t_opt, speedup, tp_orig, tp_opt, saved,
                          mem_orig, mem_opt, mem_ratio, v_tag))
        else:
            rows.append((mode, io_type, w, t_opt, tp_opt, mem_opt, v_tag))

    # table columns depend on baseline
    if has_baseline:
        hdr = ("Mode", "Type", "W", "Orig (s)", "Opt (s)", "Speedup",
               "Orig (M/s)", "Opt (M/s)", "Saved (s)",
               "Orig MB", "Opt MB", "Mem", "Verify")
        fmt = [
            lambda r: r[0],
            lambda r: r[1],
            lambda r: str(r[2]),
            lambda r: f"{r[3]:.2f}",
            lambda r: f"{r[4]:.2f}",
            lambda r: f"{r[5]:.2f}x",
            lambda r: f"{r[6]:.2f}",
            lambda r: f"{r[7]:.2f}",
            lambda r: f"{r[8]:.2f}",
            lambda r: str(r[9]) if r[9] else "-",
            lambda r: str(r[10]) if r[10] else "-",
            lambda r: r[11],
            lambda r: r[12],
        ]
    else:
        hdr = ("Mode", "Type", "W", "Time (s)", "M/s", "MB", "Verify")
        fmt = [
            lambda r: r[0],
            lambda r: r[1],
            lambda r: str(r[2]),
            lambda r: f"{r[3]:.2f}",
            lambda r: f"{r[4]:.2f}",
            lambda r: str(r[5]) if r[5] else "-",
            lambda r: r[6],
        ]

    # compute column widths
    cols = []
    for i, h in enumerate(hdr):
        w = len(h)
        for row in rows:
            w = max(w, len(fmt[i](row)))
        cols.append(w)

    def fmt_row(cells):
        parts = []
        for i, c in enumerate(cells):
            if i == 0:
                parts.append(f" {c:<{cols[i]}} ")
            else:
                parts.append(f" {c:>{cols[i]}} ")
        return "|" + "|".join(parts) + "|"

    # header row
    print(fmt_row(hdr))
    # separator
    seps = ["-" * (cols[i] + 2) for i in range(len(hdr))]
    print("|" + "|".join(seps) + "|")
    # data rows
    for row in rows:
        cells = [fmt[i](row) for i in range(len(hdr))]
        print(fmt_row(cells))

    # failed md5 details
    any_fail = False
    for mode, entry in results.items():
        verify = entry.get("verify", {})
        if verify.get("status") == "fail":
            if not any_fail:
                print()
                print("**Verification failures:**")
                any_fail = True
            for fname, fv in verify.get("files", {}).items():
                if not fv["match"]:
                    print(f"- {mode} {fname}: orig=`{fv['orig_md5'][:16]}..` opt=`{fv['opt_md5'][:16]}..`")

    print()


# ── Main ─────────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(description="fastp end-to-end benchmark")
    parser.add_argument("--json", metavar="PATH",
                        help="Load results from JSON and print summary table (skip benchmark)")
    parser.add_argument("--merge", nargs=2, metavar=("ORIG_JSON", "OPT_JSON"),
                        help="Merge two opt-only JSONs into a comparison table")
    parser.add_argument("--mode", default="all",
                        help="Comma-separated modes: fq-fq,fq-gz,gz-fq,gz-gz,"
                             "se-fq-fq,se-fq-gz,se-gz-fq,se-gz-gz,stdin-stdout,"
                             "all,all-pe,all-se (default: all)")
    parser.add_argument("--pairs", type=int, default=10_000_000,
                        help="Number of read pairs (default: 10000000)")
    parser.add_argument("--threads", "-w", type=int, default=0,
                        help="Worker threads (default: auto, cpu_count minus reader/writer per mode)")
    parser.add_argument("--runs", type=int, default=3,
                        help="Repeat count, median reported (default: 3)")
    parser.add_argument("--seed", type=int, default=2026,
                        help="Random seed for data generation (default: 2026)")
    parser.add_argument("--orig", default=None,
                        help="Baseline binary path (default: none, opt-only mode)")
    parser.add_argument("--opt", default="/tmp/fastp_opt",
                        help="Optimized binary path (default: /tmp/fastp_opt)")
    args = parser.parse_args()

    # --- Merge mode: combine two opt-only JSONs into comparison ---
    if args.merge:
        orig_p, opt_p = Path(args.merge[0]), Path(args.merge[1])
        for p in (orig_p, opt_p):
            if not p.exists():
                print(f"File not found: {p}", file=sys.stderr)
                sys.exit(1)
        orig_report = json.loads(orig_p.read_text())
        opt_report = json.loads(opt_p.read_text())
        # Build merged report: use opt_report as base, inject orig data
        merged = {
            "system": opt_report.get("system", orig_report.get("system", {})),
            "config": {
                **opt_report["config"],
                "orig": orig_report["config"]["opt"],  # baseline was run as --opt
            },
            "modes": {},
        }
        for mode in opt_report["modes"]:
            opt_entry = opt_report["modes"][mode]
            orig_entry = orig_report["modes"].get(mode, {})
            merged["modes"][mode] = {
                "threads": opt_entry.get("threads", orig_entry.get("threads")),
                "opt_median": opt_entry["opt_median"],
                "opt_peak_mb": opt_entry.get("opt_peak_mb", 0),
                "orig_median": orig_entry.get("opt_median", 0),
                "orig_peak_mb": orig_entry.get("opt_peak_mb", 0),
                "verify": opt_entry.get("verify", {}),
            }
        print_summary(merged)
        return

    # --- JSON-only mode: load and display ---
    if args.json:
        p = Path(args.json)
        if not p.exists():
            print(f"File not found: {p}", file=sys.stderr)
            sys.exit(1)
        report = json.loads(p.read_text())
        print_summary(report)
        return

    # Expand mode aliases
    raw_modes = args.mode.split(",")
    modes = []
    for m in raw_modes:
        m = m.strip()
        if m == "all":
            modes.extend(ALL_MODES)
        elif m in MODE_ALIASES:
            modes.extend(MODE_ALIASES[m])
        else:
            modes.append(m)
    # dedupe preserving order
    seen = set()
    modes = [m for m in modes if not (m in seen or seen.add(m))]

    fastp_orig = args.orig  # None if no baseline
    fastp_opt = args.opt

    # Check binaries
    if not os.access(fastp_opt, os.X_OK):
        print(f"Missing binary: {fastp_opt}", file=sys.stderr)
        sys.exit(1)
    if fastp_orig and not os.access(fastp_orig, os.X_OK):
        print(f"Missing binary: {fastp_orig}", file=sys.stderr)
        sys.exit(1)
    has_baseline = fastp_orig is not None

    DATA_DIR.mkdir(parents=True, exist_ok=True)
    OUT_DIR.mkdir(parents=True, exist_ok=True)

    # --- Resolve per-mode thread counts ---
    threads_map: dict[str, int] = {}
    for mode in modes:
        threads_map[mode] = args.threads if args.threads > 0 else auto_threads(mode)

    # --- System info ---
    sysinfo = system_info()

    # --- Header ---
    banner("fastp End-to-End Benchmark")
    if "cpu" in sysinfo:
        print(f"  CPU:     {sysinfo['cpu']}")
    print(f"  Cores:   {format_cores(sysinfo)}")
    mem_str = f"{sysinfo.get('mem_total_gb', '?')}GB"
    if "mem_type" in sysinfo:
        mem_str += f" {sysinfo['mem_type']}"
    if "mem_avail_gb" in sysinfo:
        mem_str += f", {sysinfo['mem_avail_gb']}GB avail"
    print(f"  Memory:  {mem_str}")
    disk = sysinfo.get("disk", {})
    if disk:
        d_parts = [disk.get("model", "?"), disk.get("size", "")]
        proto = disk.get("protocol", "")
        iface = disk.get("interface", "")
        link = disk.get("link_speed", "")
        if proto:
            conn = proto
            if iface and iface != proto:
                conn += f"/{iface}"
            if link:
                conn += f" {link}"
            d_parts.append(conn)
        print(f"  Disk:    {', '.join(p for p in d_parts if p)}")
    if "load_avg" in sysinfo:
        l = sysinfo["load_avg"]
        print(f"  Load:    {l[0]} (1m)  {l[1]} (5m)  {l[2]} (15m)")
    print(f"  OS:      {sysinfo['os']} ({sysinfo['arch']})")
    print(f"  Pairs:   {args.pairs}")
    unique_w = sorted(set(threads_map.values()))
    if len(unique_w) == 1:
        print(f"  Threads: {unique_w[0]}")
    else:
        print(f"  Threads: auto (PE={threads_map.get(modes[0], unique_w[0])}"
              f", SE={threads_map.get('stdin-stdout', unique_w[-1])})")
    print(f"  Runs:    {args.runs} (median reported)")
    print(f"  Modes:   {' '.join(modes)}")
    print(f"  Seed:    {args.seed}")
    print("=" * 44)
    print()

    # --- Data files ---
    r1_gz = DATA_DIR / "bench_R1.fq.gz"
    r2_gz = DATA_DIR / "bench_R2.fq.gz"
    r1_fq = DATA_DIR / "bench_R1.fq"
    r2_fq = DATA_DIR / "bench_R2.fq"

    # Generate compressed data
    if r1_gz.exists() and r2_gz.exists():
        print("[data] Reusing existing compressed test data")
        print(f"  R1.gz: {human_size(r1_gz)}")
        print(f"  R2.gz: {human_size(r2_gz)}")
    else:
        print(f"[data] Generating {args.pairs} pairs...")
        generate_data(r1_gz, r2_gz, args.pairs, args.seed)
        print(f"  R1.gz: {human_size(r1_gz)}")
        print(f"  R2.gz: {human_size(r2_gz)}")

    # Decompress if needed
    need_fq = any(parse_mode(m)[1] == "fq" or m == "stdin-stdout" for m in modes)
    if need_fq:
        if r1_fq.exists() and r2_fq.exists():
            print("[data] Reusing existing uncompressed test data")
        else:
            print("[data] Decompressing to plain FASTQ...")
            for gz_path, fq_path in ((r1_gz, r1_fq), (r2_gz, r2_fq)):
                with gzip.open(gz_path, "rb") as fi, open(fq_path, "wb") as fo:
                    shutil.copyfileobj(fi, fo)
        print(f"  R1.fq: {human_size(r1_fq)}")
        print(f"  R2.fq: {human_size(r2_fq)}")
    print()

    # --- Cache warmup ---
    print("[cache] Warming up filesystem cache...")
    for p in (r1_gz, r2_gz):
        with open(p, "rb") as f:
            while f.read(1 << 20):
                pass
    if need_fq:
        for p in (r1_fq, r2_fq):
            with open(p, "rb") as f:
                while f.read(1 << 20):
                    pass
    print()

    # --- Run benchmarks ---
    results: dict[str, dict] = {}

    for mode in modes:
        w = threads_map[mode]
        banner(f"Mode: {mode}  (W={w})", char="-")
        print()

        if has_baseline:
            t_orig, mem_orig = run_bench(
                f"orig_{mode}", fastp_orig, mode, args.runs, w,
                r1_fq, r2_fq, r1_gz, r2_gz)
        t_opt, mem_opt = run_bench(
            f"opt_{mode}", fastp_opt, mode, args.runs, w,
            r1_fq, r2_fq, r1_gz, r2_gz)
        entry = {
            "threads": w,
            "opt_median": round(t_opt, 4),
            "opt_peak_mb": mem_opt // 1024,
        }
        if has_baseline:
            entry["orig_median"] = round(t_orig, 4)
            entry["orig_peak_mb"] = mem_orig // 1024
        results[mode] = entry

    # --- Verify ---
    verification = verify_outputs(modes, has_baseline)
    for mode in modes:
        results[mode]["verify"] = verification[mode]

    # --- Save JSON ---
    report = {
        "system": sysinfo,
        "config": {
            "pairs": args.pairs,
            "cpus": CPU_COUNT,
            "runs": args.runs,
            "seed": args.seed,
            "orig": fastp_orig,  # None if no baseline
            "opt": fastp_opt,
        },
        "modes": results,
    }
    RESULTS_JSON.write_text(json.dumps(report, indent=2) + "\n")
    print(f"[json] Results saved to {RESULTS_JSON}")
    print()

    # --- Summary ---
    print_summary(report)


if __name__ == "__main__":
    main()

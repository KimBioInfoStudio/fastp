#!/usr/bin/env bash
set -euo pipefail

# Benchmark: libdeflate single-thread vs ISA-L parallel compression
# Compares wall time AND compression ratio for .fq.gz output.
# Usage: bash scripts/bench_gz_compress.sh [threads]

THREADS=${1:-4}
RUNS=3

BENCH_DIR="/tmp/fastp_benchmark"
DATA_DIR="$BENCH_DIR/data"
OUT_DIR="$BENCH_DIR/output"
FASTP_BASELINE="/tmp/fastp_baseline"
FASTP_ISAL="/tmp/fastp_parallel_isal"

R1_GZ="$DATA_DIR/bench_R1.fq.gz"
R2_GZ="$DATA_DIR/bench_R2.fq.gz"

mkdir -p "$OUT_DIR"

for bin in "$FASTP_BASELINE" "$FASTP_ISAL"; do
    [[ -x "$bin" ]] || { echo "Missing: $bin"; exit 1; }
done
for f in "$R1_GZ" "$R2_GZ"; do
    [[ -f "$f" ]] || { echo "Missing: $f (run bench_e2e.sh first)"; exit 1; }
done

echo "============================================"
echo "  gz Compression Benchmark"
echo "============================================"
echo "  Threads:  $THREADS"
echo "  Runs:     $RUNS (median reported)"
echo "  Input:    $(du -h "$R1_GZ" | cut -f1) R1 + $(du -h "$R2_GZ" | cut -f1) R2"
echo "============================================"
echo

# Warmup
cat "$R1_GZ" "$R2_GZ" > /dev/null

BENCH_MEDIAN=""
run_bench() {
    local label=$1
    local binary=$2
    local t_file="$BENCH_DIR/times_${label}.txt"
    rm -f "$t_file"

    echo "[bench] $label ($RUNS runs)"

    for run in $(seq 1 $RUNS); do
        rm -f "$OUT_DIR/${label}_R1.fq.gz" "$OUT_DIR/${label}_R2.fq.gz"

        local t_start=$(python3 -c "import time; print(time.time())")

        "$binary" \
            -i "$R1_GZ" -I "$R2_GZ" \
            -o "$OUT_DIR/${label}_R1.fq.gz" \
            -O "$OUT_DIR/${label}_R2.fq.gz" \
            -j /dev/null -h /dev/null \
            -w "$THREADS" \
            2>/dev/null

        local t_end=$(python3 -c "import time; print(time.time())")
        local wall_s=$(python3 -c "print(f'{$t_end - $t_start:.2f}')")

        echo "$wall_s" >> "$t_file"
        echo "  Run $run: ${wall_s}s"
    done

    local median=$(sort -g "$t_file" | head -$(( (RUNS + 1) / 2 )) | tail -1)
    echo "  >> Median: ${median}s"
    echo
    BENCH_MEDIAN="$median"
}

# Run benchmarks
run_bench "baseline" "$FASTP_BASELINE"
MEDIAN_BASE="$BENCH_MEDIAN"

run_bench "isal_par" "$FASTP_ISAL"
MEDIAN_ISAL="$BENCH_MEDIAN"

# --- Compression ratio ---
echo "[compression] Comparing output sizes..."
BASE_R1=$(stat -f%z "$OUT_DIR/baseline_R1.fq.gz" 2>/dev/null || stat -c%s "$OUT_DIR/baseline_R1.fq.gz")
BASE_R2=$(stat -f%z "$OUT_DIR/baseline_R2.fq.gz" 2>/dev/null || stat -c%s "$OUT_DIR/baseline_R2.fq.gz")
ISAL_R1=$(stat -f%z "$OUT_DIR/isal_par_R1.fq.gz" 2>/dev/null || stat -c%s "$OUT_DIR/isal_par_R1.fq.gz")
ISAL_R2=$(stat -f%z "$OUT_DIR/isal_par_R2.fq.gz" 2>/dev/null || stat -c%s "$OUT_DIR/isal_par_R2.fq.gz")

# Decompressed size (for ratio calculation)
DECOMP_R1=$(gunzip -c "$OUT_DIR/baseline_R1.fq.gz" | wc -c)
DECOMP_R2=$(gunzip -c "$OUT_DIR/baseline_R2.fq.gz" | wc -c)

echo "  Baseline (libdeflate): R1=$(( BASE_R1 / 1048576 ))MB  R2=$(( BASE_R2 / 1048576 ))MB  total=$(( (BASE_R1 + BASE_R2) / 1048576 ))MB"
echo "  ISA-L parallel:        R1=$(( ISAL_R1 / 1048576 ))MB  R2=$(( ISAL_R2 / 1048576 ))MB  total=$(( (ISAL_R1 + ISAL_R2) / 1048576 ))MB"
echo

# --- Verify decompressed content ---
echo "[verify] Checking decompressed content..."
BASE_MD5=$(gunzip -c "$OUT_DIR/baseline_R1.fq.gz" | md5)
ISAL_MD5=$(gunzip -c "$OUT_DIR/isal_par_R1.fq.gz" | md5)
if [[ "$BASE_MD5" == "$ISAL_MD5" ]]; then
    echo "  R1 decompressed: IDENTICAL"
else
    echo "  R1 decompressed: DIFFERENT (WARNING)"
fi

BASE_MD5=$(gunzip -c "$OUT_DIR/baseline_R2.fq.gz" | md5)
ISAL_MD5=$(gunzip -c "$OUT_DIR/isal_par_R2.fq.gz" | md5)
if [[ "$BASE_MD5" == "$ISAL_MD5" ]]; then
    echo "  R2 decompressed: IDENTICAL"
else
    echo "  R2 decompressed: DIFFERENT (WARNING)"
fi
echo

# --- Summary ---
echo "============================================"
echo "  RESULTS"
echo "============================================"

python3 -c "
base_t = float('${MEDIAN_BASE}')
isal_t = float('${MEDIAN_ISAL}')
base_sz = $BASE_R1 + $BASE_R2
isal_sz = $ISAL_R1 + $ISAL_R2
decomp_sz = $DECOMP_R1 + $DECOMP_R2

speedup = base_t / isal_t
pct = (1 - isal_t / base_t) * 100

base_ratio = decomp_sz / base_sz
isal_ratio = decomp_sz / isal_sz
ratio_diff = (isal_sz - base_sz) / base_sz * 100

print(f'  Wall time:')
print(f'    Baseline (libdeflate):  {base_t:.2f}s')
print(f'    ISA-L parallel:         {isal_t:.2f}s')
print(f'    Speedup:                {speedup:.2f}x ({pct:+.1f}%)')
print()
print(f'  Compression:')
print(f'    Decompressed size:      {decomp_sz/1e9:.2f} GB')
print(f'    Baseline output:        {base_sz/1e6:.1f} MB  (ratio {base_ratio:.2f}x)')
print(f'    ISA-L output:           {isal_sz/1e6:.1f} MB  (ratio {isal_ratio:.2f}x)')
print(f'    Size difference:        {ratio_diff:+.1f}%')
"

echo "============================================"

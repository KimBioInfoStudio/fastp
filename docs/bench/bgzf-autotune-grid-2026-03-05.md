# BGZF Autotune Mini Grid (2026-03-05)

## Setup

- Binary: `./fastp` (`v1.1.0`, commit `7620ab4`)
- Input: `/tmp/fastp_bgzf_bench/data/bench_bgzf.fq.gz`
- Threads grid: `4, 8, 12`
- Runs per point: `3`
- Raw result JSON: `/tmp/fastp_bgzf_bench/results_bgzf.json`

## Timing Results

| threads | run1 (s) | run2 (s) | run3 (s) | median (s) | mean (s) | sd (s) | cv |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 4  | 12.9370 | 12.7154 | 12.7045 | 12.7154 | 12.7856 | 0.1071 | 0.84% |
| 8  | 12.9111 | 12.7731 | 12.7447 | 12.7731 | 12.8096 | 0.0727 | 0.57% |
| 12 | 12.8536 | 12.6964 | 12.7970 | 12.7970 | 12.7823 | 0.0650 | 0.51% |

## Observations

1. Best median in this mini-grid is `w=4` (`12.7154s`).
2. Differences among `w=4/8/12` are small (all within ~`0.65%` by median).
3. Run-to-run stability is acceptable (CV `0.51%` to `0.84%`).

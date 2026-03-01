window.BENCHMARK_DATA = {
  "lastUpdate": 1772359819228,
  "repoUrl": "https://github.com/KimBioInfoStudio/fastp",
  "entries": {
    "Benchmark": [
      {
        "commit": {
          "author": {
            "email": "kimy@nvidia.com",
            "name": "Kim Yang",
            "username": "KimBioInfoStudio"
          },
          "committer": {
            "email": "kimy@nvidia.com",
            "name": "Kim Yang",
            "username": "KimBioInfoStudio"
          },
          "distinct": true,
          "id": "6bcdc30e515e3f5f0377f4002ecc4d74af522a69",
          "message": "ci: run benchmarks on all branches with per-branch data isolation\n\nCo-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>",
          "timestamp": "2026-03-01T17:30:15+08:00",
          "tree_id": "a3eeddff2631c17f4ee799e95188451683098a0e",
          "url": "https://github.com/KimBioInfoStudio/fastp/commit/6bcdc30e515e3f5f0377f4002ecc4d74af522a69"
        },
        "date": 1772359818813,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "fastp SE150 1M ungz",
            "value": 21088,
            "unit": "ms"
          },
          {
            "name": "fastp SE150 1M gz",
            "value": 21387.6,
            "unit": "ms"
          },
          {
            "name": "fastp PE150 1M ungz",
            "value": 7555.22,
            "unit": "ms"
          },
          {
            "name": "fastp PE150 1M gz",
            "value": 8042.5,
            "unit": "ms"
          },
          {
            "name": "fastplong ONT 1M ungz",
            "value": 205060,
            "unit": "ms"
          },
          {
            "name": "fastplong ONT 1M gz",
            "value": 205942,
            "unit": "ms"
          }
        ]
      }
    ]
  }
}
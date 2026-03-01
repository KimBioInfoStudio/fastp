window.BENCHMARK_DATA = {
  "lastUpdate": 1772363733831,
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
      },
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
          "id": "d8bead7fe825d5e802ecf4c77ac99084774e0b6b",
          "message": "ci: fix runner info display and reduce fastplong bench to 100K reads\n\n- Output runner info to both stdout and job summary\n- Fix markdown table missing closing pipes\n- Reduce fastplong bench from 1M to 100K ONT reads (~38min -> ~4min)\n\nCo-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>",
          "timestamp": "2026-03-01T18:46:52+08:00",
          "tree_id": "11ab3becd27dd92c5b53998028719b4b1acdbe55",
          "url": "https://github.com/KimBioInfoStudio/fastp/commit/d8bead7fe825d5e802ecf4c77ac99084774e0b6b"
        },
        "date": 1772362513791,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "fastp SE150 1M ungz",
            "value": 21186.7,
            "unit": "ms"
          },
          {
            "name": "fastp SE150 1M gz",
            "value": 21527.9,
            "unit": "ms"
          },
          {
            "name": "fastp PE150 1M ungz",
            "value": 7516.85,
            "unit": "ms"
          },
          {
            "name": "fastp PE150 1M gz",
            "value": 7972.78,
            "unit": "ms"
          },
          {
            "name": "fastplong ONT 100K ungz",
            "value": 22177.4,
            "unit": "ms"
          },
          {
            "name": "fastplong ONT 100K gz",
            "value": 23824.1,
            "unit": "ms"
          }
        ]
      },
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
          "id": "5ffada1308fb1114e9e5ce614130c08dd1f7f286",
          "message": "ci: add release workflow to package binaries on tag push\n\nBuilds fastp and fastplong for linux-amd64 and macos-arm64,\npackages each with README and LICENSE into .tar.gz archives,\nand publishes them as GitHub release assets.\n\nCo-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>",
          "timestamp": "2026-03-01T19:07:03+08:00",
          "tree_id": "d23c1bc7039fd5e7d66d6a4ead4ed24a6c537be8",
          "url": "https://github.com/KimBioInfoStudio/fastp/commit/5ffada1308fb1114e9e5ce614130c08dd1f7f286"
        },
        "date": 1772363733297,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "fastp SE150 1M ungz",
            "value": 21148.6,
            "unit": "ms"
          },
          {
            "name": "fastp SE150 1M gz",
            "value": 21477,
            "unit": "ms"
          },
          {
            "name": "fastp PE150 1M ungz",
            "value": 7461.6,
            "unit": "ms"
          },
          {
            "name": "fastp PE150 1M gz",
            "value": 7976.85,
            "unit": "ms"
          },
          {
            "name": "fastplong ONT 100K ungz",
            "value": 21683,
            "unit": "ms"
          },
          {
            "name": "fastplong ONT 100K gz",
            "value": 24158.1,
            "unit": "ms"
          }
        ]
      }
    ]
  }
}
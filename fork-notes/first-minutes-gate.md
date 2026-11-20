<!--
SPDX-FileCopyrightText: Copyright 2026 joebenberry-afk
SPDX-License-Identifier: GPL-3.0-or-later
-->
<!-- lang: en-GB -->

# Milestone 0.5 First-120-Second Cache Gate: Analysis, Tooling, and Results

[Issue #2](https://github.com/joebenberry-afk/Eglantine/issues/2) · Milestone 0.5 (`fork-main` orientation base: `dcdc82a4d6dd02fddc72efe69595ad99aa5ab817`).

> **Status:** Live isolated two-title verification and incompatible-cache rejection checks completed on candidate `Eglantine-w23-s54-verified` (`b5aef6b73b`). **Milestone 0.5 remains OPEN** because attribution is incomplete (`lost_events > 0`) on `b5aef6b73b` and XC2 also missed halving on the earlier `0.5.0-pre6` capture. Incompatible cache rejection **PASSED** on both titles.

## 1. Source & Harness Findings

1. **`fork-main` (`dcdc82a4d6`) does not contain the 0.5 cache-envelope or attribution code.** Schema-2 long-frame attribution (`src/video_core/nx2_attribution.cpp`), versioned cache envelopes (`src/video_core/cache_envelope.{cpp,h}`), pipeline pre-preparation (`src/video_core/pipeline_prepare.{cpp,h}`), and schema-2 JSON reporting (`src/yuzu_cmd/report_schema2.cpp`) live in `fork-0.5` (`0745682560`) and the s54 candidate (`b5aef6b73b` + local changes in `build/w23-tmp/wt`, packaged as `Eglantine-w23-s54-verified`).
2. **Candidate unit checks pass (`build/w23-ucrt64/bin/tests.exe`):**
   - `[pipeline_prepare],[nx2][attribution],CacheEnvelope*,PipelineRecord*`: **116 test cases, 1,759,551 assertions passed**.
   - `[report][schema2]`: **12 test cases, 294 assertions passed**.
3. **Harness gap fixed in `tools/first_minutes_gate/`:**
   - On `fork-0.5` (`0745682560`), `GetVulkanPipelineCacheDir` writes directly under `user/cache/shader/<tid>/`.
   - On `rel/0.6.0` (`03e9b17718`) and `b5aef6b73b` (`w23-s54`), `GetVulkanPipelineCacheDir` appends `VulkanSemanticCacheRevision` (`semantics_r3` / `semantics_r6`, `src/common/fs/path_util.cpp:331-337`).
   - `src/fe-lab/tools/first_minutes.py` (`cache_dir()`) inspects only `user/cache/shader/<tid>/`, missing `semantics_r6/`. `tools/first_minutes_gate/first_minutes_gate.py` (`discover_title_cache_dirs`) discovers both `<tid>/` and `<tid>/semantics_r*/`.

## 2. Live Two-Title Pairs & Cache Rejection on Candidate `b5aef6b73b`

Measured in isolated `$env:TEMP` `--user-dir` instances (seed config/keys/saves only, empty initial `cache/`) using `Eglantine-w23-s54-verified\eden-cli.exe` (`sha256: 03edae9b...`, `--baseline` v5 `c564d93a...`), `120 s` anchored at `first_real_host_present`, cleanly stopped via `--control stop` (`exit.code=0`, `metrics_complete=true`, `shutdown_complete=true`). Sanitized summary in [first_minutes_live_s54_summary.json](first_minutes_live_s54_summary.json).

| Title (`b5aef6b73b`, `n=1`) | Raw `>100 ms` (cold → 2nd) | Attributed count | Attribution completeness | Cache stats (2nd launch) | `format_version` (`999`) reject | `legacy_format` reject | Gate verdict |
|---|---|---|---|---|---|---|---|
| Fire Emblem: Fortune's Weave | `5 -> 4` (`3453`/`3529` frames; max `2260.3 -> 1700.9 ms`) | `INCOMPLETE` | **`complete: false`** (`lost_events: 154,962 -> 154,682`; `98.3% -> 98.9%` unknown) | `prepared=99`, `adopted=69`, `recorded=30` | **PASS** (`exit=0`, `rejected`, `Nx2Cache reject`) | **PASS** (`exit=0`, `rejected`, `Nx2Cache reject`) | **`INCOMPLETE`** |
| Xenoblade Chronicles 2 | `12 -> 13` (`6876`/`6894` frames; max `1096.7 -> 1371.1 ms`) | `INCOMPLETE` | **`complete: false`** (`lost_events: 99,065 -> 99,518`; `96.1% -> 96.6%` unknown) | `prepared=148`, `adopted=36`, `evicted=112`, `budget_pauses=2`, `recorded=79` | **PASS** (`exit=0`, `rejected`, `Nx2Cache reject`) | **PASS** (`exit=0`, `rejected`, `Nx2Cache reject`) | **`INCOMPLETE`** |

**Root cause of incomplete attribution on `b5aef6b73b`:** `b5aef6b73b` branched from `84b55b0a64`, before `fork-0.5` (`0745682560`) landed `753ed73b1e` ("Take the attribution rings in rounds so a starved consumer loses no history") and `b8afe4c85d`. During startup on both titles, the 16 MiB `Nx2Attribution` ring overflows (`Nx2Attribution lost events (buffer of 16777216 bytes)`), leaving `attribution.complete=false`.

## 3. Offline Re-judgement of Existing Wave-13 `0.5.0-pre6` Captures

Re-judging the existing single-pair `0.5.0-pre6` captures (`wave13-pair-fe`, `wave13-pair-xc2`, where attribution *was* complete with `lost_events=0` and `0%` unknown) using `tools/first_minutes_gate/first_minutes_gate.py score-pair`:

| Title (`0.5.0-pre6`, `n=1`) | Raw `>100 ms` (cold → 2nd) | Emulator-attributed (cold → 2nd) | Game gaps (cold → 2nd) | Halving verdict | Largest residual cause (2nd launch) |
|---|---|---|---|---|---|
| Fire Emblem: Fortune's Weave (`FE-1`) | `16 -> 16` | `6 -> 2` (`0.33`) | `10 -> 14` | `PASS` (warning: `cold < 10`, `n=1`) | `n/a` |
| Xenoblade Chronicles 2 (`XC2-field`) | `10 -> 12` | `1 -> 1` (`1.00`) | `9 -> 11` | **`FAIL`** (`2 * 1 > 1`) | `texture` (`196.6 ms` in frame 11: `329.006 ms` total, `263.783 ms` emulator; unclassified `producer_unknown_wait` = `3,199.182 ms`) |

## 4. Open Blockers Keeping Milestone 0.5 Open

1. **Attribution ring-round fix missing from `b5aef6b73b`:** Port `753ed73b1e` / `b8afe4c85d` (from `fork-0.5` `0745682560`) onto the active candidate branch before re-running paired 120s attribution counts.
2. **XC2 second-launch residual texture work and preparation budget:** On `0.5.0-pre6`, XC2 missed halving (`1 -> 1`) with `texture` (`196.6 ms`) as the largest residual cause; on `b5aef6b73b`, XC2 second launch evicted `112` of `148` prepared pipelines (`adopted=36`, `budget_pauses=2`) with raw `12 -> 13`.
3. **Incompatible cache rejection:** Already verified passing on both titles for both `format_version` (`999`) and `legacy_format` (`yuzucach`) under `semantics_r6/vulkan.mythos.bin`.

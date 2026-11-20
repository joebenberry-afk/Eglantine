<!--
SPDX-FileCopyrightText: Copyright 2026 joebenberry-afk
SPDX-License-Identifier: GPL-3.0-or-later
-->
<!-- lang: en-GB -->

# Milestone 0.5 First-120-Second Cache Gate: Analysis, Tooling, and Execution Plan

[Issue #2](https://github.com/joebenberry-afk/Eglantine/issues/2) · Milestone 0.5 (`fork-main` orientation base: `dcdc82a4d6dd02fddc72efe69595ad99aa5ab817`).

> **Status:** Offline analysis, unit verification, and portable gate harness (`tools/first_minutes_gate/first_minutes_gate.py`) are ready. Live GPU reproduction runs are **paused** pending `@coordinator` GPU and isolated `--user-dir` reservation.

## 1. Source & Build Findings

1. **`fork-main` (`dcdc82a4d6`) does not contain the 0.5 cache-envelope or attribution code.** Schema-2 long-frame attribution (`src/video_core/nx2_attribution.cpp`), versioned cache envelopes (`src/video_core/cache_envelope.{cpp,h}`), pipeline pre-preparation (`src/video_core/pipeline_prepare.{cpp,h}`), and schema-2 JSON reporting (`src/yuzu_cmd/report_schema2.cpp`) live in `fork-0.5` (`0745682560`) and the s54 candidate (`b5aef6b73b` + local changes in `build/w23-tmp/wt`, packaged as `Eglantine-w23-s54-verified`).
2. **Candidate unit checks pass (`build/w23-ucrt64/bin/tests.exe`):**
   - `[pipeline_prepare],[nx2][attribution],CacheEnvelope*,PipelineRecord*`: **116 test cases, 1,759,551 assertions passed**.
   - `[report][schema2]`: **12 test cases, 294 assertions passed**.
3. **Critical harness gap discovered in `src/fe-lab/tools/first_minutes.py`:**
   - On `fork-0.5` (`0745682560`), `GetVulkanPipelineCacheDir` writes directly under `user/cache/shader/<tid>/`.
   - On `rel/0.6.0` (`03e9b17718`) and the s54 candidate (`build/w23-tmp/wt`), `GetVulkanPipelineCacheDir` appends `VulkanSemanticCacheRevision` (`semantics_r3` on `0.6.0`, `semantics_r6` on `w23-s54`, `src/common/fs/path_util.cpp:331-337`).
   - `first_minutes.py` (`cache_dir()`) inspects only `user/cache/shader/<tid>/`, so running it unmodified against `Eglantine-w23-s54-verified` would neither isolate `semantics_r6/vulkan*.mythos.bin` for a cold run nor plant `reject-test` files where the emulator loads them.
   - `tools/first_minutes_gate/first_minutes_gate.py` (`discover_title_cache_dirs` / `find_title_cache_files`) explicitly handles both `<tid>/` and `<tid>/semantics_r*/`.

## 2. Offline Re-judgement of Existing Wave-13 0.5 Captures

Re-judging the existing single-pair `0.5.0-pre6` captures (`wave13-pair-fe`, `wave13-pair-xc2`) with `first_minutes.py` v2's `EMULATOR_CAUSES` rule (`code_translation`, `shader`, `pipeline`, `texture`, `decompression`, `disk`; remainder `<= 100 ms` counted, `> 100 ms` remainder treated as game gap, `unknown <= 10%` per frame):

| Title (`0.5.0-pre6`, 1 pair) | Raw `>100 ms` (cold → 2nd) | Emulator-attributed (cold → 2nd) | Game gaps (cold → 2nd) | Halving verdict | Largest residual cause (2nd launch) |
|---|---|---|---|---|---|
| Fire Emblem: Fortune's Weave (`FE-1`) | `16 -> 16` | `6 -> 2` (`0.33`) | `10 -> 14` | `PASS` (warning: `cold < 10`, `n=1`) | `n/a` |
| Xenoblade Chronicles 2 (`XC2-field`) | `10 -> 12` | `1 -> 1` (`1.00`) | `9 -> 11` | **`FAIL`** (`2 * 1 > 1`) | `texture` (`196.6 ms` in frame 11: `329.006 ms` total, `263.783 ms` emulator; unclassified `producer_unknown_wait` = `3,199.182 ms`) |

And `wave7run05e-reject-{fe,xc2}` passed both `legacy_raw_cache` (`yuzucach`) and `schema2_envelope` (`EGL05CCH` version `999`) rejection checks (`exit 0`, `cache.files[]` rejected, `Nx2Cache reject` logged).

**Gap:** No multi-pair (`>= 3` pairs) gate run exists on `fork-0.5` (`0745682560`) or the s54 candidate (`Eglantine-w23-s54-verified`), and XC2 failed on `0.5.0-pre6` due to residual `texture` work on frame 11; therefore Milestone 0.5 remains **open**.

## 3. Live GPU Execution Plan (Waiting on `@coordinator` Reservation)

Once `@coordinator` reserves the GPU and an isolated `--user-dir` (never touching shared writable user dirs):

1. **One build, default baseline:** Pin `Eglantine-w23-s54-verified/eden-cli.exe` (`sha256: 03edae9b...`, `baseline_version=5`) and/or a clean build of `fork-0.5` (`0745682560`), passing `--baseline --user-dir <isolated>`.
2. **Paired cold vs. second launch (FE:FW and XC2):**
   - Fresh isolated `--user-dir` per pair; cold launch removes `<tid>/` and `<tid>/semantics_r*/` transferable/mesh/used/JIT/warm-compute files (preserving `vulkan_static_pipelines.bin` and noting NVIDIA driver cache read-only state).
   - Anchor `120 s` at `startup.first_host_present_ns`; verify identical scene, inputs, baseline hash, completeness, and `unknown <= 10%`.
   - Score with `tools/first_minutes_gate/first_minutes_gate.py score-pair` and `score_gate`, reporting raw `>100 ms` counts beside emulator-attributed counts, game gaps, and any unclassified intervals.
3. **Incompatible cache rejection:** Stamp a 160-byte `EGL05CCH` v999 header (`make-reject-header`) and legacy `yuzucach` header into `<user-dir>/cache/shader/<tid>/[semantics_r6/]vulkan.mythos.bin`, run with `--report`, and verify `check-reject` (`exit 0`, `cache.files[]`, `Nx2Cache reject`).
4. **Gate decision:** Close 0.5 only if **both** FE:FW and XC2 pass `2 * second_median <= cold_median` (`cold_median > 0`) and attribution completeness plus cache rejection; if either misses, record its largest residual cause and keep 0.5 open.

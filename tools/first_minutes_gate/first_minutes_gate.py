#!/usr/bin/env python3
"""Portable 0.5 first-120-second cache gate scorer, cache-dir helper, and rejection verifier.

Implements the Issue #2 (Milestone 0.5) acceptance contract using Python 3 standard library only:
- Anchors (0, 120 s] at startup.first_host_present_ns on schema-2 run reports.
- Reports raw >100 ms real-frame counts beside emulator-attributed counts and game-gap counts.
- Removes attributable emulator time (code_translation, shader, pipeline, texture, decompression,
  disk); frames with remainder <= 100 ms count as emulator-caused, >100 ms remainders are game
  gaps, and missing/incomplete/high-Unknown (>10%) attribution is INVALID.
- Checks 2 * second_attributed <= cold_attributed (cold == 0 -> NOTHING TO HALVE), verifies
  baseline parity between halves, and names the largest residual emulator cause on any miss.
- Discovers both legacy <tid>/ and versioned <tid>/semantics_r*/ Vulkan cache directories so
  isolated cold runs and wrong-version cache rejection checks work across 0.5 and 0.6+/s54 trees.
"""

from __future__ import annotations

import argparse
import collections
import json
import os
import re
import statistics
import struct
import sys
from pathlib import Path
from typing import Any

WINDOW_NS = 120 * 1_000_000_000
LONG_FRAME_NS = 100 * 1_000_000
UNKNOWN_SHARE_LIMIT_PERMILLE = 100  # 10% per long frame

ALL_CAUSES = (
    "disk",
    "shader",
    "pipeline",
    "texture",
    "decompression",
    "waiting",
    "code_translation",
    "unknown",
)
EMULATOR_CAUSES = (
    "code_translation",
    "shader",
    "pipeline",
    "texture",
    "decompression",
    "disk",
)

CACHE_ENVELOPE_MAGIC = b"EGL05CCH"
CACHE_ENVELOPE_BYTES = 160
CACHE_VERSION_OFFSET = 8
LEGACY_MAGICS = (b"yuzucach", b"yuzuvkch")
CACHE_STEMS = (
    "vulkan",
    "vulkan_pipelines",
    "vulkan_pipelines.unoptimized",
    "vulkan_used",
    "vulkan_mesh",
    "jit_blocks",
)
CACHE_SUFFIXES = (".mythos.bin", ".bin")
WARM_FILES = ("warm-compute.bin", "warm-compute.count", "warm-compute.done")

FAILURE_EXIT_REASONS = frozenset(
    (
        "host_crash",
        "timeout",
        "report_io_error",
        "guest_crash",
        "device_lost",
        "shader_failure",
        "startup_error",
    )
)

REJECT_LOG_PATTERNS = {
    "format_version": re.compile(
        r"Nx2Cache reject kind=transferable reason=format_version "
        r"expected=(\d+) found=(\d+) action=ignore"
    ),
    "legacy_format": re.compile(
        r"Nx2Cache reject kind=transferable reason=legacy_format "
        r"expected=[0-9a-f]+ found=[0-9a-f]+ action=backup_legacy"
    ),
}


def build_synthetic_envelope(bad_version: int = 999) -> bytes:
    """Return a 160-byte schema-2 cache header stamped with an incompatible envelope version."""
    header = bytearray(CACHE_ENVELOPE_BYTES)
    header[: len(CACHE_ENVELOPE_MAGIC)] = CACHE_ENVELOPE_MAGIC
    struct.pack_into("<I", header, CACHE_VERSION_OFFSET, bad_version)
    return bytes(header)


def stamp_rejected_envelope(original: bytes, bad_version: int = 999) -> bytes | None:
    """Return a copy of an EGL05CCH cache with its envelope version stamped to bad_version."""
    if len(original) < CACHE_ENVELOPE_BYTES or not original.startswith(CACHE_ENVELOPE_MAGIC):
        return None
    mutated = bytearray(original)
    struct.pack_into("<I", mutated, CACHE_VERSION_OFFSET, bad_version)
    return bytes(mutated)


def discover_title_cache_dirs(user_dir: str | Path, title_id: str) -> list[Path]:
    """Return existing per-title shader cache dirs, including semantics_r* subdirectories."""
    tid = title_id.strip().lower()
    if tid.startswith("0x"):
        tid = tid[2:]
    base = Path(user_dir) / "cache" / "shader" / tid
    dirs: list[Path] = []
    if base.is_dir():
        dirs.append(base)
        for child in sorted(base.iterdir()):
            if child.is_dir() and child.name.startswith("semantics_r"):
                dirs.append(child)
    return dirs


def find_title_cache_files(user_dir: str | Path, title_id: str) -> list[Path]:
    """Find transferable pipeline, mesh, used, JIT-hint, and warm-compute files for a title."""
    expected_names = {stem + suffix for stem in CACHE_STEMS for suffix in CACHE_SUFFIXES}
    expected_names.update(WARM_FILES)
    found: list[Path] = []
    for folder in discover_title_cache_dirs(user_dir, title_id):
        for entry in sorted(folder.iterdir()):
            if entry.is_file() and entry.name in expected_names:
                found.append(entry)
    return found


def _baseline_summary(doc: dict[str, Any]) -> dict[str, Any]:
    base = doc.get("baseline")
    if not isinstance(base, dict):
        return {"present": False, "on_baseline": None, "version": None, "hash": None}
    return {
        "present": True,
        "on_baseline": base.get("on_baseline"),
        "version": base.get("version"),
        "hash": base.get("hash"),
        "deviations": base.get("deviations") or [],
    }


def _build_summary(doc: dict[str, Any]) -> dict[str, Any]:
    build = doc.get("build")
    if not isinstance(build, dict):
        return {"revision": None, "build_id": None}
    return {
        "revision": build.get("revision"),
        "build_id": build.get("build_id"),
    }


def score_report(report_path: str | Path) -> dict[str, Any]:
    """Validate and score a single schema-2 run report against the 0.5 attribution rule."""
    path = Path(report_path)
    try:
        doc = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        return {"valid": False, "reason": f"unreadable report: {exc}", "path": str(path)}

    if doc.get("schema_version") != 2:
        return {
            "valid": False,
            "reason": f"schema_version={doc.get('schema_version')!r} (requires 2)",
            "path": str(path),
        }

    exit_block = doc.get("exit")
    if exit_block is not None:
        if not isinstance(exit_block, dict):
            return {"valid": False, "reason": "malformed exit block", "path": str(path)}
        exit_code = exit_block.get("code")
        if exit_code not in (0, None):
            return {"valid": False, "reason": f"report exit code {exit_code!r}", "path": str(path)}
        exit_reason = exit_block.get("reason")
        if exit_reason in FAILURE_EXIT_REASONS:
            return {"valid": False, "reason": f"report exit reason {exit_reason!r}", "path": str(path)}
        if exit_block.get("metrics_complete") is False:
            return {"valid": False, "reason": "report exit.metrics_complete is false", "path": str(path)}
        if exit_block.get("shutdown_complete") is False:
            return {"valid": False, "reason": "report exit.shutdown_complete is false", "path": str(path)}

    startup = doc.get("startup")
    first_120s = doc.get("first_120s")
    attribution = doc.get("attribution")
    if not isinstance(startup, dict) or not isinstance(first_120s, dict) or not isinstance(attribution, dict):
        return {"valid": False, "reason": "missing startup, first_120s, or attribution block", "path": str(path)}

    p0 = startup.get("first_host_present_ns")
    if not isinstance(p0, int) or p0 <= 0:
        return {"valid": False, "reason": "invalid startup.first_host_present_ns", "path": str(path)}

    if first_120s.get("complete") is not True:
        return {"valid": False, "reason": "first_120s window is incomplete", "path": str(path)}

    if attribution.get("enabled") is not True:
        return {"valid": False, "reason": "attribution is disabled", "path": str(path)}
    if attribution.get("complete") is not True:
        return {"valid": False, "reason": "attribution is incomplete", "path": str(path)}

    for counter in ("lost_events", "dropped_boundaries", "sink_write_errors"):
        val = attribution.get(counter, 0)
        if not isinstance(val, int) or val != 0:
            return {"valid": False, "reason": f"attribution.{counter}={val!r} (requires 0)", "path": str(path)}

    long_frames = attribution.get("long_frames")
    if not isinstance(long_frames, list):
        return {"valid": False, "reason": "attribution.long_frames missing", "path": str(path)}

    strict_count = first_120s.get("frames_over_100ms")
    window_frames = []
    for frame in long_frames:
        if not isinstance(frame, dict):
            return {"valid": False, "reason": "malformed long_frame entry", "path": str(path)}
        end_ns = frame.get("end_ns")
        if not isinstance(end_ns, int):
            return {"valid": False, "reason": "long_frame missing end_ns", "path": str(path)}
        if p0 < end_ns <= p0 + WINDOW_NS:
            window_frames.append(frame)

    if len(window_frames) != strict_count:
        return {
            "valid": False,
            "reason": f"first_120s.frames_over_100ms={strict_count} != window_frames={len(window_frames)}",
            "path": str(path),
        }

    emulator_frames = []
    game_gap_frames = []
    cause_totals_ns = {c: 0 for c in ALL_CAUSES}
    emulator_cause_totals_ns = {c: 0 for c in EMULATOR_CAUSES}
    worst_unknown_permille = 0

    for frame in window_frames:
        if frame.get("instrumentation_complete") is False:
            return {"valid": False, "reason": f"frame {frame.get('id')} instrumentation_complete=false", "path": str(path)}
        dur = frame.get("duration_ns")
        causes = frame.get("cause_ns")
        if not isinstance(dur, int) or dur <= LONG_FRAME_NS or not isinstance(causes, dict):
            return {"valid": False, "reason": f"frame {frame.get('id')} missing valid duration_ns/cause_ns", "path": str(path)}

        cause_sum = 0
        for key in ALL_CAUSES:
            val = causes.get(key)
            if not isinstance(val, int) or val < 0:
                return {"valid": False, "reason": f"frame {frame.get('id')} invalid cause {key}={val!r}", "path": str(path)}
            cause_sum += val
            cause_totals_ns[key] += val

        if cause_sum != dur:
            return {
                "valid": False,
                "reason": f"frame {frame.get('id')} causes sum {cause_sum} != duration_ns {dur}",
                "path": str(path),
            }

        unknown_ns = causes["unknown"]
        unknown_permille = (unknown_ns * 1000) // dur
        if unknown_permille > worst_unknown_permille:
            worst_unknown_permille = unknown_permille
        if unknown_permille > UNKNOWN_SHARE_LIMIT_PERMILLE:
            return {
                "valid": False,
                "reason": f"frame {frame.get('id')} unknown share {unknown_permille}/1000 > {UNKNOWN_SHARE_LIMIT_PERMILLE}/1000",
                "path": str(path),
            }

        emu_ns = sum(causes[c] for c in EMULATOR_CAUSES)
        remainder_ns = dur - emu_ns
        entry = {
            "id": frame.get("id"),
            "duration_ms": round(dur / 1_000_000.0, 3),
            "emulator_ms": round(emu_ns / 1_000_000.0, 3),
            "remainder_ms": round(remainder_ns / 1_000_000.0, 3),
            "emulator_causes_ms": {c: round(causes[c] / 1_000_000.0, 3) for c in EMULATOR_CAUSES if causes[c] > 0},
        }
        if remainder_ns <= LONG_FRAME_NS:
            emulator_frames.append(entry)
            for c in EMULATOR_CAUSES:
                emulator_cause_totals_ns[c] += causes[c]
        else:
            game_gap_frames.append(entry)

    unclassified_intervals = []
    for interval_name in ("process_to_first_present", "first_present_to_interactive"):
        blk = startup.get(interval_name)
        if isinstance(blk, dict) and blk.get("status") != "complete":
            unclassified_intervals.append({"interval": interval_name, "status": blk.get("status")})
    prod_unk = attribution.get("producer_unknown_wait_ns") or 0
    if isinstance(prod_unk, int) and prod_unk > 0:
        unclassified_intervals.append({"interval": "producer_unknown_wait", "ns": prod_unk})

    return {
        "valid": True,
        "reason": None,
        "path": str(path),
        "raw_count": strict_count,
        "emulator_attributed_count": len(emulator_frames),
        "game_gap_count": len(game_gap_frames),
        "emulator_frames": emulator_frames,
        "game_gap_frames": game_gap_frames,
        "cause_totals_ms": {c: round(v / 1_000_000.0, 3) for c, v in cause_totals_ns.items()},
        "emulator_attributed_cause_totals_ms": {
            c: round(v / 1_000_000.0, 3) for c, v in emulator_cause_totals_ns.items()
        },
        "worst_unknown_permille": worst_unknown_permille,
        "unclassified_intervals": unclassified_intervals,
        "baseline": _baseline_summary(doc),
        "build": _build_summary(doc),
        "title_id": doc.get("title_id"),
        "exit_code": (doc.get("exit") or {}).get("code"),
    }


def largest_residual_cause(scored_second: dict[str, Any]) -> dict[str, Any] | None:
    """Identify the dominant emulator cause across emulator-attributed long frames in second launch."""
    totals = scored_second.get("emulator_attributed_cause_totals_ms") or {}
    nonzero = [(cause, ms) for cause, ms in totals.items() if ms > 0]
    if not nonzero:
        return None
    cause, ms = max(nonzero, key=lambda item: item[1])
    return {"cause": cause, "total_ms": ms}


def score_pair(
    cold_report: str | Path,
    second_report: str | Path,
    *,
    label: str = "pair",
    require_baseline: bool = True,
) -> dict[str, Any]:
    """Score one paired (cold vs. second launch) 0.5 gate run."""
    cold = score_report(cold_report)
    second = score_report(second_report)
    reasons: list[str] = []
    if not cold["valid"]:
        reasons.append(f"cold invalid: {cold['reason']}")
    if not second["valid"]:
        reasons.append(f"second invalid: {second['reason']}")

    if require_baseline and cold["valid"] and second["valid"]:
        cb, sb = cold["baseline"], second["baseline"]
        if not cb["present"] or not sb["present"]:
            reasons.append("missing baseline block in report")
        else:
            if cb["on_baseline"] is not True or sb["on_baseline"] is not True:
                reasons.append("run is not on default baseline (on_baseline != true)")
            if cb["version"] != sb["version"] or cb["hash"] != sb["hash"]:
                reasons.append(
                    f"baseline mismatch: cold=v{cb['version']}:{cb['hash']} vs "
                    f"second=v{sb['version']}:{sb['hash']}"
                )

    if cold["valid"] and second["valid"]:
        ctid, stid = cold.get("title_id"), second.get("title_id")
        if ctid and stid and str(ctid).lower() != str(stid).lower():
            reasons.append(f"title_id mismatch: cold={ctid} vs second={stid}")
        cbuild, sbuild = cold.get("build") or {}, second.get("build") or {}
        for field in ("revision", "build_id"):
            cval, sval = cbuild.get(field), sbuild.get(field)
            if cval and sval and cval != sval:
                reasons.append(f"build.{field} mismatch: cold={cval} vs second={sval}")

    if reasons:
        return {
            "label": label,
            "verdict": "INCOMPLETE",
            "reasons": reasons,
            "cold": cold,
            "second": second,
            "halved": False,
            "ratio": None,
            "largest_residual_cause": None,
        }

    c_cnt = cold["emulator_attributed_count"]
    s_cnt = second["emulator_attributed_count"]
    if c_cnt == 0:
        return {
            "label": label,
            "verdict": "NOTHING_TO_HALVE",
            "reasons": ["cold emulator-attributed count is 0; nothing to halve"],
            "cold": cold,
            "second": second,
            "halved": False,
            "ratio": None,
            "largest_residual_cause": largest_residual_cause(second),
        }

    halved = 2 * s_cnt <= c_cnt
    ratio = round(s_cnt / c_cnt, 4)
    residual = largest_residual_cause(second) if not halved else None
    out_reasons = [] if halved else [f"2 * second ({2 * s_cnt}) > cold ({c_cnt})"]
    return {
        "label": label,
        "verdict": "PASS" if halved else "FAIL",
        "reasons": out_reasons,
        "cold": cold,
        "second": second,
        "halved": halved,
        "ratio": ratio,
        "largest_residual_cause": residual,
    }


def score_gate(pairs_by_title: dict[str, list[dict[str, Any]]]) -> dict[str, Any]:
    """Aggregate paired results across titles (e.g. title_a and title_b) using medians."""
    title_summaries: dict[str, Any] = {}
    all_pass = True
    any_incomplete = False

    for title, pairs in pairs_by_title.items():
        valid_pairs = [p for p in pairs if p["verdict"] in ("PASS", "FAIL", "NOTHING_TO_HALVE")]
        if not valid_pairs or len(valid_pairs) != len(pairs):
            any_incomplete = True
            all_pass = False
            title_summaries[title] = {
                "verdict": "INCOMPLETE",
                "valid_pairs": len(valid_pairs),
                "total_pairs": len(pairs),
                "cold_median": None,
                "second_median": None,
                "ratio": None,
                "milestone_open": True,
            }
            continue

        c_vals = [p["cold"]["emulator_attributed_count"] for p in valid_pairs]
        s_vals = [p["second"]["emulator_attributed_count"] for p in valid_pairs]
        c_med = float(statistics.median(c_vals))
        s_med = float(statistics.median(s_vals))
        if c_med == 0.0:
            verdict = "NOTHING_TO_HALVE"
            ratio = None
            all_pass = False
        elif 2.0 * s_med <= c_med:
            verdict = "PASS"
            ratio = round(s_med / c_med, 4)
        else:
            verdict = "FAIL"
            ratio = round(s_med / c_med, 4)
            all_pass = False

        residuals = collections.Counter()
        for p in valid_pairs:
            res = p.get("largest_residual_cause")
            if res:
                residuals[res["cause"]] += 1

        title_summaries[title] = {
            "verdict": verdict,
            "valid_pairs": len(valid_pairs),
            "total_pairs": len(pairs),
            "cold_median": c_med,
            "second_median": s_med,
            "raw_cold_counts": [p["cold"]["raw_count"] for p in valid_pairs],
            "raw_second_counts": [p["second"]["raw_count"] for p in valid_pairs],
            "attributed_cold_counts": c_vals,
            "attributed_second_counts": s_vals,
            "ratio": ratio,
            "residual_causes": dict(residuals),
            "milestone_open": verdict != "PASS",
        }

    overall = "INCOMPLETE" if any_incomplete else ("PASS" if all_pass and title_summaries else "FAIL")
    return {
        "verdict": overall,
        "milestone_open": overall != "PASS",
        "titles": title_summaries,
    }


def verify_cache_rejection(
    report_path: str | Path,
    log_path: str | Path,
    *,
    expected_reason: str = "format_version",
    expected_version: int = 1,
    found_version: int = 999,
) -> dict[str, Any]:
    """Verify clean startup rejection of an incompatible transferable cache file."""
    rpath = Path(report_path)
    lpath = Path(log_path)
    reasons: list[str] = []

    try:
        doc = json.loads(rpath.read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        return {"verdict": "FAIL", "reasons": [f"unreadable report: {exc}"]}

    if (doc.get("exit") or {}).get("code") != 0:
        reasons.append(f"exit.code={(doc.get('exit') or {}).get('code')!r} (expected 0)")

    files = ((doc.get("cache") or {}).get("files")) or []
    matched_entry = None
    for item in files:
        if (
            isinstance(item, dict)
            and item.get("kind") == "transferable"
            and item.get("status") == "rejected"
            and item.get("reason") == expected_reason
        ):
            if expected_reason == "format_version":
                if item.get("expected") == expected_version and item.get("found") == found_version:
                    matched_entry = item
                    break
            else:
                matched_entry = item
                break

    if matched_entry is None:
        reasons.append(f"missing matching cache.files[] rejection entry for reason={expected_reason}")

    try:
        log_text = lpath.read_text(encoding="utf-8", errors="replace")
    except OSError as exc:
        reasons.append(f"unreadable log: {exc}")
        log_text = ""

    pat = REJECT_LOG_PATTERNS.get(expected_reason)
    log_ok = False
    if pat and log_text:
        for m in pat.finditer(log_text):
            if expected_reason != "format_version" or (
                int(m.group(1)) == expected_version and int(m.group(2)) == found_version
            ):
                log_ok = True
                break
    if not log_ok:
        reasons.append(f"missing Nx2Cache reject log line for reason={expected_reason}")

    return {
        "verdict": "PASS" if not reasons else "FAIL",
        "reasons": reasons,
        "matched_entry": matched_entry,
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="cmd", required=True)

    p_pair = sub.add_parser("score-pair", help="Score one cold/second schema-2 report pair")
    p_pair.add_argument("--cold", required=True)
    p_pair.add_argument("--second", required=True)
    p_pair.add_argument("--label", default="pair")
    p_pair.add_argument("--no-baseline", action="store_true")

    p_rej = sub.add_parser("check-reject", help="Verify incompatible-cache rejection report + log")
    p_rej.add_argument("--report", required=True)
    p_rej.add_argument("--log", required=True)
    p_rej.add_argument("--reason", choices=("format_version", "legacy_format"), default="format_version")
    p_rej.add_argument("--expected-version", type=int, default=1)
    p_rej.add_argument("--found-version", type=int, default=999)

    p_disc = sub.add_parser("discover-caches", help="List title cache dirs including semantics_r*")
    p_disc.add_argument("--user-dir", required=True)
    p_disc.add_argument("--title-id", required=True)

    p_hdr = sub.add_parser("make-reject-header", help="Write a synthetic 160-byte EGL05CCH v999 file")
    p_hdr.add_argument("--out", required=True)
    p_hdr.add_argument("--version", type=int, default=999)

    args = parser.parse_args(argv)
    if args.cmd == "score-pair":
        res = score_pair(args.cold, args.second, label=args.label, require_baseline=not args.no_baseline)
        print(json.dumps(res, indent=2))
        return 0 if res["verdict"] == "PASS" else (2 if res["verdict"] == "INCOMPLETE" else 1)

    if args.cmd == "check-reject":
        res = verify_cache_rejection(
            args.report,
            args.log,
            expected_reason=args.reason,
            expected_version=args.expected_version,
            found_version=args.found_version,
        )
        print(json.dumps(res, indent=2))
        return 0 if res["verdict"] == "PASS" else 1

    if args.cmd == "discover-caches":
        dirs = [str(p) for p in discover_title_cache_dirs(args.user_dir, args.title_id)]
        files = [str(p) for p in find_title_cache_files(args.user_dir, args.title_id)]
        print(json.dumps({"dirs": dirs, "files": files}, indent=2))
        return 0

    if args.cmd == "make-reject-header":
        Path(args.out).write_bytes(build_synthetic_envelope(args.version))
        return 0

    return 2


if __name__ == "__main__":
    raise SystemExit(main())

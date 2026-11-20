#!/usr/bin/env python3
"""Offline unit tests for tools/first_minutes_gate/first_minutes_gate.py using synthetic fixtures."""

from __future__ import annotations

import json
import struct
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import first_minutes_gate as fg  # noqa: E402

ORIGIN = 1_000_000_000
MS = 1_000_000
SYNTHETIC_TID = "deadbeef"


def make_frame(idx: int, duration_ms: int, **causes_ms: int) -> dict:
    cause = dict.fromkeys(fg.ALL_CAUSES, 0)
    used = 0
    for name, value in causes_ms.items():
        cause[name] = value * MS
        used += cause[name]
    cause["waiting"] += duration_ms * MS - used
    start = ORIGIN + (idx + 1) * 5_000_000_000
    return {
        "id": 100 + idx,
        "start_ns": start,
        "end_ns": start + duration_ms * MS,
        "duration_ns": duration_ms * MS,
        "cause_ns": cause,
        "primary_cause": "waiting",
        "instrumentation_complete": True,
    }


def make_report(
    frames: list[dict],
    *,
    baseline_hash: str = "a" * 64,
    on_baseline: bool = True,
    lost_events: int = 0,
    unknown_extra_ms: int = 0,
) -> dict:
    if unknown_extra_ms and frames:
        frames[0]["cause_ns"]["unknown"] += unknown_extra_ms * MS
        frames[0]["cause_ns"]["waiting"] -= unknown_extra_ms * MS
    return {
        "schema_version": 2,
        "baseline": {
            "version": 1,
            "hash": baseline_hash,
            "requested": True,
            "on_baseline": on_baseline,
            "deviations": [],
        },
        "startup": {
            "first_host_present_ns": ORIGIN,
            "process_to_first_present": {"status": "complete"},
            "first_present_to_interactive": {"status": "complete"},
        },
        "first_120s": {
            "frames_over_100ms": len(frames),
            "complete": True,
        },
        "attribution": {
            "enabled": True,
            "complete": True,
            "lost_events": lost_events,
            "dropped_boundaries": 0,
            "sink_write_errors": 0,
            "producer_unknown_wait_ns": 0,
            "long_frames": frames,
        },
        "exit": {"code": 0},
    }


class FirstMinutesGateTests(unittest.TestCase):
    def test_halving_pass_with_game_gaps_excluded(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            cold_p = root / "cold.json"
            sec_p = root / "second.json"
            # Cold: 4 emulator-caused + 2 game-gap frames (6 raw)
            cold_frames = [
                make_frame(0, 180, shader=120),
                make_frame(1, 200, pipeline=150),
                make_frame(2, 160, code_translation=100),
                make_frame(3, 190, texture=140),
                make_frame(4, 600, shader=20),  # remainder 580 ms -> game gap
                make_frame(5, 500, disk=10),    # remainder 490 ms -> game gap
            ]
            # Second: 2 emulator-caused + 4 game-gap frames (6 raw -> raw didn't halve, attributed 4->2 did)
            sec_frames = [
                make_frame(0, 170, texture=120),
                make_frame(1, 160, code_translation=90),
                make_frame(2, 550, shader=5),
                make_frame(3, 520, pipeline=10),
                make_frame(4, 480, disk=10),
                make_frame(5, 450, decompression=10),
            ]
            cold_p.write_text(json.dumps(make_report(cold_frames)), encoding="utf-8")
            sec_p.write_text(json.dumps(make_report(sec_frames)), encoding="utf-8")

            res = fg.score_pair(cold_p, sec_p, label="title_a")
            self.assertEqual(res["verdict"], "PASS")
            self.assertEqual(res["cold"]["raw_count"], 6)
            self.assertEqual(res["cold"]["emulator_attributed_count"], 4)
            self.assertEqual(res["cold"]["game_gap_count"], 2)
            self.assertEqual(res["second"]["raw_count"], 6)
            self.assertEqual(res["second"]["emulator_attributed_count"], 2)
            self.assertEqual(res["second"]["game_gap_count"], 4)

    def test_miss_reports_largest_residual_cause_and_keeps_open(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            cold_p = root / "cold.json"
            sec_p = root / "second.json"
            cold_p.write_text(
                json.dumps(make_report([make_frame(0, 220, shader=160), make_frame(1, 200, texture=150)])),
                encoding="utf-8",
            )
            sec_p.write_text(
                json.dumps(make_report([make_frame(0, 320, texture=260), make_frame(1, 180, shader=100)])),
                encoding="utf-8",
            )
            pair = fg.score_pair(cold_p, sec_p, label="title_b")
            self.assertEqual(pair["verdict"], "FAIL")
            self.assertEqual(pair["largest_residual_cause"], {"cause": "texture", "total_ms": 260.0})

            gate = fg.score_gate({"title_b": [pair]})
            self.assertEqual(gate["verdict"], "FAIL")
            self.assertTrue(gate["milestone_open"])
            self.assertTrue(gate["titles"]["title_b"]["milestone_open"])

    def test_zero_cold_is_nothing_to_halve(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            cold_p = root / "cold.json"
            sec_p = root / "second.json"
            cold_p.write_text(json.dumps(make_report([make_frame(0, 500, shader=10)])), encoding="utf-8")
            sec_p.write_text(json.dumps(make_report([])), encoding="utf-8")
            pair = fg.score_pair(cold_p, sec_p)
            self.assertEqual(pair["verdict"], "NOTHING_TO_HALVE")
            gate = fg.score_gate({"t": [pair]})
            self.assertEqual(gate["verdict"], "FAIL")
            self.assertTrue(gate["milestone_open"])

    def test_unknown_over_10_percent_and_baseline_mismatch_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            cold_p = root / "cold.json"
            sec_p = root / "second.json"
            cold_p.write_text(
                json.dumps(make_report([make_frame(0, 200, shader=120)], unknown_extra_ms=25)),
                encoding="utf-8",
            )
            sec_p.write_text(json.dumps(make_report([])), encoding="utf-8")
            res = fg.score_pair(cold_p, sec_p)
            self.assertEqual(res["verdict"], "INCOMPLETE")
            self.assertIn("unknown share", res["reasons"][0])

            cold_p.write_text(json.dumps(make_report([make_frame(0, 200, shader=120)])), encoding="utf-8")
            sec_p.write_text(
                json.dumps(make_report([], baseline_hash="b" * 64)),
                encoding="utf-8",
            )
            res2 = fg.score_pair(cold_p, sec_p)
            self.assertEqual(res2["verdict"], "INCOMPLETE")
            self.assertIn("baseline mismatch", res2["reasons"][0])

    def test_cache_dir_discovery_includes_semantics_revision_subdirs(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            user_dir = Path(tmp) / "user"
            direct = user_dir / "cache" / "shader" / SYNTHETIC_TID
            rev6 = direct / "semantics_r6"
            rev6.mkdir(parents=True)
            (direct / "jit_blocks.mythos.bin").write_bytes(b"jit")
            (rev6 / "vulkan.mythos.bin").write_bytes(fg.build_synthetic_envelope(1))
            (rev6 / "vulkan_used.mythos.bin").write_bytes(b"used")

            dirs = fg.discover_title_cache_dirs(user_dir, SYNTHETIC_TID)
            self.assertEqual(dirs, [direct, rev6])
            files = {p.relative_to(direct).as_posix() for p in fg.find_title_cache_files(user_dir, SYNTHETIC_TID)}
            self.assertEqual(
                files,
                {"jit_blocks.mythos.bin", "semantics_r6/vulkan.mythos.bin", "semantics_r6/vulkan_used.mythos.bin"},
            )

    def test_synthetic_envelope_stamp_and_rejection_verification(self) -> None:
        hdr = fg.build_synthetic_envelope(999)
        self.assertEqual(len(hdr), fg.CACHE_ENVELOPE_BYTES)
        self.assertTrue(hdr.startswith(fg.CACHE_ENVELOPE_MAGIC))
        self.assertEqual(struct.unpack_from("<I", hdr, fg.CACHE_VERSION_OFFSET)[0], 999)

        v1 = fg.build_synthetic_envelope(1)
        stamped = fg.stamp_rejected_envelope(v1, 999)
        self.assertIsNotNone(stamped)
        self.assertEqual(struct.unpack_from("<I", stamped, fg.CACHE_VERSION_OFFSET)[0], 999)

        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            rep = root / "report.json"
            log = root / "eden_log.txt"
            rep.write_text(
                json.dumps(
                    {
                        "exit": {"code": 0},
                        "cache": {
                            "files": [
                                {
                                    "kind": "transferable",
                                    "status": "rejected",
                                    "reason": "format_version",
                                    "expected": 1,
                                    "found": 999,
                                }
                            ]
                        },
                    }
                ),
                encoding="utf-8",
            )
            log.write_text(
                "[0.12] Render.Vulkan <Warning> Nx2Cache reject kind=transferable "
                "reason=format_version expected=1 found=999 action=ignore\n",
                encoding="utf-8",
            )
            ok = fg.verify_cache_rejection(rep, log)
            self.assertEqual(ok["verdict"], "PASS")

            rep_bad = root / "report_bad.json"
            rep_bad.write_text(json.dumps({"exit": {"code": 1}, "cache": {"files": []}}), encoding="utf-8")
            bad = fg.verify_cache_rejection(rep_bad, log)
            self.assertEqual(bad["verdict"], "FAIL")


if __name__ == "__main__":
    unittest.main()

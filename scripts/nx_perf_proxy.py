#!/usr/bin/env python3
"""
Estimate Nintendo Switch benchmark deltas from local benchmark runs.

Workflow:
1) Export/save a benchmark baseline on Switch (reference switch baseline).
2) Export/save a benchmark baseline on local machine from the same commit/scenes (reference local baseline).
3) Export/save a local baseline from your candidate changes.
4) Run this script to estimate Switch impact based on local speedup ratios.
"""

import argparse
import math
from dataclasses import dataclass

# Baseline file stores profiler arrays by index from FrameProfiler enums:
# phase[13] => PROFILE_PHASE_TOTAL_FRAME, counters[8] => PROFILE_COUNTER_DL_ITERATIONS.
PROFILE_PHASE_TOTAL_FRAME_INDEX = 13
PROFILE_COUNTER_DL_ITERATIONS_INDEX = 8
MIN_VALID_RENDER_MS = 0.01


@dataclass
class BaselineScene:
    name: str
    phases: list[float]
    counters: list[float]

    @property
    def render_ms(self) -> float:
        total_ms = self.phases[PROFILE_PHASE_TOTAL_FRAME_INDEX] if len(self.phases) > PROFILE_PHASE_TOTAL_FRAME_INDEX else 0.0
        dl_iterations = (
            self.counters[PROFILE_COUNTER_DL_ITERATIONS_INDEX] if len(self.counters) > PROFILE_COUNTER_DL_ITERATIONS_INDEX else 1.0
        )
        if dl_iterations < 1.0:
            dl_iterations = 1.0
        return total_ms / dl_iterations


@dataclass
class BaselineFile:
    mode: str
    scenes: dict[str, BaselineScene]


def parse_baseline(path: str) -> BaselineFile:
    with open(path, "r", encoding="utf-8") as infile:
        lines = [line.strip() for line in infile]

    if not lines or lines[0] != "BASELINE_V1":
        got_header = lines[0] if lines else "<empty file>"
        raise ValueError(f"{path}: unsupported baseline format, expected 'BASELINE_V1', got '{got_header}'")

    mode = "Unknown"
    scenes: dict[str, BaselineScene] = {}
    i = 1
    while i < len(lines):
        line = lines[i]
        if line.startswith("mode="):
            mode = line.split("=", 1)[1]
            i += 1
            continue

        if not line.startswith("SCENE="):
            i += 1
            continue

        scene_name = line.split("=", 1)[1]
        phases: list[float] = []
        counters: list[float] = []

        if i + 1 < len(lines) and lines[i + 1].startswith("phases="):
            phases = [float(x) for x in lines[i + 1].split("=", 1)[1].split(",") if x]
        if i + 2 < len(lines) and lines[i + 2].startswith("counters="):
            counters = [float(x) for x in lines[i + 2].split("=", 1)[1].split(",") if x]

        scenes[scene_name] = BaselineScene(scene_name, phases, counters)
        i += 3

    return BaselineFile(mode=mode, scenes=scenes)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Estimate Switch performance from local benchmark deltas using baseline calibration."
    )
    parser.add_argument("--switch-ref", required=True, help="Switch benchmark_baseline.txt (reference commit)")
    parser.add_argument("--local-ref", required=True, help="Local benchmark_baseline.txt from same reference commit")
    parser.add_argument("--local-candidate", required=True, help="Local benchmark_baseline.txt from candidate commit")
    parser.add_argument(
        "--fail-regression-ms",
        type=float,
        default=0.75,
        help="Fail if estimated average Switch regression exceeds this many ms (default: 0.75)",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()

    switch_ref = parse_baseline(args.switch_ref)
    local_ref = parse_baseline(args.local_ref)
    local_candidate = parse_baseline(args.local_candidate)

    if switch_ref.mode != local_ref.mode or local_ref.mode != local_candidate.mode:
        raise ValueError("All baselines must use the same benchmark mode (Quick vs Full)")

    common_scenes = sorted(set(switch_ref.scenes) & set(local_ref.scenes) & set(local_candidate.scenes))
    if not common_scenes:
        raise ValueError("No overlapping scenes found across provided baselines")

    print("=== NX Performance Proxy Estimate ===")
    print(f"Mode: {switch_ref.mode}")
    print()
    print(f"{'Scene':24} {'Switch Ref':>10} {'Est Switch':>10} {'Delta':>9} {'Delta %':>9}")
    print("-" * 68)

    ref_sum = 0.0
    est_sum = 0.0
    counted = 0

    for scene_name in common_scenes:
        s_ref = switch_ref.scenes[scene_name]
        l_ref = local_ref.scenes[scene_name]
        l_new = local_candidate.scenes[scene_name]

        ref_local_ms = l_ref.render_ms
        new_local_ms = l_new.render_ms
        if ref_local_ms <= MIN_VALID_RENDER_MS or not math.isfinite(ref_local_ms) or not math.isfinite(new_local_ms):
            print(f"Skipping scene '{scene_name}': invalid local timing data")
            continue

        speed_ratio = new_local_ms / ref_local_ms
        est_switch_ms = s_ref.render_ms * speed_ratio
        delta_ms = est_switch_ms - s_ref.render_ms
        delta_pct = (delta_ms / s_ref.render_ms * 100.0) if s_ref.render_ms > MIN_VALID_RENDER_MS else 0.0

        print(f"{scene_name:24} {s_ref.render_ms:10.2f} {est_switch_ms:10.2f} {delta_ms:9.2f} {delta_pct:8.1f}%")

        ref_sum += s_ref.render_ms
        est_sum += est_switch_ms
        counted += 1

    if counted == 0:
        raise ValueError("No valid scene data available to estimate performance")

    avg_ref = ref_sum / counted
    avg_est = est_sum / counted
    avg_delta = avg_est - avg_ref
    avg_delta_pct = (avg_delta / avg_ref * 100.0) if avg_ref > MIN_VALID_RENDER_MS else 0.0

    print("-" * 68)
    print(f"{'Average':24} {avg_ref:10.2f} {avg_est:10.2f} {avg_delta:9.2f} {avg_delta_pct:8.1f}%")

    if avg_delta > args.fail_regression_ms:
        print(
            f"\nFAIL: Estimated average Switch regression {avg_delta:.2f} ms exceeds "
            f"threshold {args.fail_regression_ms:.2f} ms"
        )
        return 1

    print(
        f"\nPASS: Estimated average Switch delta {avg_delta:.2f} ms "
        f"(threshold {args.fail_regression_ms:.2f} ms)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

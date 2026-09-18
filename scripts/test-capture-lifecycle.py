"""Run the complete generated-window capture resource acceptance batch silently.

No physical input, audio playback, driver reset, or desktop unlock is performed.
Handle acceptance retains the original +8 bound. Private memory, working set and
GUI objects are observed; memory acceptance is separate unless explicitly bounded.
"""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("room_regression", ROOT / "scripts/test-room-regression.py")
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("build_directory", type=Path)
    parser.add_argument("output_directory", type=Path)
    parser.add_argument("--cycles", type=int, default=100, choices=range(20, 101), metavar="20..100")
    parser.add_argument("--max-private-growth", type=int, help="Optional per-process private-memory growth bound in bytes")
    args = parser.parse_args()
    if args.max_private_growth is not None and args.max_private_growth < 0:
        parser.error("Private-memory growth bound must be nonnegative")
    build = args.build_directory.resolve()
    output = args.output_directory.resolve()
    cases = [
        ("rapid-close", "LiveCaptureTest.exe", ["--capture-only", "--close-source-first"]),
        ("fresh-owner", "LiveCaptureTest.exe", ["--capture-only", "--close-source-first", "--fresh-owner-thread"]),
        ("hardware-recovery", "LiveCaptureTest.exe", []),
        ("production-owner", "WindowsCaptureLifecycleTest.exe", ["--production-owner"]),
    ]
    for _, program, _ in cases:
        if not (build / program).is_file():
            parser.error(f"Missing proof target: {program}")
    if output.exists():
        parser.error("Choose a new evidence directory; existing results are never overwritten")
    output.mkdir(parents=True)
    report = {"schema": 1, "passed": False, "cyclesPerCase": args.cycles, "maxHandleGrowth": 8,
              "maxPrivateGrowth": args.max_private_growth, "runs": [],
              "limitations": ["Generated windows on one machine; no remote/network/physical input acceptance",
                              "Capture lifetime only; not a four-viewer media soak or full-room restart test",
                              "Passing a handle bound does not account for private memory or driver caches"]}
    started = time.monotonic()
    try:
        report["runnerSha256"] = runner.sha256(Path(__file__))
        stress = ROOT / "scripts/stress-live-capture.py"
        report["stressRunnerSha256"] = runner.sha256(stress)
        blocked = runner.desktop_unavailable()
        if blocked:
            report.update(blocked=True, error=blocked)
            print(f"BLOCKED: {blocked}", flush=True)
            return 2
        for name, program, flags in cases:
            executable = build / program
            identity = runner.sha256(executable)
            evidence = output / name
            command = [sys.executable, stress, executable, evidence, "--cycles", str(args.cycles),
                       "--max-handle-growth", "8", "--timeout", "1200", *flags]
            if args.max_private_growth is not None:
                command += ["--max-private-growth", str(args.max_private_growth)]
            run = {"name": name, "executableSha256": identity, "evidence": f"{name}/result.json"}
            report["runs"].append(run)
            run.update(runner.run_process(command, output / f"{name}.log", dict(os.environ), timeout=1220))
            detail = json.loads((evidence / "result.json").read_text(encoding="utf-8"))
            run["passed"] = run["passed"] and detail.get("passed") is True and detail.get("executableSha256") == identity
            run["resourceTrends"] = detail.get("resourceTrends")
            print(f"{'PASS' if run['passed'] else 'FAIL'} {name}: {detail.get('handleTrend')}", flush=True)
            if not run["passed"]:
                return 1
        report["passed"] = True
        return 0
    except Exception as error:
        report["error"] = str(error)
        return 1
    finally:
        report["elapsedSeconds"] = round(time.monotonic() - started, 3)
        (output / "result.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
        print(f"Evidence: {output / 'result.json'}", flush=True)


if __name__ == "__main__":
    sys.exit(main())

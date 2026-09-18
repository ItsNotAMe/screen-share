"""Run the generated-window capture proof with a watchdog and durable evidence.

This launches only the selected capture proof; no desktop/driver reset or global debugger
configuration. Resource samples are observations, not a leak-free assertion.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import re
import subprocess
import statistics
import time


def summarize(stderr, expected, exit_code, timed_out, max_handle_growth=None, max_private_growth=None):
    completed = [int(n) for n in re.findall(r"^Capture cycle (\d+) destroyed$", stderr, re.M)]
    samples = []
    malformed_samples = 0
    for line in stderr.splitlines():
        if not line.startswith("LIFECYCLE "):
            continue
        try:
            sample = json.loads(line.removeprefix("LIFECYCLE "))
            if not isinstance(sample, dict) or type(sample.get("cycle")) is not int:
                raise ValueError("Invalid cycle sample")
            samples.append(sample)
        except (ValueError, TypeError):
            malformed_samples += 1
    valid_samples = not malformed_samples and [sample["cycle"] for sample in samples] == list(range(expected + 1))
    trends = {}
    if valid_samples and expected >= 20:
        for metric in ("handles", "privateBytes", "workingSetBytes", "gdiObjects", "userObjects"):
            values = [s.get(metric) for s in samples]
            if all(type(value) is int and value >= 0 for value in values):
                trend = {"baselineMedian": statistics.median(values[6:11]), "finalMedian": statistics.median(values[-5:]),
                         "peak": max(values), "final": values[-1]}
                trend["growth"] = trend["finalMedian"] - trend["baselineMedian"]
                trends[metric] = trend
    trend = trends.get("handles")
    resources_pass = max_handle_growth is None or (trend is not None and trend["growth"] <= max_handle_growth)
    private = trends.get("privateBytes")
    resources_pass &= max_private_growth is None or (private is not None and private["growth"] <= max_private_growth)
    return {
        "passed": not timed_out and exit_code == 0 and completed == list(range(1, expected + 1)) and valid_samples and resources_pass,
        "timedOut": timed_out,
        "exitCode": exit_code,
        "exitCodeHex": f"0x{exit_code & 0xffffffff:08X}",
        "expectedCycles": expected,
        "completedCycles": completed,
        "samples": samples,
        "malformedSamples": malformed_samples,
        "handleTrend": trend,
        "maxHandleGrowth": max_handle_growth,
        "resourceTrends": trends,
        "maxPrivateGrowth": max_private_growth,
        "lastDiagnosticLines": stderr.splitlines()[-20:],
    }


def run_capture(command, output, timeout, max_log_bytes):
    started = time.monotonic()
    timed_out = log_limit_exceeded = False
    with (output / "stdout.log").open("wb") as stdout, (output / "stderr.log").open("wb") as stderr:
        process = subprocess.Popen(command, stdout=stdout, stderr=stderr)
        try:
            while process.poll() is None:
                log_limit_exceeded = sum((output / name).stat().st_size for name in ("stdout.log", "stderr.log")) > max_log_bytes
                timed_out = time.monotonic() - started >= timeout
                if log_limit_exceeded or timed_out:
                    process.kill()  # Only our child; the capture proofs create no children.
                    break
                time.sleep(0.05)
            code = process.wait()
        except BaseException:
            process.kill()
            process.wait()
            raise
    remaining = max_log_bytes
    for name in ("stderr.log", "stdout.log"):
        path = output / name
        size = path.stat().st_size
        if size > remaining:
            log_limit_exceeded = True
            with path.open("r+b") as log:
                log.truncate(remaining)
        remaining -= min(size, remaining)
    return code, timed_out, log_limit_exceeded


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    parser.add_argument("output", type=Path, help="New evidence directory (must not already exist)")
    parser.add_argument("--cycles", type=int, default=100, choices=range(1, 101))
    parser.add_argument("--timeout", type=float, default=1200, help="Total run deadline in seconds")
    parser.add_argument("--max-log-bytes", type=int, default=8 * 1024 * 1024, help="Combined native stdout/stderr bound; exceeding it fails and stops the child")
    parser.add_argument("--capture-only", action="store_true", help="Isolate capture teardown from hardware encoder/device-recovery work")
    parser.add_argument("--production-owner", action="store_true", help="Run WindowsCaptureLifecycleTest through the production CaptureSession owner")
    parser.add_argument("--close-source-first", action="store_true", help="Capture-only regression case: close source immediately before stopping capture")
    parser.add_argument("--rebuild-device", action="store_true", help="Capture-only isolation: rebuild the WGC device once each cycle")
    parser.add_argument("--resize-source", action="store_true", help="Capture-only isolation: resize the generated source")
    parser.add_argument("--gpu-readback", action="store_true", help="Capture-only isolation: retain and read back GPU frames")
    parser.add_argument("--hardware-only", action="store_true", help="Full capture and encode without device retirement/software recovery")
    parser.add_argument("--max-handle-growth", type=int, help="Require median handle growth <= this limit; compares cycles 6–10 with the final five cycles")
    parser.add_argument("--max-private-growth", type=int, help="Optional private-memory growth bound in bytes; same median windows as handles")
    parser.add_argument("--capture-frames", type=int, choices=range(1, 301), metavar="1..300", help="Capture-only isolation: consume additional frames before teardown")
    parser.add_argument("--fresh-owner-thread", action="store_true", help="Create and join a capture owner thread for each complete cycle")
    args = parser.parse_args()
    if not math.isfinite(args.timeout) or args.timeout <= 0:
        parser.error("timeout must be finite and positive")
    if args.max_log_bytes < 1024:
        parser.error("--max-log-bytes must be at least 1024")
    if args.close_source_first and not args.capture_only:
        parser.error("--close-source-first requires --capture-only")
    if args.rebuild_device and not args.capture_only:
        parser.error("--rebuild-device requires --capture-only")
    if (args.resize_source or args.gpu_readback) and not args.capture_only:
        parser.error("--resize-source/--gpu-readback require --capture-only")
    if args.capture_frames and not args.capture_only:
        parser.error("--capture-frames requires --capture-only")
    if args.hardware_only and args.capture_only:
        parser.error("--hardware-only cannot be combined with --capture-only")
    if args.max_handle_growth is not None and (args.max_handle_growth < 0 or args.cycles < 20):
        parser.error("handle-growth checking requires a nonnegative limit and at least 20 cycles")
    if args.max_private_growth is not None and (args.max_private_growth < 0 or args.cycles < 20):
        parser.error("private-memory checking requires a nonnegative limit and at least 20 cycles")
    if args.production_owner and (args.capture_only or args.hardware_only or args.fresh_owner_thread):
        parser.error("--production-owner cannot be combined with direct-capture modes")
    executable = args.executable.resolve(strict=True)
    if args.production_owner != (executable.stem.lower() == "windowscapturelifecycletest"):
        parser.error("WindowsCaptureLifecycleTest requires --production-owner; direct capture proofs must omit it")
    args.output.mkdir(parents=True, exist_ok=False)
    with executable.open("rb") as binary:
        identity = hashlib.file_digest(binary, "sha256").hexdigest()
    started = time.monotonic()
    command = [str(executable), "--cycles", str(args.cycles)]
    for enabled, flag in ((args.fresh_owner_thread, "--fresh-owner-thread"), (args.capture_only, "--capture-only"),
                          (args.close_source_first, "--close-source-first"), (args.rebuild_device, "--rebuild-device"),
                          (args.resize_source, "--resize-source"), (args.gpu_readback, "--gpu-readback"),
                          (args.hardware_only, "--hardware-only")):
        if enabled:
            command.append(flag)
    if args.capture_frames:
        command.extend(["--capture-frames", str(args.capture_frames)])
    code, timed_out, log_limit_exceeded = run_capture(command, args.output, args.timeout, args.max_log_bytes)
    result = summarize((args.output / "stderr.log").read_text(encoding="utf-8", errors="replace"), args.cycles, code, timed_out,
                       args.max_handle_growth, args.max_private_growth)
    mode = "hardware-only" if args.hardware_only else "hardware-recovery"
    if args.production_owner:
        mode = "production-owner-recovery"
    if args.capture_only:
        mode = "capture-only" + ("-rebuild" if args.rebuild_device else "")
        mode += "-source-closed" if args.close_source_first else "-source-open"
    result.update(executableSha256=identity, command=command,
                  elapsedSeconds=time.monotonic() - started, mode=mode,
                  logLimitExceeded=log_limit_exceeded, maxLogBytes=args.max_log_bytes)
    result["passed"] &= not log_limit_exceeded
    (args.output / "result.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({key: result[key] for key in ("passed", "timedOut", "exitCodeHex", "expectedCycles", "elapsedSeconds")}))
    return 0 if result["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())

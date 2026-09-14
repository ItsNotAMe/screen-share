"""Run the generated-window capture proof with a watchdog and durable evidence.

This launches only LiveCaptureTest; no desktop/driver reset or global debugger
configuration. Resource samples are observations, not a leak-free assertion.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import re
import subprocess
import time


def summarize(stderr, expected, exit_code, timed_out):
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
    return {
        "passed": not timed_out and exit_code == 0 and completed == list(range(1, expected + 1)) and valid_samples,
        "timedOut": timed_out,
        "exitCode": exit_code,
        "exitCodeHex": f"0x{exit_code & 0xffffffff:08X}",
        "expectedCycles": expected,
        "completedCycles": completed,
        "samples": samples,
        "malformedSamples": malformed_samples,
        "lastDiagnosticLines": stderr.splitlines()[-20:],
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    parser.add_argument("output", type=Path, help="New evidence directory (must not already exist)")
    parser.add_argument("--cycles", type=int, default=100, choices=range(1, 101))
    parser.add_argument("--timeout", type=float, default=1200, help="Total run deadline in seconds")
    parser.add_argument("--capture-only", action="store_true", help="Isolate capture teardown from hardware encoder/device-recovery work")
    parser.add_argument("--close-source-first", action="store_true", help="Capture-only regression case: close source immediately before stopping capture")
    args = parser.parse_args()
    if not math.isfinite(args.timeout) or args.timeout <= 0:
        parser.error("timeout must be finite and positive")
    if args.close_source_first and not args.capture_only:
        parser.error("--close-source-first requires --capture-only")
    executable = args.executable.resolve(strict=True)
    args.output.mkdir(parents=True, exist_ok=False)
    with executable.open("rb") as binary:
        identity = hashlib.file_digest(binary, "sha256").hexdigest()
    started = time.monotonic()
    timed_out = False
    with (args.output / "stdout.log").open("wb") as stdout, (args.output / "stderr.log").open("wb") as stderr:
        command = [str(executable), "--cycles", str(args.cycles)]
        if args.capture_only:
            command.append("--capture-only")
        if args.close_source_first:
            command.append("--close-source-first")
        process = subprocess.Popen(command, stdout=stdout, stderr=stderr)
        try:
            code = process.wait(timeout=args.timeout)
        except subprocess.TimeoutExpired:
            timed_out = True
            process.kill()  # Only the child created above; the proof creates no children.
            code = process.wait()
        except BaseException:
            process.kill()
            process.wait()
            raise
    result = summarize((args.output / "stderr.log").read_text(encoding="utf-8", errors="replace"), args.cycles, code, timed_out)
    result.update(executableSha256=identity, elapsedSeconds=time.monotonic() - started,
                  mode="capture-only-source-closed" if args.close_source_first else "capture-only-source-open" if args.capture_only else "hardware-recovery")
    (args.output / "result.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({key: result[key] for key in ("passed", "timedOut", "exitCodeHex", "expectedCycles", "elapsedSeconds")}))
    return 0 if result["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())

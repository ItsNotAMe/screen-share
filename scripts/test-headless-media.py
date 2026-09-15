"""Run built headless capture + H.264/Opus/DTLS proofs with watchdogs and evidence."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("build_directory", type=Path)
    parser.add_argument("output_directory", type=Path)
    parser.add_argument("--regression", action="store_true", help="Repeat media teardown 20 times")
    parser.add_argument("--room-build-directory", type=Path, help="Also exercise the real native room WebSocket client against a local server")
    args = parser.parse_args()
    programs = [args.build_directory.resolve() / name for name in
                ("CaptureSessionTest.exe", "CaptureDistributorTest.exe", "StreamSettingsTest.exe", "HostMediaSessionTest.exe", "IceCandidateHandoffTest.exe", "PeerConnectionLifecycleTest.exe", "HostPeerRegistryTest.exe", "SignalingExecutorTest.exe", "HostPeerOwnerTest.exe", "WebRTCProof.exe")]
    for program in programs:
        if not program.is_file():
            parser.error(f"Build the proof targets first: missing {program.name}")
    room_program = args.room_build_directory.resolve() / "RoomSocketTests.exe" if args.room_build_directory else None
    if room_program and not room_program.is_file():
        parser.error(f"Build the native application tests first: missing {room_program}")
    # Refuse to overwrite previous evidence.
    args.output_directory.mkdir(parents=True, exist_ok=False)
    report = {"schema": 1, "mode": "headless-local-media", "passed": False,
              "limitations": ["Local peers share one process", "No WGC or physical audio/display",
                               "No Cloudflare service, input injection or network impairment",
                               "Internal timing is not capture-to-display latency"], "runs": []}
    sequence = [(program, []) for program in programs[:-1]]
    if room_program:
        sequence.append((room_program, []))
    sequence += [(programs[-1], ["--negotiation-only"])]
    sequence += [(programs[-1], [])] * (20 if args.regression else 1)
    sequence += [(programs[-1], ["--multi-viewer"])] * (3 if args.regression else 1)
    try:
        for index, (program, arguments) in enumerate(sequence):
            log = args.output_directory / f"{index:02d}-{program.stem}.log"
            timeout = 75 if program == room_program else (60 if arguments else 45)
            run = {"executable": program.name, "arguments": arguments,
                   "sha256": hashlib.sha256(program.read_bytes()).hexdigest(),
                   "timeout_seconds": timeout, "timed_out": False, "log": log.name}
            report["runs"].append(run)
            started = time.monotonic()
            # These tools own threads, not child processes. subprocess.run kills
            # and waits for the sole process on timeout. No desktop interaction.
            with log.open("wb") as output:
                try:
                    completed = subprocess.run([str(program), *arguments], stdout=output, stderr=subprocess.STDOUT,
                                               timeout=timeout, check=False)
                    run["exit_code"] = completed.returncode
                except subprocess.TimeoutExpired:
                    run["timed_out"] = True
                    run["exit_code"] = None
            run["elapsed_seconds"] = round(time.monotonic() - started, 3)
            if run["timed_out"] or run["exit_code"] != 0:
                break
            if program != programs[-1] or arguments:
                run["metrics"] = json.loads(log.read_text())
            print(f"Passed {program.name} ({index + 1}/{len(sequence)})", flush=True)
        report["passed"] = len(report["runs"]) == len(sequence) and all(
            run.get("exit_code") == 0 and not run["timed_out"] for run in report["runs"])
    finally:
        (args.output_directory / "result.json").write_text(json.dumps(report, indent=2) + "\n")
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    sys.exit(main())

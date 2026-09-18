"""Run production room/UI/CLI scenarios silently, with bounded logs and process-tree cleanup."""
import argparse
import ctypes
from ctypes import wintypes
import hashlib
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
BOOTSTRAP = "import subprocess,sys; gate=sys.stdin.buffer.read(1); sys.exit(subprocess.call(sys.argv[1:]) if gate == b'1' else 125)"


def desktop_unavailable():
    """Read desktop identity only; never unlock, dismiss a saver, or send input."""
    if os.name != "nt":
        return "Windows desktop presentation requires Windows"
    user = ctypes.WinDLL("user32", use_last_error=True)
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.GetCurrentThreadId.restype = wintypes.DWORD
    user.GetThreadDesktop.argtypes, user.GetThreadDesktop.restype = [wintypes.DWORD], wintypes.HANDLE
    user.OpenInputDesktop.argtypes, user.OpenInputDesktop.restype = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD], wintypes.HANDLE
    user.CloseDesktop.argtypes, user.CloseDesktop.restype = [wintypes.HANDLE], wintypes.BOOL
    user.GetUserObjectInformationW.argtypes = [wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p, wintypes.DWORD, ctypes.POINTER(wintypes.DWORD)]
    user.GetUserObjectInformationW.restype = wintypes.BOOL

    def name(handle):
        value, needed = ctypes.create_unicode_buffer(256), wintypes.DWORD()
        if not handle or not user.GetUserObjectInformationW(handle, 2, value, ctypes.sizeof(value), ctypes.byref(needed)):
            raise ctypes.WinError(ctypes.get_last_error())
        return value.value

    active = user.OpenInputDesktop(0, False, 1)  # DESKTOP_READOBJECTS
    if not active:
        return f"Input desktop is unavailable (Windows error {ctypes.get_last_error()}); it may be locked or secure"
    try:
        current_name = name(user.GetThreadDesktop(kernel.GetCurrentThreadId()))
        active_name = name(active)
        if current_name.casefold() != active_name.casefold():
            return f"Desktop presentation unavailable: test desktop '{current_name}', input desktop '{active_name}'"
    finally:
        user.CloseDesktop(active)
    return None


class WindowsJob:
    """The bootstrap cannot spawn until it belongs to our kill-on-close job."""
    def __init__(self):
        class Basic(ctypes.Structure):
            _fields_ = [("process_time", ctypes.c_int64), ("job_time", ctypes.c_int64),
                        ("flags", wintypes.DWORD), ("min_ws", ctypes.c_size_t),
                        ("max_ws", ctypes.c_size_t), ("active", wintypes.DWORD),
                        ("affinity", ctypes.c_size_t), ("priority", wintypes.DWORD),
                        ("scheduling", wintypes.DWORD)]

        class IO(ctypes.Structure):
            _fields_ = [(name, ctypes.c_uint64) for name in
                        ("read_ops", "write_ops", "other_ops", "read_bytes", "write_bytes", "other_bytes")]

        class Extended(ctypes.Structure):
            _fields_ = [("basic", Basic), ("io", IO), ("process_memory", ctypes.c_size_t),
                        ("job_memory", ctypes.c_size_t), ("peak_process", ctypes.c_size_t),
                        ("peak_job", ctypes.c_size_t)]

        self.api = ctypes.WinDLL("kernel32", use_last_error=True)
        for name, arguments, result in (
            ("CreateJobObjectW", [ctypes.c_void_p, wintypes.LPCWSTR], wintypes.HANDLE),
            ("SetInformationJobObject", [wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p, wintypes.DWORD], wintypes.BOOL),
            ("AssignProcessToJobObject", [wintypes.HANDLE, wintypes.HANDLE], wintypes.BOOL),
            ("CloseHandle", [wintypes.HANDLE], wintypes.BOOL),
        ):
            function = getattr(self.api, name)
            function.argtypes, function.restype = arguments, result
        self.handle = self.api.CreateJobObjectW(None, None)
        if not self.handle:
            raise ctypes.WinError(ctypes.get_last_error())
        limits = Extended()
        limits.basic.flags = 0x2000  # JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
        if not self.api.SetInformationJobObject(self.handle, 9, ctypes.byref(limits), ctypes.sizeof(limits)):
            error = ctypes.WinError(ctypes.get_last_error())
            self.close()
            raise error

    def assign(self, process):
        if not self.api.AssignProcessToJobObject(self.handle, wintypes.HANDLE(int(process._handle))):
            raise ctypes.WinError(ctypes.get_last_error())

    def close(self):
        if self.handle:
            self.api.CloseHandle(self.handle)
            self.handle = None


def run_process(command, log, environment, timeout=90, max_log_bytes=1024 * 1024):
    started = time.monotonic()
    process = None
    job = WindowsJob() if os.name == "nt" else None
    result = {"exitCode": None, "timedOut": False, "logLimitExceeded": False}
    try:
        with log.open("wb") as output:
            process = subprocess.Popen([sys.executable, "-c", BOOTSTRAP, *map(str, command)],
                cwd=ROOT, env=environment, stdin=subprocess.PIPE, stdout=output, stderr=subprocess.STDOUT,
                creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0,
                start_new_session=os.name != "nt")
            if job:
                job.assign(process)
            process.stdin.write(b"1")
            process.stdin.close()
            while process.poll() is None:
                if log.stat().st_size > max_log_bytes:
                    result["logLimitExceeded"] = True
                    break
                if time.monotonic() - started >= timeout:
                    result["timedOut"] = True
                    break
                time.sleep(0.02)
            result["logLimitExceeded"] |= log.stat().st_size > max_log_bytes
            result["exitCode"] = process.poll()
    finally:
        if job:
            job.close()  # Also removes descendants left behind after successful exit.
        if process:
            if os.name != "nt":
                try:
                    os.killpg(process.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
            if process.poll() is None:
                process.kill()  # Bootstrap only if assignment failed; no child was allowed to spawn.
            process.wait(timeout=10)
        if log.exists() and log.stat().st_size > max_log_bytes:
            with log.open("r+b") as output:
                output.truncate(max_log_bytes)
        result["elapsedSeconds"] = round(time.monotonic() - started, 3)
    result["passed"] = result["exitCode"] == 0 and not result["timedOut"] and not result["logLimitExceeded"]
    return result


def sha256(path):
    with path.open("rb") as source:
        return hashlib.file_digest(source, "sha256").hexdigest()


def cases(desktop):
    result = [("input-service", "InputServiceTests.exe", None, None),
              ("diagnostic-report", "RoomDiagnosticReportTests.exe", None, None),
              ("gamepad-control", "GamepadControlTests.exe", None, None),
              ("desktop-input", "DesktopInputTests.exe", None, None),
              ("input-media", "RoomInputTests.exe", "media", None),
              ("presentation-worker", "VideoFrameInputTests.exe", None, None),
              ("room-service", "RoomServiceTests.exe", "service", None),
              ("cli-media", "RoomCliTests.exe", "media", None),
              ("ui-media", "RoomUiTests.exe", "media", None),
              ("controller-ui", "RoomUiTests.exe", "media", "controllers"),
              ("desktop-input-ui", "RoomUiTests.exe", "media", "desktop-input"),
              ("mutation-recovery", "RoomUiTests.exe", "media", "mutation-ack-delay")]
    if desktop:
        result += [("windows-cli", "RoomCliWindowsTests.exe", "windows-media", None),
                   ("windows-ui", "RoomUiWindowsTests.exe", "windows-media", None),
                   ("windows-controller-ui", "RoomUiWindowsTests.exe", "windows-media", "controllers"),
                   ("windows-desktop-input-ui", "RoomUiWindowsTests.exe", "windows-media", "desktop-input")]
    return result


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("build_directory", type=Path)
    parser.add_argument("output_directory", type=Path)
    parser.add_argument("--repeat", type=int, default=1, help="Complete scenario rounds (1..100); stops at first failure")
    parser.add_argument("--desktop", action="store_true", help="Also test generated Windows capture/GPU windows; still silent, no physical input")
    args = parser.parse_args(argv)
    if not 1 <= args.repeat <= 100:
        parser.error("--repeat must be 1..100")
    build = args.build_directory.resolve()
    output = args.output_directory.resolve()
    selected = cases(args.desktop)
    node = shutil.which("node")
    if not node:
        parser.error("Node.js is required")
    if not (ROOT / "signaling-worker/node_modules/miniflare/package.json").is_file():
        parser.error("Install signaling-worker development dependencies first")
    for _, program, _, _ in selected:
        if not (build / program).is_file():
            parser.error(f"Build the application test targets first: missing {program}")
    if output.exists():
        parser.error("Output directory already exists; choose a new evidence directory")
    output.mkdir(parents=True)
    report = {"schema": 1, "passed": False, "desktop": args.desktop, "repeat": args.repeat,
              "buildDirectory": str(build), "pythonVersion": sys.version.split()[0],
              "plannedRuns": len(selected) * args.repeat, "runs": [],
              "limitations": ["Local Worker uses a loopback plaintext adapter; no remote TLS/NAT acceptance",
                              "Host and viewers share a process within each native scenario",
                              "Synthetic silent audio and recording input sinks; no physical controller/driver acceptance",
                              "No matched performance, external latency, hibernation, load or physical driver-loss acceptance"]}
    fixture = ROOT / "signaling-worker/tests/run-native-service.mjs"
    started = time.monotonic()
    try:
        report["runnerSha256"] = sha256(Path(__file__))
        report["fixtureSha256"] = sha256(fixture)
        if args.desktop:
            reason = desktop_unavailable()
            if reason:
                report["blocked"] = True
                report["error"] = reason
                print(f"BLOCKED: {reason}. Run without --desktop for headless checks.", flush=True)
                return 2
        hashes = {program: sha256(build / program) for _, program, _, _ in selected}
        for iteration in range(args.repeat):
            for name, program, mode, fault in selected:
                case_root = output / f"{iteration + 1:03d}-{name}"
                case_root.mkdir()
                environment = dict(os.environ)
                environment["QT_QPA_PLATFORM"] = "windows" if mode == "windows-media" else "offscreen"
                command = [build / program] if mode is None else [node, fixture, build / program, case_root / "service", mode]
                if fault:
                    command.append(fault)
                run = {"name": name, "round": iteration + 1, "executable": program,
                       "executableSha256": hashes[program], "log": str((case_root / "runner.log").relative_to(output))}
                report["runs"].append(run)
                run.update(run_process(command, case_root / "runner.log", environment))
                if mode is not None:
                    reports = list((case_root / "service").glob("native-service-*/result.json"))
                    if len(reports) != 1:
                        run["passed"] = False
                        run["error"] = "Missing or ambiguous fixture evidence"
                    else:
                        evidence = json.loads(reports[0].read_text(encoding="utf-8"))
                        run["evidence"] = str(reports[0].relative_to(output))
                        run["fixtureTimedOut"] = evidence.get("timedOut")
                        run["passed"] = run["passed"] and evidence.get("passed") is True and evidence.get("executableSha256") == hashes[program]
                        run["metrics"] = evidence.get("metrics")
                        run["workerBundleSha256"] = evidence.get("workerBundleSha256")
                print(f"{'PASS' if run['passed'] else 'FAIL'} {iteration + 1}/{args.repeat} {name}", flush=True)
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

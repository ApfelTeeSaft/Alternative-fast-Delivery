#!/usr/bin/env python3
"""Repeatedly exercise the real packaged UI with an external hang timeout."""
import argparse
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    parser.add_argument("--repeat", type=int, default=10)
    parser.add_argument("--timeout", type=float, default=15)
    parser.add_argument("--offscreen", action="store_true")
    parser.add_argument("--hardware-rendering", action="store_true")
    parser.add_argument("--report", type=Path)
    args = parser.parse_args()
    executable = args.executable.resolve(strict=True)
    if args.repeat < 1 or args.timeout <= 0:
        parser.error("Repeat count and timeout must be positive")
    env = os.environ.copy()
    # Validate the application's defaults, independent of a developer test shell.
    for key in ("QT_QUICK_BACKEND", "QSG_RHI_BACKEND", "QSG_RENDER_LOOP"):
        env.pop(key, None)
    if args.offscreen:
        env["QT_QPA_PLATFORM"] = "offscreen"
        if not args.hardware_rendering:
            env["QT_QUICK_BACKEND"] = "software"
    elif os.name == "nt":
        env["QT_QPA_PLATFORM"] = "windows"
    env["QT_FORCE_STDERR_LOGGING"] = "1"
    env["QSG_INFO"] = "1"
    command = [str(executable), "--interaction-smoke-test"]
    if args.hardware_rendering:
        command.append("--hardware-rendering")
    runs = []
    for number in range(1, args.repeat + 1):
        try:
            result = subprocess.run(
                command,
                cwd=executable.parent,
                env=env,
                capture_output=True,
                timeout=args.timeout,
            )
            output = (result.stdout + result.stderr).decode("utf-8", errors="replace")
            passed = result.returncode == 0 and "PASS packaged startup:" in output
            runs.append(
                {
                    "run": number,
                    "passed": passed,
                    "exit": result.returncode,
                    "log": output,
                }
            )
        except subprocess.TimeoutExpired:
            runs.append(
                {"run": number, "passed": False, "error": "startup/render timeout"}
            )
            passed = False
        print(
            f"{'PASS' if passed else 'FAIL'} startup {number}/{args.repeat}", flush=True
        )
        if not passed:
            break
    report = {
        "time": datetime.now(timezone.utc).isoformat(),
        "executable": str(executable),
        "offscreen": args.offscreen,
        "hardware_requested": args.hardware_rendering,
        "requested_runs": args.repeat,
        "runs": runs,
    }
    if args.report:
        args.report.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    if len(runs) != args.repeat or not all(run["passed"] for run in runs):
        print(json.dumps(runs[-1], indent=2), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())

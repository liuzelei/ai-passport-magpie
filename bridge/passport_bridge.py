#!/usr/bin/env python3
"""Relay aggregate Magpie usage to the AI Passport over Bluetooth LE."""

from __future__ import annotations

import argparse
from contextlib import contextmanager
from datetime import datetime
import json
import os
from pathlib import Path
import select
import subprocess
import sys
import time
from uuid import UUID

from magpie_usage import Snapshot, UsageError, find_magpie, read_snapshot

HELPER_BUILD = Path(__file__).resolve().parent / "ble" / "build.sh"
PAIRING_TIMEOUT = 90
ACK_TIMEOUT = 8


class BluetoothSetupError(OSError):
    """The operator needs to choose a device or grant Bluetooth access."""


def build_frame(snapshot: Snapshot | None, clock: str) -> bytes:
    if snapshot is None:
        return b"MPU1,ERR\n"
    return (
        f"MPU1,OK,{clock},{snapshot.today_total},{snapshot.today_input},"
        f"{snapshot.today_output},{snapshot.today_cache},"
        f"{snapshot.week_total},{snapshot.month_total}\n"
    ).encode("ascii")


def prepare_helper() -> str:
    try:
        path = Path(subprocess.check_output([str(HELPER_BUILD)], text=True, timeout=90).strip())
    except (OSError, subprocess.SubprocessError) as error:
        raise OSError("Unable to build the Bluetooth helper. Xcode Command Line Tools are required.") from error
    if not path.is_file() or not os.access(path, os.X_OK):
        raise OSError("The Bluetooth helper build did not produce a usable executable.")
    return str(path)


class BluetoothLink:
    def __init__(self, process: subprocess.Popen):
        self.process = process
        self.pending = bytearray()

    def close_error(self) -> OSError:
        try:
            code = self.process.wait(timeout=1)
        except subprocess.TimeoutExpired:
            code = None
        if code == 2:
            return BluetoothSetupError("Select the Passport using --scan and --device.")
        if code == 3:
            return BluetoothSetupError("Bluetooth access is unavailable. Check macOS Bluetooth permissions.")
        return OSError("The Bluetooth helper disconnected.")

    def expect(self, reply: bytes, timeout: float) -> None:
        fd = self.process.stdout.fileno()
        deadline = time.monotonic() + timeout
        while b"\n" not in self.pending:
            remaining = deadline - time.monotonic()
            if remaining <= 0 or not select.select([fd], [], [], remaining)[0]:
                raise TimeoutError(f"Bluetooth timed out waiting for {reply.decode('ascii')}.")
            chunk = os.read(fd, 128)
            if not chunk:
                raise self.close_error()
            self.pending.extend(chunk)
            if len(self.pending) > 256:
                raise OSError("Invalid Bluetooth helper response.")
        line, _, remainder = self.pending.partition(b"\n")
        self.pending = remainder
        if line.rstrip(b"\r") != reply:
            raise OSError("Unexpected Bluetooth helper response.")

    def send(self, frame: bytes) -> None:
        self.process.stdin.write(frame)
        self.expect(b"ACK", ACK_TIMEOUT)

    def wait_until_refresh(self, interval: float) -> None:
        try:
            self.process.wait(timeout=interval)
        except subprocess.TimeoutExpired:
            return
        raise self.close_error()


@contextmanager
def connect(command: list[str], *, timeout: float = PAIRING_TIMEOUT):
    process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE, bufsize=0)
    try:
        link = BluetoothLink(process)
        link.expect(b"READY", timeout)
        yield link
    finally:
        process.stdin.close()
        process.stdout.close()
        if process.poll() is None:
            process.terminate()
        try:
            process.wait(timeout=2)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()


def relay(args, executable: str, command: list[str]) -> int:
    last_status = None

    def status(message, stream=sys.stderr):
        nonlocal last_status
        if message != last_status:
            print(message, file=stream, flush=True)
            last_status = message

    while True:
        try:
            with connect(command) as link:
                while True:
                    try:
                        snapshot = read_snapshot(executable, use_loopback=args.magpie is None)
                    except UsageError:
                        snapshot = None
                    link.send(build_frame(snapshot, datetime.now().strftime("%H:%M")))
                    if snapshot is None:
                        status("Unable to read Magpie usage. Passport marked the last reading as paused.")
                    else:
                        status("Passport acknowledged Magpie usage over Bluetooth.", sys.stdout)
                    if args.once:
                        return 0 if snapshot is not None else 1
                    link.wait_until_refresh(args.interval)
        except OSError as error:
            status(str(error))
            if args.once or isinstance(error, BluetoothSetupError):
                return 1
            time.sleep(3)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    modes = parser.add_mutually_exclusive_group()
    modes.add_argument("--preview", action="store_true", help="print aggregate counts without accessing Bluetooth")
    modes.add_argument("--scan", action="store_true", help="list advertising FoloToy AI Passport token displays")
    modes.add_argument("--once", action="store_true", help="send one snapshot and require a device acknowledgement")
    parser.add_argument("--device", type=UUID, help="select a Passport by the UUID shown by --scan")
    parser.add_argument("--magpie", help="path to a Magpie executable, bypassing the default local API")
    parser.add_argument("--interval", type=int, default=30, help="refresh interval in seconds (5–60, default: 30)")
    args = parser.parse_args()
    if not 5 <= args.interval <= 60:
        parser.error("--interval must be between 5 and 60 seconds")

    try:
        if args.scan:
            try:
                return subprocess.run([prepare_helper(), "--scan"], timeout=PAIRING_TIMEOUT, check=False).returncode
            except subprocess.TimeoutExpired:
                print("Bluetooth scan did not finish.", file=sys.stderr)
                return 1
        executable = find_magpie(args.magpie)
        if args.preview:
            snapshot = read_snapshot(executable, use_loopback=args.magpie is None)
            print(json.dumps({
                "today": {
                    "tokens": snapshot.today_total, "input": snapshot.today_input,
                    "output": snapshot.today_output, "cache_read": snapshot.today_cache_read,
                    "cache_write": snapshot.today_cache_write,
                },
                "7d": {"tokens": snapshot.week_total},
                "30d": {"tokens": snapshot.month_total},
            }, indent=2))
            return 0
        command = [prepare_helper()]
        if args.device:
            command.extend(["--device", str(args.device)])
        return relay(args, executable, command)
    except (OSError, UsageError) as error:
        print(str(error), file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        return 0


if __name__ == "__main__":
    raise SystemExit(main())

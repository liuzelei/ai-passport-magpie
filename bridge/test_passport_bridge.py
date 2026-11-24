import sys
import textwrap
import unittest

from magpie_usage import Snapshot
from passport_bridge import BluetoothSetupError, build_frame, connect


def helper(script):
    return [sys.executable, "-u", "-c", textwrap.dedent(script)]


class PassportBridgeTest(unittest.TestCase):
    def test_wire_frame_includes_combined_cache_and_preserves_large_counts(self):
        snapshot = Snapshot(today_input=0, today_output=12, today_cache_read=8, today_cache_write=3,
                            week_total=9_000_000_000, month_total=15_000_000_000)
        self.assertEqual(
            build_frame(snapshot, "09:05"),
            b"MPU1,OK,09:05,23,0,12,11,9000000000,15000000000\n",
        )
        self.assertEqual(build_frame(None, "09:05"), b"MPU1,ERR\n")

    def test_one_ready_session_delivers_several_frames(self):
        command = helper(r"""
            import os
            import sys
            import time

            os.write(1, b"REA")
            time.sleep(0.01)
            os.write(1, b"DY\n")
            for expected in (b"MPU1,ERR\n", b"MPU1,OK,09:05,0,0,0,0,0,0\n"):
                if sys.stdin.buffer.readline() != expected:
                    sys.exit(1)
                os.write(1, b"ACK\n")
        """)
        with connect(command, timeout=2) as link:
            link.send(build_frame(None, "09:05"))
            link.send(build_frame(Snapshot(0, 0, 0, 0, 0, 0), "09:05"))

    def test_no_snapshot_session_before_the_device_is_ready(self):
        command = helper("print('ACK')")
        with self.assertRaisesRegex(OSError, "Unexpected"):
            with connect(command, timeout=2):
                self.fail("An ACK cannot stand in for READY")

    def test_pairing_timeout_interrupts_and_reaps_helper(self):
        command = helper("import time; time.sleep(60)")
        with self.assertRaisesRegex(TimeoutError, "READY"):
            with connect(command, timeout=0.05):
                self.fail("A silent helper is never ready")

    def test_bluetooth_permission_errors_are_not_retryable(self):
        with self.assertRaisesRegex(BluetoothSetupError, "macOS Bluetooth permissions"):
            with connect(helper("raise SystemExit(3)"), timeout=2):
                self.fail("A denied helper is never ready")


if __name__ == "__main__":
    unittest.main()

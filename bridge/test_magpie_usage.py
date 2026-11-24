import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from magpie_usage import UsageError, find_magpie, read_snapshot


HEADER = "source,input_tokens,output_tokens,cache_read_tokens,cache_write_tokens,rejected,handed_on,error_message\n"


class MagpieUsageTests(unittest.TestCase):
    def test_uses_magpie_windows_and_excludes_only_rejected_tokens(self):
        exports = {
            "today": (
                "log,12,8,5000000000,50,false,false,\n"
                "through,5,2,100,20,false,true,\n"
                "through,100,150,400,80,true,false,\n"
            ),
            "7d": "log,120,80,90000,3,false,false,\n",
            "30d": "reported,1000,800,50000000000,2000,false,false,\n",
        }

        def export(command, **kwargs):
            self.assertEqual(command[:3], ["magpie", "usage", "--csv"])
            return subprocess.CompletedProcess(command, 0, HEADER + exports[command[3]])

        with patch("magpie_usage.subprocess.run", side_effect=export) as run:
            snapshot = read_snapshot("magpie", use_loopback=False)

        self.assertEqual(run.call_count, 3)
        self.assertEqual(snapshot.today_input, 17)
        self.assertEqual(snapshot.today_output, 10)
        self.assertEqual(snapshot.today_total, 5_000_000_197)
        self.assertEqual(snapshot.today_cache_read, 5000000100)
        self.assertEqual(snapshot.today_cache_write, 70)
        self.assertEqual(snapshot.week_total, 90_203)
        self.assertEqual(snapshot.month_total, 50_000_003_800)

    @patch("magpie_usage.subprocess.run")
    def test_empty_periods_return_zero(self, run):
        run.return_value = subprocess.CompletedProcess([], 0, HEADER)
        snapshot = read_snapshot("magpie", use_loopback=False)
        self.assertEqual((snapshot.today_total, snapshot.week_total, snapshot.month_total), (0, 0, 0))

    @patch("magpie_usage.subprocess.run")
    def test_missing_data_columns_raise_a_clear_error(self, run):
        run.return_value = subprocess.CompletedProcess([], 0, "input_tokens,output_tokens\n")
        with self.assertRaisesRegex(UsageError, "missing required columns"):
            read_snapshot("magpie", use_loopback=False)

    @patch("magpie_usage.subprocess.run")
    def test_bad_values_do_not_reveal_raw_csv_content(self, run):
        run.return_value = subprocess.CompletedProcess([], 0, HEADER + "log,PRIVATE,0,0,0,false,false,\n")
        with self.assertRaises(UsageError) as caught:
            read_snapshot("magpie", use_loopback=False)
        self.assertIn("row 2", str(caught.exception))
        self.assertNotIn("PRIVATE", str(caught.exception))

    @patch("magpie_usage.subprocess.run")
    def test_cli_failure_and_timeout_are_reported(self, run):
        run.return_value = subprocess.CompletedProcess([], 7, "PRIVATE")
        with self.assertRaisesRegex(UsageError, "status 7"):
            read_snapshot("magpie", use_loopback=False)
        run.side_effect = subprocess.TimeoutExpired("magpie", 30, output="PRIVATE")
        with self.assertRaisesRegex(UsageError, "timed out"):
            read_snapshot("magpie", use_loopback=False)

    @patch("magpie_usage.subprocess.run")
    @patch("magpie_usage.http.client.HTTPConnection")
    def test_loopback_uses_combined_requests_totals_and_never_launches_cli(self, server, run):
        connection = server.return_value
        response = connection.getresponse.return_value
        response.status = 200
        response.read.side_effect = [
            b'{"period":"today","input":22,"output":5,"cache_read":4200000000,"cache_write":42,"rows":[{}]}',
            b'{"period":"7d","input":100,"output":40,"cache_read":555,"cache_write":3}',
            b'{"period":"30d","input":1000,"output":400,"cache_read":555555,"cache_write":9}',
        ]
        snapshot = read_snapshot("magpie")

        server.assert_called_once_with("127.0.0.1", 3425, timeout=2)
        self.assertEqual(
            [call.args for call in connection.request.call_args_list],
            [("GET", f"/v1/magpie/usage/requests?limit=1&period={period}") for period in ("today", "7d", "30d")],
        )
        self.assertEqual(snapshot.today_total, 4_200_000_069)
        self.assertEqual(snapshot.today_cache_read, 4200000000)
        self.assertEqual(snapshot.today_cache_write, 42)
        self.assertEqual(snapshot.week_total, 698)
        self.assertEqual(snapshot.month_total, 556_964)
        run.assert_not_called()

    @patch("magpie_usage.subprocess.run")
    @patch("magpie_usage.http.client.HTTPConnection")
    def test_redirect_and_bad_schema_fall_back_to_cli(self, server, run):
        response = server.return_value.getresponse.return_value
        run.return_value = subprocess.CompletedProcess([], 0, HEADER + "log,11,2,0,0,false,false,\n")
        for status, payload in (
            (302, b""),
            (200, b'{"period":"today","input":true,"output":0,"cache_read":0}'),
            (200, b'{"period":"7d","input":0,"output":0,"cache_read":0}'),
            (200, b'{"period":"today","input":0,"output":0,"cache_read":123}'),
        ):
            with self.subTest(status=status, payload=payload):
                response.status = status
                response.read.return_value = payload
                self.assertEqual(read_snapshot("magpie").today_total, 13)
        self.assertEqual(run.call_count, 12)

    def test_find_explicit_executable_accepts_spaces(self):
        with tempfile.TemporaryDirectory() as directory:
            executable = Path(directory) / "Magpie test"
            executable.touch(mode=0o700)
            self.assertEqual(find_magpie(str(executable)), str(executable))
            with self.assertRaises(UsageError):
                find_magpie(str(executable) + " missing")


if __name__ == "__main__":
    unittest.main()

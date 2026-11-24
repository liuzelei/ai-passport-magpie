"""Read aggregate token counts from Magpie's own usage ledger."""

from __future__ import annotations

import csv
import http.client
import io
import json
import os
import shutil
import subprocess
from contextlib import closing
from dataclasses import dataclass
from pathlib import Path


class UsageError(Exception):
    """Magpie could not supply a usage snapshot."""


@dataclass(frozen=True)
class Snapshot:
    today_input: int
    today_output: int
    today_cache_read: int
    today_cache_write: int
    week_total: int
    month_total: int

    @property
    def today_cache(self) -> int:
        return self.today_cache_read + self.today_cache_write

    @property
    def today_total(self) -> int:
        return self.today_input + self.today_output + self.today_cache


@dataclass(frozen=True)
class _Totals:
    input: int
    output: int
    cache_read: int
    cache_write: int

    @property
    def total(self) -> int:
        return self.input + self.output + self.cache_read + self.cache_write


_PERIODS = ("today", "7d", "30d")
_CLI_TIMEOUT = 30
_MAX_HTTP_BYTES = 4 * 1024 * 1024


def find_magpie(explicit: str | None) -> str:
    if explicit is not None:
        candidate = shutil.which(os.path.expanduser(explicit))
        if candidate:
            return candidate
        raise UsageError("The selected Magpie executable was not found or is not executable.")

    for root in (Path("/Applications"), Path.home() / "Applications"):
        candidate = shutil.which(str(root / "magpie.app/Contents/MacOS/magpie"))
        if candidate:
            return candidate
    candidate = shutil.which("magpie")
    if candidate:
        return candidate
    raise UsageError("Magpie was not found. Install Magpie or pass --magpie with its executable path.")


def _loopback_totals() -> list[_Totals]:
    totals = []
    # The requests endpoint includes direct session imports. /v1/magpie/usage
    # only counts the gateway ledger. HTTPConnection uses neither proxies nor redirects.
    with closing(http.client.HTTPConnection("127.0.0.1", 3425, timeout=2)) as connection:
        for period in _PERIODS:
            connection.request("GET", f"/v1/magpie/usage/requests?limit=1&period={period}")
            with closing(connection.getresponse()) as response:
                if response.status != 200:
                    raise UsageError("Magpie's local usage API is unavailable.")
                raw = response.read(_MAX_HTTP_BYTES + 1)
            if len(raw) > _MAX_HTTP_BYTES:
                raise UsageError("Magpie's local usage response is too large.")
            payload = json.loads(raw)
            if not isinstance(payload, dict) or payload.get("period") != period:
                raise UsageError("Magpie's local usage response reported the wrong period.")
            counters = [payload.get(name) for name in ("input", "output", "cache_read", "cache_write")]
            if any(type(value) is not int or value < 0 for value in counters):
                raise UsageError("Magpie's local usage response is missing valid counters.")
            totals.append(_Totals(*counters))
    return totals


def _csv_totals(executable: str, period: str) -> _Totals:
    try:
        result = subprocess.run(
            [executable, "usage", "--csv", period],
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            encoding="utf-8",
            timeout=_CLI_TIMEOUT,
            check=False,
        )
    except subprocess.TimeoutExpired:
        raise UsageError(f"Magpie usage export ({period}) timed out.") from None
    except (OSError, UnicodeError):
        raise UsageError(f"Magpie usage export ({period}) could not be read.") from None
    if result.returncode != 0:
        raise UsageError(f"Magpie usage export ({period}) exited with status {result.returncode}.")

    reader = csv.reader(io.StringIO(result.stdout, newline=""), strict=True)
    try:
        header = next(reader, [])
        columns = ("input_tokens", "output_tokens", "cache_read_tokens", "cache_write_tokens", "rejected")
        if any(header.count(column) != 1 for column in columns):
            raise UsageError(f"Magpie usage export ({period}) is missing required columns.")
        input_index, output_index, read_index, write_index, rejected_index = map(header.index, columns)
        input_tokens = output_tokens = cache_read = cache_write = 0
        for row in reader:
            if not row:
                continue
            if len(row) != len(header) or row[rejected_index] not in ("true", "false"):
                raise ValueError
            if row[rejected_index] == "true":
                continue
            used_input = int(row[input_index])
            used_output = int(row[output_index])
            used_read = int(row[read_index])
            used_write = int(row[write_index])
            if min(used_input, used_output, used_read, used_write) < 0:
                raise ValueError
            input_tokens += used_input
            output_tokens += used_output
            cache_read += used_read
            cache_write += used_write
    except (csv.Error, ValueError):
        raise UsageError(f"Magpie usage export ({period}) is invalid at row {reader.line_num}.") from None
    return _Totals(input_tokens, output_tokens, cache_read, cache_write)


def read_snapshot(executable: str, *, use_loopback: bool = True) -> Snapshot:
    if use_loopback:
        try:
            today, week, month = _loopback_totals()
        except (UsageError, OSError, http.client.HTTPException, ValueError):
            use_loopback = False
    if not use_loopback:
        # Magpie owns calendar boundaries, including its configurable day start.
        today, week, month = (_csv_totals(executable, period) for period in _PERIODS)

    return Snapshot(today.input, today.output, today.cache_read, today.cache_write, week.total, month.total)

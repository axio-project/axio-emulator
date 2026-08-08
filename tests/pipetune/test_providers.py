from __future__ import annotations

import pathlib
import unittest

from pipetune.providers.perf import PerfProvider
from pipetune.providers.pcm_pcie import PcmPcieProvider


FIXTURES = pathlib.Path(__file__).with_name("fixtures")


class ProviderTest(unittest.TestCase):
    def test_provider_version_probes_are_explicit(self) -> None:
        self.assertEqual(
            PerfProvider.version_command("/usr/bin/perf"),
            ("env", "LC_ALL=C", "/usr/bin/perf", "--version"),
        )
        self.assertEqual(
            PerfProvider.parse_version(b"perf version 5.15.160\n"),
            "perf version 5.15.160",
        )
        self.assertEqual(
            PcmPcieProvider.version_command(),
            ("env", "LC_ALL=C", "dpkg-query", "-W", "pcm"),
        )
        self.assertEqual(
            PcmPcieProvider.parse_version(b"pcm\t202201-1\n"),
            "pcm 202201-1",
        )
        with self.assertRaises(ValueError):
            PcmPcieProvider.parse_version(b"different-package 1\n")

    def test_perf_command_is_bounded_target_only_and_locale_stable(self) -> None:
        provider = PerfProvider("/usr/bin/perf", "perf version 5.15")
        commands = provider.commands(pid=1234, sample_interval_seconds=2.0)
        self.assertEqual(len(commands), 2)
        for command in commands:
            self.assertEqual(command[:2], ("env", "LC_ALL=C"))
            self.assertIn("-p", command)
            self.assertEqual(command[command.index("-p") + 1], "1234")
            self.assertIn("-I", command)
            self.assertEqual(command[-2:], ("sleep", "2"))
            self.assertNotIn("pkill", command)
        self.assertEqual(
            commands[0][commands[0].index("-e") + 1],
            "LLC-loads,LLC-load-misses",
        )
        self.assertEqual(
            commands[1][commands[1].index("-e") + 1],
            "LLC-stores,LLC-store-misses",
        )

    def test_perf_parses_valid_and_reordered_periodic_samples(self) -> None:
        provider = PerfProvider("/usr/bin/perf", "perf version 5.15")
        for fixture in (
            "perf-valid.csv",
            "perf-reordered.csv",
            "perf-metric-columns.csv",
        ):
            with self.subTest(fixture=fixture):
                result = provider.parse(
                    (FIXTURES / fixture).read_bytes(),
                    sample_interval_seconds=2.0,
                )
                self.assertTrue(result.status.available)
                self.assertEqual([counter.name for counter in result.counters], [
                    "llc_load", "llc_store"
                ])
                self.assertEqual(result.counters[0].numerator, 300)
                self.assertEqual(result.counters[0].denominator, 3000)
                self.assertAlmostEqual(result.counters[0].rate_percent or 0, 10.0)
                self.assertAlmostEqual(result.counters[1].rate_percent or 0, 4.0)

    def test_perf_unavailability_is_explicit(self) -> None:
        provider = PerfProvider("/usr/bin/perf", "perf version 5.15")
        for fixture, reason in (
            ("perf-unavailable.csv", "unsupported"),
            ("perf-zero.csv", "zero denominator"),
            ("perf-truncated.csv", "truncated"),
            ("perf-low-runtime.csv", "running time"),
            ("perf-locale.csv", "numeric"),
        ):
            with self.subTest(fixture=fixture):
                result = provider.parse(
                    (FIXTURES / fixture).read_bytes(),
                    sample_interval_seconds=(
                        2.0 if fixture in ("perf-truncated.csv",) else 1.0
                    ),
                )
                self.assertTrue(any(not value.available for value in result.counters))
                self.assertTrue(any(
                    reason in (value.reason or "")
                    for value in result.counters
                    if not value.available
                ))
        denied = provider.parse(
            b"Error: No permission to enable LLC-loads event.\n",
            sample_interval_seconds=1.0,
        )
        self.assertFalse(denied.status.available)
        self.assertTrue(all(not value.available for value in denied.counters))

    def test_pcm_command_is_bounded_and_locale_stable(self) -> None:
        provider = PcmPcieProvider("/usr/sbin/pcm-pcie", "pcm 202302")
        command = provider.command(
            output_path="/tmp/metrics with spaces.csv",
            sample_interval_seconds=3.0,
            period_seconds=1.0,
        )
        self.assertEqual(command[:2], ("env", "LC_ALL=C"))
        self.assertIn("-e", command)
        self.assertIn("-i=3", command)
        self.assertIn("-csv=/tmp/metrics with spaces.csv", command)
        self.assertNotIn("pkill", command)

    def test_pcm_parses_header_names_and_selected_socket(self) -> None:
        provider = PcmPcieProvider("/usr/sbin/pcm-pcie", "pcm 202302")
        for fixture in ("pcm-valid.csv", "pcm-reordered.csv"):
            with self.subTest(fixture=fixture):
                result = provider.parse(
                    (FIXTURES / fixture).read_bytes(),
                    stderr=b"",
                    socket_id=1,
                )
                self.assertTrue(result.status.available)
                io_read, io_write = result.counters
                self.assertEqual((io_read.name, io_write.name), (
                    "io_read", "io_write"
                ))
                self.assertEqual(io_read.numerator, 1546708)
                self.assertEqual(io_read.denominator, 282294640)
                self.assertAlmostEqual(
                    io_read.rate_percent or 0,
                    1546708 / 282294640 * 100,
                )
                self.assertAlmostEqual(
                    io_write.rate_percent or 0,
                    217965108 / 273254112 * 100,
                )

    def test_pcm_rejects_zero_inconsistent_truncated_and_locale_data(self) -> None:
        provider = PcmPcieProvider("/usr/sbin/pcm-pcie", "pcm 202302")
        for fixture, reason in (
            ("pcm-zero.csv", "zero denominator"),
            ("pcm-inconsistent.csv", "inconsistent"),
            ("pcm-truncated.csv", "truncated"),
            ("pcm-locale.csv", "numeric"),
        ):
            with self.subTest(fixture=fixture):
                result = provider.parse(
                    (FIXTURES / fixture).read_bytes(),
                    stderr=b"",
                    socket_id=1,
                )
                self.assertTrue(all(not value.available for value in result.counters))
                self.assertTrue(all(
                    reason in (value.reason or "") for value in result.counters
                ))
        denied = provider.parse(
            b"",
            stderr=b"PCM Error: can't open MSR handle for core 0\n",
            socket_id=1,
        )
        self.assertFalse(denied.status.available)

    def test_pcm_unknown_socket_and_version_drift_are_auditable(self) -> None:
        provider = PcmPcieProvider("/usr/sbin/pcm-pcie", "pcm future")
        result = provider.parse(
            (FIXTURES / "pcm-reordered.csv").read_bytes(),
            stderr=b"",
            socket_id=0,
        )
        self.assertTrue(result.status.available)
        self.assertTrue(all(not value.available for value in result.counters))
        self.assertTrue(all(
            "socket 0" in (value.reason or "") for value in result.counters
        ))


if __name__ == "__main__":
    unittest.main()

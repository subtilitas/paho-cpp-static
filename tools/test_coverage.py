#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Tests for tools/coverage.py — the README coverage block and its drift check."""

from __future__ import annotations

import importlib.util
import json
import tempfile
import unittest
from pathlib import Path

_HERE = Path(__file__).resolve().parent


def _load():
    spec = importlib.util.spec_from_file_location("coverage_tool", _HERE / "coverage.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


cov = _load()


def summary_data(line=94.2, branch=88.0, function=82.6) -> dict:
    return {
        "gcovr/summary_format_version": "0.6",
        "line_percent": line,
        "line_covered": 1193,
        "line_total": 1266,
        "branch_percent": branch,
        "branch_covered": 836,
        "branch_total": 950,
        "function_percent": function,
        "function_covered": 398,
        "function_total": 482,
    }


def summary(**kwargs) -> "cov.Summary":
    return cov.Summary(summary_data(**kwargs))


def readme_with(block: str) -> str:
    return f"# Title\n\n### Coverage\n\n{block}\n\nProse afterwards.\n"


class Reading(unittest.TestCase):
    def test_reads_all_three_metrics(self):
        s = summary()
        self.assertEqual(s.percent["line"], 94.2)
        self.assertEqual(s.covered["branch"], 836)
        self.assertEqual(s.total["function"], 482)

    def test_a_missing_field_is_refused(self):
        data = summary_data()
        del data["branch_total"]
        with self.assertRaises(SystemExit):
            cov.Summary(data)

    def test_an_unknown_format_version_is_refused(self):
        # gcovr changing its summary schema must not be absorbed silently: the
        # numbers would still parse and could still be wrong.
        data = summary_data()
        data["gcovr/summary_format_version"] = "1.0"
        with self.assertRaises(SystemExit):
            cov.Summary(data)

    def test_reads_from_a_file(self):
        with tempfile.TemporaryDirectory() as t:
            p = Path(t) / "coverage.json"
            p.write_text(json.dumps(summary_data()), encoding="utf-8")
            self.assertEqual(cov.Summary.read(p).percent["line"], 94.2)

    def test_malformed_json_is_refused(self):
        with tempfile.TemporaryDirectory() as t:
            p = Path(t) / "coverage.json"
            p.write_text("{not json", encoding="utf-8")
            with self.assertRaises(SystemExit):
                cov.Summary.read(p)


class Rendering(unittest.TestCase):
    def test_the_block_round_trips_through_the_parser(self):
        # The only property that really matters: whatever --write puts in the
        # README, --check has to be able to read back out of it.
        s = summary()
        parsed = cov.parse(cov.render(s))
        self.assertEqual(parsed, {"line": 94.2, "branch": 88.0, "function": 82.6})

    def test_the_block_carries_its_markers(self):
        block = cov.render(summary())
        self.assertTrue(block.startswith(cov.START))
        self.assertTrue(block.endswith(cov.END))

    def test_splice_replaces_only_the_block(self):
        readme = readme_with(cov.render(summary()))
        updated = cov.splice(readme, cov.render(summary(line=91.0)))
        self.assertIn("91.0%", updated)
        self.assertNotIn("94.2%", updated)
        self.assertTrue(updated.startswith("# Title"))
        self.assertTrue(updated.endswith("Prose afterwards.\n"))

    def test_a_readme_without_markers_is_an_error_not_a_silent_no_op(self):
        with self.assertRaises(SystemExit):
            cov.splice("# Title\n\nNo block here.\n", cov.render(summary()))

    def test_markers_in_the_wrong_order_are_an_error(self):
        with self.assertRaises(SystemExit):
            cov.splice(f"{cov.END}\n{cov.START}\n", cov.render(summary()))

    def test_a_block_missing_a_row_is_an_error(self):
        block = "\n".join(
            line for line in cov.render(summary()).splitlines() if "Branches" not in line
        )
        with self.assertRaises(SystemExit):
            cov.parse(block)


class Checking(unittest.TestCase):
    def test_matching_figures_pass(self):
        readme = readme_with(cov.render(summary()))
        self.assertEqual(cov.check(readme, summary(), cov.DEFAULT_TOLERANCE), [])

    def test_drift_inside_the_tolerance_passes(self):
        readme = readme_with(cov.render(summary(line=94.2)))
        self.assertEqual(cov.check(readme, summary(line=94.6), 0.5), [])

    def test_drift_outside_the_tolerance_fails(self):
        readme = readme_with(cov.render(summary(line=94.2)))
        problems = cov.check(readme, summary(line=90.0), 0.5)
        self.assertEqual(len(problems), 1)
        self.assertIn("Lines", problems[0])
        self.assertIn("below", problems[0])

    def test_a_rise_is_reported_too(self):
        # An unrecorded improvement is still a stale README, and the wording
        # should not accuse it of being a regression.
        readme = readme_with(cov.render(summary(line=90.0)))
        problems = cov.check(readme, summary(line=96.0), 0.5)
        self.assertIn("above", problems[0])

    def test_every_drifted_metric_is_reported_not_just_the_first(self):
        readme = readme_with(cov.render(summary()))
        problems = cov.check(readme, summary(line=10.0, branch=10.0, function=10.0), 0.5)
        self.assertEqual(len(problems), 3)


class CommandLine(unittest.TestCase):
    def _repo(self, tmp: Path, readme_block: str, **kwargs) -> tuple[Path, Path]:
        readme = tmp / "README.md"
        readme.write_text(readme_with(readme_block), encoding="utf-8")
        report = tmp / "coverage.json"
        report.write_text(json.dumps(summary_data(**kwargs)), encoding="utf-8")
        return readme, report

    def test_check_returns_zero_when_current(self):
        with tempfile.TemporaryDirectory() as t:
            readme, report = self._repo(Path(t), cov.render(summary()))
            code = cov.main(
                ["--summary", str(report), "--readme", str(readme), "--check"]
            )
            self.assertEqual(code, 0)

    def test_check_returns_one_when_drifted(self):
        with tempfile.TemporaryDirectory() as t:
            readme, report = self._repo(Path(t), cov.render(summary(line=50.0)))
            code = cov.main(
                ["--summary", str(report), "--readme", str(readme), "--check"]
            )
            self.assertEqual(code, 1)

    def test_write_updates_the_file_and_then_check_passes(self):
        with tempfile.TemporaryDirectory() as t:
            readme, report = self._repo(Path(t), cov.render(summary(line=50.0)))
            self.assertEqual(
                cov.main(["--summary", str(report), "--readme", str(readme), "--write"]), 0
            )
            self.assertIn("94.2%", readme.read_text(encoding="utf-8"))
            self.assertEqual(
                cov.main(["--summary", str(report), "--readme", str(readme), "--check"]), 0
            )

    def test_write_and_check_together_are_refused(self):
        with tempfile.TemporaryDirectory() as t:
            readme, report = self._repo(Path(t), cov.render(summary()))
            with self.assertRaises(SystemExit):
                cov.main(
                    [
                        "--summary",
                        str(report),
                        "--readme",
                        str(readme),
                        "--write",
                        "--check",
                    ]
                )


class TheRealReadme(unittest.TestCase):
    """The check has to work against the file that actually ships."""

    def test_the_repository_readme_has_a_parsable_block(self):
        readme = (cov.REPO / "README.md").read_text(encoding="utf-8")
        parsed = cov.parse(cov._block(readme))
        self.assertEqual(set(parsed), {"line", "branch", "function"})
        for value in parsed.values():
            self.assertGreaterEqual(value, 0.0)
            self.assertLessEqual(value, 100.0)


if __name__ == "__main__":
    unittest.main()

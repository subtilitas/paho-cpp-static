#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Tests for tools/pins.py — the dependency-pin reader and drift check."""

from __future__ import annotations

import importlib.util
import tempfile
import unittest
from pathlib import Path

_HERE = Path(__file__).resolve().parent


def _load():
    spec = importlib.util.spec_from_file_location("pins", _HERE / "pins.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


pins_mod = _load()

CMAKE = """\
cmake_minimum_required(VERSION 3.16)
project(paho-cpp-static VERSION 1.2.3 LANGUAGES CXX)
set(MQTT_VERSION_PRERELEASE "" CACHE STRING "...")
set(MQTT_ETL_TAG "20.39.4" CACHE STRING "...")
set(MQTT_ETL_COMMIT "081e920302e4062dbd122fc3c86255825ccaa666" CACHE STRING "...")
"""


def fake_repo(tmp: Path, cmake: str = CMAKE, workflows: dict[str, str] | None = None) -> Path:
    (tmp / "CMakeLists.txt").write_text(cmake, encoding="utf-8")
    wf = tmp / ".github" / "workflows"
    wf.mkdir(parents=True)
    for name, body in (workflows or {"ci.yml": "env:\n  ETL_COMMIT: "
                                     "081e920302e4062dbd122fc3c86255825ccaa666\n"}).items():
        (wf / name).write_text(body, encoding="utf-8")
    return tmp


class Reading(unittest.TestCase):
    def test_reads_the_three_pins(self):
        with tempfile.TemporaryDirectory() as t:
            p = pins_mod.Pins(fake_repo(Path(t)))
            self.assertEqual(p.version, "1.2.3")
            self.assertEqual(p.etl_tag, "20.39.4")
            self.assertEqual(p.etl_commit, "081e920302e4062dbd122fc3c86255825ccaa666")

    def test_an_empty_prerelease_leaves_the_version_alone(self):
        with tempfile.TemporaryDirectory() as t:
            p = pins_mod.Pins(fake_repo(Path(t)))
            self.assertEqual(p.prerelease, "")
            self.assertEqual(p.version_full, "1.2.3")

    def test_a_prerelease_suffix_joins_onto_the_version(self):
        # project(VERSION ...) cannot hold the suffix, so the two are declared
        # separately and joined here. This is what a release tag is checked
        # against.
        rc = CMAKE.replace('set(MQTT_VERSION_PRERELEASE ""',
                           'set(MQTT_VERSION_PRERELEASE "rc1"')
        with tempfile.TemporaryDirectory() as t:
            p = pins_mod.Pins(fake_repo(Path(t), cmake=rc))
            self.assertEqual(p.version, "1.2.3")
            self.assertEqual(p.prerelease, "rc1")
            self.assertEqual(p.version_full, "1.2.3-rc1")

    def test_a_missing_prerelease_declaration_is_an_error(self):
        # Absent is not the same as empty. The value decides whether a release
        # is marked as a prerelease, so it has to be declared on purpose rather
        # than defaulted by having been deleted.
        gone = "\n".join(line for line in CMAKE.splitlines()
                         if "MQTT_VERSION_PRERELEASE" not in line)
        with tempfile.TemporaryDirectory() as t:
            with self.assertRaises(SystemExit):
                pins_mod.Pins(fake_repo(Path(t), cmake=gone))

    def test_a_short_hash_is_not_accepted_as_a_pin(self):
        # An abbreviated hash is ambiguous in principle and unverifiable in
        # practice, and it is the shape a hurried edit produces.
        bad = CMAKE.replace("081e920302e4062dbd122fc3c86255825ccaa666", "081e920")
        with tempfile.TemporaryDirectory() as t:
            with self.assertRaises(SystemExit):
                pins_mod.Pins(fake_repo(Path(t), cmake=bad))

    def test_the_real_repository_parses(self):
        p = pins_mod.Pins()
        self.assertRegex(p.version, r"^\d+\.\d+\.\d+$")
        self.assertRegex(p.etl_commit, r"^[0-9a-f]{40}$")


class DriftCheck(unittest.TestCase):
    def test_a_matching_workflow_is_clean(self):
        with tempfile.TemporaryDirectory() as t:
            self.assertEqual(pins_mod.check(pins_mod.Pins(fake_repo(Path(t)))), [])

    def test_a_stale_env_value_is_caught(self):
        with tempfile.TemporaryDirectory() as t:
            repo = fake_repo(Path(t), workflows={
                "ci.yml": "env:\n  ETL_COMMIT: " + "0" * 40 + "\n"})
            problems = pins_mod.check(pins_mod.Pins(repo))
            self.assertTrue(any("ETL_COMMIT is" in p for p in problems), problems)

    def test_a_stale_tag_is_caught(self):
        with tempfile.TemporaryDirectory() as t:
            repo = fake_repo(Path(t), workflows={"ci.yml": "env:\n  ETL_TAG: 20.38.0\n"})
            problems = pins_mod.check(pins_mod.Pins(repo))
            self.assertTrue(any("ETL_TAG is" in p for p in problems), problems)

    def test_a_hardcoded_cache_key_is_caught(self):
        # This is the real bug this check exists for: the pin moves, one cache
        # key does not, and CI keeps handing that job the previous dependency
        # -- which passes, because the old version still works.
        with tempfile.TemporaryDirectory() as t:
            repo = fake_repo(Path(t), workflows={
                "wiki.yml": "      - uses: actions/cache@v6\n"
                            "        with:\n"
                            "          key: etl-wiki-20.39.4\n"})
            problems = pins_mod.check(pins_mod.Pins(repo))
            self.assertTrue(any("cache key" in p for p in problems), problems)

    def test_a_key_referring_to_the_env_is_accepted(self):
        with tempfile.TemporaryDirectory() as t:
            repo = fake_repo(Path(t), workflows={
                "ci.yml": "env:\n  ETL_COMMIT: " + "081e920302e4062dbd122fc3c86255825ccaa666\n"
                          "      - uses: actions/cache@v6\n"
                          "        with:\n"
                          "          key: etl-linux-${{ env.ETL_COMMIT }}\n"})
            self.assertEqual(pins_mod.check(pins_mod.Pins(repo)), [])

    def test_the_real_repository_has_not_drifted(self):
        self.assertEqual(pins_mod.check(pins_mod.Pins()), [])


if __name__ == "__main__":
    unittest.main()

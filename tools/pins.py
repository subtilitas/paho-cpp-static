#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Read this project's dependency pins, and check nothing has drifted from them.

`CMakeLists.txt` is the single source of truth: the project version, the ETL
commit that is actually fetched, and the release tag that commit corresponds
to. Everything else -- CI cache keys, the SBOM, release archive names -- is
derived from here rather than restated.

    tools/pins.py                # print what is pinned
    tools/pins.py --check        # fail if any workflow disagrees

`--check` rather than a bot that rewrites the workflows. A file that edits
itself is one nobody reads the diff of, and the diff is the whole point when
the subject is which third-party commit ends up in the binary.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent

_VERSION = re.compile(r"project\(paho-cpp-static\s+VERSION\s+([0-9][0-9.]*)")
_PRERELEASE = re.compile(r'set\(MQTT_VERSION_PRERELEASE\s+"([^"]*)"')
_ETL_TAG = re.compile(r'set\(MQTT_ETL_TAG\s+"([^"]+)"')
_ETL_COMMIT = re.compile(r'set\(MQTT_ETL_COMMIT\s+"([0-9a-f]{40})"')


class Pins:
    def __init__(self, repo: Path = REPO):
        text = (repo / "CMakeLists.txt").read_text(encoding="utf-8")
        self.version = self._one(_VERSION, text, "project(... VERSION ...)")

        # project(VERSION ...) is numeric, so a release candidate carries its
        # "-rc1" in a variable of its own. version_full is what a tag has to
        # match and what names a release archive.
        self.prerelease = self._one(_PRERELEASE, text, "MQTT_VERSION_PRERELEASE")
        self.version_full = (
            f"{self.version}-{self.prerelease}" if self.prerelease else self.version
        )

        self.etl_tag = self._one(_ETL_TAG, text, "MQTT_ETL_TAG")
        self.etl_commit = self._one(_ETL_COMMIT, text, "MQTT_ETL_COMMIT")
        self.repo = repo

    @staticmethod
    def _one(pattern: re.Pattern, text: str, what: str) -> str:
        found = pattern.findall(text)
        if len(found) != 1:
            raise SystemExit(
                f"CMakeLists.txt: expected exactly one {what}, found {len(found)}"
            )
        return found[0]

    def __str__(self) -> str:
        return (
            f"project version   {self.version_full}\n"
            f"ETL tag           {self.etl_tag}\n"
            f"ETL commit        {self.etl_commit}"
        )


def check(pins: Pins) -> list[str]:
    """Every place that restates a pin has to agree with CMakeLists.

    The failure this catches is mundane and expensive: someone bumps ETL in
    CMakeLists, forgets a cache key, and CI keeps handing the build a stale
    checkout of the *previous* dependency -- which then passes, because the
    old version still works. The bump appears to have landed and has not.
    """
    problems: list[str] = []
    workflows = sorted((pins.repo / ".github" / "workflows").glob("*.yml"))
    if not workflows:
        problems.append("no workflows found under .github/workflows")

    for wf in workflows:
        text = wf.read_text(encoding="utf-8")
        rel = wf.relative_to(pins.repo)

        # Any cache key naming ETL must name the pinned commit. Referring to it
        # through ${{ env.ETL_COMMIT }} counts -- that is checked separately
        # below, by verifying the env value itself.
        for lineno, line in enumerate(text.splitlines(), 1):
            if "key:" in line and "etl" in line.lower():
                if pins.etl_commit not in line and "env.ETL_COMMIT" not in line:
                    problems.append(
                        f"{rel}:{lineno}: ETL cache key does not reference the "
                        f"pinned commit\n      {line.strip()}"
                    )

        for lineno, line in enumerate(text.splitlines(), 1):
            m = re.match(r"\s*ETL_COMMIT:\s*(\S+)", line)
            if m and m.group(1).strip('"\'') != pins.etl_commit:
                problems.append(
                    f"{rel}:{lineno}: ETL_COMMIT is {m.group(1)}, "
                    f"CMakeLists pins {pins.etl_commit}"
                )
            m = re.match(r"\s*ETL_TAG:\s*(\S+)", line)
            if m and m.group(1).strip('"\'') != pins.etl_tag:
                problems.append(
                    f"{rel}:{lineno}: ETL_TAG is {m.group(1)}, "
                    f"CMakeLists says {pins.etl_tag}"
                )

        # The ETL version matrix says which versions the library is known to
        # build against. It has to include the pinned one, or the range is
        # asserted about versions the project does not itself use.
        if "etl-range:" in text:
            versions = re.findall(r'^\s*-\s*"(\d+\.\d+\.\d+)"', text, re.MULTILINE)
            if pins.etl_tag not in versions:
                problems.append(
                    f"{rel}: the etl-range matrix does not include the pinned "
                    f"ETL {pins.etl_tag}\n      matrix: {', '.join(versions)}"
                )

    return problems


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--check", action="store_true",
                        help="exit non-zero if anything disagrees with CMakeLists.txt")
    parser.add_argument("--field",
                        choices=("version", "version-full", "prerelease",
                                 "etl-tag", "etl-commit"),
                        help="print one value and nothing else, for use in a shell")
    args = parser.parse_args()

    pins = Pins()

    if args.field:
        print({"version": pins.version,
               "version-full": pins.version_full,
               "prerelease": pins.prerelease,
               "etl-tag": pins.etl_tag,
               "etl-commit": pins.etl_commit}[args.field])
        return 0

    print(pins)

    if not args.check:
        return 0

    problems = check(pins)
    if problems:
        print("\nDRIFT:", file=sys.stderr)
        for p in problems:
            print(f"  - {p}", file=sys.stderr)
        print(f"\n{len(problems)} disagreement(s) with CMakeLists.txt.", file=sys.stderr)
        return 1

    print("\nall workflow references agree with CMakeLists.txt")
    return 0


if __name__ == "__main__":
    sys.exit(main())

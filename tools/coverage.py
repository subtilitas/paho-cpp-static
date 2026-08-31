#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Keep the coverage figures in README.md honest about what the suite measures.

`gcovr` produces the numbers; this puts them in the README and, on every push,
checks that the ones written there still match.

    tools/coverage.py --summary coverage.json            # print what was measured
    tools/coverage.py --summary coverage.json --write    # update the README
    tools/coverage.py --summary coverage.json --check    # fail if it has drifted

`--check` rather than a bot that commits the README. A file that rewrites
itself is one nobody reads the diff of, and a coverage number is worth reading
the diff of: a drop is the interesting event, and it should arrive as a failing
build that somebody has to look at, not as a commit that quietly lands
overnight.

The check is on the three percentages, within `--tolerance` points. A band
rather than an exact match for the same reason the CI floor sits a couple of
points under where the suite actually is: a one-line change can move a
percentage by a hundredth, and a gate that fires on that teaches people to
ignore it. The band is narrow enough that a real regression still trips it.

The counts beside each percentage are refreshed by `--write` but not compared,
because they are context for the percentage rather than the claim itself.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent

START = "<!-- coverage:start -->"
END = "<!-- coverage:end -->"

#: Percentage points a figure may drift before --check fails.
DEFAULT_TOLERANCE = 0.5

#: The metrics, in the order they appear in the table.
METRICS = (
    ("Lines", "line"),
    ("Branches", "branch"),
    ("Functions", "function"),
)


class Summary:
    """The totals out of a `gcovr --json-summary` report."""

    def __init__(self, data: dict):
        version = str(data.get("gcovr/summary_format_version", ""))
        if not version.startswith("0."):
            raise SystemExit(
                f"unexpected gcovr summary format version {version!r}; "
                "this tool was written against 0.x"
            )

        self.percent: dict[str, float] = {}
        self.covered: dict[str, int] = {}
        self.total: dict[str, int] = {}

        for _, key in METRICS:
            for field, into, cast in (
                (f"{key}_percent", self.percent, float),
                (f"{key}_covered", self.covered, int),
                (f"{key}_total", self.total, int),
            ):
                if field not in data:
                    raise SystemExit(f"gcovr summary has no {field!r}")
                into[key] = cast(data[field])

    @classmethod
    def read(cls, path: Path) -> "Summary":
        try:
            text = path.read_text(encoding="utf-8")
        except OSError as exc:
            raise SystemExit(f"cannot read {path}: {exc}") from exc
        try:
            return cls(json.loads(text))
        except json.JSONDecodeError as exc:
            raise SystemExit(f"{path} is not valid JSON: {exc}") from exc

    def __str__(self) -> str:
        return "\n".join(
            f"{label:<10} {self.percent[key]:5.1f}%  "
            f"({self.covered[key]} of {self.total[key]})"
            for label, key in METRICS
        )


def render(summary: Summary) -> str:
    """The README block, markers included."""
    rows = "\n".join(
        f"| {label} | {summary.covered[key]} | {summary.total[key]} "
        f"| {summary.percent[key]:.1f}% |"
        for label, key in METRICS
    )
    return f"{START}\n| Metric | Covered | Total | Measured |\n|---|---|---|---|\n{rows}\n{END}"


def _block(readme: str) -> str:
    """The current coverage block, or a diagnosis of why there isn't one."""
    start = readme.find(START)
    end = readme.find(END)
    if start < 0 or end < 0:
        raise SystemExit(
            f"README.md has no {START} / {END} block. "
            "Add one, or run this with --write to create the contents."
        )
    if end < start:
        raise SystemExit(f"README.md has {END} before {START}")
    return readme[start : end + len(END)]


def splice(readme: str, block: str) -> str:
    return readme.replace(_block(readme), block)


def parse(block: str) -> dict[str, float]:
    """Read the percentages back out of a rendered block."""
    found = {}
    for label, key in METRICS:
        match = re.search(
            rf"^\|\s*{label}\s*\|[^|]*\|[^|]*\|\s*([0-9]+(?:\.[0-9]+)?)%\s*\|$",
            block,
            re.MULTILINE,
        )
        if match is None:
            raise SystemExit(f"README coverage block has no {label} row")
        found[key] = float(match.group(1))
    return found


def check(readme: str, summary: Summary, tolerance: float) -> list[str]:
    written = parse(_block(readme))
    problems = []
    for label, key in METRICS:
        drift = abs(written[key] - summary.percent[key])
        if drift > tolerance:
            direction = "below" if summary.percent[key] < written[key] else "above"
            problems.append(
                f"{label}: README says {written[key]:.1f}%, "
                f"the suite measures {summary.percent[key]:.1f}% "
                f"({drift:.1f} points {direction}, tolerance {tolerance:.1f})"
            )
    return problems


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--summary",
        type=Path,
        required=True,
        help="a gcovr --json-summary report",
    )
    parser.add_argument(
        "--readme",
        type=Path,
        default=REPO / "README.md",
        help="the file carrying the coverage block (default: README.md)",
    )
    parser.add_argument("--write", action="store_true", help="update the README")
    parser.add_argument("--check", action="store_true", help="fail if it has drifted")
    parser.add_argument(
        "--tolerance",
        type=float,
        default=DEFAULT_TOLERANCE,
        help=f"percentage points of drift to allow (default: {DEFAULT_TOLERANCE})",
    )
    args = parser.parse_args(argv)

    if args.write and args.check:
        parser.error("--write and --check ask for opposite things")

    summary = Summary.read(args.summary)

    if args.write:
        readme = args.readme.read_text(encoding="utf-8")
        updated = splice(readme, render(summary))
        if updated == readme:
            print(f"{args.readme.name} already current")
        else:
            args.readme.write_text(updated, encoding="utf-8")
            print(f"{args.readme.name} updated")
        return 0

    if args.check:
        problems = check(
            args.readme.read_text(encoding="utf-8"), summary, args.tolerance
        )
        if problems:
            print("coverage figures in the README have drifted:", file=sys.stderr)
            for p in problems:
                print(f"  {p}", file=sys.stderr)
            print(
                "\nRe-run the coverage build and "
                "`python3 tools/coverage.py --summary coverage.json --write`.",
                file=sys.stderr,
            )
            return 1
        print(f"README coverage figures agree with the suite (±{args.tolerance})")
        return 0

    print(summary)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

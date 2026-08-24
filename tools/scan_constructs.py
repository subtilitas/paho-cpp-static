#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Scan the library for language constructs a C++ safety subset constrains.

    tools/scan_constructs.py            # report
    tools/scan_constructs.py --check    # fail if a budget is exceeded

MISRA C++:2023 is a licensed document and checking it properly needs a
qualified tool. This is not that. What it *is* is the mechanical part: a list
of constructs the subset either forbids outright or requires a written
deviation for, and a budget for each, so the claims in docs/misra.md are
re-derived on every push instead of being a snapshot somebody took once.

The budgets are the point. Most are zero. The non-zero ones are the deviations,
each named here and argued in docs/misra.md -- so adding a second
`reinterpret_cast` fails this check and forces the argument to be made again
rather than inherited.

Comments and string literals are stripped before matching. Without that, "no
dynamic allocation" matches the word "allocation" in a comment explaining that
there is none, which is the sort of result that makes a scan worse than
nothing.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
SOURCES = ("include/mqtt/*.hpp", "src/*.cpp")


def strip_comments_and_literals(text: str) -> str:
    """Code only. A scan that matches its own explanatory comments is noise."""
    out: list[str] = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            j = text.find("\n", i)
            i = n if j < 0 else j
        elif c == "/" and i + 1 < n and text[i + 1] == "*":
            j = text.find("*/", i + 2)
            i = n if j < 0 else j + 2
        elif c in "\"'":
            quote = c
            i += 1
            while i < n and text[i] != quote:
                i += 2 if text[i] == "\\" else 1
            i += 1
            out.append('""')
        else:
            out.append(c)
            i += 1
    return "".join(out)


# (label, regex, budget, why it is constrained / why the budget is not zero)
RULES: list[tuple[str, str, int, str]] = [
    ("goto",                r"\bgoto\b", 0,
     "unstructured flow"),
    ("dynamic allocation",  r"\b(malloc|calloc|realloc|free)\s*\(", 0,
     "no heap; also enforced at run time by test_no_alloc.cpp"),
    # The trailing \b matters: without it this fires on an identifier called
    # new_entries, which is a variable and not an allocation.
    ("operator new/delete", r"\b(new|delete)\b\s*(?!\s*[;=])\[?\s*[A-Za-z_(]", 0,
     "as above. '= delete' on a special member is not this and does not match"),
    ("throw / try / catch", r"\b(throw|try|catch)\b", 0,
     "built -fno-exceptions; error.hpp #errors if the flag did not apply"),
    ("dynamic_cast",        r"\bdynamic_cast\b", 0,
     "built -fno-rtti"),
    ("const_cast",          r"\bconst_cast\b", 0,
     "casting away const defeats the type system's only real guarantee"),
    ("reinterpret_cast",    r"\breinterpret_cast\b", 2,
     "DEVIATION: char/uint8_t aliasing at the wire boundary. See docs/misra.md"),
    ("union",               r"\bunion\b", 0,
     "type punning; the fixed header is plain fields for exactly this reason"),
    ("bitfield",            r"\b\w+\s+\w+\s*:\s*[0-9]+\s*;", 0,
     "implementation-defined layout; the wire format uses shifts and masks"),
    ("volatile",            r"\bvolatile\b", 0,
     "not a concurrency primitive, and this library has no MMIO"),
    ("mutable",             r"\bmutable\b", 0,
     "const that is not const"),
    ("friend",              r"\bfriend\b", 0,
     "encapsulation escape hatch"),
    ("alloca / VLA",        r"\balloca\s*\(", 0,
     "unbounded stack; the cross job also fails on a dynamic frame size"),
    ("floating point",      r"\b(float|double)\b", 0,
     "no FP anywhere, so the archive links against any float ABI"),
    ("<cstdio> / <iostream>", r"#\s*include\s*<(cstdio|iostream|stdio\.h)>", 0,
     "no I/O in a library that does not own the transport"),
    ("multiple inheritance", r"class\s+\w+\s*:\s*[^{;]*,[^{;]*\bpublic\b", 0,
     "one abstract interface per type"),
    ("function-like macro", r"^\s*#\s*define\s+\w+\s*\(", 1,
     "DEVIATION: MQTT_WRITE. See docs/misra.md"),
]


def scan() -> tuple[list[str], list[str]]:
    code: dict[str, str] = {}
    for pattern in SOURCES:
        for path in sorted(REPO.glob(pattern)):
            code[str(path.relative_to(REPO))] = strip_comments_and_literals(
                path.read_text(encoding="utf-8"))

    if not code:
        return [], ["no sources found; run from the repository or fix SOURCES"]

    report, problems = [], []
    for label, pattern, budget, why in RULES:
        hits = [(name, text[:m.start()].count("\n") + 1)
                for name, text in code.items()
                for m in re.finditer(pattern, text, re.M)]

        mark = "ok " if len(hits) <= budget else "OVER"
        report.append(f"  {mark} {label:<22} {len(hits):>2} / {budget:<2}  {why}")

        if len(hits) > budget:
            problems.append(
                f"{label}: {len(hits)} occurrence(s), budget {budget}\n"
                + "\n".join(f"        {n}:{l}" for n, l in hits))

    report.append(f"\n  scanned {len(code)} files")
    return report, problems


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--check", action="store_true",
                        help="exit non-zero if any budget is exceeded")
    args = parser.parse_args()

    report, problems = scan()
    print("Constrained constructs in include/mqtt/ and src/:\n")
    print("\n".join(report))

    if not args.check:
        return 0

    if problems:
        print("\nOVER BUDGET:\n", file=sys.stderr)
        for p in problems:
            print(f"  - {p}\n", file=sys.stderr)
        print("A budget is a deviation that was argued for in docs/misra.md.\n"
              "Adding another one means making the argument again, not raising\n"
              "the number quietly.", file=sys.stderr)
        return 1

    print("\nevery construct is within budget")
    return 0


if __name__ == "__main__":
    sys.exit(main())

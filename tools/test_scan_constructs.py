#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Tests for tools/scan_constructs.py.

The comment stripper is the load-bearing part and gets most of the attention.
A scan that matches the word "allocation" inside a comment explaining that
there is no allocation reports a violation that is not there -- and the fix
people reach for is raising the budget, which silently disables the check.
"""

from __future__ import annotations

import importlib.util
import re
import subprocess
import sys
import unittest
from pathlib import Path

_HERE = Path(__file__).resolve().parent


def _load():
    spec = importlib.util.spec_from_file_location("scan_constructs",
                                                  _HERE / "scan_constructs.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


scan = _load()
strip = scan.strip_comments_and_literals


class Stripper(unittest.TestCase):
    def test_line_comments_go(self):
        self.assertNotIn("goto", strip("int x = 1;  // never use goto here\n"))

    def test_block_comments_go(self):
        self.assertNotIn("malloc", strip("/* we never call malloc */ int x;"))

    def test_multiline_block_comments_go(self):
        self.assertNotIn("volatile", strip("/*\n * no volatile\n * anywhere\n */\nint x;"))

    def test_string_literals_go(self):
        # A to_string() table full of enumerator names must not be mistaken
        # for the constructs those names describe.
        self.assertNotIn("throw", strip('return "would throw";'))

    def test_char_literals_go(self):
        self.assertNotIn("x", strip("char c = 'x';").replace("char", ""))

    def test_escaped_quote_does_not_end_the_literal(self):
        # If the escape is mishandled the parser falls out of the string and
        # starts matching code that is really text.
        self.assertNotIn("goto", strip(r'const char* s = "a \" goto b"; int y;'))

    def test_real_code_survives(self):
        kept = strip("int x = 1;  // comment\nreinterpret_cast<int*>(p);")
        self.assertIn("reinterpret_cast", kept)
        self.assertIn("int x = 1;", kept)


class Budgets(unittest.TestCase):
    def test_the_repository_is_within_every_budget(self):
        report, problems = scan.scan()
        self.assertEqual(problems, [], "\n".join(problems))
        self.assertTrue(report)

    def test_the_deviations_are_exactly_the_two_documented_ones(self):
        # If a third appears, docs/misra.md is out of date and this fails
        # before anybody ships a compliance claim built on it.
        deviations = [label for label, _p, budget, _w in scan.RULES if budget > 0]
        self.assertEqual(sorted(deviations),
                         ["function-like macro", "reinterpret_cast"])

    def test_docs_misra_md_documents_every_deviation(self):
        text = (scan.REPO / "docs" / "misra.md").read_text(encoding="utf-8")
        for label, _p, budget, _w in scan.RULES:
            if budget > 0:
                with self.subTest(deviation=label):
                    key = label.split()[0]
                    self.assertIn(key, text)

    def test_an_identifier_beginning_with_new_is_not_an_allocation(self):
        # `size_t new_entries = 0;` in client.hpp used to trip the
        # new/delete rule, because the pattern lacked a trailing word
        # boundary. A false positive here is worse than a missing check:
        # it teaches people to raise budgets.
        pattern = next(p for label, p, _b, _w in scan.RULES
                       if label == "operator new/delete")
        self.assertIsNone(re.search(pattern, "size_t new_entries = 0;"))
        self.assertIsNone(re.search(pattern, "Client(const Client&) = delete;"))
        self.assertIsNotNone(re.search(pattern, "char* p = new char[4];"))


class Cli(unittest.TestCase):
    def test_check_exits_zero_on_a_clean_tree(self):
        r = subprocess.run([sys.executable, str(_HERE / "scan_constructs.py"), "--check"],
                           capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("within budget", r.stdout)


if __name__ == "__main__":
    unittest.main()

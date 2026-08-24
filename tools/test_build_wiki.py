#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Unit tests for the measuring parts of build_wiki.py.

Run with:  python3 -m unittest discover -s tools -p 'test_*.py' -v

Only the measurement logic is tested, and deliberately so. If the prose
rewriter is wrong the wiki looks odd and somebody notices. If *this* code is
wrong it publishes a plausible number that is silently for the wrong target,
or off by one, and nobody notices at all -- which is the failure mode the
whole no-execution rewrite exists to make impossible.

Standard library only: this runs on a CI job whose one installed dependency is
a cross compiler.
"""

from __future__ import annotations

import importlib.util
import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

_HERE = Path(__file__).resolve().parent


def _load():
    spec = importlib.util.spec_from_file_location("build_wiki", _HERE / "build_wiki.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


bw = _load()


class ToolchainDiscovery(unittest.TestCase):
    """nm and size have to follow CXX, or they measure the wrong target.

    The failure this guards against is not a crash. `size -A` handed a foreign
    object prints a header and no sections, which parses to zero bytes -- and a
    zero would be caught, but a *partial* match would not be.
    """

    def setUp(self):
        self._saved = {k: os.environ.get(k) for k in ("CXX", "NM", "SIZE",
                                                      "MQTT_PROBE_CXXFLAGS")}
        for k in self._saved:
            os.environ.pop(k, None)

    def tearDown(self):
        for k, v in self._saved.items():
            if v is None:
                os.environ.pop(k, None)
            else:
                os.environ[k] = v

    def test_prefix_is_carried_over_from_the_compiler(self):
        for compiler, expected in [
            ("arm-none-eabi-g++", "arm-none-eabi-nm"),
            ("arm-none-eabi-gcc", "arm-none-eabi-nm"),
            ("riscv64-unknown-elf-g++", "riscv64-unknown-elf-nm"),
            ("/usr/bin/arm-none-eabi-g++", "arm-none-eabi-nm"),
        ]:
            with self.subTest(compiler=compiler):
                os.environ["CXX"] = compiler
                self.assertEqual(bw.binutil("nm"), expected)

    def test_a_host_compiler_gets_the_host_tools(self):
        for compiler in ("g++", "c++", "gcc", "cc", "/usr/bin/g++"):
            with self.subTest(compiler=compiler):
                os.environ["CXX"] = compiler
                self.assertEqual(bw.binutil("nm"), "nm")
                self.assertEqual(bw.binutil("size"), "size")

    def test_clang_is_not_mistaken_for_a_prefixed_gpp(self):
        # "clang++" ends with "g++". Suffix matching in the wrong order strips
        # three characters too few and asks for "clannm", which does not exist
        # -- so this failed loudly rather than silently, but only for Clang
        # users, and only in the cross job.
        os.environ["CXX"] = "clang++"
        self.assertEqual(bw.binutil("nm"), "nm")
        self.assertEqual(bw.binutil("size"), "size")

    def test_an_explicit_override_wins(self):
        # LLVM's tools are not named the way its compiler is, so the
        # derivation cannot work there and the escape hatch has to.
        os.environ["CXX"] = "clang++"
        os.environ["NM"] = "llvm-nm"
        self.assertEqual(bw.binutil("nm"), "llvm-nm")

    def test_probe_flags_are_split_like_a_shell_would(self):
        os.environ["MQTT_PROBE_CXXFLAGS"] = "-mcpu=cortex-m0plus -mthumb"
        self.assertEqual(bw.probe_flags(), ["-mcpu=cortex-m0plus", "-mthumb"])

    def test_probe_flags_default_to_nothing(self):
        self.assertEqual(bw.probe_flags(), [])


class SectionParsing(unittest.TestCase):
    def test_function_sections_are_summed_and_bss_is_not(self):
        # -ffunction-sections names sections ".text._ZN...", so matching on
        # equality measures zero. .bss must not count: it is RAM, and it is
        # already reported by the sizeof table.
        sample = (
            "probe.o  :\n"
            "section                     size   addr\n"
            ".text._ZN4mqtt6encodeEv      120      0\n"
            ".text._ZN4mqtt6decodeEv       80      0\n"
            ".rodata.str1.1                16      0\n"
            ".bss                        4600      0\n"
            "Total                       4816\n"
        )
        self.assertEqual(bw.section_bytes(sample), 216)

    def test_unparseable_output_measures_zero_rather_than_guessing(self):
        # The callers turn a zero into a hard error naming the tool, which is
        # how a wrong-target `size` gets reported as a toolchain problem
        # instead of as a very small library.
        self.assertEqual(bw.section_bytes("size: probe.o: file format not recognized"), 0)


class PageTemplates(unittest.TestCase):
    def test_templates_have_no_stray_braces(self):
        page = bw.FOOTPRINT_TEMPLATE.format(provenance="P", table="| r |")
        page += bw.FOOTPRINT_TAIL.format(instantiation=1234)
        self.assertIn("P", page)
        self.assertIn("1234", page)

    def test_every_profile_the_table_wants_is_in_the_probe(self):
        compact = bw.PROBE.replace(" ", "")
        for tag, _label, _cfg in bw.PROFILES:
            with self.subTest(tag=tag):
                self.assertIn(f"MQTT_PROFILE({tag},", compact)
        for field in ("size", "rx", "tx", "inflight", "subs"):
            with self.subTest(field=field):
                self.assertIn(f"__{field}", bw.PROBE)


@unittest.skipIf(shutil.which(os.environ.get("CXX", "c++")) is None,
                 "no C++ compiler on PATH")
class ProbeRoundTrip(unittest.TestCase):
    """Compile the array trick for real and read the numbers back.

    This is the load-bearing test. It runs against whatever CXX is, so in the
    cross job it proves the mechanism on the actual target toolchain -- which
    is the only place it has to work and the only place it cannot be checked
    by running the result.
    """

    SOURCE = """
#define MQTT_PROFILE(tag, sz, rx, tx, inf, subs)     \\
    char mqtt_probe__##tag##__size    [(sz)   + 1];  \\
    char mqtt_probe__##tag##__rx      [(rx)   + 1];  \\
    char mqtt_probe__##tag##__tx      [(tx)   + 1];  \\
    char mqtt_probe__##tag##__inflight[(inf)  + 1];  \\
    char mqtt_probe__##tag##__subs    [(subs) + 1];

extern "C" {
MQTT_PROFILE(sensor,    1024,  256,  256,  0,  2)
MQTT_PROFILE(gateway,  23000, 4096, 4096, 16, 32)
}
"""

    def test_values_survive_the_object_file(self):
        with tempfile.TemporaryDirectory() as tmp:
            work = Path(tmp)
            src = work / "probe.cpp"
            src.write_text(self.SOURCE, encoding="utf-8")
            obj = work / "probe.o"

            subprocess.run(
                [bw.cxx(), "-std=c++17", "-O2", "-fno-exceptions", "-fno-rtti",
                 "-ffunction-sections", "-fdata-sections", *bw.probe_flags(),
                 "-c", str(src), "-o", str(obj)],
                check=True,
            )

            got = bw.read_probe(obj)

            expected = {
                "sensor":  dict(size=1024, rx=256, tx=256, inflight=0, subs=2),
                "gateway": dict(size=23000, rx=4096, tx=4096, inflight=16, subs=32),
            }
            for tag, fields in expected.items():
                for field, value in fields.items():
                    with self.subTest(tag=tag, field=field):
                        self.assertEqual(got[f"mqtt_probe__{tag}__{field}"], value)

    def test_an_object_with_no_probe_symbols_is_an_error_not_an_empty_table(self):
        with tempfile.TemporaryDirectory() as tmp:
            work = Path(tmp)
            src = work / "empty.cpp"
            src.write_text("int unrelated = 1;\n", encoding="utf-8")
            obj = work / "empty.o"
            subprocess.run([bw.cxx(), "-std=c++17", *bw.probe_flags(),
                            "-c", str(src), "-o", str(obj)], check=True)
            with self.assertRaises(RuntimeError):
                bw.read_probe(obj)


if __name__ == "__main__":
    unittest.main()

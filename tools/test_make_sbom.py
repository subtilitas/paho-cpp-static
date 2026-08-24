#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Tests for tools/make_sbom.py.

The thing worth guarding is not that the JSON parses. It is that the document
describes the dependency the build actually fetches -- an SBOM that is wrong is
worse than one that is missing, because somebody acts on it.
"""

from __future__ import annotations

import importlib.util
import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

_HERE = Path(__file__).resolve().parent


def _load(name: str):
    spec = importlib.util.spec_from_file_location(name, _HERE / f"{name}.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


sbom = _load("make_sbom")
pins_mod = _load("pins")
PINS = pins_mod.Pins()


class CycloneDX(unittest.TestCase):
    def setUp(self):
        self.doc = sbom.cyclonedx(PINS, "abc123")

    def test_required_top_level_fields(self):
        for key in ("bomFormat", "specVersion", "serialNumber", "version", "metadata"):
            self.assertIn(key, self.doc)
        self.assertEqual(self.doc["bomFormat"], "CycloneDX")
        self.assertEqual(self.doc["specVersion"], "1.6")
        self.assertTrue(self.doc["serialNumber"].startswith("urn:uuid:"))

    def test_the_dependency_is_named_by_commit_not_by_tag(self):
        # The tag can be moved; the commit is what was compiled. An SBOM whose
        # identifier can point at different content later is not evidence.
        etl = self.doc["components"][0]
        self.assertIn(PINS.etl_commit, etl["purl"])
        self.assertEqual(etl["version"], PINS.etl_tag)

    def test_the_dependency_graph_has_the_edge(self):
        root = self.doc["metadata"]["component"]["bom-ref"]
        etl = self.doc["components"][0]["bom-ref"]
        edges = {d["ref"]: d.get("dependsOn", []) for d in self.doc["dependencies"]}
        self.assertEqual(edges[root], [etl])
        self.assertEqual(edges[etl], [])

    def test_every_component_declares_a_licence(self):
        components = [self.doc["metadata"]["component"], *self.doc["components"]]
        for c in components:
            with self.subTest(component=c["name"]):
                self.assertTrue(c["licenses"][0]["license"]["id"])


class Spdx(unittest.TestCase):
    def setUp(self):
        self.doc = sbom.spdx(PINS, "abc123")

    def test_required_top_level_fields(self):
        for key in ("spdxVersion", "dataLicense", "SPDXID", "name",
                    "documentNamespace", "creationInfo", "packages", "relationships"):
            self.assertIn(key, self.doc)
        self.assertEqual(self.doc["SPDXID"], "SPDXRef-DOCUMENT")
        self.assertEqual(self.doc["dataLicense"], "CC0-1.0")

    def test_the_download_location_pins_the_revision(self):
        etl = next(p for p in self.doc["packages"] if p["name"] == "etl")
        # SPDX's own VCS syntax. This is the field a reviewer reads to answer
        # "which bytes", and a bare repository URL does not answer it.
        self.assertEqual(etl["downloadLocation"],
                         f"git+https://github.com/ETLCPP/etl.git@{PINS.etl_commit}")

    def test_every_package_is_reachable_from_the_document(self):
        ids = {p["SPDXID"] for p in self.doc["packages"]}
        described = {r["relatedSpdxElement"] for r in self.doc["relationships"]}
        described |= {r["spdxElementId"] for r in self.doc["relationships"]}
        self.assertTrue(ids <= described, f"orphaned: {ids - described}")

    def test_no_package_leaves_a_licence_unstated(self):
        for p in self.doc["packages"]:
            with self.subTest(package=p["name"]):
                self.assertNotEqual(p["licenseConcluded"], "NOASSERTION")
                self.assertNotEqual(p["licenseDeclared"], "NOASSERTION")


class Reproducibility(unittest.TestCase):
    def test_source_date_epoch_is_honoured(self):
        saved = os.environ.get("SOURCE_DATE_EPOCH")
        try:
            os.environ["SOURCE_DATE_EPOCH"] = "1700000000"
            self.assertEqual(sbom.timestamp(), "2023-11-14T22:13:20Z")
        finally:
            if saved is None:
                os.environ.pop("SOURCE_DATE_EPOCH", None)
            else:
                os.environ["SOURCE_DATE_EPOCH"] = saved

    def test_two_runs_at_a_fixed_epoch_are_byte_identical(self):
        # A release asset that differs on every rebuild cannot be checked
        # against the one that shipped, which is most of what an SBOM is for.
        env = {**os.environ, "SOURCE_DATE_EPOCH": "1700000000"}
        outputs = []
        for _ in range(2):
            with tempfile.TemporaryDirectory() as t:
                subprocess.run([sys.executable, str(_HERE / "make_sbom.py"),
                                "--all", "--outdir", t],
                               check=True, capture_output=True, env=env)
                outputs.append({p.name: p.read_bytes()
                                for p in sorted(Path(t).iterdir())})
        self.assertEqual(sorted(outputs[0]), ["sbom.cdx.json", "sbom.spdx.json"])
        self.assertEqual(outputs[0], outputs[1])


class EndToEnd(unittest.TestCase):
    def test_the_cli_writes_valid_json_for_every_format(self):
        with tempfile.TemporaryDirectory() as t:
            subprocess.run([sys.executable, str(_HERE / "make_sbom.py"),
                            "--all", "--outdir", t], check=True, capture_output=True)
            for name in ("sbom.cdx.json", "sbom.spdx.json"):
                with self.subTest(name=name):
                    doc = json.loads((Path(t) / name).read_text(encoding="utf-8"))
                    self.assertIsInstance(doc, dict)
                    # Whichever format, the pinned commit has to appear.
                    self.assertIn(PINS.etl_commit, json.dumps(doc))


if __name__ == "__main__":
    unittest.main()

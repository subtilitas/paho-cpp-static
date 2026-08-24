#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Generate a software bill of materials for paho-cpp-static.

    tools/make_sbom.py --all --outdir sbom/
    tools/make_sbom.py --format cyclonedx --output sbom.cdx.json
    tools/make_sbom.py --format spdx      --output sbom.spdx.json

Two formats, because the two organisations most likely to ask for one ask for
different ones: CycloneDX is what Siemens' clearing pipeline ingests, SPDX is
what Bosch's supplier terms require. Emitting both costs a few lines here and
saves an argument later.

Everything comes from tools/pins.py, which reads CMakeLists.txt. Nothing is
restated, so the SBOM cannot describe a dependency the build does not use --
which is the failure that makes an SBOM worse than none, because it is
believed.

Why hand-rolled rather than a generator. This project has no package manager
to introspect: the sole dependency arrives through CMake FetchContent, which
every off-the-shelf SBOM tool reads as "no dependencies". A tool that
confidently emits an empty component list is the worst available answer, so
the dependency graph is written out from the pin instead. It is one edge.

Reproducibility: SOURCE_DATE_EPOCH is honoured for the timestamp, and the
CycloneDX serial number is derived from the content rather than random, so
generating twice from the same commit produces byte-identical documents.
"""

from __future__ import annotations

import argparse
import datetime as _dt
import importlib.util
import json
import os
import subprocess
import sys
import uuid
from pathlib import Path

_HERE = Path(__file__).resolve().parent


def _load_pins():
    spec = importlib.util.spec_from_file_location("pins", _HERE / "pins.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


pins_mod = _load_pins()

SUPPLIER = "subtilitas"
REPO_URL = "https://github.com/subtilitas/paho-cpp-static"
ETL_REPO = "https://github.com/ETLCPP/etl"

# A UUID namespace of our own, so serial numbers are stable across runs and
# distinct from anybody else's.
_NS = uuid.UUID("6f1f6b1e-6a2a-5f7e-9d3f-1f0f5a2b3c4d")


def timestamp() -> str:
    """UTC, honouring SOURCE_DATE_EPOCH so a rebuild is byte-identical."""
    epoch = os.environ.get("SOURCE_DATE_EPOCH")
    when = (_dt.datetime.fromtimestamp(int(epoch), _dt.timezone.utc) if epoch
            else _dt.datetime.now(_dt.timezone.utc))
    return when.replace(microsecond=0).isoformat().replace("+00:00", "Z")


def git_commit(repo: Path) -> str | None:
    try:
        out = subprocess.run(["git", "-C", str(repo), "rev-parse", "HEAD"],
                             capture_output=True, text=True, check=True)
        return out.stdout.strip() or None
    except (subprocess.CalledProcessError, FileNotFoundError):
        return None            # a source archive with no .git is not an error


def cyclonedx(pins, commit: str | None) -> dict:
    """CycloneDX 1.6. Siemens' continuous-clearing consumes this shape."""
    root_purl = f"pkg:github/subtilitas/paho-cpp-static@{pins.version}"
    # The dependency's PURL names the commit rather than the tag. The commit is
    # what the build fetches and the only identifier that cannot be moved --
    # which is the entire question an SBOM reader is asking.
    etl_purl = f"pkg:github/ETLCPP/etl@{pins.etl_commit}"

    serial = uuid.uuid5(_NS, f"{pins.version}|{pins.etl_commit}|{commit or ''}")

    root = {
        "type": "library",
        "bom-ref": root_purl,
        "name": "paho-cpp-static",
        "version": pins.version,
        "description": "Statically allocated MQTT 3.1.1 client in C++17",
        "scope": "required",
        "licenses": [{"license": {"id": "MIT"}}],
        "purl": root_purl,
        "supplier": {"name": SUPPLIER, "url": [REPO_URL]},
        "externalReferences": [
            {"type": "vcs", "url": f"{REPO_URL}.git"},
            {"type": "website", "url": REPO_URL},
            {"type": "issue-tracker", "url": f"{REPO_URL}/issues"},
            {"type": "documentation", "url": f"{REPO_URL}/wiki"},
        ],
    }
    if commit:
        root["properties"] = [{"name": "mqtt:build:commit", "value": commit}]

    etl = {
        "type": "library",
        "bom-ref": etl_purl,
        "name": "etl",
        "group": "ETLCPP",
        "version": pins.etl_tag,
        "description": "Embedded Template Library — fixed-capacity containers "
                       "and byte-stream serialization",
        "scope": "required",
        "licenses": [{"license": {"id": "MIT"}}],
        "purl": etl_purl,
        "supplier": {"name": "John Wellbelove and contributors", "url": [ETL_REPO]},
        "externalReferences": [
            {"type": "vcs", "url": f"{ETL_REPO}.git"},
            {"type": "website", "url": "https://www.etlcpp.com/"},
        ],
        "properties": [
            {"name": "mqtt:pin:tag", "value": pins.etl_tag},
            {"name": "mqtt:pin:commit", "value": pins.etl_commit},
            # Header-only and consumed at build time: it is inside the
            # consumer's binary, not shipped beside it. Worth stating, because
            # it changes who has to patch a vulnerability in it.
            {"name": "mqtt:pin:linkage", "value": "source (header-only, compiled in)"},
        ],
    }

    return {
        "bomFormat": "CycloneDX",
        "specVersion": "1.6",
        "serialNumber": f"urn:uuid:{serial}",
        "version": 1,
        "metadata": {
            "timestamp": timestamp(),
            "tools": {"components": [{
                "type": "application",
                "name": "make_sbom.py",
                "version": pins.version,
                "author": SUPPLIER,
            }]},
            "component": root,
            "supplier": {"name": SUPPLIER, "url": [REPO_URL]},
            "licenses": [{"license": {"id": "MIT"}}],
        },
        "components": [etl],
        "dependencies": [
            {"ref": root_purl, "dependsOn": [etl_purl]},
            {"ref": etl_purl, "dependsOn": []},
        ],
    }


def spdx(pins, commit: str | None) -> dict:
    """SPDX 2.3 JSON. Bosch's supplier terms ask for SPDX; 2.3 is the current
    minor and reads anywhere 2.2 does."""
    root_id = "SPDXRef-Package-paho-cpp-static"
    etl_id = "SPDXRef-Package-etl"

    # SPDX's own syntax for "this exact revision of this repository", which is
    # the whole reason for pinning by commit.
    etl_download = f"git+{ETL_REPO}.git@{pins.etl_commit}"
    root_download = (f"git+{REPO_URL}.git@{commit}" if commit
                     else f"{REPO_URL}/releases/tag/v{pins.version}")

    namespace = (f"{REPO_URL}/spdx/"
                 f"{pins.version}-{uuid.uuid5(_NS, pins.etl_commit + (commit or ''))}")

    return {
        "spdxVersion": "SPDX-2.3",
        "dataLicense": "CC0-1.0",
        "SPDXID": "SPDXRef-DOCUMENT",
        "name": f"paho-cpp-static-{pins.version}",
        "documentNamespace": namespace,
        "creationInfo": {
            "created": timestamp(),
            "creators": [f"Tool: make_sbom.py-{pins.version}",
                         f"Organization: {SUPPLIER}"],
            "licenseListVersion": "3.23",
        },
        "packages": [
            {
                "SPDXID": root_id,
                "name": "paho-cpp-static",
                "versionInfo": pins.version,
                "downloadLocation": root_download,
                "filesAnalyzed": False,
                "homepage": REPO_URL,
                "licenseConcluded": "MIT",
                "licenseDeclared": "MIT",
                "copyrightText": "NOASSERTION",
                "supplier": f"Organization: {SUPPLIER}",
                "externalRefs": [{
                    "referenceCategory": "PACKAGE-MANAGER",
                    "referenceType": "purl",
                    "referenceLocator":
                        f"pkg:github/subtilitas/paho-cpp-static@{pins.version}",
                }],
            },
            {
                "SPDXID": etl_id,
                "name": "etl",
                "versionInfo": pins.etl_tag,
                "downloadLocation": etl_download,
                "filesAnalyzed": False,
                "homepage": "https://www.etlcpp.com/",
                "licenseConcluded": "MIT",
                "licenseDeclared": "MIT",
                "copyrightText": "Copyright (c) John Wellbelove and contributors",
                "supplier": "Person: John Wellbelove",
                "comment": (f"Pinned to commit {pins.etl_commit}, which is release "
                            f"{pins.etl_tag}. Header-only and compiled into the "
                            f"consuming binary rather than shipped alongside it."),
                "externalRefs": [{
                    "referenceCategory": "PACKAGE-MANAGER",
                    "referenceType": "purl",
                    "referenceLocator": f"pkg:github/ETLCPP/etl@{pins.etl_commit}",
                }],
            },
        ],
        "relationships": [
            {"spdxElementId": "SPDXRef-DOCUMENT",
             "relationshipType": "DESCRIBES",
             "relatedSpdxElement": root_id},
            {"spdxElementId": root_id,
             "relationshipType": "DEPENDS_ON",
             "relatedSpdxElement": etl_id},
        ],
    }


FORMATS = {
    "cyclonedx": (cyclonedx, "sbom.cdx.json"),
    "spdx": (spdx, "sbom.spdx.json"),
}


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--format", choices=sorted(FORMATS))
    parser.add_argument("--all", action="store_true", help="emit every format")
    parser.add_argument("--output", help="file to write (single format only)")
    parser.add_argument("--outdir", default=".", help="directory for --all")
    args = parser.parse_args()

    if not args.format and not args.all:
        parser.error("choose --format or --all")
    if args.output and args.all:
        parser.error("--output takes one file; use --outdir with --all")

    pins = pins_mod.Pins()
    commit = git_commit(pins.repo)

    wanted = sorted(FORMATS) if args.all else [args.format]
    outdir = Path(args.outdir)
    outdir.mkdir(parents=True, exist_ok=True)

    for name in wanted:
        build, default_name = FORMATS[name]
        document = build(pins, commit)
        target = Path(args.output) if args.output else outdir / default_name
        # A trailing newline, and sorted keys nowhere: SPDX and CycloneDX both
        # have a conventional field order that sorting would scramble.
        target.write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")
        print(f"  {name:10} -> {target}")

    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Build the GitHub wiki content for paho-cpp-static.

Three kinds of page end up in the wiki:

  * Prose carried over from docs/ and README.md, with links rewritten so they
    resolve inside a wiki (which is a flat namespace, not a directory tree).

  * Pages generated from the source: the API reference comes out of the header
    comments via Doxygen, the memory table comes from actually compiling and
    measuring sizeof, and the test inventory comes from running the suite.

  * Navigation furniture: _Sidebar and _Footer.

The second group is the point of doing this in CI at all. A hand-written table
of struct sizes is wrong the first time someone changes a buffer default, and
nobody notices for six months. These are measured on every push.

Usage:
    tools/build_wiki.py --out wiki [--repo-slug owner/name] [--ref main]
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
import textwrap
import xml.etree.ElementTree as ET
from pathlib import Path

# --------------------------------------------------------------------------
# Page map
# --------------------------------------------------------------------------

# Repository path -> wiki page name. Order is the sidebar order.
PROSE_PAGES = [
    ("README.md", "Home", "Overview"),
    ("docs/getting-started.md", "Getting-Started", "Getting started"),
    ("docs/architecture.md", "Architecture", "Architecture"),
    ("docs/porting.md", "Porting", "Porting guide"),
    ("docs/configuration.md", "Configuration", "Configuration"),
    ("docs/comparison-with-paho.md", "Comparison-with-Paho", "Comparison with Paho"),
    ("NOTICE.md", "Notices", "Licence and notices"),
]

GENERATED_PAGES = [
    ("API-Reference", "API reference"),
    ("Memory-Footprint", "Memory footprint"),
    ("Test-Inventory", "Test inventory"),
]

# Files that exist in the repository but not as wiki pages. Links to these are
# rewritten to point at the repository on GitHub.
REPO_ONLY = re.compile(
    r"^(LICENSE|CMakeLists\.txt|\.github/.*|src/.*|include/.*|tests/.*|examples/.*|tools/.*)$"
)


def run(cmd, cwd=None, check=True, capture=True):
    """Run a command, returning stdout. Raises on failure unless check=False."""
    result = subprocess.run(
        cmd,
        cwd=cwd,
        check=False,
        text=True,
        stdout=subprocess.PIPE if capture else None,
        stderr=subprocess.STDOUT if capture else None,
    )
    if check and result.returncode != 0:
        raise RuntimeError(
            f"command failed ({result.returncode}): {' '.join(map(str, cmd))}\n"
            f"{result.stdout or ''}"
        )
    return result.stdout or ""


# --------------------------------------------------------------------------
# Prose pages
# --------------------------------------------------------------------------


def rewrite_links(text: str, repo_slug: str, ref: str) -> str:
    """Point markdown links at wiki pages or at the repository, as appropriate.

    A wiki has no directories, so `docs/porting.md` has to become `Porting`.
    Anything that only exists in the repository becomes an absolute blob URL,
    because a relative link from a wiki page resolves against the wiki.
    """
    page_for_path = {src: page for src, page, _ in PROSE_PAGES}
    blob = f"https://github.com/{repo_slug}/blob/{ref}"

    title_for_path = {src: title for src, _, title in PROSE_PAGES}

    def replace(match: re.Match) -> str:
        label, target = match.group(1), match.group(2)

        # Leave absolute links, anchors and mail links alone.
        if target.startswith(("http://", "https://", "#", "mailto:")):
            return match.group(0)

        path, _, anchor = target.partition("#")
        path = path.lstrip("./")

        if path in page_for_path:
            page = page_for_path[path]
            # In the repository "[docs/porting.md](docs/porting.md)" is a
            # perfectly good link. On a wiki it reads as a stray filename, so
            # promote the label to the page title.
            if label.strip() == target.strip():
                label = title_for_path[path]
            return f"[{label}]({page}{'#' + anchor if anchor else ''})"

        if REPO_ONLY.match(path):
            return f"[{label}]({blob}/{path}{'#' + anchor if anchor else ''})"

        return match.group(0)

    return re.sub(r"\[([^\]]+)\]\(([^)]+)\)", replace, text)


def build_prose(repo: Path, out: Path, repo_slug: str, ref: str) -> list[str]:
    written = []
    for src, page, _title in PROSE_PAGES:
        source = repo / src
        if not source.exists():
            print(f"  skip {src} (absent)")
            continue
        text = source.read_text(encoding="utf-8")

        # The CI badge is noise on a wiki page; it belongs on the repo front page.
        text = re.sub(r"^\[!\[CI\].*\n", "", text, flags=re.MULTILINE)

        out.joinpath(f"{page}.md").write_text(
            rewrite_links(text, repo_slug, ref), encoding="utf-8"
        )
        written.append(page)
        print(f"  {src} -> {page}.md")
    return written


# --------------------------------------------------------------------------
# API reference, via Doxygen XML
# --------------------------------------------------------------------------

DOXYFILE = """
PROJECT_NAME           = "paho-cpp-static"
INPUT                  = {input}
FILE_PATTERNS          = *.hpp
RECURSIVE              = YES
OUTPUT_DIRECTORY       = {output}
GENERATE_HTML          = NO
GENERATE_LATEX         = NO
GENERATE_XML           = YES
XML_OUTPUT             = xml
XML_PROGRAMLISTING     = NO
EXTRACT_ALL            = YES
EXTRACT_PRIVATE        = NO
EXTRACT_STATIC         = YES
HIDE_UNDOC_MEMBERS     = NO
ENABLE_PREPROCESSING   = YES
MACRO_EXPANSION        = NO
JAVADOC_AUTOBRIEF      = YES
QUIET                  = YES
WARNINGS               = NO
WARN_IF_UNDOCUMENTED   = NO
WARN_IF_DOC_ERROR      = NO
"""


PARAM_LABEL = {
    "param": "Parameter",
    "templateparam": "Template parameter",
    "retval": "Returns",
    "exception": "Throws",
}


def _codeline_text(line: ET.Element) -> str:
    """Text of one <codeline>, preserving indentation.

    Doxygen encodes every space inside a listing as an empty <sp/> element, so
    itertext() silently drops all of them and `struct MyConfig : Base` comes out
    as `structMyConfig:Base`. They have to be expanded by hand.
    """
    parts: list[str] = []

    def rec(node: ET.Element, root: bool = False):
        if node.tag == "sp":
            parts.append(" " * max(1, int(node.get("value") or 1)))
        elif node.text:
            parts.append(node.text)
        for child in node:
            rec(child)
        # The tail of the <codeline> itself is the newline separating lines;
        # the caller supplies those.
        if not root and node.tail:
            parts.append(node.tail)

    rec(line, root=True)
    return "".join(parts)


def _code_block(el: ET.Element) -> str:
    """Render <programlisting> as a fenced block.

    Doxygen wraps each source line in <codeline> and puts a newline in that
    element's tail as well. Walking it generically therefore doubles every
    newline and double-spaces the sample, so lines are collected explicitly.
    """
    lines = [_codeline_text(cl) for cl in el.findall("codeline")]
    if not lines and el.text:
        lines = el.text.splitlines()
    body = "\n".join(lines).strip("\n")
    return f"\n\n```cpp\n{body}\n```\n\n"


def _param_list(el: ET.Element) -> str:
    """Render <parameterlist> as labelled bullets rather than a bare name."""
    label = PARAM_LABEL.get(el.get("kind", "param"), "Parameter")
    items = []
    for item in el.findall("parameteritem"):
        names = ", ".join(
            "".join(n.itertext()).strip()
            for n in item.iter("parametername")
        )
        desc = " ".join(xml_text(item.find("parameterdescription")).split())
        items.append(f"- *{label}* `{names}` — {desc}" if desc
                     else f"- *{label}* `{names}`")
    return "\n\n" + "\n".join(items) + "\n\n" if items else ""


def xml_text(node: ET.Element | None) -> str:
    """Flatten a Doxygen description node into markdown.

    Doxygen's XML is deeply nested; this handles the subset the headers in this
    project actually use and degrades to plain text for anything else.
    """
    if node is None:
        return ""

    parts: list[str] = []

    def walk(el: ET.Element):
        tag = el.tag

        # These two are rendered wholesale; recursing into them produces
        # mangled output.
        if tag == "programlisting":
            parts.append(_code_block(el))
            if el.tail:
                parts.append(el.tail)
            return
        if tag == "parameterlist":
            parts.append(_param_list(el))
            if el.tail:
                parts.append(el.tail)
            return

        if tag == "para":
            parts.append("\n\n")
        elif tag == "computeroutput":
            parts.append("`")
        elif tag in ("bold", "b"):
            parts.append("**")
        elif tag in ("emphasis", "i"):
            parts.append("*")
        elif tag == "itemizedlist":
            parts.append("\n")
        elif tag == "listitem":
            parts.append("\n- ")
        elif tag == "linebreak":
            parts.append("\n")
        elif tag == "verbatim":
            parts.append("\n\n```\n")
        elif tag == "sp":
            parts.append(" ")

        if el.text:
            parts.append(el.text)

        for child in el:
            walk(child)

        if tag == "computeroutput":
            parts.append("`")
        elif tag in ("bold", "b"):
            parts.append("**")
        elif tag in ("emphasis", "i"):
            parts.append("*")
        elif tag == "verbatim":
            parts.append("```\n\n")
        elif tag == "para":
            parts.append("\n\n")

        if el.tail:
            parts.append(el.tail)

    for child in node:
        walk(child)
    if node.text:
        parts.insert(0, node.text)

    text = "".join(parts)
    text = re.sub(r"\n{3,}", "\n\n", text)
    return text.strip()


def describe_member(member: ET.Element) -> dict:
    """Pull the bits of a <memberdef> worth printing."""
    kind = member.get("kind", "")
    name = (member.findtext("name") or "").strip()
    definition = (member.findtext("definition") or "").strip()
    args = (member.findtext("argsstring") or "").strip()

    brief = xml_text(member.find("briefdescription"))
    detail_node = member.find("detaileddescription")

    # Parameters and @retval entries are rendered by xml_text() as labelled
    # bullets, so they are deliberately not extracted into separate tables here
    # -- doing both printed everything twice.
    returns: list[str] = []
    if detail_node is not None:
        for sect in detail_node.iter("simplesect"):
            if sect.get("kind") == "return":
                returns.append(xml_text(sect))

    # Remove the structured sections from the prose so they are not printed twice.
    detail = xml_text(detail_node)
    for chunk in returns:
        detail = detail.replace(chunk, "")
    detail = re.sub(r"\n{3,}", "\n\n", detail).strip()

    signature = re.sub(r"\s+", " ", (definition + args) if definition else (name + args))
    if kind == "enum":
        values = [
            (
                (v.findtext("name") or "").strip(),
                xml_text(v.find("briefdescription")),
            )
            for v in member.findall("enumvalue")
        ]
    else:
        values = []

    return {
        "kind": kind,
        "name": name,
        "signature": signature,
        "brief": brief,
        "detail": detail,
        "returns": returns,
        "values": values,
    }


def render_member(m: dict) -> str:
    out: list[str] = []

    if m["kind"] == "enum":
        out.append(f"#### `{m['name']}`\n")
        if m["brief"]:
            out.append(m["brief"] + "\n")
        if m["detail"]:
            out.append(m["detail"] + "\n")
        if m["values"]:
            out.append("| Value | Meaning |")
            out.append("|---|---|")
            for vname, vbrief in m["values"]:
                out.append(f"| `{vname}` | {vbrief.replace(chr(10), ' ') or '—'} |")
            out.append("")
        return "\n".join(out)

    out.append(f"#### `{m['name']}`\n")
    out.append("```cpp")
    out.append(m["signature"])
    out.append("```\n")
    if m["brief"]:
        out.append(m["brief"] + "\n")
    if m["detail"]:
        out.append(m["detail"] + "\n")
    for r in m["returns"]:
        out.append(f"**Returns** {r}\n")
    return "\n".join(out)


def build_api(repo: Path, out: Path, workdir: Path) -> None:
    xml_dir = workdir / "xml"
    doxyfile = workdir / "Doxyfile"
    doxyfile.write_text(
        DOXYFILE.format(input=repo / "include" / "mqtt", output=workdir),
        encoding="utf-8",
    )
    run(["doxygen", str(doxyfile)])

    index = ET.parse(xml_dir / "index.xml").getroot()

    sections: list[str] = []
    order = {"struct": 1, "class": 0, "namespace": 2}

    compounds = []
    for compound in index.findall("compound"):
        kind = compound.get("kind", "")
        if kind not in ("class", "struct"):
            continue
        name = compound.findtext("name") or ""
        if not name.startswith("mqtt::"):
            continue
        compounds.append((order.get(kind, 9), name, compound.get("refid")))

    for _, name, refid in sorted(compounds, key=lambda c: (c[0], c[1])):
        root = ET.parse(xml_dir / f"{refid}.xml").getroot()
        cd = root.find("compounddef")
        if cd is None:
            continue

        brief = xml_text(cd.find("briefdescription"))
        detail = xml_text(cd.find("detaileddescription"))

        sections.append(f"## `{name}`\n")
        if brief:
            sections.append(brief + "\n")
        if detail:
            sections.append(detail + "\n")

        members = []
        for sd in cd.findall("sectiondef"):
            if sd.get("kind", "").startswith("private"):
                continue
            for md in sd.findall("memberdef"):
                if md.get("prot") != "public":
                    continue
                info = describe_member(md)
                # Deleted copy operations and other compiler ceremony carry no
                # information for a reader; a reference full of "=delete" lines
                # is harder to scan, not more complete.
                if "=delete" in info["signature"].replace(" ", ""):
                    continue
                members.append(info)

        if members:
            sections.append("### Members\n")
            for m in members:
                sections.append(render_member(m))

    # Free functions and enums living directly in namespace mqtt.
    for compound in index.findall("compound"):
        if compound.get("kind") != "namespace":
            continue
        name = compound.findtext("name") or ""
        if name not in ("mqtt", "mqtt::codec"):
            continue
        root = ET.parse(xml_dir / f"{compound.get('refid')}.xml").getroot()
        cd = root.find("compounddef")
        if cd is None:
            continue

        free = []
        for sd in cd.findall("sectiondef"):
            for md in sd.findall("memberdef"):
                if md.get("kind") in ("function", "enum"):
                    free.append(describe_member(md))
        if free:
            sections.append(f"## Namespace `{name}`\n")
            for m in sorted(free, key=lambda x: (x["kind"] != "enum", x["name"])):
                sections.append(render_member(m))

    header = textwrap.dedent(
        """\
        # API reference

        Generated from the header comments in `include/mqtt/` by Doxygen on every
        push. If something here disagrees with the headers, the headers win and
        this page is stale — please open an issue.

        For how the pieces fit together, start with [Architecture](Architecture);
        for what you have to implement, see the [Porting guide](Porting).

        """
    )
    out.joinpath("API-Reference.md").write_text(
        header + "\n".join(sections) + "\n", encoding="utf-8"
    )
    print(f"  API-Reference.md ({len(compounds)} types)")


# --------------------------------------------------------------------------
# Memory footprint, measured rather than asserted
# --------------------------------------------------------------------------

PROBE = r"""
#include <cstdio>
#include "mqtt/client.hpp"

struct Sensor : mqtt::DefaultConfig {
    static constexpr size_t rx_buffer_size         = 256;
    static constexpr size_t tx_buffer_size         = 256;
    static constexpr size_t max_topic_len          = 48;
    static constexpr size_t max_inflight_out       = 0;
    static constexpr size_t max_persisted_msg_size = 0;
    static constexpr size_t max_inflight_in        = 1;
    static constexpr size_t max_subscriptions      = 2;
    static constexpr size_t max_pending_acks       = 1;
    static constexpr size_t max_topics_per_request = 1;
};
struct Reliable : mqtt::DefaultConfig {
    static constexpr size_t rx_buffer_size         = 512;
    static constexpr size_t tx_buffer_size         = 1024;
    static constexpr size_t max_inflight_out       = 8;
    static constexpr size_t max_persisted_msg_size = 128;
    static constexpr size_t max_subscriptions      = 4;
};
struct Gateway : mqtt::DefaultConfig {
    static constexpr size_t rx_buffer_size         = 4096;
    static constexpr size_t tx_buffer_size         = 4096;
    static constexpr size_t max_topic_len          = 128;
    static constexpr size_t max_inflight_out       = 16;
    static constexpr size_t max_persisted_msg_size = 512;
    static constexpr size_t max_inflight_in        = 16;
    static constexpr size_t max_subscriptions      = 32;
    static constexpr size_t max_pending_acks       = 8;
    static constexpr size_t max_topics_per_request = 8;
};

template <typename C> void row(const char* name) {
    std::printf("| %s | %zu | %zu | %zu | %zu | **%zu** |\n", name,
                C::rx_buffer_size, C::tx_buffer_size,
                C::max_inflight_out, C::max_subscriptions,
                sizeof(mqtt::Client<C>));
}

int main() {
    row<Sensor>("Sensor (QoS 0 only)");
    row<mqtt::DefaultConfig>("DefaultConfig");
    row<Reliable>("Reliable sensor");
    row<Gateway>("Gateway");
    return 0;
}
"""


FOOTPRINT_TEMPLATE = """\
# Memory footprint

Every buffer and table in the client is sized from its `Config` type, so
`sizeof(Client<Cfg>)` is the whole RAM cost — there are no hidden allocations
behind a pointer. The numbers below are **measured on every push**, not written
down once and left to rot.

Measured with `{compiler}` on `{machine}`. Treat them as indicative for a
Cortex-M rather than a promise; the relative shape is what transfers.

## RAM: `sizeof(Client<Cfg>)`

| Configuration | rx buf | tx buf | inflight | subs | Total bytes |
|---|---|---|---|---|---|
{table}

The dominant term for QoS > 0 builds is
`max_inflight_out × max_persisted_msg_size` — the per-slot buffers holding
serialized packets for retransmission. Setting `max_inflight_out` to `0`
forbids QoS > 0 publishing and reclaims all of it, which is why the sensor
profile is so much smaller. See [Configuration](Configuration) for the
reasoning.

## Flash: non-template core

Compiled `-Os`, `.text` plus `.rodata` per object.

| Object | Bytes |
|---|---|
"""

INSTANTIATION_PROBE = r"""
#include "mqtt/client.hpp"
struct Cfg : mqtt::DefaultConfig {};
static mqtt::Transport* t; static mqtt::Clock* c;
static mqtt::Client<Cfg>& inst() { static mqtt::Client<Cfg> x{*t, *c}; return x; }
extern "C" void use_every_entry_point()
{
    auto& k = inst();
    mqtt::ConnectOptions o; o.client_id = etl::string_view("x");
    k.connect(o);
    k.step();
    k.publish(etl::string_view("a"), etl::string_view("b"), mqtt::QoS::ExactlyOnce);
    k.subscribe(etl::string_view("a/#"));
    k.unsubscribe(etl::string_view("a/#"));
    k.disconnect();
    k.abort();
}
"""

FOOTPRINT_TAIL = """
A `Client<Cfg>` instantiation that touches every public entry point adds
{instantiation} bytes of code on top of the figures above — less in a real
application, because the linker drops whatever you never call.

## Budgeting in your own build

The cost is a compile-time constant, so it can be asserted:

```cpp
static_assert(sizeof(mqtt::Client<MyConfig>) <= 4096,
              "MQTT client budget exceeded");
```

That is the cheapest guard against someone quietly raising a buffer size past
what the target can afford.
"""


def find_etl_include(repo: Path, explicit: str | None) -> Path | None:
    if explicit:
        return Path(explicit)
    for candidate in repo.glob("build*/_deps/etl-src/include"):
        return candidate
    return None


def build_footprint(repo: Path, out: Path, workdir: Path, etl_include: Path | None) -> None:
    if etl_include is None or not etl_include.exists():
        print("  Memory-Footprint.md skipped (no ETL include path)")
        return

    probe = workdir / "probe.cpp"
    probe.write_text(PROBE, encoding="utf-8")
    binary = workdir / "probe"

    run([
        os.environ.get("CXX", "c++"), "-std=c++17", "-O2",
        "-fno-exceptions", "-fno-rtti",
        f"-I{repo / 'include'}", "-isystem", str(etl_include),
        str(probe), "-o", str(binary),
    ])
    table = run([str(binary)])

    # Object sizes for the non-template core.
    sizes = []
    for src in sorted((repo / "src").glob("*.cpp")):
        obj = workdir / (src.stem + ".o")
        run([
            os.environ.get("CXX", "c++"), "-std=c++17", "-Os",
            "-fno-exceptions", "-fno-rtti", "-ffunction-sections", "-fdata-sections",
            f"-I{repo / 'include'}", "-isystem", str(etl_include),
            "-c", str(src), "-o", str(obj),
        ])
        out_txt = run(["size", "-A", str(obj)])
        text = 0
        for line in out_txt.splitlines():
            fields = line.split()
            # -ffunction-sections names every section ".text._ZN..." rather than
            # plain ".text", so this has to match on the prefix. Comparing for
            # equality silently measures zero.
            if len(fields) >= 2 and fields[0].startswith((".text", ".rodata")):
                try:
                    text += int(fields[1])
                except ValueError:
                    pass
        sizes.append((src.name, text))
        if text == 0:
            raise RuntimeError(
                f"measured 0 bytes of code in {src.name}; the `size -A` output "
                f"was not understood:\n{out_txt}"
            )

    compiler = run([os.environ.get("CXX", "c++"), "--version"]).splitlines()[0]
    machine = run(["uname", "-m"]).strip()

    # Note: the template is deliberately not indented and does not go through
    # textwrap.dedent. dedent computes a common leading-whitespace prefix across
    # every line, and an interpolated multi-line value (the table) has none --
    # so dedent silently gives up and the whole page renders as a code block.
    page = FOOTPRINT_TEMPLATE.format(
        compiler=compiler,
        machine=machine,
        table=table.strip(),
    )

    for name, size in sizes:
        page += f"| `{name}` | {size} |\n"
    page += f"| **total** | **{sum(s for _, s in sizes)}** |\n"

    # Measure the template instantiation too, rather than asserting a number
    # that nobody re-checks.
    inst_src = workdir / "inst.cpp"
    inst_src.write_text(INSTANTIATION_PROBE, encoding="utf-8")
    inst_obj = workdir / "inst.o"
    run([
        os.environ.get("CXX", "c++"), "-std=c++17", "-Os",
        "-fno-exceptions", "-fno-rtti", "-ffunction-sections", "-fdata-sections",
        f"-I{repo / 'include'}", "-isystem", str(etl_include),
        "-c", str(inst_src), "-o", str(inst_obj),
    ])
    inst_bytes = 0
    for line in run(["size", "-A", str(inst_obj)]).splitlines():
        fields = line.split()
        if len(fields) >= 2 and fields[0].startswith((".text", ".rodata")):
            try:
                inst_bytes += int(fields[1])
            except ValueError:
                pass

    page += FOOTPRINT_TAIL.format(instantiation=inst_bytes)

    out.joinpath("Memory-Footprint.md").write_text(page, encoding="utf-8")
    print(f"  Memory-Footprint.md ({len(sizes)} objects measured)")


# --------------------------------------------------------------------------
# Test inventory
# --------------------------------------------------------------------------

SUITE_BLURB = {
    "test_packet.cpp": "Variable byte integers against the spec's boundary table, "
                       "and fixed-header flag validation.",
    "test_codec.cpp": "Encode/decode round trips, golden byte sequences, and "
                      "malformed input.",
    "test_topic.cpp": "Wildcard matching, using the examples from MQTT 3.1.1 "
                      "section 4.7 verbatim.",
    "test_client.cpp": "Handshakes, all three QoS flows, keep-alive, "
                       "fragmentation, back-pressure and teardown.",
    "test_no_alloc.cpp": "Global `operator new` replaced with a counter, then a "
                         "full session driven through it.",
}


def build_tests(repo: Path, out: Path, test_binary: Path | None) -> None:
    rows = []
    total = 0
    for src in sorted((repo / "tests").glob("test_*.cpp")):
        names = re.findall(r"^TEST\((\w+)\)", src.read_text(encoding="utf-8"), re.MULTILINE)
        if not names:
            continue
        total += len(names)
        rows.append((src.name, len(names), names))

    page = ["# Test inventory\n"]
    page.append(
        "Generated from `tests/` on every push, so this cannot drift from what "
        "actually runs.\n"
    )

    if test_binary and test_binary.exists():
        output = run([str(test_binary)], check=False)
        summary = [l for l in output.splitlines() if "checks" in l]
        if summary:
            page.append(f"**Last CI run:** {summary[-1].strip()}\n")

    page.append(f"**{total} tests across {len(rows)} suites.**\n")
    page.append("| Suite | Tests | Covers |")
    page.append("|---|---|---|")
    for name, count, _ in rows:
        page.append(f"| `{name}` | {count} | {SUITE_BLURB.get(name, '')} |")
    page.append("")

    page.append(
        textwrap.dedent(
            """
            ## Why a hand-rolled harness

            The library is compiled `-fno-exceptions`. A framework that reports
            failures by throwing would quietly undermine the property under test,
            so `tests/test_harness.hpp` is about 100 lines with a fixed-size
            registration table that never allocates.

            ## The load-bearing suite

            `test_no_alloc.cpp` is the one that matters. It replaces global
            `operator new` with a counting version, arms it, and drives a full
            session — connect, subscribe, publish at all three QoS levels,
            inbound traffic, keep-alive, retransmission, table exhaustion, a
            malformed-packet teardown and disconnect — then asserts the counter
            is still zero. A second case proves construction does not allocate
            either, so a `Client` can live in `.bss` on a target with no heap
            linked at all.

            ## Beyond the unit tests

            CI also runs the suite under AddressSanitizer and
            UndefinedBehaviorSanitizer, and stands up a real Mosquitto broker to
            exercise QoS 0/1/2 in both directions, failing the build if the
            broker logs a protocol error. Unit tests can agree with a
            misreading of the specification; a real broker will not.

            ## Full test list
            """
        )
    )
    for name, _, names in rows:
        page.append(f"\n<details><summary><code>{name}</code></summary>\n")
        for n in names:
            page.append(f"- `{n}`")
        page.append("\n</details>")

    out.joinpath("Test-Inventory.md").write_text("\n".join(page) + "\n", encoding="utf-8")
    print(f"  Test-Inventory.md ({total} tests)")


# --------------------------------------------------------------------------
# Navigation
# --------------------------------------------------------------------------


def build_nav(out: Path, present: list[str], repo_slug: str, sha: str) -> None:
    lines = ["### paho-cpp-static\n"]
    for _src, page, title in PROSE_PAGES:
        if page in present:
            lines.append(f"- [{title}]({page})")
    lines.append("\n**Reference**\n")
    for page, title in GENERATED_PAGES:
        if (out / f"{page}.md").exists():
            lines.append(f"- [{title}]({page})")
    lines.append(f"\n---\n\n[Repository](https://github.com/{repo_slug})")
    out.joinpath("_Sidebar.md").write_text("\n".join(lines) + "\n", encoding="utf-8")

    out.joinpath("_Footer.md").write_text(
        f"Generated from [`{sha[:7]}`](https://github.com/{repo_slug}/commit/{sha}). "
        f"Edits made here are overwritten — change the sources under `docs/` instead.\n",
        encoding="utf-8",
    )
    print("  _Sidebar.md, _Footer.md")


# --------------------------------------------------------------------------


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", default="wiki", help="output directory")
    parser.add_argument("--repo", default=".", help="repository root")
    parser.add_argument("--repo-slug", default=os.environ.get("GITHUB_REPOSITORY",
                                                              "subtilitas/paho-cpp-static"))
    parser.add_argument("--ref", default=os.environ.get("GITHUB_REF_NAME", "main"))
    parser.add_argument("--sha", default=os.environ.get("GITHUB_SHA", ""))
    parser.add_argument("--etl-include", default=None)
    parser.add_argument("--test-binary", default=None)
    parser.add_argument("--skip-api", action="store_true")
    args = parser.parse_args()

    repo = Path(args.repo).resolve()
    out = Path(args.out).resolve()
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)

    sha = args.sha or run(["git", "rev-parse", "HEAD"], cwd=repo).strip()

    print(f"building wiki from {repo} -> {out}")
    present = build_prose(repo, out, args.repo_slug, args.ref)

    with tempfile.TemporaryDirectory() as tmp:
        workdir = Path(tmp)
        if not args.skip_api:
            try:
                build_api(repo, out, workdir)
            except Exception as exc:                      # noqa: BLE001
                print(f"  API-Reference.md FAILED: {exc}", file=sys.stderr)
                return 1
        try:
            build_footprint(repo, out, workdir,
                            find_etl_include(repo, args.etl_include))
        except Exception as exc:                          # noqa: BLE001
            print(f"  Memory-Footprint.md FAILED: {exc}", file=sys.stderr)
            return 1

    build_tests(repo, out,
                Path(args.test_binary) if args.test_binary else None)
    build_nav(out, present, args.repo_slug, sha)

    pages = sorted(p.name for p in out.glob("*.md"))
    print(f"\n{len(pages)} pages: {', '.join(pages)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

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
import shlex
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
    ("docs/static-analysis.md", "Static-Analysis", "Static analysis"),
    ("docs/misra.md", "MISRA", "MISRA C++:2023"),
    ("docs/compatibility.md", "Compatibility", "Compatibility"),
    ("CHANGELOG.md", "Changelog", "Changelog"),
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

# Everything in this section has to work when CXX is a cross compiler, which
# rules out two habits the obvious implementation reaches for.
#
# We cannot *run* what we build: an arm-none-eabi object does not execute on
# the runner. So every number is read out of an object file instead of printed
# by a program. See PROBE below for how.
#
# We also cannot use the host's binutils, because `size` and `nm` built for a
# different target will refuse the object -- silently reporting nothing, in the
# case of `size -A`. So the tools are derived from the compiler's own name.

# Longest first. "clang++" ends with "g++", so testing in any other order
# strips the wrong suffix and asks for "clannm".
_COMPILER_SUFFIXES = tuple(
    sorted(("g++", "clang++", "c++", "gcc", "cc"), key=len, reverse=True))


def cxx() -> str:
    return os.environ.get("CXX", "c++")


def binutil(name: str) -> str:
    """`nm` or `size` for whatever CXX targets.

    `arm-none-eabi-g++` implies `arm-none-eabi-nm`. An explicit NM or SIZE in
    the environment always wins, which is what an LLVM toolchain needs --
    `llvm-nm` is not spelled the way `clang++` is.
    """
    explicit = os.environ.get(name.upper())
    if explicit:
        return explicit

    compiler = Path(cxx()).name
    for suffix in _COMPILER_SUFFIXES:
        if compiler.endswith(suffix):
            prefix = compiler[: -len(suffix)]
            if prefix:                       # "arm-none-eabi-"
                return prefix + name
            break
    return name


def probe_flags() -> list[str]:
    """Target flags for the probe compiles, e.g. `-mcpu=cortex-m0plus -mthumb`.

    Deliberately its own variable rather than CXXFLAGS: CXXFLAGS is often
    already populated with host flags by whatever invoked us, and inheriting
    those into a cross measurement produces a number that is wrong in a way
    nobody would notice.
    """
    return shlex.split(os.environ.get("MQTT_PROBE_CXXFLAGS", ""))


def target_triple() -> str:
    """What CXX actually targets. Correct when cross-compiling; `uname -m`
    is not."""
    return run([cxx(), "-dumpmachine"], check=False).strip() or "unknown"


# Configuration profiles, in the order they appear in the table. The struct
# bodies are emitted into the probe below, so this list is the single place a
# profile is described.
PROFILES = [
    ("sensor", "Sensor (QoS 0 only)", "Sensor"),
    ("defaults", "DefaultConfig", "mqtt::DefaultConfig"),
    ("reliable", "Reliable sensor", "Reliable"),
    ("gateway", "Gateway", "Gateway"),
]

# Measured with no execution at all.
#
# Each quantity becomes the length of a global array, and `nm --print-size`
# reports that length straight out of the object file. Nothing runs, so the
# same probe works for a Cortex-M0+ as for the host.
#
# The `+ 1` is because a zero-length array is ill-formed and
# `max_inflight_out` is legitimately 0 on the sensor profile. Python subtracts
# it back off. The arrays have no initialiser on purpose -- that keeps them in
# .bss, so a 4 KiB gateway buffer costs nothing in the object file.
PROBE = r"""
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

#define MQTT_PROFILE(tag, cfg)                                          \
    char mqtt_probe__##tag##__size    [sizeof(mqtt::Client<cfg>) + 1];  \
    char mqtt_probe__##tag##__rx      [cfg::rx_buffer_size      + 1];   \
    char mqtt_probe__##tag##__tx      [cfg::tx_buffer_size      + 1];   \
    char mqtt_probe__##tag##__inflight[cfg::max_inflight_out    + 1];   \
    char mqtt_probe__##tag##__subs    [cfg::max_subscriptions   + 1];

extern "C" {
MQTT_PROFILE(sensor,   Sensor)
MQTT_PROFILE(defaults, mqtt::DefaultConfig)
MQTT_PROFILE(reliable, Reliable)
MQTT_PROFILE(gateway,  Gateway)
}
"""


FOOTPRINT_TEMPLATE = """\
# Memory footprint

Every buffer and table in the client is sized from its `Config` type, so
`sizeof(Client<Cfg>)` is the whole RAM cost — there are no hidden allocations
behind a pointer. The numbers below are **measured on every push**, not written
down once and left to rot.

{provenance}

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


def section_bytes(size_output: str) -> int:
    """`.text` plus `.rodata` from `size -A`.

    -ffunction-sections names every section `.text._ZN...` rather than plain
    `.text`, so this matches on the prefix. Comparing for equality silently
    measures zero, which is the failure mode the callers check for.
    """
    total = 0
    for line in size_output.splitlines():
        fields = line.split()
        if len(fields) >= 2 and fields[0].startswith((".text", ".rodata")):
            try:
                total += int(fields[1])
            except ValueError:
                pass
    return total


def compile_probe(src: Path, obj: Path, repo: Path, etl_include: Path, opt: str) -> None:
    run([
        cxx(), "-std=c++17", opt, "-fno-exceptions", "-fno-rtti",
        "-ffunction-sections", "-fdata-sections",
        *probe_flags(),
        f"-I{repo / 'include'}", "-isystem", str(etl_include),
        "-c", str(src), "-o", str(obj),
    ])


def read_probe(obj: Path) -> dict[str, int]:
    """Read the measured quantities back out of an object file.

    Every array the probe defines carries its measurement in nm's size
    column. Decimal radix is requested explicitly so the parse does not
    depend on nm's default, which is hexadecimal.
    """
    nm = binutil("nm")
    out = run([nm, "--print-size", "--defined-only", "-t", "d", str(obj)])

    values: dict[str, int] = {}
    for line in out.splitlines():
        fields = line.split()
        if len(fields) < 4:
            continue                     # a defined symbol nm could not size
        name = fields[-1]
        if name.startswith("mqtt_probe__"):
            try:
                values[name] = int(fields[1]) - 1      # undo the +1 in PROBE
            except ValueError:
                pass

    if not values:
        raise RuntimeError(
            f"{nm} found no mqtt_probe__* symbols in {obj.name}. Either the "
            f"probe did not define them, or nm is built for a different "
            f"target than {cxx()}. Output was:\n{out}"
        )
    return values


def build_footprint(repo: Path, out: Path, workdir: Path, etl_include: Path | None) -> None:
    if etl_include is None or not etl_include.exists():
        print("  Memory-Footprint.md skipped (no ETL include path)")
        return

    # ---- RAM: sizeof(Client<Cfg>) and the knobs that produced it ----------
    probe = workdir / "probe.cpp"
    probe.write_text(PROBE, encoding="utf-8")
    probe_obj = workdir / "probe.o"
    compile_probe(probe, probe_obj, repo, etl_include, "-O2")
    measured = read_probe(probe_obj)

    def field(tag: str, name: str) -> int:
        key = f"mqtt_probe__{tag}__{name}"
        if key not in measured:
            raise RuntimeError(f"probe did not define {key}")
        return measured[key]

    table = "\n".join(
        f"| {label} | {field(tag, 'rx')} | {field(tag, 'tx')} "
        f"| {field(tag, 'inflight')} | {field(tag, 'subs')} "
        f"| **{field(tag, 'size')}** |"
        for tag, label, _cfg in PROFILES
    )

    # ---- Flash: object sizes for the non-template core --------------------
    size_tool = binutil("size")
    sizes = []
    for src in sorted((repo / "src").glob("*.cpp")):
        obj = workdir / (src.stem + ".o")
        compile_probe(src, obj, repo, etl_include, "-Os")
        size_output = run([size_tool, "-A", str(obj)])
        text = section_bytes(size_output)
        if text == 0:
            raise RuntimeError(
                f"measured 0 bytes of code in {src.name}; the `{size_tool} -A` "
                f"output was not understood:\n{size_output}"
            )
        sizes.append((src.name, text))

    # ---- Provenance -------------------------------------------------------
    compiler = run([cxx(), "--version"]).splitlines()[0]
    triple = target_triple()
    flags = probe_flags()

    provenance = f"Measured with `{compiler}` targeting `{triple}`"
    if flags:
        provenance += f", `{' '.join(flags)}`"
    provenance += ".\n\n"

    # A bare-metal triple means these are the target's own numbers. Anything
    # else is a host build, and saying so is the difference between a measured
    # figure and one that merely looks like one.
    if "eabi" in triple or triple.startswith(("arm", "thumb", "aarch64", "riscv")):
        provenance += (
            "These are the target's own figures, read out of the object files "
            "rather than printed by a program -- nothing was executed to "
            "produce them."
        )
    else:
        provenance += (
            "This is a **host** build. Treat the numbers as indicative for a "
            "Cortex-M rather than a promise; the relative shape is what "
            "transfers. Where a cross-compiled table exists for the same "
            "commit, that one is the number to trust."
        )

    # Note: the template is deliberately not indented and does not go through
    # textwrap.dedent. dedent computes a common leading-whitespace prefix across
    # every line, and an interpolated multi-line value (the table) has none --
    # so dedent silently gives up and the whole page renders as a code block.
    page = FOOTPRINT_TEMPLATE.format(provenance=provenance, table=table)

    for name, size in sizes:
        page += f"| `{name}` | {size} |\n"
    page += f"| **total** | **{sum(s for _, s in sizes)}** |\n"

    # Measure the template instantiation too, rather than asserting a number
    # that nobody re-checks.
    inst_src = workdir / "inst.cpp"
    inst_src.write_text(INSTANTIATION_PROBE, encoding="utf-8")
    inst_obj = workdir / "inst.o"
    compile_probe(inst_src, inst_obj, repo, etl_include, "-Os")
    inst_bytes = section_bytes(run([size_tool, "-A", str(inst_obj)]))

    page += FOOTPRINT_TAIL.format(instantiation=inst_bytes)

    out.joinpath("Memory-Footprint.md").write_text(page, encoding="utf-8")
    print(f"  Memory-Footprint.md ({len(sizes)} objects, {triple})")


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
    parser.add_argument("--footprint-only", action="store_true",
                        help="emit only Memory-Footprint.md. Used by the "
                             "cross-compile job, which has a target "
                             "compiler but no Doxygen and no runnable "
                             "test binary.")
    args = parser.parse_args()

    repo = Path(args.repo).resolve()
    out = Path(args.out).resolve()
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)

    sha = args.sha or run(["git", "rev-parse", "HEAD"], cwd=repo).strip()

    print(f"building wiki from {repo} -> {out}")
    present = [] if args.footprint_only else build_prose(repo, out, args.repo_slug, args.ref)

    with tempfile.TemporaryDirectory() as tmp:
        workdir = Path(tmp)
        if not args.skip_api and not args.footprint_only:
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

    if not args.footprint_only:
        build_tests(repo, out,
                    Path(args.test_binary) if args.test_binary else None)
        build_nav(out, present, args.repo_slug, sha)

    pages = sorted(p.name for p in out.glob("*.md"))
    print(f"\n{len(pages)} pages: {', '.join(pages)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

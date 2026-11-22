# Static analysis

Three analysers run in CI. clang-tidy and clang-format are in
`.github/workflows/sca.yml` and both **gate**. CodeQL is in
`.github/workflows/codeql.yml` and is **advisory** — it reports to the Security
tab and does not fail the build.

They are in separate files so each badge means one thing: a red CodeQL badge is
a security finding, not somebody forgetting to run the formatter.

Each analyser starts by reporting, its findings are triaged, and it becomes
blocking once the backlog is empty. A check that fails on day one for hundreds
of pre-existing reasons gets ignored, and an ignored red check hides the next
real failure. This page records where that triage has got to.

| Analyser | State | Findings | Blocker before gating |
|---|---|---|---|
| CodeQL | advisory | see the Security tab | gating is a branch-protection setting |
| **clang-tidy** | **gating** | **0** | — |
| **clang-format** | **gating** | **0** | — |

## Turning a gate on

Each analyser has its own switch in the workflow's `env` block, so they can be
gated one at a time as they come clean:

```yaml
GATE_CLANG_TIDY: "true"
GATE_CLANG_FORMAT: "true"
```

CodeQL has no switch, and is not in that file. It publishes to the Security
tab, and gating it belongs in branch protection — *Settings → Branches →
Require code scanning results* — because the severity threshold is a repository
policy, not a workflow detail.

---

## clang-tidy

`.clang-tidy` enables `bugprone`, `cert`, `clang-analyzer`, `concurrency`,
`misc`, `performance`, `portability`, `readability` and `cppcoreguidelines`,
then switches off 21 checks that only restate this project's deliberate
choices. Each exclusion carries its reasoning in the config; the short version
is that a wire-protocol implementation is *made of* magic numbers, pointer
arithmetic and fixed C arrays.

That takes the count from **617 to 15**, which is the difference between a
report somebody reads and one nobody does.

### The 15, and what became of them

None of them was a bug. All fifteen are resolved, so the gate is on and any new
finding is a regression in the change that introduced it.

**Fixed**

- `bugprone-sizeof-expression` — `static_assert(sizeof(ConfigCheck<Cfg>) > 0)`
  existed only to force the config checks to instantiate. `Client` now derives
  privately from `ConfigCheck<Cfg>` instead: a base class must be complete, so
  the assertions still fire, and an empty base costs nothing under the empty
  base optimisation. Verified — `sizeof` is unchanged by the switch.
- `readability-redundant-member-init` ×5 — the five handler members.
  `etl::delegate` default-constructs to unset already.
- `cppcoreguidelines-special-member-functions` ×3 — `Transport` and `Clock`
  now declare their copy and move operations `protected` and defaulted, which
  is the Core Guidelines shape for an abstract interface (C.67): a derived
  transport may still be copyable, but assigning through a `Transport&` can no
  longer slice it. `Client` deletes all four and defaults its destructor; it
  holds references and moving one mid-session would leave the transport
  talking to a corpse.
- `performance-enum-size` — `Error` is `uint8_t` for its 22 values. It packs
  against the neighbouring small members rather than forcing padding around
  them, which takes **eight bytes off every configuration** compared with the
  two-byte enum it would otherwise be. `error.hpp` fixes the underlying type
  explicitly, so the saving does not depend on what a compiler picks.

  The underlying type and the numeric values are both part of the public API,
  spelled out in `error.hpp` and pinned by `tests/test_error_values.cpp` at
  compile time and again at run time. Widening the type, or renumbering an
  enumerator, is a breaking change — see
  [compatibility.md](compatibility.md).

**Declined, and silenced in `.clang-tidy` with the reasoning recorded**

- `readability-use-anyofallof` ×2 — would replace two four-line loops with
  `std::any_of`. The library calls no `std::` algorithm; where it needs one it
  calls ETL's. No library header includes `<algorithm>` itself, but it arrives
  transitively: `etl/algorithm.h` includes it whenever `ETL_USING_STL` is set,
  which is ETL's default, and `etl/vector.h`, `etl/string_view.h`,
  `etl/span.h`, `etl/array.h` and `etl/byte_stream.h` all include
  `etl/algorithm.h`.
- `readability-avoid-nested-conditional-operator` ×2 — `vbi_size_c` must stay a
  single expression to be usable in a constant expression under C++17.
- `cppcoreguidelines-missing-std-forward` — `enqueue_sized` invokes its
  callable once, in place; forwarding would change nothing.

---

## C library string, memory and stdio calls

CodeSonar reports calls into the C library's string, memory and formatted I/O
functions: a length in bytes is a bound it cannot tie to the buffer. The tree
makes none, in the library, the tests, the fuzz harnesses, the interop broker
or the examples. The evidence is `tools/scan_constructs.py --check` for
`include/mqtt/` and `src/`, and a grep of the whole tree. No CodeSonar run has
confirmed it.

| C library call | Replacement | Where |
|---|---|---|
| `memcpy` | `etl::copy` | `Client::enqueue`, test helpers |
| `memmove` | `etl::copy` | `Client::drain_rx`, `TxQueue::compact` |
| `memcmp` | `etl::equal` | `Client::find_subscription`, test byte comparisons |
| `memset` | `etl::fill` | guard bytes in `test_encode_bounds.cpp` |
| `strlen`, `strcmp` | `etl::string_view` comparison | tests |
| `atoi`, `strtol` | `etl::to_arithmetic<uint32_t>`, then a 1–65535 range check | examples, interop broker |
| `snprintf` | `etl::string<N>` with `etl::to_string` | examples; the string's `size()` is the payload length |
| `printf`, `fprintf`, `fflush` | `std::cout`, `std::cerr` | examples, tests, interop broker |

Both `memmove` sites shift bytes down: the destination starts before the
source. A forward copy reads each byte before it overwrites it, so `etl::copy`
is correct on the overlap. Under C++17 it is an element loop; under C++20 with
the STL it is `std::copy`, which permits this overlap because the destination
does not start inside the source range. A comment at each site says so.

`etl::mem_copy`, `etl::mem_move`, `etl::mem_compare` and `etl::mem_set` are not
used. They call `memcpy` and its relatives inside the ETL header, so the call
is still there for an analyser to find.

`tools/scan_constructs.py` holds the library to this with a budget of 0 for the
calls and for `<cstring>`; see [misra.md](misra.md). The scan covers
`include/mqtt/` and `src/` only. Tests and examples are outside it.

Left in place, each for a stated reason:

- `std::malloc`, `std::free` and `std::abort` in `tests/test_no_alloc.cpp`. They
  back the replaced global `operator new`. It cannot call `operator new`, and
  under `-fno-exceptions` it cannot throw `std::bad_alloc`.
- `std::signal` in `tests/interop/minimosq_broker.cpp`, to stop the broker on
  `SIGTERM` and ignore `SIGPIPE`. POSIX has no C++ equivalent.
- `errno`, `getaddrinfo` and the socket calls in the example transports. They
  are the OS interface the transports adapt.

---

## clang-format

Resolved. The tree is formatted, the check gates, and it cannot drift again.

The problem was not untidy code: **every one of the 29 `.cpp`/`.hpp` files then
in the tree differed from the committed `.clang-format`**, about 1360 lines,
because the config had been added and never applied. Three of its rules
disagreed with the entire codebase, so reformatting first would have baked in
the wrong answer and produced a second churn commit later. What it took:

| Rule | Problem | Resolution |
|---|---|---|
| `BreakBeforeBraces: Allman` | put the brace of `namespace mqtt {` on its own line, which no file does. Allman is a preset with no per-member override | spelled out as `Custom` with `AfterNamespace: false`, every other value copied from Allman verbatim. ~160 lines of the diff |
| `AllowShortFunctionsOnASingleLine: Inline` | expanded `at_least_one`, `utf8_size`, `valid_qos` and the `operator new` overloads, all written on one line | `All`, which reproduces what is there and — checked file by file — collapses nothing that was written expanded |
| Alignment clang-format cannot express | the `to_string` switch tables, the flag-bit assignments in `FixedHeader::to_byte`, the golden wire-byte tables in the codec tests. Those columns are the documentation | marked `// clang-format off`. The directive must be exactly that — a trailing explanation on the same line silently makes it an ordinary comment, so the reason goes on the line above |

Then `clang-format -i` over everything as its own commit, touching nothing else,
so it can be skipped when reading history or bisecting.

Checked before and after, since a whole-tree reformat is otherwise unverifiable:
the residual difference is zero, and the tree still builds clean under
`-Werror`, passes all tests, stays clean under ASan and UBSan, keeps
`sizeof(Client<Cfg>)` unchanged, and holds its coverage floor.

`AlignConsecutiveDeclarations: Consecutive` was left alone deliberately. It
occasionally pads a declaration oddly, but turning it off costs more churn than
it saves (1326 lines versus 1172), because the codebase does align its members.

---

## Running them locally

```bash
# clang-tidy
cmake -S . -B build -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DMQTT_BUILD_EXAMPLES=OFF
clang-tidy -p build $(git ls-files 'src/*.cpp')

# clang-format, as a diff rather than in place
for f in $(git ls-files '*.cpp' '*.hpp'); do
    clang-format --style=file "$f" | diff -u "$f" -
done
```

CI pins both to the version in `CLANG_VERSION` in the workflow. Different
clang-format releases disagree by a line here and there, which is enough to
make a gated check fail for someone whose distribution ships something else,
so match the pinned version before concluding the tree is unformatted.

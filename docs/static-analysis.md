# Static analysis

Three analysers run in `.github/workflows/sca.yml`: CodeQL, clang-tidy and
clang-format. clang-tidy and clang-format **gate**; CodeQL is **advisory** — it
reports to the Security tab and does not fail the build.

That is deliberate. A check that fails on day one for hundreds of pre-existing
reasons does not get fixed; it gets ignored, and an ignored red check is worse
than no check, because it hides the next real failure. So each analyser starts
by reporting, its findings get triaged, and it becomes blocking once the
backlog is empty and staying that way is a matter of not regressing.

This page records where that triage has got to.

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

CodeQL has no switch. It publishes to the Security tab, and gating it belongs
in branch protection — *Settings → Branches → Require code scanning results* —
because the severity threshold is a repository policy, not a workflow detail.

---

## clang-tidy

`.clang-tidy` enables `bugprone`, `cert`, `clang-analyzer`, `concurrency`,
`misc`, `performance`, `portability`, `readability` and `cppcoreguidelines`,
then switches off fifteen checks that only restate this project's deliberate
choices. Each exclusion has its reasoning written next to it in the config; the
short version is that a wire-protocol implementation is *made of* magic
numbers, pointer arithmetic and fixed C arrays, and a check that objects to
those is not telling us anything.

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
- `performance-enum-size` — `Error` was `int16_t` for twenty-three values. Now
  `uint8_t`, which packs better against the neighbouring members and takes
  **eight bytes off every configuration**: `Client<DefaultConfig>` went 4608 →
  4600, and the small and large profiles likewise.

  This is an API-visible change to a public enum, and it was first justified
  here on the grounds that the project was 0.1.0. That was wrong — it is 1.x.
  The change still stands, on the better reason: there is no ABI to break.
  The library is consumed from source via `add_subdirectory` or `FetchContent`,
  ships no `install(EXPORT)` and no shared library, and every consumer
  therefore recompiles against the header that declares the enum. Code that
  stores an `Error` in an explicitly-typed field is the only thing affected,
  and it is a recompile, not a silent misbehaviour. Released in 1.5.0 and
  called out as breaking; reverting is one line if that ever stops holding.

**Declined, and silenced in `.clang-tidy` with the reasoning recorded**

- `readability-use-anyofallof` ×2 — would pull `<algorithm>` into a library
  that deliberately includes it nowhere, to replace two four-line loops.
- `readability-avoid-nested-conditional-operator` ×2 — `vbi_size_c` must stay a
  single expression to be usable in a constant expression under C++17.
- `cppcoreguidelines-missing-std-forward` — `enqueue_sized` invokes its
  callable once, in place; forwarding would change nothing.

---

## clang-format

Resolved. The tree is formatted and the check gates, so it cannot drift again.

The problem was not that the code was untidy: **every one of the 29 files
differed from the committed `.clang-format`**, about 1360 lines, because the
config had been added and never applied. It described an intention. Three of
its rules disagreed with the entire codebase, which is why reformatting first
would have been the wrong move — it would have baked in the wrong answer and
produced a second churn commit later.

What it took, in order:

1. **`BreakBeforeBraces: Allman`** put the brace of `namespace mqtt {` on its
   own line. No file does that. Allman is a preset with no way to override a
   single member, so it is now spelled out as `Custom` with
   `AfterNamespace: false` and every other value copied from Allman verbatim.
   Worth roughly 160 lines of the diff.

2. **`AllowShortFunctionsOnASingleLine: Inline`** expanded `at_least_one`,
   `utf8_size`, `valid_qos` and the `operator new` overloads, all of which are
   written on one line. `All` reproduces what is already there and — checked
   file by file — collapses nothing that was written expanded.

3. **Deliberate alignment clang-format cannot express** is marked
   `// clang-format off`: the `to_string` switch tables, the flag-bit
   assignments in `FixedHeader::to_byte`, and the golden wire-byte tables in
   the codec tests. Those columns are the documentation; collapsing them loses
   the thing being documented. Note that the directive must be exactly
   `// clang-format off` — a trailing explanation on the same line silently
   makes it an ordinary comment, so the reason goes on the line above.

Then `clang-format -i` over everything, as its own commit touching nothing
else, so it can be skipped when reading history or bisecting.

Two things checked before and after, because a whole-tree reformat is
otherwise an act of faith: the residual difference is zero, and the tree still
builds clean under `-Werror`, passes all tests, stays clean under ASan and
UBSan, keeps `sizeof(Client<Cfg>)` unchanged, and holds its coverage floor.

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

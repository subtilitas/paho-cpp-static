# Static analysis

Three analysers run in `.github/workflows/sca.yml`: CodeQL, clang-tidy and
clang-format. clang-tidy **gates**; the other two are **advisory** — they report
on every push and pull request and go green regardless.

That is deliberate. A check that fails on day one for hundreds of pre-existing
reasons does not get fixed; it gets ignored, and an ignored red check is worse
than no check, because it hides the next real failure. So each analyser starts
by reporting, its findings get triaged, and it becomes blocking once the
backlog is empty and staying that way is a matter of not regressing.

This page records where that triage has got to.

| Analyser | State | Findings | Blocker before gating |
|---|---|---|---|
| CodeQL | advisory | see the Security tab | none known |
| **clang-tidy** | **gating** | **0** | — triaged to zero, see below |
| clang-format | advisory | 29 of 29 files | `.clang-format` does not describe the code |

## Turning a gate on

Each analyser has its own switch in the workflow's `env` block, so they can be
gated one at a time as they come clean:

```yaml
GATE_CLANG_TIDY: "true"      # on: the backlog was triaged to zero
GATE_CLANG_FORMAT: "false"   # -> "true" once .clang-format is settled
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
  4600, and the small and large profiles likewise. This is an API-visible
  change to a public enum, taken deliberately: the project is 0.1.0, is
  consumed from source via `add_subdirectory`/`FetchContent`, ships no
  `install(EXPORT)` and makes no ABI promise. Reverting is one line if that
  ever stops being true.

**Declined, and silenced in `.clang-tidy` with the reasoning recorded**

- `readability-use-anyofallof` ×2 — would pull `<algorithm>` into a library
  that deliberately includes it nowhere, to replace two four-line loops.
- `readability-avoid-nested-conditional-operator` ×2 — `vbi_size_c` must stay a
  single expression to be usable in a constant expression under C++17.
- `cppcoreguidelines-missing-std-forward` — `enqueue_sized` invokes its
  callable once, in place; forwarding would change nothing.

---

## clang-format

**Every one of the 29 source files differs from the committed
`.clang-format`** — about 1360 lines. The config was added but never applied,
so the file describes an intention rather than the code.

Reformatting the tree is not the first move, because the config is at least
partly wrong. Verified with clang-format 14, 16, 18 and 20, which agree to
within twenty lines, so this is not a version artefact.

The differences fall into three groups:

1. **Namespace braces.** `BreakBeforeBraces: Allman` puts the brace of
   `namespace mqtt {` on its own line; every file writes it inline. Setting
   `BraceWrapping.AfterNamespace: false` (with `BreakBeforeBraces: Custom`)
   matches what is actually written and removes ~160 lines of the diff.

2. **Trailing comment columns.** `SpacesBeforeTrailingComments` shifts `///<`
   comments by a column or two throughout. Cosmetic, and whichever way it goes
   it should be decided once.

3. **Hand-aligned `case` labels.** The `to_string` switches align their
   returns into a column. clang-format has no option that preserves this and
   will collapse it. This is the only group where reformatting actively makes
   the code less readable — `// clang-format off` around those switches is the
   usual answer.

Suggested order: fix (1) in the config, decide (2), mark (3) off, then reformat
in a single commit that touches nothing else, and gate immediately afterwards
so it never drifts again. Doing the reformat *before* fixing the config would
bake in the wrong answer and produce a second churn commit later.

The reformat itself is safe to do whenever the config is settled: applying the
current diff wholesale leaves a tree that builds clean under `-Werror` and
passes all tests, so the only thing at stake is readability, not behaviour.
The job attaches `format.diff` as an artefact, and it is a directly applicable
patch — `git apply format.diff` — so the change can be reviewed before anyone
commits to it.

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

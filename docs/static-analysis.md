# Static analysis

Three analysers run in `.github/workflows/sca.yml`: CodeQL, clang-tidy and
clang-format. All three are **advisory** right now — they report on every push
and pull request and go green regardless.

That is deliberate. A check that fails on day one for hundreds of pre-existing
reasons does not get fixed; it gets ignored, and an ignored red check is worse
than no check, because it hides the next real failure. So each analyser starts
by reporting, its findings get triaged, and it becomes blocking once the
backlog is empty and staying that way is a matter of not regressing.

This page records where that triage has got to.

| Analyser | State | Findings | Blocker before gating |
|---|---|---|---|
| CodeQL | advisory | see the Security tab | none known |
| clang-tidy | advisory | 15 | triage the list below |
| clang-format | advisory | 29 of 29 files | `.clang-format` does not describe the code |

## Turning a gate on

Each analyser has its own switch in the workflow's `env` block, so they can be
gated one at a time as they come clean:

```yaml
GATE_CLANG_TIDY: "false"     # -> "true" to make findings fail the job
GATE_CLANG_FORMAT: "false"
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

### The 15, and what to do about them

Nothing here looks like a bug. Grouped by decision needed:

**Probably just fix**

- `bugprone-sizeof-expression` — `client.hpp:106`. The
  `static_assert(sizeof(ConfigCheck<Cfg>) > 0)` that forces the config checks
  to instantiate. The comparison is indeed always true; that is the mechanism,
  not an accident. Cleaner as a private empty base, which instantiates the
  template *and* costs nothing under empty-base optimisation.
- `readability-redundant-member-init` ×5 — `on_message_{}` and friends.
  `etl::delegate` default-constructs to empty anyway.

**Worth a decision**

- `performance-enum-size` — `Error` is `int16_t` and fits in `uint8_t`. It is
  stored in every `Result<T>`, so this is real bytes on a small target, but it
  is also an ABI-visible change to a public enum.
- `cppcoreguidelines-special-member-functions` ×3 — `Transport`, `Clock` and
  `Client`. The two interfaces declare a virtual destructor and nothing else;
  the rule of five says declare or delete the rest. Cheap, and it stops a
  derived transport being sliced by accident.

**Probably decline**

- `readability-use-anyofallof` ×2 — `topic.cpp:12` and `client.hpp:1305`.
  Would mean including `<algorithm>` in a library that deliberately keeps its
  standard-library surface tiny, to replace a four-line loop.
- `readability-avoid-nested-conditional-operator` ×2 — `client.hpp:682`, the
  compile-time `vbi_size_c`. It has to stay a single expression to be
  `constexpr`-friendly on C++17.
- `cppcoreguidelines-missing-std-forward` — `enqueue_sized`'s `EncodeFn&&`.
  The callable is invoked once, in place; forwarding it would change nothing.

Once the first two groups are done, the remainder can be silenced in
`.clang-tidy` with the reasoning recorded, and `GATE_CLANG_TIDY` can go to
`true`.

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

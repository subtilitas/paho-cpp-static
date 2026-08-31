# MISRA C++:2023 — self-assessment

**This is a self-assessment, not a certification, and not a compliance claim.**

A compliance claim under MISRA requires a licensed copy of the standard, a
qualified checking tool, a documented deviation procedure per MISRA
Compliance:2020, and a guideline enforcement plan showing how every guideline is
addressed. None of that has been done here. What follows is the preparatory
step: an argument that the codebase is plausibly in reach, the mechanical
evidence for it, and the deviations that would need writing up. It is not a
substitute for running a qualified tool over the code.

---

## Is it plausible?

Three facts make the question worth asking.

**MISRA C++:2023 targets C++17** (ISO/IEC 14882:2017), the language this library
is written in. Its predecessor targeted C++03, which is why template-heavy
modern C++ was historically treated as out of scope for safety work.

**It is now the single subset.** MISRA C++:2023 merges the work of AUTOSAR
C++14, the previous de-facto choice for automotive C++. Tool vendors describe
AUTOSAR C++14 as superseded by it. MISRA has published no formal withdrawal
notice, so treat "superseded" as the industry's reading rather than a
first-party statement.

**Templates are not banned.** The standard devotes a section to them, and the
rule usually cited is narrow: function templates shall not be explicitly
specialised. This library specialises none. The cost of a template-heavy
codebase under MISRA is not prohibition but tooling expense — system-scope rules
must be checked across every instantiation.

**Not established:** any published data on what a MISRA C++:2023 assessment
costs for a template-heavy C++17 library. Treat claims in either direction,
including optimistic ones here, as unsupported.

---

## Why rule numbers are not reproduced

MISRA C++:2023 is a licensed document. Reproducing its 179 guidelines, or even
their numbering, is not something this repository can do. So this assessment is
organised by *construct* rather than by rule number: the constructs below are
the ones a C++ safety subset either forbids outright or requires a deviation
for, and each is measured rather than asserted.

Mapping them onto rule numbers is the first thing a real assessment does, and it
needs the standard in hand.

---

## The mechanical evidence

`tools/scan_constructs.py` strips comments and string literals, then counts each
construct against a budget. It runs in CI (`sca.yml`), so this table is
re-derived on every push rather than being a snapshot somebody took once.

```bash
tools/scan_constructs.py            # report
tools/scan_constructs.py --check    # fail if a budget is exceeded
```

| Construct | Budget | Why the subset cares |
|---|---|---|
| `goto` | 0 | unstructured control flow |
| `malloc` / `free` family | 0 | no heap. Also enforced at run time — `test_no_alloc.cpp` replaces global `operator new` with a counter and drives a full session through it |
| `operator new` / `delete` | 0 | as above. `= delete` on a special member is a different thing and is not counted |
| `throw` / `try` / `catch` | 0 | built `-fno-exceptions`; `error.hpp` raises `#error` if the flag failed to apply |
| `dynamic_cast` | 0 | built `-fno-rtti` |
| `const_cast` | 0 | casting away `const` removes the only guarantee the type system really gives you |
| `reinterpret_cast` | **2** | deviation — see below |
| `union` | 0 | type punning. The MQTT fixed header is plain fields for exactly this reason |
| bitfields | 0 | implementation-defined layout. The wire format uses explicit shifts and masks |
| `volatile` | 0 | not a concurrency primitive, and there is no MMIO here |
| `mutable` | 0 | `const` that is not const |
| `friend` | 0 | encapsulation escape hatch |
| `alloca` / VLA | 0 | unbounded stack. The cross-compile job independently fails on a dynamically sized frame |
| `float` / `double` | 0 | no floating point anywhere, so the archive links against either float ABI |
| `<cstdio>` / `<iostream>` | 0 | no I/O in a library that does not own the transport |
| multiple inheritance | 0 | one abstract interface per type |
| function-like macros | **1** | deviation — see below |

Enforced elsewhere, by the compiler rather than by this script:

| Property | How |
|---|---|
| No C-style casts | `-Wold-style-cast`, `-Werror` in CI |
| No implicit narrowing or sign conversion | `-Wconversion -Wsign-conversion`, `-Werror` |
| No shadowed declarations | `-Wshadow`, `-Werror` |
| No misaligned casts | `-Wcast-align`, `-Werror` |
| Non-virtual destructor in a base | `-Wnon-virtual-dtor`, `-Werror` |
| Every enumerator handled in a `switch` | `-Wswitch` (implied by `-Wall`), plus `test_to_string.cpp`, which asserts every enumerator has a distinct name and no case falls through |
| No recursion on the protocol path | by construction — see `docs/architecture.md`. Checked indirectly: the cross job's stack-usage gate makes an unbounded frame a build failure |
| Fixed-width integer types on the wire | `<cstdint>` throughout; the only plain `int` is a loop counter and the constant that bounds it |

---

## Deviations

Two, both narrow. `scan_constructs.py` fails if either budget grows, so adding a
third means making the argument again rather than inheriting it.

### `reinterpret_cast`, ×2

Both are `char*` ↔ `uint8_t*` at the boundary between MQTT strings and wire
bytes:

- `src/codec.cpp` in `write_string()` — an `etl::string_view`'s bytes handed to
  `etl::byte_stream_writer::write<uint8_t>`.
- `include/mqtt/client.hpp` in the `string_view` overload of `publish()` — the
  payload handed to the `span<const uint8_t>` overload.

Both are well-defined: `char` and `unsigned char` may alias any object type, and
this is the direction the aliasing rules explicitly permit. Neither changes
alignment, and no object is created or destroyed through the resulting pointer.
The alternative — a per-byte copy through a `static_cast` loop — would trade a
defined cast for a bounded but real cost on every publish, on a library whose
reason to exist is that cost.

A checker will flag these. The deviation record is: two sites, both at the
wire boundary, both `char`/`uint8_t`, both justified by the aliasing rules.

### One function-like macro: `MQTT_WRITE`

`error.hpp` defines it. It turns an ETL stream write returning `bool` into an
early return of `Error::BufferTooSmall`.

The early return is the point: a function cannot return from its caller, so the
alternatives are a macro or an `if` at every call site in the codec. Their
density is what makes forgetting one likely, and a forgotten one is a silent
buffer overrun rather than a compile error.

`MQTT_TRY` and `MQTT_READ` sat beside it and were deleted for having no uses.

---

## What this assessment cannot cover

Being explicit about the gap, because the gap is most of the work.

- **Rule-by-rule conformance.** Requires the licensed standard and a qualified
  tool — LDRA, Polyspace, Parasoft, PC-lint or similar. Nothing free covers
  MISRA C++:2023.
- **Undecidable and system-scope guidelines.** A meaningful proportion of the
  set cannot be settled by inspecting one translation unit, and template
  instantiation multiplies the work.
- **The deviation procedure.** MISRA Compliance:2020 defines what a deviation
  record must contain and who signs it off. The two above are arguments, not
  records.
- **A guideline enforcement plan** — the document stating, for every guideline,
  how compliance is demonstrated. That is the deliverable an assessor asks for
  first, and it does not exist.

## What does exist, and is worth counting

The nearest available proxies all pass and all gate in CI:

- **clang-tidy** with `bugprone`, `cert`, `clang-analyzer`, `concurrency`,
  `misc`, `performance`, `portability`, `readability` and `cppcoreguidelines`
  enabled: **zero findings**, gating. `cppcoreguidelines` in particular overlaps
  substantially with the AUTOSAR C++14 lineage that fed MISRA C++:2023. 21
  checks are disabled, each with its reasoning recorded in `.clang-tidy` and
  `docs/static-analysis.md`.
- **CodeQL** `security-and-quality`, weekly and on every push.
- **ASan and UBSan** clean across the suite.
- **`-Werror`** with the warning set above.
- **No undefined `malloc`, `operator new`, `__cxa_throw` or unwinder symbols**
  in the archive — checked on the host and, since the cross job landed, on the
  target archive too.

None of that is MISRA compliance. It is evidence that an assessment would not
start from a backlog of hundreds of findings, which is the usual reason such an
assessment is abandoned.

## If you need actual compliance

The honest sequence, in order of cost:

1. Buy the standard and run one qualified tool over the library. That produces
   the real finding count, which is the number nobody currently has.
2. Triage it the way `docs/static-analysis.md` describes for clang-tidy: report
   first, gate once the backlog is empty.
3. Write the deviation records and the guideline enforcement plan.

Step 1 is the one that turns this document from an argument into a fact, and
it is not something the project can do for you — the tool licence is yours, and
so is the assessment.

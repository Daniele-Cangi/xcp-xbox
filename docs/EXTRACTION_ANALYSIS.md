# XCP extraction analysis

## Authority and scope

The source examined is `Daniele-Cangi/Xbox-series-Agent` `main` at
`4e544a44dd267150fe2bc2f005c51338647a56e0`. `git ls-remote origin
refs/heads/main` returned this same commit on 2026-09-23. The destination
started at `Daniele-Cangi/XCP` `bade287145154ad8e26c2cc87e2942e794a91d73`.
The source is inspected read-only. Historical Xbox measurements remain source
laboratory evidence; this extraction does not repeat or promote them.

## Recovered architecture

The implemented C5/C6/G1 path is an editable project and deterministic bundle,
an authorized source inventory, Godot Source Model, semantic inventory/Creative
IR, host-capability plan, common lowering, C5 intent and sole target lifecycle,
then separate readiness/fidelity decisions. G2 adds typed semantic IR,
invariants, source/target traces, phase evidence, root-cause routing and
compensation authority. The worker has an independent C++ XVM verifier and
interpreter; `tools/xvm_reference.py`, `tools/xvm_assembler.py` and
`schemas/xvm-isa-v2.json` provide the portable Python oracle. The reference
supports 26 opcodes, exact static fuel, bounded control/calls, typed u32 views
and snapshot/resume. The worker SDK contract separates negotiation, runtime
discovery, artifact lifecycle, canonical result, telemetry and evidence.

Code narrows the documented separation. G1 `SourceAdapter` ends at Source
Model/IR, but its registry imports both Godot implementations. The common
`ir/model.py` also contains a campaign-scene helper keyed to a `levels/` path.
`lowering/backend.py` has world2d/host-specific paths and imports adaptive
planning. The large G2 planning/fidelity trees name experiment phases and
depend on exact profiles. Those modules cannot become a neutral core by
renaming directories. The original project builder is PC-side and has no
worker dependency. The original lifecycle imports transport and is bound to
the existing Xbox worker protocol; it is an optional target integration.

### Candidate dependency graph

```text
source adapter -> source inventory -> Source Model -> semantic IR
                                             |                 |
                                             +--> provenance --+
                                                               v
target capability profile ----------------> admission plan -> lowering
                                                     |              |
                                                     v              v
                                           target contract -> implementation
                                                                    |
source observation -------------------------------------> target observation
                              |                                     |
                              +--------> bound evidence <----------+
                                               |
                                        fidelity/claim decision

XVM spec -> XVM verifier -> reference executor -> XVM conformance
Xbox target -> XVM spec (never the reverse)
```

The arrows represent data and allowed imports. Core packages must not import
Godot, Xbox, Studio or historical G2 profiles. Evidence consumes identities
from every stage, not their mutable file paths.

## Component classification and coupling

| Source component | Classification | Dependency finding |
| --- | --- | --- |
| `schemas/xvm-isa-v2.json`, `tools/xvm_reference.py`, `tools/xvm_assembler.py` | public specification and reference | Standalone Python; reference snapshot seal is a test authority, not Xbox trust material. |
| `tools/xcp_creative_project.py`, project/bundle/host schemas | public contract and reference builder | Uses `jsonschema`, local schemas and ordinary files; no transport import. |
| G1 `core/contracts.py`, Source Model and IR integrity | candidate public abstractions | Protocols useful; package still points at legacy C6 dictionaries and contains a campaign helper. |
| Godot 2/4 adapter and semantic passes | reference adapter | Godot file syntax and Minilens-era semantics; first frontend only, some behavior unvalidated across projects. |
| G1 common planning/lowering | candidate reference implementation | Host module kinds and world2d assumptions leak into generic files. |
| G2 typed IR, observation, differential, invariant contracts | candidate general contract | Useful exact bindings and fail-closed comparison; implementations import G2 helpers and historical authority. |
| G2 stage-specific planning, policies, profile chains, run reports | research-only | Exact experiment chronology and source-specific selection; exclude. |
| C5 lifecycle, worker transport, worker SDK | optional Xbox target/SDK | Real protocol and operational boundary; credentials and device state must stay external. |
| Studio and WinUI code | product-only | UI and project library, not platform semantics. |
| ForgeProtocol, WorldLoop, games and assets | product/research/third-party | Exclude entirely. |

The G1 73-file kit demonstrates repository independence, not a minimal public
API. The G2 428-file kit is an experiment distribution and is not an extraction
list. The current Studio boundary consumes file/toolchain contracts; it does
not own the platform rules.
G1 tests assert legacy CLI/error-code compatibility and a Minilens r11
structural golden. G2 exporter tests bind the historical G1 snapshot and a
large exact overlay. Neither test set can serve as public conformance because
their authority includes private history and restricted fixtures. Synthetic
local conformance instead tests the extracted contracts directly.

At the examined source commit, `CURRENT_STATE.md` also records three bounded
Godot reconstruction/playability results, then an unseen Godot 4 holdout whose
blind semantic pass fails on a dynamic resource-path pattern. Those results
demonstrate useful breadth and a live generalization limit; they do not close
whole-source fidelity or universal Godot support. The public structural
adapter deliberately fails admission on script behavior and unknown syntax
instead of presenting private experiment policies as a general solution.

## Architecture options

1. Mirror G1's `tools/`, `schemas/`, `profiles/` and add the G2 packages.
   This preserves import compatibility quickly, but keeps legacy CLI globals,
   source-specific helpers and a rapidly expanding policy namespace as public
   API. External adapters would inherit accidental coupling.
2. Publish one installable `src/xcp` package with versioned specifications in
   package data, isolated `xvm`, core protocols, adapters, evidence and a CLI.
   This requires explicit migration and conformance tests, but gives a clean
   dependency direction, wheel installation and no repository-root assumption.

Select option 2. Keep the ISA and project/bundle wire formats versioned and
compatible where extracted. Introduce narrow new interfaces for the source,
target and evidence boundaries; do not describe them as byte-compatible with
all G1/G2 artifacts until differential tests prove that claim. XVM is a
separate package namespace under the distribution because it is independently
testable, but a separate release would add coordination without a current
consumer need. Xbox is a target plugin, not the runtime root. Schemas are
versioned separately from Python API versions.

## Public boundary and extraction order

The initial public API comprises project/bundle validation/building, an XVM
ISA/verifier/reference executor/assembler, versioned source/IR/admission/
target/evidence protocols, a replaceable Godot reference observer, and local
conformance. Reference adapters report their supported subset; an unknown
semantic feature is an explicit unsupported result, never silently lowered.
An authored intent is a distinct contract bound into the admission plan and
comparison context. Target-authored receipts bind execution and result
identities; public comparison derives success from the receipt rather than a
caller-supplied success flag. Receipt authenticity remains target-owned.
The minimal target surface can run locally without a device. Xbox transport,
worker implementation and Studio may be added after an isolated contract and
fresh provenance review.

Order: (1) public foundation, provenance and hygiene; (2) XVM specification,
reference execution and tests; (3) source/semantic/capability contracts and
Godot synthetic fixture; (4) bound evidence and conformance; (5) optional
target integrations. This puts the executable oracle before target plugins.

## Licensing and privacy risks

The private repository has no top-level license. Selected XVM/project files
and schemas have no third-party header; their relevant Git history is authored
by Daniele Cangi. This supports first-party provenance, but it is not a license
for external source assets. The user-directed extraction to the same owner's
public repository supplies authorization for these identified first-party
files. Each included file is listed in the machine-readable manifest. No
Minilens (GPL-3.0-or-later), third-party Godot project, copied game fixture,
vendor code, patch or generated asset is included. Any future uncertain file
is excluded pending provenance resolution.

The source contains private operational material, local reports, device
details and generated evidence. The public release check must scan tracked
bytes, including paths ignored by `.gitignore`, for credentials, private keys,
machine paths, local addresses, research-only names and binary packages.
Synthetic fixture inputs are authored inside XCP. Provenance paths in the
manifest identify source *repository-relative* files only, never local paths.

## Conformance and claim policy

XVM conformance verifies valid programs and controlled rejections against the
versioned ISA. Source adapter conformance checks deterministic inventory,
semantic IDs, exact source-byte binding and unsupported-feature refusal.
Target conformance checks admission before execution and exact plan binding.
Evidence conformance checks identities, tolerance handling and that execution
success alone cannot yield fidelity. A clean checkout runs these locally.
Xbox measurements from the lab are documented as historical facts only; this
repository makes no new device, GPU, game-fidelity or general Godot claim.

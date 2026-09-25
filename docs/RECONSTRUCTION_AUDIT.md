# XCP reconstruction audit

Status: **in progress**. This audit locks the first reconstruction baseline and
records missing closure. It does not certify a public source build or a new Xbox
run. The machine-readable component map is
[`provenance/reconstruction-coverage.json`](../provenance/reconstruction-coverage.json).
The per-file G1 and overlay source lock is
[`provenance/reference-source-lock.json`](../provenance/reference-source-lock.json).

## Authority and evidence boundaries

The extraction source is `Daniele-Cangi/Xbox-series-Agent`; the destination is
`Daniele-Cangi/XCP`. The source was inspected without editing it. The destination
branch for this audit starts at PR #1 commit
`9a3a52d1c3ecd5071886ae1cfa47629849db2996`, so the existing public core
remains intact. PR #1 is open and unmerged. The reference implementation must
remain distinguishable from `src/xcp`, whose new APIs and synthetic conformance
do not yet establish parity with the product toolchain or worker.

With physical Xbox validation unavailable, the Studio → lifecycle → transport
→ worker → native runtime path is behavior-frozen. Transfer it by exact extraction
or mechanical relocation. A refactor requires PC-only parity and an unchanged
hardware-facing contract; uncertainty favors preserving the original bytes.
The new public core remains alongside the reference path.

| Identity | Source | Meaning |
| --- | --- | --- |
| Frozen G1 base | `e2ff97446c1e5420c6f0c8d84b2bdeee881340f2`; kit manifest `86d2cefebd9e01d69e3eb7bd15c17b70a7a501decdcb6f0fbef109515ed46346` | Exact 72 source inputs plus generated `requirements.txt` form the 73 kit entries; the manifest is a separate file. |
| P4/r10 | source `4acb86a11396f4e3e4416afe6fc6ae18a22d3a9d`; ZIP `2f23c8ea24239e486db257df56c7aee34134f6aafdec719d70f96d746496fbb4` | Last measured Xbox Studio preview; toolchain 1.9.0, worker .181, protocol 0.73; ten operations only. |
| P5/r12 | source identity recorded as `1f847761fb1e13c8989c88036a64350db95e181e`; ZIP `0e41f63055296fb6b0599ff08749efb8c66af1f4c6442ed606210fca888fed84` | Review-hardened candidate, `P5_PREPARED_AWAITING_CLEAN_MACHINE`; **not hardware-closed**. The cited commit is not in the fetched local Git object graph or accessible through the GitHub commit API. |
| Draft source PR #18 | `acb696d3ba38d276824054ceeebb82bbe2d82182` | Current inspected source for the component map; later than some packaged component bytes. |
| Source `main` | `b08c0f25299c5e1927746ff1dabb173e22eda67e` | Separate research/main head, not a substitute for r10 or r12. |
| PR #1 | `9a3a52d1c3ecd5071886ae1cfa47629849db2996` | New public foundation and experimental API, not the product implementation. |

The r10 and r12 ZIP files were found locally and their **actual bytes** matched
the recorded ZIP digests and sizes (98,904,899 and 98,905,296 bytes). Their
embedded release manifests matched the recorded SHA-256 values
`355161fdae93e144d195521a79811e68d5b87c0fedcf2a34536fb540068d3c29`
and `98c467b4de98f6fe2b36cdaa341063525fe251d9041260936b9b43c7bdd1d4cd`.
The embedded .181 worker matched SHA-256
`0b7bf086f2423caefae876239a4ea394afa10346f15b0bf564469aed5d08d06c`.
This establishes file integrity against the recorded digests. It does not
independently authenticate the origin of the archives or repeat their Xbox
tests. The r10 receipt and capture hashes remain historical/documented; raw
operational evidence has not been approved for transfer.

## Dependency map

```text
G1 snapshot (72 source files + generated requirements) + versioned C5 overlay
    -> product toolchain 1.9.0 (P4) / 1.9.1 (P5)
    -> Studio Core and WinUI shell -> one-folder publish with XBF/PRI
    -> application-local CPython 3.13.15 + pinned wheels

project/intent + schemas -> C5 lifecycle -> transport/SDK -> worker protocol 0.73
worker C++ + shared ABI + generated headers + HLSL
    -> native module DLL + conditional topology broker DLL
    -> worker .181 MSIX -> CPU Capsule framework >= 1.3.0.0
                         -> Microsoft.VCLibs.140.00 >= 14.0.33519.0

source inventory -> Godot Source Model -> semantic/IR -> plan/lowering
    -> bundle -> worker Creative Host -> observation/evidence
XVM ISA -> Python assembler/reference + C++ verifier/interpreter + GPU backends
```

### Dependency findings and closure

1. **G1 source history — closed for sanitized public kit export:** the original Studio exporter
   calls `export_frozen_g1_kit`, which runs `git show` and `git archive` on the
   G1 commit. The checked-in source snapshot and two exact C5 overlays let
   the public wrapper build three newly identified kits without that private
   history. Four profile inputs are sanitized to remove operational evidence;
   the historical manifests remain locked references rather than public outputs.
   The original exporter control flow is still separate, and the full
   Studio/worker build is not closed by this step.
2. **Xbox package graph:** the .181 worker's actual embedded manifest requires
   `XComputeProbe.CpuCapsule.Framework`, publisher `CN=LocalDev`, minimum
   `1.3.0.0`, and Microsoft VCLibs. Both r10 and r12 ZIPs contain the worker and
   public certificate but **no capsule MSIX**. The legacy installer verifies and
   installs only the worker. A separate local capsule 1.3.0.0 MSIX exists and
   hashes to `cecced4658678bd62d6f11642ad2e91453525ee05c14a60f21449a79310ff4ce`;
   its manifest confirms framework identity/version. Its availability is not a
   substitute for closing development packaging or clean-console installation.
   The external Microsoft VCLibs APPX is inspected from the installed Windows
   SDK by package identity/version and Windows signature status; no third-party
   binary is added to XCP. This closes identification of the local SDK input,
   not provisioning or installation on a console.
3. **Native build graph:** `build.ps1` builds the native module before the
   worker. Flavor `worker-prototype-0181` maps to compiled flavor
   `worker-prototype`, which conditionally builds the topology broker. The worker
   also includes shared ABI/ledger headers, generated XVM/host-profile headers,
   HLSL and package image assets. Copying one `.vcxproj` is insufficient.
4. **Studio publish:** the WinUI project targets .NET 8 / Windows and pins
   Windows App SDK `1.8.260710003`; `CopyWinUiCompiledResourcesToPublish`
   copies generated `.xbf` and `.pri`. A successful EXE build alone does not
   prove a usable one-folder publish or startup.
5. **Historical versus development packaging:** the archive packager requires
   historical worker/certificate digests and accepts prebuilt Studio and worker
   inputs. Public development builds need independent identities, local signing,
   dependency-closed packaging and explicit untested-on-Xbox status. Historical
   r10/r12 manifests must not be rewritten to accommodate them.
6. **Binary policy:** PR #1's hygiene check originally rejected every NUL byte.
   It now admits only the four individually registered first-party XC brand PNGs
   under exact source/destination hashes; secrets, operational data and
   unexpected binary files remain denied.

### Capability coverage

| Capability | Original entrypoints / source | Current public state | Required closure |
| --- | --- | --- | --- |
| Studio shell, project library, Guided/Expert, Create/Adapt/Evolve/Playtest | `src/XComputeControlCenter/`, `tests/winui/` | Exact P4 source transferred; WinUI build/launch and extracted-distribution Studio Core create/open/edit/validate/build/close/reopen passed on PC | Full GUI click automation and worker/Xbox operation. |
| Product G1/C5 toolchain | `tools/export_xcp_agent_kit.py`, `tools/export_xcp_{g2_agent_kit,studio_toolchain}.py`, `tools/xcp_agent_lifecycle.py` | Public 1.8.0.1/1.9.0.1/1.9.1.1 kits reproduced; historical hashes retained only as locks | New worker package/evidence binding for the blind gate; lifecycle parity and Xbox operation. |
| Intent, project, evolution, adaptation | `tools/xcp_{creative_project,project_evolve,source_adapt}.py`, `tools/xcp_adaptation/`, schemas/profiles | Original G1 implementations and schemas transferred; original synthetic adaptation/evolution tests now run on PC, with one evolution assertion aligned to the sanitized public profile; PR #1 contracts remain separate | External-source fidelity, human playtest and live worker results. |
| Bridge, transport and worker SDK | `tools/xcp_studio_live_adapter.py`, `tools/xcp_worker_transport.py`, `schemas/worker-sdk-contract-v1.json` | Exact reference source; seven original adapter tests and seven new PC transport/session vectors pass; `PENDING_PARITY` | Add worker SDK and real socket integration tests, then Xbox operation. |
| XVM CPU | ISA, Python assembler/reference, `runtime/WorkerXvm*` | Exact P4 Python tools, samples and C++ source transferred; 25 original PC tests, ISA header check, two assembly flows and seven bounded P4/PR #1 Python comparisons pass | Compare snapshot behavior, Python/C++ execution vectors and Xbox behavior. |
| Creative Host and lifecycle | `runtime/WorkerCreative*`, `WorkerCommandServer*` | P4 source/profile transferred and compiled on PC; six original static/schema lifecycle tests pass | Exercise running-worker PC vectors and Xbox lifecycle/foreground/input/audio/observation. |
| GPU/scheduling | `runtime/WorkerGpu*`, async/graph runtime, `shaders/` | P4 source transferred; Release native/shader build passed | Run profile mapping and bounded differential tests. |
| Storage/persistence | artifact/CAS/recovery/reservation/persistent runtimes and shared ledger | P4 source transferred and compiled; two original recovery schemas plus six PC static contract checks present; older evidence-bound gate tests mapped separately | Real restart/recovery and storage-scale execution on Xbox. |
| CPU capsule, native module, broker, ABI | four native projects and `src/shared/` | P4 sources transferred; distinct development worker and Capsule MSIX built, graph checked and freshly signed; SDK VCLibs verified externally | Clean installation, dependency loading and runtime checks on Xbox. |
| Build/runtime/packaging | `tools/build.ps1`, `package.ps1`, runtime and preview packagers | Exact P4 scripts plus disposable identity staging; integrated development ZIP assembled and fresh-extraction Studio Core project workflow passed on PC | Full GUI exercise and device installation/operation. |
| Community tests/evidence | P4/P5 documents and portable validator | PC conformance, Studio Core, adapter/transport and Windows build checks present; clean-console attempt and redacted report protocol prepared | Execute the protocol on owned hardware and review evidence. |

The machine map records a specific status for each component. `SOURCE_IDENTIFIED`
is static inspection only. `EXACT_EXTRACTION` means equal bytes, not proven
behavior; `MECHANICAL_RELOCATION` means paths changed; `PARITY_VERIFIED_REFACTOR`
requires paired original/public results; `NEW_IMPLEMENTATION` cannot inherit
private evidence. `PENDING_PARITY`, `EXCLUDED_WITH_REASON` and `BLOCKED` remain
visible until resolved. The new `src/xcp` code is kept as useful work, but is
not a replacement for the G1/C5 reference or the worker.

## Build inputs and current verification

The inspected native projects specify `PlatformToolset=v145` and
`WindowsTargetPlatformVersion=10.0`. `build.ps1` discovers MSBuild on the host,
and the capsule packaging script also requires MakeAppx and local signing.
Studio requires .NET 8, `win-x64`, WinUI and the pinned App SDK. The runtime
profile pins CPython 3.13.15 plus five wheels and their digests. This machine
has `dotnet` and Python on PATH and MSBuild plus MakeAppx/FXC under Visual
Studio/Windows Kits. Staged Studio and native worker builds now pass from XCP
source. The integrated development ZIP and fresh-extraction PC workflow pass
from a source snapshot without `.git`; Xbox validation remains open.

The private source has no top-level license. The same owner has authorized
first-party extraction, as recorded for PR #1, but each imported asset and
external dependency still needs provenance review. The old preview's
`THIRD_PARTY_NOTICES.md` is explicitly preliminary. Do not commit packaged
runtime binaries, signed historical MSIX, certificates with private keys,
operational logs, pairing/session material, third-party game assets or local
paths. Sample redistribution remains pending license confirmation.

## Ordered closure and review gates

1. Commit this audit and its locked source/component map as a PR based on PR #1.
2. Carry the G1 code and schemas into reviewed public files, sanitize the four
   operational-evidence profiles, and implement an exporter that never reads
   private Git history. Preserve historical hashes separately and assign new
   identities to the 73-entry public kits with C5 overlays.
   **Completed for sanitized public kit identity in the toolchain tranche.**
3. Transfer Studio Core/shell and its test projects/resources; build and launch
   the one-folder publish from the public checkout.
4. Transfer the worker, native module, topology broker, CPU capsule, shared
   headers and shaders together; build and inspect the development MSIX graph.
   **Source transfer, local Release builds, unsigned framework package graph and
   fresh development signing in CI completed; clean installation remains open.**
5. Transfer broad GPU, storage and adaptation capabilities with paired PC
   vectors and explicit feature-level verification states.
6. Run isolated public-only builds/CI and document a safe community Xbox
   protocol. P4 remains historical; P5 stays open until its separate clean
   machine gate actually passes.

Each step must update the machine map and name tests run, tests unavailable and
remaining dependencies. Capability coverage, not file count, controls any
future completeness claim.

## Reference toolchain tranche result

The public reference tree contains 75 transferred files: 71 byte-exact code,
schemas and overlays, plus four profiles sanitized to exclude operational
evidence. The staged Git blobs are checked against exact source hashes or
explicit sanitized hashes after `.gitattributes` disabled line-ending conversion
for `reference/`. The historical source lock and G1/P4/P5 manifest hashes remain
unchanged. The public wrapper produces distinct 1.8.0.1, 1.9.0.1 and 1.9.1.1
kits without a Git call or source-repository access; their hashes are in the
[toolchain guide](REFERENCE_TOOLCHAIN.md) and machine coverage map. Tests check
paths with spaces and reject a modified exported lifecycle file.

Verification: 39 Python tests passed, all four existing PR #1 conformance
suites passed, and public hygiene plus the reconstruction lock validator passed.
A source-only `git archive HEAD` of the sanitized XCP branch, extracted without
`.git` and installed in a fresh virtual environment, passed nine targeted tests
and reproduced all three new public kit digests with Git absent from the process.
Studio startup, original
exporter error-path parity, Windows native builds, package dependency
installation, the complete isolated destination-only build and Xbox operation
remain untested in this tranche. No hardware-facing behavior was refactored.

## Studio source tranche result

The 53 Studio source/test files are exact P4/r10 source copies, and a source
diff confirms they are byte-identical in the locally available draft P5 head.
That does not authenticate the unavailable P5/r12 source commit. A public
staging command combines them with the newly identified public 1.9.1.1 kit while
leaving the reference files untouched. A source-only XCP archive without `.git`
passed nine targeted Python tests and staged Studio with Git absent from the
preparation process. In that staged workspace, Release C# tests passed `34/34`,
the solution built with zero warnings/errors, and publish included the
executable, ten XBF files and the application PRI. See the
[Studio build guide](REFERENCE_STUDIO.md). Runtime bootstrap, actual shell
startup, worker dependency closure and Xbox behavior remain open.

Four creative profiles are sanitized in the public tree under an explicit
provenance transformation. Historical G1/P4/P5 identities remain separate
reference locks, and the public kits use newly assigned digest identities.
Frozen lifecycle, Studio and worker-facing source remains unchanged.

## Windows runtime and local Studio startup tranche

The exact P4 runtime builder and its pinned profile were transferred as two
additional first-party reference files. Both have identical bytes in the
locally available draft P5 head. The runtime was built from the destination
checkout using its CPython 3.13.15 embedded ZIP and five SHA-256-pinned wheels;
the original builder's isolated import smoke and manifest verification passed.
The generated runtime has 168 files and 22,789,928 payload bytes. It is a new
local build, not a reproduction of the historical P4/P5 runtime identity.

The destination-only development assembler verified the publish executable,
PRI and ten XBF files, exported the sanitized 1.9.1.1 kit, copied the verified
runtime, and wrote a manifest marked `NOT_TESTED_ON_XBOX`. On a local Windows
desktop the assembled executable stayed running and exposed a main window
titled `XCP Studio` after eight seconds. No UI workflow, worker, device
connection or Xbox behavior was exercised. The new Windows CI job covers
staging with spaces in the path, 34 Core tests, solution build, publish
resources, pinned runtime build and development-folder assembly. The remote
Windows Studio job passed; hosted CI does not prove interactive startup.
See the [Studio guide](REFERENCE_STUDIO.md) and
[Copilot triage](COPILOT_REVIEW_TRIAGE.md).

## P4 native worker and capsule source tranche

The destination now includes 230 native source/resource files and five build
scripts from the P4 commit. The only changed reference file is the OneDrive
guard's source-machine-specific error text, recorded as a mechanical relocation.
The four individually registered PNGs are first-party XC brand artwork from
the owner's initial project commit. The static checker found every explicit
native project input and confirmed the `.181` worker dependency on the CPU
Capsule framework `>=1.3.0.0` with matching publisher.

A destination-only staged Release build with MSBuild 18.7.8/v145 and Windows
SDK 10.0.26100 completed the native module, broker, worker and capsule DLL
with zero warnings/errors. The worker MSIX contained the native module,
broker DLL/WinMD and VCLibs dependency. A separate **unsigned** capsule
`1.3.0.0` MSIX was built without accessing the existing local signing key;
the generated package pair passed the graph checker. All outputs are ignored
local build artifacts. The rebuilt worker still carries the source manifest's
`.181` version but does **not** inherit the historical `.181` hash or Xbox
evidence. Both initial Windows native CI runs passed. A source-only archive
without `.git` staged the same 235 files and, with limited compiler
parallelism after a PCH-memory failure, built the worker/Capsule and passed
package graph checks. A fresh, nonexportable development-signing script passed
in both Windows worker CI runs and emitted distinct package digests and a
`NOT_TESTED_ON_XBOX` manifest; a distinct development package version,
clean-console dependency installation, runtime parity and hardware validation
remain open.
See the [worker guide](REFERENCE_WORKER.md).

## P4 XVM Python reference tranche

The P4 Python assembler, verifier/interpreter, ISA header generator, five
synthetic programs, two assembly sources and original reference test were
transferred exactly. The generator confirms the frozen C++ header still matches
the ISA JSON. Twenty-five of 26 original Python tests pass on PC; the remaining
test reads a historical
live runner with a hard-coded device address, which is excluded from the
public source. The exact test remains present and that one case is explicitly
deselected in CI. Five execution results and two rejection codes also match
the separate PR #1 Python API on PC. Two assembly examples produce canonical
artifacts, validate and execute through the preserved P4 CLIs. See the
[XVM guide](REFERENCE_XVM.md).
Snapshot provenance and Python/C++ execution parity remain open; there is no
new Xbox run.

## P4 creative-host lifecycle contract

The P4 development profile and six original tests are now exact reference
copies. The profile validates against its schema; the tests connect its
declared lifecycle operations to the preserved native worker source. This
closes a PC-visible contract check only. No install, foreground, input, audio
or observation behavior has been rerun on an Xbox.

## PC-only Studio bridge check

The original P4 Studio live-adapter test is now preserved byte for byte beside
the reference Python tools. It uses a fake worker and synthetic session values,
so it exercises admitted commands, request rejection, SDK bootstrap, worker
error projection and the CLI description without contacting a device. Its
seven tests pass from the destination checkout and run in portable CI. This
establishes those PC-visible results for the frozen implementation; it does not
validate a real socket, worker, package installation or Xbox input. Seven new
PC-only vectors also exercise the unchanged transport's JSON-line framing,
response/error handling, protocol negotiation and session cleanup through
synthetic socket/client objects. They run in the portable test suite.
The separate source test that embeds a historical worker package digest was
not transferred under the operational-evidence publication boundary.

## Community reproduction boundary

The [community protocol](COMMUNITY_XBOX_REPRODUCTION.md) links the checked-in
PC build paths to the external VCLibs dependency, a fresh development
certificate, a clean Developer Mode console attempt and an identity-aware
redacted report. It has not been executed on hardware. It does not authorize
publishing raw console logs, captures or session material.

## P4 project builder PC test

The exact P4 unit test and three synthetic `hello-shapes` JSON inputs are now
preserved beside the original project builder. All 23 tests pass from the
destination source and run in portable CI. This exercises the unchanged
project/bundle path before worker installation; it does not imply lifecycle
parity on a console.

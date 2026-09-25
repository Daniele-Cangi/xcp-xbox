# XCP development distribution

The development ZIP is assembled from the XCP checkout on Windows. It contains
Studio/WinUI, its pinned application-local Python runtime, the reviewed product
toolchain, a first-party example, documentation, a file manifest, and freshly
signed development worker and CPU Capsule packages. The Microsoft VCLibs Retail
x64 APPX remains **external**: the build checks its identity and Authenticode
signature in an installed Windows SDK and records the result without copying
Microsoft's binary.

## Build and exercise on a Windows PC

Use Visual Studio with MSVC v145, a Windows SDK with MakeAppx and SignTool,
.NET 8, Python 3.12, and enough local disk space for native intermediate files.
From a fresh XCP checkout:

```powershell
New-Item -ItemType Directory -Force build | Out-Null
& tools/build_development_distribution.ps1 -OutputRoot build/development-run
```

The command verifies the frozen P4 source graph, copies it to `build/`, applies
only development identity substitutions and lower build parallelism in that
disposable copy, builds native worker/module/broker and Capsule, signs the two
MSIX files with a new nonexportable local key, builds/tests/publishes Studio,
builds/verifies pinned Python, assembles a ZIP, extracts it to a new folder,
checks every file hash and runs a PC Studio Core workflow from the extracted
files. It creates, opens, edits, validates, builds, closes and reopens a project;
it also validates and builds the included `hello-shapes` example. The signing
key is removed from the local certificate store after the build; the ZIP
contains only the public certificate. Windows CI creates a source snapshot,
verifies it without `.git`, runs the same complete build from that snapshot,
and verifies the ZIP after fresh extraction. CI does not upload the ZIP.

To reproduce the no-Git source route from a clean checkout:

```powershell
python tools/source_snapshot.py --archive build/xcp-source-snapshot.zip
Expand-Archive build/xcp-source-snapshot.zip build/source-snapshot
python build/source-snapshot/tools/source_snapshot.py --verify-root build/source-snapshot
& build/source-snapshot/tools/build_development_distribution.ps1 -OutputRoot build/full-development
```

The archive carries a generated file manifest. The verifier hashes every
source file, rejects unlisted files, and recomputes the Git tree identity from
the archived bytes without a `.git` directory. The source commit is identified
as a producer assertion in a snapshot build; the tree and file bytes are
verified independently. Keep the source ZIP digest from the creator output
when transferring the snapshot to another machine.

The ZIP is `build/development-run/xcp-development-distribution.zip`. Its
manifest is `xcp-development-distribution.json`; package hashes, signing
certificate thumbprint and VCLibs inspection record are under `packages/`.
`build-provenance.json` records the Git commit claim and verified tree, hashes of every
tracked source file, the build recipe, resolved tool versions, and hashes of
the Studio, Python, toolchain, signed-package and external VCLibs inputs.
`LICENSE`, `NOTICE`, `THIRD_PARTY_NOTICES.md`, and `licenses/manifest.json` inventory the
first-party license and exact .NET/Python dependency terms. The self-contained
Windows App SDK and .NET runtime terms are copied from the pinned NuGet packages;
the Python terms are copied or referenced from the embedded runtime. The
Windows SDK .NET targeting pack's official terms and REDIST list explicitly
address the two DLLs in this version; see the [file-specific evidence](WINDOWS_SDK_NET_REDISTRIBUTION.md).
The checkpoint remains private until binary publication is explicitly approved.
The `verification/` folder contains the PC workflow probe and extraction
checker. A standalone recheck of an existing ZIP uses:

```powershell
python tools/verify_development_distribution.py `
  --archive build/development-run/xcp-development-distribution.zip `
  --extract build/another-fresh-extraction
```

## Package identity and updates

The frozen P4/r10 worker is `XComputeProbe.WorkerPrototype` version
`0.1.181.0`; its Capsule dependency is
`XComputeProbe.CpuCapsule.Framework` version `1.3.0.0` and publisher
`CN=LocalDev`. These names and the historical package hashes are unchanged in
the reference tree. The development build uses distinct package families:

| Package | First development version | Publisher |
| --- | --- | --- |
| `XCP.Development.Worker` | `0.1.182.0` | `CN=LocalDev` |
| `XCP.Development.CpuCapsule.Framework` | `1.3.0.0` | `CN=LocalDev` |

The worker's dependency manifest and its C++ package-name selector are
substituted together in staging. Capsule version constants, package manifest
and DLL version macros are also substituted together. The source reference is
never edited. The development worker's `PhoneProductId` is distinct from the
historical one. Source behavior and package graph beyond these identity
substitutions remain the P4 implementation; this is **not** evidence of Xbox
parity.

An existing historical worker or Capsule belongs to a different family, so
the development packages do not update it. For the *next* development release,
keep each development `Name` and `Publisher`, use versions higher than both
previous development packages, and pass the prior signed package manifest:

```powershell
& tools/build_development_distribution.ps1 `
  -OutputRoot build/development-update `
  -WorkerVersion 0.1.183.0 `
  -CapsuleVersion 1.3.0.1 `
  -PreviousManifest build/development-run/signed-packages/xcp-worker-development-packages.json
```

The staging tool rejects unchanged or lower versions during an update. A
freshly generated certificate has the same publisher subject but a new public
certificate; a target device must trust that new certificate. Package family
and version rules are documented in Microsoft's
[package identity overview](https://learn.microsoft.com/en-us/windows/apps/desktop/modernize/package-identity-overview),
[MSIX update constraints](https://learn.microsoft.com/en-us/windows/msix/app-package-updates),
and [framework dependency schema](https://learn.microsoft.com/en-us/uwp/schemas/appxpackage/uapmanifestschema/element-packagedependency).
No installation or upgrade has been attempted on an Xbox from this build.

## Evidence boundary

| Check | What actually runs | Evidence class |
| --- | --- | --- |
| Fresh ZIP extraction workflow | Extracted Studio Core assembly, pinned Python and product toolchain; new and example projects | Real PC execution; no WinUI click automation |
| Original adaptation/evolution tests | Original Python with synthetic authorized source fixtures | Real PC execution on fixtures; external-source fidelity open |
| XVM reference/assembler tests | Original Python ISA, assembler and interpreter | Real PC execution; bounded PR #1 comparison vectors only |
| Adapter/transport tests | Product adapter with fake worker and synthetic sessions | Mock; no real socket or worker |
| Storage/recovery checks | Two original schemas and six static C++ safety/contract checks | Static PC contract; no recovery execution |
| Native build/package checks | MSVC, HLSL, MakeAppx, SignTool and package graph inspection | Real compilation/packaging; no worker execution |

The full original adaptation/evolution test pair runs in Windows CI. On Linux,
the original adaptation case
`test_shared_backend_generates_world2d_for_current_host` is excluded while the
other original cases still run. The reference inventory uses the host's
`mimetypes.guess_type()` for WAV files, but the original audio lowering accepts
only `audio/wav`; Linux can classify WAV as `audio/x-wav`. The observed Linux
failure omitted the audio module. This is a recorded host-dependent reference
behavior, not a reason to change the frozen implementation or a claim of
cross-platform parity.

The PC storage checks carry over the useful contract assertions from the
original recovery gate for journal hash chaining, committed-root publication,
bounded fault cases and reservation ordering. The original recovery and scale
test files also bind earlier package versions, Xbox measurement hashes and
trusted-live runners. Those historical assertions are recorded in the coverage
map rather than run as evidence for this development package. No native
recovery, restart or 8 GiB scale behavior was executed on PC.

P4/r10 remains the last measured Xbox reference. P5/r12 remains
review-hardened and not hardware-closed. Every new development package is
`NOT_TESTED_ON_XBOX` until a contributor performs and reports a separate
device attempt. No real Python-to-C++ runtime parity or native storage/GPU
parity is claimed by these PC checks.

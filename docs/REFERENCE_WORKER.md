# P4 worker and CPU Capsule reconstruction on Windows

The P4/r10 worker tree is preserved under `reference/xcompute-probe/src/` with
its native module, topology broker, shared ABI, generated headers, HLSL sources,
package manifests and four XC brand PNG assets. The five required build scripts
are under `reference/xcompute-probe/tools/`. All 230 native files and four of
five scripts are byte-exact from source commit
`4acb86a11396f4e3e4416afe6fc6ae18a22d3a9d`. The OneDrive guard has a
mechanically relocated error message that removes a source-machine path;
its workspace check and failure behavior remain the same. Each file has a
source and destination digest in the reference extraction manifest. The XC
brand PNGs entered with the owner's initial project commit and are admitted
individually by release hygiene; other binary source files remain denied.

The frozen worker `.181` manifest depends on `XComputeProbe.CpuCapsule.Framework`,
publisher `CN=LocalDev`, minimum version `1.3.0.0`, and on Microsoft VCLibs
`14.0.33519.0`. The capsule source manifest starts at `1.0.0.0`; the original
packager sets the requested `1.3.0.0` version in a generated layout and passes
the version to compilation. The worker project builds the broker for compiled
flavor `worker-prototype`; the build script compiles the native module first.
The static graph checker resolves explicit project inputs and these package
identities without executing Xbox code.

The P4 creative-host development profile and its six original lifecycle tests
are preserved exactly. The tests validate the profile against its schema and
inspect the unchanged native source for admitted install, activation, rollback,
foreground and error contracts. Run them from `reference/xcompute-probe/` with
`python -m pytest -q tests/unit/test_xcp_creative_install_lifecycle_v1.py`.
They pass on PC and run in portable CI; they do not exercise a running worker.

The Microsoft VCLibs dependency is an external SDK package, not XCP source.
`tools/inspect_reference_vclibs.py --package <path-to-retail-x64-appx>` checks
its package identity, architecture, minimum version and archive integrity
without copying it into XCP. On this Windows machine, the Microsoft VCLibs
Extension SDK Retail x64 APPX is version `14.0.33519.0`; its SHA-256 is
`9c17b521f9d690a1f504da5108ed6eec5669eb3a8fd1331eef43e40d84e74283`.
The local Windows Authenticode check reports `Valid`. Windows worker CI locates
the package in its installed SDK and repeats the identity and signature check.
The digest records this inspected SDK instance; it is not a pinned requirement
for every developer machine, and the Microsoft binary is not checked in.

For the integrated development build, follow
[the development distribution guide](DEVELOPMENT_DISTRIBUTION.md). It stages
distinct worker and Capsule package families and verifies the complete ZIP
after extraction. The command below remains a frozen-graph diagnostic build.

For that diagnostic PC build, use a VS 18 developer PowerShell with MSVC `v145` and a
Windows 10/11 SDK providing MakeAppx and HLSL compilation:

```powershell
New-Item -ItemType Directory -Force build | Out-Null
python tools/prepare_reference_worker.py --output build/worker-p4
python tools/check_reference_worker_graph.py
& build/worker-p4/tools/build.ps1 -Configuration Release -ManifestFlavor worker-prototype-0181
$msbuild = (Get-Command MSBuild.exe).Source
$makeappx = (Get-Command makeappx.exe).Source
& tools/package_reference_capsule_unsigned.ps1 -StageRoot build/worker-p4 -MSBuildPath $msbuild -MakeAppxPath $makeappx
```

The staging command copies the locked files to a new ignored workspace;
`build.ps1` may select its active manifest and write generated files there,
without modifying the reference tree. The capsule wrapper rebuilds the DLL
with version `1.3.0.0`, produces a package layout with that manifest version,
and packs an **unsigned** framework MSIX. It never invokes the historical
signer or reads a private key. The worker build may create an `.181` MSIX;
this is a new local artifact with different bytes, not the historical worker
whose SHA-256 is locked in the reconstruction audit. Do not treat the package
name/version alone as historical identity.

To validate generated packages, pass their full paths to
`tools/check_reference_worker_graph.py --worker-package ... --capsule-package ...`.
The checker requires the broker DLL/WinMD, native module DLL, capsule DLL,
framework publisher/version, and VCLibs dependency. In this Windows run,
Release MSBuild 18.7.8 with `v145` and SDK 10.0.26100 built the worker, broker,
native module and capsule with zero warnings/errors; the unsigned capsule
package and rebuilt worker MSIX passed the package graph check. The Windows
native CI job repeats the build, graph inspection and development signing. It
fails explicitly when `v145` or Windows packaging tools are unavailable.

A source-only archive of the destination head, extracted without `.git`, also
staged the 235 worker inputs and passed the package graph checker. Its first
parallel native build exhausted compiler PCH memory. Rebuilding the same
staged source with MSBuild `/m:1` and
`/p:MultiProcessorCompilation=false` succeeded, then built the Capsule and
passed the generated package graph check. That run emitted only MSBuild's
warning about placing intermediate outputs under a temporary directory.
The reduced parallelism changes build resource use, not source or worker
behavior.

`tools/sign_reference_development_packages.ps1` accepts **only** the distinct
development package families. It signs copies of the two unsigned packages
with a fresh, nonexportable self-signed `CN=LocalDev` certificate, selects that
certificate by thumbprint, exports only the public `.cer`, and removes the
fresh key from the local store when finished. The source `.181` package is not
signed by this command. Development versions and update rules are recorded in
[the development distribution guide](DEVELOPMENT_DISTRIBUTION.md).

Earlier Windows worker CI runs exercised fresh signing of the historical-name
rebuild without retaining an artifact. The integrated development job checks
the new identities and verifies the ZIP after extraction. CI does not upload
the ZIP. This is a PC packaging check, not an install or Xbox run.

Clean-console installation would require trusting the new public certificate,
provisioning the Microsoft VCLibs dependency from an authorized SDK source,
then installing the Capsule before the worker. That order is a package-graph
inference, not a completed Xbox test. Runtime behavior and Xbox validation
remain open. P4/r10 remains the last measured Xbox reference; P5/r12 remains
a review-hardened candidate without hardware closure.

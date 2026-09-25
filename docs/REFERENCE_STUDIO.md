# Reference XCP Studio source and PC build

The public `reference/xcompute-probe/src/XComputeControlCenter/` and
`reference/xcompute-probe/tests/winui/` trees contain 53 first-party files
(307,333 bytes) copied exactly from the P4/r10 source commit
`4acb86a11396f4e3e4416afe6fc6ae18a22d3a9d`. These paths are byte-identical
in the locally available draft P5 commit
`acb696d3ba38d276824054ceeebb82bbe2d82182`. This comparison does not
establish the unavailable P5/r12 source commit's identity. Every file has a
source path and SHA-256 in the
[reference extraction manifest](../provenance/reference-extraction-manifest.json).
The Studio code and C# tests retain their original bytes and relative paths.

The original tests locate `tools/xcp_agent_lifecycle.py` in an ancestor folder.
Running them directly in the reference tree selects the G1 lifecycle, so use
the public staging command to combine the Studio files with a verified versioned
kit in a **new** build workspace:

```powershell
New-Item -ItemType Directory -Force build | Out-Null
python tools/prepare_reference_studio.py --mode public-studio-1.9.1 --output build/studio-191
cd build/studio-191
dotnet test tests/winui/XComputeControlCenter.Core.Tests/XComputeControlCenter.Core.Tests.csproj -c Release
dotnet build src/XComputeControlCenter/XComputeControlCenter.sln -c Release
dotnet publish src/XComputeControlCenter/XComputeControlCenter/XComputeControlCenter.csproj -c Release -o ../studio-publish
```

The stage is a composite **build workspace**: the newly identified 1.9.0.1 or
1.9.1.1 public kit is verified before Studio source and tests are copied into
it. The added files mean the stage itself is not a kit artifact. For a standalone
public kit, use the [toolchain exporter](REFERENCE_TOOLCHAIN.md). The original
historical manifest hashes are reference locks only.
The command uses only checked-in XCP source; it never reads the original
repository or invokes Git. .NET 8 SDK and NuGet access to the project's pinned
test packages and Microsoft.WindowsAppSDK `1.8.260710003` are required for
the .NET commands.

On Windows, a source-only XCP archive containing the sanitized public kit was
extracted without `.git` and installed in a fresh virtual environment. With Git
absent from the preparation process, nine targeted Python tests passed, all
three public kit digests were reproduced and verified, and the Studio stage
was created. In that stage, the Release C# tests passed `34/34`; the Release
solution built with zero warnings and errors. Release publish produced the
WinUI executable, 10 XBF files and `XComputeControlCenter.pri` in a 516-file
output. These are PC build/resource checks.

The exact P4 runtime builder and profile now live under `reference/xcompute-probe/`;
their bytes also match the available draft P5 head. Build the pinned, isolated
CPython 3.13.15 runtime and assemble a separate development folder:

```powershell
python reference/xcompute-probe/tools/build_xcp_studio_python_runtime.py build --profile reference/xcompute-probe/profiles/studio/xcp-studio-python-runtime-v1.json --cache build/python-input-cache --output build/studio-python
python tools/assemble_reference_studio.py --publish build/studio-publish --runtime build/studio-python --output build/studio-dev
build/studio-dev/XComputeControlCenter.exe
```

The runtime builder downloads the CPython embedded ZIP and five exact wheels
from the profile, verifies every pinned SHA-256, and runs an isolated import
smoke before writing its file manifest. The assembler verifies that manifest,
exports the sanitized public toolchain, checks the WinUI executable/PRI/XBF
resources, and creates a development manifest with new identities and
`NOT_TESTED_ON_XBOX`. Generated binaries stay under ignored `build/`; they are
not historical P4 or P5 archives. The integrated distribution copies the
available pinned runtime and NuGet licenses and notices and records their
hashes. The Windows SDK .NET targeting pack's URL resolves to official terms
and a REDIST list that names the two DLLs in Studio; see
[the redistribution record](WINDOWS_SDK_NET_REDISTRIBUTION.md).

On a local Windows desktop, the assembled executable remained running after
eight seconds and exposed a main window titled `XCP Studio`. This is a PC shell
startup smoke with no device connection or UI workflow exercise. The Windows CI
job repeats staging, 34 Core tests, solution build, publish/resource checks,
runtime build and development-folder assembly. CI does not assert interactive
window startup, worker installation, transport to Xbox or Xbox behavior.

The original XAML includes an inert example project path and a private-range
example address as placeholder text. Public hygiene allows only those two
exact literals in their exact source files. They are not configured paths,
default device addresses, credentials or session material. The transferred
files remain byte-identical; any change to either file fails provenance hash
verification.

P4/r10 remains the last preview measured on Xbox. P5/r12 remains a
review-hardened candidate without clean-machine or hardware closure. This
Studio extraction changes neither historical identity nor hardware-facing
behavior, and it does not replace Studio's workflow with the new public core.

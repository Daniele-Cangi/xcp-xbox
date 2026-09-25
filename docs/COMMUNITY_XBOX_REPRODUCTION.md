# Community Xbox reproduction protocol

Status: **prepared, not run against an Xbox from this reconstruction**. This is
an attempt protocol for a contributor who owns an Xbox in Developer Mode. It
does not extend the P4/r10 hardware evidence to a new build or close P5/r12.

## Build from this checkout on a Windows PC

1. Record the XCP commit and keep the checkout on a local path without cloud
   sync. Follow the [development distribution guide](DEVELOPMENT_DISTRIBUTION.md)
   to build Studio, pinned Python, toolchain, native worker, CPU Capsule and the
   complete ZIP. Require the fresh-extraction PC project workflow to pass.
   Use the generated public `.cer`; never use a historical signing key.
2. Confirm the generated package graph binds `XCP.Development.Worker` to
   `XCP.Development.CpuCapsule.Framework` with the same publisher and exact
   minimum Capsule version. Keep P4 source and historical packages untouched.
3. Obtain the Microsoft VCLibs Retail x64 APPX from an authorized Windows SDK
   installation. Run `tools/inspect_reference_vclibs.py` on that local package
   and check its Windows Authenticode status. Keep this Microsoft binary outside
   the XCP repository. The worker requires `Microsoft.VCLibs.140.00` at least
   `14.0.33519.0`, and the first-party Capsule framework at least `1.3.0.0`.

The development worker starts at version `0.1.182.0` in a distinct package
family. The development Capsule also has a distinct family. Historical
installations must remain available as references; inspect the target's
installed packages before a device attempt and do not uninstall reference
data as part of this protocol. No development installation or upgrade has yet
been validated on an Xbox.

## Attempt installation and one bounded flow

Microsoft documents Xbox sideloading through the Xbox Device Portal with a
signed app package and its dependencies. Use the portal's app installation
flow to supply the locally built worker MSIX, fresh public `.cer`, CPU Capsule
MSIX and the SDK VCLibs APPX. The dependency graph suggests provisioning
VCLibs and Capsule before the worker; this order is an inference that still
needs a clean-console test. Follow the portal's actual prompts and record any
dependency or certificate error code. Do not put an Xbox address, pairing code
or session identifier into a script, commit or report. The official
[Xbox development options](https://learn.microsoft.com/en-us/windows/uwp/apps-for-xbox/development-options),
[Device Portal installation guide](https://learn.microsoft.com/en-us/windows/advanced-settings/device-portal),
and [WinAppDeployCmd reference](https://learn.microsoft.com/en-us/windows/uwp/packaging/install-universal-windows-apps-with-the-winappdeploycmd-tool)
describe the supported deployment surfaces.

After installation, launch the worker on the console, then launch the assembled
Studio on the PC. Use a newly authored synthetic project with no third-party
game content. Exercise one local preview before attempting the live path.
Attempt only the Studio operations exposed by the checked-in build and record
the first failing stage. Do not infer a pass for unattempted operations. In
particular, a visible Studio window or installed package does not prove that
worker lifecycle, transport, input, rendering, audio or evidence capture works.

## Report useful evidence without private material

A contribution can record these fields in a plain text issue or PR description:

- XCP commit; Studio public-kit manifest digest; development worker and Capsule
  package digests; VCLibs identity/version and digest; SDK/MSVC/.NET versions.
- PC test counts; package install result; worker launch result; Studio launch
  result; each attempted operation and its structured error code or success.
- Whether the result was PC-only or observed on an owned Developer Mode Xbox.
  Use `NOT_TESTED_ON_XBOX` whenever the device stage was not performed.

Keep console addresses, device IDs, usernames, pairing/session values,
certificate private keys, raw logs, captures, receipts and local absolute paths
out of the report. Retain those locally for diagnosis and share only a reviewed,
redacted extract if a maintainer needs it. A new report has its own commit and
package identities. It cannot replace the frozen P4/r10 or P5/r12 references.

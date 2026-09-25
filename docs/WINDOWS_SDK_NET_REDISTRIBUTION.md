# Windows SDK .NET targeting pack redistribution evidence

This record applies to the private XCP development distribution. It does not
authorize a public binary upload or change repository visibility.

Studio's `XComputeControlCenter.deps.json` identifies
`runtimepack.Microsoft.Windows.SDK.NET.Ref/10.0.19041.57`. The [exact NuGet
version](https://www.nuget.org/packages/Microsoft.Windows.SDK.NET.Ref/10.0.19041.57)
provides a nuspec `licenseUrl` of
<https://aka.ms/WinSDKLicenseURL>. On 24 September 2026 that official URL
resolved to Microsoft's `sdk_license.rtf` at
<https://download.microsoft.com/download/0/F/F/0FF2B061-47DD-4F55-89B6-FD1D8C44F14D/sdk_license.rtf>
(SHA-256 `dd07eb178e00c6bba4148457fc00ff77cd4887eb521d504186fe59c9ec8bbe62`).
The downloaded terms identify the Windows SDK REDIST list through
<https://go.microsoft.com/fwlink/?LinkId=524842>, which resolves to
Microsoft's [Windows SDK REDIST list](https://learn.microsoft.com/en-us/legal/windows-sdk/redist).

The official REDIST list expressly includes files from the
`Microsoft.Windows.SDK.NET.Ref` NuGet package as an unmodified package or as
part of an application calling WinRT APIs, subject to the SDK terms. It names
`lib/net8.0/Microsoft.Windows.SDK.NET.dll` and
`lib/net8.0/WinRT.Runtime.dll` (the published list has an extra slash before
the latter filename). The local 10.0.19041.57 package and assembled Studio
output matched byte for byte:

| File | SHA-256 in package and output |
| --- | --- |
| `Microsoft.Windows.SDK.NET.dll` | `0ec371d93798852e36461c8adddbeadce0f963a04752f0b64e54fe19c1c834a7` |
| `WinRT.Runtime.dll` | `bcf3a14e8712a90837fc5c0d8c8a24696af2bd7f74e9767597ec81edebf23db` |

The distribution builder verifies both hashes against the exact NuGet package
on every build and records the result in `licenses/manifest.json`. The NuGet
package's lack of an embedded `LICENSE` file is therefore not treated as a
redistribution prohibition. These files are not assigned MIT or Apache-2.0
terms. The Microsoft SDK terms and REDIST conditions apply to these files;
other bundled dependencies retain their own terms.

The downloaded SDK terms condition distribution on the code remaining part of
a program with significant primary functionality, preservation of notices,
end-user/distributor terms protecting the code, a valid program copyright
notice, and the stated indemnity and use restrictions. The linked terms are
the controlling text; this summary is only an inventory of conditions to
check for a later binary publication decision.

Automatic CI artifact upload of the complete ZIP is disabled. A source-only
publication can proceed independently of a later explicit decision about
binary distribution and remaining dependency review. Historical CI artifacts
from earlier private runs must be checked before any visibility change.

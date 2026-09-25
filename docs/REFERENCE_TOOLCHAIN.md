# Product toolchain: historical locks and public builds

The public reference tree contains the original G1 exporter, 72 G1 inputs and
two versioned C5 lifecycle overlays. Of these 75 files, 71 are byte-exact
first-party transfers. Four creative profiles contain a **public sanitation**
of historical operational evidence; their original source hashes and byte
counts remain in the immutable
[historical source lock](../provenance/reference-source-lock.json). The
[extraction manifest](../provenance/reference-extraction-manifest.json) records
each public file's hash and its transformation. No private run reports,
package/receipt digests, device identity or source-specific project revision
from those four profiles is needed to build the public kits.

The original G1 exporter and both lifecycle overlays retain their source bytes.
The public wrapper checks those bytes and the four sanitized profile identities,
then runs the original exporter in an isolated Python process without Git or
access to the original repository. It changes only the **new public kit
manifest** to mark the reconstruction identity and assign a numeric version
distinct from each historical kit. The lifecycle overlay code is copied
byte-for-byte into the relevant public Studio kit. A failed export leaves no
partial output at the requested path.

Use Python 3.11 or newer with `jsonschema>=4.18,<5` installed. Choose a new
output directory for each mode:

```sh
python tools/export_reference_toolchain.py export --mode public-g1 --output build/public-g1
python tools/export_reference_toolchain.py export --mode public-studio-1.9.0 --output build/public-studio-190
python tools/export_reference_toolchain.py export --mode public-studio-1.9.1 --output build/public-studio-191
python tools/export_reference_toolchain.py verify --mode public-studio-1.9.1 --kit build/public-studio-191
```

The **new public** manifest identities are:

| Mode | Version | Files including requirements | Manifest SHA-256 |
| --- | --- | ---: | --- |
| `public-g1` | 1.8.0.1 | 73 | `63bc6a745ef24f78b06762689b29dea921457d5a270c7c655ffb1b8a8aad94c7` |
| `public-studio-1.9.0` | 1.9.0.1 | 73 | `3f4db5a9a671db35634c1930c84772e8a5f6b3295d2fdb5363ba6c32c1ab7e57` |
| `public-studio-1.9.1` | 1.9.1.1 | 73 | `827237cb78a5546c42aa853ff13a7a6847fb8b86a8ba7bfdc1979f466acfc03d` |

The **historical** G1 1.8.0, P4 Studio 1.9.0 and P5 candidate Studio 1.9.1
manifest hashes remain `86d2cefebd9e01d69e3eb7bd15c17b70a7a501decdcb6f0fbef109515ed46346`,
`39d4db52184b239f75c90115f12e5e3743c5c818a89df966ff66f3cf6236ba69`
and `2d53d979a96ec8fce3413fb27cee3a94a2f22fdaa6a0891fe6257b6f6e558130`.
Those identities are historical locks, **not outputs** of the sanitized public
source. No digest was changed or relabeled to make a public build pass.

The sanitized profiles keep their pipeline, contract, safety and authority
configuration but carry `PUBLIC_RECONSTRUCTION_AWAITING_VALIDATION` status.
The historical C5 blind-gate finalizer requires a worker package identity and
measured receipts from the removed validation fields. It cannot close that
historical gate from the public profile. The replacement strategy is to build
the worker and its dependencies from public source, bind a new package identity
and fresh evidence to a **new** public profile/kit version, and run the PC and
Xbox gates separately. The exact C5 lifecycle implementation and Studio/worker
contracts remain in place; the new `src/xcp` API is not substituted for them.

The original G1 snapshot has stable JSON Schema IDs under
`xcompute-probe.local` and `xcp.local`. These are schema namespaces, not
network endpoints. Public hygiene allows only those exact namespace prefixes
inside the extracted schema files. It still rejects other internal URLs,
credential literals and unregistered reference files.

The public wrapper and source locks have PC tests, including paths with spaces,
tamper rejection and export/verify for all three modes. A source-only XCP
archive without `.git`, installed in a fresh virtual environment, passed nine
targeted tests and reproduced and verified all three sanitized public kit
digests with Git absent from the process. No original repository access was
needed.

The original P4 `xcp_creative_project.py` test and the three first-party
`hello-shapes` JSON files are preserved exactly outside the 73-file kit. Its
23 PC tests pass from `reference/xcompute-probe/` and run in portable CI:

```sh
python -m pytest -q tests/unit/test_xcp_creative_project_v1.py
```

They exercise project creation, validation, deterministic bundle construction
and negative cases using a synthetic sample. They do not establish live worker
or Xbox behavior.
No new Xbox validation is claimed. P4/r10 remains the last measured preview;
P5/r12 remains an unclosed candidate.

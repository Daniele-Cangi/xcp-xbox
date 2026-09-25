# Copilot review triage for PR #3

Reviewed on 24 September 2026. The nine inline findings were classified
against the current sanitized head. This record does not change the frozen
reference implementation or count a review suggestion as parity evidence.

| Comment ID | Area | Classification | Action |
| --- | --- | --- | --- |
| 4095212385 | Native creation profile includes operational evidence | Resolved in current head | The profile is sanitized under `PUBLIC_PROFILE_SANITIZATION`; the original historical hash remains a reference only. |
| 4095212450 | Project evolution profile includes run evidence | Resolved in current head | Same sanitation; two further profiles with comparable evidence were found and sanitized. |
| 4095212517 | Bundle schema `allOf` and `additionalProperties` | Not reproduced | The second branch declares `path`, `bytes`, and `sha256` itself. Draft 2020-12 validation accepted representative module and asset entries. No schema change. |
| 4095212577 | Adapter registry resolves a source root before symlink check | Plausible PC-side defect | Retain exact source. Reproduce with a synthetic symlink and compare original/extracted results before considering a separate correction. |
| 4095212646 | C5 artifact path resolves before symlink check | Plausible PC-side defect | Retain exact source pending a contained-path and symlink parity test. |
| 4095212721 | Engine-assisted exact source references not rebound to source bytes | Plausible contract gap | Retain exact source; test stale/fabricated references with a synthetic engine response and check the hardware-facing contract before any fix. |
| 4095212790 | Godot reference accepts Windows escaping syntax | Plausible cross-platform defect | Retain exact source pending Windows negative vectors. |
| 4095212853 | Godot model root symlink check follows resolution | Plausible PC-side defect | Retain exact source pending a synthetic symlink vector. |
| 4095212912 | Source inventory root symlink check follows resolution | Plausible PC-side defect | Retain exact source pending a synthetic symlink vector. |

The six path/source-identity findings may warrant corrections after PC-only
reproduction and a contract impact check. They were not applied to the
behavior-frozen Studio → lifecycle → transport → worker reference path in this
tranche. PR #4 had no inline Copilot findings at this review point.

At the development-distribution checkpoint on 24 September 2026, PRs #4–#11
had no additional Copilot inline comments. The six remaining PR #3 findings
are still classified as plausible PC-side issues or a contract gap, not as
permission to change behavior-frozen source. No source change in this tranche
was made solely to satisfy a Copilot suggestion.

## PR #12 review, 24 September 2026

Copilot reported three manifest path-containment findings (comments
4098323111, 4098323171 and 4098323206). All three are valid: signed package
filenames, embedded runtime/toolchain file paths, and extracted package
filenames were joined to a base directory without rejecting traversal or
absolute paths. The publication-candidate branch validates relative POSIX
paths, rejects symlink escapes, and requires package filenames to be one
component. Synthetic negative tests cover traversal and absolute paths.
These are PC-side distribution checks and do not modify the frozen P4 source.
PR #12 remains open and unchanged; the fixes are cumulative on this branch.

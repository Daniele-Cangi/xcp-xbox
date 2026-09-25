# Contributing

Install with `python -m pip install -e '.[test]'`, then run `python -m pytest -q`,
`xcp conformance all` and `python tools/public_release_check.py` before a pull
request. A wire-contract change needs a versioned schema or compatibility
explanation and positive and negative conformance tests. Add adapters under
`src/xcp/adapters` and targets under `src/xcp/targets`. The core must not
import them.

Use synthetic fixtures or material with clear redistribution rights. Record
every extraction in the provenance manifest. Keep secrets, device data and
private experiment artifacts out of Git. A build or target run alone does not
prove source fidelity.

## Agent integrations

Agent integrations are welcome, but they must use the same project, admission,
execution and evidence contracts as human-driven workflows. Do not introduce a
provider-specific privileged execution path.

Good contribution areas include coding-agent controllers, CI agents and an MCP
adapter over the existing XCP lifecycle. An MCP adapter must not be presented as
an existing XCP feature until its contract and tests are merged.

Agent-driven changes remain ordinary contributions: generated code must pass
the same tests and review, and claims require the same evidence regardless of
whether a human or an agent authored the implementation.

## Hardware reproduction

Independent Xbox reproduction is especially useful. Do not commit console
addresses, device IDs, pairing/session material, credentials, private
certificate keys, raw private logs or absolute local paths. Report exact source
and package identities and distinguish PC-only results from physical-Xbox
observations.

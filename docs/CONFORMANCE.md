# Local conformance

`xcp conformance all` runs four suites from an installed package:

| Suite | Checks |
| --- | --- |
| `xvm` | Five XVM v2 vectors, exact output/fuel and pre-execution fuel refusal. |
| `adapter` | Godot synthetic fixture, checkout-independent IDs and refusal of unmodeled scripts. |
| `target` | Exact intent/plan-bound lowering and bundle verification. |
| `evidence` | Intended invariant coverage, receipt-bound comparison, exact replay and stage routing. |

`python -m pytest -q` adds verifier, typed-memory, structured-control,
snapshot tampering and source identity tests. No Xbox, Godot executable or
private repository is needed. An external adapter must provide stable IDs,
exact source-byte binding, unsupported-semantics reporting and a passing
`verify_source_chain`. A target must refuse unsupported capabilities and
stale plans before lowering. A verifier needs exact identities and explicit
invariant tolerances; missing coverage yields `insufficient`. Conformance is
for the tested subset, not blanket fidelity.

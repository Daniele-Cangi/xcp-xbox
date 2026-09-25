# Architecture

XCP separates source meaning from target mechanics. A source adapter observes
exact bytes and emits a Source Model and semantic IR. The generic planner
admits that IR and an authored project intent only against an exact target
capability profile. A target
lowers the admitted plan and may execute it. Independent observations feed
the evidence verifier. Every report binds source, model, IR, project, plan,
profile, execution and result identities as applicable.

```text
SourceAdapter -> SourceObservation -> SourceModel -> SemanticIR
ProjectIntent ------------------------------------> AdmissionPlan
TargetProfile -----------------------------------> AdmissionPlan
                                                       |
                                               TargetAdapter.lower
                                                       |
                                            execute and observe
                                                       |
                                    Evidence.compare -> bounded claim
```

`xcp.contracts` and `xcp.evidence` import no adapter or target. The Godot
reference adapter reads a Godot 4 descriptor and text scenes and emits stable
scene/node IDs with exact source hashes. GDScript and unrecognized node syntax
are unsupported capabilities, so the archive target refuses them. This is a
structural observer, not a behavioral parser.

`xcp.targets.archive` is a local target plugin. It writes an ordinary XCP
project with a structural data module and uses the project builder to produce
a deterministic bundle. This proves structural packaging, not playability.

`xcp.xvm` contains the independently usable XVM v2 ISA, verifier, assembler
and deterministic CPU reference executor. The verifier proves resource and
fuel bounds before execution. Structured control, typed memory and snapshot
resume follow the source oracle. The public snapshot seal is only a test
mechanism; the real worker owns its own authority key.

Versioned project/bundle/host-profile schemas remain target wire contracts.
`src/xcp/spec/xcp-platform-contracts-v1.schema.json` defines the portable
source, intent, IR, admission, receipt, observation, comparison, replay and
stage-evidence document shapes. A receipt is target-authored; public core
checks its binding and outcome, while the target remains responsible for
authenticating it.
Xbox worker transport and Studio are outside this package pending isolated
public contracts and provenance review.

# Agent-native XCP

XCP treats coding agents as first-class external controllers of the same
project and execution lifecycle used by human developers.

This is not a claim that an AI model runs on Xbox. The model or agent remains
on a PC or external service. Xbox receives only work admitted through XCP's
bounded contracts.

## Control loop

```text
human or coding agent
        |
        | intent / project operations
        v
XCP Studio or agent toolchain
        |
        | versioned project + lifecycle contracts
        v
capability discovery and admission
        |
        v
XCP Worker on Xbox
        |
        | bounded CPU / GPU / XVM execution
        v
canonical result + structured evidence
        |
        v
controller inspects error.code / error.details
        |
        +---- correct and iterate
```

The important property is that the controller does not gain a second,
privileged path around XCP. Human-driven Studio workflows and agent-driven
workflows converge on the same project, admission, execution and evidence
boundaries.

## Existing reference contracts

The preserved reference implementation includes:

- `xcp-agent-native-creation-v1`: project creation and lifecycle authority;
- `xcp-agent-native-source-adaptation-v1`: source inspection, semantic
  extraction, Creative IR, adaptation planning and C5 handoff;
- `xcp-agent-project-intent-v1`: machine-readable authored intent;
- `xcp-agent-correction-ledger-v1`: append-only correction history;
- `xcp-agent-lifecycle-receipt-v1`: lifecycle receipt contract;
- `worker-sdk-contract-v1`: protocol discovery and external-controller
  contract;
- the agent-native XVM assembler/toolchain path.

The creation lifecycle is explicitly ordered through discovery, intent,
creation, validation, build, verification, install, activation, launch,
observation, interaction, acceptance evaluation, structured correction,
update, capture, rollback and cleanup.

## Structured correction instead of prose guessing

Agent decisions are expected to branch on stable machine-readable fields such
as:

```text
error.code
error.details
```

Free-form status text is not the authority for correction. Failed attempts can
remain in the correction ledger instead of being rewritten into a successful
history.

That makes the agent loop reproducible and reviewable:

```text
attempt -> structured failure -> bounded correction -> new attempt -> evidence
```

## Security boundary

The agent is not hosted on the console.

The reference worker SDK contract records the controller location as
`EXTERNAL_PC_OR_SERVICE` and `codex_runs_on_xbox: false`.

XCP projects and evidence must not contain model credentials. Agent integration
must not add:

- unrestricted shell or process creation on Xbox;
- arbitrary native-code upload;
- broad filesystem access;
- hidden unsupported features;
- hidden human intervention;
- a provider-specific bypass around capability admission.

## Current evidence boundary

Historical reference evidence includes an Xbox-measured agent-native XVM
toolchain path.

The reconstructed public development packages have different identities and
remain:

```text
NOT_TESTED_ON_XBOX
```

until a fresh device attempt produces its own evidence. Historical evidence is
not automatically promoted to a new build.

## Model-provider independence

The preserved lineage contains Codex-oriented tooling, but XCP's contracts are
not intended to require one model provider.

A controller can be:

- a coding agent;
- an IDE agent;
- a CI automation agent;
- a custom orchestration service;
- a human operating Studio;
- a future MCP client through an XCP adapter.

## MCP status

XCP does **not** currently claim a native MCP server.

A useful community project is to expose the existing typed lifecycle through an
MCP adapter, for example:

```text
xcp.project.create
xcp.project.inspect
xcp.project.adapt
xcp.project.build
xcp.project.verify
xcp.target.describe
xcp.execute
xcp.evidence.read
```

Such an adapter should call the existing XCP contracts rather than redefining
them or creating a privileged execution route.

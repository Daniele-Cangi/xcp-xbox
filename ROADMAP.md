# XCP roadmap

XCP is moving from a maintainer-built research/product system to an open
development project.

The immediate goal is not to add more private capability before publication.
It is to make the current system reproducible, understandable and extendable by
other developers.

## Current foundation

Already present:

- historical end-to-end execution on a physical Xbox Series X;
- reconstructible Windows Studio;
- reconstructible native worker, native module, topology broker and CPU
  Capsule;
- XVM v2 reference, verifier, assembler and conformance vectors;
- source adaptation and project lifecycle contracts;
- agent-native creation/adaptation/lifecycle contracts;
- source-only provenance and clean-build verification;
- fresh-extraction PC workflow verification;
- development packages isolated from historical package identities.

Current reconstructed packages remain `NOT_TESTED_ON_XBOX`.

## First community milestones

### 1. Independent Windows reproduction

Rebuild the complete development distribution from a clean clone on a machine
that is not the maintainer's development environment.

Evidence should include the source commit/tree, tool versions, build result and
distribution verification result.

### 2. First community Xbox reproduction

Build the development worker and CPU Capsule, install them on an owned Xbox
Series X in Developer Mode, run one bounded Studio-to-Xbox path and submit the
result for review.

This is the first major public hardware milestone.

### 3. Agent-native public reproduction

Exercise the agent lifecycle from the public build, including at least one
structured failure/correction cycle and evidence retrieval.

### 4. Developer experience

Useful contributions include:

- environment preflight;
- simpler build/start commands;
- clearer Xbox installation diagnostics;
- Studio UX improvements;
- evidence inspection tools.

### 5. Agent integrations

Connect additional coding-agent systems to XCP without changing the core
execution authority.

A native MCP adapter is an explicitly open contribution target. It must map to
existing typed XCP operations rather than expose a shell or bypass admission.

### 6. Runtime and platform extensions

Longer-term work includes:

- broader XVM conformance and independent implementations;
- independent GPU-path reproduction;
- deeper source-adapter coverage;
- additional source ecosystems;
- additional execution targets.

## Evidence states

Public discussions should distinguish:

- **MAINTAINER_MEASURED** — directly measured and preserved by the maintainer;
- **COMMUNITY_REPRODUCED** — independently reproduced with reviewable evidence;
- **UNVERIFIED** — proposed, implemented or reported without sufficient
  reproduction evidence.

Passing CI, successful compilation or a launched process does not by itself
promote a hardware or fidelity claim.

## Maintainer role

The maintainer primarily governs:

- architecture boundaries;
- pull-request review;
- evidence quality;
- claim promotion;
- compatibility and schema evolution;
- roadmap direction.

Contributors are encouraged to own substantial implementation and reproduction
work.

## Non-goals for the publication gate

The first public source release does not require:

- a prebuilt binary release;
- a one-click installer;
- new maintainer Xbox testing;
- universal Godot compatibility;
- unrestricted execution;
- a built-in MCP server.

Those are development opportunities, not prerequisites for opening the source.

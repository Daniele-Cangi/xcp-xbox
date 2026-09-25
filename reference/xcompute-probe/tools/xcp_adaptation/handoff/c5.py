"""C6-to-C5 intent, artifact handoff, and ledger binding."""

from __future__ import annotations

import pathlib
from typing import Any

from xcp_creative_project import sha256_file

from ..core import (
    INTENT_SCHEMA,
    LEDGER_SCHEMA,
    SourceAdaptError,
    _document_sha,
    _load_json,
    _validate,
    _validate_schema,
)
from ..lowering import _world2d_solution


def _adapted_c5_intent(
    project: dict[str, Any],
    project_dir: pathlib.Path,
    inventory: dict[str, Any],
) -> dict[str, Any]:
    action = ""
    campaign_action = ""
    campaign_entry_scene = ""
    campaign_next_scene = ""
    world: dict[str, Any] | None = None
    campaign: dict[str, Any] | None = None
    modules_by_id = {
        str(module["id"]): module for module in project["modules"]
    }
    entry = next(
        (
            module
            for module in project["modules"]
            if module["id"] == project["entry_module"]
        ),
        None,
    )
    if entry and entry["kind"] in {
        "xcp.world2d.v1",
        "xcp.world2d.v2",
    }:
        world = _load_json(project_dir / str(entry["path"]))
    elif entry and entry["kind"] == "xcp.world2d.campaign.v1":
        campaign = _load_json(project_dir / str(entry["path"]))
        campaign_entry_scene = str(campaign["entry_scene"])
        scene = next(
            (
                item
                for item in campaign["scenes"]
                if str(item["id"]) == campaign_entry_scene
            ),
            None,
        )
        if scene is not None:
            world_entry = modules_by_id.get(str(scene["module_id"]))
            if world_entry and world_entry["kind"] in {
                "xcp.world2d.v1",
                "xcp.world2d.v2",
            }:
                world = _load_json(
                    project_dir / str(world_entry["path"])
                )
        if campaign.get("input_bindings"):
            campaign_action = str(
                campaign["input_bindings"][0]["action"]
            )
        transition = next(
            (
                item
                for item in campaign["transitions"]
                if str(item["from_scene"]) == campaign_entry_scene
                and str(item["event"]) == "world.level-completed"
            ),
            None,
        )
        if transition is not None:
            campaign_next_scene = str(transition["to_scene"])

    if world is not None:
        bindings = world.get("input_bindings", [])
        if bindings:
            action = str(bindings[0]["action"])
        solution = _world2d_solution(world)
    else:
        solution = []
    acceptance: list[dict[str, Any]] = [
        {
            "id": "project-identity",
            "phase": "local",
            "severity": "blocker",
            "observation_path": "project.project_id",
            "operator": "equal",
            "expected": project["project_id"],
            "correction_hint": "regenerate the project from the bound C6 handoff",
        },
        {
            "id": "foreground-active",
            "phase": "live",
            "severity": "blocker",
            "observation_path": "active",
            "operator": "equal",
            "expected": True,
            "correction_hint": "launch the exact active C5 install",
        },
        {
            "id": "foreground-project",
            "phase": "live",
            "severity": "blocker",
            "observation_path": "project_id",
            "operator": "equal",
            "expected": project["project_id"],
            "correction_hint": "restore the handoff-bound project identity",
        },
    ]
    interactions: list[dict[str, Any]] = []
    if world is not None:
        acceptance.append(
            {
                "id": "world-observed",
                "phase": "live",
                "severity": "blocker",
                "observation_path": "world2d",
                "operator": "exists",
                "expected": True,
                "correction_hint": (
                    "launch through the declared world2d entry module"
                ),
            }
        )
    if (
        solution
        and campaign is not None
        and campaign_action
        and campaign_entry_scene
        and campaign_next_scene
    ):
        acceptance.extend(
            [
                {
                    "id": "campaign-entry-completed",
                    "phase": "live",
                    "severity": "blocker",
                    "observation_path": (
                        "world2d.campaign.completed_scenes"
                    ),
                    "operator": "contains",
                    "expected": campaign_entry_scene,
                    "correction_hint": (
                        "complete the campaign entry world before "
                        "requesting a scene transition"
                    ),
                },
                {
                    "id": "campaign-advanced",
                    "phase": "live",
                    "severity": "blocker",
                    "observation_path": "world2d.campaign.active_scene",
                    "operator": "equal",
                    "expected": campaign_next_scene,
                    "correction_hint": (
                        "dispatch the published campaign advance action "
                        "after entry-world completion"
                    ),
                },
            ]
        )
        interactions.append(
            {
                "id": "adapted-project-complete-and-advance",
                "steps": [
                    *solution,
                    {
                        "action": campaign_action,
                        "repeat": 1,
                        "wait_ms": 40,
                    },
                ],
                "assertion_ids": [
                    "foreground-active",
                    "foreground-project",
                    "world-observed",
                    "campaign-entry-completed",
                    "campaign-advanced",
                ],
            }
        )
    elif solution:
        acceptance.append(
            {
                "id": "world-completed",
                "phase": "live",
                "severity": "blocker",
                "observation_path": "world2d.completed",
                "operator": "equal",
                "expected": True,
                "correction_hint": (
                    "derive and execute a complete bounded solution for "
                    "the adapted world objective"
                ),
            }
        )
        interactions.append(
            {
                "id": "adapted-project-complete-objective",
                "steps": solution,
                "assertion_ids": [
                    "foreground-active",
                    "foreground-project",
                    "world-observed",
                    "world-completed",
                ],
            }
        )
    elif action:
        acceptance.append(
            {
                "id": "world-interacted",
                "phase": "live",
                "severity": "blocker",
                "observation_path": "world2d.turn",
                "operator": "greater_equal",
                "expected": 1,
                "correction_hint": "dispatch a declared semantic input action",
            }
        )
        interactions.append(
            {
                "id": "adapted-project-smoke",
                "steps": [
                    {
                        "action": action,
                        "repeat": 1,
                        "wait_ms": 0,
                    }
                ],
                "assertion_ids": [
                    "foreground-active",
                    "foreground-project",
                    "world-observed",
                    "world-interacted",
                ],
            }
        )
    intent = {
        "schema_version": "xcp-agent-project-intent-v1",
        "intent_id": f"{project['project_id']}.adapted-intent",
        "project_id": project["project_id"],
        "summary": (
            f"Exercise the C5 lifecycle and bounded semantic interaction for "
            f"the C6 adaptation of {inventory['source']['project_name']}."
        ),
        "experience_kind": "game",
        "non_goals": [
            "execute original engine binaries on Xbox",
            "bypass the published Creative Host contract",
        ],
        "acceptance": acceptance,
        "interaction_sequences": interactions,
        "source_provenance": {
            "kind": "external_source_adaptation",
            "reference": str(inventory["source"]["origin_locator"]),
        },
        "completion_policy": {
            "all_blockers_must_pass": True,
            "minimum_live_captures": 3,
            "structured_observation_required": True,
            "rollback_required": True,
            "cleanup_required": True,
        },
    }
    _validate_schema(intent, INTENT_SCHEMA, stage="c5_intent")
    return intent


def _handoff_artifact(
    output: pathlib.Path,
    role: str,
    relative: str,
) -> dict[str, Any]:
    return {
        "role": role,
        "path": relative,
        "sha256": sha256_file(output / pathlib.PurePosixPath(relative)),
    }


def _adaptation_handoff(
    output: pathlib.Path,
    project: dict[str, Any],
) -> dict[str, Any]:
    readiness = _load_json(output / "readiness-report.json")
    _validate(readiness, "readiness")
    artifacts = [
        _handoff_artifact(output, "source.inventory", "source-inventory.json"),
        _handoff_artifact(
            output,
            "source.semantic_inventory",
            "semantic-inventory.json",
        ),
        _handoff_artifact(output, "creative.ir", "creative-ir.json"),
        _handoff_artifact(output, "adaptation.plan", "adaptation-plan.json"),
        _handoff_artifact(
            output,
            "adaptation.fidelity_contract",
            "fidelity-contract.json",
        ),
        _handoff_artifact(
            output,
            "xcp.project",
            "xcp-project/xcp-project.json",
        ),
        _handoff_artifact(output, "source.map", "source-map.json"),
        _handoff_artifact(
            output,
            "adaptation.readiness.preliminary",
            "readiness-report.json",
        ),
        _handoff_artifact(output, "c5.intent", "c5-intent.json"),
    ]
    handoff = {
        "schema_version": "xcp-creative-adaptation-handoff-v2",
        "handoff_id": f"{project['project_id']}.c6-to-c5",
        "project_id": project["project_id"],
        "source_gate": "C6_AGENT_NATIVE_SOURCE_ADAPTATION_V1",
        "lifecycle_gate": "C5_AGENT_NATIVE_CREATION_GATE",
        "artifacts": artifacts,
        "lifecycle": {
            "tool": "tools/xcp_agent_lifecycle.py",
            "command": "run-live",
            "project_dir": "xcp-project",
            "intent": "c5-intent.json",
            "correction_ledger": "c5-correction-ledger.json",
            "fidelity_evidence": "fidelity-evidence.json",
            "finalize_command": "tools/xcp_source_adapt.py finalize",
        },
        "status": (
            "ready_for_c5"
            if readiness["decision"] == "ready_source_faithful"
            else "diagnostic_only"
        ),
    }
    _validate(handoff, "handoff")
    return handoff


def _verify_handoff_artifacts(
    run_root: pathlib.Path,
    handoff: dict[str, Any],
) -> dict[str, pathlib.Path]:
    roles: dict[str, pathlib.Path] = {}
    for artifact in handoff["artifacts"]:
        role = str(artifact["role"])
        if role in roles:
            raise SourceAdaptError(
                "xcp.adapt.handoff_role_duplicate",
                "The C6 handoff contains a duplicate artifact role.",
                stage="finalize",
                field="artifacts.role",
                expected="one artifact per role",
                actual=role,
                correction="restore the exact generated handoff",
            )
        path = (
            run_root / pathlib.PurePosixPath(str(artifact["path"]))
        ).resolve()
        try:
            path.relative_to(run_root)
        except ValueError as exc:
            raise SourceAdaptError(
                "xcp.adapt.handoff_path_escape",
                "A C6 handoff artifact escapes its run root.",
                stage="finalize",
                field="artifacts.path",
                expected=str(run_root),
                actual=str(path),
                correction="reject the modified handoff",
            ) from exc
        actual = sha256_file(path)
        if actual != artifact["sha256"]:
            raise SourceAdaptError(
                "xcp.adapt.handoff_hash_mismatch",
                "A C6 handoff artifact no longer matches its bound bytes.",
                stage="finalize",
                field=role,
                expected=str(artifact["sha256"]),
                actual=actual,
                correction="restore the exact artifact or run a fresh adaptation",
            )
        roles[role] = path
    return roles


def _validate_correction_ledger(ledger: dict[str, Any]) -> None:
    _validate_schema(ledger, LEDGER_SCHEMA, stage="correction_ledger")
    previous = ""
    for ordinal, entry in enumerate(ledger["entries"], start=1):
        if entry["ordinal"] != ordinal:
            raise SourceAdaptError(
                "xcp.adapt.ledger_ordinal_invalid",
                "The correction ledger ordinal sequence is not contiguous.",
                stage="finalize",
                field="entries.ordinal",
                expected=str(ordinal),
                actual=str(entry["ordinal"]),
                correction="restore the exact C5 correction ledger",
            )
        if entry["previous_entry_sha256"] != previous:
            raise SourceAdaptError(
                "xcp.adapt.ledger_chain_invalid",
                "The correction ledger hash chain is broken.",
                stage="finalize",
                field="previous_entry_sha256",
                expected=previous,
                actual=str(entry["previous_entry_sha256"]),
                correction="restore the exact C5 correction ledger",
            )
        unsigned = dict(entry)
        reported = str(unsigned.pop("entry_sha256"))
        actual = _document_sha(unsigned)
        if actual != reported:
            raise SourceAdaptError(
                "xcp.adapt.ledger_entry_hash_invalid",
                "A correction entry hash does not match its canonical bytes.",
                stage="finalize",
                field="entry_sha256",
                expected=actual,
                actual=reported,
                correction="restore the exact C5 correction ledger",
            )
        previous = reported
    if ledger["head_sha256"] != previous:
        raise SourceAdaptError(
            "xcp.adapt.ledger_head_invalid",
            "The correction ledger head does not bind its final entry.",
            stage="finalize",
            field="head_sha256",
            expected=previous,
            actual=str(ledger["head_sha256"]),
            correction="restore the exact C5 correction ledger",
        )

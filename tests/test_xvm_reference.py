import copy
import hashlib
import json
import unittest
from pathlib import Path

from xcp.xvm.reference import (
    XvmValidationError,
    execute_cancel_resume,
    execute_checkpoint_resume,
    execute_program,
    load_json,
    restore_state_snapshot,
    verify_program,
)


ROOT = Path(__file__).resolve().parents[1]
SPEC_PATH = ROOT / "src" / "xcp" / "spec" / "xvm-isa-v2.json"
PROGRAM_PATH = ROOT / "src" / "xcp" / "conformance" / "vectors" / "xvm-v2-call-chain.json"
TYPED_PROGRAM_PATH = ROOT / "src" / "xcp" / "conformance" / "vectors" / "xvm-v2-typed-memory-v1.json"
STRUCTURED_PROGRAM_PATH = ROOT / "src" / "xcp" / "conformance" / "vectors" / "xvm-v2-structured-control-v2.json"
PHASE_B_PROGRAM_PATH = ROOT / "src" / "xcp" / "conformance" / "vectors" / "xvm-v2-structured-control-phase-b.json"


class XvmV2ReferenceTests(unittest.TestCase):
    def setUp(self) -> None:
        self.spec = load_json(SPEC_PATH)
        self.program = load_json(PROGRAM_PATH)
        self.typed_program = load_json(TYPED_PROGRAM_PATH)
        self.structured_program = load_json(STRUCTURED_PROGRAM_PATH)
        self.phase_b_program = load_json(PHASE_B_PROGRAM_PATH)
        self.inputs = [1, 2, 0, 0, 0, 0, 0, 0, 0, 0, 1]

    def test_call_chain_executes_deterministically(self) -> None:
        first = execute_program(self.program, self.spec, self.inputs)
        second = execute_program(self.program, self.spec, self.inputs)

        self.assertEqual(first, second)
        self.assertEqual(first["output_hex"], "20010000000000000000000000000000")
        self.assertEqual(first["control_token"], "pass")
        self.assertEqual(first["fuel_consumed"], 13)

    def test_static_worst_case_fuel_is_exact_for_calls_branches_and_nested_loops(self) -> None:
        call_proof = verify_program(self.program, self.spec)["static_fuel_proof"]
        structured_proof = verify_program(self.structured_program, self.spec)["static_fuel_proof"]
        cancel_program = load_json(ROOT / "src" / "xcp" / "conformance" / "vectors" / "xvm-v2-cancel-resume-loop.json")
        cancel_proof = verify_program(cancel_program, self.spec)["static_fuel_proof"]

        self.assertEqual(call_proof["worst_case_fuel"], 13)
        self.assertEqual(structured_proof["worst_case_fuel"], 11)
        self.assertEqual(cancel_proof["worst_case_fuel"], 7864449)
        for proof in (call_proof, structured_proof, cancel_proof):
            self.assertTrue(proof["exact"])
            self.assertTrue(proof["bounded_loops_accounted"])
            self.assertTrue(proof["acyclic_calls_accounted"])
            self.assertTrue(proof["branch_maxima_accounted"])


    def test_unknown_call_target_is_rejected_before_execution(self) -> None:
        program = copy.deepcopy(self.program)
        program["bytecode_words"][9] = 9
        with self.assertRaisesRegex(XvmValidationError, "declared function entry") as raised:
            verify_program(program, self.spec)
        self.assertEqual(raised.exception.code, "xvm.call_target_invalid")

    def test_recursive_call_graph_is_rejected(self) -> None:
        program = copy.deepcopy(self.program)
        offset = 9 * 4
        program["bytecode_words"][offset : offset + 4] = [15, 8, 0, 0]
        with self.assertRaises(XvmValidationError) as raised:
            verify_program(program, self.spec)
        self.assertEqual(raised.exception.code, "xvm.call_cycle_invalid")

    def test_declared_call_depth_must_cover_verified_graph(self) -> None:
        program = copy.deepcopy(self.program)
        offset = 9 * 4
        program["bytecode_words"][offset : offset + 4] = [15, 11, 0, 0]
        with self.assertRaises(XvmValidationError) as raised:
            verify_program(program, self.spec)
        self.assertEqual(raised.exception.code, "xvm.call_depth_exceeded")

    def test_insufficient_program_fuel_fails_during_verification(self) -> None:
        program = copy.deepcopy(self.program)
        program["max_fuel"] = 12
        with self.assertRaises(XvmValidationError) as raised:
            execute_program(program, self.spec, self.inputs)
        self.assertEqual(raised.exception.code, "xvm.static_fuel_program_limit_insufficient")

    def test_checkpoint_snapshot_restores_active_call_and_matches_uninterrupted_run(self) -> None:
        program_sha256 = hashlib.sha256(PROGRAM_PATH.read_bytes()).hexdigest()
        replay = execute_checkpoint_resume(
            self.program,
            self.spec,
            self.inputs,
            checkpoint_fuel=4,
            program_sha256=program_sha256,
            bound_input_sha256="ab" * 32,
        )
        snapshot = json.loads(replay["snapshot_json"])

        self.assertEqual(replay["result"], execute_program(self.program, self.spec, self.inputs))
        self.assertEqual(replay["snapshot_sha256"], hashlib.sha256(replay["snapshot_json"].encode("utf-8")).hexdigest())
        self.assertEqual(snapshot["schema_version"], "xvm-state-snapshot-v2")
        self.assertEqual(snapshot["execution_id"], "reference-checkpoint")
        self.assertEqual(snapshot["checkpoint_sequence"], "1")
        self.assertEqual(snapshot["producer"], "xcp-reference")
        self.assertEqual(snapshot["seal_schema_version"], "xvm-state-snapshot-hmac-sha256-v1")
        self.assertEqual(len(snapshot["previous_state_sha256"]), 64)
        self.assertEqual(len(snapshot["seal_key_id"]), 64)
        self.assertEqual(len(snapshot["worker_seal_sha256"]), 64)
        self.assertEqual(snapshot["fuel_consumed"], "4")
        self.assertFalse(snapshot["halted"])
        self.assertEqual(snapshot["call_stack"], [{"return_pc": 3, "loop_depth": 0}])
        self.assertEqual(len(snapshot["registers"]), 16)
        self.assertEqual(len(snapshot["memory_hex"]), self.program["memory_bytes"] * 2)
        self.assertEqual(len(snapshot["output_hex"]), self.program["output_bytes"] * 2)
        self.assertIn("loop_stack", snapshot)

    def test_snapshot_digest_is_stable_and_context_tampering_fails_closed(self) -> None:
        program_sha256 = hashlib.sha256(PROGRAM_PATH.read_bytes()).hexdigest()
        arguments = (self.program, self.spec, self.inputs, 4, program_sha256, "cd" * 32)
        first = execute_checkpoint_resume(*arguments)
        second = execute_checkpoint_resume(*arguments)
        self.assertEqual(first["snapshot_json"], second["snapshot_json"])
        self.assertEqual(first["snapshot_sha256"], second["snapshot_sha256"])

        tampered = json.loads(first["snapshot_json"])
        tampered["program_sha256"] = "00" * 32
        with self.assertRaises(XvmValidationError) as raised:
            restore_state_snapshot(
                json.dumps(tampered, separators=(",", ":")),
                self.program,
                self.spec,
                program_sha256,
                "cd" * 32,
            )
        self.assertEqual(raised.exception.code, "xvm.snapshot_context_mismatch")

    def test_snapshot_stack_tampering_fails_closed(self) -> None:
        program_sha256 = hashlib.sha256(PROGRAM_PATH.read_bytes()).hexdigest()
        replay = execute_checkpoint_resume(self.program, self.spec, self.inputs, 4, program_sha256, "ef" * 32)
        tampered = json.loads(replay["snapshot_json"])
        tampered["call_stack"][0]["loop_depth"] = 1
        with self.assertRaises(XvmValidationError) as raised:
            restore_state_snapshot(
                json.dumps(tampered, separators=(",", ":")),
                self.program,
                self.spec,
                program_sha256,
                "ef" * 32,
            )
        self.assertEqual(raised.exception.code, "xvm.snapshot_call_frame_invalid")

        tampered = json.loads(replay["snapshot_json"])
        tampered["call_stack"][0]["return_pc"] = 1
        with self.assertRaises(XvmValidationError) as raised:
            restore_state_snapshot(
                json.dumps(tampered, separators=(",", ":")),
                self.program,
                self.spec,
                program_sha256,
                "ef" * 32,
            )
        self.assertEqual(raised.exception.code, "xvm.snapshot_call_frame_invalid")

    def test_snapshot_worker_seal_and_resume_authorization_fail_closed(self) -> None:
        program_sha256 = hashlib.sha256(PROGRAM_PATH.read_bytes()).hexdigest()
        replay = execute_checkpoint_resume(self.program, self.spec, self.inputs, 4, program_sha256, "aa" * 32)
        snapshot = json.loads(replay["snapshot_json"])

        tampered = copy.deepcopy(snapshot)
        tampered["registers"][15] = 1
        with self.assertRaises(XvmValidationError) as raised:
            restore_state_snapshot(
                json.dumps(tampered, separators=(",", ":")),
                self.program,
                self.spec,
                program_sha256,
                "aa" * 32,
            )
        self.assertEqual(raised.exception.code, "xvm.snapshot_seal_invalid")

        with self.assertRaises(XvmValidationError) as raised:
            restore_state_snapshot(
                replay["snapshot_json"],
                self.program,
                self.spec,
                program_sha256,
                "aa" * 32,
                expected_execution_id="different-execution",
                expected_checkpoint_sequence=1,
                expected_previous_state_sha256=snapshot["previous_state_sha256"],
            )
        self.assertEqual(raised.exception.code, "xvm.resume_snapshot_unauthorized")
    def test_canceled_state_resumes_in_later_job_without_reexecuting_prefix(self) -> None:
        program_sha256 = hashlib.sha256(PROGRAM_PATH.read_bytes()).hexdigest()
        resumed = execute_cancel_resume(
            self.program, self.spec, self.inputs, 7, program_sha256, "12" * 32
        )
        self.assertEqual(resumed["result"], execute_program(self.program, self.spec, self.inputs))
        self.assertEqual(resumed["resumed_from_fuel"], 7)
        self.assertFalse(resumed["prefix_reexecuted"])
        self.assertEqual(
            resumed["snapshot_sha256"],
            hashlib.sha256(resumed["snapshot_json"].encode("utf-8")).hexdigest(),
        )

    def test_later_job_resume_rejects_bound_input_change(self) -> None:
        program_sha256 = hashlib.sha256(PROGRAM_PATH.read_bytes()).hexdigest()
        canceled = execute_cancel_resume(
            self.program, self.spec, self.inputs, 7, program_sha256, "34" * 32
        )
        with self.assertRaises(XvmValidationError) as raised:
            restore_state_snapshot(
                canceled["snapshot_json"], self.program, self.spec, program_sha256, "56" * 32
            )
        self.assertEqual(raised.exception.code, "xvm.snapshot_context_mismatch")

    def test_typed_memory_views_execute_with_existing_u32_opcodes(self) -> None:
        result = execute_program(self.typed_program, self.spec, self.inputs)
        self.assertEqual(result["output_hex"], "00000000000000000100000000000000")
        self.assertEqual(result["control_token"], "pass")
        self.assertEqual(result["fuel_consumed"], 8)

    def test_typed_view_fields_require_capability(self) -> None:
        program = copy.deepcopy(self.typed_program)
        program["capabilities"].remove("typed_memory_v1")
        with self.assertRaises(XvmValidationError) as raised:
            verify_program(program, self.spec)
        self.assertEqual(raised.exception.code, "xvm.typed_memory_capability_required")

    def test_typed_input_access_must_be_inside_declared_view(self) -> None:
        program = copy.deepcopy(self.typed_program)
        program["bytecode_words"][2] = 1
        with self.assertRaises(XvmValidationError) as raised:
            verify_program(program, self.spec)
        self.assertEqual(raised.exception.code, "xvm.typed_view_access_invalid")

    def test_typed_memory_access_enforces_read_write_mode(self) -> None:
        program = copy.deepcopy(self.typed_program)
        program["memory_views"][0]["access"] = "read"
        with self.assertRaises(XvmValidationError) as raised:
            verify_program(program, self.spec)
        self.assertEqual(raised.exception.code, "xvm.typed_view_access_invalid")

    def test_typed_views_reject_overlap_and_duplicate_ownership(self) -> None:
        program = copy.deepcopy(self.typed_program)
        program["memory_views"].append({
            "view_id": "overlap",
            "element_type": "u32",
            "access": "read_write",
            "offset_bytes": 4,
            "length_bytes": 4,
        })
        with self.assertRaises(XvmValidationError) as raised:
            verify_program(program, self.spec)
        self.assertEqual(raised.exception.code, "xvm.typed_view_overlap")

    def test_xvm_v2_without_typed_memory_remains_compatible(self) -> None:
        verified = verify_program(self.program, self.spec)
        self.assertEqual(len(verified["instructions"]), 13)
        self.assertNotIn("typed_memory_v1", self.program["capabilities"])

    def test_structured_control_executes_both_arms_deterministically(self) -> None:
        then_result = execute_program(self.structured_program, self.spec, self.inputs)
        else_inputs = list(self.inputs)
        else_inputs[10] = 0
        else_result = execute_program(self.structured_program, self.spec, else_inputs)

        self.assertEqual(then_result["output_hex"], "08000000000000000000000000000000")
        self.assertEqual(then_result["fuel_consumed"], 11)
        self.assertEqual(else_result["output_hex"], "09000000000000000000000000000000")
        self.assertEqual(else_result["fuel_consumed"], 7)
        self.assertEqual(then_result["control_token"], "pass")
        self.assertEqual(else_result["control_token"], "pass")

    def test_structured_control_requires_capability(self) -> None:
        program = copy.deepcopy(self.structured_program)
        program["capabilities"].remove("structured_control_v2")
        with self.assertRaises(XvmValidationError) as raised:
            verify_program(program, self.spec)
        self.assertEqual(raised.exception.code, "xvm.structured_control_capability_required")

    def test_structured_control_rejects_malformed_targets_and_legacy_branches(self) -> None:
        malformed = copy.deepcopy(self.structured_program)
        malformed["bytecode_words"][6] = 5
        with self.assertRaises(XvmValidationError) as raised:
            verify_program(malformed, self.spec)
        self.assertEqual(raised.exception.code, "xvm.structured_control_target_invalid")

        legacy = copy.deepcopy(self.structured_program)
        legacy["bytecode_words"][8:12] = [10, 0, 4, 0]
        with self.assertRaises(XvmValidationError) as raised:
            verify_program(legacy, self.spec)
        self.assertEqual(raised.exception.code, "xvm.structured_control_legacy_branch_invalid")

    def test_structured_control_snapshot_needs_no_persistent_conditional_stack(self) -> None:
        program_sha256 = hashlib.sha256(STRUCTURED_PROGRAM_PATH.read_bytes()).hexdigest()
        replay = execute_checkpoint_resume(
            self.structured_program, self.spec, self.inputs, 3, program_sha256, "78" * 32
        )
        snapshot = json.loads(replay["snapshot_json"])
        self.assertEqual(replay["result"], execute_program(self.structured_program, self.spec, self.inputs))
        self.assertEqual(snapshot["pc"], 3)
        self.assertNotIn("conditional_stack", snapshot)
    def test_structured_control_phase_b_executes_with_exact_static_fuel_and_trap(self) -> None:
        verified = verify_program(self.phase_b_program, self.spec)
        first = execute_program(self.phase_b_program, self.spec, self.inputs)
        second = execute_program(self.phase_b_program, self.spec, self.inputs)

        self.assertEqual(first, second)
        self.assertEqual(verified["static_fuel_proof"]["worst_case_fuel"], 43)
        self.assertEqual(first["output_hex"], "64000000030000000100000000000000")
        self.assertEqual(first["control_token"], "pass")
        self.assertEqual(first["fuel_consumed"], 30)
        self.assertEqual(first["trap_state"], {
            "code": 42,
            "trap_pc": 21,
            "recovery_pc": 23,
            "occurrence_count": 1,
        })

    def test_structured_control_phase_b_snapshot_v3_preserves_trap_state(self) -> None:
        program_sha256 = hashlib.sha256(PHASE_B_PROGRAM_PATH.read_bytes()).hexdigest()
        replay = execute_checkpoint_resume(
            self.phase_b_program, self.spec, self.inputs, 22, program_sha256, "90" * 32
        )
        snapshot = json.loads(replay["snapshot_json"])

        self.assertEqual(replay["result"], execute_program(self.phase_b_program, self.spec, self.inputs))
        self.assertEqual(snapshot["schema_version"], "xvm-state-snapshot-v3")
        self.assertEqual(snapshot["fuel_consumed"], "22")
        self.assertEqual(snapshot["pc"], 23)
        self.assertEqual(snapshot["trap_state"], {
            "schema_version": "xvm-trap-state-v1",
            "status": "recovered",
            "code": 42,
            "trap_pc": 21,
            "recovery_pc": 23,
            "occurrence_count": "1",
        })

    def test_structured_control_phase_b_fails_closed_on_capability_and_ownership(self) -> None:
        missing_dependency = copy.deepcopy(self.phase_b_program)
        missing_dependency["capabilities"].remove("structured_control_v2")
        with self.assertRaises(XvmValidationError) as raised:
            verify_program(missing_dependency, self.spec)
        self.assertEqual(raised.exception.code, "xvm.structured_control_phase_b_dependency_required")

        missing_phase_b = copy.deepcopy(self.phase_b_program)
        missing_phase_b["capabilities"].remove("structured_control_v2_phase_b")
        with self.assertRaises(XvmValidationError) as raised:
            verify_program(missing_phase_b, self.spec)
        self.assertEqual(raised.exception.code, "xvm.structured_control_phase_b_capability_required")

        wrong_loop_owner = copy.deepcopy(self.phase_b_program)
        wrong_loop_owner["bytecode_words"][18 * 4 + 2] = 11
        with self.assertRaises(XvmValidationError) as raised:
            verify_program(wrong_loop_owner, self.spec)
        self.assertEqual(raised.exception.code, "xvm.structured_control_loop_owner_invalid")

        cross_region_recovery = copy.deepcopy(self.phase_b_program)
        cross_region_recovery["bytecode_words"][10 * 4 : 10 * 4 + 4] = [25, 9, 42, 12]
        with self.assertRaises(XvmValidationError) as raised:
            verify_program(cross_region_recovery, self.spec)
        self.assertEqual(raised.exception.code, "xvm.structured_control_recovery_invalid")

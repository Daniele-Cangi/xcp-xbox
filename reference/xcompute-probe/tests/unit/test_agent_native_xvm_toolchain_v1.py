import hashlib
import json
import pathlib
import sys
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
TOOLS = ROOT / "tools"
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))

from generate_xvm_isa_header import render_header
from xvm_assembler import (
    XvmAssemblerError,
    assemble_source,
    canonical_json_bytes,
    submission_node,
    validate_artifact,
)
from xvm_reference import execute_program


class AgentNativeXvmToolchainV1Tests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.spec_path = ROOT / "schemas" / "xvm-isa-v2.json"
        cls.spec_bytes = cls.spec_path.read_bytes()
        cls.spec = json.loads(cls.spec_bytes.decode("utf-8"))
        cls.source_path = (
            ROOT
            / "samples"
            / "xvm"
            / "agent-native-xvm-toolchain-v1.xvmasm"
        )
        cls.source = cls.source_path.read_text(encoding="utf-8")

    def test_assembler_emits_canonical_verified_artifact(self) -> None:
        first = assemble_source(self.source, self.spec)
        second = assemble_source(self.source, self.spec)
        self.assertEqual(first.artifact_bytes, second.artifact_bytes)
        self.assertEqual(first.artifact_sha256, second.artifact_sha256)
        self.assertEqual(first.static_worst_case_fuel, 8)
        self.assertEqual(first.artifact["max_fuel"], 8)
        self.assertEqual(first.instruction_count, 8)
        self.assertEqual(
            first.artifact_bytes,
            canonical_json_bytes(first.artifact),
        )
        self.assertEqual(
            first.artifact_sha256,
            hashlib.sha256(first.artifact_bytes).hexdigest(),
        )
        validated = validate_artifact(first.artifact_bytes, self.spec)
        self.assertEqual(validated.metadata(), first.metadata())

    def test_assembled_program_executes_in_portable_reference(self) -> None:
        assembled = assemble_source(self.source, self.spec)
        result = execute_program(
            assembled.artifact,
            self.spec,
            [7] + [0] * 10,
        )
        self.assertEqual(result["output_hex"][:8], "06000000")
        self.assertEqual(result["control_token"], "pass")
        self.assertEqual(result["fuel_consumed"], 8)

    def test_labels_and_schema_operand_shapes_drive_encoding(self) -> None:
        source = """
.program label-resolution-v1
.memory 16
.output 16
.max_fuel auto
.max_call_depth 1
.capability structured_control_v2
move_immediate r0, 0
if_zero r0, else_arm, end_choice
move_immediate r1, 7
else_arm:
else end_choice
move_immediate r1, 9
end_choice:
end_if
call finish
output_u32 r1, 0
set_control r1
halt
.function finish
return
.endfunction
"""
        assembled = assemble_source(source, self.spec)
        words = assembled.artifact["bytecode_words"]
        self.assertEqual(words[1 * 4 : 1 * 4 + 4], [17, 0, 3, 5])
        self.assertEqual(words[3 * 4 : 3 * 4 + 4], [18, 5, 0, 0])
        self.assertEqual(words[6 * 4 : 6 * 4 + 4], [15, 10, 0, 0])

    def test_local_errors_are_stable_and_structured(self) -> None:
        bad_source = self.source.replace(
            "xor_u32 r0, r0, r1",
            "host_call r0",
        )
        with self.assertRaises(XvmAssemblerError) as raised:
            assemble_source(bad_source, self.spec)
        error = raised.exception.as_dict()["error"]
        self.assertEqual(error["code"], "xvm.asm.opcode_unknown")
        self.assertEqual(
            error["details"]["schema_version"],
            "xvm-error-details-v1",
        )
        self.assertEqual(error["details"]["stage"], "assembler")
        self.assertEqual(error["details"]["field"], "opcode")
        self.assertEqual(error["details"]["actual"], "host_call")
        self.assertFalse(error["details"]["retryable"])

    def test_missing_program_metadata_names_the_accepted_directive(self) -> None:
        bad_source = self.source.replace(
            ".program agent-native-xvm-toolchain-v1\n",
            "",
        )
        with self.assertRaises(XvmAssemblerError) as raised:
            assemble_source(bad_source, self.spec)
        error = raised.exception.as_dict()["error"]
        self.assertEqual(error["code"], "xvm.asm.metadata_required")
        self.assertEqual(error["details"]["field"], "program_id")
        self.assertEqual(error["details"]["expected"], ".program <value>")
        self.assertEqual(
            error["details"]["correction"],
            "add the .program directive",
        )

    def test_program_id_alias_error_gives_exact_canonical_replacement(self) -> None:
        bad_source = self.source.replace(
            ".program agent-native-xvm-toolchain-v1",
            ".program_id agent-native-xvm-toolchain-v1",
        )
        with self.assertRaises(XvmAssemblerError) as raised:
            assemble_source(bad_source, self.spec)
        error = raised.exception.as_dict()["error"]
        self.assertEqual(error["code"], "xvm.asm.directive_unknown")
        self.assertIn(".program", error["details"]["expected"])
        self.assertEqual(
            error["details"]["correction"],
            "replace .program_id with .program <program_id>",
        )

    def test_submission_node_uses_exact_artifact_contract(self) -> None:
        assembled = assemble_source(self.source, self.spec)
        node = submission_node(
            assembled,
            artifact_id="agent-program-v1",
            node_id="agent-xvm",
            result_artifact_id="agent-result-v1",
        )
        self.assertEqual(node["command"], "xvm_program")
        self.assertEqual(node["isa_version"], "xvm-v2")
        self.assertEqual(node["backend"], "cpu_reference")
        self.assertEqual(
            node["expected_program_sha256"],
            assembled.artifact_sha256,
        )
        self.assertEqual(
            node["resource_limits"],
            {"fuel": 8, "memory_bytes": 64, "output_bytes": 16},
        )
        self.assertEqual(
            node["input_binding"],
            {"mode": "typed_xvm_inputs_v1"},
        )

    def test_worker_embedded_isa_is_generated_from_authoritative_schema(
        self,
    ) -> None:
        generated = (
            ROOT
            / "src"
            / "XComputeProbe"
            / "runtime"
            / "WorkerXvmIsaV2.generated.h"
        )
        self.assertEqual(
            generated.read_text(encoding="utf-8"),
            render_header(self.spec_bytes),
        )

    def test_worker_discovery_projects_the_current_schema_and_profile(
        self,
    ) -> None:
        runtime = ROOT / "src" / "XComputeProbe" / "runtime"
        command_server = (runtime / "WorkerCommandServer.cpp").read_text(
            encoding="utf-8"
        )
        graph_runtime = (runtime / "WorkerGraphRuntime.cpp").read_text(
            encoding="utf-8"
        )
        runtime_description = (
            runtime / "WorkerRuntimeDescription.cpp"
        ).read_text(encoding="utf-8")
        worker_isa = (runtime / "WorkerXvmIsa.cpp").read_text(
            encoding="utf-8"
        )

        for command in (
            "describe_submission_profile",
            "describe_xvm_isa",
        ):
            self.assertIn(f'command == L"{command}"', command_server)
            self.assertIn(f'L"{command}"', graph_runtime)
            self.assertIn(f'\\"{command}\\"', runtime_description)
        self.assertIn("WorkerXvmIsaDefinitionJson()", command_server)
        self.assertIn("WorkerXvmIsaDefinitionSha256()", command_server)
        self.assertIn(
            '"required_capabilities":'
            '["artifact_input","artifact_output","control_output"]',
            worker_isa.replace(r"\"", '"'),
        )
        self.assertIn(
            '"backend":"cpu_reference"',
            worker_isa.replace(r"\"", '"'),
        )
        for denied_boundary in (
            "native_or_host_code",
            "runtime_shader_compilation",
            "client_shader_upload",
            "shell_or_process",
            "broad_filesystem",
            "xvm_sockets",
            "credentials_on_xbox",
        ):
            self.assertIn(denied_boundary, worker_isa)

    def test_worker_xvm_errors_are_structured_and_correction_ready(
        self,
    ) -> None:
        runtime = ROOT / "src" / "XComputeProbe" / "runtime"
        types = (runtime / "WorkerXvmTypes.h").read_text(encoding="utf-8")
        protocol = (runtime / "WorkerProtocolBoundary.cpp").read_text(
            encoding="utf-8"
        )
        admission = (runtime / "WorkerXvmAdmission.cpp").read_text(
            encoding="utf-8"
        )
        worker_isa = (runtime / "WorkerXvmIsa.cpp").read_text(
            encoding="utf-8"
        )

        self.assertIn("struct WorkerXvmErrorDetails", types)
        self.assertIn("XvmCodedErrorResponse", protocol)
        for field in (
            "schema_version",
            "stage",
            "field",
            "expected",
            "actual",
            "correction",
            "retryable",
        ):
            self.assertIn(field, protocol)
        self.assertIn("xvm-error-details-v1", protocol)
        self.assertIn("xvm-error-catalog-v1", worker_isa)
        self.assertIn("xvm.capability_not_granted", worker_isa)
        self.assertIn('"missing:" +', admission)
        self.assertIn(
            "grant the missing published capability and resubmit "
            "the unchanged artifact",
            admission,
        )

    def test_live_gate_uses_exact_artifact_bytes_and_correction_loop(
        self,
    ) -> None:
        module = (TOOLS / "WorkerLiveRunner.psm1").read_text(
            encoding="utf-8"
        )
        runner = (
            TOOLS / "run-agent-native-xvm-toolchain-v1-live.ps1"
        ).read_text(encoding="utf-8")

        self.assertIn("function Send-WorkerLiveBytesArtifact", module)
        self.assertIn('"Send-WorkerLiveBytesArtifact"', module)
        self.assertIn("Send-WorkerLiveBytesArtifact", runner)
        self.assertIn("upload.exact_sha", runner)
        self.assertIn("xvm.capability_not_granted", runner)
        self.assertIn("xvm-error-details-v1", runner)
        self.assertIn("missing:control_output", runner)
        self.assertIn("grant_missing_published_capability", runner)
        self.assertIn("artifact_sha256_unchanged = $true", runner)
        self.assertIn("positive.verdict", runner)
        self.assertIn("result.committed", runner)
        self.assertIn("evidence.committed", runner)
        self.assertIn("$preStatus.gates.worker_running", runner)
        self.assertIn("$postStatus.gates.failed_requests", runner)
        self.assertNotIn("$preStatus.status.", runner)
        self.assertNotIn("$postStatus.status.", runner)


if __name__ == "__main__":
    unittest.main()

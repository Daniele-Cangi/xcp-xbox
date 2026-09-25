#!/usr/bin/env python3
"""Canonical PC-side assembler for the admitted XVM v2 artifact format."""

from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import re
import shlex
import sys
from dataclasses import dataclass
from typing import Any, Iterable

from .reference import XvmValidationError, load_json, verify_program


ROOT = pathlib.Path(__file__).resolve().parents[1]
DEFAULT_ISA_PATH = ROOT / "spec" / "xvm-isa-v2.json"
SAFE_ID = re.compile(r"^[A-Za-z0-9_-]{1,64}$")
REGISTER = re.compile(r"^r([0-9]|1[0-5])$", re.IGNORECASE)
LABEL = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")
IMPLICIT_ZERO_OPERANDS = {"zero", "reserved"}
REGISTER_OPERANDS = {
    "dst_reg",
    "src_reg",
    "lhs_reg",
    "rhs_reg",
    "condition_reg",
    "condition_and_dst_reg",
    "true_reg",
    "false_reg",
}
LABEL_OPERANDS = {
    "forward_pc",
    "loop_end_pc",
    "loop_begin_pc",
    "function_entry_pc",
    "else_or_end_if_pc",
    "end_if_pc",
    "recovery_pc",
}


@dataclass(frozen=True)
class AssemblerDetails:
    line: int | None = None
    field: str = ""
    expected: str = ""
    actual: str = ""
    correction: str = ""


class XvmAssemblerError(ValueError):
    def __init__(
        self,
        code: str,
        message: str,
        details: AssemblerDetails | None = None,
    ) -> None:
        super().__init__(message)
        self.code = code
        self.message = message
        self.details = details or AssemblerDetails()

    def as_dict(self) -> dict[str, Any]:
        return {
            "ok": False,
            "error": {
                "code": self.code,
                "message": self.message,
                "details": {
                    "schema_version": "xvm-error-details-v1",
                    "stage": "assembler",
                    "line": self.details.line,
                    "field": self.details.field,
                    "expected": self.details.expected,
                    "actual": self.details.actual,
                    "correction": self.details.correction,
                    "retryable": False,
                },
            },
        }


@dataclass
class SourceInstruction:
    line: int
    opcode_name: str
    operands: list[str]


@dataclass
class FunctionRegion:
    line: int
    name: str
    entry_pc: int
    end_pc: int | None = None


@dataclass(frozen=True)
class AssemblyResult:
    artifact: dict[str, Any]
    artifact_bytes: bytes
    artifact_sha256: str
    static_worst_case_fuel: int
    instruction_count: int

    def metadata(self) -> dict[str, Any]:
        return {
            "ok": True,
            "schema_version": "xvm-assembly-result-v1",
            "program_id": self.artifact["program_id"],
            "isa_version": self.artifact["isa_version"],
            "program_schema_version": self.artifact["schema_version"],
            "artifact_kind": "xvm-program-v2",
            "bytes": len(self.artifact_bytes),
            "sha256": self.artifact_sha256,
            "instruction_count": self.instruction_count,
            "static_worst_case_fuel": self.static_worst_case_fuel,
            "max_fuel": self.artifact["max_fuel"],
            "locally_verified": True,
        }


def canonical_json_bytes(value: Any) -> bytes:
    return (
        json.dumps(
            value,
            ensure_ascii=False,
            separators=(",", ":"),
            sort_keys=False,
        )
        + "\n"
    ).encode("utf-8")


def _fail(
    code: str,
    message: str,
    *,
    line: int | None = None,
    field: str = "",
    expected: str = "",
    actual: str = "",
    correction: str = "",
) -> None:
    raise XvmAssemblerError(
        code,
        message,
        AssemblerDetails(
            line=line,
            field=field,
            expected=expected,
            actual=actual,
            correction=correction,
        ),
    )


def _parse_u32(
    token: str,
    *,
    line: int,
    field: str,
    minimum: int = 0,
    maximum: int = 0xFFFFFFFF,
) -> int:
    try:
        value = int(token, 0)
    except ValueError:
        _fail(
            "xvm.asm.integer_invalid",
            f"{field} must be an integer",
            line=line,
            field=field,
            expected=f"{minimum}..{maximum}",
            actual=token,
            correction="replace the operand with a decimal or 0x-prefixed integer",
        )
    if not minimum <= value <= maximum:
        _fail(
            "xvm.asm.integer_out_of_range",
            f"{field} is outside the admitted range",
            line=line,
            field=field,
            expected=f"{minimum}..{maximum}",
            actual=token,
            correction="choose a value inside the published ISA limit",
        )
    return value


def _parse_register(token: str, *, line: int, field: str) -> int:
    match = REGISTER.fullmatch(token)
    if match is None:
        _fail(
            "xvm.asm.register_invalid",
            f"{field} must be an XVM register",
            line=line,
            field=field,
            expected="r0..r15",
            actual=token,
            correction="replace the operand with an admitted register",
        )
    return int(match.group(1))


def _tokenize(raw: str, line: int) -> list[str]:
    uncommented = raw.split("#", 1)[0].split(";", 1)[0].strip()
    if not uncommented:
        return []
    normalized = uncommented.replace(",", " ")
    try:
        return shlex.split(normalized, comments=False, posix=True)
    except ValueError as exc:
        _fail(
            "xvm.asm.syntax_invalid",
            f"cannot tokenize source: {exc}",
            line=line,
            field="source",
            actual=raw.strip(),
            correction="close quoted values and use comma- or space-separated operands",
        )


def _require_arity(
    tokens: list[str],
    expected: int,
    *,
    line: int,
    directive: str,
) -> None:
    if len(tokens) != expected:
        _fail(
            "xvm.asm.directive_invalid",
            f"{directive} has the wrong number of arguments",
            line=line,
            field=directive,
            expected=str(expected - 1),
            actual=str(len(tokens) - 1),
            correction="use the directive shape published by the assembler contract",
        )


def _resolve_operand(
    operand_type: str,
    token: str,
    labels: dict[str, int],
    *,
    line: int,
) -> int:
    if operand_type in REGISTER_OPERANDS:
        return _parse_register(token, line=line, field=operand_type)
    if operand_type in LABEL_OPERANDS:
        if token in labels:
            return labels[token]
        if LABEL.fullmatch(token):
            _fail(
                "xvm.asm.label_unresolved",
                "instruction references an unknown label",
                line=line,
                field=operand_type,
                expected="a declared source label",
                actual=token,
                correction="declare the label or correct its spelling",
            )
        return _parse_u32(token, line=line, field=operand_type)
    maximum = 31 if operand_type == "count_0_31" else 0xFFFFFFFF
    minimum = 1 if operand_type == "iteration_count" else 0
    return _parse_u32(
        token,
        line=line,
        field=operand_type,
        minimum=minimum,
        maximum=maximum,
    )


def assemble_source(
    source: str,
    spec: dict[str, Any],
) -> AssemblyResult:
    if spec.get("isa_version") != "xvm-v2":
        _fail(
            "xvm.asm.isa_schema_invalid",
            "the assembler requires the authoritative xvm-v2 ISA schema",
            field="isa_version",
            expected="xvm-v2",
            actual=str(spec.get("isa_version")),
            correction="call describe_xvm_isa and use its published ISA definition",
        )

    opcode_by_name = {
        item["name"]: item for item in spec.get("opcodes", [])
    }
    metadata: dict[str, Any] = {
        "program_id": "",
        "memory_bytes": None,
        "output_bytes": None,
        "max_fuel": "auto",
        "max_call_depth": None,
    }
    optional_capabilities: set[str] = set()
    typed_views: dict[str, list[dict[str, Any]]] = {
        "input_views": [],
        "memory_views": [],
        "output_views": [],
    }
    labels: dict[str, int] = {}
    instructions: list[SourceInstruction] = []
    functions: list[FunctionRegion] = []
    active_function: FunctionRegion | None = None

    for line_number, raw in enumerate(source.splitlines(), start=1):
        stripped = raw.split("#", 1)[0].split(";", 1)[0].strip()
        if not stripped:
            continue
        if stripped.endswith(":"):
            label = stripped[:-1].strip()
            if LABEL.fullmatch(label) is None or label in labels:
                _fail(
                    "xvm.asm.label_invalid",
                    "label must be unique and use identifier syntax",
                    line=line_number,
                    field="label",
                    expected="[A-Za-z_][A-Za-z0-9_]*",
                    actual=label,
                    correction="rename the label and update its references",
                )
            labels[label] = len(instructions)
            continue

        tokens = _tokenize(raw, line_number)
        if not tokens:
            continue
        head = tokens[0].lower()
        if head.startswith("."):
            if head == ".program":
                _require_arity(tokens, 2, line=line_number, directive=head)
                if not SAFE_ID.fullmatch(tokens[1]):
                    _fail(
                        "xvm.asm.program_id_invalid",
                        "program id must be a safe 1..64 character id",
                        line=line_number,
                        field="program_id",
                        expected="[A-Za-z0-9_-]{1,64}",
                        actual=tokens[1],
                        correction="choose a safe program id",
                    )
                metadata["program_id"] = tokens[1]
            elif head in {".memory", ".output", ".max_call_depth"}:
                _require_arity(tokens, 2, line=line_number, directive=head)
                key = {
                    ".memory": "memory_bytes",
                    ".output": "output_bytes",
                    ".max_call_depth": "max_call_depth",
                }[head]
                maximum = (
                    int(spec["limits"]["max_call_depth"])
                    if key == "max_call_depth"
                    else 0xFFFFFFFF
                )
                metadata[key] = _parse_u32(
                    tokens[1],
                    line=line_number,
                    field=key,
                    minimum=1,
                    maximum=maximum,
                )
            elif head == ".max_fuel":
                _require_arity(tokens, 2, line=line_number, directive=head)
                metadata["max_fuel"] = (
                    "auto"
                    if tokens[1].lower() == "auto"
                    else _parse_u32(
                        tokens[1],
                        line=line_number,
                        field="max_fuel",
                        minimum=1,
                    )
                )
            elif head == ".capability":
                _require_arity(tokens, 2, line=line_number, directive=head)
                optional_capabilities.add(tokens[1])
            elif head == ".view":
                _require_arity(tokens, 7, line=line_number, directive=head)
                space = {
                    "input": "input_views",
                    "memory": "memory_views",
                    "output": "output_views",
                }.get(tokens[1].lower())
                if space is None:
                    _fail(
                        "xvm.asm.view_invalid",
                        "typed view address space is invalid",
                        line=line_number,
                        field="view.space",
                        expected="input|memory|output",
                        actual=tokens[1],
                        correction="use one of the published typed address spaces",
                    )
                typed_views[space].append(
                    {
                        "view_id": tokens[2],
                        "element_type": tokens[3],
                        "access": tokens[4],
                        "offset_bytes": _parse_u32(
                            tokens[5],
                            line=line_number,
                            field="view.offset_bytes",
                        ),
                        "length_bytes": _parse_u32(
                            tokens[6],
                            line=line_number,
                            field="view.length_bytes",
                            minimum=1,
                        ),
                    }
                )
            elif head == ".function":
                _require_arity(tokens, 2, line=line_number, directive=head)
                if active_function is not None:
                    _fail(
                        "xvm.asm.function_nested",
                        "functions may not be nested",
                        line=line_number,
                        field="function",
                        actual=tokens[1],
                        correction="close the active function before declaring another",
                    )
                name = tokens[1]
                if LABEL.fullmatch(name) is None or name in labels:
                    _fail(
                        "xvm.asm.function_name_invalid",
                        "function name must be a unique source label",
                        line=line_number,
                        field="function",
                        actual=name,
                        correction="choose a unique identifier",
                    )
                active_function = FunctionRegion(
                    line=line_number,
                    name=name,
                    entry_pc=len(instructions),
                )
                labels[name] = len(instructions)
            elif head == ".endfunction":
                _require_arity(tokens, 1, line=line_number, directive=head)
                if active_function is None or len(instructions) == active_function.entry_pc:
                    _fail(
                        "xvm.asm.function_region_invalid",
                        "endfunction requires a non-empty active function",
                        line=line_number,
                        field="function",
                        correction="declare function instructions ending in return",
                    )
                active_function.end_pc = len(instructions) - 1
                functions.append(active_function)
                active_function = None
            else:
                supported_directives = (
                    ".program|.memory|.output|.max_call_depth|.max_fuel|"
                    ".capability|.view|.function|.endfunction"
                )
                _fail(
                    "xvm.asm.directive_unknown",
                    "unknown assembler directive",
                    line=line_number,
                    field="directive",
                    expected=supported_directives,
                    actual=head,
                    correction=(
                        "replace .program_id with .program <program_id>"
                        if head == ".program_id"
                        else "use one of the directives listed in error.details.expected"
                    ),
                )
            continue

        if head not in opcode_by_name:
            _fail(
                "xvm.asm.opcode_unknown",
                "opcode is not present in the authoritative ISA schema",
                line=line_number,
                field="opcode",
                actual=head,
                correction="call describe_xvm_isa and use a published opcode name",
            )
        instructions.append(
            SourceInstruction(
                line=line_number,
                opcode_name=head,
                operands=tokens[1:],
            )
        )

    if active_function is not None:
        _fail(
            "xvm.asm.function_unclosed",
            "source ended inside a function",
            line=active_function.line,
            field="function",
            actual=active_function.name,
            correction="add .endfunction after the terminal return",
        )
    required_metadata_directives = {
        "program_id": ".program",
        "memory_bytes": ".memory",
        "output_bytes": ".output",
        "max_call_depth": ".max_call_depth",
    }
    for key, directive in required_metadata_directives.items():
        if metadata[key] in ("", None):
            _fail(
                "xvm.asm.metadata_required",
                f"required assembler metadata {key} is missing",
                field=key,
                expected=f"{directive} <value>",
                correction=f"add the {directive} directive",
            )
    if not functions:
        _fail(
            "xvm.asm.functions_required",
            "xvm-v2 requires at least one reachable declared function",
            field="functions",
            expected="1..64",
            actual="0",
            correction="add a called .function region ending in return",
        )

    words: list[int] = []
    for source_instruction in instructions:
        opcode = opcode_by_name[source_instruction.opcode_name]
        operand_types = list(opcode["operands"])
        explicit_types = [
            value for value in operand_types if value not in IMPLICIT_ZERO_OPERANDS
        ]
        if len(source_instruction.operands) != len(explicit_types):
            _fail(
                "xvm.asm.operand_count_invalid",
                "instruction operand count does not match the ISA schema",
                line=source_instruction.line,
                field=source_instruction.opcode_name,
                expected=str(len(explicit_types)),
                actual=str(len(source_instruction.operands)),
                correction="supply only non-reserved operands in published order",
            )
        source_operands = iter(source_instruction.operands)
        encoded_operands: list[int] = []
        for operand_type in operand_types:
            if operand_type in IMPLICIT_ZERO_OPERANDS:
                encoded_operands.append(0)
            else:
                encoded_operands.append(
                    _resolve_operand(
                        operand_type,
                        next(source_operands),
                        labels,
                        line=source_instruction.line,
                    )
                )
        words.extend([int(opcode["code"]), *encoded_operands])

    admitted_optional = list(spec.get("optional_capabilities", []))
    unknown_capabilities = optional_capabilities - set(admitted_optional)
    if unknown_capabilities:
        unknown = sorted(unknown_capabilities)[0]
        _fail(
            "xvm.asm.capability_unknown",
            "capability is not present in the authoritative ISA schema",
            field="capabilities",
            expected="a published optional capability",
            actual=unknown,
            correction="remove the capability or refresh the ISA description",
        )
    capabilities = list(spec["capabilities"]) + [
        capability
        for capability in admitted_optional
        if capability in optional_capabilities
    ]

    artifact: dict[str, Any] = {
        "schema_version": spec["program_schema_version"],
        "isa_version": spec["isa_version"],
        "profile": spec["profile"],
        "program_id": metadata["program_id"],
        "memory_bytes": metadata["memory_bytes"],
        "output_bytes": metadata["output_bytes"],
        "max_fuel": (
            0xFFFFFFFF
            if metadata["max_fuel"] == "auto"
            else metadata["max_fuel"]
        ),
        "max_call_depth": metadata["max_call_depth"],
        "capabilities": capabilities,
    }
    if "typed_memory_v1" in capabilities:
        artifact.update(typed_views)
    elif any(typed_views.values()):
        _fail(
            "xvm.asm.typed_memory_capability_required",
            "typed views require the typed_memory_v1 capability",
            field="capabilities",
            expected="typed_memory_v1",
            correction="add .capability typed_memory_v1",
        )
    artifact["functions"] = [
        {"entry_pc": region.entry_pc, "end_pc": region.end_pc}
        for region in functions
    ]
    artifact["bytecode_words"] = words

    try:
        verified = verify_program(artifact, spec)
        if metadata["max_fuel"] == "auto":
            artifact["max_fuel"] = int(
                verified["static_fuel_proof"]["worst_case_fuel"]
            )
            verified = verify_program(artifact, spec)
    except XvmValidationError as exc:
        _fail(
            exc.code,
            str(exc),
            field="program",
            actual=exc.code,
            correction="correct the assembly using describe_xvm_isa and reassemble",
        )

    artifact_bytes = canonical_json_bytes(artifact)
    return AssemblyResult(
        artifact=artifact,
        artifact_bytes=artifact_bytes,
        artifact_sha256=hashlib.sha256(artifact_bytes).hexdigest(),
        static_worst_case_fuel=int(
            verified["static_fuel_proof"]["worst_case_fuel"]
        ),
        instruction_count=len(words) // int(
            spec["encoding"]["instruction_words"]
        ),
    )


def validate_artifact(
    artifact_bytes: bytes,
    spec: dict[str, Any],
) -> AssemblyResult:
    try:
        artifact = json.loads(artifact_bytes.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        _fail(
            "xvm.program_json_invalid",
            f"artifact is not canonical UTF-8 JSON: {exc}",
            field="artifact",
            correction="reassemble the source with the canonical assembler",
        )
    canonical = canonical_json_bytes(artifact)
    if canonical != artifact_bytes:
        _fail(
            "xvm.asm.artifact_not_canonical",
            "artifact bytes do not match canonical serialization",
            field="artifact",
            expected="compact ordered UTF-8 JSON with one trailing LF",
            correction="reassemble instead of editing the artifact manually",
        )
    try:
        verified = verify_program(artifact, spec)
    except XvmValidationError as exc:
        _fail(
            exc.code,
            str(exc),
            field="artifact",
            actual=exc.code,
            correction="correct the source and reassemble",
        )
    return AssemblyResult(
        artifact=artifact,
        artifact_bytes=artifact_bytes,
        artifact_sha256=hashlib.sha256(artifact_bytes).hexdigest(),
        static_worst_case_fuel=int(
            verified["static_fuel_proof"]["worst_case_fuel"]
        ),
        instruction_count=len(verified["instructions"]),
    )


def submission_node(
    result: AssemblyResult,
    *,
    artifact_id: str,
    node_id: str,
    result_artifact_id: str,
    capabilities: Iterable[str] | None = None,
) -> dict[str, Any]:
    granted = list(capabilities or result.artifact["capabilities"])
    return {
        "node_id": node_id,
        "command": "xvm_program",
        "isa_version": "xvm-v2",
        "backend": "cpu_reference",
        "program_artifact_id": artifact_id,
        "expected_program_sha256": result.artifact_sha256,
        "result_artifact_id": result_artifact_id,
        "capabilities": granted,
        "resource_limits": {
            "fuel": result.artifact["max_fuel"],
            "memory_bytes": result.artifact["memory_bytes"],
            "output_bytes": result.artifact["output_bytes"],
        },
        "input_binding": {"mode": "typed_xvm_inputs_v1"},
    }


def _write_bytes(path: pathlib.Path, data: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)


def _write_json(path: pathlib.Path, value: Any) -> None:
    _write_bytes(path, canonical_json_bytes(value))


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--isa",
        type=pathlib.Path,
        default=DEFAULT_ISA_PATH,
        help="authoritative XVM v2 ISA schema",
    )
    subparsers = parser.add_subparsers(dest="action", required=True)

    assemble = subparsers.add_parser("assemble")
    assemble.add_argument("source", type=pathlib.Path)
    assemble.add_argument("--output", type=pathlib.Path, required=True)
    assemble.add_argument("--metadata-output", type=pathlib.Path)

    validate = subparsers.add_parser("validate")
    validate.add_argument("artifact", type=pathlib.Path)
    validate.add_argument("--metadata-output", type=pathlib.Path)
    return parser


def main(argv: list[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    try:
        spec = load_json(args.isa)
        if args.action == "assemble":
            result = assemble_source(
                args.source.read_text(encoding="utf-8"),
                spec,
            )
            _write_bytes(args.output, result.artifact_bytes)
        else:
            result = validate_artifact(args.artifact.read_bytes(), spec)
        metadata = result.metadata()
        if args.metadata_output:
            _write_json(args.metadata_output, metadata)
        print(json.dumps(metadata, separators=(",", ":"), sort_keys=False))
        return 0
    except XvmAssemblerError as exc:
        print(
            json.dumps(
                exc.as_dict(),
                separators=(",", ":"),
                sort_keys=False,
            ),
            file=sys.stderr,
        )
        return 2


if __name__ == "__main__":
    raise SystemExit(main())

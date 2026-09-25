"""Portable verifier and CPU reference executor for the machine-readable XVM v2 seed.

This tool is intentionally host-effect free. It reads one JSON program, verifies the
bounded control-flow contract, executes integer bytecode, and writes one canonical
JSON result to stdout. The Xbox worker has an independent C++ implementation; tests
keep both implementations aligned with schemas/xvm-isa-v2.json.
"""

from __future__ import annotations

import argparse
import hashlib
import hmac
import json
from dataclasses import dataclass
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_SPEC = ROOT / "schemas" / "xvm-isa-v2.json"


class XvmValidationError(ValueError):
    def __init__(self, code: str, message: str) -> None:
        super().__init__(message)
        self.code = code


@dataclass(frozen=True)
class Region:
    begin: int
    end: int
    main: bool


def _fail(code: str, message: str) -> None:
    raise XvmValidationError(code, message)


def load_json(path: Path) -> dict[str, Any]:
    with path.open("r", encoding="utf-8") as stream:
        value = json.load(stream)
    if not isinstance(value, dict):
        _fail("xvm.program_json_invalid", "program root must be an object")
    return value


def _instructions(program: dict[str, Any], spec: dict[str, Any]) -> list[tuple[int, int, int, int]]:
    words = program.get("bytecode_words")
    width = int(spec["encoding"]["instruction_words"])
    if not isinstance(words, list) or not words or len(words) % width:
        _fail("xvm.bytecode_shape_invalid", "bytecode must contain four-word instructions")
    if len(words) // width > int(spec["limits"]["max_instructions"]):
        _fail("xvm.instruction_limit_exceeded", "instruction limit exceeded")
    if any(not isinstance(word, int) or isinstance(word, bool) or word < 0 or word > 0xFFFFFFFF for word in words):
        _fail("xvm.program_word_invalid", "bytecode words must be unsigned 32-bit integers")
    return [tuple(words[index : index + width]) for index in range(0, len(words), width)]  # type: ignore[list-item]


def _regions(program: dict[str, Any], instruction_count: int, spec: dict[str, Any]) -> list[Region]:
    functions = program.get("functions")
    if not isinstance(functions, list) or not functions or len(functions) > int(spec["limits"]["max_functions"]):
        _fail("xvm.functions_required", "xvm-v2 requires a bounded function table")
    regions: list[Region] = []
    expected_entry = 0
    for index, function in enumerate(functions):
        if not isinstance(function, dict):
            _fail("xvm.function_invalid", "function entries must be objects")
        entry = function.get("entry_pc")
        end = function.get("end_pc")
        if not isinstance(entry, int) or not isinstance(end, int):
            _fail("xvm.function_invalid", "function pc values must be integers")
        if index == 0:
            if entry <= 0:
                _fail("xvm.function_region_invalid", "main must precede functions")
            regions.append(Region(0, entry - 1, True))
            expected_entry = entry
        if entry != expected_entry or end < entry or end >= instruction_count:
            _fail("xvm.function_region_invalid", "function regions must be ordered and contiguous")
        regions.append(Region(entry, end, False))
        expected_entry = end + 1
    if expected_entry != instruction_count:
        _fail("xvm.function_region_invalid", "function regions must cover bytecode after main")
    return regions


def _region_for_pc(regions: list[Region], pc: int) -> int:
    for index, region in enumerate(regions):
        if region.begin <= pc <= region.end:
            return index
    _fail("xvm.function_region_invalid", "pc is outside declared regions")


def _verify_typed_views(program: dict[str, Any], spec: dict[str, Any]) -> dict[str, list[dict[str, Any]]]:
    fields = ("input_views", "memory_views", "output_views")
    typed_memory = "typed_memory_v1" in program["capabilities"]
    if not typed_memory:
        if any(field in program for field in fields):
            _fail("xvm.typed_memory_capability_required", "typed-view declarations require typed_memory_v1")
        return {field: [] for field in fields}

    exact_fields = {"view_id", "element_type", "access", "offset_bytes", "length_bytes"}
    spaces = {
        "input_views": (int(spec["encoding"]["typed_input_words"]) * 4, {"read"}),
        "memory_views": (program["memory_bytes"], {"read", "write", "read_write"}),
        "output_views": (program["output_bytes"], {"write"}),
    }
    result: dict[str, list[dict[str, Any]]] = {}
    all_ids: set[str] = set()
    safe_chars = set("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-")
    for field, (space_bytes, allowed_access) in spaces.items():
        raw_views = program.get(field)
        if not isinstance(raw_views, list) or not 1 <= len(raw_views) <= 64:
            _fail("xvm.typed_views_required", "typed_memory_v1 requires 1..64 views in every address space")
        views: list[dict[str, Any]] = []
        for raw in raw_views:
            if not isinstance(raw, dict) or set(raw) != exact_fields:
                _fail("xvm.typed_view_shape_invalid", "typed-view fields do not match the v1 contract")
            view_id = raw["view_id"]
            if (
                not isinstance(view_id, str)
                or not 1 <= len(view_id) <= 64
                or any(ch not in safe_chars for ch in view_id)
                or view_id in all_ids
            ):
                _fail("xvm.typed_view_id_invalid", "typed-view ids must be safe and globally unique")
            if raw["element_type"] != "u32":
                _fail("xvm.typed_view_element_invalid", "typed_memory_v1 admits only u32 elements")
            if raw["access"] not in allowed_access:
                _fail("xvm.typed_view_access_invalid", "typed-view access is invalid for its address space")
            offset = raw["offset_bytes"]
            length = raw["length_bytes"]
            if (
                not isinstance(offset, int)
                or isinstance(offset, bool)
                or not isinstance(length, int)
                or isinstance(length, bool)
                or offset < 0
                or length <= 0
                or offset % 4
                or length % 4
                or offset + length > space_bytes
            ):
                _fail("xvm.typed_view_bounds_invalid", "typed-view bounds must be non-empty, u32-aligned and in range")
            if any(offset < view["offset_bytes"] + view["length_bytes"] and view["offset_bytes"] < offset + length for view in views):
                _fail("xvm.typed_view_overlap", "typed views in one address space may not overlap")
            all_ids.add(view_id)
            views.append(dict(raw))
        result[field] = views
    return result


def _static_fuel_proof(
    program: dict[str, Any],
    instructions: list[tuple[int, int, int, int]],
    regions: list[Region],
) -> dict[str, Any]:
    function_regions = {
        region.begin: index for index, region in enumerate(regions) if not region.main
    }
    loop_bounds = {
        pc: (a, b)
        for pc, (opcode, a, b, _) in enumerate(instructions)
        if opcode == 11
    }
    region_state = [0] * len(regions)
    region_cost = [0] * len(regions)

    def analyze_region(region_index: int) -> int:
        if region_state[region_index] == 2:
            return region_cost[region_index]
        if region_state[region_index] == 1:
            _fail("xvm.call_cycle_invalid", "static fuel analysis requires an acyclic call graph")
        region_state[region_index] = 1
        region = regions[region_index]
        cost = analyze_segment(region.begin, region.end + 1)
        region_cost[region_index] = cost
        region_state[region_index] = 2
        return cost

    def successor(costs: list[int], target: int, begin: int, end: int) -> int:
        if not begin <= target <= end:
            _fail("xvm.static_fuel_analysis_invalid", "verified control flow escaped its analysis segment")
        return costs[target]

    def analyze_segment(begin: int, end: int) -> int:
        if not 0 <= begin < end <= len(instructions):
            _fail("xvm.static_fuel_analysis_invalid", "invalid static fuel analysis segment")
        costs = [0] * (end + 1)
        for pc in range(end - 1, begin - 1, -1):
            opcode, a, b, c = instructions[pc]
            next_cost = costs[pc + 1]
            if opcode in (0, 16):
                costs[pc] = 1
            elif opcode == 10:
                costs[pc] = 1 + max(next_cost, successor(costs, b, begin, end))
            elif opcode in (23, 24, 25):
                target = c + 1 if opcode == 23 else c
                costs[pc] = 1 + max(next_cost, successor(costs, target, begin, end))
            elif opcode == 11:
                iterations, loop_end = loop_bounds[pc]
                if not pc + 1 <= loop_end < end:
                    _fail("xvm.static_fuel_analysis_invalid", "verified loop escaped its analysis segment")
                body_cost = analyze_segment(pc + 1, loop_end + 1)
                costs[pc] = 1 + iterations * body_cost + costs[loop_end + 1]
            elif opcode == 15:
                if a not in function_regions:
                    _fail("xvm.static_fuel_analysis_invalid", "verified call target is unavailable")
                costs[pc] = 1 + analyze_region(function_regions[a]) + next_cost
            elif opcode == 17:
                branch_pc = b + 1 if b < c else c + 1
                costs[pc] = 1 + max(next_cost, successor(costs, branch_pc, begin, end))
            elif opcode == 18:
                costs[pc] = 1 + successor(costs, a + 1, begin, end)
            else:
                costs[pc] = 1 + next_cost
        return costs[begin]

    worst_case = analyze_region(0)
    return {
        "schema_version": "xvm-static-fuel-proof-v1",
        "algorithm": "structured_cfg_longest_path_v1",
        "instruction_cost_model": "one_per_executed_instruction",
        "worst_case_fuel": worst_case,
        "declared_program_max_fuel": program["max_fuel"],
        "admitted_node_fuel": program["max_fuel"],
        "instruction_count": len(instructions),
        "region_count": len(regions),
        "exact": True,
        "bounded_loops_accounted": True,
        "acyclic_calls_accounted": True,
        "branch_maxima_accounted": True,
        "deterministic_trap_paths_bounded": True,
        "runtime_fuel_defense_in_depth": True,
    }



def verify_program(program: dict[str, Any], spec: dict[str, Any]) -> dict[str, Any]:
    if program.get("schema_version") != spec["program_schema_version"]:
        _fail("xvm.program_schema_invalid", "schema version mismatch")
    if program.get("isa_version") != spec["isa_version"] or program.get("profile") != spec["profile"]:
        _fail("xvm.isa_version_invalid", "ISA/profile mismatch")
    if not isinstance(program.get("program_id"), str) or not program["program_id"]:
        _fail("xvm.program_id_invalid", "program_id is required")

    for field in ("memory_bytes", "output_bytes", "max_fuel", "max_call_depth"):
        value = program.get(field)
        if not isinstance(value, int) or isinstance(value, bool) or value <= 0:
            _fail(f"xvm.{field}_invalid", f"{field} must be a positive integer")
    if program["memory_bytes"] % 4 or program["output_bytes"] % 4:
        _fail("xvm.alignment_invalid", "memory and output must be four-byte aligned")
    if program["max_call_depth"] > int(spec["limits"]["max_call_depth"]):
        _fail("xvm.call_depth_invalid", "max_call_depth exceeds ISA policy")

    capabilities = program.get("capabilities")
    required = set(spec["capabilities"])
    optional = set(spec.get("optional_capabilities", []))
    if (
        not isinstance(capabilities, list)
        or any(not isinstance(capability, str) for capability in capabilities)
        or len(capabilities) != len(set(capabilities))
        or not required.issubset(capabilities)
        or not set(capabilities).issubset(required | optional)
    ):
        _fail("xvm.capability_required", "capabilities must contain the required set and only admitted optional entries")
    typed_views = _verify_typed_views(program, spec)
    structured_control = "structured_control_v2" in capabilities
    structured_control_phase_b = "structured_control_v2_phase_b" in capabilities
    if structured_control_phase_b and not structured_control:
        _fail("xvm.structured_control_phase_b_dependency_required", "structured_control_v2_phase_b requires structured_control_v2")

    instructions = _instructions(program, spec)
    regions = _regions(program, len(instructions), spec)
    opcode_codes = {int(item["code"]) for item in spec["opcodes"]}
    register_count = int(spec["encoding"]["register_count"])
    input_words = int(spec["encoding"]["typed_input_words"])
    function_entries = {region.begin: index for index, region in enumerate(regions) if not region.main}
    call_edges: list[set[int]] = [set() for _ in regions]
    loop_stack: list[tuple[int, int, int]] = []
    loop_ranges: list[tuple[int, int, int]] = []
    branches: list[tuple[int, int, int]] = []
    conditionals: list[tuple[int, int, int, int, int]] = []
    ownership_by_pc: list[tuple[int, tuple[int, ...], tuple[int, ...]]] = []
    recovery_branches: list[tuple[int, int]] = []
    has_output = False
    has_control = False

    def view_admits(field: str, offset: int, access: str) -> bool:
        if "typed_memory_v1" not in capabilities:
            return True
        return any(
            view["offset_bytes"] <= offset
            and offset + 4 <= view["offset_bytes"] + view["length_bytes"]
            and (view["access"] == access or view["access"] == "read_write")
            for view in typed_views[field]
        )

    def reg(value: int) -> None:
        if value >= register_count:
            _fail("xvm.register_invalid", "register index exceeds ISA register file")

    for pc, (opcode, a, b, c) in enumerate(instructions):
        region_index = _region_for_pc(regions, pc)
        region = regions[region_index]
        ownership_by_pc.append((region_index, tuple(loop[0] for loop in loop_stack), tuple(frame[0] for frame in conditionals)))
        if opcode not in opcode_codes:
            _fail("xvm.opcode_not_admitted", "unknown opcode")
        if opcode == 0:
            if (a, b, c) != (0, 0, 0) or not region.main or pc != region.end:
                _fail("xvm.halt_invalid", "halt must terminate main")
        elif opcode in (1, 2, 6, 7, 8, 13, 14):
            reg(a)
            if opcode == 2 and b >= input_words:
                _fail("xvm.input_index_invalid", "typed input index out of bounds")
            if opcode == 2 and not view_admits("input_views", b * 4, "read"):
                _fail("xvm.typed_view_access_invalid", "input load is outside every declared readable u32 view")
            if opcode == 6:
                reg(b)
                if c >= 32:
                    _fail("xvm.rotate_invalid", "rotate count out of bounds")
            if opcode in (7, 8) and (b % 4 or b + 4 > program["memory_bytes"]):
                _fail("xvm.memory_access_invalid", "memory access out of bounds")
            if opcode in (7, 8) and not view_admits("memory_views", b, "read" if opcode == 7 else "write"):
                _fail("xvm.typed_view_access_invalid", "memory access is outside a view with matching access")
            if opcode == 13:
                if b % 4 or b + 4 > program["output_bytes"]:
                    _fail("xvm.output_access_invalid", "output access out of bounds")
                if not view_admits("output_views", b, "write"):
                    _fail("xvm.typed_view_access_invalid", "output store is outside every declared writable u32 view")
                has_output = True
            if opcode == 14:
                has_control = True
        elif opcode in (3, 4, 5, 9):
            reg(a)
            reg(b)
            reg(c)
        elif opcode in (20, 21, 22):
            if not structured_control_phase_b:
                _fail("xvm.structured_control_phase_b_capability_required", "ordered comparisons and select require Phase B")
            reg(a)
            reg(b)
            reg(c)
        elif opcode == 10:
            reg(a)
            if structured_control:
                _fail("xvm.structured_control_legacy_branch_invalid", "structured_control_v2 does not admit legacy branches")
            if loop_stack or b <= pc or b > region.end:
                _fail("xvm.branch_target_invalid", "branch must be forward inside the current region")
            branches.append((pc, b, region_index))
        elif opcode == 11:
            if a <= 0 or a > int(spec["limits"]["max_loop_iterations"]) or b <= pc or b > region.end or c != 0:
                _fail("xvm.loop_invalid", "loop declaration is outside policy")
            if instructions[b][0] != 12 or instructions[b][1] != pc:
                _fail("xvm.loop_pair_invalid", "loop pair mismatch")
            loop_stack.append((pc, b, region_index))
            if len(loop_stack) > int(spec["limits"]["max_loop_depth"]):
                _fail("xvm.loop_depth_exceeded", "loop nesting exceeds policy")
        elif opcode == 12:
            if b != 0 or c != 0 or not loop_stack or loop_stack[-1] != (a, pc, region_index):
                _fail("xvm.loop_pair_invalid", "loop end mismatch")
            if conditionals and conditionals[-1][3] == len(loop_stack):
                _fail("xvm.structured_control_region_invalid", "structured conditional crosses its containing loop")
            loop_ranges.append(loop_stack.pop())
        elif opcode in (23, 24):
            if not structured_control_phase_b:
                _fail("xvm.structured_control_phase_b_capability_required", "loop control requires Phase B")
            reg(a)
            if not loop_stack or loop_stack[-1] != (b, c, region_index):
                _fail("xvm.structured_control_loop_owner_invalid", "loop control must name the innermost verified loop")
        elif opcode == 15:
            if loop_stack:
                _fail("xvm.call_region_invalid", "calls may not cross an active structured loop")
            if b or c:
                _fail("xvm.call_invalid", "call reserves only operand a for a function entry pc")
            if a not in function_entries:
                _fail("xvm.call_target_invalid", "call target must be a declared function entry")
            call_edges[region_index].add(function_entries[a])
        elif opcode == 16:
            if (a, b, c) != (0, 0, 0) or region.main or pc != region.end:
                _fail("xvm.return_invalid", "return must terminate a function")
        elif opcode == 17:
            if not structured_control:
                _fail("xvm.structured_control_capability_required", "if_zero requires structured_control_v2")
            reg(a)
            valid_end = pc < c <= region.end and instructions[c] == (19, 0, 0, 0)
            valid_else = b == c or (pc < b < c and instructions[b] == (18, c, 0, 0))
            if not valid_end or not valid_else:
                _fail("xvm.structured_control_target_invalid", "if_zero must reference an exact else/end_if pair")
            conditionals.append((pc, b, c, len(loop_stack), region_index))
            if len(conditionals) > int(spec["limits"]["max_conditional_depth"]):
                _fail("xvm.structured_control_depth_exceeded", "structured conditional nesting exceeds policy")
        elif opcode == 18:
            if not structured_control:
                _fail("xvm.structured_control_capability_required", "else requires structured_control_v2")
            if not conditionals or conditionals[-1][1:] != (pc, a, len(loop_stack), region_index) or b or c:
                _fail("xvm.structured_control_pair_invalid", "else does not match the active conditional")
        elif opcode == 19:
            if not structured_control:
                _fail("xvm.structured_control_capability_required", "end_if requires structured_control_v2")
            if not conditionals or conditionals[-1][2:] != (pc, len(loop_stack), region_index) or a or b or c:
                _fail("xvm.structured_control_pair_invalid", "end_if does not close the active conditional")
            conditionals.pop()
        elif opcode == 25:
            if not structured_control_phase_b:
                _fail("xvm.structured_control_phase_b_capability_required", "deterministic traps require Phase B")
            reg(a)
            trap_limit = int(spec["structured_control_v2_phase_b"]["trap_code_maximum"])
            if not 1 <= b <= trap_limit or not pc < c <= region.end:
                _fail("xvm.structured_control_recovery_invalid", "trap code or forward recovery target is invalid")
            recovery_branches.append((pc, c))

    if loop_stack:
        _fail("xvm.loop_unclosed", "loop is not closed")
    if conditionals:
        _fail("xvm.structured_control_unclosed", "structured conditional is not closed")
    for _, target, region_index in branches:
        if any(loop_region == region_index and begin < target <= end for begin, end, loop_region in loop_ranges):
            _fail("xvm.branch_region_invalid", "branch enters a loop body")
    for source, target in recovery_branches:
        if ownership_by_pc[source] != ownership_by_pc[target]:
            _fail("xvm.structured_control_recovery_invalid", "trap recovery crosses structured ownership")
    if instructions[regions[0].end][0] != 0 or any(instructions[region.end][0] != 16 for region in regions[1:]):
        _fail("xvm.region_terminator_invalid", "region terminator mismatch")
    if not has_output or not has_control:
        _fail("xvm.result_contract_invalid", "program must write output and control")

    reachable: set[int] = set()
    observed_depth = 0

    def visit(region_index: int, depth: int, stack: set[int]) -> None:
        nonlocal observed_depth
        if region_index in stack:
            _fail("xvm.call_cycle_invalid", "call graph must be acyclic")
        reachable.add(region_index)
        observed_depth = max(observed_depth, depth)
        for target in call_edges[region_index]:
            visit(target, depth + 1, stack | {region_index})

    visit(0, 0, set())
    if observed_depth > program["max_call_depth"]:
        _fail("xvm.call_depth_exceeded", "declared call depth is insufficient")
    if reachable != set(range(len(regions))):
        _fail("xvm.function_unreachable", "all functions must be reachable")
    static_fuel_proof = _static_fuel_proof(program, instructions, regions)
    if static_fuel_proof["worst_case_fuel"] > program["max_fuel"]:
        _fail("xvm.static_fuel_program_limit_insufficient", "max_fuel is smaller than the verified worst-case instruction bound")
    return {"instructions": instructions, "regions": regions, "static_fuel_proof": static_fuel_proof}


def _new_machine_state(program: dict[str, Any], spec: dict[str, Any]) -> dict[str, Any]:
    return {
        "registers": [0] * int(spec["encoding"]["register_count"]),
        "memory": bytearray(program["memory_bytes"]),
        "output": bytearray(program["output_bytes"]),
        "loops": [],
        "calls": [],
        "trap": {"code": 0, "trap_pc": 0, "recovery_pc": 0, "occurrence_count": 0},
        "control": "fail",
        "fuel": 0,
        "pc": 0,
        "halted": False,
    }


def _validate_machine_state(
    program: dict[str, Any], spec: dict[str, Any], instructions: list[tuple[int, int, int, int]], state: dict[str, Any]
) -> None:
    if len(state["registers"]) != int(spec["encoding"]["register_count"]):
        _fail("xvm.snapshot_shape_invalid", "snapshot register file has the wrong size")
    if len(state["memory"]) != program["memory_bytes"] or len(state["output"]) != program["output_bytes"]:
        _fail("xvm.snapshot_shape_invalid", "snapshot memory or output has the wrong size")
    if state["control"] not in ("pass", "fail") or not 0 <= state["fuel"] <= program["max_fuel"]:
        _fail("xvm.snapshot_state_invalid", "snapshot scalar state is outside admission")
    if not 0 <= state["pc"] <= len(instructions):
        _fail("xvm.snapshot_pc_invalid", "snapshot pc is outside admitted bytecode")
    if len(state["loops"]) > int(spec["limits"]["max_loop_depth"]) or len(state["calls"]) > program["max_call_depth"]:
        _fail("xvm.snapshot_stack_invalid", "snapshot stack depth exceeds admission")
    phase_b = "structured_control_v2_phase_b" in program["capabilities"]
    trap = state.get("trap")
    if not isinstance(trap, dict) or set(trap) != {"code", "trap_pc", "recovery_pc", "occurrence_count"}:
        _fail("xvm.snapshot_trap_state_invalid", "snapshot trap state has the wrong shape")
    trap_values = (trap["code"], trap["trap_pc"], trap["recovery_pc"], trap["occurrence_count"])
    if any(not isinstance(value, int) or isinstance(value, bool) or value < 0 for value in trap_values):
        _fail("xvm.snapshot_trap_state_invalid", "snapshot trap state values are invalid")
    if not phase_b and any(trap_values):
        _fail("xvm.snapshot_trap_state_invalid", "non-Phase-B state may not contain trap state")
    if phase_b and trap["occurrence_count"] == 0 and any(trap_values[:3]):
        _fail("xvm.snapshot_trap_state_invalid", "empty trap state must use canonical zero fields")
    if phase_b and trap["occurrence_count"]:
        if (
            not 1 <= trap["code"] <= int(spec["structured_control_v2_phase_b"]["trap_code_maximum"])
            or not 0 <= trap["trap_pc"] < len(instructions)
            or not 0 <= trap["recovery_pc"] < len(instructions)
            or instructions[trap["trap_pc"]][0] != 25
            or instructions[trap["trap_pc"]][2:] != (trap["code"], trap["recovery_pc"])
        ):
            _fail("xvm.snapshot_trap_state_invalid", "recovered trap state does not match admitted bytecode")
    functions = list(program.get("functions", []))
    regions = [(0, functions[0]["entry_pc"] - 1)] if functions else [(0, len(instructions) - 1)]
    regions.extend((function["entry_pc"], function["end_pc"]) for function in functions)

    def region_for_pc(pc: int) -> int:
        for index, (begin, end) in enumerate(regions):
            if begin <= pc <= end:
                return index
        _fail("xvm.snapshot_pc_invalid", "snapshot pc is not owned by an admitted region")
        return -1

    function_entry_regions = {function["entry_pc"]: index + 1 for index, function in enumerate(functions)}
    for begin, end, remaining in state["loops"]:
        if (
            not 0 <= begin < end < len(instructions)
            or not 1 <= remaining <= instructions[begin][1]
            or instructions[begin][0] != 11
            or instructions[begin][2] != end
            or instructions[end][0] != 12
            or instructions[end][1] != begin
            or region_for_pc(begin) != region_for_pc(end)
        ):
            _fail("xvm.snapshot_loop_frame_invalid", "snapshot loop frame does not match admitted bytecode region")

    active_region = 0
    assigned_loop_depth = 0

    def validate_loop_segment(end_depth: int, expected_region: int, execution_pc: int) -> None:
        nonlocal assigned_loop_depth
        if not assigned_loop_depth <= end_depth <= len(state["loops"]):
            _fail("xvm.snapshot_call_frame_invalid", "snapshot call frame loop depth is not monotonic")
        previous_begin = -1
        previous_end = len(instructions)
        for index in range(assigned_loop_depth, end_depth):
            begin, end, _remaining = state["loops"][index]
            if (
                region_for_pc(begin) != expected_region
                or not begin < execution_pc <= end
                or (
                    index != assigned_loop_depth
                    and not (previous_begin < begin and end < previous_end)
                )
            ):
                _fail("xvm.snapshot_loop_frame_invalid", "snapshot loop stack is not an active nested region chain")
            previous_begin, previous_end = begin, end
        assigned_loop_depth = end_depth

    for return_pc, loop_depth in state["calls"]:
        if not functions or not 1 <= return_pc <= len(instructions) or not 0 <= loop_depth <= len(state["loops"]):
            _fail("xvm.snapshot_call_frame_invalid", "snapshot call frame is outside admission")
        call_pc = return_pc - 1
        if region_for_pc(call_pc) != active_region or instructions[call_pc][0] != 15:
            _fail("xvm.snapshot_call_frame_invalid", "snapshot return address is not an admitted call site")
        validate_loop_segment(loop_depth, active_region, call_pc)
        target_pc = instructions[call_pc][1]
        if target_pc not in function_entry_regions:
            _fail("xvm.snapshot_call_frame_invalid", "snapshot call target is not a declared function entry")
        active_region = function_entry_regions[target_pc]

    if not state["halted"]:
        if region_for_pc(state["pc"]) != active_region:
            _fail("xvm.snapshot_pc_invalid", "snapshot pc does not match the active call chain")
        validate_loop_segment(len(state["loops"]), active_region, state["pc"])
    if state["halted"]:
        if state["pc"] != len(instructions) or state["loops"] or state["calls"]:
            _fail("xvm.snapshot_halt_invalid", "halted snapshot must be terminal with balanced stacks")
    elif state["pc"] >= len(instructions):
        _fail("xvm.snapshot_pc_invalid", "non-terminal snapshot pc must address bytecode")


def _advance_program(
    program: dict[str, Any],
    spec: dict[str, Any],
    instructions: list[tuple[int, int, int, int]],
    inputs: list[int],
    state: dict[str, Any],
    checkpoint_fuel: int | None = None,
) -> str:
    _validate_machine_state(program, spec, instructions, state)
    registers = state["registers"]
    memory = state["memory"]
    output = state["output"]
    loops = state["loops"]
    calls = state["calls"]

    def read_u32(buffer: bytearray, offset: int) -> int:
        return int.from_bytes(buffer[offset : offset + 4], "little")

    def write_u32(buffer: bytearray, offset: int, value: int) -> None:
        buffer[offset : offset + 4] = (value & 0xFFFFFFFF).to_bytes(4, "little")

    while not state["halted"] and state["pc"] < len(instructions):
        if checkpoint_fuel is not None and state["fuel"] >= checkpoint_fuel:
            return "checkpoint"
        if state["fuel"] >= program["max_fuel"]:
            _fail("xvm.fuel_exhausted", "execution exhausted fuel")
        state["fuel"] += 1
        opcode, a, b, c = instructions[state["pc"]]
        if opcode == 0:
            if calls:
                _fail("xvm.halt_runtime_invalid", "halt encountered with active call frames")
            state["halted"] = True
            state["pc"] = len(instructions)
            break
        if opcode == 1:
            registers[a] = b
        elif opcode == 2:
            registers[a] = inputs[b] & 0xFFFFFFFF
        elif opcode == 3:
            registers[a] = (registers[b] + registers[c]) & 0xFFFFFFFF
        elif opcode == 4:
            registers[a] = registers[b] ^ registers[c]
        elif opcode == 5:
            registers[a] = (registers[b] * registers[c]) & 0xFFFFFFFF
        elif opcode == 6:
            value = registers[b]
            registers[a] = value if c == 0 else ((value << c) | (value >> (32 - c))) & 0xFFFFFFFF
        elif opcode == 7:
            registers[a] = read_u32(memory, b)
        elif opcode == 8:
            write_u32(memory, b, registers[a])
        elif opcode == 9:
            registers[a] = int(registers[b] == registers[c])
        elif opcode == 20:
            registers[a] = int(registers[b] < registers[c])
        elif opcode == 21:
            registers[a] = int((registers[b] ^ 0x80000000) < (registers[c] ^ 0x80000000))
        elif opcode == 22:
            registers[a] = registers[b] if registers[a] else registers[c]
        elif opcode == 10:
            state["pc"] = b if registers[a] == 0 else state["pc"] + 1
            continue
        elif opcode == 11:
            loops.append([state["pc"], b, a])
        elif opcode == 12:
            if not loops or loops[-1][0] != a or loops[-1][1] != state["pc"]:
                _fail("xvm.loop_runtime_invalid", "runtime loop frame mismatch")
            if loops[-1][2] > 1:
                loops[-1][2] -= 1
                state["pc"] = loops[-1][0] + 1
                continue
            loops.pop()
        elif opcode in (23, 24):
            if not loops or loops[-1][0] != b or loops[-1][1] != c:
                _fail("xvm.structured_control_loop_runtime_violation", "runtime loop control escaped verified ownership")
            if registers[a] == 0:
                if opcode == 23:
                    loops.pop()
                    state["pc"] = c + 1
                else:
                    state["pc"] = c
                continue
        elif opcode == 13:
            write_u32(output, b, registers[a])
        elif opcode == 14:
            state["control"] = "pass" if registers[a] else "fail"
        elif opcode == 15:
            calls.append([state["pc"] + 1, len(loops)])
            state["pc"] = a
            continue
        elif opcode == 16:
            if not calls:
                _fail("xvm.return_runtime_invalid", "return has no call frame")
            state["pc"], expected_loop_depth = calls.pop()
            if len(loops) != expected_loop_depth:
                _fail("xvm.return_runtime_invalid", "loop stack changed across call")
            continue
        elif opcode == 17:
            state["pc"] = (b + 1 if b < c else c + 1) if registers[a] == 0 else state["pc"] + 1
            continue
        elif opcode == 18:
            state["pc"] = a + 1
            continue
        elif opcode == 19:
            pass
        elif opcode == 25:
            if registers[a] == 0:
                trap = state["trap"]
                trap["code"] = b
                trap["trap_pc"] = state["pc"]
                trap["recovery_pc"] = c
                trap["occurrence_count"] += 1
                state["pc"] = c
                continue
        state["pc"] += 1
    _validate_machine_state(program, spec, instructions, state)
    if not state["halted"]:
        _fail("xvm.execution_incomplete", "program did not halt")
    return "halted"


def _result_from_state(program: dict[str, Any], state: dict[str, Any]) -> dict[str, Any]:
    if not state["halted"]:
        _fail("xvm.execution_incomplete", "cannot produce a result from non-terminal state")
    result = {
        "schema_version": "xvm-reference-result-0.1",
        "isa_version": program["isa_version"],
        "program_id": program["program_id"],
        "output_hex": state["output"].hex(),
        "control_token": state["control"],
        "fuel_consumed": state["fuel"],
    }
    if "structured_control_v2_phase_b" in program["capabilities"]:
        result["trap_state"] = dict(state["trap"])
    return result


REFERENCE_SNAPSHOT_AUTHORITY_KEY = b"xvm-reference-test-authority-v1"
SNAPSHOT_SEAL_SCHEMA = "xvm-state-snapshot-hmac-sha256-v1"
SNAPSHOT_PRODUCER = "xcompute-worker"


def _snapshot_state_fields(program: dict[str, Any], state: dict[str, Any]) -> dict[str, Any]:
    fields = {
        "pc": state["pc"],
        "fuel_consumed": str(state["fuel"]),
        "halted": state["halted"],
        "control_token": state["control"],
        "registers": list(state["registers"]),
        "memory_hex": state["memory"].hex(),
        "output_hex": state["output"].hex(),
        "loop_stack": [
            {"begin_pc": frame[0], "end_pc": frame[1], "remaining": frame[2]} for frame in state["loops"]
        ],
        "call_stack": [
            {"return_pc": frame[0], "loop_depth": frame[1]} for frame in state["calls"]
        ],
    }
    if "structured_control_v2_phase_b" in program["capabilities"]:
        trap = state["trap"]
        fields["trap_state"] = {
            "schema_version": "xvm-trap-state-v1",
            "status": "none" if trap["occurrence_count"] == 0 else "recovered",
            "code": trap["code"],
            "trap_pc": trap["trap_pc"],
            "recovery_pc": trap["recovery_pc"],
            "occurrence_count": str(trap["occurrence_count"]),
        }
    return fields


def canonical_machine_state_digest_json(
    program: dict[str, Any], state: dict[str, Any], program_sha256: str, bound_input_sha256: str
) -> str:
    value = {
        "schema_version": "xvm-machine-state-digest-v2" if "structured_control_v2_phase_b" in program["capabilities"] else "xvm-machine-state-digest-v1",
        "isa_version": program["isa_version"],
        "program_id": program["program_id"],
        "program_sha256": program_sha256,
        "bound_input_sha256": bound_input_sha256,
        **_snapshot_state_fields(program, state),
    }
    return json.dumps(value, separators=(",", ":"), ensure_ascii=False)


def canonical_state_snapshot(
    program: dict[str, Any],
    state: dict[str, Any],
    program_sha256: str,
    bound_input_sha256: str,
    execution_id: str,
    checkpoint_sequence: int,
    previous_state_sha256: str,
    seal_key: bytes = REFERENCE_SNAPSHOT_AUTHORITY_KEY,
) -> str:
    hashes = (program_sha256, bound_input_sha256, previous_state_sha256)
    if any(len(value) != 64 or value != value.lower() or any(ch not in "0123456789abcdef" for ch in value)
           for value in hashes):
        _fail("xvm.snapshot_context_invalid", "snapshot hashes must be canonical lowercase SHA-256")
    if not execution_id or len(execution_id) > 64 or any(not (ch.isalnum() or ch in "-_.") for ch in execution_id):
        _fail("xvm.snapshot_provenance_invalid", "snapshot execution id is invalid")
    if not isinstance(checkpoint_sequence, int) or isinstance(checkpoint_sequence, bool) or checkpoint_sequence < 1:
        _fail("xvm.snapshot_provenance_invalid", "snapshot checkpoint sequence is invalid")
    key_id = hashlib.sha256(seal_key).hexdigest()
    unsigned = {
        "schema_version": "xvm-state-snapshot-v3" if "structured_control_v2_phase_b" in program["capabilities"] else "xvm-state-snapshot-v2",
        "isa_version": program["isa_version"],
        "program_id": program["program_id"],
        "program_sha256": program_sha256,
        "bound_input_sha256": bound_input_sha256,
        "execution_id": execution_id,
        "checkpoint_sequence": str(checkpoint_sequence),
        "previous_state_sha256": previous_state_sha256,
        "producer": SNAPSHOT_PRODUCER,
        "seal_schema_version": SNAPSHOT_SEAL_SCHEMA,
        "seal_key_id": key_id,
        **_snapshot_state_fields(program, state),
    }
    unsigned_json = json.dumps(unsigned, separators=(",", ":"), ensure_ascii=False)
    unsigned["worker_seal_sha256"] = hmac.new(seal_key, unsigned_json.encode("utf-8"), hashlib.sha256).hexdigest()
    return json.dumps(unsigned, separators=(",", ":"), ensure_ascii=False)


def restore_state_snapshot(
    snapshot_json: str,
    program: dict[str, Any],
    spec: dict[str, Any],
    program_sha256: str,
    bound_input_sha256: str,
    *,
    seal_key: bytes = REFERENCE_SNAPSHOT_AUTHORITY_KEY,
    expected_execution_id: str | None = None,
    expected_checkpoint_sequence: int | None = None,
    expected_previous_state_sha256: str | None = None,
) -> dict[str, Any]:
    try:
        snapshot = json.loads(snapshot_json)
    except json.JSONDecodeError as error:
        raise XvmValidationError("xvm.snapshot_json_invalid", "snapshot is not valid JSON") from error
    phase_b = "structured_control_v2_phase_b" in program["capabilities"]
    expected_schema = "xvm-state-snapshot-v3" if phase_b else "xvm-state-snapshot-v2"
    expected_keys = {
        "schema_version", "isa_version", "program_id", "program_sha256", "bound_input_sha256",
        "execution_id", "checkpoint_sequence", "previous_state_sha256", "producer",
        "seal_schema_version", "seal_key_id", "pc", "fuel_consumed", "halted", "control_token",
        "registers", "memory_hex", "output_hex", "loop_stack", "call_stack", "worker_seal_sha256",
    }
    if phase_b:
        expected_keys.add("trap_state")
    if not isinstance(snapshot, dict) or set(snapshot) != expected_keys:
        _fail("xvm.snapshot_json_invalid", "snapshot fields do not match the admitted versioned contract")
    if (
        snapshot["schema_version"] != expected_schema
        or snapshot["isa_version"] != program["isa_version"]
        or snapshot["program_id"] != program["program_id"]
        or snapshot["program_sha256"] != program_sha256
        or snapshot["bound_input_sha256"] != bound_input_sha256
    ):
        _fail("xvm.snapshot_context_mismatch", "snapshot is not bound to the admitted program and input")

    execution_id = snapshot["execution_id"]
    sequence_text = snapshot["checkpoint_sequence"]
    previous_sha256 = snapshot["previous_state_sha256"]
    seal_key_id = snapshot["seal_key_id"]
    worker_seal = snapshot["worker_seal_sha256"]
    canonical_hex = lambda value: isinstance(value, str) and len(value) == 64 and value == value.lower() and all(
        ch in "0123456789abcdef" for ch in value
    )
    if (
        not isinstance(execution_id, str) or not execution_id or len(execution_id) > 64
        or any(not (ch.isalnum() or ch in "-_.") for ch in execution_id)
        or not isinstance(sequence_text, str) or not sequence_text.isdigit() or sequence_text.startswith("0")
        or not canonical_hex(previous_sha256)
        or snapshot["producer"] != SNAPSHOT_PRODUCER
        or snapshot["seal_schema_version"] != SNAPSHOT_SEAL_SCHEMA
        or not canonical_hex(seal_key_id)
        or not canonical_hex(worker_seal)
    ):
        _fail("xvm.snapshot_provenance_invalid", "snapshot provenance fields are invalid")
    if seal_key_id != hashlib.sha256(seal_key).hexdigest():
        _fail("xvm.snapshot_seal_invalid", "snapshot authority key id does not match")

    fuel_text = snapshot["fuel_consumed"]
    if not isinstance(fuel_text, str) or not fuel_text.isdigit() or (len(fuel_text) > 1 and fuel_text[0] == "0"):
        _fail("xvm.snapshot_json_invalid", "snapshot fuel is not canonical uint64 decimal")
    if not isinstance(snapshot["pc"], int) or isinstance(snapshot["pc"], bool) or not isinstance(snapshot["halted"], bool):
        _fail("xvm.snapshot_json_invalid", "snapshot pc or halted field has the wrong type")
    if not isinstance(snapshot["registers"], list) or any(
        not isinstance(value, int) or isinstance(value, bool) or not 0 <= value <= 0xFFFFFFFF for value in snapshot["registers"]
    ):
        _fail("xvm.snapshot_json_invalid", "snapshot registers are not uint32 values")

    def decode_hex(name: str, expected_bytes: int) -> bytearray:
        value = snapshot[name]
        if not isinstance(value, str) or len(value) != expected_bytes * 2 or value != value.lower() or any(
            ch not in "0123456789abcdef" for ch in value
        ):
            _fail("xvm.snapshot_hex_invalid", f"snapshot {name} is not canonical admitted hex")
        return bytearray.fromhex(value)

    loops = snapshot["loop_stack"]
    calls = snapshot["call_stack"]
    if not isinstance(loops, list) or any(not isinstance(frame, dict) or set(frame) != {"begin_pc", "end_pc", "remaining"} for frame in loops):
        _fail("xvm.snapshot_json_invalid", "snapshot loop stack shape is invalid")
    if not isinstance(calls, list) or any(not isinstance(frame, dict) or set(frame) != {"return_pc", "loop_depth"} for frame in calls):
        _fail("xvm.snapshot_json_invalid", "snapshot call stack shape is invalid")
    trap_state = {"code": 0, "trap_pc": 0, "recovery_pc": 0, "occurrence_count": 0}
    if phase_b:
        trap = snapshot["trap_state"]
        trap_keys = {"schema_version", "status", "code", "trap_pc", "recovery_pc", "occurrence_count"}
        if not isinstance(trap, dict) or set(trap) != trap_keys or trap.get("schema_version") != "xvm-trap-state-v1":
            _fail("xvm.snapshot_trap_state_invalid", "snapshot trap state shape or schema is invalid")
        numeric = (trap.get("code"), trap.get("trap_pc"), trap.get("recovery_pc"))
        count_text = trap.get("occurrence_count")
        if (
            any(not isinstance(value, int) or isinstance(value, bool) or value < 0 or value > 0xFFFFFFFF for value in numeric)
            or not isinstance(count_text, str) or not count_text.isdigit()
            or (len(count_text) > 1 and count_text[0] == "0")
        ):
            _fail("xvm.snapshot_trap_state_invalid", "snapshot trap state values are not canonical")
        count = int(count_text)
        if (count == 0 and trap.get("status") != "none") or (count != 0 and trap.get("status") != "recovered"):
            _fail("xvm.snapshot_trap_state_invalid", "snapshot trap status does not match occurrence count")
        trap_state = {"code": numeric[0], "trap_pc": numeric[1], "recovery_pc": numeric[2], "occurrence_count": count}
    state = {
        "registers": list(snapshot["registers"]),
        "memory": decode_hex("memory_hex", program["memory_bytes"]),
        "output": decode_hex("output_hex", program["output_bytes"]),
        "loops": [[frame["begin_pc"], frame["end_pc"], frame["remaining"]] for frame in loops],
        "calls": [[frame["return_pc"], frame["loop_depth"]] for frame in calls],
        "trap": trap_state,
        "control": snapshot["control_token"],
        "fuel": int(fuel_text),
        "pc": snapshot["pc"],
        "halted": snapshot["halted"],
    }
    instructions = verify_program(program, spec)["instructions"]
    _validate_machine_state(program, spec, instructions, state)

    unsigned = {key: value for key, value in snapshot.items() if key != "worker_seal_sha256"}
    unsigned_json = json.dumps(unsigned, separators=(",", ":"), ensure_ascii=False)
    expected_seal = hmac.new(seal_key, unsigned_json.encode("utf-8"), hashlib.sha256).hexdigest()
    if not hmac.compare_digest(worker_seal, expected_seal):
        _fail("xvm.snapshot_seal_invalid", "snapshot worker seal verification failed")

    authorization = (expected_execution_id, expected_checkpoint_sequence, expected_previous_state_sha256)
    if any(value is not None for value in authorization):
        if any(value is None for value in authorization) or authorization != (
            execution_id, int(sequence_text), previous_sha256
        ):
            _fail("xvm.resume_snapshot_unauthorized", "resume policy did not authorize snapshot provenance")
    return state

def execute_checkpoint_resume(
    program: dict[str, Any],
    spec: dict[str, Any],
    inputs: list[int],
    checkpoint_fuel: int,
    program_sha256: str,
    bound_input_sha256: str,
) -> dict[str, Any]:
    verified = verify_program(program, spec)
    instructions: list[tuple[int, int, int, int]] = verified["instructions"]
    if len(inputs) != int(spec["encoding"]["typed_input_words"]):
        _fail("xvm.typed_input_invalid", "exactly eleven typed input words are required")
    if not 1 <= checkpoint_fuel < program["max_fuel"]:
        _fail("xvm.snapshot_checkpoint_invalid", "checkpoint fuel must be inside admitted execution fuel")
    state = _new_machine_state(program, spec)
    previous_state_sha256 = hashlib.sha256(
        canonical_machine_state_digest_json(program, state, program_sha256, bound_input_sha256).encode("utf-8")
    ).hexdigest()
    execution_id = "reference-checkpoint"
    if _advance_program(program, spec, instructions, inputs, state, checkpoint_fuel) != "checkpoint":
        _fail("xvm.snapshot_checkpoint_unreachable", "program halted before checkpoint")
    snapshot_json = canonical_state_snapshot(
        program, state, program_sha256, bound_input_sha256, execution_id, 1, previous_state_sha256
    )
    snapshot_sha256 = hashlib.sha256(snapshot_json.encode("utf-8")).hexdigest()
    restored = restore_state_snapshot(
        snapshot_json,
        program,
        spec,
        program_sha256,
        bound_input_sha256,
        expected_execution_id=execution_id,
        expected_checkpoint_sequence=1,
        expected_previous_state_sha256=previous_state_sha256,
    )
    restored_json = canonical_state_snapshot(
        program, restored, program_sha256, bound_input_sha256, execution_id, 1, previous_state_sha256
    )
    if restored_json != snapshot_json or hashlib.sha256(restored_json.encode("utf-8")).hexdigest() != snapshot_sha256:
        _fail("xvm.snapshot_restore_hash_mismatch", "restored snapshot digest changed")
    _advance_program(program, spec, instructions, inputs, restored)
    resumed = _result_from_state(program, restored)
    uninterrupted = execute_program(program, spec, inputs)
    if resumed != uninterrupted:
        _fail("xvm.snapshot_replay_mismatch", "resumed result differs from uninterrupted execution")
    return {"result": resumed, "snapshot_json": snapshot_json, "snapshot_sha256": snapshot_sha256}


def execute_cancel_resume(
    program: dict[str, Any],
    spec: dict[str, Any],
    inputs: list[int],
    cancellation_fuel: int,
    program_sha256: str,
    bound_input_sha256: str,
) -> dict[str, Any]:
    verified = verify_program(program, spec)
    instructions: list[tuple[int, int, int, int]] = verified["instructions"]
    if len(inputs) != int(spec["encoding"]["typed_input_words"]):
        _fail("xvm.typed_input_invalid", "exactly eleven typed input words are required")
    if not 1 <= cancellation_fuel < program["max_fuel"]:
        _fail("xvm.snapshot_checkpoint_invalid", "cancellation fuel must be inside admitted execution fuel")

    canceled_state = _new_machine_state(program, spec)
    previous_state_sha256 = hashlib.sha256(
        canonical_machine_state_digest_json(program, canceled_state, program_sha256, bound_input_sha256).encode("utf-8")
    ).hexdigest()
    execution_id = "reference-cancel"
    if _advance_program(program, spec, instructions, inputs, canceled_state, cancellation_fuel) != "checkpoint":
        _fail("xvm.snapshot_checkpoint_unreachable", "program halted before simulated cancellation")
    snapshot_json = canonical_state_snapshot(
        program, canceled_state, program_sha256, bound_input_sha256, execution_id, 1, previous_state_sha256
    )
    snapshot_sha256 = hashlib.sha256(snapshot_json.encode("utf-8")).hexdigest()

    later_job_state = restore_state_snapshot(
        snapshot_json,
        program,
        spec,
        program_sha256,
        bound_input_sha256,
        expected_execution_id=execution_id,
        expected_checkpoint_sequence=1,
        expected_previous_state_sha256=previous_state_sha256,
    )
    resumed_from_fuel = later_job_state["fuel"]
    _advance_program(program, spec, instructions, inputs, later_job_state)
    resumed = _result_from_state(program, later_job_state)
    uninterrupted = execute_program(program, spec, inputs)
    if resumed != uninterrupted:
        _fail("xvm.snapshot_replay_mismatch", "later-job resume differs from uninterrupted execution")
    return {
        "result": resumed,
        "snapshot_json": snapshot_json,
        "snapshot_sha256": snapshot_sha256,
        "resumed_from_fuel": resumed_from_fuel,
        "prefix_reexecuted": False,
    }


def execute_program(program: dict[str, Any], spec: dict[str, Any], inputs: list[int]) -> dict[str, Any]:
    verified = verify_program(program, spec)
    instructions: list[tuple[int, int, int, int]] = verified["instructions"]
    if len(inputs) != int(spec["encoding"]["typed_input_words"]):
        _fail("xvm.typed_input_invalid", "exactly eleven typed input words are required")
    state = _new_machine_state(program, spec)
    _advance_program(program, spec, instructions, inputs, state)
    return _result_from_state(program, state)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("program", type=Path)
    parser.add_argument("--spec", type=Path, default=DEFAULT_SPEC)
    parser.add_argument("--input-words", default="1,2,0,0,0,0,0,0,0,0,1")
    args = parser.parse_args()
    inputs = [int(value, 0) for value in args.input_words.split(",")]
    result = execute_program(load_json(args.program), load_json(args.spec), inputs)
    print(json.dumps(result, sort_keys=True, separators=(",", ":")))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

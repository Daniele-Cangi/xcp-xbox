"""Bounded PC comparison of P4 XVM execution with the separate PR #1 API."""

from __future__ import annotations

import copy
import importlib.util
import sys
from pathlib import Path

import pytest

from xcp.xvm import reference as public_reference


ROOT = Path(__file__).resolve().parents[1] / "reference/xcompute-probe"
SPEC = ROOT / "schemas/xvm-isa-v2.json"
SAMPLES = ROOT / "samples/xvm"
INPUTS = [1, 2, 0, 0, 0, 0, 0, 0, 0, 0, 1]


@pytest.fixture(scope="module")
def p4_reference():
    path = ROOT / "tools/xvm_reference.py"
    spec = importlib.util.spec_from_file_location("p4_xvm_reference", path)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    yield module
    sys.modules.pop(spec.name, None)


@pytest.mark.parametrize(
    "name",
    [
        "xvm-v2-call-chain.json",
        "xvm-v2-cancel-resume-loop.json",
        "xvm-v2-structured-control-phase-b.json",
        "xvm-v2-structured-control-v2.json",
        "xvm-v2-typed-memory-v1.json",
    ],
)
def test_pc_execution_result_matches_p4(p4_reference, name: str) -> None:
    spec = p4_reference.load_json(SPEC)
    program = p4_reference.load_json(SAMPLES / name)
    assert public_reference.execute_program(program, spec, INPUTS) == p4_reference.execute_program(
        program, spec, INPUTS
    )


@pytest.mark.parametrize(
    ("mutation", "expected"),
    [
        ("invalid_call", "xvm.call_target_invalid"),
        ("insufficient_fuel", "xvm.static_fuel_program_limit_insufficient"),
    ],
)
def test_pc_rejection_code_matches_p4(p4_reference, mutation: str, expected: str) -> None:
    spec = p4_reference.load_json(SPEC)
    program = copy.deepcopy(p4_reference.load_json(SAMPLES / "xvm-v2-call-chain.json"))
    if mutation == "invalid_call":
        program["bytecode_words"][9] = 9
    else:
        program["max_fuel"] = 12
    with pytest.raises(p4_reference.XvmValidationError) as original:
        p4_reference.verify_program(program, spec)
    with pytest.raises(public_reference.XvmValidationError) as public:
        public_reference.verify_program(program, spec)
    assert original.value.code == public.value.code == expected

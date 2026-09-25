from __future__ import annotations

import json
from pathlib import Path

from xcp.cli import main
from xcp.xvm.reference import DEFAULT_SPEC, execute_program, load_json


def test_assembler_and_cli_verify_roundtrip(tmp_path: Path, capsys) -> None:
    source = tmp_path / "program.xvmasm"
    artifact = tmp_path / "program.json"
    source.write_text("\n".join((
        ".program synthetic-xvm-v2",
        ".memory 16",
        ".output 16",
        ".max_fuel auto",
        ".max_call_depth 1",
        "move_immediate r0, 7",
        "output_u32 r0, 0",
        "move_immediate r1, 1",
        "set_control r1",
        "call finish",
        "halt",
        ".function finish",
        "return",
        ".endfunction",
    )) + "\n", encoding="utf-8")
    assert main(["xvm", "assemble", str(source), str(artifact)]) == 0
    capsys.readouterr()
    assert main(["xvm", "verify", str(artifact)]) == 0
    assert json.loads(capsys.readouterr().out)["ok"]
    result = execute_program(load_json(artifact), load_json(DEFAULT_SPEC), [0] * 11)
    assert result["output_hex"].startswith("07000000")

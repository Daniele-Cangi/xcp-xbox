"""Command line entrypoint for the public reference tools."""

from __future__ import annotations

import argparse
import json
from pathlib import Path

from . import project
from .conformance import SUITES
from .xvm.assembler import XvmAssemblerError, assemble_source
from .xvm.reference import DEFAULT_SPEC, XvmValidationError, execute_program, load_json, verify_program


def _words(value: str) -> list[int]:
    return [int(word, 0) for word in value.split(",")]


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="xcp")
    commands = parser.add_subparsers(dest="area", required=True)
    project_parser = commands.add_parser("project", help="Project and bundle tooling")
    project_parser.add_argument("args", nargs=argparse.REMAINDER)
    conformance = commands.add_parser("conformance", help="Run local conformance suites")
    conformance.add_argument("suite", choices=[*SUITES, "all"])
    xvm = commands.add_parser("xvm", help="Portable XVM v2 reference")
    xvm_commands = xvm.add_subparsers(dest="action", required=True)
    for action in ("verify", "run"):
        command = xvm_commands.add_parser(action)
        command.add_argument("program", type=Path)
        command.add_argument("--spec", type=Path, default=DEFAULT_SPEC)
        if action == "run":
            command.add_argument("--input-words", default="1,2,0,0,0,0,0,0,0,0,1")
    assemble = xvm_commands.add_parser("assemble")
    assemble.add_argument("source", type=Path)
    assemble.add_argument("output", type=Path)
    assemble.add_argument("--spec", type=Path, default=DEFAULT_SPEC)
    args = parser.parse_args(argv)
    if args.area == "project":
        return project.main(args.args)
    if args.area == "conformance":
        names = list(SUITES) if args.suite == "all" else [args.suite]
        reports = [SUITES[name]() for name in names]
        print(json.dumps({"ok": True, "reports": reports}, sort_keys=True, separators=(",", ":")))
        return 0
    try:
        spec = load_json(args.spec)
        if args.action == "assemble":
            result = assemble_source(args.source.read_text(encoding="utf-8"), spec)
            args.output.write_bytes(result.artifact_bytes)
            print(json.dumps(result.metadata(), sort_keys=True, separators=(",", ":")))
            return 0
        program = load_json(args.program)
        if args.action == "verify":
            admitted = verify_program(program, spec)
            result = {
                "ok": True,
                "isa_version": spec["isa_version"],
                "instruction_count": len(admitted["instructions"]),
                "static_fuel_proof": admitted["static_fuel_proof"],
            }
        else:
            result = execute_program(program, spec, _words(args.input_words))
        print(json.dumps(result, sort_keys=True, separators=(",", ":")))
        return 0
    except (XvmValidationError, XvmAssemblerError, ValueError) as exc:
        print(json.dumps({"ok": False, "error": {"code": getattr(exc, "code", "xvm.input_invalid"), "message": str(exc)}}, sort_keys=True))
        return 2


if __name__ == "__main__":
    raise SystemExit(main())

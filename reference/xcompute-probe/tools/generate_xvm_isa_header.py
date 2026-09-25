#!/usr/bin/env python3
"""Generate/check the worker's embedded XVM v2 ISA from its JSON authority."""

from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import sys
from typing import Any


ROOT = pathlib.Path(__file__).resolve().parents[1]
DEFAULT_SCHEMA = ROOT / "schemas" / "xvm-isa-v2.json"
DEFAULT_OUTPUT = (
    ROOT
    / "src"
    / "XComputeProbe"
    / "runtime"
    / "WorkerXvmIsaV2.generated.h"
)


def canonical_schema_json(schema: dict[str, Any]) -> str:
    return json.dumps(
        schema,
        ensure_ascii=False,
        separators=(",", ":"),
        sort_keys=False,
    )


def render_header(schema_bytes: bytes) -> str:
    schema = json.loads(schema_bytes.decode("utf-8"))
    canonical = canonical_schema_json(schema)
    sha256 = hashlib.sha256(schema_bytes).hexdigest()
    return (
        "#pragma once\n\n"
        "// Generated from schemas/xvm-isa-v2.json. Do not edit by hand.\n"
        "namespace XComputeProbe\n"
        "{\n"
        "    inline constexpr wchar_t WorkerXvmIsaV2DefinitionJsonValue[] =\n"
        f'        LR"XVMISA({canonical})XVMISA";\n'
        "    inline constexpr wchar_t WorkerXvmIsaV2DefinitionSha256Value[] =\n"
        f'        L"{sha256}";\n'
        "}\n"
    )


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--schema", type=pathlib.Path, default=DEFAULT_SCHEMA)
    parser.add_argument("--output", type=pathlib.Path, default=DEFAULT_OUTPUT)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args(argv)
    expected = render_header(args.schema.read_bytes())
    if args.check:
        actual = (
            args.output.read_text(encoding="utf-8")
            if args.output.exists()
            else ""
        )
        if actual != expected:
            print(
                "generated XVM ISA header is stale; regenerate from the schema",
                file=sys.stderr,
            )
            return 1
        return 0
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(expected, encoding="utf-8", newline="\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

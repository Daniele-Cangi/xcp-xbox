# P4 XVM PC reference

The P4/r10 Python assembler, verifier/interpreter, ISA header generator, five
synthetic XVM v2 programs, two assembly source examples and original reference
test are preserved byte for byte under `reference/xcompute-probe/`. Their
per-file hashes are in the reference extraction manifest. The already
transferred C++ worker runtime and
`WorkerXvmIsaV2.generated.h` remain unchanged. The newer `src/xcp/xvm/` API
stays separate until paired parity is established.

From the public checkout, the header generator checks the frozen C++ header
against the exact JSON ISA without rewriting either file:

```powershell
python reference/xcompute-probe/tools/generate_xvm_isa_header.py --check
```

The original Python suite has 26 tests. Twenty-five run from the reference
root without a device:

```powershell
Push-Location reference/xcompute-probe
python -m pytest -q tests/unit/test_xvm_reference.py -k 'not test_machine_readable_isa_matches_cpp_catalog'
Pop-Location
```

The remaining original test reads a historical live Xbox runner with a
hard-coded device address. That runner is excluded by the publication boundary
and is not required for PC assembly, ISA verification or native compilation.
The test itself remains exact and visible, while CI explicitly deselects that
one case. The header generator check covers its key schema-to-C++ header
assertion. Seven separate paired PC vectors compare the frozen P4 Python
reference with the new `src/xcp/xvm/` API: five execution results and two
rejection codes match. This is bounded evidence for those vectors, not full
snapshot/provenance parity. It does not prove Python/C++ execution parity,
Xbox behavior or P5/r12 hardware closure.

Two more PC tests run the exact assembler CLI on the preserved `.xvmasm`
examples, validate the canonical artifacts it produces, and execute them with
the exact Python reference. The generated artifacts stay in test temporary
directories and receive new digests; they are not historical worker inputs.

The original `test_agent_native_xvm_toolchain_v1.py` is now transferred as
well. Its PC assembler/interpreter cases run in CI. One static case reads a
historical live runner that is excluded from the source release, so CI
explicitly deselects that case. The runtime and the test body remain exact;
the deselection is recorded rather than treated as a parity pass.

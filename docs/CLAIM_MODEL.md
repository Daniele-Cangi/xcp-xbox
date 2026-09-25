# Claim model

XCP distinguishes authored intent, source observation, semantic
representation, admission, execution and fidelity. Each step needs its own
evidence. Source and target
observations may have different execution/result identities but must share an
exact comparison context: source, Source Model, IR, intent, project, plan and target
profile hashes.

`ProjectIntent` v2 hashes the author-declared acceptance keys and tolerances
before admission. The admission plan binds that intent; `xcp.evidence.compare`
requires the exact declared criteria and rejects a post-execution tolerance
substitution. It also rejects mismatched contexts and missing invariant sets.
Execution
failure or missing observation yields
`insufficient`; execution success alone never yields a fidelity pass.
The comparison derives execution success from a target receipt bound to the
exact plan, profile, execution, result and target observation hash. The public
receipt contract is v2; a v1 receipt without an observation hash cannot support
this claim. Receipt authenticity and the link from result artifact to observed
values remain the target's responsibility. Exact replay requires distinct
execution identities and compares logical observations across two successful
target receipts. Stage attribution reports only the
first failed stage in a complete prefix of separately bound stage evidence.
It is a routing result, not causal proof or an aggregate score. The archive
target claims only structural packaging. Its module bytes include the
authorized plan hash, so a changed plan changes the resulting bundle identity. XVM
reference conformance claims local ISA behavior, not Xbox execution or
worker snapshot authenticity.

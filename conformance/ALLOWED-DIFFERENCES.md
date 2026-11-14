<!-- SPDX-License-Identifier: Apache-2.0 -->

# Allowed differences

FR-11.7 requires every permitted divergence to be documented per field. The
canonicaliser normalises the first three rows; the comparator ignores the
fourth and compares the rest exactly.

| Field | Treatment | Why |
|---|---|---|
| Thread and object identifiers | replaced by labels scoped to the creating thread | rcvid, coid and chid values are allocation-order artefacts and differ by target |
| Payload bytes | SHA-256 plus length | the bytes must match; the representation in the log may differ |
| Order of independent events | canonical linear extension of the partial order | Mazurkiewicz equivalence (FR-11.3) |
| Virtual timestamps | excluded by default; `--strict-time` includes them | scheduling costs differ on real hardware; timing is not part of causal equivalence |
| Process name in `THREAD_START` | dropped | the target binary and the simulator guest have different names |
| Host paths and wall-clock time | never recorded in the canonical trace | host-only data |

## Not allowed

Outcome class, errno, which thread unblocks, message payload hashes, rcvid
validity, per-thread program order, send → receive and receive → reply edges,
event counts, effective priority at a dispatch, pulse codes and values.

## Known canonicalisation limits

Thread labels are assigned by first appearance. If a target creates two
independent threads in the opposite order, their labels can swap even though
the partial orders are isomorphic. Fixing that needs a per-target id map from
the capture agent; it is recorded as LIMITS #61, not hidden.

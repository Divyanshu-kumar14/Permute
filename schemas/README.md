<!-- SPDX-License-Identifier: Apache-2.0 -->

# Vendored schemas

| File | Source | Why |
|---|---|---|
| `sarif-2.1.0.json` | OASIS SARIF 2.1.0, `oasis-tcs/sarif-spec`, `sarif-2.1/schema/sarif-schema-2.1.0.json` | `test_report` validates `sarif.json` with `jsonschema.Draft4Validator`. Vendored so CI needs no network. The schema is used verbatim; its title identifies the OASIS document |

Retrieved 2026-11-14. The schema's own `$schema` is draft-04, so validation
uses `Draft4Validator` explicitly.

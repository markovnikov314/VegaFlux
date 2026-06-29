# Decision 0001: Manifest Component Field

Status: accepted

## Context

The public repository should describe the system in product terms, not in terms of internal milestone history. Experiment manifests are still useful, but the manifest field that identifies the subsystem under test should read as a stable component label.

## Decision

`ExperimentManifest` field number 2 is named `component_id`.

The field number stays fixed so serialized protobuf payloads keep the same wire tag. JSON artifacts and config files use `component_id` or `component` consistently.

## Verification

`manifest_component_schema` checks that both the canonical schema snapshot and the active schema keep `ExperimentManifest.component_id = 2`.

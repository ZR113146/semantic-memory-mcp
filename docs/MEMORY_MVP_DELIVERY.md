# Long-Term Memory MVP Delivery Notes

This note captures the current delivery posture for the graph-style long-term memory MVP.

## Windows Validation

Run the focused memory framework validation on Windows with:

```cmd
scripts\test-memory-windows.cmd
```

The `.cmd` wrapper runs the PowerShell script with process-local `ExecutionPolicy Bypass`, so it does not require changing machine policy.

Latest local result:

```text
19/19 passed
```

## Implemented MVP Surface

- Single SQLite-backed memory schema: `memory_event`, `memory_item`, `memory_edge`, `memory_vec`, and `memory_fts`.
- Hot path `events` writes a raw event and a `candidate` item only.
- `admin_consolidate` performs deterministic candidate processing, dedup/update/coexist handling, evidence edges, and vector upsert.
- `memories_retrieve` combines structured filters, FTS, vector candidates, evidence expansion, read-time conflict resolution, and hit counter writeback.
- `memory_update_status`, `memory_feedback`, `admin_decay`, and `memory_health` cover lifecycle, feedback, explainable decay, and audit counters.

## Intentional MVP Difference From The Target Framework

The target framework mentions `sqlite-vec` / `vec0`. This codebase currently keeps the MVP zero-dependency by storing deterministic 768-dimensional vectors in a regular SQLite table (`memory_vec`) as a BLOB. Retrieval still has a vector path, but it is not backed by the sqlite-vec extension.

Treat this as an explicit delivery decision unless sqlite-vec becomes a hard acceptance requirement. If sqlite-vec is required, add it as a separate dependency/packaging task instead of mixing it into the current stabilization pass.

## Remaining Release Checks

- Confirm local antivirus tooling has not quarantined `scripts/setup.sh` again before release.
- Run the focused Windows memory validation from a clean shell using `scripts\test-memory-windows.cmd`.
- Run broader project tests/builds as time allows after the MVP memory path is green.

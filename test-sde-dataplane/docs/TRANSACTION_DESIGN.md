# Transaction Manager Design

## 1. Architecture Overview

The transaction manager sits between the control plane and the ASIC-facing HAL tables. It records a batch of FDB and route operations, validates them, and applies them atomically as one unit.

```text
Control Plane / CLI / REST
            |
            v
   hal_txn (journal + locks + rollback)
       |    |    |
       |    |    +--> hal_resource pools
       |    +--------> FDB/Route table APIs
       +--------------> ASIC driver
```

Key data structures:
- `hal_txn_t`: transaction identity, state, options, operation journal, and stats.
- `hal_txn_op_entry_t`: one journal entry with `op`, `table`, `entry`, `original`, and `applied` flag.
- `txn_entry_lock_t`: per-entry pessimistic lock keyed by table + logical key.

State machine:

```text
PENDING --add--> ACTIVE --commit--> COMMITTED
   \                 \--rollback/abort--> ABORTED
    \--abort--> ABORTED
    \--commit failure--> FAILED --rollback/abort--> ABORTED
```

## 2. Concurrency Strategy

This implementation uses Option A: pessimistic locking.

- Lock granularity is per-entry, not per-table or global.
- FDB keys are MAC + VLAN; route keys are VRF + prefix + prefix length.
- A lock is acquired when an operation is added to the transaction and released when the transaction ends (commit, rollback, or abort).

This prevents concurrent transactions from mutating the same key simultaneously. Deadlock risk is low because all transactions acquire at most one lock per entry and release them promptly at the end of the transaction. In practice, lock acquisition is ordered by the natural table/key ordering before commit to keep the path deterministic.

## 3. Failure Handling

Transaction safety is enforced with inverse operations:

- `ADD` -> `DELETE`
- `DELETE` -> `ADD` using the saved original snapshot
- `UPDATE` -> `UPDATE` using the saved original values

Rollback is best-effort: it runs in reverse order and continues if a single inverse operation fails. The transaction then releases locks and returns the failed status to the caller.

If the ASIC driver times out or a table operation fails during commit, the transaction is marked `FAILED`, previously applied operations are undone, and any reserved resources are released. The caller can then call `hal_txn_rollback()` or `hal_txn_abort()` to finalize cleanup.

## 4. Scale Considerations

- Maximum concurrent transactions is bounded by the operation journal capacity and lock table size.
- Memory overhead is modest: each journal entry stores a compact operation record plus a snapshot for UPDATE/DELETE.
- Resource reservation happens before commit so the transaction cannot over-allocate pool capacity.
- This is suitable for single-node control-plane use; high-scale multi-node or distributed transactions are out of scope.

## 5. Trade-offs and Future Work

Chosen not to implement:
- global transaction lock for simplicity and performance
- optimistic concurrency because it is harder to reason about with rollback and ASIC-side mutation
- distributed transactions or multi-device coordination

Future improvements:
- timeout-aware waiting instead of immediate busy-fail
- per-table conflict groups for larger batch optimization
- richer metrics for rollback failures and lock contention
- explicit deadlock detection with waiter queues

# Principal Software Engineer Track: Transaction Manager

## Background

In production network operating systems, configuration changes often span multiple ASIC tables. For example, creating an L3 interface requires coordinated updates to:

- Router interface table
- VLAN membership
- ARP/neighbor table entries
- Routing table entries

If any step fails, partial state is unacceptable—it can cause traffic black-holes or loops. We need **transaction semantics** with rollback capability.

## Your Task

Design and implement a transaction manager that provides atomic multi-table operations with full rollback support.

**Time Estimate:** 3-4 hours (core implementation) + design document

## Requirements

### 1. Transaction API

```c
/* Transaction handle (opaque) */
typedef struct hal_txn_s hal_txn_t;

/* Transaction operation types */
typedef enum hal_txn_op_e {
    HAL_TXN_OP_ADD,
    HAL_TXN_OP_DELETE,
    HAL_TXN_OP_UPDATE
} hal_txn_op_t;

/* Transaction options */
typedef struct hal_txn_opts_s {
    uint32_t timeout_ms;        /* Max transaction duration (0 = no timeout) */
    bool     auto_rollback;     /* Rollback on any error (default: true) */
} hal_txn_opts_t;

/* Begin a new transaction */
hal_status_t hal_txn_begin(const hal_txn_opts_t *opts, hal_txn_t **txn);

/* Add FDB operation to transaction */
hal_status_t hal_txn_add_fdb(hal_txn_t *txn, hal_txn_op_t op,
                             hal_fdb_entry_t *entry);

/* Add route operation to transaction */
hal_status_t hal_txn_add_route(hal_txn_t *txn, hal_txn_op_t op,
                               hal_route_entry_t *entry);

/* Commit all operations atomically */
hal_status_t hal_txn_commit(hal_txn_t *txn);

/* Rollback all operations */
hal_status_t hal_txn_rollback(hal_txn_t *txn);

/* Abort transaction (cleanup without commit/rollback) */
hal_status_t hal_txn_abort(hal_txn_t *txn);
```

### 2. Atomic Commit

All operations in a transaction must succeed together or none should take effect.

**Behavior:**
- Operations are recorded but not applied until `commit()`
- On commit, apply each operation in order
- If any operation fails, automatically rollback all previously applied operations
- Return the error from the failed operation

### 3. Rollback Mechanism

Implement rollback by generating inverse operations:

| Original Operation | Rollback Operation |
|-------------------|-------------------|
| ADD entry | DELETE entry (by key) |
| DELETE entry | ADD entry (restore from snapshot) |
| UPDATE entry | UPDATE entry (restore original values) |

**Requirements:**
- For DELETE operations, snapshot the entry before deletion
- Handle rollback failures gracefully (log and continue)
- Clean up transaction state after rollback

### 4. Conflict Detection

Detect conflicting modifications to the same entries.

**Choose one approach:**

**Option A: Pessimistic Locking**
- Lock entries when added to transaction
- Block other transactions from modifying same entries
- Release locks on commit/rollback/abort

**Option B: Optimistic Concurrency**
- Record entry version/timestamp when added
- On commit, verify entries haven't changed
- Abort if conflict detected

Document your choice and reasoning.

### 5. Resource Pre-Validation

Before committing, verify resources are available:

```c
// Pseudo-code for commit
hal_status_t hal_txn_commit(hal_txn_t *txn) {
    // 1. Calculate resource requirements
    int fdb_adds = count_adds(txn, FDB);
    int route_adds = count_adds(txn, ROUTE);

    // 2. Reserve resources
    hal_resource_reserve(fdb_pool, fdb_adds);
    hal_resource_reserve(route_pool, route_adds);

    // 3. Apply operations
    for each op in txn:
        status = apply(op);
        if (status != SUCCESS) {
            rollback_applied();
            unreserve_resources();
            return status;
        }

    // 4. Release reservations (now allocated)
    unreserve_and_mark_allocated();
    return HAL_SUCCESS;
}
```

### 6. Transaction States

Implement a state machine:

```
        begin()
           │
           ▼
      ┌─────────┐
      │ PENDING │ ◄──────────────┐
      └────┬────┘                │
           │ add_*()             │ add_*() after
           ▼                     │ partial rollback
      ┌─────────┐                │
      │ ACTIVE  │────────────────┘
      └────┬────┘
           │
     ┌─────┴─────┐
     │           │
commit()    rollback()/abort()
     │           │
     ▼           ▼
┌─────────┐ ┌──────────┐
│COMMITTED│ │ ABORTED  │
└─────────┘ └──────────┘
```

## Design Document

In addition to code, provide a **1-2 page design document** covering:

### 1. Architecture Overview
- Component diagram
- Data structures for transaction state
- State machine diagram

### 2. Concurrency Strategy
- Your locking approach (pessimistic vs optimistic)
- Lock granularity (per-entry, per-table, global)
- Deadlock prevention/detection

### 3. Failure Handling
- What happens if rollback fails?
- ASIC driver timeout handling
- Recovery from partial state

### 4. Scale Considerations
- Max concurrent transactions
- Memory overhead per transaction
- Performance implications

### 5. Trade-offs Made
- What you chose NOT to implement and why
- Alternative approaches considered
- Future improvements

## Implementation Hints

### New Files Needed

```
include/hal_txn.h      # Transaction API
src/hal_txn.c          # Transaction manager implementation
tests/test_txn.c       # Transaction tests
```

### Transaction Structure Suggestion

```c
typedef struct hal_txn_s {
    uint64_t            id;             /* Unique transaction ID */
    hal_txn_state_t     state;          /* Current state */
    hal_txn_opts_t      opts;           /* Configuration */
    hal_time_us_t       start_time;     /* For timeout detection */

    /* Operation journal */
    hal_txn_entry_t    *entries;        /* Array/list of operations */
    size_t              entry_count;
    size_t              entry_capacity;

    /* Rollback info */
    size_t              applied_count;  /* How many committed so far */

    /* Locking (if pessimistic) */
    // ...

    /* Resource reservations */
    uint32_t            fdb_reserved;
    uint32_t            route_reserved;

} hal_txn_t;

typedef struct hal_txn_entry_s {
    hal_txn_op_t        op;             /* ADD/DELETE/UPDATE */
    hal_txn_table_t     table;          /* FDB/ROUTE/etc. */
    union {
        hal_fdb_entry_t     fdb;
        hal_route_entry_t   route;
    } entry;
    union {
        hal_fdb_entry_t     fdb;
        hal_route_entry_t   route;
    } original;                         /* For rollback of DELETE/UPDATE */
    bool                applied;        /* Has this been committed? */
} hal_txn_entry_t;
```

### Rollback Pseudocode

```c
static hal_status_t rollback_entry(hal_txn_entry_t *e) {
    switch (e->op) {
        case HAL_TXN_OP_ADD:
            // Reverse of add is delete
            return delete_entry(e->table, &e->entry);

        case HAL_TXN_OP_DELETE:
            // Reverse of delete is add (restore original)
            return add_entry(e->table, &e->original);

        case HAL_TXN_OP_UPDATE:
            // Reverse of update is update with original values
            return update_entry(e->table, &e->original);
    }
}

static hal_status_t txn_rollback(hal_txn_t *txn) {
    // Rollback in reverse order
    for (int i = txn->applied_count - 1; i >= 0; i--) {
        hal_status_t rv = rollback_entry(&txn->entries[i]);
        if (rv != HAL_SUCCESS) {
            // Log error but continue rollback
            LOG_ERROR("Rollback failed for entry %d: %s", i, hal_status_str(rv));
        }
    }
    return HAL_SUCCESS;
}
```

## Testing

Create comprehensive tests:

### Basic Functionality
1. Simple transaction with single operation
2. Transaction with multiple operations (same table)
3. Transaction with operations across tables (FDB + Route)

### Commit Scenarios
4. Successful commit
5. Commit with failure mid-way (verify rollback)
6. Commit with resource exhaustion

### Rollback Scenarios
7. Explicit rollback before commit
8. Automatic rollback on failure
9. Rollback of DELETE operations (restore)

### Conflict Detection
10. Concurrent modification detection
11. Proper error return on conflict

### Edge Cases
12. Empty transaction commit
13. Abort without commit
14. Double commit (should fail)
15. Operations after commit (should fail)
16. Timeout handling

### Stress Testing
17. Many operations in single transaction
18. Concurrent transactions

## Deliverables

1. **`include/hal_txn.h`** - Transaction API header
2. **`src/hal_txn.c`** - Transaction manager implementation
3. **`tests/test_txn.c`** - Comprehensive test suite
4. **`DESIGN.md`** - Design document (1-2 pages)

## Evaluation Criteria

| Category | Weight | Description |
|----------|--------|-------------|
| **Architecture** | 30% | Sound design, clear trade-offs, scalability awareness |
| **Correctness** | 25% | Transaction semantics correct, rollback works |
| **Concurrency** | 20% | Race-free, deadlock prevention, appropriate granularity |
| **Code Quality** | 15% | Clean abstractions, maintainable, documented |
| **Testing** | 10% | Failure scenarios, edge cases covered |

## What We're Looking For

**Strong signals:**
- Clear state machine with well-defined transitions
- Explicit handling of failure modes
- Discussion of alternatives considered
- Awareness of scale implications
- Tests that inject failures

**Red flags:**
- No design document
- Ignoring rollback failure scenarios
- Lock order violations / deadlock potential
- Unbounded memory growth
- No consideration of timeout
- Over-engineering without working core

## Stretch Goals (Optional)

If you finish early, consider:

1. **Nested transactions** - Transactions within transactions
2. **Read-only transactions** - Snapshot isolation for reads
3. **Transaction logging** - Persistent journal for crash recovery
4. **Batched ASIC programming** - Combine operations for efficiency

## Questions?

Document any assumptions you make. We're evaluating your ability to make reasonable decisions under ambiguity as much as your technical implementation.

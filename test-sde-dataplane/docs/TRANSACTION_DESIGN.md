# Transaction Manager Design

## 1. Architecture Overview

The transaction manager sits between the control plane and the ASIC-facing HAL tables. It records a batch of FDB and route operations, validates them, and applies them atomically as one unit.

### Component Diagram

```
┌─────────────────────────────────────────────────────────┐
│                  Transaction Manager API                │
│  (hal_txn_begin, hal_txn_add_*, hal_txn_commit/abort)   │
└──────────────────────┬──────────────────────────────────┘
                       │
        ┌──────────────┼──────────────┐
        │              │              │
        ▼              ▼              ▼
┌──────────────┐  ┌─────────────┐  ┌──────────────┐
│ Journal      │  │ Lock Table  │  │ Resource     │
│ Management   │  │ (8K entries)│  │ Pool Manager │
│              │  │             │  │              │
│ - Entries[]  │  │             │  │ Reserve/     │
│ - States     │  │ per-entry   │  │ Unreserve    │
│ - Rollback   │  │ locking     │  │              │
└──────┬───────┘  └─────────────┘  └──────────────┘
       │
       ├─────────────┬─────────────┐
       ▼             ▼             ▼
   ┌────────┐  ┌──────────┐  ┌──────────┐
   │ FDB    │  │ Route    │  │ Rollback │
   │ Table  │  │ Table    │  │ Handlers │
   └────────┘  └──────────┘  └──────────┘
```

### Key data structures

#### `hal_txn_t`

Transaction identity, state, options, operation journal, and stats.

```c
typedef struct hal_txn_s {
    uint64_t            id;             /* Unique transaction ID */
    hal_txn_state_t     state;          /* Current state */
    hal_txn_opts_t      opts;           /* Configuration options */
    hal_time_us_t       start_time;     /* Start timestamp for timeout */
    
    /* Operation journal */
    hal_txn_op_entry_t *entries;        /* Array of operations */
    size_t              entry_count;    /* Current number of entries */
    size_t              entry_capacity; /* Allocated capacity */
    
    /* Rollback tracking */
    size_t              applied_count;  /* Number of ops successfully applied */

	/* Locking index */

    /* Resource reservations */
    uint32_t            fdb_reserved;   /* FDB entries reserved */
    uint32_t            route_reserved; /* Route entries reserved */
    
    /* Stats */
    hal_txn_stats_t     stats;

	hal_txn_t			*next;
	hal_txn_t			*prev;
} hal_txn_t;
```

#### `hal_txn_op_entry_t`

One journal entry with `op`, `table`, `entry`, `original`, and `applied` flag.

```c
typedef struct hal_txn_op_entry_s {
    hal_txn_op_t        op;             /* ADD/DELETE/UPDATE */
    hal_txn_table_t     table;          /* FDB/ROUTE */
    
    union {
        hal_fdb_entry_t     fdb;
        hal_route_entry_t   route;
    } entry;
    
    /* For rollback: stores original state for DELETE/UPDATE */
    union {
        hal_fdb_entry_t     fdb;
        hal_route_entry_t   route;
    } original;
    
    bool                applied;        /* Has this op been applied? */
    uint32_t            lock_index;     /* Lock table index (if locked) */
    bool                locked;         /* Is entry locked? */
} hal_txn_op_entry_t;
```

#### `txn_entry_lock_t`

Per-entry pessimistic lock keyed by (table + logical key).

```c
	typedef struct 	txn_entry_lock_a {
		hal_txn_table_t		table;
		uint64_t 			key;            /* key (MAC+VLAN or VRF+prefix+len) */
		uint64_t 			owner_txn_id;   /* Transaction ID holding the lock */
		bool 				locked;         /* Lock state: true if currently held */
	} txn_entry_lock_t;
```

### State machine

```
                            begin()
                               │
                               ▼
                          ┌─────────┐
                          │ PENDING │─────────────┐ 
                          └────┬────┘             │ 
                               │ add_*()          │ 
                               ▼                  │ 
                          ┌─────────┐             │ 
                          │ ACTIVE  │             │ 
                          └────┬────┘           abort()
                               │                  │
           ┌────────────┬──────┴────┐             │
           │            │           │             │
        failure      commit()  rollback()/abort() │
           │            │           │             │
           ▼            ▼           ▼             │
       ┌────────┐  ┌─────────┐ ┌──────────┐       │
       │ FAILED │  │COMMITTED│ │ ABORTED  │◄──────┘
       └───┬────┘  └─────────┘ └──────────┘
           │                        ▲
           └───rollback()/abort()───┘
```


## 2. Concurrency Strategy

This implementation uses Option A: pessimistic locking.

- Lock granularity is per-entry, not per-table or global.
- FDB keys are MAC + VLAN; route keys are VRF + prefix + prefix length.
- A lock is acquired immediately when an operation is added and released after commit, successful rollback, or abort. A failed add releases only its newly acquired lock and leaves the transaction's prior state and journal unchanged.

This prevents transaction-managed operations from taking the same key concurrently. Direct FDB/route API calls do not use this lock manager and therefore are not coordinated by transaction locks. Acquisition is in add-operation order and fails immediately with `HAL_E_BUSY`; it does not wait. This avoids wait-cycle deadlocks, but a transaction may hold earlier locks while a later add returns busy.

Optimistic locking was not selected because it discovers conflicts at commit, potentially after ASIC operations have been applied and need to be undone. Pessimistic locking avoids that late conflict path, at the cost of holding locks while a transaction is being assembled.


## 3. Failure Handling

Transaction safety is enforced with inverse operations:

- `ADD` -> `DELETE`
- `DELETE` -> `ADD` using the saved original snapshot
- `UPDATE` -> `UPDATE` using the saved original values

Rollback is best-effort: it runs in reverse order, logs inverse-operation failures, continues attempting the remaining inverses, and returns the first rollback error. Operations that could not be undone remain marked as applied so rollback/abort can be retried. Locks are retained while rollback remains incomplete; commit-time resource reservations are also retained if automatic rollback of a failed commit is incomplete.

If the ASIC driver or a table operation fails during commit, the transaction is marked `FAILED`. With `auto_rollback=true`, the manager attempts to undo applied operations; locks and reservations are released only if that rollback succeeds. With `auto_rollback=false`, applied work, locks, and reservations are retained until the caller invokes `hal_txn_rollback()` or `hal_txn_abort()`. Those calls return a rollback error and leave the transaction FAILED if any inverse still fails. The original commit error is returned from `hal_txn_commit()`; rollback errors are logged.

If resource pre-validation fails, no table operation has been applied; reservations are cleared and the ACTIVE transaction retains its entry locks so it can be retried or aborted.

The configured duration timeout is checked when adding an operation, before commit begins, and between commit operations. Expiry prevents further application and triggers the configured rollback behavior; staged operations and locks are not silently discarded. The caller must abort the transaction if commit has not started or finish rollback after a timed-out commit. Lock conflicts do not wait and are returned as `HAL_E_BUSY`.

## 4. Scale Considerations

- At most 1,024 live transaction handles are admitted; each transaction can hold up to 256 operations. The 8,192 lock slots bound simultaneously held entry locks, not transaction count.
- Memory overhead is modest.
	* Each live transaction preallocates 16 journal entries and grows up to 256.
	* Each journal entry contains both an entry union and an original-state union, even when the operation does not need a snapshot.
	* This gives a bounded worst-case journal size per transaction; the total bound is set by the live transaction limit.
- Resource reservation happens before commit
	* This reduces the risk of running out of resource-pool capacity partway through commit.
	* Reservation counts ADD operations and does not net them against DELETE operations in the same transaction, so it can conservatively reject a batch that would fit after deletes.
- This is suitable for single-node control-plane use; high-scale multi-node or distributed transactions are out of scope.
- The fixed lock array is scanned linearly for conflict checks and free slots. A hash table can improve expected lookup time when collisions are resolved with key equality; a balanced tree provides predictable logarithmic lookup at greater implementation cost.

## 5. Trade-offs and Future Work

Chosen not to implement:
- A global transaction lock, which would serialize operations on unrelated keys.
- optimistic concurrency is not choosen since it detects conflicts during commit which may need rollback.
       * Lock table coordinates only transaction managed operations.
       * Direct table API calls do not use the lock table. So current design doesnt block those calls by lock table.
- Distributed transactions or multi-device coordination, which exceed this HAL's single-process scope.
- A hash/tree lock index, to keep the initial implementation simple and bounded.

Future improvements:
- Add timeout-aware waiting for conflicting entries; currently conflicts return `HAL_E_BUSY` immediately.
- Replace the linear lock-array scans with a collision-safe hash table or balanced tree for better scale.
- Add distributed transactions or multi-device coordination if the system requires cross-device atomicity.
- Add aggregate timing, contention, reservation, and rollback-failure metrics.

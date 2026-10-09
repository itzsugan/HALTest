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
- A lock is acquired when an operation is added to the transaction and released when the transaction ends (commit, rollback, or abort).

This prevents concurrent transactions from mutating the same key simultaneously. Deadlock risk is low because all transactions acquire at most one lock per entry and release them promptly at the end of the transaction. In practice, lock acquisition is ordered by the natural table/key ordering before commit to keep the path deterministic.

 [-] Why optimistic is not used in design:
       - Optimistic locking detects conflicts at the last stage (commit) which requires rollback of all previous entries.
	     In HAL, since asic programming is costly/expensive operation, blocking conflicting operation is more efficient than detecting at last and rolling back.

## 3. Failure Handling

Transaction safety is enforced with inverse operations:

- `ADD` -> `DELETE`
- `DELETE` -> `ADD` using the saved original snapshot
- `UPDATE` -> `UPDATE` using the saved original values

Rollback is best-effort: it runs in reverse order and continues if a single inverse operation fails. The transaction then releases locks and returns the failed status to the caller.

If the ASIC driver times out or a table operation fails during commit, the transaction is marked `FAILED`, previously applied operations are undone, and any reserved resources are released when auto_rollback is true. When auto_rollback is false, Applications should then call `hal_txn_rollback()` or `hal_txn_abort()` to finalize cleanup.



## 4. Scale Considerations

- Maximum concurrent transactions is bounded by the operation journal capacity and lock table size (TXN_LOCK_TABLE_SIZE).
- Memory overhead is modest.
	* Each journal entry stores a compact operation record, entry info plus a snapshot for UPDATE/DELETE.
	* The memory cost is roughly the size of two entry unions per operation, not just the operation being performed.
- Resource reservation happens before commit
	* This tries to ensure capacity before applying changes, reducing the risk of a transaction running out of resources midway
	* The implementation reserves based on the number of ADD operations; it does not currently net those against DELETE operations in the same transaction.
- This is suitable for single-node control-plane use; high-scale multi-node or distributed transactions are out of scope.
- Lock table doesnt scale well
	* the current implementation scans a fixed array to find and check locks, so lock lookup and allocation are linear in the table size.
	* Alternate option:
		- Hash --> Hash collisions will lead to incorrect conflict.
		- RB Tree would be a better choice, can support faster (table + logical key) based lookups.

## 5. Trade-offs and Future Work

Chosen not to implement:
- global transaction lock for simplicity and performance
- optimistic concurrency is not choosen since it detects conflicts during commit which may need rollback.
	* Lock table coordinates only transaction managed operations. Direct table API calls do not use the lock table.
	* So current design doesnt block those calls by lock table.
- distributed transactions or multi-device coordination - simpler scope.
- Lock table is not chosen to implemente with advanced data structures due to time limitation.

Future improvements:
- timeout-aware waiting for conflicting entries instead of immediate busy-fail
- distributed transactions or multi-device coordination.
- Lock table can be implemented with RB tree for better scale and performance.

/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 HAL Assessment Project
 *
 * hal_txn.h - L2 Forwarding Database (FDB) API
 *
 * Provides atomic multi-table transaction support with full rollback capability.
 * Transactions allow coordinated updates across multiple HAL tables (FDB, Routes)
 * with guaranteed atomicity: all operations succeed together or none take effect.
 */

#ifndef HAL_TXN_H
#define HAL_TXN_H

#include "hal_types.h"
#include "hal_error.h"
#include "hal_fdb.h"
#include "hal_route.h"
#include "hal_resource.h"

#ifdef __cplusplus
extern "C" {
#endif

#include <pthread.h>

/* ============================================================================
 * MACROS
 * ============================================================================ */

#define TXN_MAX_ENTRY_CAPACITY  	256     /* Max ops per transaction */
#define TXN_INIT_ENTRY_CAPACITY		16
#define TXN_LOCK_TIMEOUT_MS     	5000    /* Lock acquisition timeout */
#define TXN_DEFAULT_TIMEOUT_MS  	0       /* No timeout by default */
#define TXN_MAX_CONCURRENT_TRANSACTIONS 1024

/* Global lock table for all entries */
#define TXN_LOCK_TABLE_SIZE 8192

/* ============================================================================
 * TYPES and ENUMS
 * ============================================================================ */
typedef struct hal_txn_s hal_txn_t;

/**
 * Transaction operation types
 *
 * Describes the type of operation recorded in a transaction.
 */
typedef enum hal_txn_op_e {
    HAL_TXN_OP_ADD,      /* Add a new entry to table */
    HAL_TXN_OP_DELETE,   /* Delete an existing entry */
    HAL_TXN_OP_UPDATE    /* Update an existing entry */
} hal_txn_op_t;

/**
 * Transaction table types
 *
 * Identifies which HAL table an operation targets.
 */
typedef enum hal_txn_table_e {
    HAL_TXN_TABLE_FDB,   /* L2 Forwarding Database */
    HAL_TXN_TABLE_ROUTE  /* L3 Route Table */
} hal_txn_table_t;

/**
 * Transaction state machine states
 *
 * Tracks the lifecycle of a transaction through its various states.
 */
typedef enum hal_txn_state_e {
    HAL_TXN_STATE_INVALID = 0,
    HAL_TXN_STATE_PENDING,   /* Initialized, accepting operations */
    HAL_TXN_STATE_ACTIVE,    /* Operations being applied */
    HAL_TXN_STATE_COMMITTED, /* Successfully committed */
    HAL_TXN_STATE_FAILED,    /* Commit/rollback failed; recovery may be pending */
    HAL_TXN_STATE_ABORTED    /* Aborted or rolled back */
} hal_txn_state_t;



/* ============================================================================
 * TXN Structures
 * ============================================================================ */

/**
 * Transaction options
 *
 * Configuration passed to hal_txn_begin().
 */
typedef struct hal_txn_opts_s {
    uint32_t timeout_ms;        /* Max transaction duration in ms (0 = no limit) */
    bool     auto_rollback;     /* Attempt rollback on commit failure (default: true) */
} hal_txn_opts_t;

/**
 * Transaction statistics (for debugging/monitoring)
 */
typedef struct hal_txn_stats_s {
    uint32_t total_operations;  /* Total ops added to transaction */
    uint32_t applied_operations;/* Ops successfully applied */
    uint32_t fdb_adds;          /* FDB add operations */
    uint32_t fdb_deletes;       /* FDB delete operations */
    uint32_t fdb_updates;       /* FDB update operations */
    uint32_t route_adds;        /* Route add operations */
    uint32_t route_deletes;     /* Route delete operations */
    uint32_t route_updates;     /* Route update operations */
    uint32_t resources_reserved;/* Resources reserved but not yet allocated */
} hal_txn_stats_t;

/**
 * Transaction Entry Lock Structure
 *
 * Uses pessimistic locking approach: lock on add, release on commit/abort.
 * Lock granularity is per-entry
 * Key: 64bit. MAC+VLAN for FDB, VRF+prefix+prefixlen for routes.
 */

typedef struct 	txn_entry_lock_a {
	hal_txn_table_t		table;
	uint64_t 			key;            /* key (MAC+VLAN or VRF+prefix+len) */
	uint64_t 			owner_txn_id;   /* Transaction ID holding the lock */
	bool 				locked;         /* Lock state: true if currently held */
} txn_entry_lock_t;


typedef struct txn_lock_manager_s {
	txn_entry_lock_t locks[TXN_LOCK_TABLE_SIZE];
	pthread_mutex_t mutex;
} txn_lock_manager_t;


/**
 * Single operation recorded in transaction journal
 */
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


/**
 * Transaction handle - internal structure
 *
 * Returned by hal_txn_begin() and used in subsequent transaction operations.
 * Internal structure is not exposed to users.
 */
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


/**
 * Initialize FDB entry structure with default values
 *
 * Must be called before setting entry fields.
 *
 * @param entry     Pointer to entry structure
 * @param mac       MAC address (can be NULL to clear)
 * @param vlan_id   VLAN ID
 */
void hal_fdb_entry_init(hal_fdb_entry_t *entry,
                        const hal_mac_t mac,
                        hal_vlan_t vlan_id);

/* ============================================================================
 * TXN CRUD Operations
 * ============================================================================ */

/**
 * Begin a new transaction
 *
 * Allocates and initializes a transaction object with the given options.
 * Returns a transaction handle to be used with other transaction functions.
 *
 * Default options (when opts is NULL):
 * - timeout_ms: 0 (no timeout)
 * - auto_rollback: true
 * - lock_strategy: HAL_TXN_LOCK_PESSIMISTIC
 *
 * On success, updates new txn info.
 *
 * @param opts     Transaction options (NULL for defaults)
 * @param txn      Output: Pointer to transaction handle
 * @return         HAL_SUCCESS, HAL_E_PARAM, HAL_E_MEMORY, or error
 */
hal_status_t hal_txn_begin(const hal_txn_opts_t *opts, hal_txn_t **txn);

/**
 * Add an FDB operation to the transaction
 *
 * Records an FDB operation (ADD/DELETE/UPDATE) in the transaction journal.
 * The operation is not applied to the ASIC until hal_txn_commit() is called.
 *
 * @param txn       Transaction handle (must be in PENDING or ACTIVE state)
 * @param op        Operation type (ADD/DELETE/UPDATE)
 * @param entry     FDB entry (must contain valid key fields for DELETE/UPDATE)
 * @return          HAL_SUCCESS, HAL_E_PARAM, or error
 */
hal_status_t hal_txn_add_fdb(hal_txn_t *txn, hal_txn_op_t op,
                             const hal_fdb_entry_t *entry);

/**
 * Add a route operation to the transaction
 *
 * Records a route operation (ADD/DELETE/UPDATE) in the transaction journal.
 * The operation is not applied to the ASIC until hal_txn_commit() is called.
 *
 * @param txn       Transaction handle (must be in PENDING or ACTIVE state)
 * @param op        Operation type (ADD/DELETE/UPDATE)
 * @param entry     Route entry (must contain valid key fields for DELETE/UPDATE)
 * @return          HAL_SUCCESS, HAL_E_PARAM, or error
 */
hal_status_t hal_txn_add_route(hal_txn_t *txn, hal_txn_op_t op,
                               const hal_route_entry_t *entry);


/**
 * Add a route operation to the transaction
 *
 * Atomicity semantics:
 * - If commit succeeds: all operations are persisted, no rollback needed
 * - If commit fails: the operation error is returned and the transaction moves
 *   to FAILED; rollback is attempted automatically when auto_rollback is true
 * - If an inverse operation fails, the transaction remains FAILED and retains
 *   unapplied rollback tracking; the caller must retry rollback/abort
 * - If an inverse operation cannot be completed, atomicity cannot be guaranteed
 *
 * @param txn       Transaction handle (must be in PENDING or ACTIVE state)
 * @return          HAL_SUCCESS, HAL_E_RESOURCE, or error
 *
 * If auto_rollback is false, a failed commit retains locks and unapplied rollback
 * tracking until hal_txn_rollback() or hal_txn_abort() is called.
 */
hal_status_t hal_txn_commit(hal_txn_t *txn);


/**
 * Rollback all applied operations in the transaction
 *
 * Rollback is best-effort: if any inverse operation fails, an error is logged
 * but rollback continues with remaining operations.
 *
 * @param txn       Transaction handle (must be in COMMITTED or FAILED state)
 * @return          HAL_SUCCESS, HAL_E_NULL, or the first inverse-operation error
 *
 * @note Transaction enters ABORTED state after rollback completes successfully.
 *       Resources are released back to pools.
 */
hal_status_t hal_txn_rollback(hal_txn_t *txn);


/**
 * Abort transaction without commit or rollback
 *
 * Cleans up transaction state, releases locks (if any), and frees resources.
 * This is useful for abandoning a transaction that encountered an error
 * but where explicit rollback is not needed.
 *
 * @param txn       Transaction handle (can be in any state except ABORTED)
 * @return          HAL_SUCCESS or error code, including an inverse-operation error
 *                  - HAL_E_NULL: txn is NULL
 *
 * @note Transaction enters ABORTED state after cleanup succeeds.
 *       For ACTIVE or FAILED transactions, attempts to rollback applied ops.
 */
hal_status_t hal_txn_abort(hal_txn_t *txn);


/**
 * Free transaction resources
 *
 * Deallocates the transaction handle and all associated resources.
 * Must not be called on ACTIVE or FAILED transactions—use abort()/rollback()
 * first. PENDING, COMMITTED, and ABORTED handles may be freed.
 *
 * @param txn       Transaction handle (must be in PENDING, COMMITTED, or ABORTED state)
 * @return          HAL_SUCCESS or error code
 *                  - HAL_E_NULL: txn is NULL
 *                  - HAL_E_BUSY: Transaction is still active
 *
 * @note After calling this, txn pointer is invalid and must not be used.
 */
hal_status_t hal_txn_free(hal_txn_t *txn);

/**
 * Get the current state of a transaction
 *
 * @param txn       Transaction handle
 * @return          Current transaction state
 */
hal_txn_state_t hal_txn_get_state(hal_txn_t *txn);

/**
 * Get transaction statistics
 *
 * Fills the provided stats structure with current transaction statistics.
 *
 * @param txn       Transaction handle
 * @param stats     [OUT] Statistics structure
 * @return          HAL_SUCCESS or error code
 *                  - HAL_E_NULL: txn or stats is NULL
 */
hal_status_t hal_txn_get_stats(hal_txn_t *txn, hal_txn_stats_t *stats);

/**
 * Convert transaction state to string representation
 *
 * @param state     Transaction state
 * @return          Human-readable string (never NULL)
 */
const char *hal_txn_state_str(hal_txn_state_t state);


/* ============================================================================
 * Internal Initialization (Called by hal_init)
 * ============================================================================ */

/**
 * Initialize transaction manager with resource pools
 *
 * Called internally by hal_init() to wire transaction manager to
 * resource allocation pools.
 *
 * @param fdb_pool      FDB resource pool
 * @param route_pool    Route resource pool
 * @return              HAL_SUCCESS or error
 */
hal_status_t hal_txn_init(void);

/**
 * Shutdown transaction manager
 *
 * Called internally by hal_shutdown() to clean up transaction resources.
 *
 * @return              HAL_SUCCESS or error
 */
hal_status_t hal_txn_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* HAL_TXN_H */

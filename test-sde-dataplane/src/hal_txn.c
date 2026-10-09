/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 HAL Assessment Project
 *
 * hal_txn.c - Transaction Manager Implementation
 *
 * Implements atomic multi-table transaction support with full rollback.
 * Uses pessimistic locking for conflict prevention and comprehensive
 * error handling with best-effort rollback.
 */

#include "hal_txn.h"
#include "hal_init.h"
#include "hal_resource.h"
#include "asic/asic_driver.h"

#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>

/* ============================================================================
 * Global Transaction State Variables
 * ============================================================================ */
static hal_txn_t		*gp_txn_list	  = NULL;
static pthread_mutex_t 	g_txn_global_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t 		g_txn_next_id 	  = 1;

static txn_lock_manager_t g_txn_lock_mgr = {
    .locks = {0}, 
    .mutex = PTHREAD_MUTEX_INITIALIZER
};

/* ============================================================================
 * Initialization Hook
 * ============================================================================ */

/**
 * Called from hal_init to set up resource pools
 * This is called internally by the HAL layer
 */
hal_status_t hal_txn_init(void)
{
    return HAL_SUCCESS;
}

/* Dummy cleanup function */
hal_status_t hal_txn_shutdown(void)
{
    return HAL_SUCCESS;
}

/* ============================================================================
 * Transaction State Machine
 * ============================================================================ */
static void txn_transition_state(hal_txn_t *txn, hal_txn_state_t new_state)
{
    if (txn) {
        txn->state = new_state;
    }
}


/* ============================================================================
 * Timeout Checking
 * ============================================================================ */

static bool txn_check_timeout(hal_txn_t *txn)
{
    if (!txn || txn->opts.timeout_ms == 0) {
        return false;
    }

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    uint64_t now_ms = (uint64_t)now.tv_sec * 1000UL + now.tv_nsec / 1000000;
    uint64_t elapsed_ms = now_ms - txn->start_time;
    
    return elapsed_ms > txn->opts.timeout_ms;
}

/* ============================================================================
 * Ensures entry capacity dynamically grows to TXN_MAX_ENTRY_CAPACITY
 * ============================================================================ */

static hal_status_t txn_check_and_recalc_capacity(hal_txn_t *txn)
{
    if (!txn) return HAL_E_NULL;

    if (txn->entry_count < txn->entry_capacity) {
        return HAL_SUCCESS; /* Capacity sufficient */
    }
    
    /* Need to grow */
    size_t new_capacity = txn->entry_capacity > 0 ?
								txn->entry_capacity * 2 : TXN_INIT_ENTRY_CAPACITY;
    if (new_capacity > TXN_MAX_ENTRY_CAPACITY) {
        new_capacity = TXN_MAX_ENTRY_CAPACITY;
    }
    
    if (new_capacity <= txn->entry_capacity) {
        return HAL_E_FULL;  /* Already at max capacity */
    }
    
    hal_txn_op_entry_t *new_entries = realloc(txn->entries,
                                              new_capacity * sizeof(hal_txn_op_entry_t));
    if (!new_entries) {
        return HAL_E_MEMORY;
    }
    
    txn->entries = new_entries;
    txn->entry_capacity = new_capacity;
    return HAL_SUCCESS;
}


static txn_entry_lock_t* find_entry_lock(hal_txn_table_t table, uint64_t key)
{
	for (int i = 0; i < TXN_LOCK_TABLE_SIZE; i++)
	{
		if (g_txn_lock_mgr.locks[i].locked &&
			g_txn_lock_mgr.locks[i].table == table &&
			g_txn_lock_mgr.locks[i].key == key)
		{
			return &g_txn_lock_mgr.locks[i];
		}
	}

    return NULL;
}

static hal_status_t allocate_entry_lock(hal_txn_table_t table, uint64_t key,
											 uint32_t txn_id, uint32_t *lock_idx)
{
	txn_entry_lock_t *new_lock = NULL;

    for (int i = 0; i < TXN_LOCK_TABLE_SIZE; i++)
    {
        if (!g_txn_lock_mgr.locks[i].locked)
        {
            new_lock = &g_txn_lock_mgr.locks[i];
			*lock_idx = i;
			break;
        }
    }

    if (!new_lock)
    {
        return HAL_E_MEMORY;
    }
    new_lock->table = table;
    new_lock->key = key;
    new_lock->owner_txn_id = txn_id;
    new_lock->locked = true;

    return HAL_SUCCESS;
}

static void free_entry_lock(uint32_t lock_idx, uint32_t txn_id)
{
	pthread_mutex_lock(&g_txn_lock_mgr.mutex);

	txn_entry_lock_t *new_lock = &g_txn_lock_mgr.locks[lock_idx];

	if (txn_id == new_lock->owner_txn_id){
		memset(new_lock, 0, sizeof(txn_entry_lock_t));
	}

    pthread_mutex_unlock(&g_txn_lock_mgr.mutex);
}

static void txn_release_locks(hal_txn_t *txn)
{
    if (!txn) {
        return;
    }

    for (size_t i = 0; i < txn->entry_count; i++) {
        if (txn->entries[i].locked) {
            free_entry_lock(txn->entries[i].lock_index, (uint32_t)txn->id);
            txn->entries[i].locked = false;
        }
    }
}


uint64_t get_fdb_key (const hal_fdb_entry_t *entry)
{
    uint64_t key = 0;

    if (!entry) {
		return HAL_E_NULL;
	}

	/* Compose Key from MAC address (48-bit) + VLAN (12-bit) */
    for (int i = 0; i < 6; i++) {
        key = (key << 8) | entry->mac[i];
    }

    key <<= 16;
    key |= (entry->vlan_id & 0x0FFF);  /* Mask to standard 12-bit VLAN */
	
	return key;
}

uint64_t get_route_key (const hal_route_entry_t *entry)
{
     uint64_t key = 0;

    if (!entry) {
		return HAL_E_NULL;
	}

    key |= ((uint64_t)entry->vrf_id << 48);   /* VRF in high bits */
    key |= ((uint64_t)entry->prefix << 16);   /* Prefix in middle */
    key |= ((uint64_t)entry->prefix_len);     /* Prefix length in low bits */

    return key;
}

/**
 * Acquire lock for FDB entry
 */
static hal_status_t lock_acquire_fdb(const hal_fdb_entry_t *entry,
                                     uint32_t txn_id, uint32_t *lock_idx)
{
	txn_entry_lock_t *cur_lock 	= NULL;
    uint64_t 		  key 		= 0;
	hal_status_t	  ret_val 	= HAL_SUCCESS;

    if (!entry || !lock_idx) {
		return HAL_E_NULL;
	}

    key = get_fdb_key(entry);

	/* Find current entry lock */
	pthread_mutex_lock(&g_txn_lock_mgr.mutex);

	cur_lock = find_entry_lock(HAL_TXN_TABLE_FDB, key);
    if (cur_lock)
    {
        pthread_mutex_unlock(&g_txn_lock_mgr.mutex);
        return HAL_E_BUSY;
    }

	ret_val = allocate_entry_lock(HAL_TXN_TABLE_FDB, key, txn_id, lock_idx);
    if (ret_val != HAL_SUCCESS) {
        pthread_mutex_unlock(&g_txn_lock_mgr.mutex);
        return ret_val;
    }

    pthread_mutex_unlock(&g_txn_lock_mgr.mutex);
    return HAL_SUCCESS;
}


/**
 * Acquire lock for FDB entry
 */
static hal_status_t lock_acquire_route(const hal_route_entry_t *entry,
                                     uint32_t txn_id, uint32_t *lock_idx)
{
	txn_entry_lock_t *cur_lock 	= NULL;
    uint64_t 		  key 		= 0;
	hal_status_t	  ret_val 	= HAL_SUCCESS;

    if (!entry || !lock_idx) {
		return HAL_E_NULL;
	}

    key = get_route_key(entry);

	/* Find current entry lock */
	pthread_mutex_lock(&g_txn_lock_mgr.mutex);

	cur_lock = find_entry_lock(HAL_TXN_TABLE_ROUTE, key);
    if (cur_lock)
    {
        pthread_mutex_unlock(&g_txn_lock_mgr.mutex);
        return HAL_E_BUSY;
    }

	ret_val = allocate_entry_lock(HAL_TXN_TABLE_ROUTE, key, txn_id, lock_idx);
    if (ret_val != HAL_SUCCESS) {
        pthread_mutex_unlock(&g_txn_lock_mgr.mutex);
        return ret_val;
    }

    pthread_mutex_unlock(&g_txn_lock_mgr.mutex);
    return HAL_SUCCESS;
}

static void hal_txn_unreserve (hal_txn_t *txn)
{
	hal_resource_pool_t *fdb_pool 	= hal_resource_get_pool(HAL_RESOURCE_FDB_ENTRY);
	hal_resource_pool_t *route_pool = hal_resource_get_pool(HAL_RESOURCE_ROUTE_ENTRY);

    if (txn->fdb_reserved > 0 && fdb_pool) {
        hal_resource_unreserve(fdb_pool, txn->fdb_reserved);
        txn->fdb_reserved = 0;
    }

    if (txn->route_reserved > 0 && route_pool) {
        hal_resource_unreserve(route_pool, txn->route_reserved);
        txn->route_reserved = 0;
    }
}


static hal_status_t hal_txn_reserve (hal_txn_t *txn)
{
	uint32_t fdb_adds   = txn->stats.fdb_adds;
	uint32_t route_adds = txn->stats.route_adds;

    hal_status_t 		rv 			= HAL_SUCCESS;
	hal_resource_pool_t *fdb_pool 	= hal_resource_get_pool(HAL_RESOURCE_FDB_ENTRY);
	hal_resource_pool_t *route_pool = hal_resource_get_pool(HAL_RESOURCE_ROUTE_ENTRY);

    if (fdb_adds > 0 && fdb_pool) {
        rv = hal_resource_reserve(fdb_pool, fdb_adds);
        if (rv != HAL_SUCCESS) {
            hal_txn_unreserve(txn);  /* Rollback on failure */
			return rv;
        }
        txn->fdb_reserved = fdb_adds;
        txn->stats.resources_reserved += fdb_adds;
    }
    
    if (route_adds > 0 && route_pool) {
        rv = hal_resource_reserve(route_pool, route_adds);
        if (rv != HAL_SUCCESS) {
            hal_txn_unreserve(txn);  /* Rollback on failure */
			return rv;
        }
        txn->route_reserved = route_adds;
        txn->stats.resources_reserved += route_adds;
    }
    
    return HAL_SUCCESS;
}

/* ============================================================================
 * Rollback Operations
 * ============================================================================ */

static hal_status_t rollback_fdb_entry(const hal_txn_op_entry_t *op_entry)
{
    hal_status_t rv;

	if (!op_entry) {
		return HAL_E_NULL;
	}
    
    switch (op_entry->op) {
        case HAL_TXN_OP_ADD:
            /* Reverse: delete the entry */
            rv = hal_fdb_delete(op_entry->entry.fdb.mac, op_entry->entry.fdb.vlan_id);
            break;
            
        case HAL_TXN_OP_DELETE:
            /* Reverse: add back from snapshot */
            rv = hal_fdb_add((hal_fdb_entry_t *)&op_entry->original);
            break;
            
        case HAL_TXN_OP_UPDATE:
            /* Reverse: restore original */
            rv = hal_fdb_update((hal_fdb_entry_t *)&op_entry->original);
            break;
            
        default:
            rv = HAL_E_PARAM;
    }

    if (rv != HAL_SUCCESS) {
        /* Log error */
    }
    
    return rv;
}

static hal_status_t rollback_route_entry(const hal_txn_op_entry_t *op_entry)
{
    hal_status_t rv;
    
	if (!op_entry) {
		return HAL_E_NULL;
	}
    
    switch (op_entry->op) {
        case HAL_TXN_OP_ADD:
            /* Reverse: delete the entry */
            rv = hal_route_delete(op_entry->entry.route.vrf_id,
                                  op_entry->entry.route.prefix,
                                  op_entry->entry.route.prefix_len);
            break;
            
        case HAL_TXN_OP_DELETE:
            /* Reverse: add back from snapshot */
            rv = hal_route_add((hal_route_entry_t *)&op_entry->original);
            break;
            
        case HAL_TXN_OP_UPDATE:
            /* Reverse: restore original */
            rv = hal_route_update((hal_route_entry_t *)&op_entry->original);
            break;
            
        default:
            rv = HAL_E_PARAM;
    }

    if (rv != HAL_SUCCESS) {
        /* Log error */
    }
    
    return rv;
}

static hal_status_t txn_do_rollback(hal_txn_t *txn)
{
    if (!txn) {
		return HAL_E_NULL;
	}
    
    /* Rollback in reverse order */
    for (int i = (int)txn->applied_count - 1; i >= 0; i--) {
        hal_txn_op_entry_t *op_entry = &txn->entries[i];

        if (!op_entry->applied) {
            continue;
        }
        
        hal_status_t rv = HAL_SUCCESS;
        
        switch (op_entry->table) {
            case HAL_TXN_TABLE_FDB:
                rv = rollback_fdb_entry(op_entry);
                break;
            case HAL_TXN_TABLE_ROUTE:
                rv = rollback_route_entry(op_entry);
                break;
            default:
                rv = HAL_E_PARAM;
        }
        
        if (rv == HAL_SUCCESS) {
			op_entry->applied = false;
			txn->applied_count--;
		}
    }

    txn->applied_count = 0;
    return HAL_SUCCESS;
}


static hal_status_t apply_fdb_operation(const hal_txn_op_entry_t *op_entry)
{
    if (!op_entry) {
		return HAL_E_NULL;
	}

    switch (op_entry->op) {
        case HAL_TXN_OP_ADD:
            return hal_fdb_add((hal_fdb_entry_t *)&op_entry->entry.fdb);
        case HAL_TXN_OP_DELETE:
            return hal_fdb_delete(op_entry->entry.fdb.mac, op_entry->entry.fdb.vlan_id);
        case HAL_TXN_OP_UPDATE:
            return hal_fdb_update((hal_fdb_entry_t *)&op_entry->entry.fdb);
        default:
            return HAL_E_PARAM;
    }
}

static hal_status_t apply_route_operation(const hal_txn_op_entry_t *op_entry)
{
    if (!op_entry) {
		return HAL_E_NULL;
	}

    switch (op_entry->op) {
        case HAL_TXN_OP_ADD:
            return hal_route_add((hal_route_entry_t *)&op_entry->entry.route);
        case HAL_TXN_OP_DELETE:
            return hal_route_delete(op_entry->entry.route.vrf_id,
                                    op_entry->entry.route.prefix,
                                    op_entry->entry.route.prefix_len);
        case HAL_TXN_OP_UPDATE:
            return hal_route_update((hal_route_entry_t *)&op_entry->entry.route);
        default:
            return HAL_E_PARAM;
    }
}


/* ============================================================================
 * Public Transaction API
 * ============================================================================ */

hal_status_t hal_txn_begin(const hal_txn_opts_t *opts, hal_txn_t **txn)
{
    if (!txn) {
		return HAL_E_NULL;
	}

    hal_txn_t *new_txn = calloc(1, sizeof(hal_txn_t));
    if (!new_txn) {
        return HAL_E_MEMORY;
    }
    
    /* Allocate initial journal capacity */
    new_txn->entries = malloc(TXN_INIT_ENTRY_CAPACITY * sizeof(hal_txn_op_entry_t));
    if (!new_txn->entries) {
        free(new_txn);
        return HAL_E_MEMORY;
    }
    
    /* Get current time for timeout tracking */
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    new_txn->start_time = (uint64_t)ts.tv_sec * 1000UL + ts.tv_nsec / 1000000;
    
    /* Assign unique transaction ID */
    pthread_mutex_lock(&g_txn_global_lock);
    new_txn->id = g_txn_next_id++;
    pthread_mutex_unlock(&g_txn_global_lock);
    
    /* Set options */
    if (opts) {
        new_txn->opts = *opts;
    } else {
        new_txn->opts.timeout_ms 	= TXN_DEFAULT_TIMEOUT_MS;
        new_txn->opts.auto_rollback = true;
    }
    
    new_txn->entry_capacity = TXN_INIT_ENTRY_CAPACITY;
    new_txn->entry_count 	= 0;
    new_txn->state 			= HAL_TXN_STATE_PENDING;

	/* Add transaction to TXN list */
    pthread_mutex_lock(&g_txn_global_lock);
	if (gp_txn_list != NULL) {
		gp_txn_list->prev = new_txn;
	}
	new_txn->next 	= gp_txn_list;
	gp_txn_list 	= new_txn;
    pthread_mutex_unlock(&g_txn_global_lock);

    *txn = new_txn;
    return HAL_SUCCESS;
}


hal_status_t hal_txn_add_fdb(hal_txn_t *txn, hal_txn_op_t op,
                             const hal_fdb_entry_t *entry)
{
    if (!txn || !entry) {
		return HAL_E_NULL;
	}
    
    /* Check timeout */
    if (txn_check_timeout(txn)) {
        return HAL_E_TIMEOUT;
    }
    
    /* Validate state */
    if (txn->state != HAL_TXN_STATE_PENDING &&
        txn->state != HAL_TXN_STATE_ACTIVE) {
        return HAL_E_FAIL;
    }
    
    /* Validate operation */
    if (op < HAL_TXN_OP_ADD || op > HAL_TXN_OP_UPDATE) {
        return HAL_E_PARAM;
    }
    
    /* Ensure capacity */
    hal_status_t rv = txn_check_and_recalc_capacity(txn);
    if (rv != HAL_SUCCESS) {
        return rv;
    }
    
    uint32_t lock_idx = 0;
    rv = lock_acquire_fdb(entry, (uint32_t)txn->id, &lock_idx);
    if (rv != HAL_SUCCESS) {
        return rv;
    }

    hal_txn_op_entry_t *op_entry = &txn->entries[txn->entry_count];
    memset(op_entry, 0, sizeof(*op_entry));

    op_entry->op = op;
    op_entry->table = HAL_TXN_TABLE_FDB;
    op_entry->entry.fdb = *entry;
    op_entry->lock_index = lock_idx;
    op_entry->locked = true;

    /* For UPDATE/DELETE ops, take snapshot of entry first */
    if ((op == HAL_TXN_OP_UPDATE) || (op == HAL_TXN_OP_DELETE)) {
		hal_fdb_entry_t old_entry;
        rv = hal_fdb_get(entry->mac, entry->vlan_id, &old_entry);
		if (rv != HAL_SUCCESS) {
			free_entry_lock(lock_idx, (uint32_t)txn->id);
			op_entry->locked = false;
			return rv;
		}
        op_entry->original.fdb = old_entry;
    }

    txn->entry_count++;

    /* Update stats */
    txn->stats.total_operations++;
    if (op == HAL_TXN_OP_ADD) {
        txn->stats.fdb_adds++;
    } else if (op == HAL_TXN_OP_DELETE) {
        txn->stats.fdb_deletes++;
    } else {
        txn->stats.fdb_updates++;
    }

    /* Transition to ACTIVE if first operation */
    if (txn->state == HAL_TXN_STATE_PENDING) {
        txn_transition_state(txn, HAL_TXN_STATE_ACTIVE);
    }
    
    return HAL_SUCCESS;
}


hal_status_t hal_txn_add_route(hal_txn_t *txn, hal_txn_op_t op,
                               const hal_route_entry_t *entry)
{
    if (!txn || !entry) {
		return HAL_E_NULL;
	}
    
    /* Check timeout */
    if (txn_check_timeout(txn)) {
        return HAL_E_TIMEOUT;
    }
    
    /* Validate state */
    if (txn->state != HAL_TXN_STATE_PENDING &&
        txn->state != HAL_TXN_STATE_ACTIVE) {
        return HAL_E_FAIL;
    }
    
    /* Validate operation */
    if (op < HAL_TXN_OP_ADD || op > HAL_TXN_OP_UPDATE) {
        return HAL_E_PARAM;
    }
    
    /* Ensure capacity */
    hal_status_t rv = txn_check_and_recalc_capacity(txn);
    if (rv != HAL_SUCCESS) {
        return rv;
    }
    
    uint32_t lock_idx = 0;
    rv = lock_acquire_route(entry, (uint32_t)txn->id, &lock_idx);
    if (rv != HAL_SUCCESS) {
        return rv;
    }
    
    /* Record operation */
    hal_txn_op_entry_t *op_entry = &txn->entries[txn->entry_count];
    memset(op_entry, 0, sizeof(*op_entry));
    
    op_entry->op = op;
    op_entry->table = HAL_TXN_TABLE_ROUTE;
    op_entry->entry.route = *entry;
    op_entry->lock_index = lock_idx;
    op_entry->locked = true;
    
    if ((op == HAL_TXN_OP_UPDATE) || (op == HAL_TXN_OP_DELETE)) {
		hal_route_entry_t old_entry; 
		rv = hal_route_get(entry->vrf_id,
						   entry->prefix,
						   entry->prefix_len,
						   &old_entry);
		if (rv != HAL_SUCCESS) {
			free_entry_lock(lock_idx, (uint32_t)txn->id);
			op_entry->locked = false;
			return rv;
		}
        op_entry->original.route = old_entry;
    }

    txn->entry_count++;

    /* Update stats */
    txn->stats.total_operations++;
    if (op == HAL_TXN_OP_ADD) {
        txn->stats.route_adds++;
    } else if (op == HAL_TXN_OP_DELETE) {
        txn->stats.route_deletes++;
    } else {
        txn->stats.route_updates++;
    }
    
    /* Transition to ACTIVE if first operation */
    if (txn->state == HAL_TXN_STATE_PENDING) {
        txn_transition_state(txn, HAL_TXN_STATE_ACTIVE);
    }
    
    return HAL_SUCCESS;
}



hal_status_t hal_txn_commit(hal_txn_t *txn)
{
    hal_status_t rv = HAL_SUCCESS;

    if (!txn) {
		return HAL_E_NULL;
	}
    
    if (txn->state != HAL_TXN_STATE_PENDING &&
        txn->state != HAL_TXN_STATE_ACTIVE) {
        return HAL_E_FAIL;
    }

    txn_transition_state(txn, HAL_TXN_STATE_ACTIVE);
    
    /* Step 1: Count & Reserve resource requirements */
	rv = hal_txn_reserve(txn);
	if (rv != HAL_SUCCESS) {
		hal_txn_unreserve(txn);
		return rv;
	}

    /* Step 2: Apply operations in order */
    for (size_t i = 0; i < txn->entry_count; i++) {
        hal_txn_op_entry_t *op_entry = &txn->entries[i];

		switch(op_entry->table) {
			case HAL_TXN_TABLE_FDB:
				rv = apply_fdb_operation(op_entry);
				break;
			case HAL_TXN_TABLE_ROUTE:
				rv = apply_route_operation(op_entry);
				break;
			default:
				rv = HAL_E_PARAM;
				break;
		}
        
        if (rv != HAL_SUCCESS) {
            /* Operation failed - rollback applied operations */
            txn->applied_count = i;
            
            if (txn->opts.auto_rollback) {
                txn_do_rollback(txn);
            }
            txn_release_locks(txn);
            hal_txn_unreserve(txn);
		txn_transition_state(txn, HAL_TXN_STATE_FAILED);
		return rv;
        }
        op_entry->applied = true;
        txn->applied_count++;
    }
    
    /* Step 3: Mark resources as allocated */
    txn_release_locks(txn);
	hal_txn_unreserve(txn);

    txn_transition_state(txn, HAL_TXN_STATE_COMMITTED);
    return HAL_SUCCESS;
}


hal_status_t hal_txn_rollback(hal_txn_t *txn)
{
    if (!txn) return HAL_E_NULL;
    
    if (txn->state != HAL_TXN_STATE_COMMITTED && 
		txn->state != HAL_TXN_STATE_FAILED) {
        return HAL_E_FAIL;
    }

    if (txn->applied_count > 0) {
        txn_do_rollback(txn);
    }
    
    txn_release_locks(txn);
    hal_txn_unreserve(txn);

    txn_transition_state(txn, HAL_TXN_STATE_ABORTED);
    return HAL_SUCCESS;
}


hal_status_t hal_txn_abort(hal_txn_t *txn)
{
    if (!txn) {
		return HAL_E_NULL;
	}
    
    /* Already aborted transaction */
    if (txn->state == HAL_TXN_STATE_ABORTED) {
        return HAL_SUCCESS;
    }

    /* Allow cleanup after a failed commit as well as pre-commit cleanup */
    if (txn->state != HAL_TXN_STATE_PENDING && 
		txn->state != HAL_TXN_STATE_FAILED &&
        txn->state != HAL_TXN_STATE_ACTIVE) {
        return HAL_E_FAIL;
    }

    if (txn->applied_count > 0) {
        txn_do_rollback(txn);
    }

    txn_release_locks(txn);
    hal_txn_unreserve(txn);

    txn_transition_state(txn, HAL_TXN_STATE_ABORTED);
    return HAL_SUCCESS;
}

hal_status_t hal_txn_free(hal_txn_t *txn)
{
    if (!txn) return HAL_E_NULL;

	if (txn->state != HAL_TXN_STATE_PENDING &&
		txn->state != HAL_TXN_STATE_COMMITTED &&
		txn->state != HAL_TXN_STATE_ABORTED &&
		txn->state != HAL_TXN_STATE_FAILED) {
		return HAL_E_BUSY;
	}

    txn_release_locks(txn);

    if (txn->entries) {
        free(txn->entries);
    }

    /* Remove transaction from TXN list */
    pthread_mutex_lock(&g_txn_global_lock);
    if (txn->prev != NULL) {
        txn->prev->next = txn->next;
    } else if (gp_txn_list == txn) {
        gp_txn_list = txn->next;
    }
    if (txn->next != NULL) {
        txn->next->prev = txn->prev;
    }
    pthread_mutex_unlock(&g_txn_global_lock);

    free(txn);
    return HAL_SUCCESS;
}


/* ============================================================================
 * Get Functions
 * ============================================================================ */

hal_txn_state_t hal_txn_get_state(hal_txn_t *txn)
{
    if (!txn) {
		return HAL_TXN_STATE_INVALID;
	}
    return txn->state;
}

hal_status_t hal_txn_get_stats(hal_txn_t *txn, hal_txn_stats_t *stats)
{
    if (!txn || !stats) {
		return HAL_E_NULL;
	}
    
    *stats = txn->stats;
    return HAL_SUCCESS;
}

/* ============================================================================
 * String Conversion Functions
 * ============================================================================ */

const char *hal_txn_state_str(hal_txn_state_t state)
{
    switch (state) {
        case HAL_TXN_STATE_PENDING:   return "PENDING";
        case HAL_TXN_STATE_ACTIVE:    return "ACTIVE";
        case HAL_TXN_STATE_COMMITTED: return "COMMITTED";
        case HAL_TXN_STATE_ABORTED:   return "ABORTED";
        case HAL_TXN_STATE_FAILED:    return "FAILED";
        default:                      return "UNKNOWN";
    }
}

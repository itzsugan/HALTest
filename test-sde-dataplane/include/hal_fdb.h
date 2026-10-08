/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2024 HAL Assessment Project
 *
 * hal_fdb.h - L2 Forwarding Database (FDB) API
 *
 * The FDB (also known as MAC address table) stores the association between
 * MAC addresses, VLANs, and the ports where those addresses were learned
 * or statically configured.
 */

#ifndef HAL_FDB_H
#define HAL_FDB_H

#include "hal_types.h"
#include "hal_error.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * FDB Entry Structure
 * ============================================================================ */

/**
 * FDB entry structure
 *
 * Represents a single entry in the L2 forwarding database.
 * Key: (mac, vlan_id)
 */
typedef struct hal_fdb_entry_s {
    hal_mac_t       mac;            /* MAC address (key) */
    hal_vlan_t      vlan_id;        /* VLAN ID (key) */
    hal_port_t      port;           /* Destination port */
    hal_flags_t     flags;          /* Entry flags (static, hit, etc.) */
    uint32_t        age;            /* Current age in seconds (read-only) */
    hal_object_id_t object_id;      /* ASIC object handle (read-only) */
} hal_fdb_entry_t;

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
 * FDB CRUD Operations
 * ============================================================================ */

/**
 * Add FDB entry
 *
 * Adds a new entry to the FDB. The entry key is (mac, vlan_id).
 * If the entry already exists, returns HAL_E_EXISTS.
 *
 * Required fields:
 * - mac: Destination MAC address
 * - vlan_id: VLAN ID
 * - port: Destination port
 *
 * Optional fields:
 * - flags: HAL_FLAG_STATIC to prevent aging
 *
 * On success, entry->object_id is populated with the ASIC handle.
 *
 * @param entry     Entry to add (modified on success)
 * @return          HAL_SUCCESS, HAL_E_EXISTS, HAL_E_FULL, or error
 */
hal_status_t hal_fdb_add(hal_fdb_entry_t *entry);

/**
 * Delete FDB entry by key
 *
 * @param mac       MAC address
 * @param vlan_id   VLAN ID
 * @return          HAL_SUCCESS, HAL_E_NOT_FOUND, or error
 */
hal_status_t hal_fdb_delete(const hal_mac_t mac, hal_vlan_t vlan_id);

/**
 * Get FDB entry by key
 *
 * @param mac       MAC address
 * @param vlan_id   VLAN ID
 * @param entry     Output: populated entry structure
 * @return          HAL_SUCCESS, HAL_E_NOT_FOUND, or error
 */
hal_status_t hal_fdb_get(const hal_mac_t mac,
                         hal_vlan_t vlan_id,
                         hal_fdb_entry_t *entry);

/**
 * Update FDB entry
 *
 * Updates an existing entry. The key (mac, vlan_id) must match
 * an existing entry. Only non-key fields can be modified.
 *
 * @param entry     Entry with updated fields
 * @return          HAL_SUCCESS, HAL_E_NOT_FOUND, or error
 */
hal_status_t hal_fdb_update(const hal_fdb_entry_t *entry);

/* ============================================================================
 * FDB Bulk Operations
 * ============================================================================ */

/**
 * Delete all FDB entries by port
 *
 * Removes all (non-static) entries associated with a port.
 * Use HAL_FLAG_STATIC in flags to include static entries.
 *
 * @param port      Port to match
 * @param flags     Match flags (0 = non-static only)
 * @return          HAL_SUCCESS or error
 */
hal_status_t hal_fdb_delete_by_port(hal_port_t port, uint32_t flags);

/**
 * Delete all FDB entries by VLAN
 *
 * @param vlan_id   VLAN ID to match
 * @param flags     Match flags (0 = non-static only)
 * @return          HAL_SUCCESS or error
 */
hal_status_t hal_fdb_delete_by_vlan(hal_vlan_t vlan_id, uint32_t flags);

/**
 * Flush all dynamic (non-static) FDB entries
 *
 * @return          HAL_SUCCESS or error
 */
hal_status_t hal_fdb_flush_dynamic(void);

/* ============================================================================
 * FDB Traversal
 * ============================================================================ */

/**
 * FDB traverse callback
 *
 * @param entry     Current entry (read-only)
 * @param user_data User context from hal_fdb_traverse
 * @return          0 to continue, non-zero to stop
 */
typedef int (*hal_fdb_traverse_cb_t)(const hal_fdb_entry_t *entry,
                                     void *user_data);

/**
 * Traverse all FDB entries
 *
 * Calls the callback for each entry in the FDB.
 * The traversal order is undefined.
 *
 * @param callback  Function to call for each entry
 * @param user_data User context passed to callback
 * @return          HAL_SUCCESS or error
 */
hal_status_t hal_fdb_traverse(hal_fdb_traverse_cb_t callback,
                              void *user_data);

/* ============================================================================
 * FDB Statistics
 * ============================================================================ */

/**
 * FDB statistics structure
 */
typedef struct hal_fdb_stats_s {
    uint32_t count;             /* Current entry count */
    uint32_t capacity;          /* Maximum capacity */
    uint32_t static_count;      /* Number of static entries */
    uint32_t dynamic_count;     /* Number of dynamic entries */
} hal_fdb_stats_t;

/**
 * Get FDB statistics
 *
 * @param stats     Output: statistics structure
 * @return          HAL_SUCCESS or error
 */
hal_status_t hal_fdb_stats_get(hal_fdb_stats_t *stats);

/* ============================================================================
 * FDB Aging Configuration
 *
 * NOTE: Aging implementation is the candidate's task for the SE track.
 * The APIs below are defined but not implemented in the starter code.
 * ============================================================================ */

/**
 * Aging callback - invoked when an entry is about to be aged out
 *
 * @param entry     Entry being aged out
 * @param context   User context from registration
 */
typedef void (*hal_fdb_age_cb_t)(const hal_fdb_entry_t *entry, void *context);

/**
 * Register aging callback
 *
 * Register a callback to be notified before entries are aged out.
 * Multiple callbacks can be registered (up to 8).
 *
 * @param callback  Callback function
 * @param context   User context passed to callback
 * @return          HAL_SUCCESS, HAL_E_FULL, or error
 */
hal_status_t hal_fdb_age_callback_register(hal_fdb_age_cb_t callback,
                                           void *context);

/**
 * Unregister aging callback
 *
 * @param callback  Previously registered callback
 * @return          HAL_SUCCESS, HAL_E_NOT_FOUND, or error
 */
hal_status_t hal_fdb_age_callback_unregister(hal_fdb_age_cb_t callback);

/**
 * Set FDB aging time
 *
 * @param aging_time_sec    Aging time in seconds (0 to disable)
 * @return                  HAL_SUCCESS or error
 */
hal_status_t hal_fdb_aging_set(uint32_t aging_time_sec);

/**
 * Get FDB aging time
 *
 * @param aging_time_sec    Output: current aging time
 * @return                  HAL_SUCCESS or error
 */
hal_status_t hal_fdb_aging_get(uint32_t *aging_time_sec);

/**
 * Set aging scan interval
 *
 * Controls how often the aging thread scans the FDB table.
 * Default: 10 seconds
 *
 * @param interval_sec      Scan interval in seconds
 * @return                  HAL_SUCCESS or error
 */
hal_status_t hal_fdb_aging_interval_set(uint32_t interval_sec);

/**
 * Get aging scan interval
 *
 * @param interval_sec      Output: current scan interval
 * @return                  HAL_SUCCESS or error
 */
hal_status_t hal_fdb_aging_interval_get(uint32_t *interval_sec);

/**
 * Start the aging thread
 *
 * Begins background aging of dynamic FDB entries.
 * Must be called after HAL initialization.
 *
 * @return          HAL_SUCCESS or error
 */
hal_status_t hal_fdb_aging_start(void);

/**
 * Stop the aging thread
 *
 * Stops background aging. Safe to call even if not started.
 *
 * @return          HAL_SUCCESS or error
 */
hal_status_t hal_fdb_aging_stop(void);

/**
 * Shutdown FDB subsystem
 *
 * Frees all entries and resets state. Called by hal_shutdown().
 */
void hal_fdb_shutdown(void);

/**
 * Check if aging is running
 *
 * @return          true if aging thread is active
 */
bool hal_fdb_aging_is_running(void);

#ifdef __cplusplus
}
#endif

#endif /* HAL_FDB_H */

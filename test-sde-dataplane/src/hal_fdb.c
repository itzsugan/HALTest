/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2024 HAL Assessment Project
 *
 * hal_fdb.c - L2 Forwarding Database Implementation
 *
 * This file implements the FDB (MAC address table) management.
 *
 * NOTE FOR CANDIDATES (SE Track):
 * The aging functionality (hal_fdb_aging_*) is NOT implemented.
 * Your task is to implement the FDB aging mechanism.
 * See docs/TASK_SE.md for requirements.
 */

#include "hal_fdb.h"
#include "hal_init.h"
#include "hal_resource.h"
#include "asic/asic_driver.h"

#include <stdlib.h>
#include <string.h>
#include <pthread.h>

/* ============================================================================
 * Internal Data Structures
 * ============================================================================ */

/* Software FDB entry (mirrors ASIC state) */
typedef struct fdb_sw_entry_s {
    bool            valid;
    hal_fdb_entry_t entry;
    hal_time_us_t   last_update;    /* Last update timestamp */
    struct fdb_sw_entry_s *next;    /* Hash chain */
} fdb_sw_entry_t;

/* FDB hash table */
#define FDB_HASH_SIZE   4096

typedef struct fdb_table_s {
    pthread_rwlock_t lock;
    fdb_sw_entry_t  *buckets[FDB_HASH_SIZE];
    uint32_t         count;
    uint32_t         static_count;
} fdb_table_t;

/* Aging configuration */
typedef struct fdb_aging_config_s {
    uint32_t aging_time_sec;        /* Entry aging time (0 = disabled) */
    uint32_t scan_interval_sec;     /* Aging scan interval */
    bool     running;               /* Aging thread running */
    pthread_t thread;               /* Aging thread handle */
    pthread_mutex_t mutex;          /* Protects running state */
    pthread_cond_t cond;            /* For signaling stop */
} fdb_aging_config_t;

/* Aging callbacks */
#define FDB_MAX_AGE_CALLBACKS   8

typedef struct fdb_age_callback_s {
    hal_fdb_age_cb_t callback;
    void            *context;
} fdb_age_callback_t;

/* Global FDB state */
static fdb_table_t g_fdb_table;
static fdb_aging_config_t g_fdb_aging = {
    .aging_time_sec = 300,          /* Default: 5 minutes */
    .scan_interval_sec = 10,        /* Default: 10 seconds */
    .running = false,
};
static fdb_age_callback_t g_fdb_callbacks[FDB_MAX_AGE_CALLBACKS];
static int g_fdb_callback_count = 0;
static pthread_mutex_t g_fdb_callback_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_fdb_init_lock = PTHREAD_MUTEX_INITIALIZER;
static bool g_fdb_initialized = false;

/* ============================================================================
 * Internal Helpers
 * ============================================================================ */

/* Hash function for MAC+VLAN */
static uint32_t fdb_hash(const hal_mac_t mac, hal_vlan_t vlan)
{
    uint32_t hash = 0;
    hash = (hash * 31) + mac[0];
    hash = (hash * 31) + mac[1];
    hash = (hash * 31) + mac[2];
    hash = (hash * 31) + mac[3];
    hash = (hash * 31) + mac[4];
    hash = (hash * 31) + mac[5];
    hash = (hash * 31) + vlan;
    return hash % FDB_HASH_SIZE;
}

/* Initialize FDB subsystem (called from hal_init) */
static void fdb_free_all_entries(void)
{
    /* Free all entries in hash table */
    for (int i = 0; i < FDB_HASH_SIZE; i++) {
        fdb_sw_entry_t *e = g_fdb_table.buckets[i];
        while (e) {
            fdb_sw_entry_t *next = e->next;
            free(e);
            e = next;
        }
        g_fdb_table.buckets[i] = NULL;
    }
    g_fdb_table.count = 0;
    g_fdb_table.static_count = 0;
}

static hal_status_t fdb_init(void)
{
    pthread_mutex_lock(&g_fdb_init_lock);

    if (g_fdb_initialized) {
        pthread_mutex_unlock(&g_fdb_init_lock);
        return HAL_SUCCESS;
    }

    pthread_rwlock_init(&g_fdb_table.lock, NULL);
    memset(g_fdb_table.buckets, 0, sizeof(g_fdb_table.buckets));
    g_fdb_table.count = 0;
    g_fdb_table.static_count = 0;

    pthread_mutex_init(&g_fdb_aging.mutex, NULL);
    pthread_cond_init(&g_fdb_aging.cond, NULL);

    g_fdb_initialized = true;
    pthread_mutex_unlock(&g_fdb_init_lock);
    return HAL_SUCCESS;
}

/* Shutdown FDB subsystem - called from hal_shutdown() */
void hal_fdb_shutdown(void)
{
    pthread_mutex_lock(&g_fdb_init_lock);

    if (!g_fdb_initialized) {
        pthread_mutex_unlock(&g_fdb_init_lock);
        return;
    }

    /* Stop aging thread if running */
    hal_fdb_aging_stop();

    /* Free all entries */
    pthread_rwlock_wrlock(&g_fdb_table.lock);
    fdb_free_all_entries();
    pthread_rwlock_unlock(&g_fdb_table.lock);

    /* Destroy synchronization primitives */
    pthread_rwlock_destroy(&g_fdb_table.lock);
    pthread_mutex_destroy(&g_fdb_aging.mutex);
    pthread_cond_destroy(&g_fdb_aging.cond);

    /* Reset initialization flag */
    g_fdb_initialized = false;
    pthread_mutex_unlock(&g_fdb_init_lock);
}

/* Find entry in hash chain (must hold read lock) */
static fdb_sw_entry_t *fdb_find_entry(const hal_mac_t mac, hal_vlan_t vlan)
{
    uint32_t hash = fdb_hash(mac, vlan);
    fdb_sw_entry_t *e = g_fdb_table.buckets[hash];

    while (e) {
        if (e->valid &&
            HAL_MAC_EQUAL(e->entry.mac, mac) &&
            e->entry.vlan_id == vlan) {
            return e;
        }
        e = e->next;
    }
    return NULL;
}

/* ============================================================================
 * Entry Initialization
 * ============================================================================ */

void hal_fdb_entry_init(hal_fdb_entry_t *entry,
                        const hal_mac_t mac,
                        hal_vlan_t vlan_id)
{
    if (!entry) return;

    memset(entry, 0, sizeof(*entry));
    if (mac) {
        HAL_MAC_COPY(entry->mac, mac);
    }
    entry->vlan_id = vlan_id;
    entry->port = HAL_PORT_INVALID;
    entry->flags = HAL_FLAG_NONE;
    entry->age = 0;
    entry->object_id = HAL_OBJECT_ID_INVALID;
}

/* ============================================================================
 * CRUD Operations
 * ============================================================================ */

hal_status_t hal_fdb_add(hal_fdb_entry_t *entry)
{
    if (!entry) {
        return HAL_E_NULL;
    }
    if (!g_fdb_initialized) {
        fdb_init();  /* Lazy init */
    }
    if (HAL_MAC_IS_ZERO(entry->mac)) {
        return HAL_E_PARAM;
    }
    if (entry->vlan_id < HAL_VLAN_MIN || entry->vlan_id > HAL_VLAN_MAX) {
        return HAL_E_RANGE;
    }

    hal_status_t rv;
    hal_object_id_t oid;

    /* Add to ASIC first */
    rv = asic_l2_add(hal_unit_default(),
                     entry->mac,
                     entry->vlan_id,
                     entry->port,
                     entry->flags,
                     &oid);
    if (rv != HAL_SUCCESS) {
        return rv;
    }

    /* Add to software table */
    pthread_rwlock_wrlock(&g_fdb_table.lock);

    /* Check if already exists */
    if (fdb_find_entry(entry->mac, entry->vlan_id)) {
        pthread_rwlock_unlock(&g_fdb_table.lock);
        asic_l2_delete(hal_unit_default(), oid);
        return HAL_E_EXISTS;
    }

    /* Allocate new entry */
    fdb_sw_entry_t *sw_entry = calloc(1, sizeof(fdb_sw_entry_t));
    if (!sw_entry) {
        pthread_rwlock_unlock(&g_fdb_table.lock);
        asic_l2_delete(hal_unit_default(), oid);
        return HAL_E_MEMORY;
    }

    /* Fill entry */
    sw_entry->valid = true;
    sw_entry->entry = *entry;
    sw_entry->entry.object_id = oid;
    sw_entry->entry.age = 0;
    sw_entry->last_update = hal_time_now();

    /* Insert into hash chain */
    uint32_t hash = fdb_hash(entry->mac, entry->vlan_id);
    sw_entry->next = g_fdb_table.buckets[hash];
    g_fdb_table.buckets[hash] = sw_entry;

    g_fdb_table.count++;
    if (entry->flags & HAL_FLAG_STATIC) {
        g_fdb_table.static_count++;
    }

    /* Update caller's entry with ASIC handle */
    entry->object_id = oid;

    pthread_rwlock_unlock(&g_fdb_table.lock);
    return HAL_SUCCESS;
}

hal_status_t hal_fdb_delete(const hal_mac_t mac, hal_vlan_t vlan_id)
{
    if (!mac) {
        return HAL_E_NULL;
    }
    if (!hal_is_initialized()) {
        return HAL_E_INIT;
    }

    pthread_rwlock_wrlock(&g_fdb_table.lock);

    uint32_t hash = fdb_hash(mac, vlan_id);
    fdb_sw_entry_t *e = g_fdb_table.buckets[hash];
    fdb_sw_entry_t *prev = NULL;

    while (e) {
        if (e->valid &&
            HAL_MAC_EQUAL(e->entry.mac, mac) &&
            e->entry.vlan_id == vlan_id) {

            /* Delete from ASIC */
            hal_status_t rv = asic_l2_delete(hal_unit_default(),
                                             e->entry.object_id);
            if (rv != HAL_SUCCESS && rv != HAL_E_NOT_FOUND) {
                pthread_rwlock_unlock(&g_fdb_table.lock);
                return rv;
            }

            /* Remove from chain */
            if (prev) {
                prev->next = e->next;
            } else {
                g_fdb_table.buckets[hash] = e->next;
            }

            g_fdb_table.count--;
            if (e->entry.flags & HAL_FLAG_STATIC) {
                g_fdb_table.static_count--;
            }

            free(e);
            pthread_rwlock_unlock(&g_fdb_table.lock);
            return HAL_SUCCESS;
        }
        prev = e;
        e = e->next;
    }

    pthread_rwlock_unlock(&g_fdb_table.lock);
    return HAL_E_NOT_FOUND;
}

hal_status_t hal_fdb_get(const hal_mac_t mac,
                         hal_vlan_t vlan_id,
                         hal_fdb_entry_t *entry)
{
    if (!mac || !entry) {
        return HAL_E_NULL;
    }
    if (!hal_is_initialized()) {
        return HAL_E_INIT;
    }

    pthread_rwlock_rdlock(&g_fdb_table.lock);

    fdb_sw_entry_t *e = fdb_find_entry(mac, vlan_id);
    if (!e) {
        pthread_rwlock_unlock(&g_fdb_table.lock);
        return HAL_E_NOT_FOUND;
    }

    /* Copy entry data */
    *entry = e->entry;

    /* Calculate current age */
    hal_time_us_t now = hal_time_now();
    entry->age = (uint32_t)((now - e->last_update) / 1000000);

    /* Refresh HIT flag from ASIC */
    uint32_t asic_flags;
    hal_status_t rv = asic_l2_get(hal_unit_default(),
                                  mac, vlan_id, NULL, &asic_flags, NULL);
    if (rv == HAL_SUCCESS) {
        entry->flags = (entry->flags & ~HAL_FLAG_HIT) |
                       (asic_flags & HAL_FLAG_HIT);
    }

    pthread_rwlock_unlock(&g_fdb_table.lock);
    return HAL_SUCCESS;
}

hal_status_t hal_fdb_update(const hal_fdb_entry_t *entry)
{
    if (!entry) {
        return HAL_E_NULL;
    }
    if (!hal_is_initialized()) {
        return HAL_E_INIT;
    }

    pthread_rwlock_wrlock(&g_fdb_table.lock);

    fdb_sw_entry_t *e = fdb_find_entry(entry->mac, entry->vlan_id);
    if (!e) {
        pthread_rwlock_unlock(&g_fdb_table.lock);
        return HAL_E_NOT_FOUND;
    }

    /* Track static flag changes */
    bool was_static = (e->entry.flags & HAL_FLAG_STATIC) != 0;
    bool is_static = (entry->flags & HAL_FLAG_STATIC) != 0;

    /* Update non-key fields */
    e->entry.port = entry->port;
    e->entry.flags = entry->flags;
    e->last_update = hal_time_now();

    /* Update static count */
    if (was_static && !is_static) {
        g_fdb_table.static_count--;
    } else if (!was_static && is_static) {
        g_fdb_table.static_count++;
    }

    pthread_rwlock_unlock(&g_fdb_table.lock);
    return HAL_SUCCESS;
}

/* ============================================================================
 * Bulk Operations
 * ============================================================================ */

hal_status_t hal_fdb_delete_by_port(hal_port_t port, uint32_t flags)
{
    if (!hal_is_initialized()) {
        return HAL_E_INIT;
    }

    /* Delete from ASIC */
    asic_l2_delete_by_port(hal_unit_default(), port, flags);

    /* Delete from software table */
    pthread_rwlock_wrlock(&g_fdb_table.lock);

    for (int i = 0; i < FDB_HASH_SIZE; i++) {
        fdb_sw_entry_t *e = g_fdb_table.buckets[i];
        fdb_sw_entry_t *prev = NULL;

        while (e) {
            fdb_sw_entry_t *next = e->next;
            bool should_delete = e->valid && e->entry.port == port;

            /* Check flags filter */
            if (should_delete && flags == 0) {
                /* Don't delete static unless explicitly requested */
                if (e->entry.flags & HAL_FLAG_STATIC) {
                    should_delete = false;
                }
            } else if (should_delete && flags != 0) {
                if ((e->entry.flags & flags) == 0) {
                    should_delete = false;
                }
            }

            if (should_delete) {
                if (prev) {
                    prev->next = next;
                } else {
                    g_fdb_table.buckets[i] = next;
                }
                g_fdb_table.count--;
                if (e->entry.flags & HAL_FLAG_STATIC) {
                    g_fdb_table.static_count--;
                }
                free(e);
            } else {
                prev = e;
            }
            e = next;
        }
    }

    pthread_rwlock_unlock(&g_fdb_table.lock);
    return HAL_SUCCESS;
}

hal_status_t hal_fdb_delete_by_vlan(hal_vlan_t vlan_id, uint32_t flags)
{
    if (!hal_is_initialized()) {
        return HAL_E_INIT;
    }

    /* Delete from ASIC */
    asic_l2_delete_by_vlan(hal_unit_default(), vlan_id, flags);

    /* Delete from software table */
    pthread_rwlock_wrlock(&g_fdb_table.lock);

    for (int i = 0; i < FDB_HASH_SIZE; i++) {
        fdb_sw_entry_t *e = g_fdb_table.buckets[i];
        fdb_sw_entry_t *prev = NULL;

        while (e) {
            fdb_sw_entry_t *next = e->next;
            bool should_delete = e->valid && e->entry.vlan_id == vlan_id;

            if (should_delete && flags == 0) {
                if (e->entry.flags & HAL_FLAG_STATIC) {
                    should_delete = false;
                }
            } else if (should_delete && flags != 0) {
                if ((e->entry.flags & flags) == 0) {
                    should_delete = false;
                }
            }

            if (should_delete) {
                if (prev) {
                    prev->next = next;
                } else {
                    g_fdb_table.buckets[i] = next;
                }
                g_fdb_table.count--;
                if (e->entry.flags & HAL_FLAG_STATIC) {
                    g_fdb_table.static_count--;
                }
                free(e);
            } else {
                prev = e;
            }
            e = next;
        }
    }

    pthread_rwlock_unlock(&g_fdb_table.lock);
    return HAL_SUCCESS;
}

hal_status_t hal_fdb_flush_dynamic(void)
{
    if (!hal_is_initialized()) {
        return HAL_E_INIT;
    }

    pthread_rwlock_wrlock(&g_fdb_table.lock);

    for (int i = 0; i < FDB_HASH_SIZE; i++) {
        fdb_sw_entry_t *e = g_fdb_table.buckets[i];
        fdb_sw_entry_t *prev = NULL;

        while (e) {
            fdb_sw_entry_t *next = e->next;

            /* Only delete non-static entries */
            if (e->valid && !(e->entry.flags & HAL_FLAG_STATIC)) {
                asic_l2_delete(hal_unit_default(), e->entry.object_id);

                if (prev) {
                    prev->next = next;
                } else {
                    g_fdb_table.buckets[i] = next;
                }
                g_fdb_table.count--;
                free(e);
            } else {
                prev = e;
            }
            e = next;
        }
    }

    pthread_rwlock_unlock(&g_fdb_table.lock);
    return HAL_SUCCESS;
}

/* ============================================================================
 * Traversal
 * ============================================================================ */

hal_status_t hal_fdb_traverse(hal_fdb_traverse_cb_t callback, void *user_data)
{
    if (!callback) {
        return HAL_E_NULL;
    }
    if (!hal_is_initialized()) {
        return HAL_E_INIT;
    }

    pthread_rwlock_rdlock(&g_fdb_table.lock);

    for (int i = 0; i < FDB_HASH_SIZE; i++) {
        fdb_sw_entry_t *e = g_fdb_table.buckets[i];
        while (e) {
            if (e->valid) {
                hal_fdb_entry_t entry = e->entry;

                /* Calculate current age */
                hal_time_us_t now = hal_time_now();
                entry.age = (uint32_t)((now - e->last_update) / 1000000);

                int rv = callback(&entry, user_data);
                if (rv != 0) {
                    pthread_rwlock_unlock(&g_fdb_table.lock);
                    return HAL_SUCCESS;
                }
            }
            e = e->next;
        }
    }

    pthread_rwlock_unlock(&g_fdb_table.lock);
    return HAL_SUCCESS;
}

/* ============================================================================
 * Statistics
 * ============================================================================ */

hal_status_t hal_fdb_stats_get(hal_fdb_stats_t *stats)
{
    if (!stats) {
        return HAL_E_NULL;
    }
    if (!hal_is_initialized()) {
        return HAL_E_INIT;
    }

    pthread_rwlock_rdlock(&g_fdb_table.lock);

    uint32_t capacity = 0;
    asic_l2_stats_get(hal_unit_default(), NULL, &capacity);

    stats->count = g_fdb_table.count;
    stats->capacity = capacity;
    stats->static_count = g_fdb_table.static_count;
    stats->dynamic_count = g_fdb_table.count - g_fdb_table.static_count;

    pthread_rwlock_unlock(&g_fdb_table.lock);
    return HAL_SUCCESS;
}

/* ============================================================================
 * Aging Configuration
 *
 * TODO (SE Track Candidate): Implement the aging functionality below.
 * See docs/TASK_SE.md for detailed requirements.
 * ============================================================================ */

hal_status_t hal_fdb_age_callback_register(hal_fdb_age_cb_t callback,
                                           void *context)
{
    if (!callback) {
        return HAL_E_NULL;
    }

    /* TODO: Implement callback registration
     *
     * Requirements:
     * - Store callback and context in g_fdb_callbacks array
     * - Support up to FDB_MAX_AGE_CALLBACKS callbacks
     * - Return HAL_E_FULL if array is full
     * - Use g_fdb_callback_lock for thread safety
     */

    (void)context;
    return HAL_E_NOT_IMPL;
}

hal_status_t hal_fdb_age_callback_unregister(hal_fdb_age_cb_t callback)
{
    if (!callback) {
        return HAL_E_NULL;
    }

    /* TODO: Implement callback unregistration
     *
     * Requirements:
     * - Find and remove callback from g_fdb_callbacks array
     * - Return HAL_E_NOT_FOUND if callback not registered
     * - Use g_fdb_callback_lock for thread safety
     */

    return HAL_E_NOT_IMPL;
}

hal_status_t hal_fdb_aging_set(uint32_t aging_time_sec)
{
    /* TODO: Implement aging time configuration
     *
     * Requirements:
     * - Store aging_time_sec in g_fdb_aging.aging_time_sec
     * - Value of 0 disables aging
     * - Use appropriate locking
     */

    (void)aging_time_sec;
    return HAL_E_NOT_IMPL;
}

hal_status_t hal_fdb_aging_get(uint32_t *aging_time_sec)
{
    if (!aging_time_sec) {
        return HAL_E_NULL;
    }

    /* TODO: Implement aging time query
     *
     * Requirements:
     * - Return current g_fdb_aging.aging_time_sec
     * - Use appropriate locking
     */

    return HAL_E_NOT_IMPL;
}

hal_status_t hal_fdb_aging_interval_set(uint32_t interval_sec)
{
    if (interval_sec == 0) {
        return HAL_E_PARAM;
    }

    /* TODO: Implement scan interval configuration
     *
     * Requirements:
     * - Store interval_sec in g_fdb_aging.scan_interval_sec
     * - Wake up aging thread if it's waiting (so it uses new interval)
     * - Use appropriate locking
     */

    (void)interval_sec;
    return HAL_E_NOT_IMPL;
}

hal_status_t hal_fdb_aging_interval_get(uint32_t *interval_sec)
{
    if (!interval_sec) {
        return HAL_E_NULL;
    }

    /* TODO: Implement scan interval query */

    return HAL_E_NOT_IMPL;
}

hal_status_t hal_fdb_aging_start(void)
{
    /* TODO: Implement aging thread startup
     *
     * Requirements:
     * - Create a background thread that periodically scans the FDB table
     * - The thread should:
     *   1. Sleep for scan_interval_sec seconds (use condition variable)
     *   2. Scan all entries in g_fdb_table
     *   3. For each entry where age > aging_time_sec and not static:
     *      a. Invoke all registered callbacks
     *      b. Delete the entry from ASIC and software table
     *   4. Handle HAL_FLAG_HIT bit:
     *      - If HIT is set, clear it and reset the entry's last_update
     *      - If HIT is not set, entry continues to age
     *   5. Repeat until stopped
     *
     * Thread Safety:
     * - Use pthread_create to start the thread
     * - Store thread handle in g_fdb_aging.thread
     * - Set g_fdb_aging.running = true
     * - Use g_fdb_aging.mutex and g_fdb_aging.cond for coordination
     *
     * Error Handling:
     * - Return HAL_E_EXISTS if already running
     * - Return HAL_E_FAIL if thread creation fails
     */

    return HAL_E_NOT_IMPL;
}

hal_status_t hal_fdb_aging_stop(void)
{
    /* TODO: Implement aging thread shutdown
     *
     * Requirements:
     * - Signal the aging thread to stop
     * - Wait for thread to exit (pthread_join)
     * - Set g_fdb_aging.running = false
     *
     * Thread Safety:
     * - Use g_fdb_aging.cond to wake up sleeping thread
     * - Handle case where aging is not running (return success)
     */

    return HAL_SUCCESS;  /* Safe to call even if not running */
}

bool hal_fdb_aging_is_running(void)
{
    /* TODO: Return current running state */
    return false;
}

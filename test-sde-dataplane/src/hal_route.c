/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2024 HAL Assessment Project
 *
 * hal_route.c - L3 Route Table Implementation
 */

#include "hal_route.h"
#include "hal_init.h"
#include "asic/asic_driver.h"

#include <stdlib.h>
#include <string.h>
#include <pthread.h>

/* ============================================================================
 * Internal Data Structures
 * ============================================================================ */

typedef struct route_sw_entry_s {
    bool                 valid;
    hal_route_entry_t    entry;
    struct route_sw_entry_s *next;
} route_sw_entry_t;

#define ROUTE_HASH_SIZE     1024

typedef struct route_table_s {
    pthread_rwlock_t     lock;
    route_sw_entry_t    *buckets[ROUTE_HASH_SIZE];
    uint32_t             count;
} route_table_t;

static route_table_t g_route_table;
static bool g_route_initialized = false;

/* ============================================================================
 * Internal Helpers
 * ============================================================================ */

static uint32_t route_hash(hal_vrf_t vrf, hal_ipv4_t prefix, uint8_t prefix_len)
{
    uint32_t hash = vrf;
    hash = (hash * 31) + prefix;
    hash = (hash * 31) + prefix_len;
    return hash % ROUTE_HASH_SIZE;
}

static void route_free_all_entries(void)
{
    for (int i = 0; i < ROUTE_HASH_SIZE; i++) {
        route_sw_entry_t *e = g_route_table.buckets[i];
        while (e) {
            route_sw_entry_t *next = e->next;
            free(e);
            e = next;
        }
        g_route_table.buckets[i] = NULL;
    }
    g_route_table.count = 0;
}

static hal_status_t route_init(void)
{
    if (g_route_initialized) {
        return HAL_SUCCESS;
    }

    pthread_rwlock_init(&g_route_table.lock, NULL);
    memset(g_route_table.buckets, 0, sizeof(g_route_table.buckets));
    g_route_table.count = 0;

    g_route_initialized = true;
    return HAL_SUCCESS;
}

/* Shutdown route subsystem - called from hal_shutdown() */
void hal_route_shutdown(void)
{
    if (!g_route_initialized) {
        return;
    }

    pthread_rwlock_wrlock(&g_route_table.lock);
    route_free_all_entries();
    pthread_rwlock_unlock(&g_route_table.lock);

    pthread_rwlock_destroy(&g_route_table.lock);

    g_route_initialized = false;
}

static route_sw_entry_t *route_find_entry(hal_vrf_t vrf, hal_ipv4_t prefix,
                                          uint8_t prefix_len)
{
    uint32_t hash = route_hash(vrf, prefix, prefix_len);
    route_sw_entry_t *e = g_route_table.buckets[hash];

    while (e) {
        if (e->valid &&
            e->entry.vrf_id == vrf &&
            e->entry.prefix == prefix &&
            e->entry.prefix_len == prefix_len) {
            return e;
        }
        e = e->next;
    }
    return NULL;
}

/* ============================================================================
 * Entry Initialization
 * ============================================================================ */

void hal_route_entry_init(hal_route_entry_t *entry)
{
    if (!entry) return;

    memset(entry, 0, sizeof(*entry));
    entry->vrf_id = HAL_VRF_DEFAULT;
    entry->egress_port = HAL_PORT_INVALID;
    entry->object_id = HAL_OBJECT_ID_INVALID;
}

/* ============================================================================
 * CRUD Operations
 * ============================================================================ */

hal_status_t hal_route_add(hal_route_entry_t *entry)
{
    if (!entry) {
        return HAL_E_NULL;
    }
    if (!g_route_initialized) {
        route_init();  /* Lazy init */
    }
    if (entry->prefix_len > 32) {
        return HAL_E_RANGE;
    }

    hal_status_t rv;
    hal_object_id_t oid;

    /* Add to ASIC */
    rv = asic_l3_route_add(hal_unit_default(),
                           entry->vrf_id,
                           entry->prefix,
                           entry->prefix_len,
                           entry->nexthop,
                           entry->egress_port,
                           entry->nexthop_mac,
                           &oid);
    if (rv != HAL_SUCCESS) {
        return rv;
    }

    /* Add to software table */
    pthread_rwlock_wrlock(&g_route_table.lock);

    if (route_find_entry(entry->vrf_id, entry->prefix, entry->prefix_len)) {
        pthread_rwlock_unlock(&g_route_table.lock);
        asic_l3_route_delete(hal_unit_default(), oid);
        return HAL_E_EXISTS;
    }

    route_sw_entry_t *sw_entry = calloc(1, sizeof(route_sw_entry_t));
    if (!sw_entry) {
        pthread_rwlock_unlock(&g_route_table.lock);
        asic_l3_route_delete(hal_unit_default(), oid);
        return HAL_E_MEMORY;
    }

    sw_entry->valid = true;
    sw_entry->entry = *entry;
    sw_entry->entry.object_id = oid;

    uint32_t hash = route_hash(entry->vrf_id, entry->prefix, entry->prefix_len);
    sw_entry->next = g_route_table.buckets[hash];
    g_route_table.buckets[hash] = sw_entry;

    g_route_table.count++;
    entry->object_id = oid;

    pthread_rwlock_unlock(&g_route_table.lock);
    return HAL_SUCCESS;
}

hal_status_t hal_route_delete(hal_vrf_t vrf, hal_ipv4_t prefix, uint8_t prefix_len)
{
    if (!hal_is_initialized()) {
        return HAL_E_INIT;
    }

    pthread_rwlock_wrlock(&g_route_table.lock);

    uint32_t hash = route_hash(vrf, prefix, prefix_len);
    route_sw_entry_t *e = g_route_table.buckets[hash];
    route_sw_entry_t *prev = NULL;

    while (e) {
        if (e->valid &&
            e->entry.vrf_id == vrf &&
            e->entry.prefix == prefix &&
            e->entry.prefix_len == prefix_len) {

            hal_status_t rv = asic_l3_route_delete(hal_unit_default(),
                                                   e->entry.object_id);
            if (rv != HAL_SUCCESS && rv != HAL_E_NOT_FOUND) {
                pthread_rwlock_unlock(&g_route_table.lock);
                return rv;
            }

            if (prev) {
                prev->next = e->next;
            } else {
                g_route_table.buckets[hash] = e->next;
            }

            g_route_table.count--;
            free(e);

            pthread_rwlock_unlock(&g_route_table.lock);
            return HAL_SUCCESS;
        }
        prev = e;
        e = e->next;
    }

    pthread_rwlock_unlock(&g_route_table.lock);
    return HAL_E_NOT_FOUND;
}

hal_status_t hal_route_get(hal_vrf_t vrf,
                           hal_ipv4_t prefix,
                           uint8_t prefix_len,
                           hal_route_entry_t *entry)
{
    if (!entry) {
        return HAL_E_NULL;
    }
    if (!hal_is_initialized()) {
        return HAL_E_INIT;
    }

    pthread_rwlock_rdlock(&g_route_table.lock);

    route_sw_entry_t *e = route_find_entry(vrf, prefix, prefix_len);
    if (!e) {
        pthread_rwlock_unlock(&g_route_table.lock);
        return HAL_E_NOT_FOUND;
    }

    *entry = e->entry;

    pthread_rwlock_unlock(&g_route_table.lock);
    return HAL_SUCCESS;
}

hal_status_t hal_route_update(const hal_route_entry_t *entry)
{
    if (!entry) {
        return HAL_E_NULL;
    }
    if (!hal_is_initialized()) {
        return HAL_E_INIT;
    }

    pthread_rwlock_wrlock(&g_route_table.lock);

    route_sw_entry_t *e = route_find_entry(entry->vrf_id, entry->prefix,
                                            entry->prefix_len);
    if (!e) {
        pthread_rwlock_unlock(&g_route_table.lock);
        return HAL_E_NOT_FOUND;
    }

    /* Update non-key fields */
    e->entry.nexthop = entry->nexthop;
    e->entry.egress_port = entry->egress_port;
    HAL_MAC_COPY(e->entry.nexthop_mac, entry->nexthop_mac);
    e->entry.flags = entry->flags;
    e->entry.priority = entry->priority;

    pthread_rwlock_unlock(&g_route_table.lock);
    return HAL_SUCCESS;
}

/* ============================================================================
 * Lookup
 * ============================================================================ */

hal_status_t hal_route_lookup(hal_vrf_t vrf,
                              hal_ipv4_t dest_ip,
                              hal_route_entry_t *entry)
{
    if (!entry) {
        return HAL_E_NULL;
    }
    if (!hal_is_initialized()) {
        return HAL_E_INIT;
    }

    pthread_rwlock_rdlock(&g_route_table.lock);

    route_sw_entry_t *best = NULL;
    uint8_t best_len = 0;

    /* LPM: find longest matching prefix */
    for (int i = 0; i < ROUTE_HASH_SIZE; i++) {
        route_sw_entry_t *e = g_route_table.buckets[i];
        while (e) {
            if (e->valid && e->entry.vrf_id == vrf) {
                /* Calculate mask */
                uint32_t mask = (e->entry.prefix_len == 0) ? 0 :
                    ~((1U << (32 - e->entry.prefix_len)) - 1);

                if ((dest_ip & mask) == (e->entry.prefix & mask)) {
                    if (!best || e->entry.prefix_len > best_len) {
                        best = e;
                        best_len = e->entry.prefix_len;
                    }
                }
            }
            e = e->next;
        }
    }

    if (!best) {
        pthread_rwlock_unlock(&g_route_table.lock);
        return HAL_E_NOT_FOUND;
    }

    *entry = best->entry;

    pthread_rwlock_unlock(&g_route_table.lock);
    return HAL_SUCCESS;
}

/* ============================================================================
 * Bulk Operations
 * ============================================================================ */

hal_status_t hal_route_delete_by_vrf(hal_vrf_t vrf)
{
    if (!hal_is_initialized()) {
        return HAL_E_INIT;
    }

    pthread_rwlock_wrlock(&g_route_table.lock);

    for (int i = 0; i < ROUTE_HASH_SIZE; i++) {
        route_sw_entry_t *e = g_route_table.buckets[i];
        route_sw_entry_t *prev = NULL;

        while (e) {
            route_sw_entry_t *next = e->next;

            if (e->valid && e->entry.vrf_id == vrf) {
                asic_l3_route_delete(hal_unit_default(), e->entry.object_id);

                if (prev) {
                    prev->next = next;
                } else {
                    g_route_table.buckets[i] = next;
                }
                g_route_table.count--;
                free(e);
            } else {
                prev = e;
            }
            e = next;
        }
    }

    pthread_rwlock_unlock(&g_route_table.lock);
    return HAL_SUCCESS;
}

hal_status_t hal_route_delete_by_nexthop(hal_ipv4_t nexthop)
{
    if (!hal_is_initialized()) {
        return HAL_E_INIT;
    }

    pthread_rwlock_wrlock(&g_route_table.lock);

    for (int i = 0; i < ROUTE_HASH_SIZE; i++) {
        route_sw_entry_t *e = g_route_table.buckets[i];
        route_sw_entry_t *prev = NULL;

        while (e) {
            route_sw_entry_t *next = e->next;

            if (e->valid && e->entry.nexthop == nexthop) {
                asic_l3_route_delete(hal_unit_default(), e->entry.object_id);

                if (prev) {
                    prev->next = next;
                } else {
                    g_route_table.buckets[i] = next;
                }
                g_route_table.count--;
                free(e);
            } else {
                prev = e;
            }
            e = next;
        }
    }

    pthread_rwlock_unlock(&g_route_table.lock);
    return HAL_SUCCESS;
}

/* ============================================================================
 * Traversal
 * ============================================================================ */

hal_status_t hal_route_traverse(hal_route_traverse_cb_t callback, void *user_data)
{
    if (!callback) {
        return HAL_E_NULL;
    }
    if (!hal_is_initialized()) {
        return HAL_E_INIT;
    }

    pthread_rwlock_rdlock(&g_route_table.lock);

    for (int i = 0; i < ROUTE_HASH_SIZE; i++) {
        route_sw_entry_t *e = g_route_table.buckets[i];
        while (e) {
            if (e->valid) {
                int rv = callback(&e->entry, user_data);
                if (rv != 0) {
                    pthread_rwlock_unlock(&g_route_table.lock);
                    return HAL_SUCCESS;
                }
            }
            e = e->next;
        }
    }

    pthread_rwlock_unlock(&g_route_table.lock);
    return HAL_SUCCESS;
}

hal_status_t hal_route_traverse_vrf(hal_vrf_t vrf,
                                    hal_route_traverse_cb_t callback,
                                    void *user_data)
{
    if (!callback) {
        return HAL_E_NULL;
    }
    if (!hal_is_initialized()) {
        return HAL_E_INIT;
    }

    pthread_rwlock_rdlock(&g_route_table.lock);

    for (int i = 0; i < ROUTE_HASH_SIZE; i++) {
        route_sw_entry_t *e = g_route_table.buckets[i];
        while (e) {
            if (e->valid && e->entry.vrf_id == vrf) {
                int rv = callback(&e->entry, user_data);
                if (rv != 0) {
                    pthread_rwlock_unlock(&g_route_table.lock);
                    return HAL_SUCCESS;
                }
            }
            e = e->next;
        }
    }

    pthread_rwlock_unlock(&g_route_table.lock);
    return HAL_SUCCESS;
}

/* ============================================================================
 * Statistics
 * ============================================================================ */

hal_status_t hal_route_stats_get(hal_route_stats_t *stats)
{
    if (!stats) {
        return HAL_E_NULL;
    }
    if (!hal_is_initialized()) {
        return HAL_E_INIT;
    }

    pthread_rwlock_rdlock(&g_route_table.lock);

    uint32_t capacity = 0;
    asic_l3_route_stats_get(hal_unit_default(), NULL, &capacity);

    stats->count = g_route_table.count;
    stats->capacity = capacity;
    stats->ipv4_count = g_route_table.count;  /* Currently only IPv4 */
    stats->ipv6_count = 0;

    pthread_rwlock_unlock(&g_route_table.lock);
    return HAL_SUCCESS;
}

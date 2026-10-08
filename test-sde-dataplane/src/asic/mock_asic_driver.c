/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2024 HAL Assessment Project
 *
 * mock_asic_driver.c - Mock ASIC driver implementation
 *
 * This provides a simulated ASIC for testing and development.
 * It maintains in-memory tables and can simulate latency and errors.
 */

#include "asic/asic_driver.h"
#include "hal_error.h"

#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>

/* ============================================================================
 * Configuration
 * ============================================================================ */

#define DEFAULT_MAX_FDB_ENTRIES     16384
#define DEFAULT_MAX_ROUTE_ENTRIES   8192
#define DEFAULT_LATENCY_US          1000    /* 1ms default latency */

/* ============================================================================
 * Internal Data Structures
 * ============================================================================ */

/* L2 FDB entry (internal representation) */
typedef struct l2_entry_s {
    bool            valid;
    hal_mac_t       mac;
    hal_vlan_t      vlan;
    hal_port_t      port;
    uint32_t        flags;
    hal_object_id_t oid;
} l2_entry_t;

/* L3 Route entry (internal representation) */
typedef struct l3_route_entry_s {
    bool            valid;
    hal_vrf_t       vrf;
    hal_ipv4_t      prefix;
    uint8_t         prefix_len;
    hal_ipv4_t      nexthop;
    hal_port_t      egress_port;
    hal_mac_t       nexthop_mac;
    hal_object_id_t oid;
} l3_route_entry_t;

/* Per-unit ASIC state */
typedef struct asic_state_s {
    bool                initialized;
    pthread_mutex_t     lock;

    /* Capabilities */
    asic_capabilities_t caps;

    /* L2 FDB table */
    l2_entry_t         *l2_table;
    uint32_t            l2_count;
    uint32_t            l2_capacity;

    /* L3 Route table */
    l3_route_entry_t   *l3_table;
    uint32_t            l3_count;
    uint32_t            l3_capacity;

    /* Object ID generator */
    uint64_t            next_oid;

    /* Simulation controls */
    uint32_t            latency_us;
    hal_status_t        inject_error;
    uint32_t            inject_count;

} asic_state_t;

/* Global state for all units */
static asic_state_t g_asic[ASIC_UNIT_MAX];
static pthread_mutex_t g_init_lock = PTHREAD_MUTEX_INITIALIZER;
static bool g_module_initialized = false;

/* ============================================================================
 * Internal Helpers
 * ============================================================================ */

static inline void simulate_latency(asic_state_t *asic)
{
    if (asic->latency_us > 0) {
        usleep(asic->latency_us);
    }
}

static inline hal_status_t check_error_injection(asic_state_t *asic)
{
    if (asic->inject_count > 0) {
        asic->inject_count--;
        return asic->inject_error;
    }
    return HAL_SUCCESS;
}

static hal_object_id_t make_oid(asic_unit_t unit, hal_object_type_t type, uint64_t index)
{
    return ((uint64_t)type << 56) | ((uint64_t)unit << 48) | (index & 0x0000FFFFFFFFFFFF);
}

/* Hash function for MAC+VLAN lookup */
static uint32_t l2_hash(const hal_mac_t mac, hal_vlan_t vlan, uint32_t capacity)
{
    uint32_t hash = 0;
    hash = (hash * 31) + mac[0];
    hash = (hash * 31) + mac[1];
    hash = (hash * 31) + mac[2];
    hash = (hash * 31) + mac[3];
    hash = (hash * 31) + mac[4];
    hash = (hash * 31) + mac[5];
    hash = (hash * 31) + vlan;
    return hash % capacity;
}

/* Find L2 entry by key (must hold lock) */
static l2_entry_t *l2_find_by_key(asic_state_t *asic, const hal_mac_t mac, hal_vlan_t vlan)
{
    /* Linear search with hash hint for starting position */
    uint32_t start = l2_hash(mac, vlan, asic->l2_capacity);

    for (uint32_t i = 0; i < asic->l2_capacity; i++) {
        uint32_t idx = (start + i) % asic->l2_capacity;
        l2_entry_t *e = &asic->l2_table[idx];

        if (e->valid && HAL_MAC_EQUAL(e->mac, mac) && e->vlan == vlan) {
            return e;
        }
    }
    return NULL;
}

/* Find L2 entry by OID (must hold lock) */
static l2_entry_t *l2_find_by_oid(asic_state_t *asic, hal_object_id_t oid)
{
    uint64_t index = HAL_OID_INDEX(oid);
    if (index >= asic->l2_capacity) {
        return NULL;
    }

    l2_entry_t *e = &asic->l2_table[index];
    if (e->valid && e->oid == oid) {
        return e;
    }
    return NULL;
}

/* Find empty L2 slot (must hold lock) */
static l2_entry_t *l2_find_empty(asic_state_t *asic, const hal_mac_t mac, hal_vlan_t vlan)
{
    uint32_t start = l2_hash(mac, vlan, asic->l2_capacity);

    for (uint32_t i = 0; i < asic->l2_capacity; i++) {
        uint32_t idx = (start + i) % asic->l2_capacity;
        if (!asic->l2_table[idx].valid) {
            return &asic->l2_table[idx];
        }
    }
    return NULL;
}

/* Find L3 route by key (must hold lock) */
static l3_route_entry_t *l3_find_by_key(asic_state_t *asic, hal_vrf_t vrf,
                                         hal_ipv4_t prefix, uint8_t prefix_len)
{
    for (uint32_t i = 0; i < asic->l3_capacity; i++) {
        l3_route_entry_t *e = &asic->l3_table[i];
        if (e->valid && e->vrf == vrf && e->prefix == prefix && e->prefix_len == prefix_len) {
            return e;
        }
    }
    return NULL;
}

/* Find L3 route by OID (must hold lock) */
static l3_route_entry_t *l3_find_by_oid(asic_state_t *asic, hal_object_id_t oid)
{
    uint64_t index = HAL_OID_INDEX(oid);
    if (index >= asic->l3_capacity) {
        return NULL;
    }

    l3_route_entry_t *e = &asic->l3_table[index];
    if (e->valid && e->oid == oid) {
        return e;
    }
    return NULL;
}

/* Find empty L3 slot (must hold lock) */
static l3_route_entry_t *l3_find_empty(asic_state_t *asic)
{
    for (uint32_t i = 0; i < asic->l3_capacity; i++) {
        if (!asic->l3_table[i].valid) {
            return &asic->l3_table[i];
        }
    }
    return NULL;
}

/* ============================================================================
 * Unit Management
 * ============================================================================ */

hal_status_t asic_init(asic_unit_t unit)
{
    if (unit < 0 || unit >= ASIC_UNIT_MAX) {
        return HAL_E_PARAM;
    }

    pthread_mutex_lock(&g_init_lock);

    if (!g_module_initialized) {
        /* First-time module init */
        memset(g_asic, 0, sizeof(g_asic));
        g_module_initialized = true;
    }

    asic_state_t *asic = &g_asic[unit];

    if (asic->initialized) {
        pthread_mutex_unlock(&g_init_lock);
        return HAL_E_EXISTS;
    }

    /* Initialize mutex */
    pthread_mutex_init(&asic->lock, NULL);

    /* Set capabilities */
    asic->caps.max_fdb_entries = DEFAULT_MAX_FDB_ENTRIES;
    asic->caps.max_route_entries = DEFAULT_MAX_ROUTE_ENTRIES;
    asic->caps.max_acl_entries = 4096;
    asic->caps.max_nexthops = 16384;
    asic->caps.max_ecmp_groups = 1024;
    asic->caps.max_ecmp_members = 64;
    asic->caps.max_ports = 128;
    asic->caps.max_vlans = 4094;
    asic->caps.max_vrfs = 256;
    asic->caps.ipv6_support = true;
    asic->caps.mpls_support = true;
    asic->caps.vxlan_support = true;

    /* Allocate L2 table */
    asic->l2_capacity = asic->caps.max_fdb_entries;
    asic->l2_table = calloc(asic->l2_capacity, sizeof(l2_entry_t));
    if (!asic->l2_table) {
        pthread_mutex_destroy(&asic->lock);
        pthread_mutex_unlock(&g_init_lock);
        return HAL_E_MEMORY;
    }
    asic->l2_count = 0;

    /* Allocate L3 table */
    asic->l3_capacity = asic->caps.max_route_entries;
    asic->l3_table = calloc(asic->l3_capacity, sizeof(l3_route_entry_t));
    if (!asic->l3_table) {
        free(asic->l2_table);
        pthread_mutex_destroy(&asic->lock);
        pthread_mutex_unlock(&g_init_lock);
        return HAL_E_MEMORY;
    }
    asic->l3_count = 0;

    /* Initialize OID generator */
    asic->next_oid = 1;

    /* Set default latency */
    asic->latency_us = DEFAULT_LATENCY_US;

    /* No error injection by default */
    asic->inject_error = HAL_SUCCESS;
    asic->inject_count = 0;

    asic->initialized = true;

    pthread_mutex_unlock(&g_init_lock);
    return HAL_SUCCESS;
}

hal_status_t asic_shutdown(asic_unit_t unit)
{
    if (unit < 0 || unit >= ASIC_UNIT_MAX) {
        return HAL_E_PARAM;
    }

    pthread_mutex_lock(&g_init_lock);

    asic_state_t *asic = &g_asic[unit];

    if (!asic->initialized) {
        pthread_mutex_unlock(&g_init_lock);
        return HAL_E_INIT;
    }

    pthread_mutex_lock(&asic->lock);

    /* Free tables */
    free(asic->l2_table);
    free(asic->l3_table);

    asic->l2_table = NULL;
    asic->l3_table = NULL;
    asic->initialized = false;

    pthread_mutex_unlock(&asic->lock);
    pthread_mutex_destroy(&asic->lock);

    pthread_mutex_unlock(&g_init_lock);
    return HAL_SUCCESS;
}

bool asic_is_initialized(asic_unit_t unit)
{
    if (unit < 0 || unit >= ASIC_UNIT_MAX) {
        return false;
    }
    return g_asic[unit].initialized;
}

hal_status_t asic_capabilities_get(asic_unit_t unit, asic_capabilities_t *caps)
{
    if (unit < 0 || unit >= ASIC_UNIT_MAX) {
        return HAL_E_PARAM;
    }
    if (!caps) {
        return HAL_E_NULL;
    }

    asic_state_t *asic = &g_asic[unit];
    if (!asic->initialized) {
        return HAL_E_INIT;
    }

    pthread_mutex_lock(&asic->lock);
    *caps = asic->caps;
    pthread_mutex_unlock(&asic->lock);

    return HAL_SUCCESS;
}

/* ============================================================================
 * L2 FDB Operations
 * ============================================================================ */

hal_status_t asic_l2_add(asic_unit_t unit,
                         const hal_mac_t mac,
                         hal_vlan_t vlan,
                         hal_port_t port,
                         uint32_t flags,
                         hal_object_id_t *oid)
{
    if (unit < 0 || unit >= ASIC_UNIT_MAX) {
        return HAL_E_PARAM;
    }
    if (!mac || !oid) {
        return HAL_E_NULL;
    }
    if (vlan < HAL_VLAN_MIN || vlan > HAL_VLAN_MAX) {
        return HAL_E_RANGE;
    }

    asic_state_t *asic = &g_asic[unit];
    if (!asic->initialized) {
        return HAL_E_INIT;
    }

    pthread_mutex_lock(&asic->lock);

    /* Check for error injection */
    hal_status_t injected = check_error_injection(asic);
    if (injected != HAL_SUCCESS) {
        pthread_mutex_unlock(&asic->lock);
        return injected;
    }

    /* Simulate latency */
    simulate_latency(asic);

    /* Check if entry already exists */
    if (l2_find_by_key(asic, mac, vlan) != NULL) {
        pthread_mutex_unlock(&asic->lock);
        return HAL_E_EXISTS;
    }

    /* Check capacity */
    if (asic->l2_count >= asic->l2_capacity) {
        pthread_mutex_unlock(&asic->lock);
        return HAL_E_FULL;
    }

    /* Find empty slot */
    l2_entry_t *entry = l2_find_empty(asic, mac, vlan);
    if (!entry) {
        pthread_mutex_unlock(&asic->lock);
        return HAL_E_FULL;
    }

    /* Calculate index for OID */
    uint64_t index = entry - asic->l2_table;

    /* Fill entry */
    entry->valid = true;
    HAL_MAC_COPY(entry->mac, mac);
    entry->vlan = vlan;
    entry->port = port;
    entry->flags = flags;
    entry->oid = make_oid(unit, HAL_OBJ_TYPE_FDB, index);

    asic->l2_count++;
    *oid = entry->oid;

    pthread_mutex_unlock(&asic->lock);
    return HAL_SUCCESS;
}

hal_status_t asic_l2_delete(asic_unit_t unit, hal_object_id_t oid)
{
    if (unit < 0 || unit >= ASIC_UNIT_MAX) {
        return HAL_E_PARAM;
    }
    if (oid == HAL_OBJECT_ID_INVALID) {
        return HAL_E_PARAM;
    }

    asic_state_t *asic = &g_asic[unit];
    if (!asic->initialized) {
        return HAL_E_INIT;
    }

    pthread_mutex_lock(&asic->lock);

    hal_status_t injected = check_error_injection(asic);
    if (injected != HAL_SUCCESS) {
        pthread_mutex_unlock(&asic->lock);
        return injected;
    }

    simulate_latency(asic);

    l2_entry_t *entry = l2_find_by_oid(asic, oid);
    if (!entry) {
        pthread_mutex_unlock(&asic->lock);
        return HAL_E_NOT_FOUND;
    }

    entry->valid = false;
    asic->l2_count--;

    pthread_mutex_unlock(&asic->lock);
    return HAL_SUCCESS;
}

hal_status_t asic_l2_delete_by_key(asic_unit_t unit, const hal_mac_t mac, hal_vlan_t vlan)
{
    if (unit < 0 || unit >= ASIC_UNIT_MAX) {
        return HAL_E_PARAM;
    }
    if (!mac) {
        return HAL_E_NULL;
    }

    asic_state_t *asic = &g_asic[unit];
    if (!asic->initialized) {
        return HAL_E_INIT;
    }

    pthread_mutex_lock(&asic->lock);

    hal_status_t injected = check_error_injection(asic);
    if (injected != HAL_SUCCESS) {
        pthread_mutex_unlock(&asic->lock);
        return injected;
    }

    simulate_latency(asic);

    l2_entry_t *entry = l2_find_by_key(asic, mac, vlan);
    if (!entry) {
        pthread_mutex_unlock(&asic->lock);
        return HAL_E_NOT_FOUND;
    }

    entry->valid = false;
    asic->l2_count--;

    pthread_mutex_unlock(&asic->lock);
    return HAL_SUCCESS;
}

hal_status_t asic_l2_get(asic_unit_t unit,
                         const hal_mac_t mac,
                         hal_vlan_t vlan,
                         hal_port_t *port,
                         uint32_t *flags,
                         hal_object_id_t *oid)
{
    if (unit < 0 || unit >= ASIC_UNIT_MAX) {
        return HAL_E_PARAM;
    }
    if (!mac) {
        return HAL_E_NULL;
    }

    asic_state_t *asic = &g_asic[unit];
    if (!asic->initialized) {
        return HAL_E_INIT;
    }

    pthread_mutex_lock(&asic->lock);

    simulate_latency(asic);

    l2_entry_t *entry = l2_find_by_key(asic, mac, vlan);
    if (!entry) {
        pthread_mutex_unlock(&asic->lock);
        return HAL_E_NOT_FOUND;
    }

    if (port) *port = entry->port;
    if (flags) *flags = entry->flags;
    if (oid) *oid = entry->oid;

    pthread_mutex_unlock(&asic->lock);
    return HAL_SUCCESS;
}

hal_status_t asic_l2_flags_set(asic_unit_t unit, hal_object_id_t oid, uint32_t flags)
{
    if (unit < 0 || unit >= ASIC_UNIT_MAX) {
        return HAL_E_PARAM;
    }

    asic_state_t *asic = &g_asic[unit];
    if (!asic->initialized) {
        return HAL_E_INIT;
    }

    pthread_mutex_lock(&asic->lock);

    simulate_latency(asic);

    l2_entry_t *entry = l2_find_by_oid(asic, oid);
    if (!entry) {
        pthread_mutex_unlock(&asic->lock);
        return HAL_E_NOT_FOUND;
    }

    entry->flags = flags;

    pthread_mutex_unlock(&asic->lock);
    return HAL_SUCCESS;
}

hal_status_t asic_l2_hit_clear(asic_unit_t unit, hal_object_id_t oid)
{
    if (unit < 0 || unit >= ASIC_UNIT_MAX) {
        return HAL_E_PARAM;
    }

    asic_state_t *asic = &g_asic[unit];
    if (!asic->initialized) {
        return HAL_E_INIT;
    }

    pthread_mutex_lock(&asic->lock);

    l2_entry_t *entry = l2_find_by_oid(asic, oid);
    if (!entry) {
        pthread_mutex_unlock(&asic->lock);
        return HAL_E_NOT_FOUND;
    }

    entry->flags &= ~HAL_FLAG_HIT;

    pthread_mutex_unlock(&asic->lock);
    return HAL_SUCCESS;
}

hal_status_t asic_l2_delete_by_port(asic_unit_t unit, hal_port_t port, uint32_t flags)
{
    if (unit < 0 || unit >= ASIC_UNIT_MAX) {
        return HAL_E_PARAM;
    }

    asic_state_t *asic = &g_asic[unit];
    if (!asic->initialized) {
        return HAL_E_INIT;
    }

    pthread_mutex_lock(&asic->lock);

    simulate_latency(asic);

    for (uint32_t i = 0; i < asic->l2_capacity; i++) {
        l2_entry_t *e = &asic->l2_table[i];
        if (e->valid && e->port == port) {
            if (flags == 0 || (e->flags & flags) != 0) {
                /* Skip static entries unless specifically requested */
                if (flags == 0 && (e->flags & HAL_FLAG_STATIC)) {
                    continue;
                }
                e->valid = false;
                asic->l2_count--;
            }
        }
    }

    pthread_mutex_unlock(&asic->lock);
    return HAL_SUCCESS;
}

hal_status_t asic_l2_delete_by_vlan(asic_unit_t unit, hal_vlan_t vlan, uint32_t flags)
{
    if (unit < 0 || unit >= ASIC_UNIT_MAX) {
        return HAL_E_PARAM;
    }

    asic_state_t *asic = &g_asic[unit];
    if (!asic->initialized) {
        return HAL_E_INIT;
    }

    pthread_mutex_lock(&asic->lock);

    simulate_latency(asic);

    for (uint32_t i = 0; i < asic->l2_capacity; i++) {
        l2_entry_t *e = &asic->l2_table[i];
        if (e->valid && e->vlan == vlan) {
            if (flags == 0 || (e->flags & flags) != 0) {
                if (flags == 0 && (e->flags & HAL_FLAG_STATIC)) {
                    continue;
                }
                e->valid = false;
                asic->l2_count--;
            }
        }
    }

    pthread_mutex_unlock(&asic->lock);
    return HAL_SUCCESS;
}

hal_status_t asic_l2_traverse(asic_unit_t unit,
                              asic_l2_traverse_cb_t callback,
                              void *user_data)
{
    if (unit < 0 || unit >= ASIC_UNIT_MAX) {
        return HAL_E_PARAM;
    }
    if (!callback) {
        return HAL_E_NULL;
    }

    asic_state_t *asic = &g_asic[unit];
    if (!asic->initialized) {
        return HAL_E_INIT;
    }

    pthread_mutex_lock(&asic->lock);

    for (uint32_t i = 0; i < asic->l2_capacity; i++) {
        l2_entry_t *e = &asic->l2_table[i];
        if (e->valid) {
            asic_l2_entry_t entry;
            HAL_MAC_COPY(entry.mac, e->mac);
            entry.vlan = e->vlan;
            entry.port = e->port;
            entry.flags = e->flags;
            entry.oid = e->oid;

            int rv = callback(unit, &entry, user_data);
            if (rv != 0) {
                pthread_mutex_unlock(&asic->lock);
                return HAL_SUCCESS; /* Early termination is not an error */
            }
        }
    }

    pthread_mutex_unlock(&asic->lock);
    return HAL_SUCCESS;
}

hal_status_t asic_l2_stats_get(asic_unit_t unit, uint32_t *count, uint32_t *capacity)
{
    if (unit < 0 || unit >= ASIC_UNIT_MAX) {
        return HAL_E_PARAM;
    }

    asic_state_t *asic = &g_asic[unit];
    if (!asic->initialized) {
        return HAL_E_INIT;
    }

    pthread_mutex_lock(&asic->lock);
    if (count) *count = asic->l2_count;
    if (capacity) *capacity = asic->l2_capacity;
    pthread_mutex_unlock(&asic->lock);

    return HAL_SUCCESS;
}

/* ============================================================================
 * L3 Route Operations
 * ============================================================================ */

hal_status_t asic_l3_route_add(asic_unit_t unit,
                               hal_vrf_t vrf,
                               hal_ipv4_t prefix,
                               uint8_t prefix_len,
                               hal_ipv4_t nexthop,
                               hal_port_t egress_port,
                               const hal_mac_t nexthop_mac,
                               hal_object_id_t *oid)
{
    if (unit < 0 || unit >= ASIC_UNIT_MAX) {
        return HAL_E_PARAM;
    }
    if (!oid) {
        return HAL_E_NULL;
    }
    if (prefix_len > 32) {
        return HAL_E_RANGE;
    }

    asic_state_t *asic = &g_asic[unit];
    if (!asic->initialized) {
        return HAL_E_INIT;
    }

    pthread_mutex_lock(&asic->lock);

    hal_status_t injected = check_error_injection(asic);
    if (injected != HAL_SUCCESS) {
        pthread_mutex_unlock(&asic->lock);
        return injected;
    }

    simulate_latency(asic);

    /* Check if route already exists */
    if (l3_find_by_key(asic, vrf, prefix, prefix_len) != NULL) {
        pthread_mutex_unlock(&asic->lock);
        return HAL_E_EXISTS;
    }

    /* Check capacity */
    if (asic->l3_count >= asic->l3_capacity) {
        pthread_mutex_unlock(&asic->lock);
        return HAL_E_FULL;
    }

    /* Find empty slot */
    l3_route_entry_t *entry = l3_find_empty(asic);
    if (!entry) {
        pthread_mutex_unlock(&asic->lock);
        return HAL_E_FULL;
    }

    uint64_t index = entry - asic->l3_table;

    /* Fill entry */
    entry->valid = true;
    entry->vrf = vrf;
    entry->prefix = prefix;
    entry->prefix_len = prefix_len;
    entry->nexthop = nexthop;
    entry->egress_port = egress_port;
    if (nexthop_mac) {
        HAL_MAC_COPY(entry->nexthop_mac, nexthop_mac);
    } else {
        HAL_MAC_CLEAR(entry->nexthop_mac);
    }
    entry->oid = make_oid(unit, HAL_OBJ_TYPE_ROUTE, index);

    asic->l3_count++;
    *oid = entry->oid;

    pthread_mutex_unlock(&asic->lock);
    return HAL_SUCCESS;
}

hal_status_t asic_l3_route_delete(asic_unit_t unit, hal_object_id_t oid)
{
    if (unit < 0 || unit >= ASIC_UNIT_MAX) {
        return HAL_E_PARAM;
    }

    asic_state_t *asic = &g_asic[unit];
    if (!asic->initialized) {
        return HAL_E_INIT;
    }

    pthread_mutex_lock(&asic->lock);

    hal_status_t injected = check_error_injection(asic);
    if (injected != HAL_SUCCESS) {
        pthread_mutex_unlock(&asic->lock);
        return injected;
    }

    simulate_latency(asic);

    l3_route_entry_t *entry = l3_find_by_oid(asic, oid);
    if (!entry) {
        pthread_mutex_unlock(&asic->lock);
        return HAL_E_NOT_FOUND;
    }

    entry->valid = false;
    asic->l3_count--;

    pthread_mutex_unlock(&asic->lock);
    return HAL_SUCCESS;
}

hal_status_t asic_l3_route_delete_by_key(asic_unit_t unit,
                                         hal_vrf_t vrf,
                                         hal_ipv4_t prefix,
                                         uint8_t prefix_len)
{
    if (unit < 0 || unit >= ASIC_UNIT_MAX) {
        return HAL_E_PARAM;
    }

    asic_state_t *asic = &g_asic[unit];
    if (!asic->initialized) {
        return HAL_E_INIT;
    }

    pthread_mutex_lock(&asic->lock);

    hal_status_t injected = check_error_injection(asic);
    if (injected != HAL_SUCCESS) {
        pthread_mutex_unlock(&asic->lock);
        return injected;
    }

    simulate_latency(asic);

    l3_route_entry_t *entry = l3_find_by_key(asic, vrf, prefix, prefix_len);
    if (!entry) {
        pthread_mutex_unlock(&asic->lock);
        return HAL_E_NOT_FOUND;
    }

    entry->valid = false;
    asic->l3_count--;

    pthread_mutex_unlock(&asic->lock);
    return HAL_SUCCESS;
}

hal_status_t asic_l3_route_traverse(asic_unit_t unit,
                                    asic_l3_route_traverse_cb_t callback,
                                    void *user_data)
{
    if (unit < 0 || unit >= ASIC_UNIT_MAX) {
        return HAL_E_PARAM;
    }
    if (!callback) {
        return HAL_E_NULL;
    }

    asic_state_t *asic = &g_asic[unit];
    if (!asic->initialized) {
        return HAL_E_INIT;
    }

    pthread_mutex_lock(&asic->lock);

    for (uint32_t i = 0; i < asic->l3_capacity; i++) {
        l3_route_entry_t *e = &asic->l3_table[i];
        if (e->valid) {
            asic_l3_route_entry_t entry;
            entry.vrf = e->vrf;
            entry.prefix = e->prefix;
            entry.prefix_len = e->prefix_len;
            entry.nexthop = e->nexthop;
            entry.egress_port = e->egress_port;
            HAL_MAC_COPY(entry.nexthop_mac, e->nexthop_mac);
            entry.oid = e->oid;

            int rv = callback(unit, &entry, user_data);
            if (rv != 0) {
                pthread_mutex_unlock(&asic->lock);
                return HAL_SUCCESS;
            }
        }
    }

    pthread_mutex_unlock(&asic->lock);
    return HAL_SUCCESS;
}

hal_status_t asic_l3_route_stats_get(asic_unit_t unit, uint32_t *count, uint32_t *capacity)
{
    if (unit < 0 || unit >= ASIC_UNIT_MAX) {
        return HAL_E_PARAM;
    }

    asic_state_t *asic = &g_asic[unit];
    if (!asic->initialized) {
        return HAL_E_INIT;
    }

    pthread_mutex_lock(&asic->lock);
    if (count) *count = asic->l3_count;
    if (capacity) *capacity = asic->l3_capacity;
    pthread_mutex_unlock(&asic->lock);

    return HAL_SUCCESS;
}

/* ============================================================================
 * Simulation Control
 * ============================================================================ */

void asic_set_latency(asic_unit_t unit, uint32_t latency_us)
{
    if (unit < 0 || unit >= ASIC_UNIT_MAX) {
        return;
    }

    asic_state_t *asic = &g_asic[unit];
    if (!asic->initialized) {
        return;
    }

    pthread_mutex_lock(&asic->lock);
    asic->latency_us = latency_us;
    pthread_mutex_unlock(&asic->lock);
}

uint32_t asic_get_latency(asic_unit_t unit)
{
    if (unit < 0 || unit >= ASIC_UNIT_MAX) {
        return 0;
    }

    asic_state_t *asic = &g_asic[unit];
    if (!asic->initialized) {
        return 0;
    }

    pthread_mutex_lock(&asic->lock);
    uint32_t lat = asic->latency_us;
    pthread_mutex_unlock(&asic->lock);

    return lat;
}

void asic_inject_error(asic_unit_t unit, hal_status_t error, uint32_t count)
{
    if (unit < 0 || unit >= ASIC_UNIT_MAX) {
        return;
    }

    asic_state_t *asic = &g_asic[unit];
    if (!asic->initialized) {
        return;
    }

    pthread_mutex_lock(&asic->lock);
    asic->inject_error = error;
    asic->inject_count = count;
    pthread_mutex_unlock(&asic->lock);
}

void asic_simulate_hits(asic_unit_t unit, uint32_t percentage)
{
    if (unit < 0 || unit >= ASIC_UNIT_MAX) {
        return;
    }
    if (percentage > 100) {
        percentage = 100;
    }

    asic_state_t *asic = &g_asic[unit];
    if (!asic->initialized) {
        return;
    }

    pthread_mutex_lock(&asic->lock);

    srand((unsigned int)time(NULL));

    for (uint32_t i = 0; i < asic->l2_capacity; i++) {
        l2_entry_t *e = &asic->l2_table[i];
        if (e->valid) {
            if ((uint32_t)(rand() % 100) < percentage) {
                e->flags |= HAL_FLAG_HIT;
            }
        }
    }

    pthread_mutex_unlock(&asic->lock);
}

/* ============================================================================
 * Helper: Error String
 * ============================================================================ */

const char *hal_status_str(hal_status_t status)
{
    switch (status) {
        case HAL_SUCCESS:       return "SUCCESS";
        case HAL_E_PARAM:       return "E_PARAM";
        case HAL_E_NULL:        return "E_NULL";
        case HAL_E_RANGE:       return "E_RANGE";
        case HAL_E_MEMORY:      return "E_MEMORY";
        case HAL_E_RESOURCE:    return "E_RESOURCE";
        case HAL_E_FULL:        return "E_FULL";
        case HAL_E_EMPTY:       return "E_EMPTY";
        case HAL_E_NOT_FOUND:   return "E_NOT_FOUND";
        case HAL_E_EXISTS:      return "E_EXISTS";
        case HAL_E_BUSY:        return "E_BUSY";
        case HAL_E_UNAVAIL:     return "E_UNAVAIL";
        case HAL_E_DISABLED:    return "E_DISABLED";
        case HAL_E_TIMEOUT:     return "E_TIMEOUT";
        case HAL_E_CANCELED:    return "E_CANCELED";
        case HAL_E_FAIL:        return "E_FAIL";
        case HAL_E_IO:          return "E_IO";
        case HAL_E_INIT:        return "E_INIT";
        case HAL_E_CONFIG:      return "E_CONFIG";
        case HAL_E_VERSION:     return "E_VERSION";
        case HAL_E_HW:          return "E_HW";
        case HAL_E_HW_ACCESS:   return "E_HW_ACCESS";
        case HAL_E_HW_BUSY:     return "E_HW_BUSY";
        case HAL_E_INTERNAL:    return "E_INTERNAL";
        case HAL_E_NOT_IMPL:    return "E_NOT_IMPL";
        case HAL_E_UNKNOWN:     return "E_UNKNOWN";
        default:                return "UNKNOWN";
    }
}

/* ============================================================================
 * Time Utility
 * ============================================================================ */

static struct timespec g_start_time;
static bool g_time_initialized = false;

hal_time_us_t hal_time_now(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    if (!g_time_initialized) {
        g_start_time = now;
        g_time_initialized = true;
        return 0;
    }

    time_t sec = now.tv_sec - g_start_time.tv_sec;
    long nsec = now.tv_nsec - g_start_time.tv_nsec;

    if (nsec < 0) {
        sec--;
        nsec += 1000000000L;
    }

    return (hal_time_us_t)(sec * 1000000) + (hal_time_us_t)(nsec / 1000);
}

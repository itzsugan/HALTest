/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2024 HAL Assessment Project
 *
 * hal_route.h - L3 Route Table API
 *
 * Manages IPv4 routing entries in the ASIC forwarding table.
 * Supports VRF (Virtual Routing and Forwarding) for route separation.
 */

#ifndef HAL_ROUTE_H
#define HAL_ROUTE_H

#include "hal_types.h"
#include "hal_error.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Route Entry Structure
 * ============================================================================ */

/**
 * Route entry structure
 *
 * Key: (vrf_id, prefix, prefix_len)
 */
typedef struct hal_route_entry_s {
    hal_vrf_t       vrf_id;         /* Virtual router ID (key) */
    hal_ipv4_t      prefix;         /* IPv4 prefix (key) */
    uint8_t         prefix_len;     /* Prefix length 0-32 (key) */
    hal_ipv4_t      nexthop;        /* Next hop IP address */
    hal_port_t      egress_port;    /* Egress port */
    hal_mac_t       nexthop_mac;    /* Next hop MAC (for direct routes) */
    hal_flags_t     flags;          /* Route flags */
    uint32_t        priority;       /* Route priority/metric */
    hal_object_id_t object_id;      /* ASIC object handle (read-only) */
} hal_route_entry_t;

/**
 * Initialize route entry with default values
 *
 * @param entry     Entry to initialize
 */
void hal_route_entry_init(hal_route_entry_t *entry);

/* ============================================================================
 * Route CRUD Operations
 * ============================================================================ */

/**
 * Add route entry
 *
 * Adds a new route to the routing table. The key is (vrf_id, prefix, prefix_len).
 *
 * Required fields:
 * - vrf_id: VRF identifier (use HAL_VRF_DEFAULT for main table)
 * - prefix: IPv4 network prefix
 * - prefix_len: Network mask length
 * - nexthop: Next hop IP address
 *
 * Optional fields:
 * - egress_port: Direct egress port (for connected routes)
 * - nexthop_mac: Next hop MAC address (avoids ARP lookup)
 * - priority: Route metric
 *
 * @param entry     Route entry (object_id populated on success)
 * @return          HAL_SUCCESS, HAL_E_EXISTS, HAL_E_FULL, or error
 */
hal_status_t hal_route_add(hal_route_entry_t *entry);

/**
 * Delete route entry by key
 *
 * @param vrf       VRF identifier
 * @param prefix    IPv4 prefix
 * @param prefix_len Prefix length
 * @return          HAL_SUCCESS, HAL_E_NOT_FOUND, or error
 */
hal_status_t hal_route_delete(hal_vrf_t vrf,
                              hal_ipv4_t prefix,
                              uint8_t prefix_len);

/**
 * Get route entry by key
 *
 * @param vrf       VRF identifier
 * @param prefix    IPv4 prefix
 * @param prefix_len Prefix length
 * @param entry     Output: populated entry
 * @return          HAL_SUCCESS, HAL_E_NOT_FOUND, or error
 */
hal_status_t hal_route_get(hal_vrf_t vrf,
                           hal_ipv4_t prefix,
                           uint8_t prefix_len,
                           hal_route_entry_t *entry);

/**
 * Update route entry
 *
 * Updates nexthop information for an existing route.
 *
 * @param entry     Entry with updated fields
 * @return          HAL_SUCCESS, HAL_E_NOT_FOUND, or error
 */
hal_status_t hal_route_update(const hal_route_entry_t *entry);

/* ============================================================================
 * Route Lookup
 * ============================================================================ */

/**
 * Longest prefix match (LPM) lookup
 *
 * Finds the best matching route for a destination IP.
 *
 * @param vrf       VRF identifier
 * @param dest_ip   Destination IP address
 * @param entry     Output: matching route entry
 * @return          HAL_SUCCESS, HAL_E_NOT_FOUND, or error
 */
hal_status_t hal_route_lookup(hal_vrf_t vrf,
                              hal_ipv4_t dest_ip,
                              hal_route_entry_t *entry);

/* ============================================================================
 * Route Bulk Operations
 * ============================================================================ */

/**
 * Delete all routes in a VRF
 *
 * @param vrf       VRF identifier
 * @return          HAL_SUCCESS or error
 */
hal_status_t hal_route_delete_by_vrf(hal_vrf_t vrf);

/**
 * Delete all routes with a specific nexthop
 *
 * Useful when a nexthop becomes unreachable.
 *
 * @param nexthop   Next hop IP address
 * @return          HAL_SUCCESS or error
 */
hal_status_t hal_route_delete_by_nexthop(hal_ipv4_t nexthop);

/* ============================================================================
 * Route Traversal
 * ============================================================================ */

/**
 * Route traverse callback
 */
typedef int (*hal_route_traverse_cb_t)(const hal_route_entry_t *entry,
                                       void *user_data);

/**
 * Traverse all routes
 *
 * @param callback  Function called for each route
 * @param user_data User context
 * @return          HAL_SUCCESS or error
 */
hal_status_t hal_route_traverse(hal_route_traverse_cb_t callback,
                                void *user_data);

/**
 * Traverse routes in a specific VRF
 *
 * @param vrf       VRF to traverse
 * @param callback  Function called for each route
 * @param user_data User context
 * @return          HAL_SUCCESS or error
 */
hal_status_t hal_route_traverse_vrf(hal_vrf_t vrf,
                                    hal_route_traverse_cb_t callback,
                                    void *user_data);

/* ============================================================================
 * Route Statistics
 * ============================================================================ */

typedef struct hal_route_stats_s {
    uint32_t count;             /* Current route count */
    uint32_t capacity;          /* Maximum capacity */
    uint32_t ipv4_count;        /* IPv4 routes */
    uint32_t ipv6_count;        /* IPv6 routes (future) */
} hal_route_stats_t;

/**
 * Get route table statistics
 *
 * @param stats     Output: statistics structure
 * @return          HAL_SUCCESS or error
 */
hal_status_t hal_route_stats_get(hal_route_stats_t *stats);

/**
 * Shutdown route subsystem
 *
 * Frees all entries and resets state. Called by hal_shutdown().
 */
void hal_route_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* HAL_ROUTE_H */

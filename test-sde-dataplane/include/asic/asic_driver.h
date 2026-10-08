/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2024 HAL Assessment Project
 *
 * asic_driver.h - ASIC driver interface (abstraction layer)
 *
 * This header defines the low-level interface to the ASIC hardware.
 * In production, this would be implemented by vendor SDK calls.
 * For this assessment, a mock implementation is provided.
 */

#ifndef ASIC_DRIVER_H
#define ASIC_DRIVER_H

#include "hal_types.h"
#include "hal_error.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * ASIC Unit Management
 * ============================================================================ */

/**
 * ASIC unit identifier
 * Supports multi-chip configurations (typically 0-3)
 */
typedef int asic_unit_t;

#define ASIC_UNIT_MAX       4
#define ASIC_UNIT_INVALID   (-1)

/**
 * ASIC capabilities structure
 */
typedef struct asic_capabilities_s {
    uint32_t    max_fdb_entries;        /* Max L2 FDB entries */
    uint32_t    max_route_entries;      /* Max L3 route entries */
    uint32_t    max_acl_entries;        /* Max ACL entries */
    uint32_t    max_nexthops;           /* Max nexthop entries */
    uint32_t    max_ecmp_groups;        /* Max ECMP groups */
    uint32_t    max_ecmp_members;       /* Max members per ECMP group */
    uint32_t    max_ports;              /* Max physical ports */
    uint32_t    max_vlans;              /* Max VLANs (typically 4K) */
    uint32_t    max_vrfs;               /* Max VRFs */
    bool        ipv6_support;           /* IPv6 routing support */
    bool        mpls_support;           /* MPLS support */
    bool        vxlan_support;          /* VXLAN support */
} asic_capabilities_t;

/**
 * Initialize ASIC unit
 *
 * @param unit      Unit number (0 to ASIC_UNIT_MAX-1)
 * @return          HAL_SUCCESS or error code
 */
hal_status_t asic_init(asic_unit_t unit);

/**
 * Shutdown ASIC unit
 *
 * @param unit      Unit number
 * @return          HAL_SUCCESS or error code
 */
hal_status_t asic_shutdown(asic_unit_t unit);

/**
 * Check if unit is initialized
 *
 * @param unit      Unit number
 * @return          true if initialized
 */
bool asic_is_initialized(asic_unit_t unit);

/**
 * Get ASIC capabilities
 *
 * @param unit      Unit number
 * @param caps      Output: capabilities structure
 * @return          HAL_SUCCESS or error code
 */
hal_status_t asic_capabilities_get(asic_unit_t unit, asic_capabilities_t *caps);

/* ============================================================================
 * L2 FDB Table Operations
 * ============================================================================ */

/**
 * Add L2 FDB entry to ASIC
 *
 * @param unit      Unit number
 * @param mac       MAC address
 * @param vlan      VLAN ID
 * @param port      Destination port
 * @param flags     Entry flags
 * @param oid       Output: object ID for the entry
 * @return          HAL_SUCCESS, HAL_E_FULL, or error
 */
hal_status_t asic_l2_add(asic_unit_t unit,
                         const hal_mac_t mac,
                         hal_vlan_t vlan,
                         hal_port_t port,
                         uint32_t flags,
                         hal_object_id_t *oid);

/**
 * Delete L2 FDB entry by object ID
 *
 * @param unit      Unit number
 * @param oid       Object ID from asic_l2_add
 * @return          HAL_SUCCESS or error code
 */
hal_status_t asic_l2_delete(asic_unit_t unit, hal_object_id_t oid);

/**
 * Delete L2 FDB entry by key (MAC + VLAN)
 *
 * @param unit      Unit number
 * @param mac       MAC address
 * @param vlan      VLAN ID
 * @return          HAL_SUCCESS, HAL_E_NOT_FOUND, or error
 */
hal_status_t asic_l2_delete_by_key(asic_unit_t unit,
                                   const hal_mac_t mac,
                                   hal_vlan_t vlan);

/**
 * Get L2 FDB entry by key
 *
 * @param unit      Unit number
 * @param mac       MAC address
 * @param vlan      VLAN ID
 * @param port      Output: destination port
 * @param flags     Output: entry flags (including HIT bit)
 * @param oid       Output: object ID
 * @return          HAL_SUCCESS, HAL_E_NOT_FOUND, or error
 */
hal_status_t asic_l2_get(asic_unit_t unit,
                         const hal_mac_t mac,
                         hal_vlan_t vlan,
                         hal_port_t *port,
                         uint32_t *flags,
                         hal_object_id_t *oid);

/**
 * Update L2 FDB entry flags
 *
 * @param unit      Unit number
 * @param oid       Object ID
 * @param flags     New flags value
 * @return          HAL_SUCCESS or error code
 */
hal_status_t asic_l2_flags_set(asic_unit_t unit,
                               hal_object_id_t oid,
                               uint32_t flags);

/**
 * Clear HIT bit for L2 entry
 *
 * @param unit      Unit number
 * @param oid       Object ID
 * @return          HAL_SUCCESS or error code
 */
hal_status_t asic_l2_hit_clear(asic_unit_t unit, hal_object_id_t oid);

/**
 * Delete all L2 entries by port
 *
 * @param unit      Unit number
 * @param port      Port to match
 * @param flags     Optional: only delete entries matching these flags (0 = all)
 * @return          HAL_SUCCESS or error code
 */
hal_status_t asic_l2_delete_by_port(asic_unit_t unit,
                                    hal_port_t port,
                                    uint32_t flags);

/**
 * Delete all L2 entries by VLAN
 *
 * @param unit      Unit number
 * @param vlan      VLAN ID to match
 * @param flags     Optional: only delete entries matching these flags (0 = all)
 * @return          HAL_SUCCESS or error code
 */
hal_status_t asic_l2_delete_by_vlan(asic_unit_t unit,
                                    hal_vlan_t vlan,
                                    uint32_t flags);

/**
 * L2 entry structure for traversal
 */
typedef struct asic_l2_entry_s {
    hal_mac_t       mac;
    hal_vlan_t      vlan;
    hal_port_t      port;
    uint32_t        flags;
    hal_object_id_t oid;
} asic_l2_entry_t;

/**
 * Traverse callback for L2 entries
 * Return 0 to continue, non-zero to stop
 */
typedef int (*asic_l2_traverse_cb_t)(asic_unit_t unit,
                                     const asic_l2_entry_t *entry,
                                     void *user_data);

/**
 * Traverse all L2 FDB entries
 *
 * @param unit      Unit number
 * @param callback  Function called for each entry
 * @param user_data User context passed to callback
 * @return          HAL_SUCCESS or error code
 */
hal_status_t asic_l2_traverse(asic_unit_t unit,
                              asic_l2_traverse_cb_t callback,
                              void *user_data);

/**
 * Get L2 table statistics
 *
 * @param unit      Unit number
 * @param count     Output: current entry count
 * @param capacity  Output: maximum capacity
 * @return          HAL_SUCCESS or error code
 */
hal_status_t asic_l2_stats_get(asic_unit_t unit,
                               uint32_t *count,
                               uint32_t *capacity);

/* ============================================================================
 * L3 Route Table Operations
 * ============================================================================ */

/**
 * Add L3 route entry to ASIC
 *
 * @param unit          Unit number
 * @param vrf           Virtual router ID
 * @param prefix        IPv4 prefix (host byte order)
 * @param prefix_len    Prefix length (0-32)
 * @param nexthop       Next hop IP address
 * @param egress_port   Egress port
 * @param nexthop_mac   Next hop MAC address (for direct routes)
 * @param oid           Output: object ID
 * @return              HAL_SUCCESS, HAL_E_FULL, or error
 */
hal_status_t asic_l3_route_add(asic_unit_t unit,
                               hal_vrf_t vrf,
                               hal_ipv4_t prefix,
                               uint8_t prefix_len,
                               hal_ipv4_t nexthop,
                               hal_port_t egress_port,
                               const hal_mac_t nexthop_mac,
                               hal_object_id_t *oid);

/**
 * Delete L3 route entry by object ID
 *
 * @param unit      Unit number
 * @param oid       Object ID from asic_l3_route_add
 * @return          HAL_SUCCESS or error code
 */
hal_status_t asic_l3_route_delete(asic_unit_t unit, hal_object_id_t oid);

/**
 * Delete L3 route entry by key (VRF + prefix)
 *
 * @param unit          Unit number
 * @param vrf           Virtual router ID
 * @param prefix        IPv4 prefix
 * @param prefix_len    Prefix length
 * @return              HAL_SUCCESS, HAL_E_NOT_FOUND, or error
 */
hal_status_t asic_l3_route_delete_by_key(asic_unit_t unit,
                                         hal_vrf_t vrf,
                                         hal_ipv4_t prefix,
                                         uint8_t prefix_len);

/**
 * L3 route entry structure for traversal
 */
typedef struct asic_l3_route_entry_s {
    hal_vrf_t       vrf;
    hal_ipv4_t      prefix;
    uint8_t         prefix_len;
    hal_ipv4_t      nexthop;
    hal_port_t      egress_port;
    hal_mac_t       nexthop_mac;
    hal_object_id_t oid;
} asic_l3_route_entry_t;

/**
 * Traverse callback for L3 routes
 */
typedef int (*asic_l3_route_traverse_cb_t)(asic_unit_t unit,
                                           const asic_l3_route_entry_t *entry,
                                           void *user_data);

/**
 * Traverse all L3 route entries
 */
hal_status_t asic_l3_route_traverse(asic_unit_t unit,
                                    asic_l3_route_traverse_cb_t callback,
                                    void *user_data);

/**
 * Get L3 route table statistics
 */
hal_status_t asic_l3_route_stats_get(asic_unit_t unit,
                                     uint32_t *count,
                                     uint32_t *capacity);

/* ============================================================================
 * Simulation Control (Mock Driver Only)
 * ============================================================================ */

/**
 * Set simulated operation latency
 *
 * @param unit          Unit number
 * @param latency_us    Latency in microseconds (0 = no delay)
 */
void asic_set_latency(asic_unit_t unit, uint32_t latency_us);

/**
 * Get simulated operation latency
 *
 * @param unit          Unit number
 * @return              Current latency in microseconds
 */
uint32_t asic_get_latency(asic_unit_t unit);

/**
 * Inject an error for testing
 *
 * @param unit          Unit number
 * @param error         Error to return on next operation
 * @param count         Number of operations to fail (0 = clear)
 */
void asic_inject_error(asic_unit_t unit, hal_status_t error, uint32_t count);

/**
 * Simulate hardware hit-bit updates
 * Marks random entries as "hit" to simulate traffic
 *
 * @param unit          Unit number
 * @param percentage    Percentage of entries to mark (0-100)
 */
void asic_simulate_hits(asic_unit_t unit, uint32_t percentage);

#ifdef __cplusplus
}
#endif

#endif /* ASIC_DRIVER_H */

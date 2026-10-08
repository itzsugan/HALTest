/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2024 HAL Assessment Project
 *
 * hal_types.h - Common type definitions for the HAL layer
 *
 * This header defines fundamental types used throughout the HAL API,
 * following patterns from industry-standard ASIC SDKs.
 */

#ifndef HAL_TYPES_H
#define HAL_TYPES_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Basic Types
 * ============================================================================ */

/**
 * MAC address type (6 bytes)
 * Stored in network byte order (big-endian)
 */
typedef uint8_t hal_mac_t[6];

/**
 * VLAN ID type
 * Valid range: 1-4094 (0 and 4095 are reserved)
 */
typedef uint16_t hal_vlan_t;

#define HAL_VLAN_MIN        1
#define HAL_VLAN_MAX        4094
#define HAL_VLAN_INVALID    0
#define HAL_VLAN_DEFAULT    1

/**
 * Physical port identifier
 * Format: (unit << 16) | port_num
 */
typedef uint32_t hal_port_t;

#define HAL_PORT_INVALID    0xFFFFFFFF
#define HAL_PORT_CPU        0xFFFFFFFE
#define HAL_PORT_ALL        0xFFFFFFFD

/* Port macros */
#define HAL_PORT_UNIT(p)    (((p) >> 16) & 0xFFFF)
#define HAL_PORT_NUM(p)     ((p) & 0xFFFF)
#define HAL_PORT_MAKE(u, n) ((((uint32_t)(u)) << 16) | ((n) & 0xFFFF))

/**
 * Virtual Router ID (VRF)
 * Used for L3 route table partitioning
 */
typedef uint16_t hal_vrf_t;

#define HAL_VRF_DEFAULT     0
#define HAL_VRF_MAX         4095

/* ============================================================================
 * IP Address Types
 * ============================================================================ */

/**
 * IPv4 address (host byte order)
 */
typedef uint32_t hal_ipv4_t;

/**
 * IPv6 address (network byte order)
 */
typedef struct hal_ipv6_s {
    uint8_t addr[16];
} hal_ipv6_t;

/**
 * Generic IP address (supports both v4 and v6)
 */
typedef struct hal_ip_addr_s {
    enum {
        HAL_IP_VERSION_4 = 4,
        HAL_IP_VERSION_6 = 6
    } version;
    union {
        hal_ipv4_t v4;
        hal_ipv6_t v6;
    } addr;
} hal_ip_addr_t;

/* IPv4 address macros */
#define HAL_IPV4(a, b, c, d) \
    ((((uint32_t)(a)) << 24) | (((uint32_t)(b)) << 16) | \
     (((uint32_t)(c)) << 8) | ((uint32_t)(d)))

#define HAL_IPV4_A(ip)      (((ip) >> 24) & 0xFF)
#define HAL_IPV4_B(ip)      (((ip) >> 16) & 0xFF)
#define HAL_IPV4_C(ip)      (((ip) >> 8) & 0xFF)
#define HAL_IPV4_D(ip)      ((ip) & 0xFF)

/* ============================================================================
 * Object Handle Types
 * ============================================================================ */

/**
 * Opaque object handle for ASIC resources
 * Used to reference entries programmed into hardware tables
 */
typedef uint64_t hal_object_id_t;

#define HAL_OBJECT_ID_INVALID   0ULL

/**
 * Object ID composition (internal):
 * Bits 63-56: Object type
 * Bits 55-48: Unit number
 * Bits 47-0:  Table index
 */
#define HAL_OID_TYPE(oid)   (((oid) >> 56) & 0xFF)
#define HAL_OID_UNIT(oid)   (((oid) >> 48) & 0xFF)
#define HAL_OID_INDEX(oid)  ((oid) & 0x0000FFFFFFFFFFFF)

typedef enum hal_object_type_e {
    HAL_OBJ_TYPE_FDB         = 0x01,
    HAL_OBJ_TYPE_ROUTE       = 0x02,
    HAL_OBJ_TYPE_NEXTHOP     = 0x03,
    HAL_OBJ_TYPE_ACL_TABLE   = 0x04,
    HAL_OBJ_TYPE_ACL_ENTRY   = 0x05,
    HAL_OBJ_TYPE_PORT        = 0x06,
    HAL_OBJ_TYPE_VLAN        = 0x07,
    HAL_OBJ_TYPE_COUNTER     = 0x08,
} hal_object_type_t;

/* ============================================================================
 * Common Flags
 * ============================================================================ */

/**
 * Entry flags used across multiple table types
 */
typedef enum hal_flags_e {
    HAL_FLAG_NONE           = 0,
    HAL_FLAG_STATIC         = (1 << 0),  /* Entry is static (not aged) */
    HAL_FLAG_HIT            = (1 << 1),  /* Entry was hit (hardware) */
    HAL_FLAG_COPY_TO_CPU    = (1 << 2),  /* Copy matching packets to CPU */
    HAL_FLAG_DROP           = (1 << 3),  /* Drop matching packets */
    HAL_FLAG_PENDING        = (1 << 4),  /* Entry pending programming */
    HAL_FLAG_MULTIPATH      = (1 << 5),  /* Entry uses ECMP */
} hal_flags_t;

/* ============================================================================
 * Callback Types
 * ============================================================================ */

/**
 * Generic traverse callback
 * Return 0 to continue, non-zero to stop traversal
 */
typedef int (*hal_traverse_cb_t)(void *entry, void *user_data);

/**
 * Event callback type
 */
typedef void (*hal_event_cb_t)(uint32_t event_type, void *event_data, void *user_data);

/* ============================================================================
 * Time Types
 * ============================================================================ */

/**
 * Timestamp in microseconds since HAL initialization
 */
typedef uint64_t hal_time_us_t;

/**
 * Get current timestamp
 */
hal_time_us_t hal_time_now(void);

/* ============================================================================
 * Utility Macros
 * ============================================================================ */

/* Array size */
#define HAL_ARRAY_SIZE(a)   (sizeof(a) / sizeof((a)[0]))

/* Min/Max */
#define HAL_MIN(a, b)       (((a) < (b)) ? (a) : (b))
#define HAL_MAX(a, b)       (((a) > (b)) ? (a) : (b))

/* Alignment */
#define HAL_ALIGN(x, a)     (((x) + ((a) - 1)) & ~((a) - 1))

/* Bit operations */
#define HAL_BIT(n)          (1UL << (n))
#define HAL_BIT_SET(v, b)   ((v) |= HAL_BIT(b))
#define HAL_BIT_CLR(v, b)   ((v) &= ~HAL_BIT(b))
#define HAL_BIT_TEST(v, b)  (((v) & HAL_BIT(b)) != 0)

/* MAC address comparison */
#define HAL_MAC_EQUAL(a, b) \
    ((a)[0] == (b)[0] && (a)[1] == (b)[1] && (a)[2] == (b)[2] && \
     (a)[3] == (b)[3] && (a)[4] == (b)[4] && (a)[5] == (b)[5])

#define HAL_MAC_IS_ZERO(m) \
    ((m)[0] == 0 && (m)[1] == 0 && (m)[2] == 0 && \
     (m)[3] == 0 && (m)[4] == 0 && (m)[5] == 0)

#define HAL_MAC_IS_BROADCAST(m) \
    ((m)[0] == 0xFF && (m)[1] == 0xFF && (m)[2] == 0xFF && \
     (m)[3] == 0xFF && (m)[4] == 0xFF && (m)[5] == 0xFF)

#define HAL_MAC_IS_MULTICAST(m) \
    (((m)[0] & 0x01) != 0)

/* MAC address copy */
#define HAL_MAC_COPY(dst, src) \
    do { \
        (dst)[0] = (src)[0]; (dst)[1] = (src)[1]; (dst)[2] = (src)[2]; \
        (dst)[3] = (src)[3]; (dst)[4] = (src)[4]; (dst)[5] = (src)[5]; \
    } while (0)

#define HAL_MAC_CLEAR(m) \
    do { \
        (m)[0] = 0; (m)[1] = 0; (m)[2] = 0; \
        (m)[3] = 0; (m)[4] = 0; (m)[5] = 0; \
    } while (0)

#ifdef __cplusplus
}
#endif

#endif /* HAL_TYPES_H */

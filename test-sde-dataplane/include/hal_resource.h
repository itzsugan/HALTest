/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2024 HAL Assessment Project
 *
 * hal_resource.h - Resource Pool Management
 *
 * Provides resource allocation tracking for ASIC table entries.
 * Useful for pre-validating resource availability before commits.
 */

#ifndef HAL_RESOURCE_H
#define HAL_RESOURCE_H

#include "hal_types.h"
#include "hal_error.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Resource Types
 * ============================================================================ */

/**
 * Resource pool types
 */
typedef enum hal_resource_type_e {
    HAL_RESOURCE_FDB_ENTRY,
    HAL_RESOURCE_ROUTE_ENTRY,
    HAL_RESOURCE_ACL_ENTRY,
    HAL_RESOURCE_COUNTER,
    HAL_RESOURCE_NEXTHOP,
    HAL_RESOURCE_ECMP_GROUP,
    HAL_RESOURCE_MAX
} hal_resource_type_t;

/**
 * Resource pool handle (opaque)
 */
typedef struct hal_resource_pool_s hal_resource_pool_t;

/* ============================================================================
 * Pool Management
 * ============================================================================ */

/**
 * Create a resource pool
 *
 * @param type      Resource type
 * @param capacity  Maximum pool capacity
 * @param pool      Output: pool handle
 * @return          HAL_SUCCESS or error
 */
hal_status_t hal_resource_pool_create(hal_resource_type_t type,
                                      uint32_t capacity,
                                      hal_resource_pool_t **pool);

/**
 * Destroy a resource pool
 *
 * All allocated resources must be freed first.
 *
 * @param pool      Pool handle
 * @return          HAL_SUCCESS, HAL_E_BUSY (if resources allocated), or error
 */
hal_status_t hal_resource_pool_destroy(hal_resource_pool_t *pool);

/* ============================================================================
 * Resource Allocation
 * ============================================================================ */

/**
 * Allocate resource from pool
 *
 * Returns a unique ID for the allocated resource.
 *
 * @param pool      Pool handle
 * @param id        Output: allocated resource ID
 * @return          HAL_SUCCESS, HAL_E_FULL, or error
 */
hal_status_t hal_resource_alloc(hal_resource_pool_t *pool,
                                hal_object_id_t *id);

/**
 * Free resource back to pool
 *
 * @param pool      Pool handle
 * @param id        Resource ID from hal_resource_alloc
 * @return          HAL_SUCCESS, HAL_E_NOT_FOUND, or error
 */
hal_status_t hal_resource_free(hal_resource_pool_t *pool,
                               hal_object_id_t id);

/**
 * Reserve multiple resources (without allocating)
 *
 * Useful for checking availability before committing a transaction.
 *
 * @param pool      Pool handle
 * @param count     Number of resources to reserve
 * @return          HAL_SUCCESS, HAL_E_RESOURCE, or error
 */
hal_status_t hal_resource_reserve(hal_resource_pool_t *pool,
                                  uint32_t count);

/**
 * Release reserved resources
 *
 * @param pool      Pool handle
 * @param count     Number of reservations to release
 * @return          HAL_SUCCESS or error
 */
hal_status_t hal_resource_unreserve(hal_resource_pool_t *pool,
                                    uint32_t count);

/* ============================================================================
 * Pool Statistics
 * ============================================================================ */

/**
 * Resource pool statistics
 */
typedef struct hal_resource_stats_s {
    uint32_t capacity;      /* Total capacity */
    uint32_t allocated;     /* Currently allocated */
    uint32_t reserved;      /* Currently reserved */
    uint32_t available;     /* Available (capacity - allocated - reserved) */
    uint32_t high_watermark; /* Peak allocation */
} hal_resource_stats_t;

/**
 * Get pool statistics
 *
 * @param pool      Pool handle
 * @param stats     Output: statistics
 * @return          HAL_SUCCESS or error
 */
hal_status_t hal_resource_stats_get(hal_resource_pool_t *pool,
                                    hal_resource_stats_t *stats);

/**
 * Get resource type name
 *
 * @param type      Resource type
 * @return          Human-readable name
 */
const char *hal_resource_type_str(hal_resource_type_t type);

/* ============================================================================
 * Global Resource Management
 * ============================================================================ */

/**
 * Get the system-wide pool for a resource type
 *
 * Returns the default pool created during HAL initialization.
 *
 * @param type      Resource type
 * @return          Pool handle or NULL if not initialized
 */
hal_resource_pool_t *hal_resource_get_pool(hal_resource_type_t type);

/**
 * Check global resource availability
 *
 * Quick check if resources are available across all pools.
 *
 * @param fdb_count     Required FDB entries
 * @param route_count   Required route entries
 * @param acl_count     Required ACL entries
 * @return              HAL_SUCCESS if available, HAL_E_RESOURCE if not
 */
hal_status_t hal_resource_check_available(uint32_t fdb_count,
                                          uint32_t route_count,
                                          uint32_t acl_count);

#ifdef __cplusplus
}
#endif

#endif /* HAL_RESOURCE_H */

/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2024 HAL Assessment Project
 *
 * hal_resource.c - Resource Pool Management Implementation
 */

#include "hal_resource.h"

#include <stdlib.h>
#include <string.h>
#include <pthread.h>

/* ============================================================================
 * Internal Data Structures
 * ============================================================================ */

struct hal_resource_pool_s {
    hal_resource_type_t type;
    uint32_t            capacity;
    uint32_t            allocated;
    uint32_t            reserved;
    uint32_t            high_watermark;
    pthread_mutex_t     lock;

    /* Bitmap for allocation tracking */
    uint64_t           *bitmap;
    uint32_t            bitmap_size;    /* Number of uint64_t elements */

    /* Next OID to allocate */
    hal_object_id_t     next_oid;
};

/* ============================================================================
 * Internal Helpers
 * ============================================================================ */

static inline uint32_t bitmap_words(uint32_t bits)
{
    return (bits + 63) / 64;
}

static inline bool bitmap_test(const uint64_t *bitmap, uint32_t bit)
{
    return (bitmap[bit / 64] & (1ULL << (bit % 64))) != 0;
}

static inline void bitmap_set(uint64_t *bitmap, uint32_t bit)
{
    bitmap[bit / 64] |= (1ULL << (bit % 64));
}

static inline void bitmap_clear(uint64_t *bitmap, uint32_t bit)
{
    bitmap[bit / 64] &= ~(1ULL << (bit % 64));
}

static int32_t bitmap_find_free(const uint64_t *bitmap, uint32_t capacity)
{
    uint32_t words = bitmap_words(capacity);

    for (uint32_t w = 0; w < words; w++) {
        if (bitmap[w] != ~0ULL) {
            /* Found a word with at least one free bit */
            for (int b = 0; b < 64; b++) {
                uint32_t idx = w * 64 + b;
                if (idx >= capacity) {
                    return -1;
                }
                if (!bitmap_test(bitmap, idx)) {
                    return (int32_t)idx;
                }
            }
        }
    }
    return -1;
}

/* ============================================================================
 * Pool Management
 * ============================================================================ */

hal_status_t hal_resource_pool_create(hal_resource_type_t type,
                                      uint32_t capacity,
                                      hal_resource_pool_t **pool)
{
    if (!pool || capacity == 0) {
        return HAL_E_PARAM;
    }
    if (type >= HAL_RESOURCE_MAX) {
        return HAL_E_RANGE;
    }

    hal_resource_pool_t *p = calloc(1, sizeof(hal_resource_pool_t));
    if (!p) {
        return HAL_E_MEMORY;
    }

    p->type = type;
    p->capacity = capacity;
    p->allocated = 0;
    p->reserved = 0;
    p->high_watermark = 0;

    /* Allocate bitmap */
    p->bitmap_size = bitmap_words(capacity);
    p->bitmap = calloc(p->bitmap_size, sizeof(uint64_t));
    if (!p->bitmap) {
        free(p);
        return HAL_E_MEMORY;
    }

    /* Initialize OID generator based on type */
    p->next_oid = ((uint64_t)type << 56) | 1;

    pthread_mutex_init(&p->lock, NULL);

    *pool = p;
    return HAL_SUCCESS;
}

hal_status_t hal_resource_pool_destroy(hal_resource_pool_t *pool)
{
    if (!pool) {
        return HAL_E_NULL;
    }

    pthread_mutex_lock(&pool->lock);

    if (pool->allocated > 0) {
        pthread_mutex_unlock(&pool->lock);
        return HAL_E_BUSY;
    }

    free(pool->bitmap);
    pthread_mutex_unlock(&pool->lock);
    pthread_mutex_destroy(&pool->lock);
    free(pool);

    return HAL_SUCCESS;
}

/* ============================================================================
 * Resource Allocation
 * ============================================================================ */

hal_status_t hal_resource_alloc(hal_resource_pool_t *pool, hal_object_id_t *id)
{
    if (!pool || !id) {
        return HAL_E_NULL;
    }

    pthread_mutex_lock(&pool->lock);

    if (pool->allocated + pool->reserved >= pool->capacity) {
        pthread_mutex_unlock(&pool->lock);
        return HAL_E_FULL;
    }

    /* Find free slot */
    int32_t idx = bitmap_find_free(pool->bitmap, pool->capacity);
    if (idx < 0) {
        pthread_mutex_unlock(&pool->lock);
        return HAL_E_FULL;
    }

    /* Mark as allocated */
    bitmap_set(pool->bitmap, (uint32_t)idx);
    pool->allocated++;

    if (pool->allocated > pool->high_watermark) {
        pool->high_watermark = pool->allocated;
    }

    /* Generate unique OID */
    *id = ((uint64_t)pool->type << 56) | ((uint64_t)idx & 0x00FFFFFFFFFFFFFF);

    pthread_mutex_unlock(&pool->lock);
    return HAL_SUCCESS;
}

hal_status_t hal_resource_free(hal_resource_pool_t *pool, hal_object_id_t id)
{
    if (!pool) {
        return HAL_E_NULL;
    }

    /* Validate OID type matches pool */
    uint8_t oid_type = (uint8_t)(id >> 56);
    if (oid_type != pool->type) {
        return HAL_E_PARAM;
    }

    uint64_t idx = id & 0x00FFFFFFFFFFFFFF;
    if (idx >= pool->capacity) {
        return HAL_E_RANGE;
    }

    pthread_mutex_lock(&pool->lock);

    if (!bitmap_test(pool->bitmap, (uint32_t)idx)) {
        pthread_mutex_unlock(&pool->lock);
        return HAL_E_NOT_FOUND;
    }

    bitmap_clear(pool->bitmap, (uint32_t)idx);
    pool->allocated--;

    pthread_mutex_unlock(&pool->lock);
    return HAL_SUCCESS;
}

hal_status_t hal_resource_reserve(hal_resource_pool_t *pool, uint32_t count)
{
    if (!pool) {
        return HAL_E_NULL;
    }
    if (count == 0) {
        return HAL_SUCCESS;
    }

    pthread_mutex_lock(&pool->lock);

    uint32_t available = pool->capacity - pool->allocated - pool->reserved;
    if (count > available) {
        pthread_mutex_unlock(&pool->lock);
        return HAL_E_RESOURCE;
    }

    pool->reserved += count;

    pthread_mutex_unlock(&pool->lock);
    return HAL_SUCCESS;
}

hal_status_t hal_resource_unreserve(hal_resource_pool_t *pool, uint32_t count)
{
    if (!pool) {
        return HAL_E_NULL;
    }
    if (count == 0) {
        return HAL_SUCCESS;
    }

    pthread_mutex_lock(&pool->lock);

    if (count > pool->reserved) {
        /* Can only unreserve what was reserved */
        pool->reserved = 0;
    } else {
        pool->reserved -= count;
    }

    pthread_mutex_unlock(&pool->lock);
    return HAL_SUCCESS;
}

/* ============================================================================
 * Statistics
 * ============================================================================ */

hal_status_t hal_resource_stats_get(hal_resource_pool_t *pool,
                                    hal_resource_stats_t *stats)
{
    if (!pool || !stats) {
        return HAL_E_NULL;
    }

    pthread_mutex_lock(&pool->lock);

    stats->capacity = pool->capacity;
    stats->allocated = pool->allocated;
    stats->reserved = pool->reserved;
    stats->available = pool->capacity - pool->allocated - pool->reserved;
    stats->high_watermark = pool->high_watermark;

    pthread_mutex_unlock(&pool->lock);
    return HAL_SUCCESS;
}

const char *hal_resource_type_str(hal_resource_type_t type)
{
    switch (type) {
        case HAL_RESOURCE_FDB_ENTRY:    return "FDB_ENTRY";
        case HAL_RESOURCE_ROUTE_ENTRY:  return "ROUTE_ENTRY";
        case HAL_RESOURCE_ACL_ENTRY:    return "ACL_ENTRY";
        case HAL_RESOURCE_COUNTER:      return "COUNTER";
        case HAL_RESOURCE_NEXTHOP:      return "NEXTHOP";
        case HAL_RESOURCE_ECMP_GROUP:   return "ECMP_GROUP";
        default:                        return "UNKNOWN";
    }
}

/* ============================================================================
 * Global Resource Check
 * ============================================================================ */

hal_status_t hal_resource_check_available(uint32_t fdb_count,
                                          uint32_t route_count,
                                          uint32_t acl_count)
{
    hal_resource_pool_t *pool;
    hal_resource_stats_t stats;

    if (fdb_count > 0) {
        pool = hal_resource_get_pool(HAL_RESOURCE_FDB_ENTRY);
        if (!pool) return HAL_E_INIT;
        hal_resource_stats_get(pool, &stats);
        if (fdb_count > stats.available) {
            return HAL_E_RESOURCE;
        }
    }

    if (route_count > 0) {
        pool = hal_resource_get_pool(HAL_RESOURCE_ROUTE_ENTRY);
        if (!pool) return HAL_E_INIT;
        hal_resource_stats_get(pool, &stats);
        if (route_count > stats.available) {
            return HAL_E_RESOURCE;
        }
    }

    if (acl_count > 0) {
        pool = hal_resource_get_pool(HAL_RESOURCE_ACL_ENTRY);
        if (!pool) return HAL_E_INIT;
        hal_resource_stats_get(pool, &stats);
        if (acl_count > stats.available) {
            return HAL_E_RESOURCE;
        }
    }

    return HAL_SUCCESS;
}

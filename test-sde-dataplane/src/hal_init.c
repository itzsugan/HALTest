/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2024 HAL Assessment Project
 *
 * hal_init.c - HAL Initialization Implementation
 */

#include "hal_init.h"
#include "hal_fdb.h"
#include "hal_route.h"
#include "hal_resource.h"
#include "asic/asic_driver.h"

#include <string.h>
#include <pthread.h>

/* ============================================================================
 * Version Information
 * ============================================================================ */

#define HAL_VERSION_MAJOR   1
#define HAL_VERSION_MINOR   0
#define HAL_VERSION_PATCH   0

#ifndef HAL_BUILD_DATE
#define HAL_BUILD_DATE      __DATE__
#endif

#ifndef HAL_GIT_HASH
#define HAL_GIT_HASH        "unknown"
#endif

/* ============================================================================
 * Global State
 * ============================================================================ */

static bool g_hal_initialized = false;
static hal_config_t g_hal_config;
static pthread_mutex_t g_hal_init_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_hal_unit_count = 0;

/* Resource pools (created during init) */
static hal_resource_pool_t *g_pools[HAL_RESOURCE_MAX] = {NULL};

/* ============================================================================
 * Configuration
 * ============================================================================ */

void hal_config_init(hal_config_t *config)
{
    if (!config) return;

    memset(config, 0, sizeof(*config));
    config->num_units = 1;
    config->fdb_size = 0;       /* Use ASIC default */
    config->route_size = 0;     /* Use ASIC default */
    config->acl_size = 0;       /* Use ASIC default */
    config->asic_latency_us = 1000;
    config->enable_logging = false;
}

/* ============================================================================
 * Lifecycle
 * ============================================================================ */

hal_status_t hal_init(const hal_config_t *config)
{
    hal_status_t rv = HAL_SUCCESS;

    pthread_mutex_lock(&g_hal_init_lock);

    if (g_hal_initialized) {
        pthread_mutex_unlock(&g_hal_init_lock);
        return HAL_SUCCESS;
    }

    /* Store configuration */
    if (config) {
        g_hal_config = *config;
    } else {
        hal_config_init(&g_hal_config);
    }

    /* Initialize ASIC units */
    for (uint32_t i = 0; i < g_hal_config.num_units; i++) {
        rv = asic_init((asic_unit_t)i);
        if (rv != HAL_SUCCESS) {
            /* Cleanup already initialized units */
            for (uint32_t j = 0; j < i; j++) {
                asic_shutdown((asic_unit_t)j);
            }
            pthread_mutex_unlock(&g_hal_init_lock);
            return rv;
        }

        /* Set configured latency */
        if (g_hal_config.asic_latency_us > 0) {
            asic_set_latency((asic_unit_t)i, g_hal_config.asic_latency_us);
        }
    }
    g_hal_unit_count = (int)g_hal_config.num_units;

    /* Get capabilities from primary ASIC */
    asic_capabilities_t caps;
    rv = asic_capabilities_get(0, &caps);
    if (rv != HAL_SUCCESS) {
        for (uint32_t i = 0; i < g_hal_config.num_units; i++) {
            asic_shutdown((asic_unit_t)i);
        }
        pthread_mutex_unlock(&g_hal_init_lock);
        return rv;
    }

    /* Create resource pools */
    uint32_t fdb_size = g_hal_config.fdb_size > 0 ?
                        g_hal_config.fdb_size : caps.max_fdb_entries;
    uint32_t route_size = g_hal_config.route_size > 0 ?
                          g_hal_config.route_size : caps.max_route_entries;
    uint32_t acl_size = g_hal_config.acl_size > 0 ?
                        g_hal_config.acl_size : caps.max_acl_entries;

    rv = hal_resource_pool_create(HAL_RESOURCE_FDB_ENTRY, fdb_size,
                                  &g_pools[HAL_RESOURCE_FDB_ENTRY]);
    if (rv != HAL_SUCCESS) goto cleanup;

    rv = hal_resource_pool_create(HAL_RESOURCE_ROUTE_ENTRY, route_size,
                                  &g_pools[HAL_RESOURCE_ROUTE_ENTRY]);
    if (rv != HAL_SUCCESS) goto cleanup;

    rv = hal_resource_pool_create(HAL_RESOURCE_ACL_ENTRY, acl_size,
                                  &g_pools[HAL_RESOURCE_ACL_ENTRY]);
    if (rv != HAL_SUCCESS) goto cleanup;

    rv = hal_resource_pool_create(HAL_RESOURCE_NEXTHOP, caps.max_nexthops,
                                  &g_pools[HAL_RESOURCE_NEXTHOP]);
    if (rv != HAL_SUCCESS) goto cleanup;

    g_hal_initialized = true;
    pthread_mutex_unlock(&g_hal_init_lock);
    return HAL_SUCCESS;

cleanup:
    /* Destroy any created pools */
    for (int i = 0; i < HAL_RESOURCE_MAX; i++) {
        if (g_pools[i]) {
            hal_resource_pool_destroy(g_pools[i]);
            g_pools[i] = NULL;
        }
    }

    /* Shutdown ASIC units */
    for (uint32_t i = 0; i < g_hal_config.num_units; i++) {
        asic_shutdown((asic_unit_t)i);
    }

    pthread_mutex_unlock(&g_hal_init_lock);
    return rv;
}

hal_status_t hal_shutdown(void)
{
    pthread_mutex_lock(&g_hal_init_lock);

    if (!g_hal_initialized) {
        pthread_mutex_unlock(&g_hal_init_lock);
        return HAL_SUCCESS;
    }

    /* Shutdown FDB subsystem (stops aging, frees entries) */
    hal_fdb_shutdown();

    /* Shutdown route subsystem */
    hal_route_shutdown();

    /* Destroy resource pools */
    for (int i = 0; i < HAL_RESOURCE_MAX; i++) {
        if (g_pools[i]) {
            hal_resource_pool_destroy(g_pools[i]);
            g_pools[i] = NULL;
        }
    }

    /* Shutdown ASIC units */
    for (int i = 0; i < g_hal_unit_count; i++) {
        asic_shutdown((asic_unit_t)i);
    }

    g_hal_initialized = false;
    g_hal_unit_count = 0;

    pthread_mutex_unlock(&g_hal_init_lock);
    return HAL_SUCCESS;
}

bool hal_is_initialized(void)
{
    return g_hal_initialized;
}

/* ============================================================================
 * Version
 * ============================================================================ */

hal_status_t hal_version_get(hal_version_t *version)
{
    if (!version) {
        return HAL_E_NULL;
    }

    version->major = HAL_VERSION_MAJOR;
    version->minor = HAL_VERSION_MINOR;
    version->patch = HAL_VERSION_PATCH;
    version->build_date = HAL_BUILD_DATE;
    version->git_hash = HAL_GIT_HASH;

    return HAL_SUCCESS;
}

/* ============================================================================
 * Unit Management
 * ============================================================================ */

int hal_unit_default(void)
{
    return 0;
}

int hal_unit_count(void)
{
    return g_hal_unit_count;
}

/* ============================================================================
 * Resource Pool Access
 * ============================================================================ */

hal_resource_pool_t *hal_resource_get_pool(hal_resource_type_t type)
{
    if (type >= HAL_RESOURCE_MAX) {
        return NULL;
    }
    return g_pools[type];
}

/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2024 HAL Assessment Project
 *
 * hal_init.h - HAL Initialization and Lifecycle Management
 */

#ifndef HAL_INIT_H
#define HAL_INIT_H

#include "hal_types.h"
#include "hal_error.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * HAL Configuration
 * ============================================================================ */

/**
 * HAL configuration structure
 *
 * Pass to hal_init() to customize initialization.
 * Use hal_config_init() for default values.
 */
typedef struct hal_config_s {
    uint32_t    num_units;          /* Number of ASIC units (default: 1) */
    uint32_t    fdb_size;           /* FDB table size (0 = default) */
    uint32_t    route_size;         /* Route table size (0 = default) */
    uint32_t    acl_size;           /* ACL table size (0 = default) */
    uint32_t    asic_latency_us;    /* Simulated ASIC latency (default: 1000) */
    bool        enable_logging;     /* Enable HAL logging (default: false) */
} hal_config_t;

/**
 * Initialize configuration with defaults
 *
 * @param config    Configuration structure to initialize
 */
void hal_config_init(hal_config_t *config);

/* ============================================================================
 * HAL Lifecycle
 * ============================================================================ */

/**
 * Initialize HAL subsystem
 *
 * Must be called before any other HAL functions.
 * Can be called multiple times (idempotent after first call).
 *
 * @param config    Configuration (NULL for defaults)
 * @return          HAL_SUCCESS or error
 */
hal_status_t hal_init(const hal_config_t *config);

/**
 * Shutdown HAL subsystem
 *
 * Releases all resources. Safe to call multiple times.
 *
 * @return          HAL_SUCCESS or error
 */
hal_status_t hal_shutdown(void);

/**
 * Check if HAL is initialized
 *
 * @return          true if hal_init() was successful
 */
bool hal_is_initialized(void);

/* ============================================================================
 * HAL Version Information
 * ============================================================================ */

/**
 * HAL version structure
 */
typedef struct hal_version_s {
    uint16_t major;
    uint16_t minor;
    uint16_t patch;
    const char *build_date;
    const char *git_hash;
} hal_version_t;

/**
 * Get HAL version information
 *
 * @param version   Output: version information
 * @return          HAL_SUCCESS or error
 */
hal_status_t hal_version_get(hal_version_t *version);

/* ============================================================================
 * Unit Management
 * ============================================================================ */

/**
 * Get the default (primary) ASIC unit
 *
 * @return          Unit number for primary ASIC
 */
int hal_unit_default(void);

/**
 * Get the number of initialized units
 *
 * @return          Number of ASIC units
 */
int hal_unit_count(void);

#ifdef __cplusplus
}
#endif

#endif /* HAL_INIT_H */

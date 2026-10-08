/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2024 HAL Assessment Project
 *
 * hal_error.h - Error codes and error handling utilities
 *
 * Defines the status codes returned by all HAL API functions.
 * Following SDK patterns, negative values indicate errors.
 */

#ifndef HAL_ERROR_H
#define HAL_ERROR_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * HAL status/error codes
 *
 * All HAL functions return hal_status_t:
 * - HAL_SUCCESS (0) indicates success
 * - Negative values indicate errors
 */
typedef enum hal_status_e {
    /* Success */
    HAL_SUCCESS         = 0,

    /* Parameter errors */
    HAL_E_PARAM         = -1,   /* Invalid parameter */
    HAL_E_NULL          = -2,   /* NULL pointer passed */
    HAL_E_RANGE         = -3,   /* Value out of range */

    /* Resource errors */
    HAL_E_MEMORY        = -10,  /* Memory allocation failed */
    HAL_E_RESOURCE      = -11,  /* Resource exhausted */
    HAL_E_FULL          = -12,  /* Table/buffer full */
    HAL_E_EMPTY         = -13,  /* Table/buffer empty */

    /* State errors */
    HAL_E_NOT_FOUND     = -20,  /* Entry not found */
    HAL_E_EXISTS        = -21,  /* Entry already exists */
    HAL_E_BUSY          = -22,  /* Resource busy */
    HAL_E_UNAVAIL       = -23,  /* Feature/resource unavailable */
    HAL_E_DISABLED      = -24,  /* Feature disabled */

    /* Operation errors */
    HAL_E_TIMEOUT       = -30,  /* Operation timed out */
    HAL_E_CANCELED      = -31,  /* Operation canceled */
    HAL_E_FAIL          = -32,  /* Operation failed */
    HAL_E_IO            = -33,  /* I/O error */

    /* Initialization errors */
    HAL_E_INIT          = -40,  /* Not initialized */
    HAL_E_CONFIG        = -41,  /* Configuration error */
    HAL_E_VERSION       = -42,  /* Version mismatch */

    /* Hardware errors */
    HAL_E_HW            = -50,  /* Hardware error */
    HAL_E_HW_ACCESS     = -51,  /* Hardware access error */
    HAL_E_HW_BUSY       = -52,  /* Hardware busy */

    /* Internal errors */
    HAL_E_INTERNAL      = -100, /* Internal error */
    HAL_E_NOT_IMPL      = -101, /* Not implemented */
    HAL_E_UNKNOWN       = -102, /* Unknown error */

} hal_status_t;

/**
 * Convert status code to string representation
 *
 * @param status    The status code
 * @return          Human-readable string (never NULL)
 */
const char *hal_status_str(hal_status_t status);

/**
 * Check if status indicates success
 */
#define HAL_SUCCESS_P(status) ((status) == HAL_SUCCESS)

/**
 * Check if status indicates failure
 */
#define HAL_FAILURE_P(status) ((status) != HAL_SUCCESS)

/**
 * Return from function if status indicates failure
 *
 * Usage:
 *   hal_status_t rv;
 *   rv = some_function();
 *   HAL_IF_ERROR_RETURN(rv);
 */
#define HAL_IF_ERROR_RETURN(status) \
    do { \
        hal_status_t _rv = (status); \
        if (HAL_FAILURE_P(_rv)) { \
            return _rv; \
        } \
    } while (0)

/**
 * Return from function if status indicates failure, with cleanup
 *
 * Usage:
 *   HAL_IF_ERROR_GOTO(rv, cleanup);
 */
#define HAL_IF_ERROR_GOTO(status, label) \
    do { \
        hal_status_t _rv = (status); \
        if (HAL_FAILURE_P(_rv)) { \
            goto label; \
        } \
    } while (0)

/**
 * Log and return if error
 * Note: Requires HAL_LOG_ERROR to be defined
 */
#ifdef HAL_LOG_ERROR
#define HAL_IF_ERROR_LOG_RETURN(status, fmt, ...) \
    do { \
        hal_status_t _rv = (status); \
        if (HAL_FAILURE_P(_rv)) { \
            HAL_LOG_ERROR(fmt ": %s", ##__VA_ARGS__, hal_status_str(_rv)); \
            return _rv; \
        } \
    } while (0)
#else
#define HAL_IF_ERROR_LOG_RETURN(status, fmt, ...) \
    HAL_IF_ERROR_RETURN(status)
#endif

/**
 * Assert condition and return error if false
 *
 * Usage:
 *   HAL_ASSERT_RETURN(ptr != NULL, HAL_E_NULL);
 */
#define HAL_ASSERT_RETURN(cond, error) \
    do { \
        if (!(cond)) { \
            return (error); \
        } \
    } while (0)

/**
 * Check for NULL pointer and return HAL_E_NULL
 */
#define HAL_NULL_CHECK(ptr) \
    HAL_ASSERT_RETURN((ptr) != NULL, HAL_E_NULL)

/**
 * Check for valid parameter and return HAL_E_PARAM
 */
#define HAL_PARAM_CHECK(cond) \
    HAL_ASSERT_RETURN(cond, HAL_E_PARAM)

/**
 * Check for valid range and return HAL_E_RANGE
 */
#define HAL_RANGE_CHECK(val, min, max) \
    HAL_ASSERT_RETURN((val) >= (min) && (val) <= (max), HAL_E_RANGE)

#ifdef __cplusplus
}
#endif

#endif /* HAL_ERROR_H */

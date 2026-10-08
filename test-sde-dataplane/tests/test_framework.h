/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2024 HAL Assessment Project
 *
 * test_framework.h - Minimal Test Framework
 *
 * A lightweight test framework for C, inspired by minunit.
 * No external dependencies required.
 */

#ifndef TEST_FRAMEWORK_H
#define TEST_FRAMEWORK_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

/* ============================================================================
 * Test Framework State
 * ============================================================================ */

static int tf_tests_run = 0;
static int tf_tests_passed = 0;
static int tf_tests_failed = 0;
static int tf_assertions = 0;
static const char *tf_current_test = NULL;
static bool tf_current_failed = false;

/* ============================================================================
 * Color Output (if terminal supports it)
 * ============================================================================ */

#define TF_COLOR_RESET   "\033[0m"
#define TF_COLOR_RED     "\033[31m"
#define TF_COLOR_GREEN   "\033[32m"
#define TF_COLOR_YELLOW  "\033[33m"
#define TF_COLOR_CYAN    "\033[36m"

/* Simple color macros - always use color for simplicity */
#define TF_RED(s)    TF_COLOR_RED s TF_COLOR_RESET
#define TF_GREEN(s)  TF_COLOR_GREEN s TF_COLOR_RESET
#define TF_YELLOW(s) TF_COLOR_YELLOW s TF_COLOR_RESET
#define TF_CYAN(s)   TF_COLOR_CYAN s TF_COLOR_RESET

/* ============================================================================
 * Test Macros
 * ============================================================================ */

/**
 * Define a test function
 */
#define TEST(name) static void test_##name(void)

/**
 * Run a test function
 */
#define RUN_TEST(name) do { \
    tf_current_test = #name; \
    tf_current_failed = false; \
    tf_tests_run++; \
    printf("  Running: %s... ", #name); \
    fflush(stdout); \
    test_##name(); \
    if (tf_current_failed) { \
        tf_tests_failed++; \
        printf(TF_RED("FAILED") "\n"); \
    } else { \
        tf_tests_passed++; \
        printf(TF_GREEN("OK") "\n"); \
    } \
} while (0)

/**
 * Assert that a condition is true
 */
#define ASSERT(cond) do { \
    tf_assertions++; \
    if (!(cond)) { \
        printf("\n    " TF_RED("ASSERT FAILED") ": %s\n", #cond); \
        printf("    at %s:%d\n", __FILE__, __LINE__); \
        tf_current_failed = true; \
        return; \
    } \
} while (0)

/**
 * Assert that a condition is true with custom message
 */
#define ASSERT_MSG(cond, msg) do { \
    tf_assertions++; \
    if (!(cond)) { \
        printf("\n    " TF_RED("ASSERT FAILED") ": %s\n", msg); \
        printf("    condition: %s\n", #cond); \
        printf("    at %s:%d\n", __FILE__, __LINE__); \
        tf_current_failed = true; \
        return; \
    } \
} while (0)

/**
 * Assert equality (integers)
 */
#define ASSERT_EQ(expected, actual) do { \
    tf_assertions++; \
    long long _exp = (long long)(expected); \
    long long _act = (long long)(actual); \
    if (_exp != _act) { \
        printf("\n    " TF_RED("ASSERT_EQ FAILED") "\n"); \
        printf("    expected: %lld\n", _exp); \
        printf("    actual:   %lld\n", _act); \
        printf("    at %s:%d\n", __FILE__, __LINE__); \
        tf_current_failed = true; \
        return; \
    } \
} while (0)

/**
 * Assert not equal (integers)
 */
#define ASSERT_NE(unexpected, actual) do { \
    tf_assertions++; \
    long long _unexp = (long long)(unexpected); \
    long long _act = (long long)(actual); \
    if (_unexp == _act) { \
        printf("\n    " TF_RED("ASSERT_NE FAILED") "\n"); \
        printf("    unexpected: %lld\n", _unexp); \
        printf("    actual:     %lld\n", _act); \
        printf("    at %s:%d\n", __FILE__, __LINE__); \
        tf_current_failed = true; \
        return; \
    } \
} while (0)

/**
 * Assert string equality
 */
#define ASSERT_STR_EQ(expected, actual) do { \
    tf_assertions++; \
    const char *_exp = (expected); \
    const char *_act = (actual); \
    if (_exp == NULL || _act == NULL || strcmp(_exp, _act) != 0) { \
        printf("\n    " TF_RED("ASSERT_STR_EQ FAILED") "\n"); \
        printf("    expected: \"%s\"\n", _exp ? _exp : "(null)"); \
        printf("    actual:   \"%s\"\n", _act ? _act : "(null)"); \
        printf("    at %s:%d\n", __FILE__, __LINE__); \
        tf_current_failed = true; \
        return; \
    } \
} while (0)

/**
 * Assert pointer is not NULL
 */
#define ASSERT_NOT_NULL(ptr) do { \
    tf_assertions++; \
    if ((ptr) == NULL) { \
        printf("\n    " TF_RED("ASSERT_NOT_NULL FAILED") ": %s\n", #ptr); \
        printf("    at %s:%d\n", __FILE__, __LINE__); \
        tf_current_failed = true; \
        return; \
    } \
} while (0)

/**
 * Assert pointer is NULL
 */
#define ASSERT_NULL(ptr) do { \
    tf_assertions++; \
    if ((ptr) != NULL) { \
        printf("\n    " TF_RED("ASSERT_NULL FAILED") ": %s = %p\n", #ptr, (void*)(ptr)); \
        printf("    at %s:%d\n", __FILE__, __LINE__); \
        tf_current_failed = true; \
        return; \
    } \
} while (0)

/**
 * Assert HAL status is success
 */
#define ASSERT_SUCCESS(status) do { \
    tf_assertions++; \
    hal_status_t _st = (status); \
    if (_st != HAL_SUCCESS) { \
        printf("\n    " TF_RED("ASSERT_SUCCESS FAILED") "\n"); \
        printf("    status: %s (%d)\n", hal_status_str(_st), _st); \
        printf("    at %s:%d\n", __FILE__, __LINE__); \
        tf_current_failed = true; \
        return; \
    } \
} while (0)

/**
 * Assert HAL status matches expected error
 */
#define ASSERT_STATUS(expected, actual) do { \
    tf_assertions++; \
    hal_status_t _exp = (expected); \
    hal_status_t _act = (actual); \
    if (_exp != _act) { \
        printf("\n    " TF_RED("ASSERT_STATUS FAILED") "\n"); \
        printf("    expected: %s (%d)\n", hal_status_str(_exp), _exp); \
        printf("    actual:   %s (%d)\n", hal_status_str(_act), _act); \
        printf("    at %s:%d\n", __FILE__, __LINE__); \
        tf_current_failed = true; \
        return; \
    } \
} while (0)

/**
 * Assert MAC addresses are equal
 */
#define ASSERT_MAC_EQ(expected, actual) do { \
    tf_assertions++; \
    const uint8_t *_exp = (expected); \
    const uint8_t *_act = (actual); \
    if (!HAL_MAC_EQUAL(_exp, _act)) { \
        printf("\n    " TF_RED("ASSERT_MAC_EQ FAILED") "\n"); \
        printf("    expected: %02x:%02x:%02x:%02x:%02x:%02x\n", \
               _exp[0], _exp[1], _exp[2], _exp[3], _exp[4], _exp[5]); \
        printf("    actual:   %02x:%02x:%02x:%02x:%02x:%02x\n", \
               _act[0], _act[1], _act[2], _act[3], _act[4], _act[5]); \
        printf("    at %s:%d\n", __FILE__, __LINE__); \
        tf_current_failed = true; \
        return; \
    } \
} while (0)

/**
 * Skip a test (with reason)
 */
#define SKIP_TEST(reason) do { \
    printf(TF_YELLOW("SKIPPED") " (%s)\n", reason); \
    return; \
} while (0)

/* ============================================================================
 * Test Suite
 * ============================================================================ */

/**
 * Begin a test suite
 */
#define TEST_SUITE_BEGIN(name) do { \
    printf("\n" TF_CYAN("=== Test Suite: %s ===") "\n\n", name); \
} while (0)

/**
 * End test suite and print summary
 */
#define TEST_SUITE_END() do { \
    printf("\n" TF_CYAN("=== Summary ===") "\n"); \
    printf("  Tests:      %d\n", tf_tests_run); \
    printf("  Passed:     " TF_GREEN("%d") "\n", tf_tests_passed); \
    if (tf_tests_failed > 0) { \
        printf("  Failed:     " TF_RED("%d") "\n", tf_tests_failed); \
    } else { \
        printf("  Failed:     %d\n", tf_tests_failed); \
    } \
    printf("  Assertions: %d\n\n", tf_assertions); \
} while (0)

/**
 * Return exit code based on test results
 */
#define TEST_EXIT_CODE() (tf_tests_failed > 0 ? 1 : 0)

/* ============================================================================
 * Setup/Teardown
 * ============================================================================ */

/**
 * Define setup function (called before each test)
 */
#define TEST_SETUP() static void test_setup(void)

/**
 * Define teardown function (called after each test)
 */
#define TEST_TEARDOWN() static void test_teardown(void)

/**
 * Run test with setup/teardown
 */
#define RUN_TEST_WITH_FIXTURE(name) do { \
    test_setup(); \
    RUN_TEST(name); \
    test_teardown(); \
} while (0)

#endif /* TEST_FRAMEWORK_H */

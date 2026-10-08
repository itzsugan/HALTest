/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2024 HAL Assessment Project
 *
 * test_route.c - Route Table Unit Tests
 */

#include "test_framework.h"
#include "hal_init.h"
#include "hal_route.h"
#include "hal_types.h"

#include <string.h>

/* ============================================================================
 * Test Fixtures
 * ============================================================================ */

static hal_config_t test_config;

TEST_SETUP()
{
    hal_config_init(&test_config);
    test_config.asic_latency_us = 0;
    hal_init(&test_config);
}

TEST_TEARDOWN()
{
    hal_shutdown();
}

/* ============================================================================
 * Basic CRUD Tests
 * ============================================================================ */

TEST(route_entry_init)
{
    hal_route_entry_t entry;

    hal_route_entry_init(&entry);

    ASSERT_EQ(HAL_VRF_DEFAULT, entry.vrf_id);
    ASSERT_EQ(0, entry.prefix);
    ASSERT_EQ(0, entry.prefix_len);
    ASSERT_EQ(HAL_PORT_INVALID, entry.egress_port);
    ASSERT_EQ(HAL_OBJECT_ID_INVALID, entry.object_id);
}

TEST(route_add_basic)
{
    hal_route_entry_t entry;

    hal_route_entry_init(&entry);
    entry.prefix = HAL_IPV4(10, 0, 0, 0);
    entry.prefix_len = 24;
    entry.nexthop = HAL_IPV4(192, 168, 1, 1);
    entry.egress_port = HAL_PORT_MAKE(0, 1);

    hal_status_t rv = hal_route_add(&entry);
    ASSERT_SUCCESS(rv);
    ASSERT_NE(HAL_OBJECT_ID_INVALID, entry.object_id);
}

TEST(route_add_duplicate)
{
    hal_route_entry_t entry;

    hal_route_entry_init(&entry);
    entry.prefix = HAL_IPV4(10, 1, 0, 0);
    entry.prefix_len = 24;
    entry.nexthop = HAL_IPV4(192, 168, 1, 1);

    ASSERT_SUCCESS(hal_route_add(&entry));

    /* Try to add duplicate */
    hal_status_t rv = hal_route_add(&entry);
    ASSERT_STATUS(HAL_E_EXISTS, rv);
}

TEST(route_add_invalid_params)
{
    hal_route_entry_t entry;

    /* NULL entry */
    ASSERT_STATUS(HAL_E_NULL, hal_route_add(NULL));

    /* Invalid prefix length */
    hal_route_entry_init(&entry);
    entry.prefix = HAL_IPV4(10, 0, 0, 0);
    entry.prefix_len = 33;  /* Invalid: max is 32 */
    entry.nexthop = HAL_IPV4(192, 168, 1, 1);
    ASSERT_STATUS(HAL_E_RANGE, hal_route_add(&entry));
}

TEST(route_get_basic)
{
    hal_route_entry_t entry, retrieved;

    hal_route_entry_init(&entry);
    entry.vrf_id = HAL_VRF_DEFAULT;
    entry.prefix = HAL_IPV4(172, 16, 0, 0);
    entry.prefix_len = 16;
    entry.nexthop = HAL_IPV4(192, 168, 1, 254);
    entry.egress_port = HAL_PORT_MAKE(0, 10);

    ASSERT_SUCCESS(hal_route_add(&entry));

    hal_status_t rv = hal_route_get(HAL_VRF_DEFAULT,
                                    HAL_IPV4(172, 16, 0, 0), 16,
                                    &retrieved);
    ASSERT_SUCCESS(rv);
    ASSERT_EQ(HAL_IPV4(172, 16, 0, 0), retrieved.prefix);
    ASSERT_EQ(16, retrieved.prefix_len);
    ASSERT_EQ(HAL_IPV4(192, 168, 1, 254), retrieved.nexthop);
    ASSERT_EQ(HAL_PORT_MAKE(0, 10), retrieved.egress_port);
}

TEST(route_get_not_found)
{
    hal_route_entry_t entry;

    hal_status_t rv = hal_route_get(HAL_VRF_DEFAULT,
                                    HAL_IPV4(10, 99, 0, 0), 24,
                                    &entry);
    ASSERT_STATUS(HAL_E_NOT_FOUND, rv);
}

TEST(route_delete_basic)
{
    hal_route_entry_t entry;

    hal_route_entry_init(&entry);
    entry.prefix = HAL_IPV4(10, 2, 0, 0);
    entry.prefix_len = 24;
    entry.nexthop = HAL_IPV4(192, 168, 1, 1);

    ASSERT_SUCCESS(hal_route_add(&entry));
    ASSERT_SUCCESS(hal_route_delete(HAL_VRF_DEFAULT,
                                    HAL_IPV4(10, 2, 0, 0), 24));

    /* Verify deleted */
    ASSERT_STATUS(HAL_E_NOT_FOUND,
                  hal_route_get(HAL_VRF_DEFAULT,
                               HAL_IPV4(10, 2, 0, 0), 24, &entry));
}

TEST(route_update_basic)
{
    hal_route_entry_t entry, updated;

    hal_route_entry_init(&entry);
    entry.prefix = HAL_IPV4(10, 3, 0, 0);
    entry.prefix_len = 24;
    entry.nexthop = HAL_IPV4(192, 168, 1, 1);
    entry.egress_port = HAL_PORT_MAKE(0, 1);

    ASSERT_SUCCESS(hal_route_add(&entry));

    /* Update nexthop */
    entry.nexthop = HAL_IPV4(192, 168, 1, 2);
    entry.egress_port = HAL_PORT_MAKE(0, 2);
    ASSERT_SUCCESS(hal_route_update(&entry));

    /* Verify update */
    ASSERT_SUCCESS(hal_route_get(HAL_VRF_DEFAULT,
                                 HAL_IPV4(10, 3, 0, 0), 24, &updated));
    ASSERT_EQ(HAL_IPV4(192, 168, 1, 2), updated.nexthop);
    ASSERT_EQ(HAL_PORT_MAKE(0, 2), updated.egress_port);
}

/* ============================================================================
 * LPM Tests
 * ============================================================================ */

TEST(route_lpm_exact)
{
    hal_route_entry_t entry, result;

    /* Add /24 route */
    hal_route_entry_init(&entry);
    entry.prefix = HAL_IPV4(10, 10, 10, 0);
    entry.prefix_len = 24;
    entry.nexthop = HAL_IPV4(192, 168, 1, 1);
    ASSERT_SUCCESS(hal_route_add(&entry));

    /* Lookup should match exactly */
    hal_status_t rv = hal_route_lookup(HAL_VRF_DEFAULT,
                                       HAL_IPV4(10, 10, 10, 100),
                                       &result);
    ASSERT_SUCCESS(rv);
    ASSERT_EQ(HAL_IPV4(10, 10, 10, 0), result.prefix);
    ASSERT_EQ(24, result.prefix_len);
}

TEST(route_lpm_longest_match)
{
    hal_route_entry_t entry, result;

    /* Add /8 route */
    hal_route_entry_init(&entry);
    entry.prefix = HAL_IPV4(10, 0, 0, 0);
    entry.prefix_len = 8;
    entry.nexthop = HAL_IPV4(192, 168, 1, 1);
    ASSERT_SUCCESS(hal_route_add(&entry));

    /* Add /16 route (more specific) */
    hal_route_entry_init(&entry);
    entry.prefix = HAL_IPV4(10, 20, 0, 0);
    entry.prefix_len = 16;
    entry.nexthop = HAL_IPV4(192, 168, 1, 2);
    ASSERT_SUCCESS(hal_route_add(&entry));

    /* Add /24 route (most specific) */
    hal_route_entry_init(&entry);
    entry.prefix = HAL_IPV4(10, 20, 30, 0);
    entry.prefix_len = 24;
    entry.nexthop = HAL_IPV4(192, 168, 1, 3);
    ASSERT_SUCCESS(hal_route_add(&entry));

    /* Lookup in /24 subnet should match /24 */
    hal_status_t rv = hal_route_lookup(HAL_VRF_DEFAULT,
                                       HAL_IPV4(10, 20, 30, 50),
                                       &result);
    ASSERT_SUCCESS(rv);
    ASSERT_EQ(24, result.prefix_len);
    ASSERT_EQ(HAL_IPV4(192, 168, 1, 3), result.nexthop);

    /* Lookup in /16 (but not /24) should match /16 */
    rv = hal_route_lookup(HAL_VRF_DEFAULT,
                          HAL_IPV4(10, 20, 40, 50),
                          &result);
    ASSERT_SUCCESS(rv);
    ASSERT_EQ(16, result.prefix_len);
    ASSERT_EQ(HAL_IPV4(192, 168, 1, 2), result.nexthop);

    /* Lookup in /8 (but not /16 or /24) should match /8 */
    rv = hal_route_lookup(HAL_VRF_DEFAULT,
                          HAL_IPV4(10, 99, 99, 99),
                          &result);
    ASSERT_SUCCESS(rv);
    ASSERT_EQ(8, result.prefix_len);
    ASSERT_EQ(HAL_IPV4(192, 168, 1, 1), result.nexthop);
}

TEST(route_lpm_default_route)
{
    hal_route_entry_t entry, result;

    /* Add default route */
    hal_route_entry_init(&entry);
    entry.prefix = 0;
    entry.prefix_len = 0;
    entry.nexthop = HAL_IPV4(192, 168, 1, 1);
    ASSERT_SUCCESS(hal_route_add(&entry));

    /* Any IP should match default */
    hal_status_t rv = hal_route_lookup(HAL_VRF_DEFAULT,
                                       HAL_IPV4(8, 8, 8, 8),
                                       &result);
    ASSERT_SUCCESS(rv);
    ASSERT_EQ(0, result.prefix_len);
}

TEST(route_lpm_no_match)
{
    hal_route_entry_t entry, result;

    /* Add specific route */
    hal_route_entry_init(&entry);
    entry.prefix = HAL_IPV4(10, 0, 0, 0);
    entry.prefix_len = 8;
    entry.nexthop = HAL_IPV4(192, 168, 1, 1);
    ASSERT_SUCCESS(hal_route_add(&entry));

    /* Lookup outside /8 should fail (no default route) */
    hal_status_t rv = hal_route_lookup(HAL_VRF_DEFAULT,
                                       HAL_IPV4(172, 16, 0, 1),
                                       &result);
    ASSERT_STATUS(HAL_E_NOT_FOUND, rv);
}

/* ============================================================================
 * VRF Tests
 * ============================================================================ */

TEST(route_vrf_isolation)
{
    hal_route_entry_t entry, result;

    /* Add route in VRF 1 */
    hal_route_entry_init(&entry);
    entry.vrf_id = 1;
    entry.prefix = HAL_IPV4(10, 0, 0, 0);
    entry.prefix_len = 8;
    entry.nexthop = HAL_IPV4(192, 168, 1, 1);
    ASSERT_SUCCESS(hal_route_add(&entry));

    /* Add same prefix in VRF 2 with different nexthop */
    hal_route_entry_init(&entry);
    entry.vrf_id = 2;
    entry.prefix = HAL_IPV4(10, 0, 0, 0);
    entry.prefix_len = 8;
    entry.nexthop = HAL_IPV4(192, 168, 2, 1);
    ASSERT_SUCCESS(hal_route_add(&entry));

    /* Lookup in VRF 1 */
    hal_status_t rv = hal_route_lookup(1, HAL_IPV4(10, 0, 0, 1), &result);
    ASSERT_SUCCESS(rv);
    ASSERT_EQ(HAL_IPV4(192, 168, 1, 1), result.nexthop);

    /* Lookup in VRF 2 */
    rv = hal_route_lookup(2, HAL_IPV4(10, 0, 0, 1), &result);
    ASSERT_SUCCESS(rv);
    ASSERT_EQ(HAL_IPV4(192, 168, 2, 1), result.nexthop);

    /* Lookup in VRF 0 should fail (no route there) */
    rv = hal_route_lookup(HAL_VRF_DEFAULT, HAL_IPV4(10, 0, 0, 1), &result);
    ASSERT_STATUS(HAL_E_NOT_FOUND, rv);
}

/* ============================================================================
 * Traversal Tests
 * ============================================================================ */

static int route_traverse_count = 0;
static int route_traverse_callback(const hal_route_entry_t *entry, void *user_data)
{
    (void)entry;
    (void)user_data;
    route_traverse_count++;
    return 0;
}

TEST(route_traverse)
{
    hal_route_entry_t entry;

    /* Add some routes */
    for (int i = 0; i < 10; i++) {
        hal_route_entry_init(&entry);
        entry.prefix = HAL_IPV4(10, i, 0, 0);
        entry.prefix_len = 16;
        entry.nexthop = HAL_IPV4(192, 168, 1, i + 1);
        ASSERT_SUCCESS(hal_route_add(&entry));
    }

    route_traverse_count = 0;
    ASSERT_SUCCESS(hal_route_traverse(route_traverse_callback, NULL));
    ASSERT_EQ(10, route_traverse_count);
}

/* ============================================================================
 * Statistics Tests
 * ============================================================================ */

TEST(route_stats)
{
    hal_route_entry_t entry;
    hal_route_stats_t stats;

    ASSERT_SUCCESS(hal_route_stats_get(&stats));
    ASSERT_EQ(0, stats.count);

    /* Add some routes */
    for (int i = 0; i < 5; i++) {
        hal_route_entry_init(&entry);
        entry.prefix = HAL_IPV4(192, 168, i, 0);
        entry.prefix_len = 24;
        entry.nexthop = HAL_IPV4(10, 0, 0, i + 1);
        ASSERT_SUCCESS(hal_route_add(&entry));
    }

    ASSERT_SUCCESS(hal_route_stats_get(&stats));
    ASSERT_EQ(5, stats.count);
    ASSERT_EQ(5, stats.ipv4_count);
    ASSERT(stats.capacity > 0);
}

/* ============================================================================
 * Main
 * ============================================================================ */

static void run_basic_tests(void)
{
    TEST_SUITE_BEGIN("Route Basic Tests");

    RUN_TEST_WITH_FIXTURE(route_entry_init);
    RUN_TEST_WITH_FIXTURE(route_add_basic);
    RUN_TEST_WITH_FIXTURE(route_add_duplicate);
    RUN_TEST_WITH_FIXTURE(route_add_invalid_params);
    RUN_TEST_WITH_FIXTURE(route_get_basic);
    RUN_TEST_WITH_FIXTURE(route_get_not_found);
    RUN_TEST_WITH_FIXTURE(route_delete_basic);
    RUN_TEST_WITH_FIXTURE(route_update_basic);
    RUN_TEST_WITH_FIXTURE(route_traverse);
    RUN_TEST_WITH_FIXTURE(route_stats);

    TEST_SUITE_END();
}

static void run_lpm_tests(void)
{
    TEST_SUITE_BEGIN("Route LPM Tests");

    RUN_TEST_WITH_FIXTURE(route_lpm_exact);
    RUN_TEST_WITH_FIXTURE(route_lpm_longest_match);
    RUN_TEST_WITH_FIXTURE(route_lpm_default_route);
    RUN_TEST_WITH_FIXTURE(route_lpm_no_match);
    RUN_TEST_WITH_FIXTURE(route_vrf_isolation);

    TEST_SUITE_END();
}

int main(int argc, char *argv[])
{
    bool run_all = true;
    bool run_basic = false;
    bool run_lpm = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--basic") == 0) {
            run_basic = true;
            run_all = false;
        } else if (strcmp(argv[i], "--lpm") == 0) {
            run_lpm = true;
            run_all = false;
        }
    }

    if (run_all || run_basic) {
        run_basic_tests();
    }
    if (run_all || run_lpm) {
        run_lpm_tests();
    }

    return TEST_EXIT_CODE();
}

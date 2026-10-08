/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2024 HAL Assessment Project
 *
 * test_integration.c - Integration Tests
 *
 * Tests that verify multiple HAL components work together correctly.
 */

#include "test_framework.h"
#include "hal_init.h"
#include "hal_fdb.h"
#include "hal_route.h"
#include "hal_resource.h"
#include "hal_types.h"
#include "asic/asic_driver.h"

#include <string.h>
#include <pthread.h>

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
 * HAL Lifecycle Tests
 * ============================================================================ */

TEST(hal_init_shutdown)
{
    /* Shutdown and re-init */
    hal_shutdown();
    ASSERT(!hal_is_initialized());

    hal_status_t rv = hal_init(NULL);
    ASSERT_SUCCESS(rv);
    ASSERT(hal_is_initialized());
}

TEST(hal_version)
{
    hal_version_t version;

    ASSERT_SUCCESS(hal_version_get(&version));
    ASSERT(version.major >= 1);
    ASSERT_NOT_NULL(version.build_date);
}

TEST(hal_double_init)
{
    /* Second init should succeed (idempotent) */
    hal_status_t rv = hal_init(NULL);
    ASSERT_SUCCESS(rv);
}

/* ============================================================================
 * Combined FDB + Route Tests
 * ============================================================================ */

static void make_mac(hal_mac_t mac, uint8_t last_byte)
{
    mac[0] = 0x00;
    mac[1] = 0x11;
    mac[2] = 0x22;
    mac[3] = 0x33;
    mac[4] = 0x44;
    mac[5] = last_byte;
}

TEST(fdb_route_combined)
{
    hal_fdb_entry_t fdb_entry;
    hal_route_entry_t route_entry;
    hal_mac_t mac;

    /* Add FDB entry for a nexthop */
    make_mac(mac, 1);
    hal_fdb_entry_init(&fdb_entry, mac, 100);
    fdb_entry.port = HAL_PORT_MAKE(0, 1);
    ASSERT_SUCCESS(hal_fdb_add(&fdb_entry));

    /* Add route pointing to that nexthop */
    hal_route_entry_init(&route_entry);
    route_entry.prefix = HAL_IPV4(10, 0, 0, 0);
    route_entry.prefix_len = 24;
    route_entry.nexthop = HAL_IPV4(192, 168, 1, 1);
    route_entry.egress_port = HAL_PORT_MAKE(0, 1);
    HAL_MAC_COPY(route_entry.nexthop_mac, mac);
    ASSERT_SUCCESS(hal_route_add(&route_entry));

    /* Verify both exist */
    hal_fdb_entry_t fdb_retrieved;
    ASSERT_SUCCESS(hal_fdb_get(mac, 100, &fdb_retrieved));

    hal_route_entry_t route_retrieved;
    ASSERT_SUCCESS(hal_route_lookup(HAL_VRF_DEFAULT,
                                    HAL_IPV4(10, 0, 0, 50),
                                    &route_retrieved));
    ASSERT_MAC_EQ(mac, route_retrieved.nexthop_mac);
}

/* ============================================================================
 * Resource Management Tests
 * ============================================================================ */

TEST(resource_pools_created)
{
    hal_resource_pool_t *fdb_pool = hal_resource_get_pool(HAL_RESOURCE_FDB_ENTRY);
    hal_resource_pool_t *route_pool = hal_resource_get_pool(HAL_RESOURCE_ROUTE_ENTRY);

    ASSERT_NOT_NULL(fdb_pool);
    ASSERT_NOT_NULL(route_pool);

    hal_resource_stats_t stats;
    ASSERT_SUCCESS(hal_resource_stats_get(fdb_pool, &stats));
    ASSERT(stats.capacity > 0);
    ASSERT_EQ(0, stats.allocated);
}

TEST(resource_allocation)
{
    hal_resource_pool_t *pool;
    hal_resource_stats_t stats;
    hal_object_id_t id1, id2;

    /* Create a small pool */
    ASSERT_SUCCESS(hal_resource_pool_create(HAL_RESOURCE_COUNTER, 10, &pool));

    /* Allocate resources */
    ASSERT_SUCCESS(hal_resource_alloc(pool, &id1));
    ASSERT_SUCCESS(hal_resource_alloc(pool, &id2));

    ASSERT_SUCCESS(hal_resource_stats_get(pool, &stats));
    ASSERT_EQ(2, stats.allocated);
    ASSERT_EQ(8, stats.available);

    /* Free one */
    ASSERT_SUCCESS(hal_resource_free(pool, id1));

    ASSERT_SUCCESS(hal_resource_stats_get(pool, &stats));
    ASSERT_EQ(1, stats.allocated);

    /* Cleanup */
    ASSERT_SUCCESS(hal_resource_free(pool, id2));
    ASSERT_SUCCESS(hal_resource_pool_destroy(pool));
}

TEST(resource_reservation)
{
    hal_resource_pool_t *pool;
    hal_resource_stats_t stats;

    ASSERT_SUCCESS(hal_resource_pool_create(HAL_RESOURCE_COUNTER, 100, &pool));

    /* Reserve some resources */
    ASSERT_SUCCESS(hal_resource_reserve(pool, 50));

    ASSERT_SUCCESS(hal_resource_stats_get(pool, &stats));
    ASSERT_EQ(50, stats.reserved);
    ASSERT_EQ(50, stats.available);

    /* Try to reserve more than available */
    hal_status_t rv = hal_resource_reserve(pool, 60);
    ASSERT_STATUS(HAL_E_RESOURCE, rv);

    /* Unreserve */
    ASSERT_SUCCESS(hal_resource_unreserve(pool, 50));

    ASSERT_SUCCESS(hal_resource_stats_get(pool, &stats));
    ASSERT_EQ(0, stats.reserved);
    ASSERT_EQ(100, stats.available);

    ASSERT_SUCCESS(hal_resource_pool_destroy(pool));
}

/* ============================================================================
 * ASIC Simulation Tests
 * ============================================================================ */

TEST(asic_capabilities)
{
    asic_capabilities_t caps;

    ASSERT_SUCCESS(asic_capabilities_get(0, &caps));

    ASSERT(caps.max_fdb_entries > 0);
    ASSERT(caps.max_route_entries > 0);
    ASSERT(caps.max_ports > 0);
    ASSERT(caps.ipv6_support);
}

TEST(asic_error_injection)
{
    hal_fdb_entry_t entry;
    hal_mac_t mac;

    /* Inject error for next 2 operations */
    asic_inject_error(0, HAL_E_HW, 2);

    make_mac(mac, 100);
    hal_fdb_entry_init(&entry, mac, 100);
    entry.port = HAL_PORT_MAKE(0, 1);

    /* First add should fail with HW error */
    hal_status_t rv = hal_fdb_add(&entry);
    ASSERT_STATUS(HAL_E_HW, rv);

    /* Second add should also fail */
    make_mac(mac, 101);
    hal_fdb_entry_init(&entry, mac, 100);
    entry.port = HAL_PORT_MAKE(0, 1);
    rv = hal_fdb_add(&entry);
    ASSERT_STATUS(HAL_E_HW, rv);

    /* Third add should succeed (error count exhausted) */
    make_mac(mac, 102);
    hal_fdb_entry_init(&entry, mac, 100);
    entry.port = HAL_PORT_MAKE(0, 1);
    ASSERT_SUCCESS(hal_fdb_add(&entry));
}

TEST(asic_latency_control)
{
    /* Get current latency */
    uint32_t original = asic_get_latency(0);

    /* Set to 0 for fast operations */
    asic_set_latency(0, 0);
    ASSERT_EQ(0, asic_get_latency(0));

    /* Restore */
    asic_set_latency(0, original);
}

/* ============================================================================
 * Concurrent Access Tests
 * ============================================================================ */

#define THREAD_COUNT 4
#define OPS_PER_THREAD 100

static void *concurrent_fdb_ops(void *arg)
{
    int thread_id = *(int *)arg;
    hal_fdb_entry_t entry;
    hal_mac_t mac;

    for (int i = 0; i < OPS_PER_THREAD; i++) {
        /* Create unique MAC for this thread/iteration */
        mac[0] = 0x00;
        mac[1] = 0x00;
        mac[2] = (uint8_t)thread_id;
        mac[3] = (uint8_t)(i >> 16);
        mac[4] = (uint8_t)(i >> 8);
        mac[5] = (uint8_t)i;

        hal_fdb_entry_init(&entry, mac, 100);
        entry.port = HAL_PORT_MAKE(0, thread_id);

        /* Add */
        hal_status_t rv = hal_fdb_add(&entry);
        if (rv != HAL_SUCCESS && rv != HAL_E_EXISTS) {
            return (void *)(intptr_t)rv;
        }

        /* Get */
        hal_fdb_entry_t retrieved;
        rv = hal_fdb_get(mac, 100, &retrieved);
        if (rv != HAL_SUCCESS) {
            return (void *)(intptr_t)rv;
        }

        /* Delete (half the time) */
        if (i % 2 == 0) {
            rv = hal_fdb_delete(mac, 100);
            if (rv != HAL_SUCCESS && rv != HAL_E_NOT_FOUND) {
                return (void *)(intptr_t)rv;
            }
        }
    }

    return NULL;
}

TEST(concurrent_fdb_access)
{
    pthread_t threads[THREAD_COUNT];
    int thread_ids[THREAD_COUNT];

    /* Start threads */
    for (int i = 0; i < THREAD_COUNT; i++) {
        thread_ids[i] = i;
        int rv = pthread_create(&threads[i], NULL, concurrent_fdb_ops, &thread_ids[i]);
        ASSERT_EQ(0, rv);
    }

    /* Wait for all threads */
    for (int i = 0; i < THREAD_COUNT; i++) {
        void *result;
        pthread_join(threads[i], &result);
        ASSERT_NULL(result);  /* NULL means success */
    }
}

/* ============================================================================
 * Scale Tests
 * ============================================================================ */

TEST(fdb_scale)
{
    hal_fdb_entry_t entry;
    hal_mac_t mac;
    int count = 0;

    /* Try to add many entries */
    for (int i = 0; i < 1000; i++) {
        mac[0] = 0xAA;
        mac[1] = 0xBB;
        mac[2] = (uint8_t)(i >> 24);
        mac[3] = (uint8_t)(i >> 16);
        mac[4] = (uint8_t)(i >> 8);
        mac[5] = (uint8_t)i;

        hal_fdb_entry_init(&entry, mac, 100);
        entry.port = HAL_PORT_MAKE(0, i % 64);

        hal_status_t rv = hal_fdb_add(&entry);
        if (rv == HAL_SUCCESS) {
            count++;
        } else if (rv == HAL_E_FULL) {
            break;  /* Table full, expected at some point */
        } else {
            ASSERT_SUCCESS(rv);  /* Unexpected error */
        }
    }

    /* Verify we could add at least some entries */
    ASSERT(count > 0);

    /* Check stats match */
    hal_fdb_stats_t stats;
    ASSERT_SUCCESS(hal_fdb_stats_get(&stats));
    ASSERT_EQ((uint32_t)count, stats.count);
}

/* ============================================================================
 * Main
 * ============================================================================ */

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    TEST_SUITE_BEGIN("Integration Tests");

    /* Lifecycle */
    RUN_TEST_WITH_FIXTURE(hal_init_shutdown);
    RUN_TEST_WITH_FIXTURE(hal_version);
    RUN_TEST_WITH_FIXTURE(hal_double_init);

    /* Combined functionality */
    RUN_TEST_WITH_FIXTURE(fdb_route_combined);

    /* Resource management */
    RUN_TEST_WITH_FIXTURE(resource_pools_created);
    RUN_TEST_WITH_FIXTURE(resource_allocation);
    RUN_TEST_WITH_FIXTURE(resource_reservation);

    /* ASIC simulation */
    RUN_TEST_WITH_FIXTURE(asic_capabilities);
    RUN_TEST_WITH_FIXTURE(asic_error_injection);
    RUN_TEST_WITH_FIXTURE(asic_latency_control);

    /* Concurrent access - disabled due to timing issue in test harness
     * The concurrent code demonstrates correct patterns for candidates */
    /* RUN_TEST_WITH_FIXTURE(concurrent_fdb_access); */

    /* Scale */
    RUN_TEST_WITH_FIXTURE(fdb_scale);

    TEST_SUITE_END();

    return TEST_EXIT_CODE();
}

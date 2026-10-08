/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2024 HAL Assessment Project
 *
 * test_fdb.c - FDB Unit Tests
 */

#include "test_framework.h"
#include "hal_init.h"
#include "hal_fdb.h"
#include "hal_types.h"
#include "asic/asic_driver.h"

#include <string.h>

/* ============================================================================
 * Test Fixtures
 * ============================================================================ */

static hal_config_t test_config;

TEST_SETUP()
{
    hal_config_init(&test_config);
    test_config.asic_latency_us = 0;  /* Disable latency for faster tests */
    hal_init(&test_config);
}

TEST_TEARDOWN()
{
    hal_shutdown();
}

/* ============================================================================
 * Helper Functions
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

/* ============================================================================
 * Basic CRUD Tests
 * ============================================================================ */

TEST(fdb_entry_init)
{
    hal_fdb_entry_t entry;
    hal_mac_t mac = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};

    hal_fdb_entry_init(&entry, mac, 100);

    ASSERT_MAC_EQ(mac, entry.mac);
    ASSERT_EQ(100, entry.vlan_id);
    ASSERT_EQ(HAL_PORT_INVALID, entry.port);
    ASSERT_EQ(HAL_FLAG_NONE, entry.flags);
    ASSERT_EQ(HAL_OBJECT_ID_INVALID, entry.object_id);
}

TEST(fdb_add_basic)
{
    hal_fdb_entry_t entry;
    hal_mac_t mac;

    make_mac(mac, 1);
    hal_fdb_entry_init(&entry, mac, 100);
    entry.port = HAL_PORT_MAKE(0, 1);

    hal_status_t rv = hal_fdb_add(&entry);
    ASSERT_SUCCESS(rv);
    ASSERT_NE(HAL_OBJECT_ID_INVALID, entry.object_id);
}

TEST(fdb_add_duplicate)
{
    hal_fdb_entry_t entry;
    hal_mac_t mac;

    make_mac(mac, 2);
    hal_fdb_entry_init(&entry, mac, 100);
    entry.port = HAL_PORT_MAKE(0, 1);

    ASSERT_SUCCESS(hal_fdb_add(&entry));

    /* Try to add duplicate */
    hal_status_t rv = hal_fdb_add(&entry);
    ASSERT_STATUS(HAL_E_EXISTS, rv);
}

TEST(fdb_add_invalid_params)
{
    hal_fdb_entry_t entry;
    hal_mac_t mac;

    /* NULL entry */
    ASSERT_STATUS(HAL_E_NULL, hal_fdb_add(NULL));

    /* Zero MAC */
    HAL_MAC_CLEAR(mac);
    hal_fdb_entry_init(&entry, mac, 100);
    entry.port = HAL_PORT_MAKE(0, 1);
    ASSERT_STATUS(HAL_E_PARAM, hal_fdb_add(&entry));

    /* Invalid VLAN */
    make_mac(mac, 3);
    hal_fdb_entry_init(&entry, mac, 0);  /* VLAN 0 is invalid */
    entry.port = HAL_PORT_MAKE(0, 1);
    ASSERT_STATUS(HAL_E_RANGE, hal_fdb_add(&entry));

    hal_fdb_entry_init(&entry, mac, 4095);  /* VLAN 4095 is reserved */
    entry.port = HAL_PORT_MAKE(0, 1);
    ASSERT_STATUS(HAL_E_RANGE, hal_fdb_add(&entry));
}

TEST(fdb_get_basic)
{
    hal_fdb_entry_t entry, retrieved;
    hal_mac_t mac;

    make_mac(mac, 10);
    hal_fdb_entry_init(&entry, mac, 200);
    entry.port = HAL_PORT_MAKE(0, 5);
    entry.flags = HAL_FLAG_STATIC;

    ASSERT_SUCCESS(hal_fdb_add(&entry));

    hal_status_t rv = hal_fdb_get(mac, 200, &retrieved);
    ASSERT_SUCCESS(rv);
    ASSERT_MAC_EQ(mac, retrieved.mac);
    ASSERT_EQ(200, retrieved.vlan_id);
    ASSERT_EQ(HAL_PORT_MAKE(0, 5), retrieved.port);
    ASSERT(retrieved.flags & HAL_FLAG_STATIC);
}

TEST(fdb_get_not_found)
{
    hal_fdb_entry_t entry;
    hal_mac_t mac;

    make_mac(mac, 99);
    hal_status_t rv = hal_fdb_get(mac, 100, &entry);
    ASSERT_STATUS(HAL_E_NOT_FOUND, rv);
}

TEST(fdb_delete_basic)
{
    hal_fdb_entry_t entry;
    hal_mac_t mac;

    make_mac(mac, 20);
    hal_fdb_entry_init(&entry, mac, 100);
    entry.port = HAL_PORT_MAKE(0, 1);

    ASSERT_SUCCESS(hal_fdb_add(&entry));
    ASSERT_SUCCESS(hal_fdb_delete(mac, 100));

    /* Verify deleted */
    ASSERT_STATUS(HAL_E_NOT_FOUND, hal_fdb_get(mac, 100, &entry));
}

TEST(fdb_delete_not_found)
{
    hal_mac_t mac;
    make_mac(mac, 99);

    hal_status_t rv = hal_fdb_delete(mac, 100);
    ASSERT_STATUS(HAL_E_NOT_FOUND, rv);
}

TEST(fdb_update_basic)
{
    hal_fdb_entry_t entry, updated;
    hal_mac_t mac;

    make_mac(mac, 30);
    hal_fdb_entry_init(&entry, mac, 100);
    entry.port = HAL_PORT_MAKE(0, 1);

    ASSERT_SUCCESS(hal_fdb_add(&entry));

    /* Update port */
    entry.port = HAL_PORT_MAKE(0, 10);
    entry.flags = HAL_FLAG_STATIC;
    ASSERT_SUCCESS(hal_fdb_update(&entry));

    /* Verify update */
    ASSERT_SUCCESS(hal_fdb_get(mac, 100, &updated));
    ASSERT_EQ(HAL_PORT_MAKE(0, 10), updated.port);
    ASSERT(updated.flags & HAL_FLAG_STATIC);
}

/* ============================================================================
 * Bulk Operation Tests
 * ============================================================================ */

TEST(fdb_delete_by_port)
{
    hal_fdb_entry_t entry;
    hal_mac_t mac;

    /* Add entries on different ports */
    for (int i = 0; i < 5; i++) {
        make_mac(mac, 40 + i);
        hal_fdb_entry_init(&entry, mac, 100);
        entry.port = HAL_PORT_MAKE(0, 1);  /* Port 1 */
        ASSERT_SUCCESS(hal_fdb_add(&entry));
    }

    for (int i = 0; i < 3; i++) {
        make_mac(mac, 50 + i);
        hal_fdb_entry_init(&entry, mac, 100);
        entry.port = HAL_PORT_MAKE(0, 2);  /* Port 2 */
        ASSERT_SUCCESS(hal_fdb_add(&entry));
    }

    /* Delete by port 1 */
    ASSERT_SUCCESS(hal_fdb_delete_by_port(HAL_PORT_MAKE(0, 1), 0));

    /* Verify port 1 entries deleted */
    for (int i = 0; i < 5; i++) {
        make_mac(mac, 40 + i);
        ASSERT_STATUS(HAL_E_NOT_FOUND, hal_fdb_get(mac, 100, &entry));
    }

    /* Verify port 2 entries still exist */
    for (int i = 0; i < 3; i++) {
        make_mac(mac, 50 + i);
        ASSERT_SUCCESS(hal_fdb_get(mac, 100, &entry));
    }
}

TEST(fdb_delete_by_vlan)
{
    hal_fdb_entry_t entry;
    hal_mac_t mac;

    /* Add entries in different VLANs */
    for (int i = 0; i < 5; i++) {
        make_mac(mac, 60 + i);
        hal_fdb_entry_init(&entry, mac, 100);
        entry.port = HAL_PORT_MAKE(0, 1);
        ASSERT_SUCCESS(hal_fdb_add(&entry));
    }

    for (int i = 0; i < 3; i++) {
        make_mac(mac, 70 + i);
        hal_fdb_entry_init(&entry, mac, 200);
        entry.port = HAL_PORT_MAKE(0, 1);
        ASSERT_SUCCESS(hal_fdb_add(&entry));
    }

    /* Delete VLAN 100 */
    ASSERT_SUCCESS(hal_fdb_delete_by_vlan(100, 0));

    /* Verify VLAN 100 entries deleted */
    for (int i = 0; i < 5; i++) {
        make_mac(mac, 60 + i);
        ASSERT_STATUS(HAL_E_NOT_FOUND, hal_fdb_get(mac, 100, &entry));
    }

    /* Verify VLAN 200 entries still exist */
    for (int i = 0; i < 3; i++) {
        make_mac(mac, 70 + i);
        ASSERT_SUCCESS(hal_fdb_get(mac, 200, &entry));
    }
}

TEST(fdb_static_not_deleted)
{
    hal_fdb_entry_t entry;
    hal_mac_t mac;

    /* Add static entry */
    make_mac(mac, 80);
    hal_fdb_entry_init(&entry, mac, 100);
    entry.port = HAL_PORT_MAKE(0, 1);
    entry.flags = HAL_FLAG_STATIC;
    ASSERT_SUCCESS(hal_fdb_add(&entry));

    /* Add dynamic entry */
    make_mac(mac, 81);
    hal_fdb_entry_init(&entry, mac, 100);
    entry.port = HAL_PORT_MAKE(0, 1);
    ASSERT_SUCCESS(hal_fdb_add(&entry));

    /* Delete by port (should only delete dynamic) */
    ASSERT_SUCCESS(hal_fdb_delete_by_port(HAL_PORT_MAKE(0, 1), 0));

    /* Static should still exist */
    make_mac(mac, 80);
    ASSERT_SUCCESS(hal_fdb_get(mac, 100, &entry));

    /* Dynamic should be gone */
    make_mac(mac, 81);
    ASSERT_STATUS(HAL_E_NOT_FOUND, hal_fdb_get(mac, 100, &entry));
}

/* ============================================================================
 * Traversal Tests
 * ============================================================================ */

static int traverse_count = 0;
static int traverse_callback(const hal_fdb_entry_t *entry, void *user_data)
{
    (void)entry;
    (void)user_data;
    traverse_count++;
    return 0;
}

TEST(fdb_traverse)
{
    hal_fdb_entry_t entry;
    hal_mac_t mac;

    /* Add some entries */
    for (int i = 0; i < 10; i++) {
        make_mac(mac, 90 + i);
        hal_fdb_entry_init(&entry, mac, 100);
        entry.port = HAL_PORT_MAKE(0, 1);
        ASSERT_SUCCESS(hal_fdb_add(&entry));
    }

    traverse_count = 0;
    ASSERT_SUCCESS(hal_fdb_traverse(traverse_callback, NULL));
    ASSERT_EQ(10, traverse_count);
}

/* ============================================================================
 * Statistics Tests
 * ============================================================================ */

TEST(fdb_stats)
{
    hal_fdb_entry_t entry;
    hal_fdb_stats_t stats;
    hal_mac_t mac;

    ASSERT_SUCCESS(hal_fdb_stats_get(&stats));
    ASSERT_EQ(0, stats.count);

    /* Add some entries */
    for (int i = 0; i < 5; i++) {
        make_mac(mac, 100 + i);
        hal_fdb_entry_init(&entry, mac, 100);
        entry.port = HAL_PORT_MAKE(0, 1);
        if (i < 2) entry.flags = HAL_FLAG_STATIC;
        ASSERT_SUCCESS(hal_fdb_add(&entry));
    }

    ASSERT_SUCCESS(hal_fdb_stats_get(&stats));
    ASSERT_EQ(5, stats.count);
    ASSERT_EQ(2, stats.static_count);
    ASSERT_EQ(3, stats.dynamic_count);
    ASSERT(stats.capacity > 0);
}

/* ============================================================================
 * Aging API Tests (Stubs - Candidates Will Implement)
 * ============================================================================ */

TEST(fdb_aging_not_implemented)
{
    /* These tests verify the stubs return HAL_E_NOT_IMPL
     * After candidate implements aging, these should pass */

    hal_status_t rv;
    uint32_t value;

    rv = hal_fdb_aging_set(300);
    /* Will be HAL_E_NOT_IMPL until implemented */
    ASSERT(rv == HAL_E_NOT_IMPL || rv == HAL_SUCCESS);

    rv = hal_fdb_aging_get(&value);
    ASSERT(rv == HAL_E_NOT_IMPL || rv == HAL_SUCCESS);

    rv = hal_fdb_aging_interval_set(10);
    ASSERT(rv == HAL_E_NOT_IMPL || rv == HAL_SUCCESS);

    rv = hal_fdb_aging_start();
    ASSERT(rv == HAL_E_NOT_IMPL || rv == HAL_SUCCESS);

    /* Stop should always succeed (even if not running) */
    rv = hal_fdb_aging_stop();
    ASSERT_SUCCESS(rv);
}

/* ============================================================================
 * Main
 * ============================================================================ */

static void run_basic_tests(void)
{
    TEST_SUITE_BEGIN("FDB Basic Tests");

    RUN_TEST_WITH_FIXTURE(fdb_entry_init);
    RUN_TEST_WITH_FIXTURE(fdb_add_basic);
    RUN_TEST_WITH_FIXTURE(fdb_add_duplicate);
    RUN_TEST_WITH_FIXTURE(fdb_add_invalid_params);
    RUN_TEST_WITH_FIXTURE(fdb_get_basic);
    RUN_TEST_WITH_FIXTURE(fdb_get_not_found);
    RUN_TEST_WITH_FIXTURE(fdb_delete_basic);
    RUN_TEST_WITH_FIXTURE(fdb_delete_not_found);
    RUN_TEST_WITH_FIXTURE(fdb_update_basic);
    RUN_TEST_WITH_FIXTURE(fdb_stats);
    RUN_TEST_WITH_FIXTURE(fdb_aging_not_implemented);

    TEST_SUITE_END();
}

static void run_bulk_tests(void)
{
    TEST_SUITE_BEGIN("FDB Bulk Tests");

    RUN_TEST_WITH_FIXTURE(fdb_delete_by_port);
    RUN_TEST_WITH_FIXTURE(fdb_delete_by_vlan);
    RUN_TEST_WITH_FIXTURE(fdb_static_not_deleted);
    RUN_TEST_WITH_FIXTURE(fdb_traverse);

    TEST_SUITE_END();
}

int main(int argc, char *argv[])
{
    bool run_all = true;
    bool run_basic = false;
    bool run_bulk = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--basic") == 0) {
            run_basic = true;
            run_all = false;
        } else if (strcmp(argv[i], "--bulk") == 0) {
            run_bulk = true;
            run_all = false;
        }
    }

    if (run_all || run_basic) {
        run_basic_tests();
    }
    if (run_all || run_bulk) {
        run_bulk_tests();
    }

    return TEST_EXIT_CODE();
}

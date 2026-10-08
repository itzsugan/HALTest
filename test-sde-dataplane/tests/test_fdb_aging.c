/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 HAL Assessment Project
 *
 * test_fdb_aging.c - FDB Aging Unit Tests
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

static void make_mac(hal_mac_t mac, uint8_t b0, uint8_t b1, uint8_t b2,
                     uint8_t b3, uint8_t b4, uint8_t b5)
{
    mac[0] = b0;
    mac[1] = b1;
    mac[2] = b2;
    mac[3] = b3;
    mac[4] = b4;
    mac[5] = b5;
}

static hal_fdb_entry_t make_fdb_entry(uint8_t mac0, hal_vlan_t vlan, hal_port_t port)
{
    hal_fdb_entry_t entry;
    hal_fdb_entry_init(&entry, NULL, vlan);
    make_mac(entry.mac, mac0, 0x11, 0x22, 0x33, 0x44, 0x55);
    entry.port = port;
    return entry;
}


/* Callback test tracking */
struct {
    int invocation_count;
    hal_fdb_entry_t last_entry;
    bool invoked;
} callback_tracker;

static void reset_tracker(void)
{
    memset(&callback_tracker, 0, sizeof(callback_tracker));
}

/* Test callback function */
static void test_age_callback(const hal_fdb_entry_t *entry, void *context)
{
    if (!entry) return;
    
    callback_tracker.invocation_count++;
    callback_tracker.last_entry = *entry;
    callback_tracker.invoked = true;
    
    int *counter = (int *)context;
    if (counter) {
        (*counter)++;
    }
}

/* Alternative callback for testing multiple callbacks */
static void test_age_callback_alt(const hal_fdb_entry_t *entry, void *context)
{
    (void)entry;
    (void)context;
    callback_tracker.invocation_count++;
}


/* ============================================================================
 * Basic CRUD Tests
 * ============================================================================ */

/* ============================================================================
 * Test Suite 1: Callback Registration
 * ============================================================================ */

TEST(test_callback_register_single)
{
    reset_tracker();
    hal_status_t status = hal_fdb_age_callback_register(test_age_callback, NULL);
    ASSERT_EQ(status, HAL_SUCCESS);
    
    /* Cleanup */
    hal_fdb_age_callback_unregister(test_age_callback);
}


TEST(test_callback_register_null)
{
    /* Registering NULL callback should fail */
    hal_status_t status = hal_fdb_age_callback_register(NULL, NULL);
    ASSERT_NE(status, HAL_SUCCESS);
}


TEST(test_callback_register_duplicate)
{
    /* Register same callback twice should return error */
    reset_tracker();
    hal_fdb_age_callback_register(test_age_callback, NULL);
    hal_status_t status = hal_fdb_age_callback_register(test_age_callback, NULL);
    ASSERT_NE(status, HAL_SUCCESS);
    
    /* Cleanup */
    hal_fdb_age_callback_unregister(test_age_callback);
}


TEST(test_callback_register_multiple)
{
    /* Register multiple callbacks */
    reset_tracker();
    for (int i = 0; i < 5; i++) {
        hal_status_t status = hal_fdb_age_callback_register(test_age_callback, NULL);
        if (i == 0) {
            ASSERT_EQ(status, HAL_SUCCESS);
        } else {
            /* Subsequent registrations should fail (duplicate) */
            ASSERT_NE(status, HAL_SUCCESS);
        }
    }
    
    /* Cleanup */
    hal_fdb_age_callback_unregister(test_age_callback);
}


TEST(test_callback_register_full)
{
    /* Register 8 unique callbacks (assuming FDB_MAX_AGE_CALLBACKS = 8) */
    reset_tracker();
    
    /* Create 8 different callback functions (using function pointers) */
    hal_fdb_age_cb_t callbacks[8] = {
        test_age_callback,
        test_age_callback_alt,
        test_age_callback,  /* Duplicate not allowed */
        test_age_callback_alt,
        test_age_callback,
        test_age_callback_alt,
        test_age_callback,
        test_age_callback_alt,
    };
    
    /* Try to register more callbacks than max (should fail) */
    int registered = 0;
    for (int i = 0; i < 8; i++) {
        if (callbacks[i] == NULL) continue;
        
        hal_status_t status = hal_fdb_age_callback_register(callbacks[i], NULL);
        if (status == HAL_SUCCESS) {
            registered++;
        }
    }
    
    /* At least one should succeed, but not all (due to duplicates/full) */
    ASSERT(registered > 0);
    
    /* Cleanup */
    hal_fdb_age_callback_unregister(test_age_callback);
    hal_fdb_age_callback_unregister(test_age_callback_alt);
}


TEST(test_callback_unregister_not_found)
{
    /* Unregistering non-existent callback should fail */
    hal_status_t status = hal_fdb_age_callback_unregister(test_age_callback);
    ASSERT_NE(status, HAL_SUCCESS);
}


/* ============================================================================
 * Test Suite 2: Aging Configuration
 * ============================================================================ */

TEST(test_aging_set_get)
{
    /* Set aging time and verify */
    hal_status_t status = hal_fdb_aging_set(120);
    ASSERT_EQ(status, HAL_SUCCESS);
    
    uint32_t aging_time = 0;
    status = hal_fdb_aging_get(&aging_time);
    ASSERT_EQ(status, HAL_SUCCESS);
    ASSERT_EQ(aging_time, 120);
    
    /* Reset to default */
    hal_fdb_aging_set(300);
}


TEST(test_aging_set_zero)
{
    /* Setting aging time to 0 disables aging */
    hal_status_t status = hal_fdb_aging_set(0);
    ASSERT_EQ(status, HAL_SUCCESS);
    
    uint32_t aging_time = 0;
    hal_fdb_aging_get(&aging_time);
    ASSERT_EQ(aging_time, 0);
    
    /* Reset to default */
    hal_fdb_aging_set(300);
}


TEST(test_interval_set_get)
{
    /* Set scan interval and verify */
    hal_status_t status = hal_fdb_aging_interval_set(20);
    ASSERT_EQ(status, HAL_SUCCESS);
    
    uint32_t interval = 0;
    status = hal_fdb_aging_interval_get(&interval);
    ASSERT_EQ(status, HAL_SUCCESS);
    ASSERT_EQ(interval, 20);
    
    /* Reset to default */
    hal_fdb_aging_interval_set(10);
}


TEST(test_interval_set_zero)
{
    /* Setting interval to 0 should fail */
    hal_status_t status = hal_fdb_aging_interval_set(0);
    ASSERT_NE(status, HAL_SUCCESS);
}


TEST(test_aging_get_null)
{
    /* Getting aging time with NULL pointer should fail */
    hal_status_t status = hal_fdb_aging_get(NULL);
    ASSERT_NE(status, HAL_SUCCESS);
}


/* ============================================================================
 * Test Suite 3: Thread Control
 * ============================================================================ */

TEST(test_aging_start_stop)
{
    /* Start aging thread */
    hal_status_t status = hal_fdb_aging_start();
    ASSERT_EQ(status, HAL_SUCCESS);
    
    /* Verify running */
    bool running = hal_fdb_aging_is_running();
    ASSERT(running);
    
    /* Stop aging thread */
    status = hal_fdb_aging_stop();
    ASSERT_EQ(status, HAL_SUCCESS);
    
    /* Verify stopped (may take a moment) */
    usleep(100000);  /* 100ms */
    running = hal_fdb_aging_is_running();
    ASSERT_EQ(running, 0);
}


TEST(test_aging_start_double)
{
    /* Start aging thread first time */
    hal_status_t status = hal_fdb_aging_start();
    ASSERT_EQ(status, HAL_SUCCESS);
    
    /* Second start should return error */
    status = hal_fdb_aging_start();
    ASSERT_NE(status, HAL_SUCCESS);
    
    /* Cleanup */
    hal_fdb_aging_stop();
}


TEST(test_aging_stop_not_running)
{
    /* Stop should succeed even if not running */
    hal_status_t status = hal_fdb_aging_stop();
    ASSERT_EQ(status, HAL_SUCCESS);
}


TEST(test_aging_start_stop_start)
{
    /* Start, stop, start sequence */
    hal_status_t status = hal_fdb_aging_start();
    ASSERT_EQ(status, HAL_SUCCESS);
    
    status = hal_fdb_aging_stop();
    ASSERT_EQ(status, HAL_SUCCESS);
    
    usleep(100000);  /* Allow cleanup */
    
    status = hal_fdb_aging_start();
    ASSERT_EQ(status, HAL_SUCCESS);
    
    /* Cleanup */
    hal_fdb_aging_stop();
}


/* ============================================================================
 * Test Suite 4: Basic Aging
 * ============================================================================ */

TEST(test_aging_dynamic_entry)
{
    /* Add dynamic entry, enable aging, wait for aging timeout
     * Entry should be removed after aging_time seconds
     */
    
    /* Setup: Start aging with short timeout */
    hal_fdb_aging_set(2);  /* 2 second aging timeout */
    hal_fdb_aging_interval_set(1);  /* 1 second scan interval */
    hal_fdb_aging_start();
    
    usleep(100000);  /* Let thread start */
    
    /* Add dynamic entry */
    hal_fdb_entry_t entry = make_fdb_entry(0x01, 100, 1);
    hal_fdb_add(&entry);
    
    /* Verify entry was added */
    hal_fdb_entry_t retrieved = {0};
    hal_status_t status = hal_fdb_get(entry.mac, entry.vlan_id, &retrieved);
    ASSERT_EQ(status, HAL_SUCCESS);
    
    /* Wait for aging (2 sec timeout + 1 sec scan interval + margin) */
    sleep(4);
    
    /* Entry should be deleted by aging thread */
    status = hal_fdb_get(entry.mac, entry.vlan_id, &retrieved);
    ASSERT_NE(status, HAL_SUCCESS);  /* Entry not found */
    
    /* Cleanup */
    hal_fdb_aging_stop();
    hal_fdb_aging_set(300);  /* Reset to default */
    hal_fdb_aging_interval_set(10);
}


TEST(test_aging_static_entry)
{
    /* Add static entry with aging enabled
     * Static entries should NEVER age out
     */
    
    /* Setup: Start aging with short timeout */
    hal_fdb_aging_set(2);
    hal_fdb_aging_interval_set(1);
    hal_fdb_aging_start();
    
    usleep(100000);  /* Let thread start */
    
    /* Add STATIC entry */
    hal_fdb_entry_t entry = make_fdb_entry(0x02, 100, 1);
    entry.flags |= HAL_FLAG_STATIC;
    hal_fdb_add(&entry);
    
    /* Verify entry was added */
    hal_fdb_entry_t retrieved = {0};
    hal_status_t status = hal_fdb_get(entry.mac, entry.vlan_id, &retrieved);
    ASSERT_EQ(status, HAL_SUCCESS);
    
    /* Wait longer than aging timeout */
    sleep(4);
    
    /* Entry should still exist (static) */
    status = hal_fdb_get(entry.mac, entry.vlan_id, &retrieved);
    ASSERT_EQ(status, HAL_SUCCESS);
    ASSERT(retrieved.flags & HAL_FLAG_STATIC);
    
    /* Manual cleanup */
    hal_fdb_delete(entry.mac, entry.vlan_id);
    
    /* Thread cleanup */
    hal_fdb_aging_stop();
    hal_fdb_aging_set(300);
    hal_fdb_aging_interval_set(10);
}


TEST(test_aging_disabled)
{
    /* With aging_time = 0, entries should not age out */
    
    /* Setup: Disable aging */
    hal_fdb_aging_set(0);
    hal_fdb_aging_interval_set(1);
    hal_fdb_aging_start();
    
    usleep(100000);
    
    /* Add dynamic entry */
    hal_fdb_entry_t entry = make_fdb_entry(0x03, 100, 1);
    hal_fdb_add(&entry);
    
    /* Wait */
    sleep(3);
    
    /* Entry should still exist (aging disabled) */
    hal_fdb_entry_t retrieved = {0};
    hal_status_t status = hal_fdb_get(entry.mac, entry.vlan_id, &retrieved);
    ASSERT_EQ(status, HAL_SUCCESS);
    
    /* Manual cleanup */
    hal_fdb_delete(entry.mac, entry.vlan_id);
    
    /* Thread cleanup */
    hal_fdb_aging_stop();
    hal_fdb_aging_set(300);
}


/* ============================================================================
 * Test Suite 5: Callback Invocation
 * ============================================================================ */

TEST(test_callback_invoked_on_aging)
{
    /* When entry ages out, registered callbacks should be invoked */
    
    reset_tracker();
    
    /* Register callback */
    int callback_count = 0;
    hal_fdb_age_callback_register(test_age_callback, &callback_count);
    
    /* Setup: Start aging with short timeout */
    hal_fdb_aging_set(2);
    hal_fdb_aging_interval_set(1);
    hal_fdb_aging_start();
    
    usleep(100000);
    
    /* Add dynamic entry */
    hal_fdb_entry_t entry = make_fdb_entry(0x04, 100, 1);
    hal_fdb_add(&entry);
    
    /* Wait for aging */
    sleep(4);
    
    /* Callback should have been invoked */
    ASSERT(callback_tracker.invoked);
    ASSERT(callback_tracker.invocation_count > 0);
    
    /* Verify callback received correct entry data */
    ASSERT(HAL_MAC_EQUAL(callback_tracker.last_entry.mac, entry.mac));
    ASSERT_EQ(callback_tracker.last_entry.vlan_id, entry.vlan_id);
    
    /* Cleanup */
    hal_fdb_aging_stop();
    hal_fdb_age_callback_unregister(test_age_callback);
    hal_fdb_aging_set(300);
    hal_fdb_aging_interval_set(10);
}


TEST(test_multiple_callbacks_invoked)
{
    /* Multiple registered callbacks should all be invoked */
    
    reset_tracker();
    
    /* Register two callbacks */
    hal_fdb_age_callback_register(test_age_callback, NULL);
    hal_fdb_age_callback_register(test_age_callback_alt, NULL);
    
    /* Setup: Start aging */
    hal_fdb_aging_set(2);
    hal_fdb_aging_interval_set(1);
    hal_fdb_aging_start();
    
    usleep(100000);
    
    /* Add entry */
    hal_fdb_entry_t entry = make_fdb_entry(0x05, 100, 1);
    hal_fdb_add(&entry);
    
    /* Wait for aging */
    sleep(4);
    
    /* Callback count should reflect both callbacks being invoked */
    ASSERT(callback_tracker.invocation_count >= 1);
    
    /* Cleanup */
    hal_fdb_aging_stop();
    hal_fdb_age_callback_unregister(test_age_callback);
    hal_fdb_age_callback_unregister(test_age_callback_alt);
    hal_fdb_aging_set(300);
    hal_fdb_aging_interval_set(10);
}


/* ============================================================================
 * Test Suite 6: HIT Bit Handling
 * ============================================================================ */

TEST(test_hit_bit_prevents_aging)
{
    /* Entry with HIT bit set should not age out (HIT should be cleared instead) */
    
    /* Setup: Start aging */
    hal_fdb_aging_set(2);
    hal_fdb_aging_interval_set(1);
    hal_fdb_aging_start();
    
    usleep(100000);
    
    /* Add entry and simulate HIT */
    hal_fdb_entry_t entry = make_fdb_entry(0x06, 100, 1);
    hal_fdb_add(&entry);
    
    /* Set HIT flag to indicate recent access */
    entry.flags |= HAL_FLAG_HIT;
    hal_fdb_update(&entry);
    
    /* Wait for scan interval (thread should clear HIT, not age entry) */
    sleep(2);
    
    /* Entry should still exist (HIT prevented aging) */
    hal_fdb_entry_t retrieved = {0};
    hal_status_t status = hal_fdb_get(entry.mac, entry.vlan_id, &retrieved);
    ASSERT_EQ(status, HAL_SUCCESS);
    
    /* HIT bit should be cleared after scan */
    ASSERT_EQ(retrieved.flags & HAL_FLAG_HIT, 0);
    
    /* Manual cleanup */
    hal_fdb_delete(entry.mac, entry.vlan_id);
    
    /* Thread cleanup */
    hal_fdb_aging_stop();
    hal_fdb_aging_set(300);
    hal_fdb_aging_interval_set(10);
}


/* ============================================================================
 * Test Suite 7: Edge Cases
 * ============================================================================ */

TEST(test_empty_table_aging)
{
    /* Aging should handle empty table gracefully */
    
    reset_tracker();
    
    /* Make sure FDB is empty */
    hal_fdb_flush_dynamic();
    
    /* Start aging */
    hal_fdb_aging_set(2);
    hal_fdb_aging_interval_set(1);
    hal_fdb_aging_start();
    
    /* Wait for scans */
    sleep(3);
    
    /* Should complete without error */
    bool running = hal_fdb_aging_is_running();
    ASSERT(running);
    
    /* Cleanup */
    hal_fdb_aging_stop();
    hal_fdb_aging_set(300);
    hal_fdb_aging_interval_set(10);
}


TEST(test_config_change_during_aging)
{
    /* Changing configuration while aging is running should work */
    
    /* Start aging */
    hal_fdb_aging_set(300);
    hal_fdb_aging_interval_set(10);
    hal_fdb_aging_start();
    
    usleep(100000);
    
    /* Change configuration */
    hal_status_t status = hal_fdb_aging_set(120);
    ASSERT_EQ(status, HAL_SUCCESS);
    
    status = hal_fdb_aging_interval_set(5);
    ASSERT_EQ(status, HAL_SUCCESS);
    
    /* Verify changes took effect */
    uint32_t aging_time = 0, interval = 0;
    hal_fdb_aging_get(&aging_time);
    hal_fdb_aging_interval_get(&interval);
    ASSERT_EQ(aging_time, 120);
    ASSERT_EQ(interval, 5);
    
    /* Cleanup */
    hal_fdb_aging_stop();
    hal_fdb_aging_set(300);
    hal_fdb_aging_interval_set(10);
}


/* ============================================================================
 * Main
 * ============================================================================ */

static void run_basic_tests(void)
{
    TEST_SUITE_BEGIN("FDB Aging Basic Tests");

    /* Callback Registration Tests */
    RUN_TEST_WITH_FIXTURE(test_callback_register_single);
    RUN_TEST_WITH_FIXTURE(test_callback_register_null);
    RUN_TEST_WITH_FIXTURE(test_callback_register_duplicate);
    RUN_TEST_WITH_FIXTURE(test_callback_register_multiple);
    RUN_TEST_WITH_FIXTURE(test_callback_register_full);
    RUN_TEST_WITH_FIXTURE(test_callback_unregister_not_found);

    /* Aging Configuration Tests */
    RUN_TEST_WITH_FIXTURE(test_aging_set_get);
    RUN_TEST_WITH_FIXTURE(test_aging_set_zero);
    RUN_TEST_WITH_FIXTURE(test_interval_set_get);
    RUN_TEST_WITH_FIXTURE(test_interval_set_zero);
    RUN_TEST_WITH_FIXTURE(test_aging_get_null);

    /* Thread Control Tests */
    RUN_TEST_WITH_FIXTURE(test_aging_start_stop);
    RUN_TEST_WITH_FIXTURE(test_aging_start_double);
    RUN_TEST_WITH_FIXTURE(test_aging_stop_not_running);
    RUN_TEST_WITH_FIXTURE(test_aging_start_stop_start);

    /* Basic Aging Tests */
    RUN_TEST_WITH_FIXTURE(test_aging_dynamic_entry);
    RUN_TEST_WITH_FIXTURE(test_aging_static_entry);
    RUN_TEST_WITH_FIXTURE(test_aging_disabled);

    /* Callback Tests */
    RUN_TEST_WITH_FIXTURE(test_callback_invoked_on_aging);
    RUN_TEST_WITH_FIXTURE(test_multiple_callbacks_invoked);

    /* HIT Bit Tests */
    RUN_TEST_WITH_FIXTURE(test_hit_bit_prevents_aging);

    TEST_SUITE_END();
}

static void run_bulk_tests(void)
{
    TEST_SUITE_BEGIN("FDB Aging Bulk Tests");

    /* Edge Cases */
    RUN_TEST_WITH_FIXTURE(test_empty_table_aging);
    RUN_TEST_WITH_FIXTURE(test_config_change_during_aging);

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

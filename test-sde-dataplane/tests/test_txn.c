/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2024 HAL Assessment Project
 *
 * test_txn.c - Transaction Manager Tests
 */

#include "test_framework.h"
#include "hal_init.h"
#include "hal_txn.h"
#include "hal_fdb.h"
#include "hal_route.h"
#include "hal_types.h"
#include "hal_resource.h"

#include <string.h>
#include <pthread.h>

/* ============================================================================
 * Test Fixtures
 * ============================================================================ */

static hal_config_t test_config;

TEST_SETUP()
{
    hal_config_init(&test_config);
    test_config.asic_latency_us = 0;  /* Disable latency for faster tests */
    test_config.fdb_size = 256;
    test_config.route_size = 128;
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

static void make_fdb_entry(hal_fdb_entry_t *entry, uint8_t mac_last,
                          uint16_t vlan, uint16_t port)
{
    hal_fdb_entry_init(entry, NULL, vlan);
    make_mac(entry->mac, mac_last);
    entry->port = HAL_PORT_MAKE(0, port);
}

static void make_route_entry(hal_route_entry_t *entry, uint8_t prefix_last,
                            uint8_t prefix_len, uint16_t nexthop_last,
                            uint16_t port)
{
    hal_route_entry_init(entry);
    entry->vrf_id = 0;
    entry->prefix = (192 << 24) | (168 << 16) | (1 << 8) | prefix_last;
    entry->prefix_len = prefix_len;
    entry->nexthop = (10 << 24) | (0 << 16) | (0 << 8) | nexthop_last;
    entry->egress_port = HAL_PORT_MAKE(0, port);
}

/* ============================================================================
 * Basic Transaction Lifecycle Tests
 * ============================================================================ */

TEST(txn_begin_basic)
{
    hal_txn_t *txn = NULL;
    hal_status_t rv = hal_txn_begin(NULL, &txn);
    
    ASSERT_SUCCESS(rv);
    ASSERT_NOT_NULL(txn);
    ASSERT_EQ(HAL_TXN_STATE_PENDING, hal_txn_get_state(txn));
    
    hal_txn_free(txn);
}

TEST(txn_begin_null_txn_pointer)
{
    hal_status_t rv = hal_txn_begin(NULL, NULL);
    ASSERT_STATUS(HAL_E_NULL, rv);
}

TEST(txn_begin_with_options)
{
    hal_txn_opts_t opts;
    opts.timeout_ms = 1000;
    opts.auto_rollback = true;
    
    hal_txn_t *txn = NULL;
    hal_status_t rv = hal_txn_begin(&opts, &txn);
    
    ASSERT_SUCCESS(rv);
    ASSERT_NOT_NULL(txn);
    ASSERT_EQ(HAL_TXN_STATE_PENDING, hal_txn_get_state(txn));
    
    hal_txn_free(txn);
}

TEST(txn_free_committed)
{
    hal_txn_t *txn = NULL;
    hal_txn_begin(NULL, &txn);
    hal_txn_abort(txn);
    
    hal_status_t rv = hal_txn_free(txn);
    ASSERT_SUCCESS(rv);
}

TEST(txn_free_active)
{
    hal_txn_t *txn = NULL;
    hal_txn_begin(NULL, &txn);
    
    hal_status_t rv = hal_txn_free(txn);
    ASSERT_SUCCESS(rv);
}

/* ============================================================================
 * Single Operation Tests
 * ============================================================================ */

TEST(txn_add_fdb_basic)
{
    hal_txn_t *txn = NULL;
    hal_fdb_entry_t entry;
    
    hal_txn_begin(NULL, &txn);
    make_fdb_entry(&entry, 1, 100, 1);
    
    hal_status_t rv = hal_txn_add_fdb(txn, HAL_TXN_OP_ADD, &entry);
    ASSERT_SUCCESS(rv);
    ASSERT_EQ(HAL_TXN_STATE_ACTIVE, hal_txn_get_state(txn));
    
    /* Verify stats */
    hal_txn_stats_t stats;
    hal_txn_get_stats(txn, &stats);
    ASSERT_EQ(1, stats.total_operations);
    ASSERT_EQ(1, stats.fdb_adds);
    
    hal_txn_abort(txn);
    hal_txn_free(txn);
}

TEST(txn_add_route_basic)
{
    hal_txn_t *txn = NULL;
    hal_route_entry_t entry;
    
    hal_txn_begin(NULL, &txn);
    make_route_entry(&entry, 1, 24, 1, 2);
    
    hal_status_t rv = hal_txn_add_route(txn, HAL_TXN_OP_ADD, &entry);
    ASSERT_SUCCESS(rv);
    ASSERT_EQ(HAL_TXN_STATE_ACTIVE, hal_txn_get_state(txn));
    
    /* Verify stats */
    hal_txn_stats_t stats;
    hal_txn_get_stats(txn, &stats);
    ASSERT_EQ(1, stats.total_operations);
    ASSERT_EQ(1, stats.route_adds);
    
    hal_txn_abort(txn);
    hal_txn_free(txn);
}

TEST(txn_add_null_params)
{
    hal_txn_t *txn = NULL;
    hal_fdb_entry_t entry;
    
    hal_txn_begin(NULL, &txn);
    make_fdb_entry(&entry, 1, 100, 1);
    
    /* NULL transaction */
    ASSERT_STATUS(HAL_E_NULL, hal_txn_add_fdb(NULL, HAL_TXN_OP_ADD, &entry));
    
    /* NULL entry */
    ASSERT_STATUS(HAL_E_NULL, hal_txn_add_fdb(txn, HAL_TXN_OP_ADD, NULL));
    
    hal_txn_abort(txn);
    hal_txn_free(txn);
}

TEST(txn_add_invalid_operation)
{
    hal_txn_t *txn = NULL;
    hal_fdb_entry_t entry;
    
    hal_txn_begin(NULL, &txn);
    make_fdb_entry(&entry, 1, 100, 1);
    
    /* Invalid operation */
    hal_status_t rv = hal_txn_add_fdb(txn, (hal_txn_op_t)999, &entry);
    ASSERT_STATUS(HAL_E_PARAM, rv);
    
    hal_txn_abort(txn);
    hal_txn_free(txn);
}

/* ============================================================================
 * Successful Commit Tests
 * ============================================================================ */

TEST(txn_commit_single_fdb_add)
{
    hal_txn_t *txn = NULL;
    hal_fdb_entry_t entry;
    
    hal_txn_begin(NULL, &txn);
    make_fdb_entry(&entry, 10, 100, 1);
    ASSERT_SUCCESS(hal_txn_add_fdb(txn, HAL_TXN_OP_ADD, &entry));
    
    hal_status_t rv = hal_txn_commit(txn);
    ASSERT_SUCCESS(rv);
    ASSERT_EQ(HAL_TXN_STATE_COMMITTED, hal_txn_get_state(txn));
    
    /* Verify entry was actually added */
    hal_fdb_entry_t retrieved;
    make_fdb_entry(&retrieved, 10, 100, 1);
    rv = hal_fdb_get(retrieved.mac, retrieved.vlan_id, &retrieved);
    ASSERT_SUCCESS(rv);
    
    hal_txn_free(txn);
}

TEST(txn_commit_multiple_operations)
{
    hal_txn_t *txn = NULL;
    hal_fdb_entry_t fdb_entry;
    hal_route_entry_t route_entry;
    
    hal_txn_begin(NULL, &txn);
    
    /* Add FDB entries */
    for (int i = 1; i <= 2; i++) {
        make_fdb_entry(&fdb_entry, 10 + i, 100, i);
        ASSERT_SUCCESS(hal_txn_add_fdb(txn, HAL_TXN_OP_ADD, &fdb_entry));
    }
    
    /* Add route entry */
    make_route_entry(&route_entry, 5, 24, 1, 2);
    ASSERT_SUCCESS(hal_txn_add_route(txn, HAL_TXN_OP_ADD, &route_entry));
    
    hal_status_t rv = hal_txn_commit(txn);
    ASSERT_SUCCESS(rv);
    ASSERT_EQ(HAL_TXN_STATE_COMMITTED, hal_txn_get_state(txn));
    
    hal_txn_free(txn);
}

TEST(txn_commit_empty_transaction)
{
    hal_txn_t *txn = NULL;
    
    hal_txn_begin(NULL, &txn);
    
    /* Commit without adding any operations */
    hal_status_t rv = hal_txn_commit(txn);
    ASSERT_SUCCESS(rv);
    ASSERT_EQ(HAL_TXN_STATE_COMMITTED, hal_txn_get_state(txn));
    
    hal_txn_free(txn);
}

/* ============================================================================
 * Rollback Tests
 * ============================================================================ */

TEST(txn_abort_before_commit)
{
    hal_txn_t *txn = NULL;
    hal_fdb_entry_t entry;
    
    hal_txn_begin(NULL, &txn);
    make_fdb_entry(&entry, 21, 100, 1);
    ASSERT_SUCCESS(hal_txn_add_fdb(txn, HAL_TXN_OP_ADD, &entry));
    
    /* Abort without commit */
    hal_status_t rv = hal_txn_abort(txn);
    ASSERT_SUCCESS(rv);
    ASSERT_EQ(HAL_TXN_STATE_ABORTED, hal_txn_get_state(txn));
    
    hal_txn_free(txn);
}

TEST(txn_rollback_after_commit)
{
    hal_txn_t *txn = NULL;
    hal_fdb_entry_t entry;
    
    hal_txn_begin(NULL, &txn);
    make_fdb_entry(&entry, 22, 100, 1);
    ASSERT_SUCCESS(hal_txn_add_fdb(txn, HAL_TXN_OP_ADD, &entry));
    
    ASSERT_SUCCESS(hal_txn_commit(txn));
    
    hal_status_t rv = hal_txn_rollback(txn);
    ASSERT_SUCCESS(rv);
    
    hal_txn_free(txn);
}

TEST(txn_commit_failure_cleans_up)
{
    hal_txn_t *txn = NULL;
    hal_fdb_entry_t entry;
    hal_route_entry_t rt_entry;

    make_fdb_entry(&entry, 24, 100, 1);
    ASSERT_SUCCESS(hal_fdb_add(&entry));

    hal_txn_begin(NULL, &txn);

    make_route_entry(&rt_entry, 74, 24, 1, 2);
    ASSERT_SUCCESS(hal_txn_add_route(txn, HAL_TXN_OP_ADD, &rt_entry));
    
    make_fdb_entry(&entry, 24, 100, 1);
    ASSERT_SUCCESS(hal_txn_add_fdb(txn, HAL_TXN_OP_ADD, &entry));

    hal_status_t rv = hal_txn_commit(txn);
    ASSERT_STATUS(HAL_E_EXISTS, rv);
    ASSERT_EQ(HAL_TXN_STATE_FAILED, hal_txn_get_state(txn));

    make_fdb_entry(&entry, 24, 100, 1);
    rv = hal_fdb_get(entry.mac, entry.vlan_id, &entry);
    ASSERT_SUCCESS(rv);

    make_route_entry(&rt_entry, 74, 24, 1, 2);
    rv = hal_route_get(rt_entry.vrf_id, rt_entry.prefix, rt_entry.prefix_len, &rt_entry);
    ASSERT_STATUS(HAL_E_NOT_FOUND, rv);

    ASSERT_SUCCESS(hal_txn_abort(txn));
    ASSERT_EQ(HAL_TXN_STATE_ABORTED, hal_txn_get_state(txn));

    hal_txn_free(txn);
}

TEST(txn_commit_asic_failure_cleans_up)
{
    hal_txn_t *txn = NULL;
    hal_fdb_entry_t entry;
    hal_route_entry_t rt_entry;

    hal_txn_begin(NULL, &txn);

    make_route_entry(&rt_entry, 74, 24, 1, 2);
    ASSERT_SUCCESS(hal_txn_add_route(txn, HAL_TXN_OP_ADD, &rt_entry));
    
    make_fdb_entry(&entry, 24, 100, 1);
    ASSERT_SUCCESS(hal_txn_add_fdb(txn, HAL_TXN_OP_ADD, &entry));

	/* Inject ASIC error */
    asic_inject_error(0, HAL_E_PARAM, 1);

	/* Commit and check error */
    hal_status_t rv = hal_txn_commit(txn);
    ASSERT_STATUS(HAL_E_PARAM, rv);
    ASSERT_EQ(HAL_TXN_STATE_FAILED, hal_txn_get_state(txn));

    make_fdb_entry(&entry, 24, 100, 1);
    rv = hal_fdb_get(entry.mac, entry.vlan_id, &entry);
    ASSERT_STATUS(HAL_E_NOT_FOUND, rv);

    make_route_entry(&rt_entry, 74, 24, 1, 2);
    rv = hal_route_get(rt_entry.vrf_id, rt_entry.prefix, rt_entry.prefix_len, &rt_entry);
    ASSERT_STATUS(HAL_E_NOT_FOUND, rv);

    ASSERT_SUCCESS(hal_txn_abort(txn));
    ASSERT_EQ(HAL_TXN_STATE_ABORTED, hal_txn_get_state(txn));

    hal_txn_free(txn);
}


TEST(txn_fdb_delete_missing_entry_releases_lock)
{
    hal_txn_t *txn = NULL;
    hal_fdb_entry_t entry;

    hal_txn_begin(NULL, &txn);
    make_fdb_entry(&entry, 25, 100, 1);

    hal_status_t rv = hal_txn_add_fdb(txn, HAL_TXN_OP_DELETE, &entry);
    ASSERT_STATUS(HAL_E_NOT_FOUND, rv);
    ASSERT_EQ(HAL_TXN_STATE_PENDING, hal_txn_get_state(txn));

    ASSERT_SUCCESS(hal_txn_abort(txn));
    ASSERT_EQ(HAL_TXN_STATE_ABORTED, hal_txn_get_state(txn));

    hal_txn_free(txn);
}

TEST(txn_double_commit_fails)
{
    hal_txn_t *txn = NULL;
    hal_fdb_entry_t entry;
    
    hal_txn_begin(NULL, &txn);
    make_fdb_entry(&entry, 23, 100, 1);
    ASSERT_SUCCESS(hal_txn_add_fdb(txn, HAL_TXN_OP_ADD, &entry));
    
    /* First commit succeeds */
    ASSERT_SUCCESS(hal_txn_commit(txn));
    
    /* Second commit should fail (not in PENDING/ACTIVE state) */
    hal_status_t rv = hal_txn_commit(txn);
    ASSERT_STATUS(HAL_E_FAIL, rv);
    
    hal_txn_free(txn);
}

TEST(txn_rollback_fdb_add_removes_entry)
{
    hal_txn_t *txn = NULL;
    hal_fdb_entry_t entry;
    
    hal_txn_begin(NULL, &txn);
    make_fdb_entry(&entry, 71, 100, 1);
    ASSERT_SUCCESS(hal_txn_add_fdb(txn, HAL_TXN_OP_ADD, &entry));
    
    ASSERT_SUCCESS(hal_txn_commit(txn));
    
    hal_fdb_entry_t retrieved;
    make_fdb_entry(&retrieved, 71, 100, 1);
    hal_status_t rv = hal_fdb_get(retrieved.mac, retrieved.vlan_id, &retrieved);
    ASSERT_SUCCESS(rv);
    
    ASSERT_SUCCESS(hal_txn_rollback(txn));
    
    make_fdb_entry(&retrieved, 71, 100, 1);
    rv = hal_fdb_get(retrieved.mac, retrieved.vlan_id, &retrieved);
    ASSERT_STATUS(HAL_E_NOT_FOUND, rv);
    
    hal_txn_free(txn);
}

TEST(txn_rollback_fdb_delete_restores_entry)
{
    hal_txn_t *txn = NULL;
    hal_fdb_entry_t entry;
    
    make_fdb_entry(&entry, 72, 100, 1);
    ASSERT_SUCCESS(hal_fdb_add(&entry));
    
    hal_txn_begin(NULL, &txn);
    make_fdb_entry(&entry, 72, 100, 1);
    ASSERT_SUCCESS(hal_txn_add_fdb(txn, HAL_TXN_OP_DELETE, &entry));
    
    ASSERT_SUCCESS(hal_txn_commit(txn));
    
    make_fdb_entry(&entry, 72, 100, 1);
    hal_status_t rv = hal_fdb_get(entry.mac, entry.vlan_id, &entry);
    ASSERT_STATUS(HAL_E_NOT_FOUND, rv);
    
    ASSERT_SUCCESS(hal_txn_rollback(txn));
    
    make_fdb_entry(&entry, 72, 100, 1);
    rv = hal_fdb_get(entry.mac, entry.vlan_id, &entry);
    ASSERT_SUCCESS(rv);
    
    hal_txn_free(txn);
}

TEST(txn_rollback_fdb_update_restores_original)
{
    hal_txn_t *txn = NULL;
    hal_fdb_entry_t entry;
    
    make_fdb_entry(&entry, 73, 100, 1);
    ASSERT_SUCCESS(hal_fdb_add(&entry));
    
    hal_txn_begin(NULL, &txn);
    make_fdb_entry(&entry, 73, 100, 2);
    ASSERT_SUCCESS(hal_txn_add_fdb(txn, HAL_TXN_OP_UPDATE, &entry));
    
    ASSERT_SUCCESS(hal_txn_commit(txn));
    
    hal_fdb_entry_t retrieved;
    make_fdb_entry(&retrieved, 73, 100, 2);
    hal_status_t rv = hal_fdb_get(retrieved.mac, retrieved.vlan_id, &retrieved);
    ASSERT_SUCCESS(rv);
    ASSERT_EQ(2, retrieved.port);
    
    ASSERT_SUCCESS(hal_txn_rollback(txn));
    
    make_fdb_entry(&retrieved, 73, 100, 1);
    rv = hal_fdb_get(retrieved.mac, retrieved.vlan_id, &retrieved);
    ASSERT_SUCCESS(rv);
    ASSERT_EQ(1, retrieved.port);
    
    hal_txn_free(txn);
}

TEST(txn_rollback_route_delete_restores_entry)
{
    hal_txn_t *txn = NULL;
    hal_route_entry_t entry;
    
    make_route_entry(&entry, 74, 24, 1, 2);
    ASSERT_SUCCESS(hal_route_add(&entry));
    
    hal_txn_begin(NULL, &txn);
    make_route_entry(&entry, 74, 24, 1, 2);
    ASSERT_SUCCESS(hal_txn_add_route(txn, HAL_TXN_OP_DELETE, &entry));
    
    ASSERT_SUCCESS(hal_txn_commit(txn));
    
    make_route_entry(&entry, 74, 24, 1, 2);
    hal_status_t rv = hal_route_get(entry.vrf_id, entry.prefix, entry.prefix_len, &entry);
    ASSERT_STATUS(HAL_E_NOT_FOUND, rv);
    
    ASSERT_SUCCESS(hal_txn_rollback(txn));
    
    make_route_entry(&entry, 74, 24, 1, 2);
    rv = hal_route_get(entry.vrf_id, entry.prefix, entry.prefix_len, &entry);
    ASSERT_SUCCESS(rv);
    
    hal_txn_free(txn);
}

TEST(txn_rollback_wrong_state_active)
{
    hal_txn_t *txn = NULL;
    hal_fdb_entry_t entry;
    
    hal_txn_begin(NULL, &txn);
    make_fdb_entry(&entry, 75, 100, 1);
    ASSERT_SUCCESS(hal_txn_add_fdb(txn, HAL_TXN_OP_ADD, &entry));
    
    ASSERT_EQ(HAL_TXN_STATE_ACTIVE, hal_txn_get_state(txn));
    
    hal_status_t rv = hal_txn_rollback(txn);
    ASSERT_STATUS(HAL_E_FAIL, rv);
    
    hal_txn_abort(txn);
    hal_txn_free(txn);
}

TEST(txn_rollback_wrong_state_pending)
{
    hal_txn_t *txn = NULL;
    
    hal_txn_begin(NULL, &txn);
    
    ASSERT_EQ(HAL_TXN_STATE_PENDING, hal_txn_get_state(txn));
    
    hal_status_t rv = hal_txn_rollback(txn);
    ASSERT_STATUS(HAL_E_FAIL, rv);
    
    hal_txn_abort(txn);
    hal_txn_free(txn);
}

TEST(txn_abort_idempotent)
{
    hal_txn_t *txn = NULL;
    hal_fdb_entry_t entry;
    
    hal_txn_begin(NULL, &txn);
    make_fdb_entry(&entry, 76, 100, 1);
    ASSERT_SUCCESS(hal_txn_add_fdb(txn, HAL_TXN_OP_ADD, &entry));
    
    ASSERT_SUCCESS(hal_txn_abort(txn));
    ASSERT_EQ(HAL_TXN_STATE_ABORTED, hal_txn_get_state(txn));
    
    hal_status_t rv = hal_txn_abort(txn);
    ASSERT_SUCCESS(rv);
    ASSERT_EQ(HAL_TXN_STATE_ABORTED, hal_txn_get_state(txn));
    
    hal_txn_free(txn);
}

TEST(txn_multiple_adds_rollback_all)
{
    hal_txn_t *txn = NULL;
    hal_fdb_entry_t entry;
    
    hal_txn_begin(NULL, &txn);
    
    for (int i = 1; i <= 3; i++) {
        make_fdb_entry(&entry, 80 + i, 100, i);
        ASSERT_SUCCESS(hal_txn_add_fdb(txn, HAL_TXN_OP_ADD, &entry));
    }
    
    ASSERT_SUCCESS(hal_txn_commit(txn));
    
    for (int i = 1; i <= 3; i++) {
        make_fdb_entry(&entry, 80 + i, 100, i);
        hal_status_t rv = hal_fdb_get(entry.mac, entry.vlan_id, &entry);
        ASSERT_SUCCESS(rv);
    }
    
    ASSERT_SUCCESS(hal_txn_rollback(txn));
    
    for (int i = 1; i <= 3; i++) {
        make_fdb_entry(&entry, 80 + i, 100, i);
        hal_status_t rv = hal_fdb_get(entry.mac, entry.vlan_id, &entry);
        ASSERT_STATUS(HAL_E_NOT_FOUND, rv);
    }
    
    hal_txn_free(txn);
}

TEST(txn_mixed_operations_rollback_all)
{
    hal_txn_t *txn = NULL;
    hal_fdb_entry_t fdb_entry;
    hal_route_entry_t route_entry;
    
    make_fdb_entry(&fdb_entry, 84, 100, 1);
    ASSERT_SUCCESS(hal_fdb_add(&fdb_entry));
    
    make_route_entry(&route_entry, 84, 24, 1, 2);
    ASSERT_SUCCESS(hal_route_add(&route_entry));
    
    hal_txn_begin(NULL, &txn);
    
    make_fdb_entry(&fdb_entry, 84, 100, 1);
    ASSERT_SUCCESS(hal_txn_add_fdb(txn, HAL_TXN_OP_DELETE, &fdb_entry));
    
    make_route_entry(&route_entry, 84, 24, 1, 2);
    ASSERT_SUCCESS(hal_txn_add_route(txn, HAL_TXN_OP_DELETE, &route_entry));
    
    make_fdb_entry(&fdb_entry, 85, 100, 1);
    ASSERT_SUCCESS(hal_txn_add_fdb(txn, HAL_TXN_OP_ADD, &fdb_entry));
    
    ASSERT_SUCCESS(hal_txn_commit(txn));
    
    make_fdb_entry(&fdb_entry, 84, 100, 1);
    hal_status_t rv = hal_fdb_get(fdb_entry.mac, fdb_entry.vlan_id, &fdb_entry);
    ASSERT_STATUS(HAL_E_NOT_FOUND, rv);
    
    ASSERT_SUCCESS(hal_txn_rollback(txn));
    
    make_fdb_entry(&fdb_entry, 84, 100, 1);
    rv = hal_fdb_get(fdb_entry.mac, fdb_entry.vlan_id, &fdb_entry);
    ASSERT_SUCCESS(rv);
    
    make_fdb_entry(&fdb_entry, 85, 100, 1);
    rv = hal_fdb_get(fdb_entry.mac, fdb_entry.vlan_id, &fdb_entry);
    ASSERT_STATUS(HAL_E_NOT_FOUND, rv);
    
    hal_txn_free(txn);
}

/* ============================================================================
 * Operation Type Tests (DELETE, UPDATE)
 * ============================================================================ */

TEST(txn_add_fdb_delete_op)
{
    hal_txn_t *txn = NULL;
    hal_fdb_entry_t entry;
    
    /* First add an entry directly */
    make_fdb_entry(&entry, 30, 100, 1);
    ASSERT_SUCCESS(hal_fdb_add(&entry));
    
    /* Now delete via transaction */
    hal_txn_begin(NULL, &txn);
    make_fdb_entry(&entry, 30, 100, 1);
    ASSERT_SUCCESS(hal_txn_add_fdb(txn, HAL_TXN_OP_DELETE, &entry));
    
    hal_status_t rv = hal_txn_commit(txn);
    ASSERT_SUCCESS(rv);
    
    /* Verify entry was deleted */
    make_fdb_entry(&entry, 30, 100, 1);
    rv = hal_fdb_get(entry.mac, entry.vlan_id, &entry);
    ASSERT_STATUS(HAL_E_NOT_FOUND, rv);
    
    hal_txn_free(txn);
}

TEST(txn_add_route_delete_op)
{
    hal_txn_t *txn = NULL;
    hal_route_entry_t entry;
    
    /* First add an entry directly */
    make_route_entry(&entry, 40, 24, 1, 2);
    ASSERT_SUCCESS(hal_route_add(&entry));
    
    /* Now delete via transaction */
    hal_txn_begin(NULL, &txn);
    make_route_entry(&entry, 40, 24, 1, 2);
    ASSERT_SUCCESS(hal_txn_add_route(txn, HAL_TXN_OP_DELETE, &entry));
    
    hal_status_t rv = hal_txn_commit(txn);
    ASSERT_SUCCESS(rv);
    
    /* Verify entry was deleted */
    make_route_entry(&entry, 40, 24, 1, 2);
    rv = hal_route_get(entry.vrf_id, entry.prefix, entry.prefix_len, &entry);
    ASSERT_STATUS(HAL_E_NOT_FOUND, rv);
    
    hal_txn_free(txn);
}

TEST(txn_add_fdb_update_op)
{
    hal_txn_t *txn = NULL;
    hal_fdb_entry_t entry;
    
    /* First add an entry directly */
    make_fdb_entry(&entry, 50, 100, 1);
    ASSERT_SUCCESS(hal_fdb_add(&entry));
    
    /* Now update via transaction */
    hal_txn_begin(NULL, &txn);
    make_fdb_entry(&entry, 50, 100, 2);  /* Change port */
    ASSERT_SUCCESS(hal_txn_add_fdb(txn, HAL_TXN_OP_UPDATE, &entry));
    
    hal_status_t rv = hal_txn_commit(txn);
    ASSERT_SUCCESS(rv);
    
    /* Verify entry was updated */
    make_fdb_entry(&entry, 50, 100, 2);
    rv = hal_fdb_get(entry.mac, entry.vlan_id, &entry);
    ASSERT_SUCCESS(rv);
    
    hal_txn_free(txn);
}

/* ============================================================================
 * Test Runner
 * ============================================================================ */

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;
    
    printf("\n" TF_CYAN("=== Transaction Manager Tests ===") "\n\n");
    
    test_setup();
    
    printf(TF_YELLOW("Basic Lifecycle Tests:\n"));
    RUN_TEST(txn_begin_basic);
    RUN_TEST(txn_begin_null_txn_pointer);
    RUN_TEST(txn_begin_with_options);
    RUN_TEST(txn_free_committed);
    RUN_TEST(txn_free_active);
    
    printf(TF_YELLOW("\nSingle Operation Tests:\n"));
    RUN_TEST(txn_add_fdb_basic);
    RUN_TEST(txn_add_route_basic);
    RUN_TEST(txn_add_null_params);
    RUN_TEST(txn_add_invalid_operation);
    
    printf(TF_YELLOW("\nCommit Tests:\n"));
    RUN_TEST(txn_commit_single_fdb_add);
    RUN_TEST(txn_commit_multiple_operations);
    RUN_TEST(txn_commit_empty_transaction);
    
    printf(TF_YELLOW("\nRollback Tests:\n"));
    RUN_TEST(txn_abort_before_commit);
    RUN_TEST(txn_rollback_after_commit);
    RUN_TEST(txn_commit_failure_cleans_up);
	RUN_TEST(txn_commit_asic_failure_cleans_up);
    RUN_TEST(txn_fdb_delete_missing_entry_releases_lock);
    RUN_TEST(txn_double_commit_fails);
    RUN_TEST(txn_rollback_fdb_add_removes_entry);
    RUN_TEST(txn_rollback_fdb_delete_restores_entry);
    RUN_TEST(txn_rollback_fdb_update_restores_original);
    RUN_TEST(txn_rollback_route_delete_restores_entry);
    RUN_TEST(txn_rollback_wrong_state_active);
    RUN_TEST(txn_rollback_wrong_state_pending);
    RUN_TEST(txn_abort_idempotent);
    RUN_TEST(txn_multiple_adds_rollback_all);
    RUN_TEST(txn_mixed_operations_rollback_all);
    
    printf(TF_YELLOW("\nOperation Type Tests:\n"));
    RUN_TEST(txn_add_fdb_delete_op);
    RUN_TEST(txn_add_route_delete_op);
    RUN_TEST(txn_add_fdb_update_op);
    
    test_teardown();
    
    /* Print summary */
    printf("\n" TF_CYAN("=== Test Summary ===") "\n");
    printf("Tests run:    %d\n", tf_tests_run);
    printf("Tests passed: " TF_GREEN("%d") "\n", tf_tests_passed);
    printf("Tests failed: " TF_RED("%d") "\n", tf_tests_failed);
    printf("Assertions:   %d\n\n", tf_assertions);
    
    return tf_tests_failed > 0 ? 1 : 0;
}

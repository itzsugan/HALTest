# SPDX-License-Identifier: Apache-2.0
# HAL Assessment Project - Meson Build Configuration

project('hal-assessment', 'c',
    version: '1.0.0',
    license: 'Apache-2.0',
    default_options: [
        'c_std=c11',
        'warning_level=2',
        'werror=false',
        'buildtype=debug',
    ],
)

# Compiler configuration
cc = meson.get_compiler('c')

# Common compile arguments
add_project_arguments([
    '-D_GNU_SOURCE',
    '-D_POSIX_C_SOURCE=200809L',
], language: 'c')

# Dependencies
thread_dep = dependency('threads')

# Include directories
inc_dirs = include_directories('include')

# =============================================================================
# Library: Mock ASIC Driver
# =============================================================================

asic_sources = files(
    'src/asic/mock_asic_driver.c',
)

asic_lib = static_library('mock_asic',
    asic_sources,
    include_directories: inc_dirs,
    dependencies: [thread_dep],
)

# =============================================================================
# Library: HAL
# =============================================================================

hal_sources = files(
    'src/hal_init.c',
    'src/hal_fdb.c',
    'src/hal_route.c',
    'src/hal_resource.c',
    'src/hal_txn.c',
)

hal_lib = static_library('hal',
    hal_sources,
    include_directories: inc_dirs,
    link_with: [asic_lib],
    dependencies: [thread_dep],
)

# =============================================================================
# Tests
# =============================================================================

# Test framework header is in tests/
test_inc_dirs = include_directories('include', 'tests')

# FDB Tests
test_fdb = executable('test_fdb',
    'tests/test_fdb.c',
    include_directories: test_inc_dirs,
    link_with: [hal_lib, asic_lib],
    dependencies: [thread_dep],
)

test('fdb_basic', test_fdb,
    args: ['--basic'],
    timeout: 30,
)

test('fdb_bulk', test_fdb,
    args: ['--bulk'],
    timeout: 60,
)

# FDB Aging Tests
test_fdb_aging = executable('test_fdb_aging',
    'tests/test_fdb_aging.c',
    include_directories: test_inc_dirs,
    link_with: [hal_lib, asic_lib],
    dependencies: [thread_dep],
)

test('fdb_aging_basic', test_fdb_aging,
    args: ['--basic'],
    timeout: 30,
)

test('fdb_aging_bulk', test_fdb_aging,
    args: ['--bulk'],
    timeout: 60,
)

# Route Tests
test_route = executable('test_route',
    'tests/test_route.c',
    include_directories: test_inc_dirs,
    link_with: [hal_lib, asic_lib],
    dependencies: [thread_dep],
)

test('route_basic', test_route,
    args: ['--basic'],
    timeout: 30,
)

test('route_lpm', test_route,
    args: ['--lpm'],
    timeout: 30,
)

# Integration Tests
test_integration = executable('test_integration',
    'tests/test_integration.c',
    include_directories: test_inc_dirs,
    link_with: [hal_lib, asic_lib],
    dependencies: [thread_dep],
)

test('integration', test_integration,
    timeout: 60,
)

# Transaction Manager Tests
test_txn = executable('test_txn',
    'tests/test_txn.c',
    include_directories: test_inc_dirs,
    link_with: [hal_lib, asic_lib],
    dependencies: [thread_dep],
)

test('txn_basic', test_txn,
    args: ['--basic'],
    timeout: 30,
)

# =============================================================================
# Summary
# =============================================================================

summary({
    'Build type': get_option('buildtype'),
    'C compiler': cc.get_id(),
    'C std': get_option('c_std'),
}, section: 'Build Configuration')

summary({
    'HAL library': hal_lib.full_path(),
    'ASIC library': asic_lib.full_path(),
}, section: 'Libraries')

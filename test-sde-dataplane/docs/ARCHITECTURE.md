# HAL Architecture Overview

This document describes the architecture of the Hardware Abstraction Layer (HAL) and its components.

## System Overview

```
┌─────────────────────────────────────────────────────────────────┐
│                     Control Plane Applications                   │
│              (BGP, OSPF, CLI, SNMP, REST API, etc.)             │
└─────────────────────────────────────────────────────────────────┘
                                │
                                ▼
┌─────────────────────────────────────────────────────────────────┐
│                    Hardware Abstraction Layer                    │
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────┐             │
│  │   hal_fdb   │  │  hal_route  │  │ hal_resource│             │
│  │    (L2)     │  │    (L3)     │  │   (pools)   │             │
│  └─────────────┘  └─────────────┘  └─────────────┘             │
│                                                                  │
│  ┌─────────────────────────────────────────────────────────────┐│
│  │                      hal_init                                ││
│  │            (initialization, lifecycle, config)               ││
│  └─────────────────────────────────────────────────────────────┘│
└─────────────────────────────────────────────────────────────────┘
                                │
                                ▼
┌─────────────────────────────────────────────────────────────────┐
│                      ASIC Driver Layer                           │
│  ┌─────────────────────────────────────────────────────────────┐│
│  │              mock_asic_driver (or vendor SDK)                ││
│  │         (L2 tables, L3 tables, resource management)          ││
│  └─────────────────────────────────────────────────────────────┘│
└─────────────────────────────────────────────────────────────────┘
                                │
                                ▼
┌─────────────────────────────────────────────────────────────────┐
│                         ASIC Hardware                            │
│                    (or Mock In-Memory Tables)                    │
└─────────────────────────────────────────────────────────────────┘
```

## Component Descriptions

### HAL Types (`hal_types.h`)

Defines fundamental types used throughout the system:

- **`hal_mac_t`** - 6-byte MAC address
- **`hal_vlan_t`** - VLAN ID (1-4094)
- **`hal_port_t`** - Port identifier (unit + port number)
- **`hal_vrf_t`** - Virtual Router ID
- **`hal_ipv4_t`** - IPv4 address (host byte order)
- **`hal_object_id_t`** - Opaque handle for ASIC resources
- **`hal_flags_t`** - Common entry flags (STATIC, HIT, DROP, etc.)

### Error Handling (`hal_error.h`)

Standardized error codes returned by all HAL functions:

```c
typedef enum hal_status_e {
    HAL_SUCCESS         = 0,
    HAL_E_PARAM         = -1,   // Invalid parameter
    HAL_E_NOT_FOUND     = -20,  // Entry not found
    HAL_E_EXISTS        = -21,  // Entry already exists
    HAL_E_FULL          = -12,  // Table full
    // ... etc
} hal_status_t;
```

Helper macros:
- `HAL_IF_ERROR_RETURN(rv)` - Return if error
- `HAL_NULL_CHECK(ptr)` - Return `HAL_E_NULL` if NULL
- `ASSERT_SUCCESS(rv)` - Test assertion for success

### HAL Initialization (`hal_init.h`, `hal_init.c`)

Manages HAL lifecycle:

```c
// Initialize with custom configuration
hal_config_t config;
hal_config_init(&config);
config.num_units = 1;
config.asic_latency_us = 1000;
hal_init(&config);

// Shutdown
hal_shutdown();
```

### L2 FDB Module (`hal_fdb.h`, `hal_fdb.c`)

Manages the L2 forwarding database (MAC address table).

**Data Structures:**
```c
typedef struct hal_fdb_entry_s {
    hal_mac_t       mac;        // Key
    hal_vlan_t      vlan_id;    // Key
    hal_port_t      port;       // Destination port
    hal_flags_t     flags;      // STATIC, HIT, etc.
    uint32_t        age;        // Current age (read-only)
    hal_object_id_t object_id;  // ASIC handle (read-only)
} hal_fdb_entry_t;
```

**Operations:**
- `hal_fdb_add()` - Add entry
- `hal_fdb_delete()` - Delete by key
- `hal_fdb_get()` - Lookup by key
- `hal_fdb_traverse()` - Iterate all entries
- `hal_fdb_delete_by_port()` - Bulk delete by port
- `hal_fdb_delete_by_vlan()` - Bulk delete by VLAN

**Aging (To Be Implemented):**
- `hal_fdb_aging_start()` - Start aging thread
- `hal_fdb_aging_stop()` - Stop aging thread
- `hal_fdb_age_callback_register()` - Register age-out notification

### L3 Route Module (`hal_route.h`, `hal_route.c`)

Manages the L3 IPv4 routing table with VRF support.

**Data Structures:**
```c
typedef struct hal_route_entry_s {
    hal_vrf_t       vrf_id;      // Key
    hal_ipv4_t      prefix;      // Key
    uint8_t         prefix_len;  // Key
    hal_ipv4_t      nexthop;
    hal_port_t      egress_port;
    hal_mac_t       nexthop_mac;
    hal_object_id_t object_id;
} hal_route_entry_t;
```

**Operations:**
- `hal_route_add()` - Add route
- `hal_route_delete()` - Delete route
- `hal_route_lookup()` - Longest prefix match
- `hal_route_traverse()` - Iterate routes

### Resource Management (`hal_resource.h`, `hal_resource.c`)

Tracks resource allocation across table types.

**Pool Operations:**
```c
hal_resource_pool_t *pool;
hal_resource_pool_create(HAL_RESOURCE_FDB_ENTRY, 16384, &pool);

hal_object_id_t id;
hal_resource_alloc(pool, &id);
hal_resource_free(pool, id);

// For transactions
hal_resource_reserve(pool, 10);
hal_resource_unreserve(pool, 10);
```

### ASIC Driver (`asic_driver.h`, `mock_asic_driver.c`)

Low-level interface to the ASIC hardware. In production, this would wrap vendor SDK calls. Our mock implementation provides:

- In-memory table storage
- Configurable latency simulation
- Error injection for testing
- Thread-safe operations

```c
// Direct ASIC operations (usually called by HAL, not applications)
asic_l2_add(unit, mac, vlan, port, flags, &oid);
asic_l2_delete(unit, oid);
asic_l3_route_add(unit, vrf, prefix, len, nexthop, port, mac, &oid);

// Simulation control
asic_set_latency(unit, 1000);    // 1ms per operation
asic_inject_error(unit, HAL_E_FULL, 5);  // Next 5 ops fail
asic_simulate_hits(unit, 50);    // Mark 50% entries as hit
```

## Data Flow

### Adding an FDB Entry

```
Application                HAL (hal_fdb)           ASIC Driver
    │                           │                       │
    │  hal_fdb_add(&entry)      │                       │
    │──────────────────────────>│                       │
    │                           │  asic_l2_add(...)     │
    │                           │──────────────────────>│
    │                           │                       │ (simulate latency)
    │                           │      HAL_SUCCESS      │ (store in memory)
    │                           │<──────────────────────│
    │                           │                       │
    │                           │ (store in SW table)   │
    │                           │ (update object_id)    │
    │      HAL_SUCCESS          │                       │
    │<──────────────────────────│                       │
```

### FDB Aging (When Implemented)

```
                          Aging Thread
                               │
                               │ (periodic scan)
                               ▼
                    ┌────────────────────┐
                    │  For each entry:   │
                    │  - Check age       │
                    │  - Check HIT bit   │
                    └────────────────────┘
                               │
              ┌────────────────┼────────────────┐
              │                │                │
              ▼                ▼                ▼
        HIT bit set      Age expired      Age OK
              │                │                │
              │                │                │
              ▼                ▼                ▼
        Clear HIT,      Invoke callbacks,   Continue
        reset age       delete entry
```

## Threading Model

### Current Implementation

- HAL uses read-write locks (`pthread_rwlock_t`) for table access
- Read operations (get, traverse) acquire read lock
- Write operations (add, delete, update) acquire write lock
- ASIC driver uses mutex for internal state protection

### Aging Thread (To Be Implemented)

The aging thread should:

1. Sleep using condition variable (not busy-wait)
2. Wake periodically or when signaled to stop
3. Acquire write lock only when modifying entries
4. Invoke callbacks before deleting entries
5. Handle graceful shutdown

## Memory Management

- HAL allocates entry structures dynamically (`calloc`)
- ASIC driver pre-allocates table arrays
- Resource pools track allocation without storing entry data
- No reference counting - entries are owned by single table

## Error Handling

All functions follow these conventions:

1. Check parameters first (NULL, range)
2. Check initialization state
3. Perform operation
4. Return status code

```c
hal_status_t hal_fdb_add(hal_fdb_entry_t *entry)
{
    HAL_NULL_CHECK(entry);

    if (!hal_is_initialized()) {
        return HAL_E_INIT;
    }

    if (entry->vlan_id < HAL_VLAN_MIN) {
        return HAL_E_RANGE;
    }

    // ... perform operation

    return HAL_SUCCESS;
}
```

## Extension Points

### Adding New Table Types

1. Define entry structure in new header
2. Implement CRUD operations following existing patterns
3. Add ASIC driver functions if needed
4. Add resource type to `hal_resource_type_t`
5. Create resource pool in `hal_init()`
6. Add tests

### Adding Transaction Support (Principal Track)

1. Define transaction structure and states
2. Implement operation recording (journal)
3. Implement rollback logic (inverse operations)
4. Add locking strategy for concurrent transactions
5. Integrate with existing table managers

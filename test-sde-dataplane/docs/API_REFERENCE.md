# HAL API Reference

## Table of Contents

- [Error Handling](#error-handling)
- [HAL Initialization](#hal-initialization)
- [FDB (L2) API](#fdb-l2-api)
- [Route (L3) API](#route-l3-api)
- [Resource Management API](#resource-management-api)
- [ASIC Driver API](#asic-driver-api)

---

## Error Handling

### Status Codes

```c
typedef enum hal_status_e {
    HAL_SUCCESS         = 0,

    /* Parameter errors */
    HAL_E_PARAM         = -1,   /* Invalid parameter */
    HAL_E_NULL          = -2,   /* NULL pointer passed */
    HAL_E_RANGE         = -3,   /* Value out of range */

    /* Resource errors */
    HAL_E_MEMORY        = -10,  /* Memory allocation failed */
    HAL_E_RESOURCE      = -11,  /* Resource exhausted */
    HAL_E_FULL          = -12,  /* Table/buffer full */

    /* State errors */
    HAL_E_NOT_FOUND     = -20,  /* Entry not found */
    HAL_E_EXISTS        = -21,  /* Entry already exists */
    HAL_E_BUSY          = -22,  /* Resource busy */

    /* Operation errors */
    HAL_E_TIMEOUT       = -30,  /* Operation timed out */
    HAL_E_INIT          = -40,  /* Not initialized */

    /* Internal errors */
    HAL_E_INTERNAL      = -100, /* Internal error */
    HAL_E_NOT_IMPL      = -101, /* Not implemented */
} hal_status_t;
```

### Helper Functions

```c
const char *hal_status_str(hal_status_t status);
```

Returns human-readable string for status code.

### Helper Macros

```c
HAL_IF_ERROR_RETURN(status)      // Return if status != HAL_SUCCESS
HAL_NULL_CHECK(ptr)              // Return HAL_E_NULL if ptr is NULL
HAL_PARAM_CHECK(cond)            // Return HAL_E_PARAM if cond is false
HAL_RANGE_CHECK(val, min, max)   // Return HAL_E_RANGE if out of range
```

---

## HAL Initialization

### hal_config_init

```c
void hal_config_init(hal_config_t *config);
```

Initialize configuration structure with default values.

**Parameters:**
- `config` - Configuration structure to initialize

### hal_init

```c
hal_status_t hal_init(const hal_config_t *config);
```

Initialize HAL subsystem. Must be called before other HAL functions.

**Parameters:**
- `config` - Configuration (NULL for defaults)

**Returns:** `HAL_SUCCESS` or error code

### hal_shutdown

```c
hal_status_t hal_shutdown(void);
```

Shutdown HAL and release all resources.

### hal_is_initialized

```c
bool hal_is_initialized(void);
```

Check if HAL is initialized.

### hal_version_get

```c
hal_status_t hal_version_get(hal_version_t *version);
```

Get HAL version information.

---

## FDB (L2) API

### Data Types

```c
typedef struct hal_fdb_entry_s {
    hal_mac_t       mac;        /* MAC address (key) */
    hal_vlan_t      vlan_id;    /* VLAN ID (key) */
    hal_port_t      port;       /* Destination port */
    hal_flags_t     flags;      /* Entry flags */
    uint32_t        age;        /* Current age in seconds (read-only) */
    hal_object_id_t object_id;  /* ASIC handle (read-only) */
} hal_fdb_entry_t;
```

### hal_fdb_entry_init

```c
void hal_fdb_entry_init(hal_fdb_entry_t *entry,
                        const hal_mac_t mac,
                        hal_vlan_t vlan_id);
```

Initialize FDB entry with default values.

### hal_fdb_add

```c
hal_status_t hal_fdb_add(hal_fdb_entry_t *entry);
```

Add FDB entry.

**Parameters:**
- `entry` - Entry to add (object_id populated on success)

**Returns:**
- `HAL_SUCCESS` - Entry added
- `HAL_E_EXISTS` - Entry already exists
- `HAL_E_FULL` - Table full
- `HAL_E_PARAM` - Invalid MAC or VLAN

### hal_fdb_delete

```c
hal_status_t hal_fdb_delete(const hal_mac_t mac, hal_vlan_t vlan_id);
```

Delete FDB entry by key.

**Returns:**
- `HAL_SUCCESS` - Entry deleted
- `HAL_E_NOT_FOUND` - Entry not found

### hal_fdb_get

```c
hal_status_t hal_fdb_get(const hal_mac_t mac,
                         hal_vlan_t vlan_id,
                         hal_fdb_entry_t *entry);
```

Get FDB entry by key.

### hal_fdb_update

```c
hal_status_t hal_fdb_update(const hal_fdb_entry_t *entry);
```

Update existing FDB entry (non-key fields).

### hal_fdb_delete_by_port

```c
hal_status_t hal_fdb_delete_by_port(hal_port_t port, uint32_t flags);
```

Delete all entries associated with a port.

**Parameters:**
- `port` - Port to match
- `flags` - Match flags (0 = non-static only, `HAL_FLAG_STATIC` = include static)

### hal_fdb_delete_by_vlan

```c
hal_status_t hal_fdb_delete_by_vlan(hal_vlan_t vlan_id, uint32_t flags);
```

Delete all entries in a VLAN.

### hal_fdb_traverse

```c
typedef int (*hal_fdb_traverse_cb_t)(const hal_fdb_entry_t *entry,
                                     void *user_data);

hal_status_t hal_fdb_traverse(hal_fdb_traverse_cb_t callback,
                              void *user_data);
```

Traverse all FDB entries. Callback returns 0 to continue, non-zero to stop.

### hal_fdb_stats_get

```c
typedef struct hal_fdb_stats_s {
    uint32_t count;
    uint32_t capacity;
    uint32_t static_count;
    uint32_t dynamic_count;
} hal_fdb_stats_t;

hal_status_t hal_fdb_stats_get(hal_fdb_stats_t *stats);
```

Get FDB table statistics.

### Aging Functions (To Be Implemented)

```c
/* Callback invoked before entry is aged out */
typedef void (*hal_fdb_age_cb_t)(const hal_fdb_entry_t *entry, void *context);

hal_status_t hal_fdb_age_callback_register(hal_fdb_age_cb_t callback,
                                           void *context);
hal_status_t hal_fdb_age_callback_unregister(hal_fdb_age_cb_t callback);

hal_status_t hal_fdb_aging_set(uint32_t aging_time_sec);
hal_status_t hal_fdb_aging_get(uint32_t *aging_time_sec);
hal_status_t hal_fdb_aging_interval_set(uint32_t interval_sec);
hal_status_t hal_fdb_aging_start(void);
hal_status_t hal_fdb_aging_stop(void);
bool hal_fdb_aging_is_running(void);
```

---

## Route (L3) API

### Data Types

```c
typedef struct hal_route_entry_s {
    hal_vrf_t       vrf_id;         /* VRF (key) */
    hal_ipv4_t      prefix;         /* IPv4 prefix (key) */
    uint8_t         prefix_len;     /* Prefix length 0-32 (key) */
    hal_ipv4_t      nexthop;        /* Next hop IP */
    hal_port_t      egress_port;    /* Egress port */
    hal_mac_t       nexthop_mac;    /* Next hop MAC */
    hal_flags_t     flags;          /* Route flags */
    uint32_t        priority;       /* Route metric */
    hal_object_id_t object_id;      /* ASIC handle */
} hal_route_entry_t;
```

### hal_route_entry_init

```c
void hal_route_entry_init(hal_route_entry_t *entry);
```

Initialize route entry with defaults.

### hal_route_add

```c
hal_status_t hal_route_add(hal_route_entry_t *entry);
```

Add route entry.

### hal_route_delete

```c
hal_status_t hal_route_delete(hal_vrf_t vrf,
                              hal_ipv4_t prefix,
                              uint8_t prefix_len);
```

Delete route by key.

### hal_route_get

```c
hal_status_t hal_route_get(hal_vrf_t vrf,
                           hal_ipv4_t prefix,
                           uint8_t prefix_len,
                           hal_route_entry_t *entry);
```

Get route by exact key match.

### hal_route_lookup

```c
hal_status_t hal_route_lookup(hal_vrf_t vrf,
                              hal_ipv4_t dest_ip,
                              hal_route_entry_t *entry);
```

Longest prefix match lookup.

### hal_route_traverse

```c
typedef int (*hal_route_traverse_cb_t)(const hal_route_entry_t *entry,
                                       void *user_data);

hal_status_t hal_route_traverse(hal_route_traverse_cb_t callback,
                                void *user_data);
```

Traverse all routes.

### hal_route_stats_get

```c
typedef struct hal_route_stats_s {
    uint32_t count;
    uint32_t capacity;
    uint32_t ipv4_count;
    uint32_t ipv6_count;
} hal_route_stats_t;

hal_status_t hal_route_stats_get(hal_route_stats_t *stats);
```

---

## Resource Management API

### Data Types

```c
typedef enum hal_resource_type_e {
    HAL_RESOURCE_FDB_ENTRY,
    HAL_RESOURCE_ROUTE_ENTRY,
    HAL_RESOURCE_ACL_ENTRY,
    HAL_RESOURCE_COUNTER,
    HAL_RESOURCE_NEXTHOP,
    HAL_RESOURCE_ECMP_GROUP,
    HAL_RESOURCE_MAX
} hal_resource_type_t;

typedef struct hal_resource_pool_s hal_resource_pool_t;
```

### hal_resource_pool_create

```c
hal_status_t hal_resource_pool_create(hal_resource_type_t type,
                                      uint32_t capacity,
                                      hal_resource_pool_t **pool);
```

Create resource pool.

### hal_resource_pool_destroy

```c
hal_status_t hal_resource_pool_destroy(hal_resource_pool_t *pool);
```

Destroy pool (must be empty).

### hal_resource_alloc

```c
hal_status_t hal_resource_alloc(hal_resource_pool_t *pool,
                                hal_object_id_t *id);
```

Allocate resource from pool.

### hal_resource_free

```c
hal_status_t hal_resource_free(hal_resource_pool_t *pool,
                               hal_object_id_t id);
```

Free resource back to pool.

### hal_resource_reserve / unreserve

```c
hal_status_t hal_resource_reserve(hal_resource_pool_t *pool, uint32_t count);
hal_status_t hal_resource_unreserve(hal_resource_pool_t *pool, uint32_t count);
```

Reserve resources without allocating (for transaction pre-validation).

### hal_resource_stats_get

```c
typedef struct hal_resource_stats_s {
    uint32_t capacity;
    uint32_t allocated;
    uint32_t reserved;
    uint32_t available;
    uint32_t high_watermark;
} hal_resource_stats_t;

hal_status_t hal_resource_stats_get(hal_resource_pool_t *pool,
                                    hal_resource_stats_t *stats);
```

---

## ASIC Driver API

### Unit Management

```c
hal_status_t asic_init(asic_unit_t unit);
hal_status_t asic_shutdown(asic_unit_t unit);
bool asic_is_initialized(asic_unit_t unit);
hal_status_t asic_capabilities_get(asic_unit_t unit, asic_capabilities_t *caps);
```

### L2 Operations

```c
hal_status_t asic_l2_add(asic_unit_t unit, const hal_mac_t mac,
                         hal_vlan_t vlan, hal_port_t port,
                         uint32_t flags, hal_object_id_t *oid);

hal_status_t asic_l2_delete(asic_unit_t unit, hal_object_id_t oid);

hal_status_t asic_l2_get(asic_unit_t unit, const hal_mac_t mac,
                         hal_vlan_t vlan, hal_port_t *port,
                         uint32_t *flags, hal_object_id_t *oid);

hal_status_t asic_l2_traverse(asic_unit_t unit,
                              asic_l2_traverse_cb_t callback,
                              void *user_data);
```

### L3 Operations

```c
hal_status_t asic_l3_route_add(asic_unit_t unit, hal_vrf_t vrf,
                               hal_ipv4_t prefix, uint8_t prefix_len,
                               hal_ipv4_t nexthop, hal_port_t egress_port,
                               const hal_mac_t nexthop_mac,
                               hal_object_id_t *oid);

hal_status_t asic_l3_route_delete(asic_unit_t unit, hal_object_id_t oid);
```

### Simulation Control (Mock Only)

```c
void asic_set_latency(asic_unit_t unit, uint32_t latency_us);
uint32_t asic_get_latency(asic_unit_t unit);
void asic_inject_error(asic_unit_t unit, hal_status_t error, uint32_t count);
void asic_simulate_hits(asic_unit_t unit, uint32_t percentage);
```

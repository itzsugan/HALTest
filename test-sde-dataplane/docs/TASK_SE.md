# Software Engineer Track: FDB Entry Aging

## Background

In network switches, the Forwarding Database (FDB) stores learned MAC addresses. These entries must be "aged out" if not refreshed, to:

1. Prevent stale forwarding decisions when devices move
2. Free table space for new learning
3. Handle network topology changes

Real ASICs support hardware-based aging with hit-bit tracking. When a packet matches an FDB entry, the hardware sets a "HIT" bit. Software periodically scans entries, clearing HIT bits and removing entries that haven't been hit.

## Your Task

Implement FDB entry aging with notification callbacks in the HAL.

**Time Estimate:** 2-4 hours

## Requirements

### 1. Aging Thread

Create a background thread that periodically scans the FDB table.

**Behavior:**
- Default scan interval: 10 seconds (configurable via `hal_fdb_aging_interval_set`)
- Default aging time: 300 seconds (configurable via `hal_fdb_aging_set`)
- For each entry:
  - If `HAL_FLAG_STATIC` is set: skip (never age static entries)
  - If `HAL_FLAG_HIT` is set: clear it and reset the entry's age
  - If age > aging_time: invoke callbacks, then delete the entry
- Thread should sleep efficiently (use condition variables, NOT busy-wait)

**Thread Safety:**
- Aging thread must coexist with concurrent CRUD operations
- Use appropriate locking (the FDB already uses `pthread_rwlock_t`)
- Don't hold locks during callback invocation (callbacks may call HAL functions)

### 2. Callback Registration

Allow multiple callbacks to be notified when entries age out.

**API:**
```c
typedef void (*hal_fdb_age_cb_t)(const hal_fdb_entry_t *entry, void *context);

hal_status_t hal_fdb_age_callback_register(hal_fdb_age_cb_t callback,
                                           void *context);
hal_status_t hal_fdb_age_callback_unregister(hal_fdb_age_cb_t callback);
```

**Requirements:**
- Support up to 8 registered callbacks (`FDB_MAX_AGE_CALLBACKS`)
- Return `HAL_E_FULL` if max callbacks reached
- Callbacks are invoked BEFORE the entry is removed
- Callbacks receive a copy of the entry data (not a pointer to internal state)
- Use `g_fdb_callback_lock` mutex for thread safety

### 3. Configuration APIs

Implement the aging configuration functions:

```c
hal_status_t hal_fdb_aging_set(uint32_t aging_time_sec);
hal_status_t hal_fdb_aging_get(uint32_t *aging_time_sec);
hal_status_t hal_fdb_aging_interval_set(uint32_t interval_sec);
hal_status_t hal_fdb_aging_interval_get(uint32_t *interval_sec);
```

- Aging time of 0 disables aging (entries never expire)
- Changing the scan interval should wake the sleeping thread

### 4. Thread Control

```c
hal_status_t hal_fdb_aging_start(void);
hal_status_t hal_fdb_aging_stop(void);
bool hal_fdb_aging_is_running(void);
```

- `start()` returns `HAL_E_EXISTS` if already running
- `stop()` signals thread and waits for clean exit (pthread_join)
- `stop()` should return `HAL_SUCCESS` even if not running
- Clean shutdown when `hal_shutdown()` is called

## Implementation Hints

### Existing Code

Look at `src/hal_fdb.c`:
- `fdb_sw_entry_t` has `last_update` timestamp
- `g_fdb_aging` structure holds configuration
- `g_fdb_callbacks` array for callback storage
- `g_fdb_callback_lock` mutex for callback list protection

### Suggested Approach

1. **Implement callback registration first** (simplest)
   - Add to `g_fdb_callbacks` array
   - Handle duplicates and full array

2. **Implement configuration APIs**
   - Store in `g_fdb_aging` structure
   - Add locking as needed

3. **Implement aging thread**
   - Use `pthread_create` / `pthread_join`
   - Sleep with `pthread_cond_timedwait` (allows early wakeup)
   - Scan entries while holding read lock
   - Build list of entries to delete
   - Release lock, invoke callbacks
   - Acquire write lock, delete entries

4. **Handle HIT bit**
   - Use `asic_l2_hit_clear()` to clear in ASIC
   - Update `last_update` timestamp

### Thread Loop Pseudocode

```c
while (running) {
    // Wait for interval or stop signal
    pthread_cond_timedwait(...);

    if (!running) break;
    if (aging_disabled) continue;

    // Scan phase (read lock)
    pthread_rwlock_rdlock(&table->lock);
    for each entry:
        if (entry->flags & HAL_FLAG_STATIC) continue;
        if (entry->flags & HAL_FLAG_HIT) {
            // Mark for reset (don't modify while holding read lock)
            add_to_reset_list(entry);
        } else if (age > aging_time) {
            add_to_delete_list(entry);
        }
    pthread_rwlock_unlock(&table->lock);

    // Process resets (need write lock for each)
    for each reset_entry:
        clear_hit_bit_and_reset_age(entry);

    // Invoke callbacks (no lock held!)
    for each delete_entry:
        invoke_callbacks(entry);

    // Delete phase (write lock)
    pthread_rwlock_wrlock(&table->lock);
    for each delete_entry:
        remove_from_table(entry);
    pthread_rwlock_unlock(&table->lock);
}
```

## Testing

Create `tests/test_fdb_aging.c` with tests for:

1. **Basic aging**
   - Add dynamic entry, wait, verify it's removed
   - Add static entry, wait, verify it's NOT removed

2. **Callback invocation**
   - Register callback, add entry, wait for aging
   - Verify callback was called with correct data

3. **Multiple callbacks**
   - Register multiple callbacks
   - Verify all are invoked

4. **Configuration**
   - Test `aging_set` / `aging_get`
   - Test `interval_set` / `interval_get`
   - Test aging disabled (time = 0)

5. **Thread control**
   - Test start/stop/start sequence
   - Test double-start returns error
   - Test stop when not running

6. **Hit bit handling**
   - Use `asic_simulate_hits()` to set hit bits
   - Verify entries with HIT aren't deleted
   - Verify HIT bit is cleared after scan

7. **Thread safety**
   - Concurrent add/delete during aging
   - No crashes or deadlocks

## Deliverables

1. **Modified `src/hal_fdb.c`** with aging implementation
2. **New `tests/test_fdb_aging.c`** with comprehensive tests
3. **Brief notes** (in code comments or separate file) explaining:
   - Your locking strategy
   - Any design decisions or trade-offs

## Evaluation Criteria

| Category | Weight | Description |
|----------|--------|-------------|
| **Correctness** | 30% | Aging works correctly, static entries preserved, callbacks invoked |
| **Thread Safety** | 25% | No race conditions, deadlocks, or crashes under concurrent access |
| **Code Quality** | 20% | Clean, readable code consistent with existing style |
| **Testing** | 15% | Comprehensive tests covering edge cases |
| **API Design** | 10% | APIs consistent with existing HAL patterns |

## What We're Looking For

**Strong signals:**
- Uses condition variables (not busy-waiting or `sleep()`)
- Considers callback blocking (doesn't hold locks during callbacks)
- Handles edge cases (stop during scan, empty table, etc.)
- Tests include concurrency scenarios

**Red flags:**
- Busy-waiting or polling
- Holding locks while invoking callbacks
- No error handling
- Only happy-path tests
- Memory leaks

## Questions?

If requirements are unclear, document your assumptions and proceed. We're interested in how you handle ambiguity as well as your technical implementation.

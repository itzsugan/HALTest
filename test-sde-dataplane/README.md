# HAL Assessment - Dataplane Software Engineer

A technical assessment for evaluating Software Engineer candidates for the Hardware Abstraction Layer (HAL) team. This project provides a mock HAL codebase inspired by real-world ASIC SDKs (Broadcom, SONiC) for candidates to extend with new functionality.

## Overview

This repository contains a simplified but realistic HAL implementation that abstracts away ASIC details while providing APIs for:

- **L2 Forwarding Database (FDB)** - MAC address table management
- **L3 Route Table** - IPv4 routing with VRF support
- **Resource Management** - Table entry allocation tracking
- **Mock ASIC Driver** - Simulated hardware with configurable behavior

## Quick Start

### Prerequisites

- GCC or Clang (C11 support required)
- Meson build system (>= 0.50)
- Ninja build tool
- POSIX threads library (pthread)

### Build

```bash
# Configure (first time)
meson setup build

# Build
meson compile -C build

# Run tests
meson test -C build
```

### Run Individual Tests

```bash
# All FDB tests
./build/test_fdb

# Specific FDB test suite
./build/test_fdb --basic
./build/test_fdb --bulk

# All route tests
./build/test_route

# Specific route test suite
./build/test_route --basic
./build/test_route --lpm

# Integration tests
./build/test_integration
```

## Project Structure

```
.
├── include/
│   ├── hal_types.h          # Common type definitions
│   ├── hal_error.h          # Error codes
│   ├── hal_init.h           # HAL initialization
│   ├── hal_fdb.h            # L2 FDB API
│   ├── hal_route.h          # L3 Route API
│   ├── hal_resource.h       # Resource pool API
│   └── asic/
│       └── asic_driver.h    # Mock ASIC interface
├── src/
│   ├── hal_init.c
│   ├── hal_fdb.c            # FDB implementation (partial)
│   ├── hal_route.c
│   ├── hal_resource.c
│   └── asic/
│       └── mock_asic_driver.c
├── tests/
│   ├── test_framework.h     # Test macros
│   ├── test_fdb.c
│   ├── test_route.c
│   └── test_integration.c
├── docs/
│   ├── ARCHITECTURE.md      # System design
│   ├── API_REFERENCE.md     # API documentation
│   ├── TASK_SE.md           # SE track assignment
│   └── TASK_PRINCIPAL.md    # Principal track assignment
└── meson.build
```

## Assessment Tracks

This assessment has two tracks based on experience level:

### Software Engineer Track

**Task**: Implement FDB entry aging with callbacks

See [docs/TASK_SE.md](docs/TASK_SE.md) for detailed requirements.

Key areas tested:
- Thread synchronization (mutexes, condition variables)
- Callback patterns
- API design consistency
- Test coverage

### Principal Software Engineer Track

**Task**: Design and implement a multi-table transaction manager

See [docs/TASK_PRINCIPAL.md](docs/TASK_PRINCIPAL.md) for detailed requirements.

Key areas tested:
- System architecture
- Concurrency design
- Rollback mechanisms
- Trade-off analysis

## Documentation

- [Architecture Overview](docs/ARCHITECTURE.md) - System design and component interaction
- [API Reference](docs/API_REFERENCE.md) - Detailed API documentation

## API Patterns

The HAL follows common SDK patterns:

```c
// Entry initialization
hal_fdb_entry_t entry;
hal_fdb_entry_init(&entry, mac, vlan);
entry.port = HAL_PORT_MAKE(0, 1);

// CRUD operations
hal_fdb_add(&entry);
hal_fdb_get(mac, vlan, &entry);
hal_fdb_delete(mac, vlan);

// Traversal
hal_fdb_traverse(callback_fn, user_data);

// Status handling
hal_status_t rv = hal_fdb_add(&entry);
if (rv != HAL_SUCCESS) {
    printf("Error: %s\n", hal_status_str(rv));
}
```

## Mock ASIC Features

The mock ASIC driver supports:

- Configurable table sizes (default: 16K FDB, 8K routes)
- Simulated operation latency (default: 1ms)
- Error injection for testing
- Hit-bit simulation for aging tests

```c
// Disable latency for faster tests
asic_set_latency(0, 0);

// Inject errors
asic_inject_error(0, HAL_E_FULL, 3);  // Next 3 ops return E_FULL

// Simulate hardware hits
asic_simulate_hits(0, 50);  // Mark 50% of entries as "hit"
```

## Evaluation

Your submission will be evaluated on:

1. **Correctness** - Does the implementation meet requirements?
2. **Code Quality** - Is the code readable, maintainable, consistent?
3. **Thread Safety** - Are concurrent operations handled correctly?
4. **Testing** - Are edge cases covered?
5. **Design** - Are APIs consistent with existing patterns?

## Submission

1. Create a new branch for your work
2. Implement the required functionality
3. Add tests for your implementation
4. Include a brief README or design notes explaining your approach
5. Submit a pull request

## Questions?

If you have questions about the requirements or need clarification, please reach out to your recruiter or the technical contact provided.

---

*This assessment is designed to evaluate practical skills relevant to HAL development. Good luck!*

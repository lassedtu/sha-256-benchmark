/**
 * @file registry.c
 *
 * Registry for all reverse-hashing solvers.
 *
 * Add a new solver here so the benchmark can test it.
 */

#include "registry.h"

// These declarations tell the linker that these records exist.
extern const Solver solver_linear;
extern const Solver solver_threaded;

// This array holds the full list of algorithms for the benchmark.
static const Solver *const all_solvers[] = {
    &solver_linear,
    &solver_threaded,
};

// These two symbols give the rest of the program a view of the list.
const Solver *const *const solver_registry = all_solvers;
const int solver_count = (int)(sizeof(all_solvers) / sizeof(all_solvers[0]));

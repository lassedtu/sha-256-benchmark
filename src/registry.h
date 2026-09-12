/**
 * @file registry.h
 *
 * List of all reverse-hashing solvers.
 *
 * The benchmark harness uses this list to run every solver.
 */

#ifndef REGISTRY_H
#define REGISTRY_H

#include "solver.h"

// solver_registry is the list of solvers.
// solver_count is the number of solvers in that list.
extern const Solver *const *const solver_registry;
extern const int solver_count;

#endif // REGISTRY_H
